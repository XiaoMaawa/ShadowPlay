// SegmentRecorder.cpp —— 分片录制的实现。设计说明见 SegmentRecorder.h。
#include "SegmentRecorder.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#include "FfmpegUtil.h"
#include "JsonUtil.h"

namespace shadow {
namespace fs = std::filesystem;

// 由 main.cpp 提供：子进程作业对象。挂进作业的进程在服务退出时会被内核
// 一并收掉，不留孤儿 ffmpeg 继续写分片。返回 nullptr 表示没有作业对象。
extern "C" void* ShadowChildJob();

namespace {

// ---------------------------------------------------------------------------
// 命令行拼装
//
// ResolveFfmpeg / FromUtf8 / ToUtf8 / QuoteArg 已经挪到 FfmpegUtil（导出端也要
// 用同一份，免得路径里带中文时两边的引号规则不一致）。本地只留"往管道里写干净"。
// ---------------------------------------------------------------------------

bool WriteAll(HANDLE h, const uint8_t* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        const DWORD chunk =
            static_cast<DWORD>(std::min<size_t>(len - off, 1u << 20)); // 每次最多 1MB
        DWORD written = 0;
        if (!WriteFile(h, data + off, chunk, &written, nullptr) || written == 0) return false;
        off += written;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 编码器自动选择
//
// 以前写死 libx264（软编）。1080p60 它跟得上，但 2560x1440@240 就不行了：
// 每拍 4.17ms 要编码 14.7MB 的 BGRA，实测只能跑到 150~180fps —— 用户看到的
// 「GPU 高负载时帧数不达标」有一大半就是软编吃满 CPU 造成的。
//
// 硬件编码器（NVENC / QSV / AMF）把编码丢给显卡的专用电路，同样的画面几乎
// 不占 CPU，240fps 轻松跑满。代价是画质略逊于软编，但这个场景（即时重放）
// 比特率本来就给得足，看不出来。
//
// 选择顺序：显式环境变量 > nvenc > qsv > amf > libx264。
// 探测方法：`ffmpeg -hide_banner -encoders` 里有没有这个名字。这一步只在
// 建录制器时做一次（每路一次），几十毫秒，比"猜错了再回退"简单得多。
// ---------------------------------------------------------------------------
std::string ChooseEncoder(const std::string& ffmpegPath) {
    char buf[64] = {0};
    const DWORD len = GetEnvironmentVariableA("SHADOW_FFMPEG_ENCODER", buf, sizeof(buf));
    if (len > 0 && len < sizeof(buf)) return buf; // 用户指定了，照办

    // 列一遍编码器。用管道收 stdout，失败就当"全都没有"。
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return "libx264";
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = INVALID_HANDLE_VALUE;
    si.hStdOutput = wr;
    si.hStdError = INVALID_HANDLE_VALUE;

    PROCESS_INFORMATION pi{};
    std::wstring cmd = FromUtf8("\"" + ffmpegPath + "\" -hide_banner -encoders");
    std::string listing;
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                       nullptr, &si, &pi)) {
        CloseHandle(wr);
        char out[4096];
        DWORD got = 0;
        while (ReadFile(rd, out, sizeof(out) - 1, &got, nullptr) && got > 0) {
            out[got] = 0;
            listing.append(out, got);
            if (listing.size() > 65536) break; // 只会更大，不用全收
        }
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        CloseHandle(wr);
    }
    CloseHandle(rd);

