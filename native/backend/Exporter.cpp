// Exporter.cpp —— 见 Exporter.h 的说明。
//
// 【和 ffmpeg 时代最大的不同】
//   以前合并分片是起 `ffmpeg.exe -f concat -c copy`：读清单、拼字节、
//   换封装全交给子进程。现在没有子进程了，改成本进程内两件 MF 对象对接：
//
//     IMFSourceReader（读分片里的**压缩**样本，不解码）
//          ↓  样本原样搬过去（等价于 -c copy）
//     IMFSinkWriter（写成一个正常 MP4）
//
//   SourceReader 被要求输出「原生类型」（GetNativeMediaType），也就是
//   H.264 裸流；SinkWriter 的输入类型也设成同一个，这叫 passthrough ——
//   一边不编码、一边不解码，纯搬样本，所以导出仍然是秒级的，画质零损失。
//
// 【时间戳为什么必须自己重建】
//   探针 native/probe_mf_sink.cpp 实测：直接把源样本的 PTS 搬过去，180 个
//   样本进、产物只剩 150 帧。原因是编码器默认带 B 帧，压缩样本的 PTS 与
//   解码顺序不一致，SinkWriter 的直拷通道要求时间戳严格递增，一旦回落就
//   **静默丢样本**（不报错，最坑）。
//   所以这里的时间轴完全不用源 PTS，而是按「这是第几帧、源帧率是多少」
//   重新算：第 i 帧的时间就是 i / fps。录制本来就是恒定帧率（喂帧线程按
//   帧数出图），时间轴本来就等于 帧号/帧率，重建反而更准。
//   顺带解决了「多轮次分片首尾相接」—— 每轮都从 0 开始，拼接时加偏移即可。
//
// 【为什么产物是 VFR（可变帧率）】
//   用户改帧率后，旧的 60fps 分片和新的 240fps 分片会同时进同一次导出。
//   直拷没法把 60fps 的帧"变成" 240fps（那要重新编码）。但也不需要：
//   每帧都带着自己的时长（1/源帧率），时间轴按真实秒数累加，播放器照样
//   按正确速度播。这比 ffmpeg 时代「重编码插帧」的做法更省、更快、无损。
//   只有当所有分片帧率一致时，产物自然就是 CFR。
#include "Exporter.h"

#include <windows.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "EnvPath.h"    // ExpandEnv：导出目录里的 %TEMP% 这类占位符
#include "JsonUtil.h"   // JsonEscape：诊断日志里要转义
#include "MfEncoder.h"  // MfAcquire / MfRelease：MF 的进程级生命周期
#include "Utf8Path.h"   // FromUtf8 / ToUtf8 / PathToUtf8 / PathFromUtf8

namespace shadow {
namespace fs = std::filesystem;

namespace {

/** 分片小于这个字节数就当成"还没写完"的尾巴，不参与导出。 */
constexpr uint64_t kMinSegmentBytes = 4096;

// ---------------------------------------------------------------------------
// 诊断日志出口
//
// Exporter 跑在自己的工作线程里，而 main.cpp 的 LogLine 是那个 translation
// unit 的内部函数（没导出）。这里不硬去改它，改成"谁需要诊断谁来挂钩子"：
// 服务启动时把 main.cpp 的 LogLine 注册进来，探针/单测不注册就是静默。
//
// 为什么要打这份计划：用户报"调整窗口大小后导出，出现 NO SIGNAL"，只看摘要
// （"9 个分片 / 20 秒 / 其中 2 秒补的"）根本分不清是
//   (a) 分片集合里真有空洞（录制端丢片），
//   (b) 计划器把片算漏了（lead / skip / gap 判错），
//   (c) 某片 probe 出来 ok=false 被 continue 掉。
// 三种的修法完全不同，猜一次就是半小时。
// ---------------------------------------------------------------------------
std::mutex g_planLogMu;
std::function<void(const std::string&)> g_planLog;

void LogExportPlan(const std::string& text) {
    std::function<void(const std::string&)> sink;
    {
        std::lock_guard<std::mutex> lk(g_planLogMu);
        sink = g_planLog;
    }
    if (!sink) return;
    sink("{\"type\":\"export_plan\",\"detail\":\"" + JsonEscape(text) + "\"}");
}

} // namespace

// 由 main.cpp 在服务启动时调用，把 NDJSON 日志出口接进来。
void SetExporterPlanLog(std::function<void(const std::string&)> sink) {
    std::lock_guard<std::mutex> lk(g_planLogMu);
    g_planLog = std::move(sink);
}

namespace {

// ---------------------------------------------------------------------------
// NO SIGNAL 遮罩用的 5x7 点阵字
//
// 【为什么自己画字不引入字体】这里只需要 8 个字母，为了它链 gdi32/gdiplus
// 去走一遍 GDI 渲染再读回位图，代码量比这张表大十倍，还得处理 DPI、字体
// 缺失、ClearType 彩色边缘（贴到 YUV 上会有彩边）。点阵表就是 8 行常量。
//
// 每个字符 7 行，每行用低 5 位表示 5 个像素（最高位是左边第一个像素）。
// ---------------------------------------------------------------------------
struct Glyph5x7 {
    char    ch;
    uint8_t rows[7];
};

const Glyph5x7 kGlyphs[] = {
    // N
    {'N', {0x11, 0x19, 0x15, 0x15, 0x13, 0x11, 0x11}},
    // O
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    // S
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    // I
    {'I', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F}},
    // G
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},
    // A
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    // L
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
};

const uint8_t* GlyphOf(char c) {
    for (const Glyph5x7& g : kGlyphs) {
        if (g.ch == c) return g.rows;
    }
    return nullptr; // 空格等没有字形的字符
}

/**
 * 把一张画布涂成黑底 + 居中的 "NO SIGNAL"。
 *
 * 用户要的补齐语义见 DESIGN.md 9.4：回放窗口内没有录到画面的那几段，用这张
 * 遮罩填满，保证导出的成品时长整好等于设置的回放秒数。
 *
 * 灰度按亮的一档给（0xE8）：太暗在深色视频里看不出来，纯白又刺眼。
 */
void FillNoSignal(uint8_t* canvas, uint32_t width, uint32_t height, size_t pitch) {
    if (!canvas || width == 0 || height == 0) return;

    // 整块刷成纯黑。这一步每段 NO SIGNAL 只需要做一次（同一张画布复用）。
    for (uint32_t y = 0; y < height; ++y) {
        std::memset(canvas + static_cast<size_t>(y) * pitch, 0, static_cast<size_t>(width) * 4u);
    }

    constexpr char kText[] = "NO SIGNAL";
    constexpr uint32_t kChars = sizeof(kText) - 1;
    constexpr uint32_t kAdvance = 6; // 5 像素字形 + 1 像素字间距

    // 字号随画布高度走：height/90 让 1080p 得到 12 倍（字高 84 像素），
    // 480p 得到 5 倍。下限 2，否则小画布上根本看不清。
    uint32_t scale = height / 90u;
    if (scale < 2) scale = 2;

    const uint32_t textW = (kChars * kAdvance - 1) * scale;
    const uint32_t textH = 7u * scale;
    if (textW >= width || textH >= height) return; // 画布小到放不下字，就只留黑底

    const uint32_t originX = (width - textW) / 2u;
    const uint32_t originY = (height - textH) / 2u;

    // 字用 BGRA 的浅灰。写 4 字节（低 3 字节是 BGR），alpha 给 0xFF 无所谓
    // —— 这张缓冲是送给编码器的原始像素，编码器不看 alpha。
    constexpr uint8_t kTone = 0xE8;

    for (uint32_t ci = 0; ci < kChars; ++ci) {
        const uint8_t* rows = GlyphOf(kText[ci]);
        if (!rows) continue; // 空格
        const uint32_t cellX = originX + ci * kAdvance * scale;
        for (uint32_t gy = 0; gy < 7; ++gy) {
            const uint8_t bits = rows[gy];
            for (uint32_t gx = 0; gx < 5; ++gx) {
                // 最高位是左边的像素：0x10 >> gx
                if ((bits & (0x10u >> gx)) == 0) continue;
                const uint32_t px0 = cellX + gx * scale;
                const uint32_t py0 = originY + gy * scale;
                for (uint32_t sy = 0; sy < scale; ++sy) {
                    uint8_t* row = canvas + static_cast<size_t>(py0 + sy) * pitch;
                    for (uint32_t sx = 0; sx < scale; ++sx) {
                        uint8_t* p = row + static_cast<size_t>(px0 + sx) * 4u;
                        p[0] = kTone;
                        p[1] = kTone;
                        p[2] = kTone;
                        p[3] = 0xFF;
                    }
                }
            }
        }
    }
}

struct SegmentFile {
    fs::path    path;
    std::string name;
    uint64_t    bytes = 0;
    int64_t     writeTime = 0; // 100ns 单位（fs::file_time_type 的原生刻度）
    // 这一片的墙钟起点（Unix **毫秒**，来自 index.csv 的第四列）。0 = 不知道。
    //
    // 【为什么必须要它】见 DESIGN.md 9.3。分片是按槽位覆盖写的，服务退出一阵
    // 再启动时，目录里还躺着昨天写的那几片；只按"最近 N 秒"截断的话，昨天的
    // 画面会被当成今天回放的一部分导出去。
    //
    // 【为什么是毫秒】秒级分辨率下，两片紧挨着的 2 秒分片墙钟差可能是 2 也可能
    // 是 3（截断误差），据此判"断档"会凭空补出 1 秒 NO SIGNAL —— 实测出现过。
    int64_t     startUnixMs = 0;
    // 这一片里**真实画面**的尺寸（来自 index.csv 的第五、六列）。0 = 不知道。
    //
    // 【为什么不能直接用文件帧尺寸】过渡期开片时画布可能是帧尺寸的 2 倍
    // （MfEncoder 的 kTransitionFactor）：画面只占左上角一块、右下是补的
    // 黑边。按文件帧尺寸定画布会把用户的画面撑成左上角小方块 —— 所以导出端
    // 要按这一列裁掉黑边再定画布。区分不了时退回文件帧尺寸（老数据）。
    uint32_t    contentW = 0;
    uint32_t    contentH = 0;
};

/** 当前的墙钟（Unix 秒）。 */
int64_t NowUnixSeconds() {
    return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count());
}

/**
 * 把文件系统的 file_clock 刻度换算成 Unix 秒。
 *
 * 老分片（ffmpeg 时代、或者 index.csv 里没有第四列）只能靠 mtime 推断它属于
 * 哪个时刻。两个 clock 的原点不同，用"现在"两个时钟各取一次做差，误差在
 * 毫秒级 —— 对于"这片是今天的还是昨天的"这种判断绰绰有余。
 */
int64_t FileClockToUnix(int64_t fileTicks) {
    using FileDur = fs::file_time_type::duration;
    using FileClock = fs::file_time_type::clock;

    static const int64_t offsetTicks = [] {
        const auto sysNow = std::chrono::system_clock::now().time_since_epoch();
        const auto fileNow = FileClock::now().time_since_epoch();
        return static_cast<int64_t>(
            std::chrono::duration_cast<FileDur>(sysNow).count() - fileNow.count());
    }();

    const int64_t sysTicks = fileTicks + offsetTicks;
    const auto sysDur = std::chrono::duration_cast<std::chrono::system_clock::duration>(
        FileDur(sysTicks));
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(sysDur).count());
}

/**
 * 这一片的墙钟起点，统一换成"浮点秒"给计划器用。
 *
 * index.csv 里记的是毫秒（够分辨"真断了"和"只是取整"），导出端按秒算时间轴，
 * 所以这里除以 1000 —— 别在调用处散着除，免得某一处忘了。老分片没有第四列时
 * 退回 mtime（只有秒，够判断"昨天还是今天"，不够判几十毫秒的接缝）。
 */
double StartWallSec(const SegmentFile& s) {
    if (s.startUnixMs > 0) return static_cast<double>(s.startUnixMs) / 1000.0;
    return static_cast<double>(FileClockToUnix(s.writeTime));
}

