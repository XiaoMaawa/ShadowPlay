// GuardianMode.cpp — 采集服务的守护进程（--guard 模式），见 GuardianMode.h
//
// 循环只有三步：看服务在不在 → 不在就拉起来 → 写心跳。没有别的职责，
// 所以它自己几乎不可能崩，也不需要什么资源。
#include "GuardianMode.h"

#include "RingNames.h"
#include "StatusFile.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace shadow {
namespace {

namespace fs = std::filesystem;

constexpr int kPollMs = 2000;            // 正常巡检间隔
constexpr int kStartWaitMs = 8000;       // 拉起服务后等它就绪的时间
constexpr int kBackoffMaxMs = 30000;     // 连续失败时的退避上限
constexpr int kHeartbeatStaleMs = 12000; // 心跳多久算过期

/** 现在的时间（ISO 8601，本地时间，秒精度）。心跳里的时间戳只给人看。 */
std::string NowIso() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32]{};
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return std::string(buf);
}

/** 最简 JSON 字符串转义：路径里有反斜杠，中文用户名也可能进来。 */
std::string JsonEsc(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\u%04x", c);
                    out += b;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

/** 从一行 JSON 里抠一个数字字段（够用就行，不追求完整解析）。 */
double FindNumber(const std::string& text, const std::string& key, double dflt = 0) {
    const std::string pat = "\"" + key + "\"";
    size_t at = text.find(pat);
    if (at == std::string::npos) return dflt;
    at = text.find(':', at + pat.size());
    if (at == std::string::npos) return dflt;
    return std::strtod(text.c_str() + at + 1, nullptr);
}

/** 某个 pid 现在还活着吗（只是探活，不打开有副作用的句柄）。 */
bool PidAlive(uint32_t pid) {
    if (pid == 0) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h) return false;
    DWORD code = 0;
    const bool exited = GetExitCodeProcess(h, &code) && code != STILL_ACTIVE;
    CloseHandle(h);
    return !exited;
}

/**
 * 采集服务在不在跑。
 *
 * 只看互斥体：它是内核对象，服务进程一退出（哪怕是被强杀）内核立刻把它销毁，
 * OpenMutex 随即失败 —— 不会像"状态文件还新鲜"那样残留假象，也不会像
 * CreateMutex 那样被守护自己续命。
 */
bool ServiceAlive() {
    HANDLE m = OpenMutexA(SYNCHRONIZE, FALSE, ServiceMutexName().c_str());
    if (!m) return false;
    CloseHandle(m);
    return true;
}

/** 自己的 exe 路径（守护和服务是同一个 exe，拉起服务时直接用它）。 */
std::wstring SelfExePath() {
    wchar_t buf[MAX_PATH * 2]{};
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return n ? std::wstring(buf, n) : std::wstring();
}

/** 日志：%TEMP%/shadow-play/app/logs/guardian.log（和状态文件同一个临时根）。 */
std::wstring LogFilePath() {
    const fs::path status = fs::u8path(StatusFilePath());
    return (status.parent_path().parent_path() / L"logs" / L"guardian.log").wstring();
}

/**
 * 往日志里追加一行。
 *
 * 【守护绝对不能 printf】它是 DETACHED_PROCESS 起起来的，**没有控制台**，
 * stdout 是个无效句柄 —— 往里写一次就会让整个守护进程当场消失（第一次实测
 * 就是这么没的：服务被杀之后守护也跟着没了，看门等于不存在）。
 * 日志写文件，既安全又能事后查"到底重启过几次"。
 */