    for (const char* name : {"h264_nvenc", "h264_qsv", "h264_amf"}) {
        if (listing.find(name) != std::string::npos) return name;
    }
    return "libx264";
}

/** ffmpeg 的参数表。返回可执行文件路径 + 参数。 */
std::vector<std::string> BuildArgs(const std::string& ffmpegPath, const std::string& segmentsDir,
                                   const std::string& listFile, const VideoSpec& video,
                                   uint32_t width, uint32_t height, uint32_t fps,
                                   uint32_t keepSegments, uint32_t segmentSeconds) {
    const uint32_t bitrateKbps = (video.bitrateMbps ? video.bitrateMbps : 20u) * 1000u;
    const uint32_t maxrateKbps = bitrateKbps + bitrateKbps / 4; // 1.25x，给运动画面留头
    const uint32_t bufsizeKbps = bitrateKbps * 2;

    // 每 N 个分片一个 IDR：保证任何分片都能独立起播，也让滚动删除切在干净点上。
    const unsigned gopFrames = fps * segmentSeconds;

    // 分片文件名：6 位序号，从 0 开始。
    const std::string pattern = PathToUtf8(fs::u8path(segmentsDir) / "%06d.ts");

    std::vector<std::string> args;
    args.push_back(ffmpegPath);
    args.push_back("-hide_banner");
    args.push_back("-loglevel");
    args.push_back("error");   // 只在真错时说话，免得刷爆服务日志
    args.push_back("-nostdin"); // 别再读我们自己的控制台

    // ---- 输入：stdin 上的裸 BGRA ----
    args.push_back("-f");
    args.push_back("rawvideo");
    args.push_back("-pixel_format");
    args.push_back("bgra");
    args.push_back("-video_size");
    args.push_back(std::to_string(width) + "x" + std::to_string(height));
    args.push_back("-framerate");
    args.push_back(std::to_string(fps));
    args.push_back("-i");
    args.push_back("pipe:0");

    // ---- 编码 ----
    // 自动选硬件编码器（NVENC/QSV/AMF），没有才退回 libx264。见 ChooseEncoder 的注释。
    const std::string encoder = ChooseEncoder(ffmpegPath);
    const bool isNvenc = encoder == "h264_nvenc";
    const bool isQsv = encoder == "h264_qsv";
    const bool isAmf = encoder == "h264_amf";
    const bool isX264 = encoder == "libx264";

    // BGRA -> yuv420p。scale=out_range=tv 是默认电视范围，播放器都认。
    //
    // `trunc(iw/2)*2:trunc(ih/2)*2` 不是可有可无的 —— H.264（yuv420p）要求宽高
    // 都是偶数，奇数尺寸下 libx264 直接拒绝开工：
    //     [libx264] width not divisible by 2 (1545x773)
    //     Error while opening encoder - maybe incorrect parameters such as
    //     bit_rate, rate, width or height.
    // 然后 ffmpeg 立刻退出、一帧都不写，表现就是界面上那句
    // 「ffmpeg 提前退出（分片写入中断）」。
    //
    // WGC 抓的是窗口**真实**客户区大小，奇数很常见（1545x773），不留神就必现。
    // 用 trunc 向下取到偶数而不是 scale 到固定值：只丢最多 1 行/1 列像素，
    // 且不改变画面比例。
    //
    // ---- 缩放（video.scaleWidth / scaleHeight）----
    // 输入仍然是**原始**尺寸的裸 BGRA（采集端不缩放，省一次全画面搬运），
    // 缩放交给 ffmpeg 在转 yuv420p 的同一步里做，GPU/多线程都归它调度。
    //
    // 目标是高帧率（120/240）时的关键开关：2560x1440 BGRA 一帧 14.7MB，
    // 240fps 就是 3.5GB/s，光 memcpy 就超掉 4.17ms 的每拍预算，
    // 实测只能到 146fps。降到 1280x720 后每帧 3.7MB，管道带宽和编码
    // 压力都降 4 倍，240fps 才真的能满。
    //
    // 宽高各自可选：只填一个时另一个按原比例算（保持画面不变形）。
    // 值仍要向下取偶 —— 手填 1281 也会被修正成 1280，否则又撞上面那个坑。
    const uint32_t sw = video.scaleWidth;
    const uint32_t sh = video.scaleHeight;
    std::string scaleFilter;
    if (sw > 0 || sh > 0) {
        // 只有一个填了：按原比例推另一个（-2 让 ffmpeg 自己算成偶数，且不变形）。
        if (sw > 0 && sh > 0) {
            scaleFilter = "scale=" + std::to_string(sw & ~1u) + ":" + std::to_string(sh & ~1u);
        } else if (sw > 0) {
            // 锚定宽，高按比例；-2 让 ffmpeg 自己算成偶数。
            scaleFilter = "scale=" + std::to_string(sw & ~1u) + ":-2";
        } else {
            scaleFilter = "scale=-2:" + std::to_string(sh & ~1u);
        }
        scaleFilter += ":flags=bilinear,format=yuv420p";
    }
    args.push_back("-vf");
    if (scaleFilter.empty()) {
        args.push_back("scale=trunc(iw/2)*2:trunc(ih/2)*2:out_range=tv,format=yuv420p");
    } else {
        // out_range=tv 挂在缩放后面：先缩放到目标尺寸，再走电视范围。
        args.push_back("scale=trunc(iw/2)*2:trunc(ih/2)*2:out_range=tv," + scaleFilter);
    }

    args.push_back("-c:v");
    args.push_back(encoder);

    // 每个编码器认的参数不一样，写死 ultrafast/zerolatency 会让硬编直接报错。
    if (isX264) {
        args.push_back("-preset");
        args.push_back("ultrafast");
        args.push_back("-tune");
        args.push_back("zerolatency");
    } else if (isNvenc) {
        // p1 = 最快档；llhq 是低延迟高画质，ul 是超低延迟。录屏用 llhq 画质更好。
        // zerolatency 把编码队列压到 1 帧，免得硬件排队把每拍预算吃掉。
        args.push_back("-preset");
        args.push_back("p1");
        args.push_back("-tune");
        args.push_back("ll");
        args.push_back("-rc");
        args.push_back("cbr");
        args.push_back("-zerolatency");
        args.push_back("1");
        // 每路一个 NVENC 会话，显存里只留 4 个 surface。
        args.push_back("-surfaces");
        args.push_back("4");
    } else if (isQsv) {
        args.push_back("-preset");
        args.push_back("veryfast");
        args.push_back("-look_ahead");
        args.push_back("0");
    } else if (isAmf) {
        args.push_back("-quality");
        args.push_back("speed");
        args.push_back("-rc");
        args.push_back("cbr");
    }

    args.push_back("-b:v");
    args.push_back(std::to_string(bitrateKbps) + "k");
    args.push_back("-maxrate");
    args.push_back(std::to_string(maxrateKbps) + "k");
    args.push_back("-bufsize");
    args.push_back(std::to_string(bufsizeKbps) + "k");
    args.push_back("-g");
    args.push_back(std::to_string(gopFrames ? gopFrames : 60u));
    args.push_back("-force_key_frames");
    args.push_back("expr:gte(t,n_forced*" + std::to_string(segmentSeconds) + ")");
    if (isX264) args.push_back("-sc_threshold"); // 只有 x264 认这个
    if (isX264) args.push_back("0");
    // 硬编在切分片处强制 IDR（分片必须能独立起播，导出要按精确时间轴拼）。
    if (!isX264) {
        args.push_back("-forced-idr");
        args.push_back("1");
    }

    // ---- 分片输出 ----
    args.push_back("-f");
    args.push_back("segment");
    args.push_back("-segment_time");
    args.push_back(std::to_string(segmentSeconds));
    args.push_back("-segment_format");
    args.push_back("mpegts");
    // 关键：环形覆盖。写到第 keepSegments+1 个时，ffmpeg 自动把第 1 个删掉。
    // 这样"总时长不超过回放时长"是 muxer 保证的，不需要我们在外面删文件。
    args.push_back("-segment_wrap");
    args.push_back(std::to_string(keepSegments));
    args.push_back("-reset_timestamps");
    args.push_back("1");
    args.push_back("-segment_list");
    args.push_back(listFile);
    args.push_back("-segment_list_type");
    args.push_back("csv");
    args.push_back("-segment_list_flags");
    args.push_back("+live"); // 清单每切一片就重写，导出/界面能看到最新的
    args.push_back(pattern);

    return args;
}

} // namespace

// ---------------------------------------------------------------------------
// 目录分配
// ---------------------------------------------------------------------------