/**
 * 导出计划里的一项：要么是一段真实分片，要么是一段 NO SIGNAL 遮罩。
 *
 * 计划是按**墙钟**排的，所以真实分片两头都可能被回放窗口切开：窗口起点
 * 落在某一片中间时，那一片的前半截属于"更早的回放"，不能要（否则成品比
 * 用户设的时长更长）；同理窗口末尾也可能停在片中间。skipSec / seconds
 * 就是干这个的。
 */
struct PlanItem {
    bool   noSignal = false;
    size_t segIndex = 0;    // noSignal=false 时指向 segs/copies 的下标
    double seconds = 0.0;   // noSignal=true: 遮罩多长；否则: 只取这一片的前多少秒（0=整片）
    double skipSec = 0.0;   // 从这一片开头跳过多少秒（被窗口起点切开时用）
};

std::string HrText(HRESULT hr) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

/**
 * 从分片名里读录制时声明的帧率：`0002-fps240__000005.mp4` → 240。
 *
 * 新格式（`segments/<代次>/000000.mp4`）的目录名里没有 fps 了，这种返回 0，
 * 调用方改用 run.json 或 MF 自己问。保留这个函数是为了认老数据。
 */
uint32_t TagFpsOf(const std::string& name) {
    const size_t at = name.find("-fps");
    if (at == std::string::npos) return 0;
    size_t p = at + 4;
    uint32_t v = 0;
    bool got = false;
    while (p < name.size() && std::isdigit(static_cast<unsigned char>(name[p]))) {
        v = v * 10 + static_cast<uint32_t>(name[p] - '0');
        got = true;
        ++p;
    }
    return got ? v : 0;
}

/** 一个分片的元信息（打开一次 SourceReader 顺便问出来）。 */
struct SegmentProbe {
    bool     ok = false;
    double   durationSec = 0.0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps = 0; // 源帧率；读不出来是 0，调用方自己兜底
};

/**
 * 打开一个分片读它的时长和画面尺寸。
 *
 * 为什么时长要问 MF 而不是靠 mtime 猜：这一段历史在旧的 ffmpeg 实现里踩过
 * 三次（4KB 阈值 / 体积中位数 / mtime 4 秒），最后一次直接把刚切完的完整
 * 2 秒片误判成不完整，于是 20 秒缓冲导出来只有 18 秒 —— 用户报的"少两秒"
 * 就是它。分片自己知道自己有多少帧，问它才是唯一可靠的判据。
 */
SegmentProbe ProbeSegment(const fs::path& file) {
    SegmentProbe out;

    IMFSourceReader* reader = nullptr;
    HRESULT hr = MFCreateSourceReaderFromURL(file.wstring().c_str(), nullptr, &reader);
    if (FAILED(hr) || !reader) return out;

    // 时长：presentation descriptor 上的 MF_PD_DURATION，单位 100ns。
    PROPVARIANT var;
    PropVariantInit(&var);
    hr = reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &var);
    if (SUCCEEDED(hr) && var.vt == VT_UI8) {
        out.durationSec = static_cast<double>(var.uhVal.QuadPart) / 10000000.0;
        out.ok = true;
    }
    PropVariantClear(&var);

    // 画面尺寸和源帧率：从原生（压缩）类型的属性里读。
    IMFMediaType* nativeType = nullptr;
    hr = reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0,
                                    &nativeType);
    if (SUCCEEDED(hr) && nativeType) {
        UINT32 w = 0, h = 0;
        if (SUCCEEDED(MFGetAttributeSize(nativeType, MF_MT_FRAME_SIZE, &w, &h))) {
            out.width = w;
            out.height = h;
        }
        UINT32 num = 0, den = 1;
        if (SUCCEEDED(MFGetAttributeRatio(nativeType, MF_MT_FRAME_RATE, &num, &den)) && num && den) {
            out.fps = (num + den / 2) / den;
        }
        nativeType->Release();
    }

    reader->Release();
    return out;
}

/** index.csv 里一行的信息。 */
struct IndexRow {
    std::string name;
    int64_t     startUnixMs = 0;  // 该片开始写时的墙钟（Unix 毫秒），0 = 老数据没有
    uint32_t    contentW = 0;     // 该片里真实画面的宽（不含过渡期补的黑边），0 = 不知道
    uint32_t    contentH = 0;
};

/** 一个目录的 index.csv 解析结果。 */
using WallClockIndex = std::vector<IndexRow>;

/**
 * 读 `<dir>/index.csv`，取出每个分片的墙钟起点和真实画面尺寸。
 *
 * 格式是六列：`文件名,起始秒,结束秒,起始墙钟Unix毫秒,真实宽,真实高`。
 * 前三列沿用 ffmpeg `-segment_list` 的老格式（老录像没有后三列，缺列就当
 * "不知道"，分别用 mtime 和文件帧尺寸兜底）。
 *
 * 为什么不直接用文件 mtime：mtime 是"最后一次写入的时刻"，而分片是按槽位
 * 覆盖写的 —— 昨天的片今天被覆盖之后 mtime 是今天的，但它里面的画面是
 * 中断之前那一刻的。真正要的是"这一片第一帧是什么时候录的"，只有录制端
 * 自己知道，所以它写进 index.csv 里。
 *
 * 为什么还要记真实画面尺寸：过渡期开片时画布可能是帧尺寸的 2 倍
 * （MfEncoder 的 kTransitionFactor），画面只占左上角一块、右下是补的黑边。
 * 导出端若按文件里的帧尺寸定画布，用户的画面就被这块黑边撑成小方块 ——
 * 所以按这一列裁掉黑边再定画布。
 */
WallClockIndex ReadIndexCsv(const fs::path& dir) {
    WallClockIndex out;
    const fs::path file = dir / fs::u8path("index.csv");
    std::error_code ec;
    if (!fs::exists(file, ec) || ec) return out;

    std::ifstream f(PathToUtf8(file), std::ios::binary);
    if (!f) return out;

    // 按逗号切列（文件名不含逗号 —— 都是 000000.mp4 这种）。
    auto splitCols = [](const std::string& line, std::vector<std::string>& cols) {
        cols.clear();
        size_t pos = 0;
        for (;;) {
            const size_t c = line.find(',', pos);
            if (c == std::string::npos) {
                cols.push_back(line.substr(pos));
                break;
            }
            cols.push_back(line.substr(pos, c - pos));
            pos = c + 1;
        }
    };

    std::string line;
    std::vector<std::string> cols;
    while (std::getline(f, line)) {
        // 行尾可能有 \r（录制端写的是 \n，但老数据/手工改过的不一定）。
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) continue;

        splitCols(line, cols);
        if (cols.size() < 4) continue; // 老格式只有三列，没有墙钟
        if (cols[0].empty()) continue;
        int64_t startUnixMs = 0;
        try {
            startUnixMs = static_cast<int64_t>(std::stoll(cols[3]));
        } catch (...) {
            continue;
        }
        if (startUnixMs <= 0) continue;

        IndexRow row;
        row.name = cols[0];
        row.startUnixMs = startUnixMs;
        if (cols.size() >= 6) {
            try {
                const long long w = std::stoll(cols[4]);
                const long long h = std::stoll(cols[5]);
                if (w > 0 && h > 0 && w < 100000 && h < 100000) {
                    row.contentW = static_cast<uint32_t>(w);
                    row.contentH = static_cast<uint32_t>(h);
                }
            } catch (...) {
                // 缺列/坏值一律当"不知道"，退回文件帧尺寸
            }
        }
        out.push_back(std::move(row));
    }
    return out;
}

/** 在索引里找这个文件的墙钟起点；找不到返回 0。 */
int64_t LookupWallClock(const WallClockIndex& idx, const std::string& leafName) {
    for (const IndexRow& kv : idx) {
        if (kv.name == leafName) return kv.startUnixMs;
    }
    return 0;
}

/** 在索引里找这个文件的真实画面尺寸；找不到返回 false。 */
bool LookupContentSize(const WallClockIndex& idx, const std::string& leafName, uint32_t& w,
                       uint32_t& h) {
    for (const IndexRow& kv : idx) {
        if (kv.name != leafName) continue;
        if (kv.contentW == 0 || kv.contentH == 0) return false;
        w = kv.contentW;
        h = kv.contentH;
        return true;
    }
    return false;
}

/** 按修改时间排好序的分片清单（只含真正写完了的）。
 *
 *  nominalSegmentSeconds：单片标称时长（秒），用来判"最后一片够不够长"。
 *  传 0 或负数时按 2 秒处理。 */
bool ListSegments(const fs::path& dir, std::vector<SegmentFile>& out, std::string& err,
                  double nominalSegmentSeconds = 2.0) {
    (void)nominalSegmentSeconds;
    out.clear();
    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    if (ec) {
        err = "打不开分片目录：" + ec.message();
        return false;
    }
    // 收一片（平铺在 dir 下的老数据，和 dir/轮次子目录/ 下的新数据都走这里）。
    //
    // 平铺时 name 就是文件名；在轮次子目录里时要拼成 `<轮次名>__<文件名>`
    // （比如 `0002-fps240__000005.mp4`）—— 各轮次的分片都叫 000000.mp4，不拼成
    // 唯一名字的话，后面复制到临时目录时会互相覆盖。
    //
    // idx 是**这一层目录**的 index.csv（每个轮次子目录各有一份）。
    auto take = [&](const fs::directory_entry& e, const std::string& namePrefix,
                    const WallClockIndex& idx) {
        std::error_code e2;
        if (!e.is_regular_file(e2) || e2) return;
        const std::string ext = e.path().extension().u8string();
        // .mp4 = 现在的 MF fMP4 分片；.ts = ffmpeg 时代的老录像。
        // 两种都收：升级之后主人以前录的东西还得能导出来。
        if (ext != u8".mp4" && ext != u8".ts") return;
        SegmentFile f;
        f.path = e.path();
        const std::string base = PathToUtf8(e.path().filename());
        f.name = namePrefix.empty() ? base : (namePrefix + "__" + base);
        f.bytes = static_cast<uint64_t>(e.file_size(e2));
        if (e2 || f.bytes < kMinSegmentBytes) return; // 半截尾巴
        const auto t = e.last_write_time(e2);
        f.writeTime = e2 ? 0 : static_cast<int64_t>(t.time_since_epoch().count());
        f.startUnixMs = LookupWallClock(idx, base);
        uint32_t cw = 0, ch = 0;
        const bool haveContent = LookupContentSize(idx, base, cw, ch);
        if (haveContent) {
            f.contentW = cw;
            f.contentH = ch;
        }
        // 【孤儿分片】文件还在盘上、但 index.csv 里已经没有它的行。
        //
        // 怎么产生的：槽位是复用的，`RewriteIndex()` 只写 done_ 里的**当前**
        // 窗口清单。第 61 片覆盖第 1 片的文件时，清单里第 1 片那行就没了，可
        // 文件名被新内容占着 —— 于是盘上出现一个"清单里查不到"的文件。它的
        // 墙钟和真实画面尺寸都无从得知。
        //
        // 这种片子必须**整片跳过**，不能收进导出：
        //   · 没有墙钟 → 计划器把它当 startUnixMs=0，时间轴无从摆放；
        //   · 没有真实尺寸 → 只能退回文件帧尺寸，而过渡期的画布可能是真实
        //     画面的 2 倍（MfEncoder 的 kTransitionFactor），这个虚高值会把
        //     「20 秒内最大尺寸」顶到 4096x2912，用户导出一个比显示器还大的
        //     视频、画面缩在左上角一小块（m12477 报的就是这个）。
        // 跳过它不影响回放完整性：它本来就不属于当前窗口，缺的时间由 NO SIGNAL
        // 或相邻分片顶上。
        if (f.startUnixMs == 0 && !haveContent) return;
        out.push_back(std::move(f));
    };

    // 平铺在 dir 下的老数据用 dir 自己的 index.csv。
    const WallClockIndex rootIdx = ReadIndexCsv(dir);

    for (const auto& e : it) {
        std::error_code e2;
        if (e.is_directory(e2) && !e2) {
            // ---- 轮次子目录 --------------------------------------------------
            //
            // 录制端每起一次编码器就新建 `segments/<代次数字>/`（见
            // SegmentRecorder.cpp 的 Start）。改帧率后旧轮次不删，所以这里要
            // 往里扫 —— 用户的诉求是"前面录的也要一起导出"。
            // mtime 排序天然把各轮按时间串起来（旧轮的片子年代更早）。
            const std::string prefix = PathToUtf8(e.path().filename());
            const WallClockIndex subIdx = ReadIndexCsv(e.path());
            std::error_code e3;
            fs::directory_iterator sub(e.path(), e3);
            if (e3) continue;
            for (const auto& se : sub) take(se, prefix, subIdx);
            continue;
        }
        take(e, std::string(), rootIdx);
    }
    std::sort(out.begin(), out.end(), [](const SegmentFile& a, const SegmentFile& b) {
        if (a.writeTime != b.writeTime) return a.writeTime < b.writeTime;
        return a.name < b.name; // 同一刻写入时按名字兜底，保证顺序稳定
    });
    return true;
}

