// Exporter.cpp —— 见 Exporter.h 的说明。
#include "Exporter.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

#include "FfmpegUtil.h"
#include "EnvPath.h" // ExpandEnv：导出目录里的 %TEMP% 这类占位符

namespace shadow {
namespace fs = std::filesystem;

namespace {

// 跟 SegmentRecorder.cpp 一样：子进程挂进作业对象，服务退出时不会留孤儿。
extern "C" void* ShadowChildJob();

/** 分片小于这个字节数就当成"还没写完"的尾巴，不参与导出。 */
constexpr uint64_t kMinSegmentBytes = 4096;

struct SegmentFile {
    fs::path    path;
    std::string name;
    uint64_t    bytes = 0;
    int64_t     writeTime = 0; // 100ns 单位（fs::file_time_type 的原生刻度）
};

/** 读一个分片的实际时长（秒），失败返回 -1。定义在下面（依赖 RunFfmpeg）。 */
double ProbeSegmentSeconds(const fs::path& file);

/** 从分片名里读录制时声明的帧率：`0002-fps240__000005.ts` → 240。
 *  平铺的老数据（`000005.ts`）没有标签，返回 0，调用方会退回问 ffmpeg。 */
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

/** 按修改时间排好序的分片清单（只含真正写完了的）。
 *
 *  nominalSegmentSeconds：单片标称时长（秒），用来判"最后一片够不够长"。
 *  传 0 或负数时按 2 秒处理。 */
bool ListSegments(const fs::path& dir, std::vector<SegmentFile>& out, std::string& err,
                  double nominalSegmentSeconds = 2.0) {
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
    // （比如 `0002-fps240__000005.ts`）—— 各轮次的分片都叫 000000.ts，不拼成
    // 唯一名字的话，后面复制到临时目录、写 concat 清单时会互相覆盖。
    auto take = [&](const fs::directory_entry& e, const std::string& namePrefix) {
        std::error_code e2;
        if (!e.is_regular_file(e2) || e2) return;
        if (e.path().extension() != ".ts") return;
        SegmentFile f;
        f.path = e.path();
        const std::string base = ToUtf8(e.path().filename().wstring());
        f.name = namePrefix.empty() ? base : (namePrefix + "__" + base);
        f.bytes = static_cast<uint64_t>(e.file_size(e2));
        if (e2 || f.bytes < kMinSegmentBytes) return; // 半截尾巴
        const auto t = e.last_write_time(e2);
        f.writeTime = e2 ? 0 : static_cast<int64_t>(t.time_since_epoch().count());
        out.push_back(std::move(f));
    };
    for (const auto& e : it) {
        std::error_code e2;
        if (e.is_directory(e2) && !e2) {
            // ---- 轮次子目录 --------------------------------------------------
            //
            // 录制端每起一次 ffmpeg 就新建 `segments/<序号>-fps<N>/`（见
            // SegmentRecorder.cpp 的 Start）。改帧率后旧轮次不删，所以这里要
            // 往里扫 —— 用户的诉求是"前面录的也要一起导出，按新帧率补帧"。
            // mtime 排序天然把各轮按时间串起来（旧轮的片子年代更早）。
            const std::string prefix = ToUtf8(e.path().filename().wstring());
            std::error_code e3;
            fs::directory_iterator sub(e.path(), e3);
            if (e3) continue;
            for (const auto& se : sub) take(se, prefix);
            continue;
        }
        take(e, std::string());
    }
    std::sort(out.begin(), out.end(), [](const SegmentFile& a, const SegmentFile& b) {
        if (a.writeTime != b.writeTime) return a.writeTime < b.writeTime;
        return a.name < b.name; // 同一刻写入时按名字兜底，保证顺序稳定
    });

    // ---- 丢掉"正在写"的那一片 --------------------------------------------
    //
    // ffmpeg 的 segment muxer 是**边写边切**：此刻磁盘上总有一片是半片
    // （实测 240fps 下只有 257 帧 / 1.07 秒，而完整片是 480 帧 / 2.00 秒）。
    // 拿它去 concat，产物尾部会有半秒钟的"卡住的画面"，所以得剔掉。
    //
    // ---- 历史：这地方错过两次，都是"猜"出来的判据 ------------------------
    //
    // 1) 按 4KB 阈值：半片有六七 MB，完全拦不住。
    // 2) 按体积中位数的一半：分片目录被重起过（改帧率/改分辨率会重建 ffmpeg）
    //    时，环形覆盖的序号绕回来，目录里混着"上一轮的完整片"和"这一轮刚开
    //    始写的片"，体积不在同一量级 —— 实测把 3.3MB 的完整片当半片剔了。
    // 3) 按"mtime 年龄小于 400ms 就丢掉"：**这一版就是用户看到"少两秒"的
    //    元凶**。一片正好 2.000 秒，ffmpeg 刚切完上一片、刚要开始写下一片
    //    的那 400ms 里，最新那一片其实**已经完整**（480 帧、2.000 秒），却因为
    //    mtime 太新被整片丢掉 —— 于是 20 秒的缓冲导出来只有 18 秒，稳定复现。
    //    mtime 只能说明"最近被碰过"，说明不了"写完了没"，用它是错的。
    //
    // ---- 现在：直接问这一片自己 --------------------------------
    //
    // 真正要判的是"这一片里有几帧"，只有 ffmpeg 知道。所以对**最新那一片**
    // 跑一次 `ffmpeg -i <片>`，从 stderr 的 `Duration: 00:00:01.98` 读出它的
    // 实际时长，再跟"单片标称时长 × 90%"比：
    //   * 够长 → 它已经写完了，**留着**（这正是修复"少两秒"的关键）；
    //   * 不够 → 确确实实没写完，丢掉。
    //
    // 只测最新一片，代价是导出时多跑一次 ffmpeg（~100ms），相对整段 concat
    // 或重编码可以忽略；换来的是"不多不少"。
    //
    // nominalSegmentSeconds 由调用方从 profile 里传进来（默认 2 秒）。
    if (!out.empty() && out.size() > 1) {
        const SegmentFile& lastOne = out.back();
        double tailSeconds = -1.0;
        // 只有"看起来可能不完整"的才值得去问一次：它要么很新，要么明显偏小。
        // 都已经隔了 1 秒以上没人碰、体积也不小的，不再白跑一次 ffmpeg。
        const auto now = std::filesystem::file_time_type::clock::now();
        const auto last = std::filesystem::file_time_type(
            std::filesystem::file_time_type::duration(lastOne.writeTime));
        const auto ageMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count();
        const bool maybeInProgress = (ageMs >= 0 && ageMs < 1500);

        if (maybeInProgress) {
            tailSeconds = ProbeSegmentSeconds(lastOne.path);
        }
        // 读到时长且不够长 → 真的没写完，丢掉。
        // 读不到（-1）时保守处理：按"没写完"丢掉，免得半片头部出错让整次导出失败。
        const double needSeconds =
            (nominalSegmentSeconds > 0.0 ? nominalSegmentSeconds : 2.0) * 0.9;
        if (tailSeconds >= 0.0) {
            if (tailSeconds < needSeconds) out.pop_back();
        } else if (ageMs >= 0 && ageMs < 1500) {
            out.pop_back();
        }
    }
    return true;
}

/** 复制一个文件并校验字节数。失败返回 false（多半就是它正好被环形覆盖了）。 */
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
        err = "分片在复制途中被改写（读到 " + std::to_string(got) + " 字节，应为 " +
              std::to_string(expectBytes) + "）";
        return false;
    }
    return true;
}