namespace {

/** 把任意显示名压成只含 [a-z0-9-] 的短名，空则返回 "src"。 */
std::string Slugify(const std::string& name, size_t maxLen) {
    std::string out;
    bool lastDash = false;
    for (unsigned char c : name) {
        if (c >= 'a' && c <= 'z') {
            out.push_back(static_cast<char>(c));
            lastDash = false;
        } else if (c >= 'A' && c <= 'Z') {
            out.push_back(static_cast<char>(c - 'A' + 'a'));
            lastDash = false;
        } else if (c >= '0' && c <= '9') {
            out.push_back(static_cast<char>(c));
            lastDash = false;
        } else {
            // 中文字节、空格、符号一律折成连字符，但不连续出两个
            if (!lastDash && !out.empty()) {
                out.push_back('-');
                lastDash = true;
            }
        }
        if (out.size() >= maxLen) break;
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? std::string("src") : out;
}

const char* SourceKindSlug(SourceKind k) {
    switch (k) {
        case SourceKind::Desktop: return "desktop";
        case SourceKind::Monitor: return "monitor";
        case SourceKind::Window:  return "win";
    }
    return "src";
}

/** 读 .index.json 里的 profileId -> dir 映射。 */
bool ReadIndex(const fs::path& indexPath, std::vector<std::pair<std::string, std::string>>& out) {
    std::ifstream in(indexPath, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    if (text.empty()) return true;

    // 极简解析：只找 "profiles": { "id": "dir", ... } 里的一层键值对。
    // 写这份文件的只有我们自己，格式受控。
    const size_t key = text.find("\"profiles\"");
    if (key == std::string::npos) return true;
    const size_t open = text.find('{', key);
    if (open == std::string::npos) return true;

    int depth = 0;
    size_t i = open;
    std::string pendingKey;
    const auto skipWs = [&](size_t& p) {
        while (p < text.size() &&
               (text[p] == ' ' || text[p] == '\n' || text[p] == '\r' || text[p] == '\t')) {
            ++p;
        }
    };
    for (; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '{') {
            ++depth;
            continue;
        }
        if (c == '}') {
            --depth;
            if (depth < 1) break;
            continue;
        }
        if (c != '"') continue;
        // 读一个字符串
        std::string s;
        ++i;
        while (i < text.size() && text[i] != '"') {
            if (text[i] == '\\' && i + 1 < text.size()) ++i;
            s.push_back(text[i]);
            ++i;
        }
        size_t p = i + 1;
        skipWs(p);
        if (p < text.size() && text[p] == ':' && depth == 1) {
            size_t q = p + 1;
            skipWs(q);
            if (q < text.size() && text[q] == '"') {
                std::string v;
                ++q;
                while (q < text.size() && text[q] != '"') {
                    if (text[q] == '\\' && q + 1 < text.size()) ++q;
                    v.push_back(text[q]);
                    ++q;
                }
                out.emplace_back(s, v);
            }
        } else if (depth == 1) {
            pendingKey = s;
        }
        (void)pendingKey;
        i = p;
        if (i >= text.size()) break;
        --i;
    }
    return true;
}

} // namespace

namespace {

/**
 * 分配目录这一整段是**读-改-写**：读 .index.json → 挑一个没被占的名字 →
 * 建目录 → 整份重写 .index.json。多路配置同时启动时会并发跑到这里
 * （Session.cpp 里每个会话各自在自己的线程上调 AllocateProfileDir），
 * 于是：
 *   - 两路都读到旧的 index，挑中同一个名字；
 *   - 两路都写自己的 .tmp，然后都去 rename 覆盖 .index.json —— Windows 的
 *     MoveFileEx 在目标已被别的线程抢先改名时不覆盖，直接 ERROR_ACCESS_DENIED，
 *     输的那路就报「替换 .index.json 失败」而整路录制起不来。
 * 用一把进程内锁把这段串起来。跨进程（服务 + 预览）仍有窗口，靠下面的
 * rename 重试兜住。
 */
std::mutex& IndexFileMutex() {
    static std::mutex mu;
    return mu;
}

} // namespace

namespace {

/**
 * 一路一个分配记录文件，路径是 `<recordRoot>/.alloc/<profileId>.json`。
 *
 * 为什么不继续用共享的 `.index.json`：那是一份**读-改-写**的文件，多路配置
 * 同时启动时必然竞争。踩过的坑是一路 rename 覆盖时撞上
 * `ERROR_SHARING_VIOLATION`（The process cannot access the file because it is
 * being used by another process.），重试 20 次仍失败 —— 那一路录制直接起不来，
 * 界面显示「替换 .index.json 失败」，而且失败会随机落在哪一路上。
 *
 * 根除办法不是"把锁再加厚一层"，而是**取消共享的可变文件**：
 *   * 谁占哪个目录，由 create_directory 的原子性决定（成功即是我的）；
 *   * 谁分配过什么，记在自己那个文件里 —— 只有自己在写，没有第二个人竞争。
 *
 * profileId 会被用作文件名，所以必须过滤掉路径分隔符等危险字符。
 */
std::string SafeFileStem(const std::string& id) {
    std::string out;
    out.reserve(id.size());
    for (unsigned char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
        out.push_back(ok ? static_cast<char>(c) : '_');
    }
    if (out.empty()) out = "profile";
    return out;
}

} // namespace

std::string AllocateProfileDir(const std::string& recordRoot, const Profile& profile,
                               std::string& outError) {
    outError.clear();
    if (recordRoot.empty()) {
        outError = "没有配置录制根目录";
        return {};
    }

    std::error_code ec;
    fs::path root = fs::u8path(recordRoot);
    fs::create_directories(root, ec);
    if (ec) {
        outError = "建不了录制根目录：" + ec.message();
        return {};
    }

    // ---- 0) 有新式数字编号就直接用它，一了百了 ---------------------------
    //
    // 目录名 = `records/<num>`，num 来自 profiles.json 的 "num" 字段，
    // 由界面在创建配置时分配、之后永不变。这样：
    //   * 名字和配置一一对应，不用再"猜"哪堆录像属于哪条配置；
    //   * 不再有重名补 -2/-3 的问题，也不会因为改名/换源类型又长出一个新目录；
    //   * 中文名不再被 Slugify 折成一串连字符导致所有配置撞名。
    //
    // 老的 `<源>-<名字>-<尾号>` 目录仍然认领（下面第 1、2 步），保证升级后
    // 主人已有的录像不会被丢下；认领到的老目录会**继续沿用**，因为迁移它
    // 需要搬动正在被 ffmpeg 写着的文件，风险大于收益。
    if (profile.num > 0) {
        const std::string name = std::to_string(profile.num);
        fs::path dir = root / fs::u8path(name);
        ec.clear();
        fs::create_directories(dir / "segments", ec);
        if (ec) {
            outError = "录像目录建不起来：" + ec.message();
            return {};
        }

        // 每建一次就写一份 meta.json，用来说明这堆录像属于哪条配置。
        // （界面把配置名改了，这里的名字也跟着更新，但目录名 num 不动。）
        {
            std::ofstream meta(dir / "meta.json", std::ios::binary | std::ios::trunc);
            if (meta) {
                meta << "{\n"
                     << "  \"profileId\": \"" << JsonEscape(profile.id) << "\",\n"
                     << "  \"profileName\": \"" << JsonEscape(profile.name) << "\",\n"
                     << "  \"sourceKind\": \"" << SourceKindSlug(profile.source.kind) << "\",\n"
                     << "  \"directory\": \"" << JsonEscape(name) << "\"\n"
                     << "}\n";
            }
        }
        return PathToUtf8(dir);
    }

    const fs::path allocDir = root / ".alloc";
    fs::create_directories(allocDir, ec);
    if (ec) {
        outError = "建不了分配记录目录：" + ec.message();
        return {};
    }

    const fs::path indexPath = root / ".index.json";
    const fs::path allocPath = allocDir / (SafeFileStem(profile.id) + ".json");

    // ---- 1) 本路自己的分配记录：有就直接复用 -----------------------------
    //
    // 这是重启后"录像还在同一个目录里"的保证。自己的文件、只有自己在写，
    // 所以读它不需要任何锁。
    {
        std::ifstream in(allocPath, std::ios::binary);
        if (in) {
            std::stringstream ss;
            ss << in.rdbuf();
            const std::string text = ss.str();
            // 格式极简，只有一个字段：{"dir": "目录名"}
            const size_t key = text.find("\"dir\"");
            if (key != std::string::npos) {
                const size_t colon = text.find(':', key);
                const size_t open = colon == std::string::npos ? std::string::npos
                                                               : text.find('"', colon);
                const size_t close = open == std::string::npos ? std::string::npos
                                                               : text.find('"', open + 1);
                if (close != std::string::npos && close > open + 1) {
                    const std::string name = text.substr(open + 1, close - open - 1);
                    if (!name.empty() && name.find("..") == std::string::npos &&
                        name.find('/') == std::string::npos &&
                        name.find('\\') == std::string::npos) {
                        fs::path dir = root / fs::u8path(name);
                        ec.clear();
                        fs::create_directories(dir / "segments", ec);
                        if (!ec) return PathToUtf8(dir);
                        outError = "录像目录建不起来：" + ec.message();
                        return {};
                    }
                }
            }
        }
    }

    // ---- 2) 兼容旧的共享索引：读它来找本路的旧分配 -------------------------
    //
    // 只读，不写。主人已有的录像目录靠这一步保住 —— 升级之后那些目录会被
    // 认领回同一个 profile，不会被重新分配成新名字。
    {
        std::vector<std::pair<std::string, std::string>> known;
        IndexFileMutex().lock();
        ReadIndex(indexPath, known);
        IndexFileMutex().unlock();

        for (const auto& kv : known) {
            if (kv.first != profile.id || kv.second.empty()) continue;
            fs::path dir = root / fs::u8path(kv.second);
            ec.clear();
            fs::create_directories(dir / "segments", ec);
            if (ec) continue;
            // 认领成功，写进自己的分配文件，下次就不用再读索引了。
            std::ofstream rec(allocPath, std::ios::binary | std::ios::trunc);
            if (rec) rec << "{\"dir\": \"" << JsonEscape(kv.second) << "\"}\n";
            return PathToUtf8(dir);
        }
    }

    // ---- 3) 新分配 --------------------------------------------------------
    //
    // 名字用 <源类型>-<短名>-<id 后4位>，重名就加 -2、-3。
    //
    // 关键：判定"这个名字被占了没有"**不靠任何共享表**，而是直接
    // create_directory —— 它是原子的，成功就说明这个名字是本路的，
    // 失败就说明已经被别人（别的配置、上一次的服务、甚至另一个进程）占了。
    // 这样即使索引文件整个丢了或者写坏了，也绝不会两路录进同一个目录。
    std::string tail = profile.id;
    if (tail.size() > 4) tail = tail.substr(tail.size() - 4);
    const std::string base = std::string(SourceKindSlug(profile.source.kind)) + "-" +
                             Slugify(profile.name, 24) + "-" + Slugify(tail, 8);

    fs::path dir;
    std::string chosen;
    for (int attempt = 1; attempt < 1000; ++attempt) {
        chosen = (attempt == 1) ? base : (base + "-" + std::to_string(attempt));
        fs::path candidate = root / fs::u8path(chosen);
        ec.clear();
        // 已经存在时 create_directory 不算错误（ec 为空），所以必须分开判：
        // 先看存在性，再看"由本路创建成功"。
        const bool existed = fs::exists(candidate, ec);
        if (ec) continue;
        if (existed) {
            // 目录可能是上一次录制的残留（索引丢了），也可能是别人正在用。
            // 只要它的 meta.json 不是别的 profile，就认领它。
            std::ifstream meta(candidate / "meta.json", std::ios::binary);
            bool sameProfile = false;
            if (meta) {
                std::stringstream ss;
                ss << meta.rdbuf();
                const std::string t = ss.str();
                sameProfile = t.find("\"profileId\": \"" + JsonEscape(profile.id) + "\"") !=
                              std::string::npos;
            }
            if (!sameProfile) continue;
            // meta 是我们的，但还得确认它没有正在被别的会话用 —— 由调用方
            // （每路一个 id）保证 id 唯一，这里不再重复判断。
            dir = candidate;
            break;
        }

        ec.clear();
        const bool made = fs::create_directories(candidate / "segments", ec);
        if (ec) {
            // 别人在同一瞬间抢先建了 —— 换下一个候选名，不是错误。
            continue;
        }
        if (!made) continue; // 竞态下没建成，重试下一个名字
        dir = candidate;
        break;
    }

    if (dir.empty()) {
        outError = "分配不到录像目录（重名候选都试过了）";
        return {};
    }

    // 记进本路自己的分配文件。
    {
        std::ofstream rec(allocPath, std::ios::binary | std::ios::trunc);
        if (!rec) {
            outError = "写不了分配记录：" + PathToUtf8(allocPath);
            return {};
        }
        rec << "{\"dir\": \"" << JsonEscape(chosen) << "\"}\n";
    }

    // 每个目录一份 meta.json，说明这堆录像属于哪个配置。
    {
        std::ofstream meta(dir / "meta.json", std::ios::binary | std::ios::trunc);
        if (meta) {
            meta << "{\n"
                 << "  \"profileId\": \"" << JsonEscape(profile.id) << "\",\n"
                 << "  \"profileName\": \"" << JsonEscape(profile.name) << "\",\n"
                 << "  \"sourceKind\": \"" << SourceKindSlug(profile.source.kind) << "\",\n"
                 << "  \"directory\": \"" << JsonEscape(chosen) << "\"\n"
                 << "}\n";
        }
    }

    return PathToUtf8(dir);
}

// ---------------------------------------------------------------------------
// SegmentRecorder
// ---------------------------------------------------------------------------

SegmentRecorder::SegmentRecorder(std::string dir, VideoSpec video, uint32_t width,
                                 uint32_t height)
    : dir_(std::move(dir)), video_(video), width_(width), height_(height) {
    if (!dir_.empty()) {
        segmentsDir_ = PathToUtf8(fs::u8path(dir_) / "segments");
    }
    fps_ = video_.fps ? video_.fps : 60u;
    if (fps_ > 240) fps_ = 240; // 离谱值兜一下，别让 GOP 算爆
}

SegmentRecorder::~SegmentRecorder() { Stop(); }

std::string SegmentRecorder::ListFilePath() const { return listFile_; }
std::string SegmentRecorder::SegmentsDir() const { return segmentsDir_; }

bool SegmentRecorder::Running() const {
    std::lock_guard<std::mutex> lk(mu_);
    return stats_.active;
}

// 清掉上一轮录制留下的分片文件和清单。
//
// 为什么必须清：ffmpeg 的 `-f segment -segment_wrap N` 是**按槽位循环复写**的 ——
// 它把 000000.ts 从头**覆盖**写一遍，但**不会先把文件截断**。上一轮这个槽位里有
// 6.4 MB、这一轮只写了 2 MB，文件大小就还是 6.4 MB，磁盘统计凭空翻几倍。
// 实测：回放 120s / 分片 2s 的配置（60 个槽位）在界面上显示 356 MB，
// 而按窗口时长 × 码率算应该只有 120 MB 上下。
//
// segments.csv 也一起删：`-segment_list` 是追加写的，不删的话时间轴行数会
// 跨轮次累积，用它算"录了多久"就完全不对了。
//
// ---- 为什么只清"本轮的目录"而不是整个 segments/ ----
// 用户改帧率后希望前面的录像也一起导出（用插帧/抽帧补到新帧率），所以上一轮
// 的分片不能再删。每一轮都有自己的子目录（见 .h 里的目录结构），新一轮的
// ffmpeg 只往自己的空目录里写，天然不会撞到旧的槽位。
//
// 旧轮次由 ReclaimOldRuns() 按回放窗口回收（超窗的整目录删掉），所以磁盘占用
// 仍然被"回放时长"限住，不会无限涨。
void SegmentRecorder::ClearStaleSlots() {
    std::error_code ec;
    const fs::path runPath = fs::u8path(runDir_);
    if (fs::exists(runPath, ec)) {
        for (const auto& e : fs::directory_iterator(runPath, ec)) {
            std::error_code ec2;
            if (!e.is_regular_file(ec2)) continue;
            const std::string ext = e.path().extension().u8string();
            if (ext == u8".ts") fs::remove(e.path(), ec2);
        }
    }
    if (!listFile_.empty()) {
        std::error_code ec3;
        fs::remove(fs::u8path(listFile_), ec3);
    }
}

bool SegmentRecorder::Start() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (started_) return stats_.active;
        started_ = true;
    }

    if (dir_.empty() || width_ == 0 || height_ == 0) {
        std::lock_guard<std::mutex> lk(mu_);
        stats_.failed = true;
        stats_.detail = "录制参数不全（目录或分辨率是空的）";
        return false;
    }

    ffmpegPath_ = ResolveFfmpeg();
    if (ffmpegPath_.empty()) {
        std::lock_guard<std::mutex> lk(mu_);
        stats_.failed = true;
        stats_.detail = "找不到 ffmpeg.exe（可设 SHADOW_FFMPEG 指定路径）";
        return false;
    }

    std::error_code ec;
    fs::create_directories(fs::u8path(segmentsDir_), ec);
    if (ec) {
        std::lock_guard<std::mutex> lk(mu_);
        stats_.failed = true;
        stats_.detail = "建不了分片目录：" + ec.message();
        return false;
    }

    // ---- 本轮运行目录 ----------------------------------------------------
    //
    // 每一轮录制（每起一次 ffmpeg）单独一个子目录，名字就是**代次数字**：
    // `1`、`2`、`3`…（老目录叫 `0001-fps60`，那是历史，仍然能认出来）。
    //
    // 为什么要分代：新一轮的 `-segment_wrap` 只在**自己的**目录里绕圈，不会
    // 覆盖上一轮的分片 —— 这是"改帧率后前面的录像还能一起导出"的前提。
    //
    // 【为什么不把帧率写进目录名了】以前叫 `0001-fps60`，看着像"第 1 代、
    // 60 帧"，可它只解决了导出端少跑一次 ffmpeg 的便利，代价是名字一长串、
    // 同一台机器上换个帧率就多一个目录。真正决定输出帧率的是**配置里的
    // 目标帧率**，导出时本来就要重采样；源帧率改成导出前探一次即可
    // （见 Exporter.cpp 里 srcFps 的取法），所以名字里的 fps 其实没必要。
    // 现在帧率只记在 meta.json / index.csv 里，目录名保持最短。
    //
    // 代次取现有目录里最大的 +1（找不到就用 1）。
    {
        uint32_t nextSeq = 1;
        std::error_code ec2;
        if (fs::exists(fs::u8path(segmentsDir_), ec2)) {
            for (const auto& e : fs::directory_iterator(fs::u8path(segmentsDir_), ec2)) {
                std::error_code ec3;
                if (!e.is_directory(ec3)) continue;
                const std::string nm = e.path().filename().u8string();
                const size_t dash = nm.find('-');
                const std::string num = dash == std::string::npos ? nm : nm.substr(0, dash);
                if (num.empty()) continue;
                bool allDigit = true;
                for (char c : num) {
                    if (!std::isdigit(static_cast<unsigned char>(c))) {
                        allDigit = false;
                        break;
                    }
                }
                if (!allDigit) continue;
                const uint32_t v = static_cast<uint32_t>(std::strtoul(num.c_str(), nullptr, 10));
                if (v >= nextSeq) nextSeq = v + 1;
            }
        }
        char name[32];
        std::snprintf(name, sizeof(name), "%u", nextSeq);
        runDir_ = PathToUtf8(fs::u8path(segmentsDir_) / fs::u8path(name));
        listFile_ = PathToUtf8(fs::u8path(runDir_) / fs::u8path("index.csv"));
    }

    fs::create_directories(fs::u8path(runDir_), ec);
    if (ec) {
        std::lock_guard<std::mutex> lk(mu_);
        stats_.failed = true;
        stats_.detail = "建不了本轮分片目录：" + ec.message();
        return false;
    }

    // 把本轮的参数记一份，导出端不用再猜。
    // （帧率、尺寸都写在这里，目录名只留代次数字。）
    {
        std::ofstream man(PathToUtf8(fs::u8path(runDir_) / fs::u8path("run.json")),
                          std::ios::binary | std::ios::trunc);
        if (man) {
            man << "{\n"
                << "  \"generation\": " << std::strtoul(fs::path(runDir_).filename().u8string().c_str(), nullptr, 10) << ",\n"
                << "  \"fps\": " << fps_ << ",\n"
                << "  \"width\": " << width_ << ",\n"
                << "  \"height\": " << height_ << ",\n"
                << "  \"segmentSeconds\": " << video_.segmentSeconds << "\n"
                << "}\n";
        }
    }

    ClearStaleSlots();

    const uint32_t segmentSeconds = video_.segmentSeconds ? video_.segmentSeconds : 2u;
    // 保留几个分片 = ceil(回放时长 / 分片时长)，至少 2 个（1 个没法滚动）。
    const uint32_t replaySeconds = video_.replaySeconds ? video_.replaySeconds : 60u;
    uint32_t keep = (replaySeconds + segmentSeconds - 1) / segmentSeconds;
    if (keep < 2) keep = 2;

    // 回收超窗的旧轮次目录（本轮刚建的空目录不动）。
    //
    // 保留最近 N 轮，N = max(2, keep / 4)。keep 是环形槽位数（回放时长/单片时长），
    // 每轮最多也只有 keep 片，所以留 keep/4 轮已经能覆盖"改了几次帧率、窗口内
    // 的旧画面还在"。太老的整目录删掉，磁盘不会无限涨。
    {
        std::error_code ec4;
        std::vector<std::pair<int64_t, fs::path>> runs;
        for (const auto& e : fs::directory_iterator(fs::u8path(segmentsDir_), ec4)) {
            std::error_code ec5;
            if (!e.is_directory(ec5)) continue;
            if (e.path() == fs::u8path(runDir_)) continue;
            const auto t = e.last_write_time(ec5);
            runs.emplace_back(ec5 ? 0 : static_cast<int64_t>(t.time_since_epoch().count()),
                              e.path());
        }
        std::sort(runs.begin(), runs.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        const size_t keepRuns = std::max<size_t>(2, static_cast<size_t>(keep) / 4);
        for (size_t i = keepRuns; i < runs.size(); ++i) {
            std::error_code ec8;
            fs::remove_all(runs[i].second, ec8);
        }
    }

    const std::vector<std::string> args =
        BuildArgs(ffmpegPath_, runDir_, listFile_, video_, width_, height_, fps_, keep,
                  segmentSeconds);

    // 拼命令行（argv[0] 按约定要重复一遍 exe 路径）
    std::wstring cmd;
    for (const std::string& a : args) {
        if (!cmd.empty()) cmd.push_back(L' ');
        cmd += QuoteArg(FromUtf8(a));
    }

    // ---- 管道：只留写端 ----
    //
    // 缓冲区给 8MB，而不是默认的 4KB。
    //
    // 这条管道里流的是**原始像素**：2560x1440 BGRA 一帧就是 14.7MB，240fps
    // 下每秒 3.5GB。管道缓冲只有 4KB 时，每写一帧都要反复"填满缓冲 → 阻塞
    // 等 ffmpeg 读走 → 再填"，一次采集就要多花一两毫秒，实测每拍 6.0ms 而
    // 目标周期只有 4.17ms，于是只能跑到 150~170fps。
    //
    // 给到 8MB 之后一次 WriteFile 就能把半帧交出去，ffmpeg 在对面并行地读，
    // 写端几乎不再等 —— 这是纯收益，代价只是 8MB 内核内存。
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr;
    HANDLE wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 8u << 20)) {
        std::lock_guard<std::mutex> lk(mu_);
        stats_.failed = true;
        stats_.detail = "建管道失败";
        return false;
    }
    SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0); // 写端我们自己留着，不继承
    stdinWrite_ = wr;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;      // 别闪黑框
    si.hStdInput = rd;
    si.hStdOutput = INVALID_HANDLE_VALUE;
    si.hStdError = INVALID_HANDLE_VALUE;

    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = cmd;
    const BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(rd); // 子进程已经拿到读端，我们这份要关掉，否则读到不了 EOF

    if (!ok) {
        CloseHandle(wr);
        stdinWrite_ = nullptr;
        std::lock_guard<std::mutex> lk(mu_);
        stats_.failed = true;
        stats_.detail = "起不了 ffmpeg（" + std::to_string(GetLastError()) + "）：" +
                        ToUtf8(FromUtf8(ffmpegPath_));
        return false;
    }

    process_ = pi.hProcess;
    thread_ = pi.hThread;

    // 挂到作业对象上：服务被强杀时 ffmpeg 也会跟着走，不会变成孤儿
    // 继续占着 test/ 里的分片（挂不上也不影响录制，忽略返回值）。
    if (void* job = ShadowChildJob()) {
        AssignProcessToJobObject(static_cast<HANDLE>(job), pi.hProcess);
    }

    std::lock_guard<std::mutex> lk(mu_);
    stats_.active = true;
    stats_.failed = false;
    stats_.detail = "录制中";
    stats_.segmentSeconds = segmentSeconds;
    stats_.keepSegments = keep;
    StartFeeder();
    return true;
}