/** 复制一个文件并校验字节数。 */
bool CopyChecked(const fs::path& from, const fs::path& to, uint64_t expectBytes,
                 std::string& err) {
    std::error_code ec;
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        err = "复制分片失败：" + ec.message();
        return false;
    }
    const uint64_t got = static_cast<uint64_t>(fs::file_size(to, ec));
    if (ec || got != expectBytes) {
        err = "复制分片后字节数不对（期望 " + std::to_string(expectBytes) + "，实际 " +
              std::to_string(got) + "）";
        return false;
    }
    return true;
}

/** 目录里挑一个不重名的文件名。 */
fs::path UniqueTarget(const fs::path& dir, const std::string& base, const std::string& ext) {
    std::error_code ec;
    fs::path p = dir / fs::u8path(base + ext);
    if (!fs::exists(p, ec)) return p;
    for (int i = 2; i < 1000; ++i) {
        p = dir / fs::u8path(base + "-" + std::to_string(i) + ext);
        if (!fs::exists(p, ec)) return p;
    }
    return dir / fs::u8path(base + "-" + std::to_string(GetTickCount()) + ext);
}

} // namespace

// ---------------------------------------------------------------------------
// 命名
// ---------------------------------------------------------------------------

std::string ReplayTimeStamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char buf[64];
    // 跟 NVIDIA 即时重放一致：日期用点，时间用短横，名字里不会出现冒号
    // （Windows 文件名不允许冒号）。
    std::snprintf(buf, sizeof(buf), "%04u.%02u.%02u_%02u-%02u-%02u", st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::string ReplayFileName(const std::string& prefix, const std::string& stamp) {
    std::string base;
    if (!prefix.empty()) base = prefix + "_";
    base += "Replay_";
    base += stamp.empty() ? ReplayTimeStamp() : stamp;
    return base + ".mp4";
}

// ---------------------------------------------------------------------------
// Exporter
// ---------------------------------------------------------------------------

Exporter::Exporter(std::string segmentsDir, std::string exportDir, std::string prefix,
                   uint32_t maxSeconds, uint32_t targetFps, uint32_t segmentSeconds)
    : segmentsDir_(std::move(segmentsDir)),
      // 展开 `%TEMP%` 这类占位符。
      //
      // 服务自己读配置时会展开（Profiles.cpp 的 ExpandEnv），但**界面进程
      // 也有一条直达这里的路**：addon.exportStart 直接把配置里的原始字符串
      // 递过来。以前没在这层展开，`%TEMP%\shadow-play\app\exports` 就变成了
      // 相对路径，`create_directories` 在当前目录建出一个名叫 `%TEMP%` 的
      // 文件夹（实际踩过，仓库根多了个 `%TEMP%`）。导出目录是用户要看的
      // 东西，绝不能悄悄落到莫名其妙的地方，所以这层兜底必须有。
      exportDir_(ExpandEnv(exportDir)),
      prefix_(std::move(prefix)),
      maxSeconds_(maxSeconds),
      targetFps_(targetFps > 240u ? 240u : targetFps),
      // 分片时长夹到合理区间：0 或者离谱的值都按 2 秒处理，否则"最后一片够
      // 不够长"那个判据就失去意义了。
      segmentSeconds_(segmentSeconds >= 1u && segmentSeconds <= 60u ? segmentSeconds : 2u) {}

Exporter::~Exporter() {
    // 析构时如果还在导，等它自己收尾（强杀线程会让临时目录留在磁盘上）。
    // 最多等 10 秒 —— 导出正常是一两秒的事。
    if (worker_.joinable()) {
        if (!WaitIdle(10000)) {
            // 实在等不到就 detach：至少不会阻塞服务退出。
            // 残留的临时目录由下次导出启动时清理，不影响正确性。
            worker_.detach();
            return;
        }
        worker_.join();
    }
}

ExportStatus Exporter::Status() const {
    std::lock_guard<std::mutex> lk(mu_);
    return status_;
}

void Exporter::SetPhase(const std::string& phase) {
    std::lock_guard<std::mutex> lk(mu_);
    status_.phase = phase;
}

bool Exporter::WaitIdle(uint32_t timeoutMs) const {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!status_.running) return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) return false;
        Sleep(20);
    }
}

bool Exporter::Start() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (status_.running) return false; // 上一轮还在导
        status_ = ExportStatus{};
        status_.running = true;
        status_.jobId = ++lastJobId_;
        status_.phase = "准备";
    }
    if (worker_.joinable()) worker_.join(); // 收上一轮的僵尸线程
    worker_ = std::thread(&Exporter::Run, this);
    return true;
}

void Exporter::Finish(bool ok, const std::string& detail, const std::string& outPath,
                      const std::string& segmentsDirUsed, const std::string& listFile) {
    std::error_code ec;
    // 临时目录整个删掉（里面有分片副本）
    if (!segmentsDirUsed.empty()) fs::remove_all(fs::u8path(segmentsDirUsed), ec);
    if (!listFile.empty()) fs::remove(fs::u8path(listFile), ec);

    std::lock_guard<std::mutex> lk(mu_);
    status_.running = false;
    status_.ok = ok;
    status_.failed = !ok;
    status_.detail = detail;
    if (ok) status_.outPath = outPath;
    status_.phase = ok ? "完成" : "失败";
}