void AppendLog(const std::string& line) {
    const std::wstring p = LogFilePath();
    std::error_code ec;
    fs::create_directories(fs::path(p).parent_path(), ec);
    HANDLE h = CreateFileW(p.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::string text = line;
    text += "\n";
    DWORD written = 0;
    WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(h);
}

/**
 * 以 detached 方式起一个进程。
 *
 * DETACHED_PROCESS：不继承本进程的控制台 —— 守护/服务都不需要控制台，
 * 继承了反而会在控制台被关掉时收到 CTRL_CLOSE_EVENT 一起退出。
 *
 * 【但脱离控制台 = 标准句柄是空的，必须显式给】
 * 服务主循环每秒 printf 一行状态。被守护拉起来的服务如果 stdout 是个无效
 * 句柄，那一下 printf 就能把它送走 —— 守护就会陷入"拉起 → 立刻死 → 再拉起"。
 * 所以这里把 stdin 接到 NUL、stdout/stderr 接到日志文件。
 *
 * 不建作业对象：以前服务给子进程建过 job，结果"界面关掉把服务一起带走"
 * （那段代码已经删了，见 main.cpp 里 g_childJob 删除处的说明）。这里同样
 * 不能把被拉起的服务绑到守护身上 —— 守护自己被杀时服务不该跟着死。
 */
bool SpawnDetached(const std::wstring& args) {
    const std::wstring exe = SelfExePath();
    if (exe.empty()) return false;

    std::wstring cmd = L"\"" + exe + L"\" " + args;
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');

    std::error_code ec;
    fs::create_directories(fs::path(LogFilePath()).parent_path(), ec);

    // stdin → NUL（服务/守护都不读它；给 NUL 好过留空句柄）
    HANDLE hIn = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    // stdout/stderr → 日志（追加）。打不开就退回 NUL，绝不留空句柄。
    HANDLE hOut = CreateFileW(LogFilePath().c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hOut == INVALID_HANDLE_VALUE) {
        hOut = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    bool useHandles = (hIn != INVALID_HANDLE_VALUE) && (hOut != INVALID_HANDLE_VALUE);
    if (useHandles) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdInput = hIn;
        si.hStdOutput = hOut;
        si.hStdError = hOut;
    }

    PROCESS_INFORMATION pi{};
    const DWORD flags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP;
    const BOOL ok = CreateProcessW(exe.c_str(), mutableCmd.data(), nullptr, nullptr,
                                   useHandles ? TRUE : FALSE, flags, nullptr, nullptr, &si, &pi);
    // 这几个句柄属于**子进程**（继承过去了），父进程这边立刻关掉。
    if (hIn != INVALID_HANDLE_VALUE) CloseHandle(hIn);
    if (hOut != INVALID_HANDLE_VALUE) CloseHandle(hOut);
    if (!ok) return false;

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

/** 服务模式的命令行参数：--profiles <配置>；没有配置就不带（服务走单路旧模式）。 */
std::wstring ServiceArgs(const std::string& profilesPath) {
    return profilesPath.empty() ? std::wstring()
                                : (L"--profiles \"" + Utf8ToWide(profilesPath) + L"\"");
}

/** 整份覆盖写心跳（先写临时文件再原子替换，避免读者读到半截 JSON）。 */
void WriteHeartbeat(uint32_t restarts, uint32_t servicePid, const std::string& lastError,
                    const std::string& lastRestartAt, const std::string& profilesPath) {
    const std::string path = GuardianHeartbeatPath();
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    if (ec) return;

    const std::string doc =
        "{\"pid\":" + std::to_string(GetCurrentProcessId()) +
        ",\"updatedAt\":\"" + NowIso() + "\"" +
        ",\"servicePid\":" + std::to_string(servicePid) +
        ",\"restarts\":" + std::to_string(restarts) +
        ",\"lastRestartAt\":\"" + JsonEsc(lastRestartAt) + "\"" +
        ",\"lastError\":\"" + JsonEsc(lastError) + "\"" +
        ",\"profilesPath\":\"" + JsonEsc(profilesPath) + "\"}";

    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f << doc;
    }
    // MOVEFILE_REPLACE_EXISTING：原子替换，界面永远不会读到写了一半的文件。
    MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

} // namespace

// ---- 停机哨兵（外部链接，TrayIcon.cpp / main.cpp 也要调）---------------------
// 见 GuardianMode.h 的说明：状态目录下的 shutdown-requested 一旦存在，服务与守护
// 都该退出且不再互拉。

std::string ShutdownSentinelPath() {
    const fs::path status = fs::u8path(StatusFilePath());
    return (status.parent_path() / "shutdown-requested").u8string();
}

bool ShutdownRequested() {
    std::error_code ec;
    return fs::exists(fs::u8path(ShutdownSentinelPath()), ec);
}

void WriteShutdownSentinel() {
    const std::string path = ShutdownSentinelPath();
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    // 内容无所谓，存在即意图。空文件即可。
    HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

std::string GuardianHeartbeatPath() {
    // 和状态文件同目录：界面已经在算状态文件的路径了，守护这份它就顺手能读。
    // StatusFilePath() 返回的是 UTF-8（中文用户名也不会乱码），fs::path 从
    // string 构造在 Windows 上按 ANSI 解释会出问题，所以走 u8path。
    static const std::string cached = []() {
        const fs::path status = fs::u8path(StatusFilePath());
        return (status.parent_path() / "guardian-v1.json").u8string();
    }();
    return cached;
}

bool GuardianHeartbeatAlive(uint32_t* pidOut) {
    const std::string path = GuardianHeartbeatPath();
    std::error_code ec;
    const auto mtime = fs::last_write_time(fs::path(path), ec);
    if (ec) return false;
    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
        fs::file_time_type::clock::now() - mtime);
    if (age.count() > kHeartbeatStaleMs) return false;

    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    const std::string text((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
    const uint32_t pid = static_cast<uint32_t>(FindNumber(text, "pid", 0));
    if (pidOut) *pidOut = pid;
    return PidAlive(pid);
}

bool SpawnGuardian(const std::string& profilesPath) {
    // 停机哨兵已立：别再把服务拉起来（退出流程里先立哨兵再杀，双保险）。
    if (ShutdownRequested()) return false;
    const std::wstring args = L"--guard " + ServiceArgs(profilesPath);
    return SpawnDetached(args);
}

int RunGuardianMode(const std::string& profilesPath) {
    // 守护自己的互斥体：保证只有一个守护。
    //
    // 用 CreateMutex（不是 OpenMutex）：这里要的是"我是不是第一个"，而不是
    // "有没有别人"。已经有一个守护在跑就直接退出，别抢。
    SetLastError(0);
    HANDLE guardMutex = CreateMutexA(nullptr, FALSE, "Local\\ShadowPlay.Guardian.v1");
    if (guardMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(guardMutex);
        AppendLog("guardian_already_running（已经有一个守护在跑，本次退出）");
        return 0;
    }
    if (!guardMutex) {
        AppendLog("guardian_mutex_failed（创建守护互斥体失败）");
        return 1;
    }

    AppendLog("guardian_started pid=" + std::to_string(GetCurrentProcessId()) +
              " profiles=" + (profilesPath.empty() ? "(none)" : profilesPath));

    uint32_t restarts = 0;
    uint32_t servicePid = 0;
    std::string lastError;
    std::string lastRestartAt;
    int backoffMs = kPollMs;

    for (;;) {
        // 有人请求整体停机：别再拉服务，自己退出。
        if (ShutdownRequested()) {
            AppendLog("shutdown-requested 存在，守护退出（不再拉起服务）");
            return 0;
        }

        if (ServiceAlive()) {
            if (backoffMs != kPollMs) backoffMs = kPollMs;
            WriteHeartbeat(restarts, servicePid, lastError, lastRestartAt, profilesPath);
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
            continue;
        }

        // 服务不在：拉起来。
        ++restarts;
        lastRestartAt = NowIso();
        const bool spawned = SpawnDetached(ServiceArgs(profilesPath));

        // 等它把互斥体建起来（建起来才算真的活着）。
        bool up = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kStartWaitMs);
        while (std::chrono::steady_clock::now() < deadline) {
            if (ServiceAlive()) {
                up = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        if (up) {
            lastError.clear();
            backoffMs = kPollMs;
            AppendLog("已重新拉起采集服务（第 " + std::to_string(restarts) + " 次）");
        } else {
            // 拉不起来（配置坏了会"起来就退"）：退避，别变成每秒一次的重启风暴。
            lastError = spawned ? "拉起后 8 秒仍未就绪" : "CreateProcess 失败";
            backoffMs = backoffMs * 2 < kBackoffMaxMs ? backoffMs * 2 : kBackoffMaxMs;
            AppendLog("重新拉起失败：" + lastError + "，退避 " + std::to_string(backoffMs) + "ms");
        }

        WriteHeartbeat(restarts, servicePid, lastError, lastRestartAt, profilesPath);
        std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
    }
}

} // namespace shadow