/** 起喂帧线程。必须在 Start() 成功之后调（管道和进程都就绪了）。 */
void SegmentRecorder::StartFeeder() {
    {
        std::lock_guard<std::mutex> lk(slotMu_);
        feedStop_ = false;
        slotFull_ = false;
        slotSeq_ = 0;
        slotTakenSeq_ = 0;
    }
    feeder_ = std::thread([this] { FeederLoop(); });
}

bool SegmentRecorder::Feed(const void* pixels, uint32_t stride, uint32_t width, uint32_t height) {
    if (!pixels || width != width_ || height != height_) {
        std::lock_guard<std::mutex> lk(mu_);
        ++stats_.droppedFrames;
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!stats_.active || !stdinWrite_) return false;
    }

    const uint8_t* p = static_cast<const uint8_t*>(pixels);
    const size_t rowBytes = static_cast<size_t>(width_) * 4u;
    const size_t need = rowBytes * height_;

    // 拷进槽里就得返回。这一步是纯内存拷贝（一次 4.78MB 的那种），
    // 不会阻塞在 ffmpeg 的管道上 —— 那才是原来把采集节拍拖垮的东西。
    {
        std::lock_guard<std::mutex> lk(slotMu_);
        slot_.resize(need);
        // 行距和紧凑行距不一致（DXGI 的 staging 纹理会对齐到 256 字节）：
        // 一行一行拷，别把行尾的填充字节也当成像素（那样画面会斜）。
        if (stride == rowBytes) {
            std::memcpy(slot_.data(), p, need);
        } else {
            for (uint32_t y = 0; y < height_; ++y) {
                std::memcpy(slot_.data() + static_cast<size_t>(y) * rowBytes,
                            p + static_cast<size_t>(y) * stride, rowBytes);
            }
        }
        slotFull_ = true;
        ++slotSeq_;
    }
    slotCv_.notify_one();
    return true;
}