/** 跑一个 ffmpeg 子进程并等它结束，把 stderr 收进 outLog。
 *  用匿名管道收 stderr —— 不重定向的话出错信息会跑到服务自己的控制台上，
 *  而服务是被界面当子进程拉起来的，用户根本看不到。 */
bool RunFfmpeg(const std::string& commandLine, std::string& outLog, DWORD& outExitCode) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE errRd = nullptr;
    HANDLE errWr = nullptr;
    if (!CreatePipe(&errRd, &errWr, &sa, 0)) {
        outLog = "建 stderr 管道失败";
        return false;
    }
    SetHandleInformation(errRd, HANDLE_FLAG_INHERIT, 0); // 读端我们自己留着

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = INVALID_HANDLE_VALUE;
    si.hStdOutput = INVALID_HANDLE_VALUE;
    si.hStdError = errWr;

    PROCESS_INFORMATION pi{};
    std::wstring cmd = FromUtf8(commandLine);
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                   nullptr, nullptr, &si, &pi);
    CloseHandle(errWr); // 子进程已经拿到写端

    if (!ok) {
        CloseHandle(errRd);
        outLog = "起不了 ffmpeg（" + std::to_string(GetLastError()) + "）";
        return false;
    }

    if (void* job = ShadowChildJob()) {
        AssignProcessToJobObject(static_cast<HANDLE>(job), pi.hProcess);
    }

    // 边等边读，否则 ffmpeg 输出多了会把管道写满、它自己卡死在那。
    outLog.clear();
    char buf[4096];
    for (;;) {
        DWORD avail = 0;
        if (PeekNamedPipe(errRd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            DWORD got = 0;
            if (ReadFile(errRd, buf, sizeof(buf) - 1, &got, nullptr) && got > 0) {
                outLog.append(buf, got);
                if (outLog.size() > 8192) outLog.erase(0, outLog.size() - 8192); // 只留尾巴
            }
            continue;
        }
        if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) break;
    }
    // 收尾：进程退出后再把管道里剩下的读完
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(errRd, buf, sizeof(buf) - 1, &got, nullptr) || got == 0) break;
        outLog.append(buf, got);
        if (outLog.size() > 8192) outLog.erase(0, outLog.size() - 8192);
    }
    CloseHandle(errRd);

    outExitCode = 1;
    GetExitCodeProcess(pi.hProcess, &outExitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

/** 读一个分片的实际时长（秒）。失败返回 -1。
 *
 *  【为什么需要它】导出前要判断"最新那一片写完了没"。以前靠 mtime 猜，结果是
 *  把刚切完、其实完整的 2 秒整片也丢掉 —— 用户实测 20 秒缓冲导出来只有 18 秒。
 *  分片里到底有多少内容，只有读它自己知道，所以直接跑一次 ffmpeg 问。
 *
 *  ffmpeg 不带 -o 时不会产出文件，只是把输入信息打到 stderr，形如
 *      Duration: 00:00:01.98, start: 1.400000, bitrate: ...
 *  这里就从这个字段里取秒数。读不到（半片头部损坏、还没写 moov 之类）就返回 -1，
 *  由调用方按"没写完"处理。 */
double ProbeSegmentSeconds(const fs::path& file) {
    const std::string ffmpeg = ResolveFfmpeg();
    if (ffmpeg.empty()) return -1.0;

    std::vector<std::string> args;
    args.push_back("-hide_banner");
    args.push_back("-nostdin");
    args.push_back("-i");
    args.push_back(PathToUtf8(file));
    const std::wstring cmd = BuildCommandLine(ffmpeg, args);
    std::string log;
    DWORD code = 1;
    if (!RunFfmpeg(ToUtf8(cmd), log, code)) return -1.0;

    const size_t at = log.find("Duration:");
    if (at == std::string::npos) return -1.0;
    // "Duration: 00:00:01.98," → 只取 HH:MM:SS.mmm 这一段
    size_t p = at + 9;
    while (p < log.size() && (log[p] == ' ' || log[p] == '\t')) ++p;
    int h = 0;
    int m = 0;
    double s = 0.0;
    if (std::sscanf(log.c_str() + p, "%d:%d:%lf", &h, &m, &s) != 3) return -1.0;
    return h * 3600.0 + m * 60.0 + s;
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

/** concat 清单：ffmpeg 的 concat demuxer 语法。文件名用 POSIX 分隔符，
 *  单引号按它自己的规则转义（' -> '\''）。 */
std::string EscapeConcatPath(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '\\') {
            out.push_back('/');
        } else if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    return out;
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
    // 临时目录整个删掉（里面有分片副本和 concat 清单）
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
    std::string listFile;

    auto fail = [&](const std::string& why) {
        // 注意：Finish() 自己会加锁，所以这里**不能**先拿着 mu_ 再调它，
        // 否则 std::mutex 不可重入 —— 会直接死锁在导出线程里。
        {
            std::lock_guard<std::mutex> lk(mu_);
            status_.elapsedMs = elapsedMs();
        }
        Finish(false, why, {}, tmpDir, listFile);
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
    const std::string ffmpeg = ResolveFfmpeg();
    if (ffmpeg.empty()) {
        fail("找不到 ffmpeg.exe（可以设 SHADOW_FFMPEG 环境变量指过去）。");
        return;
    }

    std::error_code ec;
    fs::create_directories(fs::u8path(exportDir_), ec);
    if (ec) {
        fail("建不了导出目录：" + ec.message());
        return;
    }

    // ---- 1. 快照分片 ------------------------------------------------------
    SetPhase("快照分片");
    std::vector<SegmentFile> segs;
    std::string err;
    if (!ListSegments(fs::u8path(segmentsDir_), segs, err,
                      static_cast<double>(segmentSeconds_))) {
        fail(err);
        return;
    }
    if (segs.empty()) {
        fail("分片目录里还没有可导出的完整分片。");
        return;
    }

    // 按回放时长截断：只导最近 maxSeconds 秒。
    if (maxSeconds_ > 0) {
        // 分片时长从文件名推不出来，用配置里的标称值估。
        // 对用户来说"导出最近 N 秒"误差一个分片是可接受的（分片默认 2 秒）。
        // 更准的做法要读 TS 里的 PTS，代价不值得。
        //
        // 注意这里要**向上取整再留一片余量**：以前只按 ceil(maxSeconds/2) 留，
        // 而最后一片可能是半分片（正在写的那片被剔掉后，实际可用时长会少一截），
        // 结果"导最近 20 秒"只给了 18 秒。
        const uint32_t perSegment = segmentSeconds_;
        const size_t keep = static_cast<size_t>((maxSeconds_ + perSegment - 1) / perSegment) + 1;
        if (keep > 0 && segs.size() > keep) {
            segs.erase(segs.begin(), segs.end() - static_cast<ptrdiff_t>(keep));
        }
    }

    tmpDir = PathToUtf8(fs::u8path(exportDir_) / fs::u8path(".shadow-export-tmp"));
    fs::remove_all(fs::u8path(tmpDir), ec);
    fs::create_directories(fs::u8path(tmpDir), ec);
    if (ec) {
        fail("建不了临时目录：" + ec.message());
        return;
    }

    // 复制（校验字节数）—— 失败就重试一次，因为多半是"这一片正好被环形覆盖"，
    // 隔一下再读一般就稳定了。
    for (int attempt = 0; attempt < 2; ++attempt) {
        bool allOk = true;
        for (const SegmentFile& f : segs) {
            const fs::path dst = fs::u8path(tmpDir) / fs::u8path(f.name);
            std::string cerr2;
            if (!CopyChecked(f.path, dst, f.bytes, cerr2)) {
                allOk = false;
                err = cerr2;
                break;
            }
        }
        if (allOk) break;
        if (attempt == 0) {
            Sleep(120);
            fs::remove_all(fs::u8path(tmpDir), ec);
            fs::create_directories(fs::u8path(tmpDir), ec);
        } else {
            fail(err + "（重试后仍然失败，可能录制正在快速滚动，稍后再试）");
            return;
        }
    }

    // 复制完再列一次，确认顺序没变（录制还在跑，最旧的那些可能又被换了）。
    {
        std::vector<SegmentFile> again;
        std::string err2;
        if (ListSegments(fs::u8path(segmentsDir_), again, err2,
                         static_cast<double>(segmentSeconds_)) &&
            again.size() != segs.size()) {
            // 变了也没关系：我们手里的副本是自洽的一整圈，只是可能多了/少了
            // 最旧的一片。继续导出，但在 detail 里说一句。
        }
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        status_.segmentCount = static_cast<uint32_t>(segs.size());
    }

    // ---- 2. concat 清单 ---------------------------------------------------
    listFile = PathToUtf8(fs::u8path(tmpDir) / fs::u8path("list.txt"));
    {
        std::ofstream out(listFile, std::ios::binary | std::ios::trunc);
        if (!out) {
            fail("写不了 concat 清单。");
            return;
        }
        for (const SegmentFile& f : segs) {
            out << "file '" << EscapeConcatPath(f.name) << "'\n";
        }
    }

    // ---- 3. 合并 ----------------------------------------------------------
    SetPhase("合并");
    // 文件名：<配置名>_Replay_<时间戳>.mp4，重名自动加 -2、-3。
    std::string baseName = ReplayFileName(prefix_, {});
    baseName = baseName.substr(0, baseName.size() - 4); // 去掉 ".mp4" 交给扩展名参数
    const fs::path finalPath = UniqueTarget(fs::u8path(exportDir_), baseName, ".mp4");

    std::vector<std::string> args;
    args.push_back("-hide_banner");
    args.push_back("-loglevel");
    args.push_back("error");
    args.push_back("-nostdin");
    args.push_back("-f");
    args.push_back("concat");
    args.push_back("-safe");
    args.push_back("0");
    args.push_back("-i");
    args.push_back(listFile);

    // ---- 帧率重采样 ------------------------------------------------------
    //
    // 分片里存的是**录制当时**的帧率。用户可能刚刚把 60 改成 240，此时缓冲里
    // 大半还是 60fps 的老分片。要让"改完立刻生效"，就在这里按目标帧率重建
    // 时间轴：`fps=240` 会把每帧按 1/240 秒的节奏重排，源只有 60 帧/秒时
    // ffmpeg 自动重复上一帧补齐 —— 就是需求里的"重复插帧补到 240"。
    //
    // 同时这也是"导出加速"的第二道保险：录制端已经把帧数补足，但万一某一片
    // 因为机器卡顿仍然缺帧，这里按恒定帧率重排会把时长拉回真实值。
    //
    // 代价：重编码比 -c copy 慢得多。所以只有当"必须重排时间轴"时才走这条路：
    //   * 目标帧率和源不一样（要在老画面上插帧/抽帧）；
    //   * 或者清单里的分片来自**多个不同帧率的轮次** —— 那就更不能 -c copy 了，
    //     concat 直拼会把两种帧率的 PTS 混在一起，播放器时长和帧率全乱。
    //
    // 多轮次的帧率直接从目录名读（录制端把 fps 写进了 `0002-fps240`），
    // 不用为每一片跑一次 ffmpeg：只要最旧那片和最旧那轮之外有任何一片的
    // 标签跟最后一片不一样，就是混了帧率。
    uint32_t srcFps = 0;
    const uint32_t lastTagFps = TagFpsOf(segs.back().name);
    bool mixedFps = false;
    for (const SegmentFile& f : segs) {
        const uint32_t t = TagFpsOf(f.name);
        if (t != 0 && lastTagFps != 0 && t != lastTagFps) {
            mixedFps = true;
            break;
        }
    }
    {
        // 源帧率：优先信目录标签（最准），没有才去问 ffmpeg。
        if (lastTagFps != 0) {
            srcFps = lastTagFps;
        } else {
            // 从 ffmpeg 的 stderr 里读第一片的帧率（比解析 TS 简单可靠）。
            std::vector<std::string> pargs;
            pargs.push_back("-hide_banner");
            pargs.push_back("-nostdin");
            pargs.push_back("-f");
            pargs.push_back("concat");
            pargs.push_back("-safe");
            pargs.push_back("0");
            pargs.push_back("-i");
            pargs.push_back(listFile);
            const std::wstring pcmd = BuildCommandLine(ffmpeg, pargs);
            std::string plog;
            DWORD pcode = 1;
            RunFfmpeg(ToUtf8(pcmd), plog, pcode);
            const size_t at = plog.find("Video:");
            if (at != std::string::npos) {
                // 形如 "Video: h264 ..., yuv420p, 2560x1440, 240 fps, 240 tbr, 90k tbn"
                const size_t fpsPos = plog.find(" fps", at);
                if (fpsPos != std::string::npos && fpsPos > at) {
                    // 往前回退到数字开头
                    size_t s = fpsPos;
                    while (s > at && (std::isdigit(static_cast<unsigned char>(plog[s - 1])) ||
                                      plog[s - 1] == '.')) {
                        --s;
                    }
                    const double v = std::atof(plog.c_str() + s);
                    if (v > 0.5) srcFps = static_cast<uint32_t>(v + 0.5);
                }
            }
        }
    }

    // 目标帧率：调用方没指定时，就用**最后那一轮**的帧率（用户刚改成的值）。
    const uint32_t outFps = targetFps_ > 0 ? targetFps_ : (srcFps > 0 ? srcFps : 0);
    const bool needResample = outFps > 0 && (mixedFps || srcFps != outFps);

    if (needResample) {
        // 重编码重采样。质量参数给得高一些：这是用户要拿去用的成品，
        // 而且重采样天然比拷贝损失大，用 crf 18 + veryfast 在速度和观感间取平衡。
        args.push_back("-vf");
        args.push_back("fps=" + std::to_string(outFps));
        args.push_back("-r");
        args.push_back(std::to_string(outFps));
        args.push_back("-c:v");
        args.push_back("libx264");
        args.push_back("-preset");
        args.push_back("veryfast");
        args.push_back("-crf");
        args.push_back("18");
        args.push_back("-pix_fmt");
        args.push_back("yuv420p");
        // ---- 必须在编码参数之后、输出文件之前再写一遍 -r -------------------
        //
        // 上面那个 `-r N` 挨着 `-vf fps=N` 会被 ffmpeg 当成**输入**选项
        // （输出文件还没出现，它归属到 `-i` 那一路），效果是"读输入时按 N
        // 重排"，而输出流的帧率仍由 libx264 从实际帧间隔推断 —— 实测 60fps
        // 源要插到 120fps 时，成品帧数对了（720 帧）但容器头写的是 60fps，
        // 播放器就按 60fps 播，成了一段 12 秒的慢动作。
        //
        // 输出选项位置的那一个才是"成品标称帧率"，两者都要。
        args.push_back("-r");
        args.push_back(std::to_string(outFps));
        // 保留音频（目前没有音轨，但以后加了也不会被这里意外丢掉）。
        args.push_back("-c:a");
        args.push_back("copy");
    } else {
        args.push_back("-c");
        args.push_back("copy");
    }
    // 快启动：把 moov 挪到文件头，分享出去能立刻起播。
    args.push_back("-movflags");
    args.push_back("+faststart");
    args.push_back("-y");
    args.push_back(PathToUtf8(finalPath));

    const std::wstring cmd = BuildCommandLine(ffmpeg, args);

    std::string log;
    DWORD exitCode = 1;
    if (!RunFfmpeg(ToUtf8(cmd), log, exitCode)) {
        fail(log);
        return;
    }
    if (exitCode != 0) {
        std::string why = "ffmpeg 合并失败（退出码 " + std::to_string(exitCode) + "）";
        if (!log.empty()) why += "：" + log;
        fail(why);
        return;
    }

    // ---- 4. 结果 ----------------------------------------------------------
    //
    // 查文件大小时**必须**重新用 u8path 走一遍：直接 fs::file_size(path) 时
    // 标准库会把路径按本地 ANSI 代码页（GBK）转窄字符串，而路径里带的是
    // UTF-8 的配置名（"整屏-主显示器"）。GBK 编不出这些字节 → ENOENT，
    // 于是明明写出了一个十几 MB 的 MP4，却被判成"产物是空的"。
    // （同理，.wstring() 也不行：UTF-8 读进来的字节被当成 GBK 解释，乱码后同样找不到。）
    const fs::path finalProbe = finalPath; // PathToUtf8/native 已经保证这是 UTF-16 正确的路径
    const uint64_t bytes = static_cast<uint64_t>(fs::file_size(finalProbe, ec));
    if (ec || bytes == 0) {
        fail("合并完成但产物是空的（" + PathToUtf8(finalPath) + "，查大小出错：" + ec.message() + "）。");
        return;
    }

    // 时长用 ffmpeg 报的（不用自己算，TS 的 PTS 只有 ffmpeg 清楚）。
    double durationSeconds = status_.segmentCount * 2.0; // 兜底估计
    {
        std::vector<std::string> pargs;
        pargs.push_back("-hide_banner");
        pargs.push_back("-nostdin");
        pargs.push_back("-i");
        pargs.push_back(PathToUtf8(finalPath));
        const std::wstring pcmd = BuildCommandLine(ffmpeg, pargs);
        std::string plog;
        DWORD pcode = 1;
        // -i 不带输出时 ffmpeg 返回 1，但 stderr 里有 "Duration: 00:00:30.00"
        RunFfmpeg(ToUtf8(pcmd), plog, pcode);
        const size_t at = plog.find("Duration: ");
        if (at != std::string::npos) {
            int h = 0, m = 0;
            double s = 0.0;
            if (std::sscanf(plog.c_str() + at, "Duration: %d:%d:%lf", &h, &m, &s) == 3) {
                durationSeconds = h * 3600.0 + m * 60.0 + s;
            }
        }
        (void)pcode;
    }

    char summary[256];
    std::snprintf(summary, sizeof(summary), "%u 个分片 / %.1f 秒 / %.1f MB", status_.segmentCount,
                  durationSeconds, static_cast<double>(bytes) / (1024.0 * 1024.0));

    {
        std::lock_guard<std::mutex> lk(mu_);
        status_.durationSeconds = durationSeconds;
        status_.bytes = bytes;
        status_.elapsedMs = elapsedMs();
    }
    Finish(true, summary, PathToUtf8(finalPath), tmpDir, listFile);
}

} // namespace shadow