void Exporter::Run() {
    const auto t0 = std::chrono::steady_clock::now();
    auto elapsedMs = [&t0]() {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
            .count();
    };

    std::string tmpDir;

    auto fail = [&](const std::string& why) {
        // 注意：Finish() 自己会加锁，所以这里**不能**先拿着 mu_ 再调它，
        // 否则 std::mutex 不可重入 —— 会直接死锁在导出线程里。
        {
            std::lock_guard<std::mutex> lk(mu_);
            status_.elapsedMs = elapsedMs();
        }
        Finish(false, why, {}, tmpDir, {});
    };

    // ---- 0. 前置检查 ------------------------------------------------------
    if (segmentsDir_.empty()) {
        fail("这一路还没有录像目录（窗口没匹配到过，或者还没开始录）。");
        return;
    }
    if (exportDir_.empty()) {
        fail("没有配置导出目录。");
        return;
    }

    // Media Foundation 是线程感知的：这个导出线程要自己 Acquire 一次。
    // 成功与否都记下来，收尾时成对 Release。
    const bool mfOk = MfAcquire();
    if (!mfOk) {
        fail("这台机器起不来 Media Foundation（可能缺 Media Feature Pack）。");
        return;
    }

    std::error_code ec;
    fs::create_directories(fs::u8path(exportDir_), ec);
    if (ec) {
        MfRelease();
        fail("建不了导出目录：" + ec.message());
        return;
    }

    // ---- 1. 快照分片 ------------------------------------------------------
    SetPhase("快照分片");
    std::vector<SegmentFile> segs;
    std::string err;
    if (!ListSegments(fs::u8path(segmentsDir_), segs, err,
                      static_cast<double>(segmentSeconds_))) {
        MfRelease();
        fail(err);
        return;
    }
    if (segs.empty()) {
        MfRelease();
        fail("分片目录里还没有可导出的完整分片。");
        return;
    }

    // ---- 1b. 按墙钟剔掉"上一辈子的分片" -----------------------------------
    //
    // 见 DESIGN.md 9.3。服务退出之后再启动，分片目录里还躺着退出之前写的片子
    // —— 有可能就是昨天的。它们按"最近 N 秒"的算法会排在最前面被一起导出来，
    // 于是回放里出现"很久以前"的画面。判据是每一片的墙钟起点（index.csv 第四
    // 列；老数据退回 mtime）。
    //
    // 收尾时刻取"现在"与"最新一片的结尾"里较早的那个：录制刚停下来的那一下，
    // 用现在算窗口会把最后几片挤出去。
    const double nowUnix = static_cast<double>(NowUnixSeconds());
    double newestEnd = 0.0;
    {
        // 最新一片的结尾 = 它的墙钟起点 + **它自己的真实时长**。
        //
        // 【为什么不能加标称时长】见 DESIGN.md 9.4.1。改窗口尺寸、改帧率、
        // 服务重启都会把一片切短（实测有只剩 4 帧 = 0.13 秒的），而标称永远
        // 按 segmentSeconds_（2 秒）算 —— 于是"最新一片的结尾"被判到 1.9 秒
        // 之后，回放窗口的收尾时刻也跟着往后推了 1.9 秒，那 1.9 秒里根本
        // 没有画面，全被补成 NO SIGNAL。用户看到的就是"调完窗口大小后导出，
        // 最后两秒是 NO SIGNAL"。
        //
        // 只探最新那一片（不是每片都探）：这一步只需要一个"最近的时刻"，
        // 而最新的一片必然是最靠后的那一片，它的真实长度就够用。
        const SegmentFile* latest = nullptr;
        double latestStart = 0.0;
        for (const SegmentFile& s : segs) {
            const double start = StartWallSec(s);
            if (start <= 0.0) continue;
            if (start > latestStart) {
                latestStart = start;
                latest = &s;
            }
        }
        if (latest) {
            double durSec = 0.0;
            if (latest->bytes > 0) {
                const SegmentProbe pr = ProbeSegment(latest->path);
                if (pr.ok && pr.durationSec > 0.0) durSec = pr.durationSec;
            }
            // 探不出来（正在写 / 索引还没落盘）才退回标称值。
            if (durSec <= 0.0) durSec = static_cast<double>(segmentSeconds_);
            newestEnd = latestStart + durSec;
        }
    }
    const double cutoffEnd = (newestEnd > 0.0 && newestEnd < nowUnix) ? newestEnd : nowUnix;
    const double windowSec = maxSeconds_ > 0 ? static_cast<double>(maxSeconds_) : 0.0;
    if (windowSec > 0) {
        // 留两个分片的余量：最后一片的结尾是"起点 + 标称时长"的粗估，边界上
        // 多留一点，比少留导致回放短一截好。
        const double budget = windowSec + static_cast<double>(segmentSeconds_) * 2.0 + 1.0;
        const double earliest = cutoffEnd - budget;
        segs.erase(std::remove_if(segs.begin(), segs.end(),
                                  [&](const SegmentFile& s) {
                                      const double start = StartWallSec(s);
                                      return start > 0.0 && start < earliest;
                                  }),
                   segs.end());
        if (segs.empty()) {
            MfRelease();
            fail("回放窗口内的分片都已经过期了（目录里只有更早录制的内容）。");
            return;
        }
    }

    // ---- 1c. 按墙钟排一遍序 ---------------------------------------------
    //
    // 【为什么不能只靠 mtime】见 DESIGN.md 9.3。mtime 是"最后一次写入"的时刻，
    // 分片是**按槽位覆盖**写的：一个只写了 1 秒就切走的片（改尺寸、改帧率、
    // 服务重启都会）会在很久之后才被别的片覆盖成完整长度，于是它的 mtime 比
    // 时间上更晚的片还新。实测就出现过这种乱序：导出时相邻两片按 mtime 排是
    // 倒着接的，逐片累加的时间轴一路"往回跳"，补齐计划于是凭空算出几秒缺口
    // （成片里出现几段莫名的 NO SIGNAL），末尾还被截短。
    //
    // 有墙钟就按墙钟排（那是录制时的真实先后）；一片墙钟都没有（全是老数据）
    // 才退回 mtime —— 那时候没有更好的信息。
    {
        bool allWall = !segs.empty();
        for (const SegmentFile& s : segs) {
            if (s.startUnixMs <= 0) {
                allWall = false;
                break;
            }
        }
        if (allWall) {
            std::stable_sort(segs.begin(), segs.end(), [](const SegmentFile& a, const SegmentFile& b) {
                return a.startUnixMs < b.startUnixMs;
            });
        }
    }

    // 按回放时长截断（**必须按墙钟，不能按片数**）。
    //
    // 【原来的写法为什么是灾难】这里原来是
    //   keep = ceil(maxSeconds / segmentSeconds) + 1;
    //   if (segs.size() > keep) segs.erase(begin, end - keep);
    // 它假定每片都是标称长度。而现在尺寸一变就立刻切一片（用户明确要求的），
    // 拖几次窗口就能攒出几十片零点几秒的过渡片 —— 20 秒窗口的实际内容有
    // 21 秒，但片数是 40，`keep` 算出 11，只留最后 11 片 ≈ 3 秒。剩下 17 秒
    // 全被补成 NO SIGNAL。主人报的「几乎全程 NO SIGNAL，只有后面几秒是画面」
    // 就是这条 + 编码端槽位被吃满，两个原因叠加。
    //
    // 现在：按每片**真实时长**从最后一片往前累加，累到 maxSeconds 就停。
    // 片长用 ProbeSegment 问（这里已经要逐片探一次，成本可接受），拿不到
    // 时长时退回标称值 —— 只影响"多留半片"，不会误删。
    if (maxSeconds_ > 0) {
        const double want = static_cast<double>(maxSeconds_);
        const double nominal = static_cast<double>(segmentSeconds_);
        // 多留一片的余量：窗口起点通常落在某片中间，那一片得整片留着让
        // 后面的计划器去裁（它按 wall clock 算 skip）。
        double acc = 0.0;
        size_t firstKeep = segs.size();
        for (size_t i = segs.size(); i > 0; --i) {
            const size_t idx = i - 1;
            const SegmentProbe pr = ProbeSegment(segs[idx].path);
            const double d = (pr.ok && pr.durationSec > 0.0) ? pr.durationSec : nominal;
            acc += d;
            firstKeep = idx;
            if (acc > want + nominal) break; // 已经足够覆盖回放窗口，再往前就是多余的
        }
        if (firstKeep > 0 && firstKeep < segs.size()) {
            segs.erase(segs.begin(), segs.begin() + static_cast<ptrdiff_t>(firstKeep));
        }
    }

    // ---- 2. 剔掉"正在写"的最后一片 ---------------------------------------
    //
    // 录制是活着的：硬盘上最后一片随时可能只有半截。判据是**问这一片自己
    // 有多长**（ProbeSegment），短于标称时长的 90% 就当没写完。
    //
    // 【为什么只剔"读不出"的，不剔"半片"】见 DESIGN.md 9.4 / 9.7。
    //
    // 这里原来写的是 `tail.durationSec < nominal * 0.9` 就 pop_back，即把
    // 正在写的那半片整个丢掉。那是错的：**正在写的那一片里已经有画面了**，
    // 丢掉它就是丢掉回放窗口最尾部最长一整片（2 秒）的实录内容，随后计划器
    // 只好把那 2 秒补成 NO SIGNAL —— 主人报的「点导出，最后两秒出现 no
    // signal」就是这个。片长是随机的（点导出的时刻落在片内哪个位置全看运气），
    // 所以它时有时无、很难复现。
    //
    // 现在只为"读不出"而剔：ProbeSegment 失败（还没写出 moov / 文件是 0 字节）
    // 或时长不是正数。半片一律保留 —— 分片在导出开始时就整份复制到临时目录，
    // 复制过来的那一份是定格的快照，fMP4 每个 moof 自带索引，所以半片也是
    // 完整可读的，它真实有多长就铺多长（segSecs 用的是真实时长，不是标称）。
    {
        bool tailBad = true;
        if (!segs.empty()) {
            const SegmentProbe tail = ProbeSegment(segs.back().path);
            tailBad = !(tail.ok && tail.durationSec > 0.0);
        }
        if (tailBad && !segs.empty()) {
            segs.pop_back();
        }
        if (segs.empty()) {
            MfRelease();
            fail("分片目录里还没有可导出的完整分片。");
            return;
        }
    }

    tmpDir = PathToUtf8(fs::u8path(exportDir_) / fs::u8path(".shadow-export-tmp"));
    fs::remove_all(fs::u8path(tmpDir), ec);
    fs::create_directories(fs::u8path(tmpDir), ec);
    if (ec) {
        MfRelease();
        fail("建不了导出临时目录：" + ec.message());
        return;
    }
    // tmpDir 先赋给成员之外的地方，fail() 才能清掉它 —— 这里直接返回前
    // 手动清一次，避免临时目录残留。
    auto giveUp = [&](const std::string& why) {
        std::error_code ec9;
        fs::remove_all(fs::u8path(tmpDir), ec9);
        MfRelease();
        fail(why);
    };

    // 复制到临时目录。
    //
    // 为什么必须复制：录制还在跑，分片的槽位是**覆盖写**的（MfEncoder 会
    // 删掉旧文件重新建）。我们正读着的那片可能下一秒就被换掉，导出的视频
    // 就会出现花屏或者时长变短。复制一份虽然多花一次磁盘 IO，但换来
    // "导出内容前后自洽"，而且复制时校验字节数，能把正被写坏的片挑出来重试。
    //
    // 【为什么复制失败要"跳过"而不是"整次放弃"】见 DESIGN.md 9.7。
    // 槽位轮转的一瞬间，文件被 remove 之后、SinkWriter 写出第一块之前，
    // 它在磁盘上就是 0 字节。用户报的
    // 「复制分片后字节数不对（期望 479005，实际 0）」正是撞上这个窗口。
    // 老实现直接 giveUp，于是"清理过 temp 目录之后导出必失败"——因为清理
    // 之后槽位全是新文件，轮转撞车概率最高。现在重试三次，还不行就把这一
    // 片排除掉、继续导出其余部分，并在摘要里说清少了几片。
    std::vector<fs::path> copies;
    std::vector<SegmentFile> kept;
    copies.reserve(segs.size());
    kept.reserve(segs.size());
    size_t skippedCopy = 0;
    for (size_t i = 0; i < segs.size(); ++i) {
        // 临时文件名必须唯一：各轮次的分片都叫 000000.mp4。
        char leaf[64];
        std::snprintf(leaf, sizeof(leaf), "%05u_", static_cast<unsigned>(kept.size()));
        const fs::path dst = fs::u8path(tmpDir) / fs::u8path(std::string(leaf) + segs[i].name);
        std::string copyErr;
        bool copied = false;
        for (int attempt = 0; attempt < 3 && !copied; ++attempt) {
            if (attempt > 0) Sleep(static_cast<DWORD>(80 * attempt));
            copyErr.clear();
            copied = CopyChecked(segs[i].path, dst, segs[i].bytes, copyErr);
        }
        if (!copied) {
            // 这一片正在被重写（或已经被删）。排除它，别让整次导出失败。
            ++skippedCopy;
            std::error_code ecd;
            fs::remove(dst, ecd);
            continue;
        }
        copies.push_back(dst);
        kept.push_back(segs[i]);
    }
    segs = kept;
    if (segs.empty()) {
        giveUp("分片正在被录制覆盖，这一瞬间没有可读取的完整分片，请稍后重试。");
        return;
    }

    // ---- 3. 合并 ----------------------------------------------------------
    //
    // 两条路，按「窗口内各分片的画面尺寸是否一致」自动选：
    //
    //   * 一致   → **直拷**：IMFSourceReader 读压缩样本，原样喂给 IMFSinkWriter
    //              （等价 ffmpeg 的 -c copy）。不解码不重编码，20 秒的片子
    //              一两百毫秒就出来，画质零损失。
    //   * 不一致 → **重编码**：画布取窗口内**最大那一帧**的尺寸，小画面贴左上角，
    //              右边和下面补黑边（不拉伸）。用户把窗口拖大又拖小的整段
    //              录像都能保留，不会像以前那样整片丢掉。
    //
    // 但**只要需要补 NO SIGNAL**（见 DESIGN.md 9.4），就必须走重编码：直拷
    // 只能搬现有样本，造不出"本来没录到"的那几秒画面。
    //
    // 为什么不能"直接拼"不同尺寸的片：H.264 码流在流中间换分辨率，播放器
    // 要么花屏要么只解出前一段。这是编码格式的硬约束，不是实现偷懒。
    SetPhase("合并");

    {
        // 每个分片的实际时长（秒）。后面排补齐计划、算"总共差多少"都要用。
        //
        // 为什么不再"按标称时长估"：拖动窗口改尺寸会在录制中途切出**短分片**
        // （不足一个 segmentSeconds），按标称累加会让时间轴凭空多出好几秒，
        // 成片里就出现莫名的黑场。逐片问一句代价很小（一次 reader 打开）。
        std::vector<double> segSecs(copies.size(), 0.0);
        for (size_t si = 0; si < copies.size(); ++si) {
            const SegmentProbe p = ProbeSegment(copies[si]);
            if (p.ok && p.durationSec > 0) segSecs[si] = p.durationSec;
        }
        double recordedSec = 0.0;
        for (double s : segSecs) recordedSec += s;


        // ---- 3a. 先按墙钟算出"现在"的时间轴位置 --------------------------
        //
        // 【为什么要在这里算一次】见 DESIGN.md 9.4.2。分片的 startUnixMs 是
        // **内容时间轴**：第一片开片时取一次真实墙钟，之后每片按"帧数 × 单片
        // 秒数"顺延（见 MfEncoder 的 nextSegStartUnix_）。编码器跟不上时
        // （1440p@144 实测常常只跑到 106~135fps），时间轴会越走越落后于墙钟 ——
        // 实测 40 秒后落后 16 秒。
        //
        // 如果拿**墙钟**当"现在"（`cutoffEnd = now`），窗口右端就落在时间轴
        // 根本还没走到的地方，导出端只好拿最后一片的结尾当 cutoffEnd。于是
        // "窗口起点 → 第一片起点"之间凭空出现一段空洞（实测 -3.99 秒的负 lead
        // 加上尾部 2 秒补齐），被补成 NO SIGNAL —— 用户看到的就是"调整窗口
        // 大小后导出，末尾两秒是 NO SIGNAL"。
        //
        // 用**时间轴**当"现在"（最新一片的起点 + 真实时长）就一致了：窗口右端
        // 正好是录到的最后一刻，左端自然是"再往前 20 秒的内容"。
        double axisNow = nowUnix;
        {
            double newestAxisEnd = 0.0;
            // segs 与 segSecs 下标一一对应（复制循环按原顺序 push）。
            for (size_t si = 0; si < segs.size(); ++si) {
                const double start = StartWallSec(segs[si]);
                if (start <= 0.0) continue;
                // 用这一片自己的真实时长（上面刚探过），别用标称值。
                const double dur = segSecs[si] > 0.0 ? segSecs[si] : static_cast<double>(segmentSeconds_);
                const double end = start + dur;
                if (end > newestAxisEnd) newestAxisEnd = end;
            }
            // 时间轴只会落后墙钟（编码慢）或持平；万一超前也夹回墙钟。
            if (newestAxisEnd > 0.0 && newestAxisEnd < axisNow) axisNow = newestAxisEnd;
        }


        // ---- 3b. 先探清每一片的画面尺寸和帧率 -----------------------------
        //
        // 为什么要整轮探一遍：画布尺寸取决于**窗口内最大那一帧**，而"最大"
        // 只有全看完才知道。先扫一遍是有意的——把它和下面的读写循环分开，
        // 判定逻辑才清晰。
        //
        // 【为什么探测要排在排计划之前】见 DESIGN.md 9.4。排计划时要按
        // "这一片能不能读、有多少秒"决定铺多少，读不了的片不能占时间轴。
        struct SegShape {
            uint32_t width = 0;
            uint32_t height = 0;
            uint32_t fps = 0;
            bool     ok = false;
            // 真实画面尺寸（不含过渡期补的黑边）。0 = 不知道，退回文件帧尺寸。
            uint32_t contentW = 0;
            uint32_t contentH = 0;
        };
        std::vector<SegShape> shapes(copies.size());
        uint32_t maxW = 0;
        uint32_t maxH = 0;

        for (size_t si = 0; si < copies.size(); ++si) {
            IMFSourceReader* probe = nullptr;
            if (FAILED(MFCreateSourceReaderFromURL(copies[si].wstring().c_str(), nullptr,
                                                   &probe)) ||
                !probe) {
                continue; // 这一片读不了，后面按跳过处理
            }
            IMFMediaType* nt = nullptr;
            if (SUCCEEDED(probe->GetNativeMediaType(
                    static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &nt)) &&
                nt) {
                UINT32 w = 0, h = 0;
                MFGetAttributeSize(nt, MF_MT_FRAME_SIZE, &w, &h);

                // 老数据名里有 `-fps240`；新数据没有，就问 MF 自己的平均帧率。
                uint32_t fps = TagFpsOf(copies[si].filename().u8string());
                if (fps == 0) {
                    UINT32 num = 0, den = 1;
                    if (SUCCEEDED(MFGetAttributeRatio(nt, MF_MT_FRAME_RATE, &num, &den)) && num &&
                        den) {
                        fps = (num + den / 2) / den;
                    }
                }
                if (fps == 0) fps = 60;

                if (w > 0 && h > 0) {
                    // 【真实画面尺寸】过渡期开片时画布可能是帧尺寸的 2 倍，
                    // 画面只占左上角一块、右下是补的黑边（见 MfEncoder 的
                    // kTransitionFactor）。index.csv 里记了真实尺寸，按它裁剪
                    // 并定画布 —— 否则用户的画面被黑边撑成左上角小方块。
                    // 真实画面尺寸来自 index.csv（`segs` 与 `copies` 下标一一对应）。
                    uint32_t cw = segs[si].contentW;
                    uint32_t ch = segs[si].contentH;
                    if (cw == 0 || ch == 0 || cw > w || ch > h) {
                        cw = w;
                        ch = h;
                    }
                    shapes[si].width = w;
                    shapes[si].height = h;
                    shapes[si].contentW = cw;
                    shapes[si].contentH = ch;
                    shapes[si].fps = fps;
                    shapes[si].ok = true;
                    if (cw > maxW) maxW = cw;
                    if (ch > maxH) maxH = ch;
                }
                nt->Release();
            }
            probe->Release();
        }


        // ---- 3a. 排补齐计划 ----------------------------------------------
        //
        // 用户要的语义（原话整理）：回放窗口 20 秒，实际只录到 15 秒，那差的
        // 5 秒用 NO SIGNAL 补上 —— 而且断在哪就补在哪。举的例子：录 5 秒 →
        // 服务退出 5 秒 → 又录 5 秒，成品应当是
        //   [5 秒真实] [5 秒 NO SIGNAL] [5 秒真实] [5 秒 NO SIGNAL]
        // 也就是**缺口按墙钟落在真实位置**，不是把 NO SIGNAL 全堆在片尾。
        //
        // 【为什么要按墙钟一路铺过去，而不是"补 lead + 补 gap"】见 DESIGN.md 9.4。
        // 老实现是"片头补一块、每片前面按需补一块、最后再兜个底"，看着对，实测
        // 两处都错：①缺口总量按 missingSec 算，而 lead+gap 可能比它大也可能比它
        // 小，成品于是随机变长变短（实测 21.5 秒 / 13.3 秒都出现过）；②"够长的
        // 缺口才补"用整秒 wall clock 和浮点时长混算，两片紧挨着也会被判成缺口。
        // 现在改成：从窗口起点开始，逐片问"这一片在墙钟上落在哪、它前面空了
        // 多久"，铺到正好 maxSeconds 为止，多出来的部分连片带遮罩一起裁掉。
        std::vector<PlanItem> plan;
        plan.reserve(copies.size() + 8);
        // 片头缺的那一截（窗口起点到第一片起点之间）。
        //
        // 用墙钟算，但有墙钟信息才敢用：老数据第四列是 0，只能退回"总共差
        // 多少"全堆片头（那时候没有更好的信息）。
        double lead = 0.0;
        if (maxSeconds_ > 0) {
            if (segs.front().startUnixMs > 0) {
                const double windowStart = axisNow - windowSec;
                lead = StartWallSec(segs.front()) - windowStart;
                if (lead < 0.0) lead = 0.0;
                if (lead > windowSec) lead = windowSec;
            } else if (recordedSec < windowSec) {
                lead = windowSec - recordedSec;
            }
        }
        if (lead > 0.05) plan.push_back(PlanItem{true, 0, lead, 0.0});

        // 按"成品必须正好 maxSeconds 秒"铺时间轴。maxSeconds_ == 0（不限时长，
        // 预览/调试用）时退化成"把每片原样接上"，不补也不裁。
        if (maxSeconds_ <= 0) {
            for (size_t si = 0; si < copies.size(); ++si) {
                plan.push_back(PlanItem{false, si, 0.0, 0.0});
            }
        } else {
            const double want = static_cast<double>(maxSeconds_);
            double placedSec = 0.0;
            for (const PlanItem& item : plan) {
                placedSec += item.noSignal ? item.seconds : 0.0;
            }
            // 铺到哪了（墙钟）。这是**时间轴游标**，必须从窗口起点开始算，
            // 不能写成"有片头遮罩时才给值、否则 0"。
            //
            // 【踩过的坑】原来这里写成 `lead > 0.05 ? 窗口起点 + lead : 0.0`，
            // 而 havePrevEnd 只看"有没有墙钟"。片头没缺口时（窗口把第一片切
            // 掉了前面）prevEndWall 就是 0，于是第一片的墙钟起点 1792…410 被
            // 当成"距离 0 点有十七亿秒的空档"，一个遮罩项吃满 20 秒，计划里
            // 再没有真实分片，导出报「分片里读不到任何可用的视频帧」。
            const double windowStart = axisNow - windowSec;
            double prevEndWall = windowStart + lead;
            bool havePrevEnd = segs.front().startUnixMs > 0;

            for (size_t si = 0; si < copies.size() && placedSec < want - 0.02; ++si) {
                if (!shapes[si].ok) continue;
                const double segSec = segSecs[si] > 0 ? segSecs[si] : 0.0;
                if (segSec <= 0.0) continue;

                // 这一片在墙钟上的起点。没有墙钟的片按"紧挨着上一片"处理。
                double segStartWall = 0.0;
                if (segs[si].startUnixMs > 0) {
                    segStartWall = StartWallSec(segs[si]);
                } else if (havePrevEnd) {
                    segStartWall = prevEndWall;
                }

                if (havePrevEnd && segStartWall > prevEndWall) {
                    const double gap = segStartWall - prevEndWall;
                    // 【门槛以下的小缺口怎么办：不补，直接把下一片接上】
                    // 尺寸每变一次就切一片，片可以短到 4 帧（kTransientFrames），
                    // 接缝本身能攒出上百毫秒；拖动窗口期间采集也是断续的，两片
                    // 之间真的断半秒很常见。这种"短过一片"的缝隙补成 NO SIGNAL
                    // 就是画面中间闪一下黑，人眼立刻能看见；而把下一片紧接在
                    // 上一片后面只是时间轴轻微压缩，看不出来。所以门槛以下的
                    // 缺口**什么都不做** —— plan 里不加遮罩项，下一片的样本
                    // 自然就接在当前游标后面。
                    const double gapFloor =
                        std::max(0.3, static_cast<double>(segmentSeconds_) * 0.25);
                    if (gap > gapFloor) {
                        const double take = std::min(gap, want - placedSec);
                        plan.push_back(PlanItem{true, 0, take, 0.0});
                        placedSec += take;
                        if (placedSec >= want - 0.02) break;
                    }
                }

                // 窗口起点可能落在这片中间：跳掉前面那截。
                double skip = 0.0;
                if (segs[si].startUnixMs > 0) {
                    if (windowStart > segStartWall) {
                        skip = std::min(windowStart - segStartWall, segSec);
                    }
                }
                const double avail = segSec - skip;
                if (avail <= 0.02) {
                    if (segs[si].startUnixMs > 0) {
                        prevEndWall = segStartWall + segSec;
                        havePrevEnd = true;
                    }
                    continue;
                }
                const double take = std::min(avail, want - placedSec);
                if (take <= 0.02) break;
                // take < avail 表示回放窗口在片中间就结束了，后面的不要。
                plan.push_back(PlanItem{false, si, take, skip});
                placedSec += take;

                if (segs[si].startUnixMs > 0) {
                    prevEndWall = segStartWall + segSec;
                    havePrevEnd = true;
                }
            }

            // 片尾补齐：铺完还不够，剩下的堆在最后。
            //
            // 真实场景：槽位轮转把最早那几片覆盖掉了（磁盘上只剩 14 秒），
            // 或者服务刚起来还没录满。不补这一段，用户设 20 秒只拿到 13 秒。
            if (placedSec < want - 0.02) {
                plan.push_back(PlanItem{true, 0, want - placedSec, 0.0});
            }
        }

        // 计划里到底铺了多少（摘要里要报"其中 X 秒用 NO SIGNAL 补齐"）。
        double plannedNoSignal = 0.0;
        for (const PlanItem& item : plan) {
            if (item.noSignal) plannedNoSignal += item.seconds;
        }

        // 【诊断】把计划铺开写进服务日志。
        //
        // 为什么要这一段：用户报"调整窗口大小后导出，末尾/片头出现 NO SIGNAL"，
        // 只看摘要（"9 个分片 / 20 秒 / 其中 2 秒补的"）根本分不清是
        //   (a) 分片集合里真有空洞（录制端丢片），
        //   (b) 计划器把片算漏了（lead / skip / gap 判错），
        //   (c) 某片 probe 出来 ok=false 被 continue 掉。
        // 三种的修法完全不同，猜一次就是半小时。计划项本来就在内存里，
        // 打出来只多几十行日志，却是唯一能一眼定位的东西。
        {
            std::string dump = "export_plan axisNow=" + std::to_string(axisNow) +
                               " windowSec=" + std::to_string(windowSec) +
                               " segs=" + std::to_string(segs.size()) +
                               " copies=" + std::to_string(copies.size());
            for (size_t si = 0; si < copies.size(); ++si) {
                const double st = segs[si].startUnixMs > 0 ? StartWallSec(segs[si]) : 0.0;
                dump += " | " + std::to_string(si) + ":" + segs[si].name + " ok=" +
                        (shapes[si].ok ? "1" : "0") + " dur=" + std::to_string(segSecs[si]) +
                        " wall=" + std::to_string(st);
            }
            for (const PlanItem& item : plan) {
                dump += " || ";
                dump += item.noSignal ? "NOSIGNAL " : "VIDEO ";
                if (!item.noSignal) dump += std::to_string(item.segIndex) + " ";
                dump += "sec=" + std::to_string(item.seconds) +
                        " skip=" + std::to_string(item.skipSec);
            }
            LogExportPlan(dump);
        }

        // 一个可读的分片都没有（全是 0 秒 / 全损坏）：先说清是为什么，别让
        // SinkWriter 抛一个 0xC00D4A44 出来 —— 那个错误码对用户毫无信息量。
        // 见 DESIGN.md 9.7。
        bool anyReadable = false;
        for (const auto& s : shapes) {
            if (s.ok) {
                anyReadable = true;
                break;
            }
        }
        if (!anyReadable) {
            MfRelease();
            fail("回放窗口里还没有录到任何画面（分片还没写出第一帧），稍等一下再导出。");
            return;
        }

        // H.264 的宏块是 16x16，宽高必须是偶数 —— 向上取偶，多出来的那一列/
        // 一行是黑的，肉眼看不出来（向下取会裁掉一列画面）。
        //
        // maxW/maxH 取的是**真实画面**尺寸（index.csv 第五、六列），不是文件
        // 帧尺寸 —— 过渡期画布可能大 2 倍，右下是补的黑边，按文件尺寸定画布
        // 会把用户的画面缩成左上角一个小方块。
        const uint32_t canvasW = (maxW + 1u) & ~1u;
        const uint32_t canvasH = (maxH + 1u) & ~1u;

        // 所有可读分片都和画布一样大 → 可以直拷。只要有一片不同就不行。
        //
        // 「一样大」看的是真实画面尺寸：分片文件本身可能带着过渡期的黑边
        // （比画布大），那种片子不能直拷 —— 搬过去就把黑边一起搬进成品了。
        bool uniform = true;
        for (const auto& s : shapes) {
            if (!s.ok) continue;
            if (s.contentW != canvasW || s.contentH != canvasH) {
                uniform = false;
                break;
            }
            if (s.width != s.contentW || s.height != s.contentH) {
                uniform = false; // 文件里带黑边，必须重编码裁掉
                break;
            }
        }
        // 要补 NO SIGNAL 就只能重编码：直拷通道搬的是现成的压缩样本，
        // 造不出"本来没录到"的画面。计划里只要有一个遮罩项，或者有分片被
        // 窗口切掉了头尾（takeSec < 整片），都得走重编码。
        if (plannedNoSignal > 0.0001) {
            for (const PlanItem& item : plan) {
                if (item.noSignal) {
                    uniform = false;
                    break;
                }
            }
        }

        // 输出容器：普通 MP4（不是 fMP4）。分片用 fMP4 是为了"随时被杀都能
        // 读"，成品用普通 MP4 是为了"任何播放器/剪辑软件都认"。
        //
        // 文件名：`<前缀>Replay_<时间戳>`。UniqueTarget 负责重名加 -2、-3。
        //
        // 【为什么 finalPath 要放在这里算】以前它算在块首，离真正建 writer
        // 隔了几十行；中途任何一条返回路径忘了删它，导出目录里就留下一个
        // 0 字节的成品名字（用户报过"导出失败，目录里还有个空文件"）。
        // 现在紧贴着 writer 创建，失败分支一眼能看全。
        const std::string baseName = ReplayFileName(prefix_, {}).substr(
            0, ReplayFileName(prefix_, {}).size() - 4); // 去掉 ".mp4"，由 UniqueTarget 补
        const fs::path finalPath = UniqueTarget(fs::u8path(exportDir_), baseName, ".mp4");

        // 【必须开 MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS】这个属性默认是 FALSE
        // （见 um\mfreadwrite.h 的说明），不开的话 SinkWriter 只挑微软软件编码器
        // mfh264enc.dll。录制端 MfEncoder 早就开了它，导出端一直漏着 ——
        // 2560x1440@144 的 20 秒成片纯 CPU 软编要跑十几秒，而 GPU 上只要 1 秒左右。
        IMFAttributes* sinkAttrs = nullptr;
        if (SUCCEEDED(MFCreateAttributes(&sinkAttrs, 3))) {
            sinkAttrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
            sinkAttrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
            sinkAttrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        }

        IMFSinkWriter* writer = nullptr;
        HRESULT hr = MFCreateSinkWriterFromURL(finalPath.wstring().c_str(), nullptr, sinkAttrs,
                                              &writer);
        if (sinkAttrs) sinkAttrs->Release();
        if (FAILED(hr) || !writer) {
            giveUp("建不了导出文件的写入器（hr=" + HrText(hr) + "）。");
            return;
        }

        DWORD streamIndex = 0;

        // ---- 时间轴（两条路共用同一套语义）--------------------------------
        //
        // 按**帧号 × 1e7 / 帧率**算时间戳，不用源 PTS：
        //   * 直拷通道遇到回落的 PTS 会**静默丢样本**（探针实测 180 帧只剩 150），
        //     而分片各有各的时间原点，拼起来必回落。
        //   * 1e7/144 这种除不尽的帧率，用"每帧累加整数间隔"会累积误差
        //     （2592 帧能漂成 17.999882 秒），按帧号算误差锁在 ±0.5 tick。
        // 每一片只在自己结束时舍入一次，误差不跨片累计。
        const uint32_t outFps = targetFps_ > 0 ? targetFps_ : 0;
        uint64_t outFrames = 0;   // 已经写进成品的输出帧数
        uint64_t totalFrames = 0; // 从分片里读到的源帧数
        uint32_t usedSegments = 0;
        double noSignalSec = 0.0; // 实际铺了多少秒遮罩（摘要里要报）
        std::string mergeErr;
        LONGLONG cursorTicks = 0;

        if (uniform) {
            // ---- 3c. 直拷 ---------------------------------------------------
            //
            // 以**最后一片**的原生类型为准（此时各片尺寸一致，取谁都一样，
            // 最后一片最"新"，属性也最完整）。
            //
            // 【为什么必须挑一片"真能读出类型"的】见 DESIGN.md 9.7。以前死盯
            // copies.back()：最后一片如果是刚被覆盖的 0 字节片子，或者尺寸探
            // 测时就被跳过的那一片，这里会直接放弃整次导出。改成从后往前找
            // 第一片能读出原生类型的。
            IMFMediaType* outType = nullptr;
            for (size_t k = copies.size(); k > 0; --k) {
                const size_t si = k - 1;
                if (!shapes[si].ok) continue;
                IMFSourceReader* first = nullptr;
                hr = MFCreateSourceReaderFromURL(copies[si].wstring().c_str(), nullptr, &first);
                if (FAILED(hr) || !first) continue;
                hr = first->GetNativeMediaType(
                    static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &outType);
                first->Release();
                if (SUCCEEDED(hr) && outType) break;
                outType = nullptr;
            }
            if (!outType) {
                writer->Release();
                giveUp("读不出分片的视频编码参数（分片可能已损坏）。");
                return;
            }

            // 【passthrough 的关键】输入类型 = 输出类型 = 压缩后的原生类型。
            // 这一步返回 S_OK 就说明 SinkWriter 不会去解压/重压，纯搬样本。
            hr = writer->AddStream(outType, &streamIndex);
            if (SUCCEEDED(hr)) hr = writer->SetInputMediaType(streamIndex, outType, nullptr);
            outType->Release();
            if (SUCCEEDED(hr)) hr = writer->BeginWriting();
            if (FAILED(hr)) {
                writer->Release();
                giveUp("分片的编码格式没法直拷进 MP4（hr=" + HrText(hr) + "）。");
                return;
            }

            // 按计划走，不是按分片列表走：计划里带着"从这片第几秒开始、
            // 要几秒"，窗口切在片中间时全靠它。
            for (const PlanItem& item : plan) {
                if (!mergeErr.empty()) break;
                if (item.noSignal) continue; // 直拷通道造不出遮罩（uniform 已拆开这种情况）
                const size_t si = item.segIndex;
                if (si >= copies.size()) continue;
                if (!shapes[si].ok) continue;
                const uint32_t fps = shapes[si].fps ? shapes[si].fps : 60u;

                // 这一片在计划里的时间范围（相对本片开头）。
                const double skipSec = item.skipSec > 0 ? item.skipSec : 0.0;
                const double takeSec = item.seconds > 0 ? item.seconds : 0.0;
                const double stopSec = takeSec > 0 ? skipSec + takeSec : 0.0;

                IMFSourceReader* reader = nullptr;
                HRESULT rhr =
                    MFCreateSourceReaderFromURL(copies[si].wstring().c_str(), nullptr, &reader);
                if (FAILED(rhr) || !reader) continue; // 单片读不了：跳过

                IMFMediaType* nativeType = nullptr;
                rhr = reader->GetNativeMediaType(
                    static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &nativeType);
                if (FAILED(rhr) || !nativeType) {
                    reader->Release();
                    continue;
                }
                rhr = reader->SetCurrentMediaType(
                    static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, nativeType);
                nativeType->Release();
                if (FAILED(rhr)) {
                    reader->Release();
                    continue;
                }

                // 这一片的输出帧率：有目标帧率就按目标，否则按源帧率。
                //
                // 【目标帧率高于源帧率、又不是整数倍时必须退回源帧率】
                // 见 DESIGN.md 9.9.1。用户配 240 帧，硬件编码器只接受 165 帧
                // （MfEncoder 自动降档，见 MfEncoder.h 的 EffectiveFps），分片
                // 里实际就是 165fps。如果这里还按 240 推时间轴，而
                //   repeat = 1（240 % 165 != 0，不能整数倍插帧），
                // 就等于「每秒只喂 165 个样本、每个样本只占 1/240 秒」——
                // 20 秒的内容被压成 13.7 秒，成片被"加速"了。用户原话是
                // 「帧率设的太高，导出的视频卡成ppt的」。
                //
                // 规则与重编码路径的 canvasFps 钳制一致：只有整数倍才升帧
                // （60→120、72→144 那种靠重复样本补齐，是有意义的升帧）；
                // 非整数倍就按源帧率输出，播放器按每帧自带的时长播，速度是对的。
                const bool canRaiseFps =
                    outFps && fps > 0 && outFps >= fps && (outFps % fps) == 0;
                const uint32_t segOutFps = canRaiseFps ? outFps : fps;

                // 每帧要写几遍才能达到目标帧率（只有整数倍才做）。
                //
                // 为什么只认整数倍：60 → 240 是 4 倍，每帧复制 4 份，时间轴精确；
                // 而 60 → 100 是 1.67 倍，要按 3:5 交错复制才不抖，那属于重采样
                // 算法了，收益不抵复杂度。非整数倍时直接按源帧率输出 —— 播放器
                // 照样按正确速度播（每帧自带时长），只是帧率不是用户设的那个。
                const uint32_t repeat = canRaiseFps ? (outFps / fps) : 1u;

                const long long segBaseTicks = cursorTicks;
                uint64_t segOutFrames = 0;
                uint64_t segFrames = 0;

                for (;;) {
                    DWORD flags = 0;
                    LONGLONG ts = 0;
                    IMFSample* sample = nullptr;
                    rhr = reader->ReadSample(
                        static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr,
                        &flags, &ts, &sample);
                    if (FAILED(rhr)) break;
                    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                        if (sample) sample->Release();
                        break;
                    }
                    if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
                        // 分片内部换格式（正常不会发生）：停在这一片，免得拼坏。
                        if (sample) sample->Release();
                        break;
                    }
                    if (!sample) continue; // gap / 无数据，继续读

                    // 被窗口切掉的头尾：按"这一片自己的时间轴"（ts）判。
                    //
                    // 源时间戳是压缩样本自带的显示时间，单位 100ns，从 0 起。
                    // 用它而不是数帧号，是因为片子中间可能有 gap 样本。
                    const double sampleSec =
                        static_cast<double>(ts) / 10000000.0;
                    if (skipSec > 0.0 && sampleSec < skipSec - 1e-6) {
                        sample->Release();
                        continue;
                    }
                    if (stopSec > 0.0 && sampleSec >= stopSec - 1e-6) {
                        sample->Release();
                        break;
                    }

                    // 同一帧写 repeat 遍，每遍往后推一帧。
                    for (uint32_t k = 0; k < repeat; ++k) {
                        const long long t0 = static_cast<long long>(
                            static_cast<double>(segOutFrames) * 10000000.0 /
                                static_cast<double>(segOutFps) +
                            0.5);
                        const long long t1 = static_cast<long long>(
                            static_cast<double>(segOutFrames + 1) * 10000000.0 /
                                static_cast<double>(segOutFps) +
                            0.5);
                        sample->SetSampleTime(segBaseTicks + t0);
                        sample->SetSampleDuration(t1 - t0);
                        const HRESULT whr = writer->WriteSample(streamIndex, sample);
                        if (FAILED(whr)) {
                            mergeErr = "写样本失败（hr=" + HrText(whr) + "）";
                            break;
                        }
                        ++segOutFrames;
                        ++outFrames;
                    }
                    sample->Release();
                    if (!mergeErr.empty()) break;

                    ++segFrames;
                    ++totalFrames;
                }

                // 整片推进时间轴：按这一片实际输出帧数换算，只在这里舍入一次。
                cursorTicks =
                    segBaseTicks + static_cast<long long>(static_cast<double>(segOutFrames) *
                                                             10000000.0 /
                                                         static_cast<double>(segOutFps) +
                                                     0.5);

                reader->Release();
                if (!mergeErr.empty()) break;
                if (segFrames > 0) ++usedSegments;
            }
        } else {
            // ---- 3c. 重编码：画布 = 最大尺寸，左上角对齐，右下补黑边 ---------
            //
            // 输出帧率：用户设了目标就按目标，否则取各片里**最高**的那个
            // （尺寸取最大、帧率也取最大，画面只进不退）。
            uint32_t canvasFps = outFps;
            if (canvasFps == 0) {
                for (const auto& s : shapes) {
                    if (s.ok && s.fps > canvasFps) canvasFps = s.fps;
                }
            }
            if (canvasFps == 0) canvasFps = 60;

            // 【画布帧率不能超过源分片真的能给的帧率】见 DESIGN.md 9.9.1。
            //
            // 用户配 240 帧，但硬件编码器只接受 165 帧（见 MfEncoder.h 的
            // EffectiveFps），分片里实际就是 165fps。如果画布还按 240 帧，
            // 重编码路径里 `repeat = canvasFps / srcFps` 不是整数（240/165），
            // 于是 rateFps 退回 165、时间戳按 165 推，而输出流的
            // MF_MT_FRAME_RATE 是 240 —— 编码器把 20 秒的时间戳压成 13.7 秒
            // （实测 nb_frames=3290 / duration=13.708），成片就被"加速"了，
            // 用户看到的正是"卡成 ppt 一样、还短了一截"。
            //
            // 规则：目标帧率高于所有源片帧率、且不是任何一片的整数倍时，把
            // 画布帧率降到**源里最高的那个**。这样 repeat=1、时间戳与输出流
            // 帧率一致，时长才对。整数倍的情况（60→120、72→144）保持原样，
            // 靠重复插帧补齐，那是有意义的升帧。
            if (outFps > 0) {
                uint32_t srcMaxFps = 0;
                for (const auto& s : shapes) {
                    if (s.ok && s.fps > srcMaxFps) srcMaxFps = s.fps;
                }
                if (srcMaxFps > 0 && outFps > srcMaxFps && (outFps % srcMaxFps) != 0) {
                    canvasFps = srcMaxFps;
                }
            }

            // 码率：按画布像素估。0.15 bit/像素/帧和 MfEncoder 是同一个经验值，
            // 但导出成片要留余量（用户可能拿去剪辑、二次压缩），下限 8Mbps。
            //
            // 【上限必须压住】1440p@144 按这个公式算出 79Mbps，20 秒成片 60MB，
            // 而同样的画面录制端分片只有 2.5Mbps（因为用户的 bitrateMbps=5）。
            // 重编码把码率放大 10 倍毫无意义 —— 源就那么多信息，再堆码率只是
            // 拿体积换肉眼看不出的细节。上限按「源分片实际码率的 2 倍」估：
            // 源码率取各片 ffprobe 的平均，拿不到就退回像素估算法。
            uint32_t bitrate = static_cast<uint32_t>(static_cast<double>(canvasW) * canvasH * 0.15 *
                                                         static_cast<double>(canvasFps) / 1e6 +
                                                     0.5);
            if (bitrate < 8) bitrate = 8;
            if (bitrate > 200) bitrate = 200;

            // 【按源分片实际码率压上限】上面那个像素估算法在 1440p@144 下算出
            // 79Mbps，重编码出来 20 秒成片 60MB；而同样画面录制端分片只有
            // 2.5Mbps（用户配的 bitrateMbps=5）。源里就只有那么多信息，把码率
            // 放大 10 倍纯粹拿体积换肉眼看不出的细节。
            //
            // 取各片实际字节数 / 实际时长得到源码率，上限 = 源码率 × 2（留一点
            // 余量给重编码的重新量化）。算不出来就保持像素估算法。
            double srcBitsPerSec = 0.0;
            double srcSecs = 0.0;
            for (size_t i = 0; i < copies.size() && i < segSecs.size(); ++i) {
                if (segSecs[i] <= 0.0) continue;
                std::error_code fe;
                const auto fsz = fs::file_size(copies[i], fe);
                if (fe) continue;
                srcBitsPerSec += static_cast<double>(fsz) * 8.0;
                srcSecs += segSecs[i];
            }
            if (srcSecs > 0.0) {
                const double srcMbps = srcBitsPerSec / srcSecs / 1e6;
                if (srcMbps > 0.0) {
                    const double cap = srcMbps * 2.0;
                    if (cap < static_cast<double>(bitrate)) {
                        bitrate = static_cast<uint32_t>(cap + 0.5);
                        if (bitrate < 8) bitrate = 8;
                    }
                }
            }

            IMFMediaType* encOut = nullptr;
            MFCreateMediaType(&encOut);
            encOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            encOut->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
            encOut->SetUINT32(MF_MT_AVG_BITRATE, bitrate * 1000000u);
            encOut->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            encOut->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
            MFSetAttributeSize(encOut, MF_MT_FRAME_SIZE, canvasW, canvasH);
            MFSetAttributeRatio(encOut, MF_MT_FRAME_RATE, canvasFps, 1);
            MFSetAttributeRatio(encOut, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

            IMFMediaType* encIn = nullptr;
            MFCreateMediaType(&encIn);
            encIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            encIn->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
            encIn->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            // 正 stride = 自上而下。不给这个属性，编码器会当成自下而上，画面
            // 上下颠倒（MfEncoder 里踩过同一个坑）。
            encIn->SetUINT32(MF_MT_DEFAULT_STRIDE, canvasW * 4u);
            MFSetAttributeSize(encIn, MF_MT_FRAME_SIZE, canvasW, canvasH);
            MFSetAttributeRatio(encIn, MF_MT_FRAME_RATE, canvasFps, 1);
            MFSetAttributeRatio(encIn, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

            // 关 B 帧。这一步**必须**作为 SetInputMediaType 的第三参数传下去，
            // 只塞 MF_SINK_WRITER_ENCODER_CONFIG 实测无效。
            //
            // 不关会怎样：编码器按显示顺序缓存 B 帧再重排，而我们喂的时间戳是
            // 按帧序号单调递增的（没有重排信息），两套顺序一打架，解出来就是
            // 拖影 / 叠影 —— 用户在导出成品里看到"重影"正是这个。
            IMFAttributes* encCfg = nullptr;
            MFCreateAttributes(&encCfg, 3);
            if (encCfg) {
                encCfg->SetUINT32(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
                encCfg->SetUINT32(CODECAPI_AVEncCommonLowLatency, TRUE);
                // 【和录制端保持一致】录制端 MfEncoder 用 QualityVsSpeed=20
                // 把 NVENC 从「质量优先」档压下来（videoencode 44%→30%）。
                // 导出端要是不同步，重编码一遍 20 秒 1440p 成片会慢好几倍。
                encCfg->SetUINT32(CODECAPI_AVEncCommonQualityVsSpeed, 20);
            }

            hr = writer->AddStream(encOut, &streamIndex);
            if (SUCCEEDED(hr)) hr = writer->SetInputMediaType(streamIndex, encIn, encCfg);
            if (encCfg) encCfg->Release();
            encOut->Release();
            encIn->Release();
            if (SUCCEEDED(hr)) hr = writer->BeginWriting();
            if (FAILED(hr)) {
                writer->Release();
                giveUp("建不了导出编码器（hr=" + HrText(hr) + "）。");
                return;
            }

            const size_t canvasPitch = static_cast<size_t>(canvasW) * 4u;
            const size_t canvasBytes = canvasPitch * canvasH;
            std::vector<uint8_t> canvas(canvasBytes, 0);

            // 把画布当前内容作为一帧样本写出去。
            //
            // 抽成 lambda 是因为有两条来源要共用：真实分片解出来的画面，和
            // NO SIGNAL 遮罩。时间戳由调用方给 —— 它才知道这一帧属于哪一段、
            // 用的是什么帧率。
            auto pushCanvas = [&](LONGLONG timeTicks, LONGLONG durationTicks) -> bool {
                IMFMediaBuffer* outBuf = nullptr;
                if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(canvasBytes), &outBuf)) ||
                    !outBuf) {
                    mergeErr = "分配编码缓冲失败（画布太大？）";
                    return false;
                }
                BYTE* dst = nullptr;
                if (FAILED(outBuf->Lock(&dst, nullptr, nullptr)) || !dst) {
                    outBuf->Release();
                    mergeErr = "锁定编码缓冲失败";
                    return false;
                }
                std::memcpy(dst, canvas.data(), canvasBytes);
                outBuf->Unlock();
                outBuf->SetCurrentLength(static_cast<DWORD>(canvasBytes));

                IMFSample* outSample = nullptr;
                if (FAILED(MFCreateSample(&outSample)) || !outSample) {
                    outBuf->Release();
                    mergeErr = "创建样本失败";
                    return false;
                }
                outSample->AddBuffer(outBuf);
                outBuf->Release();
                outSample->SetSampleTime(timeTicks);
                outSample->SetSampleDuration(durationTicks);

                const HRESULT whr = writer->WriteSample(streamIndex, outSample);
                outSample->Release();
                if (FAILED(whr)) {
                    mergeErr = "写样本失败（hr=" + HrText(whr) + "）";
                    return false;
                }
                ++outFrames;
                return true;
            };

            // 铺一段 NO SIGNAL。帧率固定用画布帧率 —— 这一段没有源帧率可
            // 参照，用画布的才不会和相邻段抖。
            auto emitNoSignal = [&](double seconds) -> bool {
                if (seconds <= 0.0) return true;
                FillNoSignal(canvas.data(), canvasW, canvasH, canvasPitch);
                const double wantFrames = seconds * static_cast<double>(canvasFps) + 0.5;
                const long long n = static_cast<long long>(wantFrames);
                const long long base = cursorTicks;
                for (long long k = 0; k < n; ++k) {
                    const long long t0 = static_cast<long long>(
                        static_cast<double>(k) * 10000000.0 / static_cast<double>(canvasFps) +
                        0.5);
                    const long long t1 = static_cast<long long>(
                        static_cast<double>(k + 1) * 10000000.0 / static_cast<double>(canvasFps) +
                        0.5);
                    if (!pushCanvas(base + t0, t1 - t0)) return false;
                }
                cursorTicks = base + static_cast<long long>(
                                        static_cast<double>(n) * 10000000.0 /
                                            static_cast<double>(canvasFps) +
                                        0.5);
                noSignalSec += static_cast<double>(n) / static_cast<double>(canvasFps);
                return true;
            };

            // 按计划走：真实分片段解码贴画布，NO SIGNAL 段铺遮罩。顺序就是
            // 墙钟顺序，所以成片里缺口出现在它真正发生的位置。
            for (const PlanItem& item : plan) {
                if (!mergeErr.empty()) break;

                if (item.noSignal) {
                    if (!emitNoSignal(item.seconds)) break;
                    continue;
                }

                const size_t si = item.segIndex;
                if (si >= copies.size()) continue;
                if (!shapes[si].ok) continue;

                const uint32_t srcW = shapes[si].width;
                const uint32_t srcH = shapes[si].height;
                // 真实画面尺寸 ≤ 文件帧尺寸。过渡期的分片文件比画面大
                // （右/下是补的黑边），贴画布时只取左上角这一块 —— 见
                // SegmentFile::contentW 的注释。
                const uint32_t contentW = shapes[si].contentW;
                const uint32_t contentH = shapes[si].contentH;
                const uint32_t srcFps = shapes[si].fps ? shapes[si].fps : 60u;

                // 窗口可能把这一片切在中间：跳过开头那截、到点就停。
                const double skipSec = item.skipSec > 0 ? item.skipSec : 0.0;
                const double takeSec = item.seconds > 0 ? item.seconds : 0.0;
                const double stopSec = takeSec > 0 ? skipSec + takeSec : 0.0;

                // 让 SourceReader 自己插解码器，并且要它输出 BGRA 而不是原生
                // H.264：重编码这一路必须拿到像素才能贴到画布上。
                IMFAttributes* readerAttrs = nullptr;
                if (SUCCEEDED(MFCreateAttributes(&readerAttrs, 1))) {
                    // 允许高级视频处理：H.264 → RGB32 的色彩转换要靠它。
                    readerAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
                }
                IMFSourceReader* reader = nullptr;
                HRESULT rhr = MFCreateSourceReaderFromURL(copies[si].wstring().c_str(),
                                                          readerAttrs, &reader);
                if (readerAttrs) readerAttrs->Release();
                if (FAILED(rhr) || !reader) continue;

                IMFMediaType* rgbType = nullptr;
                MFCreateMediaType(&rgbType);
                rgbType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                rgbType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
                rgbType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
                rgbType->SetUINT32(MF_MT_DEFAULT_STRIDE, srcW * 4u);
                MFSetAttributeSize(rgbType, MF_MT_FRAME_SIZE, srcW, srcH);
                MFSetAttributeRatio(rgbType, MF_MT_FRAME_RATE, srcFps, 1);
                MFSetAttributeRatio(rgbType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

                rhr = reader->SetCurrentMediaType(
                    static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, rgbType);
                rgbType->Release();
                if (FAILED(rhr)) {
                    reader->Release();
                    continue;
                }

                // 每帧写几遍才能跟上画布帧率（只认整数倍，理由同直拷路径）。
                uint32_t repeat = 1;
                if (canvasFps > srcFps && srcFps > 0 && (canvasFps % srcFps) == 0) {
                    repeat = canvasFps / srcFps;
                }
                // 时间轴的名义帧率：重复时就是画布帧率；源帧率更高时按源帧率
                // 走 —— 宁可帧率不是用户设的那个，也绝不能变速。
                const uint32_t rateFps = (repeat > 1 || srcFps >= canvasFps) ? canvasFps : srcFps;

                const long long segBaseTicks = cursorTicks;
                uint64_t segOutFrames = 0;
                uint64_t segFrames = 0;

                for (;;) {
                    DWORD flags = 0;
                    LONGLONG ts = 0;
                    IMFSample* sample = nullptr;
                    rhr = reader->ReadSample(
                        static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr,
                        &flags, &ts, &sample);
                    if (FAILED(rhr)) break;
                    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                        if (sample) sample->Release();
                        break;
                    }
                    if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
                        // 重编码这一路**不能**因为类型变更就停：SourceReader 在
                        // 第一条样本之前会拿"协商好的实际类型"顶回来一次
                        // （尤其尺寸不符、走了视频处理器的时候），那是正常
                        // 握手，不是流中间换格式。真正奇怪的帧由下面的拷贝
                        // 逻辑兜住（按画布裁剪，多出来的列/行直接不要）。
                        //
                        // 直拷路径（3b）则必须停：H.264 裸流中间换 SPS 会拼坏。
                        if (sample) sample->Release();
                        continue;
                    }
                    if (!sample) continue;

                    // 被回放窗口切掉的头尾，用源样本自己的时间戳判（100ns）。
                    // 用 ts 而不是数帧号：片子中间可能有 gap 样本。
                    const double sampleSec = static_cast<double>(ts) / 10000000.0;
                    if (skipSec > 0.0 && sampleSec < skipSec - 1e-6) {
                        sample->Release();
                        continue;
                    }
                    if (stopSec > 0.0 && sampleSec >= stopSec - 1e-6) {
                        sample->Release();
                        break;
                    }

                    // 取像素：优先 2D 锁（能拿到真实 RowPitch），拿不到再退回
                    // 连续缓冲。两种情况都要按 pitch 逐行拷，不能假设紧密排布。
                    IMFMediaBuffer* mbuf = nullptr;
                    if (FAILED(sample->ConvertToContiguousBuffer(&mbuf)) || !mbuf) {
                        sample->Release();
                        continue;
                    }

                    const uint8_t* srcPixels = nullptr;
                    LONG srcPitch = static_cast<LONG>(srcW * 4u);
                    IMF2DBuffer* mbuf2d = nullptr;
                    bool locked2d = false;
                    if (SUCCEEDED(mbuf->QueryInterface(IID_PPV_ARGS(&mbuf2d))) && mbuf2d) {
                        BYTE* scan0 = nullptr;
                        LONG pitch = 0;
                        if (SUCCEEDED(mbuf2d->Lock2D(&scan0, &pitch)) && scan0) {
                            srcPixels = scan0;
                            srcPitch = pitch;
                            locked2d = true;
                        } else {
                            mbuf2d->Release();
                            mbuf2d = nullptr;
                        }
                    }
                    BYTE* lockedBytes = nullptr;
                    if (!srcPixels) {
                        if (FAILED(mbuf->Lock(&lockedBytes, nullptr, nullptr)) || !lockedBytes) {
                            if (mbuf2d) mbuf2d->Release();
                            mbuf->Release();
                            sample->Release();
                            continue;
                        }
                        srcPixels = lockedBytes;
                    }

                    // 补黑边。只清"这一帧覆盖不到"的两条：右侧竖条（限制在本帧
                    // 高度内）和底部横条（整宽）。上一帧更大的部分会被这两条
                    // 覆盖掉，所以不用每帧全屏清零 —— 4K 画布全清一次是 33MB，
                    // 4800 帧下来就是 150GB 的 memset，纯浪费。
                    //
                    // 【为什么用 contentW/contentH 而不是 srcW/srcH】过渡期的
                    // 分片文件比真实画面大（右/下是录制端补的黑边）。按文件尺寸
                    // 算的话，这一帧会被当成"有内容"整块搬上画布，那块黑边就
                    // 一起搬进去了。只搬真实画面那一块。
                    const uint32_t frameW = contentW ? contentW : srcW;
                    const uint32_t frameH = contentH ? contentH : srcH;
                    const uint32_t copyRows = frameH < canvasH ? frameH : canvasH;
                    const uint32_t copyCols = frameW < canvasW ? frameW : canvasW;
                    if (frameW < canvasW) {
                        const size_t rightBytes = canvasPitch - static_cast<size_t>(frameW) * 4u;
                        for (uint32_t y = 0; y < copyRows; ++y) {
                            std::memset(canvas.data() + static_cast<size_t>(y) * canvasPitch +
                                            static_cast<size_t>(frameW) * 4u,
                                        0, rightBytes);
                        }
                    }
                    if (frameH < canvasH) {
                        std::memset(canvas.data() + static_cast<size_t>(frameH) * canvasPitch, 0,
                                    static_cast<size_t>(canvasH - frameH) * canvasPitch);
                    }

                    for (uint32_t y = 0; y < copyRows; ++y) {
                        std::memcpy(canvas.data() + static_cast<size_t>(y) * canvasPitch,
                                    srcPixels + static_cast<size_t>(y) * srcPitch,
                                    static_cast<size_t>(copyCols) * 4u);
                    }

                    if (locked2d) {
                        mbuf2d->Unlock2D();
                    } else {
                        mbuf->Unlock();
                    }
                    if (mbuf2d) mbuf2d->Release();
                    mbuf->Release();
                    sample->Release();

                    // 同一帧写 repeat 遍。
                    for (uint32_t k = 0; k < repeat; ++k) {
                        const long long t0 = static_cast<long long>(
                            static_cast<double>(segOutFrames) * 10000000.0 /
                                static_cast<double>(rateFps) +
                            0.5);
                        const long long t1 = static_cast<long long>(
                            static_cast<double>(segOutFrames + 1) * 10000000.0 /
                                static_cast<double>(rateFps) +
                            0.5);
                        if (!pushCanvas(segBaseTicks + t0, t1 - t0)) break;
                        ++segOutFrames;
                    }
                    if (!mergeErr.empty()) break;

                    ++segFrames;
                    ++totalFrames;
                }

                cursorTicks = segBaseTicks +
                              static_cast<long long>(static_cast<double>(segOutFrames) *
                                                         10000000.0 /
                                                     static_cast<double>(rateFps) +
                                                     0.5);

                reader->Release();
                if (segFrames > 0) ++usedSegments;
            }
        }

        // 成品时长 = 时间轴推到了多远（下面再问一次文件本身校准）。
        const double cursorSec = static_cast<double>(cursorTicks) / 10000000.0;

        HRESULT fhr = writer->Finalize();
        writer->Release();

        if (!mergeErr.empty()) {
            std::error_code ecx;
            fs::remove(finalPath, ecx);
            giveUp(mergeErr);
            return;
        }
        if (FAILED(fhr)) {
            std::error_code ecx;
            fs::remove(finalPath, ecx);
            giveUp("收尾写入 MP4 索引失败（hr=" + HrText(fhr) + "）。");
            return;
        }
        if (totalFrames == 0 || usedSegments == 0) {
            std::error_code ecx;
            fs::remove(finalPath, ecx);
            giveUp("分片里读不到任何可用的视频帧。");
            return;
        }

        // ---- 4. 校验产物 --------------------------------------------------
        uint64_t bytes = 0;
        {
            std::error_code ecSize;
            // 必须走 u8path：路径里可能有中文用户名，用 ANSI 的 string()
            // 会 ENOENT，把十几 MB 的成品判成空文件（踩过）。
            bytes = static_cast<uint64_t>(fs::file_size(finalPath, ecSize));
            if (ecSize) bytes = 0;
        }
        if (bytes == 0) {
            std::error_code ecx;
            fs::remove(finalPath, ecx);
            giveUp("合并完成但产物是空的（" + PathToUtf8(finalPath) + "）。");
            return;
        }

        // 成品时长直接问文件本身（和探测分片是同一个函数），不再去解析
        // 什么文本输出。
        double outSeconds = cursorSec;
        {
            const SegmentProbe probe = ProbeSegment(finalPath);
            if (probe.ok && probe.durationSec > 0) outSeconds = probe.durationSec;
        }

        // 重编码路径要说清"画布比画面大"，否则用户看到黑边会以为录坏了。
        std::string canvasNote;
        if (!uniform) {
            char note[160];
            std::snprintf(note, sizeof(note),
                          "（按窗口内最大尺寸 %ux%u 输出，较小的画面贴左上角补黑边）", canvasW,
                          canvasH);
            canvasNote = note;
        }

        // 补过 NO SIGNAL 就明说，免得用户以为录到了、只是画面黑的。
        if (noSignalSec > 0.05) {
            char note[160];
            std::snprintf(note, sizeof(note), "（其中 %.1f 秒没录到，用 NO SIGNAL 补齐）",
                          noSignalSec);
            canvasNote += note;
        }
        if (skippedCopy > 0) {
            char note[120];
            std::snprintf(note, sizeof(note), "（跳过 %u 个正在被重写的分片）",
                          static_cast<unsigned>(skippedCopy));
            canvasNote += note;
        }

        char summary[400];
        std::snprintf(summary, sizeof(summary), "%u 个分片 / %.1f 秒 / %.1f MB%s", usedSegments,
                      outSeconds, static_cast<double>(bytes) / (1024.0 * 1024.0),
                      canvasNote.c_str());

        {
            std::lock_guard<std::mutex> lk(mu_);
            status_.segmentCount = usedSegments;
            status_.durationSeconds = outSeconds;
            status_.bytes = bytes;
            status_.elapsedMs = elapsedMs();
        }
        (void)totalFrames;
        (void)outFrames;

        Finish(true, summary, PathToUtf8(finalPath), tmpDir, {});
        MfRelease();
        return;
    }
}

} // namespace shadow