void SegmentRecorder::FeederLoop() {
    const double fps = fps_ > 0 ? static_cast<double>(fps_) : 60.0;

    // 本地复用的帧缓冲：取出槽里的帧之后立刻放开槽锁，写管道时不再占着它。
    std::vector<uint8_t> frame;
    uint64_t lastSeq = 0;

    // ---- 时间轴为什么按"帧数"而不是按"节拍"维护 --------------------------
    //
    // 喂给 ffmpeg 的是一个裸 BGRA 流 + `-framerate <fps>`。ffmpeg 不看墙钟，
    // 它只看收到了多少帧：第 N 帧的 PTS 就是 N / fps。所以**帧数必须严格等于
    // fps × 真实时长**，否则 PTS 被压缩/拉伸，回放就加速或变慢。
    //
    // 原来的写法是"落后超过一个 interval 就把 nextDue 重新对齐到 now"，那一段
    // 该发的帧被**静默丢掉**了。240fps 每拍预算只有 4.17ms，GPU 一忙、管道
    // 一堵就成片落后，于是 240fps 录出来的回放明显加速，而 60fps 因为留有
    // 三倍余量不容易触发 —— 正是"只测出 240 有问题"的原因。
    //
    // 现在改成：每拍结束前，按墙钟算出"到现在总共应该写多少帧"，补齐差额
    // （补的内容就是上一帧，等价于重复插帧）。时间轴由帧数保证，与采集掉帧、
    // 管道堵塞完全解耦 —— 采不到新画面只会让画面变"卡"，绝不会让回放变速。
    uint64_t framesWritten = 0;
    auto t0 = std::chrono::steady_clock::now();

    auto writeOnce = [&](const std::vector<uint8_t>& buf) -> bool {
        if (buf.empty()) return true; // 还没采到第一帧，没法补
        if (!WriteFrameToPipe(buf)) return false;
        ++framesWritten;
        return true;
    };

    bool alive = true;
    while (alive) {
        // 还没采到第一帧：帧数是 0 时没有任何东西可写，等第一帧到了再开始
        // 计时。否则 t0 会在"空转"期间把欠账攒成天文数字。
        if (frame.empty()) {
            std::unique_lock<std::mutex> lk(slotMu_);
            if (feedStop_) break;
            slotCv_.wait(lk, [this] { return feedStop_ || slotFull_; });
            if (feedStop_) break;
            if (slotFull_ && slotSeq_ != lastSeq) {
                frame.swap(slot_);
                lastSeq = slotSeq_;
                slotFull_ = false;
            }
            t0 = std::chrono::steady_clock::now();
            framesWritten = 0;
            continue;
        }

        // ---- 睡到下一个节拍 ----------------------------------------------
        const auto nextDue =
            t0 + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                     std::chrono::duration<double>(static_cast<double>(framesWritten) / fps));
        if (std::chrono::steady_clock::now() < nextDue) {
            // 条件变量带超时：Stop() 置 feedStop_ 时能立刻醒。
            std::unique_lock<std::mutex> lk(slotMu_);
            if (feedStop_) break;
            slotCv_.wait_until(lk, nextDue, [this] { return feedStop_ || slotFull_; });
            if (feedStop_) break;
        }

        // ---- 取槽里的最新帧 ----------------------------------------------
        {
            std::lock_guard<std::mutex> lk(slotMu_);
            if (feedStop_) break;
            if (slotFull_ && slotSeq_ != lastSeq) {
                frame.swap(slot_); // 交换而不是拷贝：连一次 memcpy 都省掉
                lastSeq = slotSeq_;
                slotFull_ = false;
            }
        }

        // ---- 补到"现在该有的帧数" ------------------------------------------
        //
        // 这一步同时覆盖两种情况：
        //   * 正常节奏：due 比已写的多 1，就是正常写一帧；
        //   * 落后了：due 比已写的多 k 张，连续写 k 张（内容是同一帧），
        //     把欠账一次补齐，时间轴立刻回到真实时间。
        const double elapsedSec =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const uint64_t due = static_cast<uint64_t>(elapsedSec * fps) + 1;

        // ---- 为什么这里**绝对不能**给补帧设上限 ----------------------------
        //
        // 原来这里是 `if (due > framesWritten + 240) { 重起基准 }`：机器被彻底
        // 抢占、或者 ffmpeg 管道堵住时，把这一段该有的时长直接**丢掉**，只写
        // 一帧。后果就是主人看到的那种情况 —— 画面卡但时间轴被压短，回放成
        // 了快进。帧率越高、机器越忙，这种缺口越多，240fps 尤其明显。
        //
        // 现在的取舍（用户明确要求）：**允许卡，不允许变速**。落后多少就补多少
        // 重复帧，重复帧走同一条 libx264 管道、编码代价极低（同一张画面连编，
        // x264 会压成极小的跳帧），内存占用不随缺口大小增长 —— 补帧用的始
        // 终是本地那一帧缓冲，不会额外分配。
        //
        // 唯一的例外是"从没采到过任何帧"（frame 为空），那种情况下面补不了，
        // 只能等，等到了重新计时。
        const uint64_t need = due > framesWritten ? (due - framesWritten) : 0;
        for (uint64_t i = 0; i < need; ++i) {
            if (!writeOnce(frame)) {
                alive = false;
                break;
            }
        }
    }

    if (!alive) {
        std::lock_guard<std::mutex> lk(mu_);
        stats_.active = false;
        stats_.failed = true;
        if (stats_.detail == "录制中" || stats_.detail.empty()) {
            stats_.detail = "ffmpeg 提前退出（分片写入中断）";
        }
        stdinWrite_ = nullptr;
    }
}

bool SegmentRecorder::WriteFrameToPipe(const std::vector<uint8_t>& frame) {
    HANDLE h;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!stdinWrite_) return false;
        h = static_cast<HANDLE>(stdinWrite_);
    }
    const size_t rowBytes = static_cast<size_t>(width_) * 4u;
    if (frame.size() < rowBytes * height_) return true; // 不完整，跳过这一拍
    // 槽里已经是紧凑排布（stride = rowBytes），一条 WriteAll 写完。
    if (!WriteAll(h, frame.data(), rowBytes * height_)) return false;

    std::lock_guard<std::mutex> lk(mu_);
    ++stats_.framesIn;
    stats_.bytesIn += static_cast<uint64_t>(rowBytes) * height_;
    return true;
}

void SegmentRecorder::Stop() {
    // 先收喂帧线程：它可能正卡在 WriteFile 上等 ffmpeg 消费。
    // 必须等它退出来再关写端，否则它可能往已经关掉的句柄里写。
    if (feeder_.joinable()) {
        {
            std::lock_guard<std::mutex> lk(slotMu_);
            feedStop_ = true;
        }
        slotCv_.notify_all();
        feeder_.join();
    }

    HANDLE proc = nullptr;
    HANDLE thr = nullptr;
    HANDLE wr = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (stopped_) return;
        stopped_ = true;
        proc = static_cast<HANDLE>(process_);
        thr = static_cast<HANDLE>(thread_);
        wr = static_cast<HANDLE>(stdinWrite_);
        stdinWrite_ = nullptr;
        process_ = nullptr;
        thread_ = nullptr;
        stats_.active = false;
        if (stats_.detail == "录制中") stats_.detail = "已停止";
    }

    // 关写端 -> ffmpeg 读到 EOF -> 把最后一个分片 flush 完整后自己退出。
    if (wr) CloseHandle(static_cast<HANDLE>(wr));

    if (proc) {
        const DWORD w = WaitForSingleObject(static_cast<HANDLE>(proc), 8000);
        if (w == WAIT_TIMEOUT) {
            TerminateProcess(static_cast<HANDLE>(proc), 1);
            WaitForSingleObject(static_cast<HANDLE>(proc), 2000);
            std::lock_guard<std::mutex> lk(mu_);
            stats_.failed = true;
            stats_.detail = "ffmpeg 收尾超时，已强制结束";
        }
        CloseHandle(static_cast<HANDLE>(proc));
    }
    if (thr) CloseHandle(static_cast<HANDLE>(thr));
}

void SegmentRecorder::RefreshDiskStatsLocked() const {
    std::error_code ec;
    uint32_t count = 0;
    uint64_t bytes = 0;
    // 分片现在分布在各轮次的子目录里，磁盘统计要连子目录一起扫 ——
    // 只扫一层的话，界面上永远显示"0 个分片 / 0 MB"。
    if (!segmentsDir_.empty()) {
        const fs::path root = fs::u8path(segmentsDir_);
        std::error_code ecDir;
        if (fs::exists(root, ecDir)) {
            for (const auto& e : fs::recursive_directory_iterator(root, ecDir)) {
                if (ecDir) break;
                std::error_code ecFile;
                if (!e.is_regular_file(ecFile) || ecFile) continue;
                if (e.path().extension() != ".ts") continue;
                ++count;
                bytes += static_cast<uint64_t>(e.file_size(ecFile));
            }
        }
    }
    stats_.segmentsOnDisk = count;
    stats_.diskBytes = bytes;
}

RecorderStats SegmentRecorder::Stats() const {
    std::lock_guard<std::mutex> lk(mu_);
    RefreshDiskStatsLocked();
    return stats_;
}

} // namespace shadow
