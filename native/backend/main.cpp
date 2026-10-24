// main.cpp — ShadowPlay 采集服务（生产者）
//
// 这是一个**常驻后台进程**，自己不开窗口：
//
//     shadow-capture.exe        <-- 独立启动，长期运行
//            |
//            +-- DXGI 桌面复制 --> 共享内存帧环 <-- 命名事件通知
//                                        ^
//                                        |
//                                  Electron 主进程
//
// 为什么是"服务 + 连接"而不是"Electron 拉起子进程"：
//   * 采集端和界面完全解耦：关掉/重开界面不影响采集，采集也不会因为
//     界面崩溃而变成孤儿进程；
//   * 采集服务用默认权限运行，不继承界面的低权限沙箱令牌，
//     桌面复制和 GPU 访问都更稳；
//   * 多开界面时可以共享同一份采集结果。
//
// 命令行参数：
//   --session <id>       自定义会话 id（默认用固定的服务 id，见 RingNames.h）
//   --output <n>         显示器序号，默认 0
//   --fps <n>            目标帧率上限，0 = 桌面有变化就发（默认 60）
//   --consumer-pid <pid> 可选：绑定一个固定的消费者 pid
//   --once               打印一次状态后退出（自检用）
//   --status             只探测服务是否在运行，然后退出
//   --help
//
// stdout 输出 NDJSON：
//   {"type":"service_started",...}
//   {"type":"consumer_connected","pid":N}
//   {"type":"status",...}          每秒一次
//   {"type":"capture_reset"}       分辨率/权限变化后重建
//   {"type":"error","code":...,"message":...}
//
// profiles 模式下还会额外做两件事：
//   * 把同样这几行状态**镜像**到状态文件（默认
//     %LOCALAPPDATA%\shadow-play\status-v1.json，见 StatusFile.h）。
//     这一条是给"不是本进程启动者"的界面看的：在 cmd 里直接起服务时，
//     Electron 读不到我们的 stdout，只能读那个文件。
//   * 从 stdin 读 NDJSON 命令（export / reload），命令通道**只**走 stdin。
//
// 退出码：0 正常；2 参数错误；3 采集无法初始化；4 共享内存创建失败；5 已有实例在运行。


#include "Exporter.h"
#include "MfEncoder.h" // MfAcquire / MfRelease：报编码能力自检
#include "ServiceEntry.h" // ServiceMain / RequestServiceStop / Query*：入口层
#include "Utf8Path.h"
#include "ScreenCapture.h"
#include "FrameRing.h"
#include "JsonUtil.h"
#include "Profiles.h"
#include "RingNames.h"
#include "Session.h"
#include "StatusFile.h"
#include "GuardianMode.h"
#include "TrayIcon.h"
#include "WindowEnum.h"

#include <windows.h>
// 命令管道的 DACL：EXPLICIT_ACCESSW / SetEntriesInAclW（见管道线程上方的注释）。
#include <aclapi.h>
#pragma comment(lib, "advapi32.lib")
// timeBeginPeriod / timeEndPeriod：把系统定时器精度提到 1ms。
// 否则默认约 15.6ms 的粒度会毁掉 16.67ms 的发布节奏。
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace shadow;

// 【作业对象已经删了】
//
// 以前这里有一个 g_childJob + ShadowChildJob()：每个会话起 ffmpeg 子进程时
// 把它挂进去，服务被强杀时靠 JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE 一起收掉，
// 免得留下孤儿进程继续往 records/ 里写分片。
//
// 现在编码走进程内 Media Foundation（MfEncoder），整个项目**没有任何子进程**，
// 也就没有孤儿可担心 —— 服务一挂，SinkWriter 跟着地址空间一起消失，内核
// 会把文件句柄关掉，最多留一个没写完的 fMP4 分片（fMP4 每个 moof 自带索引，
// 前面写进去的部分照样能读）。所以这个作业对象连同 TerminateJobObject 一起
// 移除了，别再往这里加回来。

namespace {

std::atomic<bool> g_running{ true };
HANDLE g_wakeEvent = nullptr;  // RunProfilesMode 建好后再填上，退出时用来立刻唤醒主循环
std::mutex g_outMutex;

// 睡到指定时刻（分段睡：Sleep 单次超过 ~15ms 在中途被打断时误差会变大）。
// 配合 main() 里的 timeBeginPeriod(1)，实际落点误差在 1ms 内。
void SkipUntil(const std::chrono::steady_clock::time_point& target) {
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= target) return;
        const auto remainMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(target - now).count();
        if (remainMs <= 0) return;
        Sleep(static_cast<DWORD>(remainMs > 8 ? 8 : remainMs));
    }
}

void LogLine(const std::string& json) {
    std::lock_guard<std::mutex> lock(g_outMutex);
    std::fputs(json.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

// 状态镜像对象。定义在 PublishStatusFile 之前（那个函数要用它）。
// 为什么是文件而不是命名管道：见 StatusFile.h 顶部说明 —— 管道那版实现过、
// 也验证失败了，失败原因是"死掉的观察者永远发现不了"。
shadow::StatusFile g_statusMirror;

// 状态行：走 stdout（启动者读得到）。
//
// 之所以**不**顺手镜像进状态文件：状态文件里存的是一整份 JSON 文档，如果
// 再往里塞 "profiles_loaded" / "export_result" 这种零散行，读的人就没法用
// 一个 JSON.parse 直接拿到状态了 —— 而"打开就读，不必自己按行拼"正是换成
// 文件方案的理由。所以：
//   * stdout  -> NDJSON，事件流，随到随发（LogStatusLine / LogLine）
//   * 状态文件 -> 一整份 JSON，每秒覆盖重写（PublishStatusFile）
// 状态文件的价值是"谁都能读"，事件流的价值是"一次性的通知不会丢"。
void LogStatusLine(const std::string& json) { LogLine(json); }

// 状态文件里那份完整文档。每秒调一次。
//
// 除了 sessions，还要带上**这份状态是哪个进程、用哪份配置产生的**（pid /
// profilesPath）。理由：界面会复用系统里已经在跑的服务，而它只能靠
// `shadow-capture --status` 知道"有服务"，不知道那个服务加载的是哪份配置。
// 少了这两个字段，就会出现"界面按 A 配置显示的录制状态，其实是 B 配置的服务
// 录的" —— 表现为界面报 7 段、点导出却说没有可导出的分片。界面对比
// profilesPath 和自己那份不一致时会显式提示（见 app/main.cjs 的 profilesMismatch）。
void PublishStatusFile(const std::string& sessionsJson, const std::string& recordRoot,
                       const std::string& exportDir, const std::string& profilesPath) {
    g_statusMirror.Publish("{\"type\":\"sessions\",\"ts\":" +
                           std::to_string(static_cast<long long>(GetTickCount64())) +
                           ",\"pid\":" + std::to_string(GetCurrentProcessId()) +
                           ",\"profilesPath\":\"" + JsonEscape(profilesPath) +
                           "\",\"recordRoot\":\"" + JsonEscape(recordRoot) +
                           "\",\"exportDir\":\"" + JsonEscape(exportDir) +
                           "\",\"items\":[" + sessionsJson + "]}");
}

void StartStatusMirror() { g_statusMirror.Start(); }
void StopStatusMirror() { g_statusMirror.Stop(); }

// 转义实现统一在 native/backend/JsonUtil.h（inline），这里只把它引进
// 当前 translation unit，避免每个调用点都写 shadow:: 前缀。
using shadow::JsonEscape;

void LogError(const std::string& code, const std::string& message) {
    LogLine("{\"type\":\"error\",\"code\":\"" + JsonEscape(code) + "\",\"message\":\"" +
            JsonEscape(message) + "\"}");
}

// ---------------------------------------------------------------------------
// 这里原本有一个 ShadowRelaunchElevated()：抓不到采集能力时用 ShellExecuteExW
// 带 "runas" 谓词把自己以管理员身份重新拉起来（服务内的自动提权）。
//
// 已经**整个删掉**。理由都来自实测，不是偏好：
//   * 普通权限的服务抓资源管理器窗口是正常的（WGC 那一路 live 且在录），
//     "窗口捕获必须要管理员"这个前提不成立；
//   * 它会在启动时莫名其妙弹一次 UAC —— 用户看到的是"我收到两次 uac 提权，
//     一次 node，一次什么什么 capture.exe 的"，第二次就是它；
//   * 提权后的进程是 High 完整性级别，普通权限的 taskkill / 命名管道都够不着
//     它，停服务和发命令全得再提权一次，越绕越复杂；
//   * 它只会在"一路会话都没起来"时触发，而那种情况更可能就是配置写错了
//     （窗口不存在），提权也不会让一个不存在的窗口出现 —— 白弹一次窗。
//
// 现在服务就用当前权限跑。真起不来，就在启动日志里把原因说清楚（见
// RunProfilesMode 里那段自检），把判断交给用户。
// ---------------------------------------------------------------------------

// 转义实现统一在 native/backend/JsonUtil.h（inline），这里只把它引进
// 当前 translation unit，避免每个调用点都写 shadow:: 前缀。
using shadow::JsonEscape;

struct Options {
    std::string session;
    uint32_t output = 0;
    // 目标帧率上限。默认 60：桌面复制本身可能以 200Hz+ 出帧，
    // 不设上限会白烧 CPU/GPU，而人眼和显示器也用不上。
    // 传 --fps 0 表示不限制（桌面有变化就发，且静止时也全速重复发布）。
    double fps = 60.0;
    uint32_t consumerPid = 0;
    bool once = false;
    bool statusOnly = false;
    bool listWindows = false;         // --list-windows：列出可采集的顶层窗口
    bool listMonitors = false;        // --list-monitors：列出可采集的显示器
    bool guardian = false;            // --guard：跑守护模式（见 GuardianMode.h）
    // --include-hwnd=N（可重复）：这些 hwnd 无条件进 --list-windows 的结果。
    // 界面把"服务正在录的 hwnd"传进来，好让窗口下拉里一定有它
    // （它可能是最小化的，正常枚举会被过滤掉）。
    std::vector<HWND> includeHwnds;
    std::string profilesPath;         // --profiles：配置文件路径（空 = 单路旧模式）
};

void PrintUsage() {
    std::fputs(
        "shadow-capture [--session ID] [--output N] [--fps N] [--once] [--status]\n"
        "               [--list-windows] [--list-monitors] [--profiles FILE]\n"
        "  --session <id>     自定义会话 id（默认固定服务 id）\n"
        "  --output <n>       显示器序号，默认 0\n"
        "  --fps <n>          目标帧率上限，默认 60；传 0 表示不限制\n"
        "  --once             打印一次状态后退出\n"
        "  --status           只检查服务是否在运行\n"
        "  --list-windows     列出当前可采集的顶层窗口（NDJSON）后退出\n"
        "  --include-hwnd N   与 --list-windows 同用：指定的 hwnd 无条件列出\n"
        "                     （可重复；用于把正在录的最小化窗口带进下拉框）\n"
        "  --list-monitors    列出当前已连接的显示器（NDJSON）后退出\n"
        "  --profiles <path>  读配置并按配置起多路会话（见 DESIGN.md 第 7 节）\n",
        stderr);
}

bool ParseCommandLine(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](std::string& out) -> bool {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return true;
        };
        if (a == "--session") {
            if (!next(opt.session)) return false;
        } else if (a == "--output") {
            std::string v;
            if (!next(v)) return false;
            opt.output = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
        } else if (a == "--fps") {
            std::string v;
            if (!next(v)) return false;
            opt.fps = std::strtod(v.c_str(), nullptr);
        } else if (a == "--consumer-pid") {
            std::string v;
            if (!next(v)) return false;
            opt.consumerPid = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
        } else if (a == "--profiles") {
            if (!next(opt.profilesPath)) return false;
        } else if (a == "--list-windows") {
            opt.listWindows = true;
        } else if (a == "--list-monitors") {
            opt.listMonitors = true;
        } else if (a == "--include-hwnd") {
            std::string v;
            if (!next(v)) return false;
            const unsigned long long n = std::strtoull(v.c_str(), nullptr, 10);
            if (n != 0) opt.includeHwnds.push_back(reinterpret_cast<HWND>(static_cast<uintptr_t>(n)));
        } else if (a == "--once") {
            opt.once = true;
        } else if (a == "--status") {
            opt.statusOnly = true;
        } else if (a == "--guard") {
            // 守护模式：不碰服务互斥体，只盯着服务、不在了就把它拉起来。
            // 由服务启动后自己拉起（RunProfilesMode 里的 SpawnGuardian）。
            opt.guardian = true;
        } else if (a == "--help" || a == "-h") {
            return false;
        }
        // 未知参数忽略，方便以后加参数不破坏兼容
    }
    if (opt.session.empty()) opt.session = kServiceSessionId();
    return true;
}

// 服务互斥体：同一登录会话里只允许一个采集服务。
// 返回 nullptr 表示创建失败；alreadyExists 表示已经有实例在跑。
HANDLE AcquireServiceMutex(bool& alreadyExists) {
    alreadyExists = false;
    const std::string name = ServiceMutexName();
    // 关键：CreateMutexA **成功时不会把 last-error 清成 0**。
    // 如果上一次调用（哪怕完全无关的 API）留下了 ERROR_ALREADY_EXISTS(183)，
    // 这里 GetLastError() 读到的就是那个陈旧值，于是把自己创建成功的互斥体
    // 误判成"已有实例在跑" —— 表现就是服务永远起不来，还一直报 already_running，
    // 而且 `--status` 也会谎报在运行。必须先显式清零。
    SetLastError(0);
    HANDLE h = CreateMutexA(nullptr, FALSE, name.c_str());
    if (!h) return nullptr;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        alreadyExists = true;
    }
    return h;
}

// 从一行 NDJSON 里抠出某个字符串字段的值（只认 "key":"value" 这种最简形式）。
//
// 为什么不用 Profiles.cpp 里那个正经解析器：那个是给配置文档用的、带完整状态机，
// 而命令通道每行只有两三个字段。为这点东西把解析器暴露出来不值当。
// 转义也只处理 \" 和 \\ —— 配置 id 是 Electron 生成的 `p-xxxxxx`，不会更复杂。
std::string FindJsonString(const std::string& line, const std::string& key) {
    const std::string pat = "\"" + key + "\"";
    size_t at = line.find(pat);
    if (at == std::string::npos) return {};
    at = line.find(':', at + pat.size());
    if (at == std::string::npos) return {};
    at = line.find('"', at + 1);
    if (at == std::string::npos) return {};

    std::string out;
    for (size_t i = at + 1; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '\\' && i + 1 < line.size()) {
            out.push_back(line[++i]);
            continue;
        }
        if (c == '"') break;
        out.push_back(c);
    }
    return out;
}

// 从一行 NDJSON 里抠出某个数字字段（找不到时返回 dflt）。
double FindJsonNumber(const std::string& line, const std::string& key, double dflt) {
    const std::string pat = "\"" + key + "\"";
    size_t at = line.find(pat);
    if (at == std::string::npos) return dflt;
    at = line.find(':', at + pat.size());
    if (at == std::string::npos) return dflt;
    return std::strtod(line.c_str() + at + 1, nullptr);
}

// ---------------------------------------------------------------------------
// 多配置模式（--profiles）
//
// 跟单路旧模式的区别：
//   * 一路配置 = 一个 Session = 一个帧环（环名就是配置 id）
//   * 桌面源按显示器序号分建 ScreenCapture（同一台显示器共享一个）
//   * 每秒把所有会话的状态拼成一条 {"type":"sessions","items":[...]} 输出
//
// 界面端只要按配置 id 去 open 对应的环就行，跟单路模式的协议完全兼容。
// ---------------------------------------------------------------------------
int RunProfilesMode(const Options& opt, HANDLE mutex) {
    // 状态镜像要在第一条状态行之前起来 —— 观察者（在 cmd 里起的那个服务的
    // 界面）只能从管道读，晚起来的行它永远补不到。它内部是"连不上就等"，
    // 所以这里立刻返回，不会拖慢启动。
    StartStatusMirror();

    // 把服务的 NDJSON 日志出口接到 Exporter 上 —— 导出时会打一行
    // export_plan，把"这一次到底铺了哪些分片、哪些位置补了 NO SIGNAL"
    // 完整记下来。没有这行日志，"导出里莫名出现 NO SIGNAL"只能靠猜。
    SetExporterPlanLog([](const std::string& line) { LogLine(line); });

    ProfileDoc doc;
    std::string err;
    if (!ProfileStore::Load(opt.profilesPath, doc, err)) {
        LogError("profiles_load_failed", err);
        CloseHandle(mutex);
        return 4;
    }

    // 哪些配置要真的起会话？enabled 的都要。
    std::vector<const Profile*> wanted;
    for (const auto& p : doc.profiles) {
        if (p.enabled) wanted.push_back(&p);
    }

    LogStatusLine("{\"type\":\"profiles_loaded\",\"path\":\"" +
                  JsonEscape(opt.profilesPath) + "\",\"total\":" +
                  std::to_string(doc.profiles.size()) + ",\"enabled\":" +
                  std::to_string(wanted.size()) + ",\"recordRoot\":\"" +
                  JsonEscape(doc.recordRoot) + "\",\"exportDir\":\"" +
                  JsonEscape(doc.exportDir) + "\"}");

    if (wanted.empty()) {
        LogStatusLine("{\"type\":\"no_sessions\",\"message\":\"没有任何启用的配置。\"}");
    }

    timeBeginPeriod(1);

    // （这里原来建子进程作业对象。现在没有子进程了，见文件上方 g_childJob
    // 删除处的说明。）

    // 按显示器分建桌面采集：每一路桌面源用自己的显示器序号，序号相同的
    // 共享同一个 ScreenCapture（桌面复制对同一块屏重复初始化会浪费一大块
    // GPU 和内存，而且没必要）。
    //
    // 为什么必须按路建，而不是像以前那样只建一个：「只有主显示器的画面」
    // 就是以前那个"取第一路的序号当全局"埋下的 —— 只要第一路配的是显示器
    // 0，其余配显示器 1/2 的配置也全都在录显示器 0。
    std::map<uint32_t, std::unique_ptr<ScreenCapture>> desktops;
    std::vector<std::string> desktopFailures;
    auto desktopFor = [&](uint32_t index) -> ScreenCapture* {
        auto it = desktops.find(index);
        if (it != desktops.end()) return it->second.get();
        auto cap = std::make_unique<ScreenCapture>();
        if (!cap->Init(index, 2)) {
            LogError("capture_init_failed", "无法初始化显示器 " + std::to_string(index) +
                                                " 的桌面复制。");
            desktopFailures.push_back("显示器 " + std::to_string(index));
            desktops.emplace(index, nullptr); // 记住失败，别反复重试
            return nullptr;
        }
        LogStatusLine("{\"type\":\"desktop_ready\",\"monitor\":" + std::to_string(index) +
                      ",\"width\":" + std::to_string(cap->Width()) + ",\"height\":" +
                      std::to_string(cap->Height()) + "}");
        ScreenCapture* raw = cap.get();
        desktops.emplace(index, std::move(cap));
        return raw;
    };

    // 预览帧率上限（0 = 不限）。
    //
    // 采集循环的节拍由**录制帧率**定，每一拍都要把整屏帧 memcpy 进共享环给
    // 界面预览。1440p 一帧 14.7MB，120fps 就是 1.77GB/s —— 实测会把循环从
    // 120fps 拖到 91fps。预览窗口在屏幕上没多大，少发一半帧肉眼无感，但带宽
    // 直接减半。这个值由界面的设置页下发，改完下一拍就生效（不需要 rebuild）。
    //
    // 声明在这里（而不是命令通道那片）是因为下面创建会话时要立刻套上它；
    // 用 atomic 是为了让会话线程每拍读它时不用进 cmdMutex —— 那把锁被命令
    // 解析持有，采集线程去抢会让节拍抖动。
    std::atomic<uint32_t> previewFps{0};

    std::vector<std::unique_ptr<Session>> sessions;
    sessions.reserve(wanted.size());
    for (const auto* p : wanted) {
        ScreenCapture* shared = nullptr;
        if (!p->source.wgc) {
            shared = desktopFor(p->source.monitorIndex);
            if (!shared) continue; // 这块屏抓不到，这一路就算了，别的路照常
        }
        auto s = std::make_unique<Session>(*p, shared, doc.recordRoot);
        // 新建的会话也要立刻套上当前的预览帧率上限，否则启动时配置的 60
        // 要等到用户重设一次设置才生效（默认 store 里存的是 60）。
        s->SetPreviewFps(previewFps.load(std::memory_order_relaxed));
        if (!s->Start()) {
            LogError("session_start_failed", "配置 " + p->id + " 的会话线程起不来。");
            continue;
        }
        sessions.push_back(std::move(s));
    }

    // 启动自检：把"有没有路真的跑起来了"记一笔。
    //
    // 以前这里是一段"起不来就提权重来一次"的逻辑，已经删掉（理由见上面
    // ShadowRelaunchElevated 那段注释）。现在只做诊断：留一条日志，界面和
    // 排查脚本能一眼看出是"配置写错了"还是"权限不够"。
    //
    // 为什么不能只看 sessions 向量是不是空的：Session::Start() 只是把线程
    // 起起来，线程内部解析显示器 / 建 WGC 失败时 Session 对象照样在向量里，
    // 只是状态变成了 Failed。用显示器 9 试过 —— 向量里有一路、状态是 failed。
    {
        auto anySessionAlive = [&]() -> bool {
            for (const auto& s : sessions) {
                const SessionState st = s->Status().state;
                if (st != SessionState::Failed && st != SessionState::Disabled) return true;
            }
            return false;
        };
        if (!wanted.empty() && sessions.empty()) {
            LogError("no_session_started",
                     "没有任何会话能建立：检查一下配置里的显示器序号 / 目标窗口还在不在。");
        } else if (!sessions.empty() && !anySessionAlive()) {
            // 会话刚起来时是 Starting，给它一点时间落到 Waiting/Failed。
            Sleep(1200);
            if (!anySessionAlive()) {
                LogError("all_sessions_failed",
                         "所有会话都初始化失败了：一般是配置里指定的窗口不存在、"
                         "或者显示器序号超出了实际屏幕数量。");
            }
        }
    }

    LogStatusLine("{\"type\":\"service_started\",\"mode\":\"profiles\",\"pid\":" +
                  std::to_string(GetCurrentProcessId()) + ",\"sessions\":" +
                  std::to_string(sessions.size()) + "}");

    // 顺带报一下编码能力的自检结果。
    //
    // 以前这里报的是 `ffmpeg_resolved`（ffmpeg.exe 在哪）。现在编码全在进程内
    // 走 Media Foundation，没有外部程序可找了 —— 但"点了导出/录制没反应"这
    // 类问题仍然需要一个一眼能看出来的信号，所以改报 MF 起没起来。
    // 起不来的典型原因是系统缺 Media Feature Pack（N 版 Windows 默认没有）。
    {
        const bool mfOk = MfAcquire();
        if (mfOk) MfRelease();
        LogStatusLine("{\"type\":\"encoder_ready\",\"backend\":\"mediafoundation\",\"ok\":" +
                      std::string(mfOk ? "true" : "false") + "}");
    }

    // ---- 拉起守护进程（双向看门的第一条边）----------------------------------
    //
    // 守护是同一个 exe 的 --guard 模式（见 GuardianMode.h）：它盯着这个服务，
    // 服务没了就重新拉起。这里由**服务自己**把它拉起来 —— 不经过 Electron，
    // 因为界面关掉之后恰恰是最需要有人看着服务的时刻。
    // 已经有守护在跑时它自己会立刻退出（守护互斥体），重复拉是安全的。
    if (SpawnGuardian(opt.profilesPath)) {
        LogStatusLine("{\"type\":\"guardian_spawned\"}");
    } else {
        // 拉不起来不致命：服务照常录，只是崩了没人管。
        LogError("guardian_spawn_failed", "守护进程没能拉起来（服务照常运行，但崩溃后不会自动重启）。");
    }

    // ---- 托盘图标（右键菜单里有「退出」）----------------------------------
    //
    // 挂在服务进程上，不挂在 Electron：用户要的是「Electron 关掉服务还在」，
    // 那托盘作为唯一常驻入口也得跟着服务走。见 TrayIcon.h。
    StartTrayIcon(opt.profilesPath);

    // 服务模式是后台进程：若有控制台窗口（被带控制台方式启动时会有），藏掉，
    // 让托盘成为唯一的可见界面。DETACHED 启动的本来就没有控制台窗口。
    if (HWND cw = GetConsoleWindow()) ShowWindow(cw, SW_HIDE);

    // ---- 命令通道（stdin，NDJSON 一行一条）--------------------------------
    //
    // 命令有四条：
    //   {"cmd":"export","id":"p-desk"}            导出最近 replaySeconds 秒
    //   {"cmd":"export","id":"p-desk","seconds":30}  覆盖时长
    //   {"cmd":"reload","id":"p-desk"}            重新读盘，只重建这一路会话
    //   {"cmd":"flush","id":"p-desk"}             把正在写的那一片立刻收尾落盘
    //
    // 【为什么有 flush】界面点导出走的是**插件**（bridge/shadow_ring.cc 的
    // exportStart，在 Electron 主进程里跑），服务根本不知道有这回事 —— 于是
    // Session::ExportNow 里那句 recorder_->FlushSegment() 从来没被执行过。
    // 后果就是用户报的（m12714）：「导出的视频都是点击前一秒或两秒就停止了」：
    // 点导出时总有一片正在写，它没 Finalize、index.csv 里也没有它，导出端只能
    // 读到最后一片**已完成**的，那一片最多 2 秒的内容永远导不出来。
    //
    // 所以界面在调 exportStart 之前，先发这条 flush 让服务把那一片收尾。
    // 收尾不中断录制：紧接着开新片继续录、切片序号继续递增、不换轮次目录，
    // 代价只是那一片比标称短（导出端按每片真实时长铺时间轴，不受影响）。
    //
    // 【为什么有 reload】界面保存配置后，新参数（帧率、位码率、窗口匹配规则）
    // 只落在磁盘上的 profiles.json 里，而正在跑的 Session 手里拿的是**启动时
    // 那份** Profile 副本。以前的做法是"杀掉整个服务再拉起来"—— 可是服务是
    // 多路的，为了改一路就把别人的录制全掐断，等于隔壁分路跟着遭殃。reload
    // 只拆掉目标那一路的 Session 再按新配置重建，其它路一个字节都不动。
    //
    // 为什么要开一条命令通道：导出是**运行中**才能做的事（分片一直在滚动），
    // 而导出现在走进程内 Media Foundation（SourceReader + SinkWriter 直拷），
    // 不再有子进程往服务的 stdout 里混写输出。这条命令通道留着的理由是
    // "什么时候导、导哪一路"由界面说了算，而且不用给它一条新的进程入口。
    //
    // 注意：命令**同时**从两条通道收 —— stdin（Electron 自己 spawn 它时）
    // 和命名管道（谁起的服务都能连）。两条共用同一个解析函数，语义完全一致。
    // 状态镜像那边（状态文件）是**只写**的，原因见 StatusFile.h。
    std::mutex cmdMutex;
    std::vector<std::string> pendingExports;  // 待处理的 "id\tseconds"
    std::vector<std::string> pendingReloads;  // 待重载的 profile id
    std::vector<std::string> pendingFlushes;  // 待收尾分片的 profile id

    // ---- 会话表 / 后台重载 ------------------------------------------------
    //
    // sessions 原来只有主循环一个线程碰，所以没有锁。现在 reload 挪到了后台线程
    // （见主循环里那段注释），增删会话必须加锁；读的那一侧（状态上报、导出、
    // desktopFor 里的会话存活判断）也一律走 sessionsMutex。
    //
    // 只保护**表**本身，不保护表里的 Session —— Session 自己有锁，Stop() 也是
    // 在锁外调的，否则等 ffmpeg 收尾的那一秒多会把主循环全堵死。
    std::mutex sessionsMutex;
    // doc（主循环手里那份配置）的保护。
    //
    // 【为什么需要】doc 过去只在启动时读一次，reload 线程读进来的 fresh 是局部
    // 变量 —— 改了「回放时长」「导出目录」并保存后，正在跑的服务还在用旧值：
    // 导出按旧的 replaySeconds 裁窗口、写到旧的 exportDir，状态文件里报的也是
    // 旧目录，必须重启服务才对。现在 reload 成功后把 fresh 写回 doc，读写都走
    // 这把锁（主循环读、reload 线程写）。
    std::mutex docMutex;
    // 同一时刻只允许一次重建：连点保存不会叠出两组 ffmpeg 抢同一批分片目录。
    std::mutex reloadSerial;
    std::mutex reloadMutex;
    std::vector<std::future<void>> reloadThreads;

    // 收到命令就把主循环叫醒，让它马上去处理。
    //
    // 【为什么要这个】主循环原本是固定的整秒节拍（Sleep(100) × 10）。界面点了
    // 保存、命令 4 毫秒就送到了，却要排在节拍点上才被执行 —— 用户体感就是
    // "点了保存，画面还愣一下才变"。用事件把人叫醒，重载延迟从"最多 1 秒"
    // 变成"最多一次 Sleep(100) 的粒度"，通常几十毫秒内就生效。
    //
    // 用 Windows 事件而不是 condition_variable：主循环还得能被 Ctrl+C 打断，
    // 这个事件可以直接塞进 WaitForMultipleObjects 一起等。
    HANDLE wakeEvent = CreateEventA(nullptr, FALSE /* 自动复位 */, FALSE, nullptr);
    g_wakeEvent = wakeEvent;  // 给 RequestServiceExit 用，退出时立刻唤醒主循环

    // 一条命令行的解析（两条通道共用）。传进来时已经去掉了行尾。
    auto handleCommandLine = [&](const std::string& line) {
        // 优雅停机：{"cmd":"shutdown"}
        //
        // 【为什么需要它】界面上的「重启服务」过去是直接 taskkill /F —— 进程被
        // 立刻打死，Session::Stop() 根本没机会跑：正在写的那个分片既没有
        // Finalize（moov/mfra 索引缺失，文件读不出来），待喂队列里已经采到的
        // 那几帧也一起没了。用户看到的就是「点重启之前最后两秒的切片全部丢失，
        // 而且画面在点按钮之前就断了」。
        //
        // 现在界面先发这一条，服务按正常路径收尾（停会话 → Finalize 当前分片 →
        // 落状态），确认进程退了才轮到 taskkill 兜底。
        //
        // 必须在 "reload"/"export"/"flush" 之前判：那几个是按字样子串匹配的，
        // 这里也按自己的字样匹配，互相不含，顺序只影响可读性。
        if (line.find("\"shutdown\"") != std::string::npos) {
            LogLine("{\"type\":\"shutdown_requested\"}");
            RequestServiceStop();
            return;
        }
        // 预览帧率：{"cmd":"set-preview-fps","fps":60}
        //
        // 先于别的命令判：它靠 "cmd" 字段识别，而 reload/export 靠 "reload"/
        // "export" 字样识别，两者字符串形式完全不同，不会互相误伤。
        if (line.find("\"set-preview-fps\"") != std::string::npos) {
            const double fps = FindJsonNumber(line, "fps", 0.0);
            // 上限 120（与 MfEncoder::kMaxFps 同一口径）：预览再高也没有更多
            // 新帧可画，只是让采集线程多搬几次共享环。
            uint32_t want = fps > 0.0 ? static_cast<uint32_t>(fps + 0.5) : 0u;
            if (want > MfEncoder::kMaxFps) want = MfEncoder::kMaxFps;
            previewFps.store(want, std::memory_order_relaxed);
            // 立刻套到所有已存在的会话上，下一拍就生效
            {
                std::lock_guard<std::mutex> lk(sessionsMutex);
                for (auto& s : sessions) s->SetPreviewFps(previewFps.load(std::memory_order_relaxed));
            }
            LogLine("{\"type\":\"preview_fps_set\",\"fps\":" +
                    std::to_string(previewFps.load(std::memory_order_relaxed)) + "}");
            return;
        }
        if (line.find("\"set-preview-size\"") != std::string::npos) {
            // 界面把预览面板的设备像素尺寸报上来：{"cmd":"set-preview-size","w":2400,"h":1350}
            //
            // 服务用它决定预览要不要降采样（见 Session::SetPreviewTarget）。
            // 这条命令必须和 set-preview-fps 一样"立刻套到所有会话上"：改了窗口
            // 大小之后下一拍就该按新尺寸发，否则要等一次重新连接才生效。
            const double w = FindJsonNumber(line, "w", 0.0);
            const double h = FindJsonNumber(line, "h", 0.0);
            const uint32_t tw = w > 0.0 ? static_cast<uint32_t>(w + 0.5) : 0u;
            const uint32_t th = h > 0.0 ? static_cast<uint32_t>(h + 0.5) : 0u;
            {
                std::lock_guard<std::mutex> lk(sessionsMutex);
                for (auto& s : sessions) s->SetPreviewTarget(tw, th);
            }
            LogLine("{\"type\":\"preview_size_set\",\"w\":" + std::to_string(tw) +
                    ",\"h\":" + std::to_string(th) + "}");
            return;
        }
        if (line.find("\"reload\"") != std::string::npos) {
            const std::string id = FindJsonString(line, "id");
            if (!id.empty()) {
                {
                    std::lock_guard<std::mutex> lk(cmdMutex);
                    pendingReloads.push_back(id);
                }
                SetEvent(wakeEvent);  // 别等节拍，马上处理
            }
            return;
        }
        // 收尾正在写的那一片：{"cmd":"flush","id":"p-desk"}
        //
        // 必须在 "export" 之前判 —— 界面发的是 {"cmd":"flush",...}，
        // 里面不含 "export" 字样，两条不会误伤；但反过来如果以后有人
        // 写成 {"cmd":"export","flush":true}，先判 flush 会把它截走。
        // 用 "flush" 字样识别，和 reload/export 同一套风格。
        if (line.find("\"flush\"") != std::string::npos) {
            const std::string id = FindJsonString(line, "id");
            if (!id.empty()) {
                {
                    std::lock_guard<std::mutex> lk(cmdMutex);
                    pendingFlushes.push_back(id);
                }
                SetEvent(wakeEvent);
            }
            return;
        }
        if (line.find("\"export\"") != std::string::npos) {
            const std::string id = FindJsonString(line, "id");
            if (!id.empty()) {
                // seconds 省略或 <= 0 时用配置自己的回放时长
                const double secs = FindJsonNumber(line, "seconds", 0.0);
                {
                    std::lock_guard<std::mutex> lk(cmdMutex);
                    pendingExports.push_back(id + "\t" + std::to_string(secs));
                }
                SetEvent(wakeEvent);
            }
        }
    };

    // 把一整块读到的字节按行切开，逐行交给上面的解析器。
    auto feedChunk = [&](std::string& acc, const char* data, size_t len) {
        acc.append(data, len);
        for (;;) {
            const size_t nl = acc.find('\n');
            if (nl == std::string::npos) break;
            std::string line = acc.substr(0, nl);
            acc.erase(0, nl + 1);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            handleCommandLine(line);
        }
    };

    // ---- 命令管道的安全描述符 --------------------------------------------
    //
    // 【为什么必须显式给 DACL】
    //
    // CreateNamedPipeA 的最后一个参数传 nullptr 时，Windows 用**调用进程的默认
    // DACL** —— 实际效果是"只有同一登录会话里、完整性级别不低于服务端"的客户端
    // 才能连。服务现在跟界面一样是普通权限（Medium），默认 DACL 通常够用；
    // 但只要有人把这个 exe 手动"以管理员身份运行"，服务就跑到 High，界面
    // （Medium）立刻连不上 —— 这是**曾经真实发生过**的事故。
    //
    // 后果实测过，症状极具欺骗性：
    //   * 界面点保存 → 主进程 pipeSend 连接管道 → **EPERM**；
    //   * 命令一条都送不到服务，服务的 previewFps 恒为 0（= 不限流）；
    //   * 设置页明明写着 60fps，预览照样画出 90fps；
    //   * 改帧率、切换配置同理"存了但没生效"。
    // 而界面那边**看不出错**：保存成功、文件也写了，只是服务从来没收到。
    //
    // 所以这里自己造一个 SD：允许 Everyone 连接并写入（Generic Write / FILE_WRITE
    // _DATA 够了），Administrators 与 SYSTEM 完全控制，保证高完整性客户端也连得上。
    //
    // 【安全性】这条管道只能发"重载某一路配置 / 导出 / 设置预览帧率"这类命令，
    // 不能读写文件、不能执行任意代码。允许 Everyone 的边界是"同一台机器上的
    // 本地用户"，与"开着这个服务就等于允许本地用户操作它"的既有产品语义一致。
    SECURITY_DESCRIPTOR sd{};
    SECURITY_ATTRIBUTES saPipe{};
    PSID everyoneSid = nullptr;
    PSID adminSid = nullptr;
    PSID systemSid = nullptr;
    SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
    SID_IDENTIFIER_AUTHORITY worldAuth = SECURITY_WORLD_SID_AUTHORITY;
    bool saReady = false;
    EXPLICIT_ACCESSW ea[3]{};
    // SetEntriesInAclW 会自己 LocalAlloc 一块新的 ACL 给我们，所以这里只需要
    // 一个指针来接；千万别把栈上的缓冲当输出参数传进去（那是 AC L 的"内容"，
    // 不是"输出容器"）。
    PACL pipeAcl = nullptr;
    if (AllocateAndInitializeSid(&worldAuth, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0,
                                 &everyoneSid) &&
        AllocateAndInitializeSid(&ntAuth, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                 DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminSid) &&
        AllocateAndInitializeSid(&ntAuth, 1, SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0, 0, 0, 0,
                                 &systemSid)) {
        // 客户端只需要"能打开一个入站管道并写进去"。
        //
        // 【为什么给的是 GENERIC_WRITE|GENERIC_READ|SYNCHRONIZE 而不是只要 WRITE】
        //
        // 管道是 PIPE_ACCESS_INBOUND（服务端只读），逻辑上客户端只需要写权限。
        // 但 Node 的 fs.openSync(pipe, 'w') 在 Windows 上请求的访问掩码里带了
        // 读位，DACL 只给 WRITE 时整个 open 直接被拒（EPERM）—— 实测过：
        // 给了纯 GENERIC_WRITE，"打不开管道"照旧。既然服务端本来就是只读的
        // 入站管道，给客户端多一个读位并不会让它真读到什么，索性给全。
        ea[0].grfAccessPermissions = GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE;
        ea[0].grfAccessMode = SET_ACCESS;
        ea[0].grfInheritance = NO_INHERITANCE;
        ea[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
        ea[0].Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        ea[0].Trustee.ptstrName = reinterpret_cast<LPWSTR>(everyoneSid);

        ea[1].grfAccessPermissions = GENERIC_ALL;
        ea[1].grfAccessMode = SET_ACCESS;
        ea[1].grfInheritance = NO_INHERITANCE;
        ea[1].Trustee.TrusteeForm = TRUSTEE_IS_SID;
        ea[1].Trustee.TrusteeType = TRUSTEE_IS_GROUP;
        ea[1].Trustee.ptstrName = reinterpret_cast<LPWSTR>(adminSid);

        ea[2].grfAccessPermissions = GENERIC_ALL;
        ea[2].grfAccessMode = SET_ACCESS;
        ea[2].grfInheritance = NO_INHERITANCE;
        ea[2].Trustee.TrusteeForm = TRUSTEE_IS_SID;
        ea[2].Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        ea[2].Trustee.ptstrName = reinterpret_cast<LPWSTR>(systemSid);

        if (SetEntriesInAclW(3, ea, nullptr, &pipeAcl) == ERROR_SUCCESS && pipeAcl &&
            InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION) &&
            SetSecurityDescriptorDacl(&sd, TRUE, pipeAcl, FALSE)) {
            saPipe.nLength = sizeof(saPipe);
            saPipe.lpSecurityDescriptor = &sd;
            saPipe.bInheritHandle = FALSE;
            saReady = true;
        }
    }
    if (!saReady) {
        // 造不出来也不能让服务起不来：退回默认 DACL。默认 DACL 在"服务与界面
        // 同权限"时够用，只有服务被手动提权跑时命令通道才会失效。
        // 打一条日志，下次再遇到 EPERM 能直接对上。
        LogLine("{\"type\":\"pipe_dacl_failed\"}");
    }

    std::thread cmdThread([&]() {
        const HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
        if (in == nullptr || in == INVALID_HANDLE_VALUE) return;
        std::string acc;
        char buf[1024];
        for (;;) {
            DWORD got = 0;
            if (!ReadFile(in, buf, sizeof(buf), &got, nullptr) || got == 0) break;
            feedChunk(acc, buf, got);
        }
    });
    cmdThread.detach();

    // ---- 命令通道：命名管道 -----------------------------------------------
    //
    // 为什么还要这条：stdin 只有**启动服务的进程**能写。`launch.mjs --bg`
    // 是用 stdio:'ignore' 拉起的服务（stdin 是 NUL，服务端立刻读到 EOF），
    // 在 cmd 里直接跑 exe 同样没有可用 stdin。这两种情况下"界面保存配置 →
    // 让服务重载那一路"就完全走不通，只能退回"杀掉整个服务再拉一个"，把
    // 隔壁分路一起掐断。
    //
    // 命名管道没有这个限制，谁起的服务都能连上来发命令。服务端只读
    // （PIPE_ACCESS_INBOUND），每处理完一个连接就重建一个实例继续等下一个，
    // 客户端（Electron 主进程）每次发完就断开 —— 简单，不存在"卡在一个
    // 死连接上"的问题。
    std::thread pipeThread([&]() {
        const std::string name = CommandPipeName();
        std::string acc;
        char buf[1024];
        while (g_running.load(std::memory_order_acquire)) {
            HANDLE pipe = CreateNamedPipeA(
                name.c_str(), PIPE_ACCESS_INBOUND,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1,
                4096, 4096, 0, saReady ? &saPipe : nullptr);
            if (pipe == INVALID_HANDLE_VALUE) {
                // 建不起来就别死循环空转，退一步等一会儿再试。
                Sleep(500);
                continue;
            }
            const BOOL connected = ConnectNamedPipe(pipe, nullptr)
                                       ? TRUE
                                       : (GetLastError() == ERROR_PIPE_CONNECTED);
            if (connected) {
                acc.clear();
                DWORD got = 0;
                // 【为什么这里是阻塞 ReadFile，而不是"读一次就断开"】
                //
                // 客户端（app/main.cjs 的 pipeSend）每次命令都单独打开一次管道、
                // 写完就关。所以这里读到 EOF（got == 0）就结束一个连接，回到
                // 外层重建实例等下一个人。
                //
                // 这里踩过两个大坑，都记下来免得再犯：
                //
                // 1) 原来读完第一块后跟一句 `PeekNamedPipe(...) 失败就 break`。
                //    可 PeekNamedPipe 在客户端还活着时永远成功，于是循环空转；
                //    等到客户端断开才 break —— 数据其实都读到了，只是断得晚。
                //    单纯"读得晚"不至于丢命令，真正致命的是下面这条。
                //
                // 2) **客户端侧**曾经用 net.connect 建常驻长连接复用。Node 的
                //    net.Socket 连命名管道时，写完之后内部会把管道客户端当成
                //    可半关闭，服务端这条 ReadFile 立刻拿到 ERROR_BROKEN_PIPE
                //    (109) 就断开重建；客户端还以为连接活着，继续写就吃 EPIPE。
                //    结果是：**只有第一条命令能送到，之后的全部石沉大海**。
                //    表现极其阴险 —— 界面点保存、命令写成功、服务也"像是"收到了，
                //    可帧率纹丝不动。修法在客户端：改成每条命令 fs.open 一次。
                //
                // 所以这里保留一句 pipe_read_fail 日志：以后再有类似的"命令发出去
                // 却没反应"，先看有没有这条，能一眼分辨是通道问题还是逻辑问题。
                while (g_running.load(std::memory_order_acquire)) {
                    const BOOL ok = ReadFile(pipe, buf, sizeof(buf), &got, nullptr);
                    if (!ok) {
                        LogLine("{\"type\":\"pipe_read_fail\",\"err\":" +
                                std::to_string(GetLastError()) + "}");
                        break;
                    }
                    if (got == 0) break; // 客户端正常关掉，这个连接就到此为止
                    feedChunk(acc, buf, got); // 立刻处理，不等连接结束
                }
            } else {
                LogLine("{\"type\":\"pipe_connect_failed\",\"err\":" +
                        std::to_string(GetLastError()) + "}");
            }
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
        }
    });
    pipeThread.detach();


    // ---- 主循环：每秒汇总一次 --------------------------------------------
    // 上一次报给界面的导出状态（按 jobId 记），用来只报"变化"。
    std::map<uint64_t, ExportStatus> lastExport;

    // 状态上报的节拍（毫秒）。命令处理不占这个节拍，但会把下一次上报提前。
    int64_t lastReportAt = 0;
    auto nowMs = []() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };

    // 守护心跳检查的节拍计数（下面循环里每 100ms 一拍，50 拍 = 5 秒一次）。
    // 反向看门：守护盯服务，服务也盯守护 —— 它的心跳没了就再拉一个。
    int guardianCheckTicks = 0;

    // ---- 实时电平表（30Hz）------------------------------------------------
    //
    // 【为什么单独一个文件】status-v1.json 是每秒整份重写一次的（几 KB），
    // 拿它画音量条只能得到 1Hz 的台阶。用户要的是"像 OBS 那样实时动"，
    // 所以另起一个几十字节的小文件 id→电平，界面按 30~60Hz 轮询它。
    //
    // 写法和状态文件一样用"先写 .tmp 再 MoveFileEx 换过去"：读的一侧永远
    // 不会读到半截 JSON。代价是每秒 30 次小文件写，忽略不计。
    std::atomic<bool> meterStop{false};
    const auto nowWall = []() {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER u;
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        return static_cast<int64_t>(u.QuadPart / 10000ull) - 11644473600000ll;
    };
    std::thread meterThread([&]() {
        // 和 status-v1.json 放同一个目录（主进程据此找它）。
        const std::wstring meterPath =
            std::filesystem::path(FromUtf8(StatusFilePath())).parent_path().wstring() +
            L"\\meter-v1.json";
        const std::wstring meterTmp = meterPath + L".tmp";
        while (!meterStop.load(std::memory_order_acquire)) {
            std::string js = "{\"ts\":" + std::to_string(nowWall()) + ",\"items\":[";
            bool first = true;
            {
                std::lock_guard<std::mutex> lk(sessionsMutex);
                for (auto& s : sessions) {
                    if (!s) continue;
                    if (!first) js += ",";
                    first = false;
                    js += "{\"id\":\"" + JsonEscape(s->Id()) + "\",\"lv\":" +
                          std::to_string(static_cast<double>(s->MeterLevel())) + "}";
                }
            }
            js += "]}";
            {
                HANDLE f = CreateFileW(meterTmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL, nullptr);
                if (f != INVALID_HANDLE_VALUE) {
                    DWORD written = 0;
                    const BOOL ok = WriteFile(f, js.data(), static_cast<DWORD>(js.size()),
                                              &written, nullptr);
                    CloseHandle(f);
                    if (ok) {
                        if (!MoveFileExW(meterTmp.c_str(), meterPath.c_str(),
                                         MOVEFILE_REPLACE_EXISTING)) {
                            // 目标被占用（极偶尔）：直接覆盖写一次，别停表。
                            HANDLE g = CreateFileW(meterPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                            if (g != INVALID_HANDLE_VALUE) {
                                DWORD w2 = 0;
                                WriteFile(g, js.data(), static_cast<DWORD>(js.size()), &w2,
                                          nullptr);
                                CloseHandle(g);
                            }
                            DeleteFileW(meterTmp.c_str());
                        }
                    } else {
                        DeleteFileW(meterTmp.c_str());
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }
    });

    while (g_running.load(std::memory_order_acquire)) {
        // ---- 守护进程还活着吗（每 5 秒一次）--------------------------------
        //
        // 只读一个 1KB 的 JSON + 一次 OpenProcess，几微秒的事，不影响采集节拍。
        // 没有这一段的话，"守护自己被杀 / 崩了"就没人补回来，双向看门只剩单向。
        if (++guardianCheckTicks >= 50) {
            guardianCheckTicks = 0;
            if (!GuardianHeartbeatAlive()) {
                if (SpawnGuardian(opt.profilesPath)) {
                    LogLine("{\"type\":\"guardian_respawned\"}");
                } else {
                    LogError("guardian_respawn_failed",
                             "守护进程不在了，而且重新拉起失败（服务崩溃后将不会自动重启）。");
                }
            }
        }

        // 等一个短节拍，但**有命令就提前醒**（见 wakeEvent 的注释）。
        // 100ms 而不是 1 秒：命令延迟压到半个节拍以内，Ctrl+C 也依然灵敏。
        // 用 WaitForSingleObject 而不是 WaitForMultipleObjects，是因为停止信号
        // 已经由 g_running 兜住了 —— 下面这个循环体本身就是几十微秒的量级。
        WaitForSingleObject(wakeEvent, 100);
        if (!g_running.load(std::memory_order_acquire)) break;

        // ---- 重载命令：只重建指定的那几路会话 ---------------------------
        //
        // 【为什么挪到后台线程】
        //
        // 这一段原来就在主循环里同步做：Stop() 要把当前分片 Finalize 完
        // （SegmentRecorder::Stop 里 encoder_->Finish() 要等编码器收尾），Start() 又要
        // 重建 SinkWriter 和硬件编码会话。三路一起改就是三份"停+起"，实测界面点一次
        // 保存要盯着"保存中…"等满两秒 —— 而且这期间主循环停摆，状态上报也跟着
        // 卡住，界面看起来像死了一样。
        //
        // 现在主循环只把 id 丢给后台线程就继续跑：状态照常每秒上报（界面上那一路
        // 会短暂显示旧配置，随后被新会话替换），"保存"这个动作本身几十毫秒就返回。
        //
        // 后台线程持 reloadMutex，保证同一时刻只有一次重建在跑 —— 用户连点保存
        // 不会叠出两组编码器抢同一批分片目录。
        std::vector<std::string> reloads;
        {
            std::lock_guard<std::mutex> lk(cmdMutex);
            reloads.swap(pendingReloads);
        }
        if (!reloads.empty()) {
            std::lock_guard<std::mutex> lk(reloadMutex);
            // std::async 而不是裸 std::thread：future 的析构会 join 已经跑完的
            // 任务，异常也能在 get() 里收掉，不会变成 terminate。
            // 注意不能用 emplace_back(lambda)：vector<future<void>>::emplace_back
            // 会去找 future 的构造函数，而 future 的构造只认 std::async / promise。
            reloadThreads.push_back(std::async(std::launch::async, [&, reloads]() {
                std::lock_guard<std::mutex> serial(reloadSerial);
                // 重新读一遍磁盘上的配置。读失败就整体跳过 —— 宁可让界面看到
                // "还是旧的那一路"，也不能拿半份配置把正在录的会话弄坏。
                ProfileDoc fresh;
                std::string loadErr;
                const std::string cfgPath =
                    opt.profilesPath.empty() ? std::string() : opt.profilesPath;
                if (cfgPath.empty() || !ProfileStore::Load(cfgPath, fresh, loadErr)) {
                    LogError("profile_reload_failed",
                             std::string("重新读取配置失败，保留旧会话：") + loadErr);
                    return;
                }
                // 把新配置同步回主循环手里那份：导出时长、导出目录、录制根目录
                // 都是主循环从 doc 里取的（见 ExportNow 的调用处），不同步的话
                // 保存完这几项仍然按旧值干（详见 docMutex 处注释）。
                {
                    std::lock_guard<std::mutex> lk(docMutex);
                    doc = fresh;
                }
                for (const std::string& id : reloads) {
                    const Profile* np = fresh.Find(id);
                    const bool want = np && np->enabled;

                    // 增删会话要独占 sessions；Stop() 会等编码器收尾，所以
                    // 这里**不能**一直攥着 sessionsMutex（主循环要用它报状态）。
                    // 先把旧会话摘出来（独占时间极短），在外面停掉再建新的。
                    std::unique_ptr<Session> old;

                    // ---- 先判「真的变了没有」 ----------------------------------
                    //
                    // 界面保存一次配置就把所有改过的路各发一条 reload，而用户
                    // 经常只是点开编辑、看一眼、点确定 —— 一个字段都没改。以前
                    // 这种情况也会把会话拆了重建：新建录制器（换一轮分片目录）、
                    // 重建帧环、清空 snap_。界面那边看到的是画面黑一下、NO SIGNAL
                    // 遮罩闪一下。这是用户报的「窗口捕获点编辑确定后，导出的画面
                    // 和预览都会短暂出现 no signal」。
                    //
                    // 所以这里先比一遍采集 + 编码参数：完全一致就什么都不做。
                    // 只更新预览帧率（它本来就可以随时改，不需要重建）。
                    {
                        std::lock_guard<std::mutex> lk(sessionsMutex);
                        auto it = std::find_if(sessions.begin(), sessions.end(),
                                               [&](const std::unique_ptr<Session>& s) {
                                                   return s->Id() == id;
                                               });
                        if (it != sessions.end() && want && (*it)->SameSettings(*np)) {
                            (*it)->SetPreviewFps(previewFps.load(std::memory_order_relaxed));
                            LogLine("{\"type\":\"session_reloaded\",\"id\":\"" + JsonEscape(id) +
                                    "\",\"active\":true,\"applied\":false,\"detail\":"
                                    "\"配置没有实质变化，保留原会话\"}");
                            continue;
                        }
                        // ---- 只改了帧率 / 码率：就地改，别拆会话 ----------------
                        //
                        // 用户把 144 改成 60（或反过来）、把码率 20 改成 10 时，
                        // 其它参数一个字没动。拆掉重建会换轮次目录、
                        // recordSegments 归零、时间轴出现空洞 —— 界面表现就是
                        // 「短暂的 no signal」「导出按钮被禁用」「刚切过去点导出
                        // 报该路未在录制」（m13003），以及「改个码率像是重启了
                        // 一次」（m14071）。
                        //
                        // ApplyLiveSettings 只改帧率与码率：录制器让编码器收掉
                        // 当前片、按新参数开新片，墙钟靠 nextSegStartUnix_ 继续
                        // 顺延，导出端看到的是一条连续的时间轴。
                        if (it != sessions.end() && want &&
                            (*it)->SameSettingsIgnoringFpsBitrate(*np)) {
                            const bool applied = (*it)->ApplyLiveSettings(*np);
                            (*it)->SetPreviewFps(previewFps.load(std::memory_order_relaxed));
                            LogLine("{\"type\":\"session_reloaded\",\"id\":\"" + JsonEscape(id) +
                                    "\",\"active\":true,\"applied\":" +
                                    (applied ? "true" : "false") +
                                    ",\"detail\":\"只改了帧率/码率，已就地生效（不重建会话）\"}");
                            if (applied) continue;
                        }
                        // ---- 只改了回放时长：同样就地改，绝不重建 --------------
                        //
                        // 用户的操作：点"保存视频"（导出开始）→ 立刻点"编辑配置"、
                        // 把回放时间从 120 秒改成 10 秒 → 确定。以前这会被判成
                        // "配置变了"，于是拆掉整路会话：预览立刻 NO SIGNAL、录制
                        // 从零开始（用户："似乎什么东西重启了，我其实并不希望他
                        // 重启"），正在跑的导出还会被 reload 线程等住而变得极慢
                        // （用户："刚刚导出的视频会导出的非常的慢"）。
                        //
                        // 回放时长只决定环形缓冲保留多久，改它不需要动采集器/
                        // 编码器：换一个保留预算，下一次分片轮转就把旧片清掉。
                        if (it != sessions.end() && want &&
                            (*it)->SameSettingsIgnoringFpsBitrateReplay(*np)) {
                            const bool applied = (*it)->ApplyLiveSettings(*np);
                            (*it)->SetPreviewFps(previewFps.load(std::memory_order_relaxed));
                            LogLine("{\"type\":\"session_reloaded\",\"id\":\"" + JsonEscape(id) +
                                    "\",\"active\":true,\"applied\":" +
                                    (applied ? "true" : "false") +
                                    ",\"detail\":\"只改了回放时长/帧率/码率，已就地生效"
                                    "（不重建会话、不中断录制）\"}");
                            if (applied) continue;
                        }
                        // 配置确实变了（或者要停用）。旧的摘出来，锁外收尾。
                        if (it != sessions.end()) {
                            old = std::move(*it);
                            sessions.erase(it);
                        }
                    }
                    // ---- 正在导出的这一路要等它导完再拆 --------------------
                    //
                    // 用户的操作顺序是「点保存（开始导出）→ 顺手把回放时长改一下
                    // → 确定」。以前这里直接拆会话：Exporter 析构、导出被拦腰
                    // 打断，成品停在 0KB；界面报"导出失败"，而用户觉得"我就改
                    // 了个设置"。
                    //
                    // 导出通常一两秒，先等一等（最多 3 秒）再继续重建；等不到
                    // 就跳过这一路 —— 宁可这次配置不生效，也不要把一个正在写的
                    // 成品弄坏。下一次保存会重新应用。
                    if (old) {
                        const auto deadline =
                            std::chrono::steady_clock::now() + std::chrono::seconds(3);
                        while (old->ExportState().running &&
                               std::chrono::steady_clock::now() < deadline) {
                            Sleep(80);
                        }
                        if (old->ExportState().running) {
                            // 还没导完：把会话放回去，这次改动不应用。
                            {
                                std::lock_guard<std::mutex> lk(sessionsMutex);
                                sessions.push_back(std::move(old));
                            }
                            LogLine("{\"type\":\"session_reloaded\",\"id\":\"" +
                                    JsonEscape(id) +
                                    "\",\"active\":true,\"applied\":false,\"detail\":"
                                    "\"这一路正在导出，配置改动等下次保存再应用\"}");
                            continue;
                        }
                    }
                    if (old) old->Stop(); // 锁外收尾，主循环照常报状态
                    old.reset();

                    if (!want) {
                        LogLine("{\"type\":\"session_reloaded\",\"id\":\"" + JsonEscape(id) +
                                "\",\"active\":false,\"detail\":\"配置已禁用或不存在\"}");
                        continue;
                    }

                    ScreenCapture* shared = nullptr;
                    if (!np->source.wgc) {
                        shared = desktopFor(np->source.monitorIndex);
                        if (!shared) {
                            LogError("session_reload_failed",
                                     "配置 " + id + " 要的显示器抓不到，这一路没有起来。");
                            continue;
                        }
                    }
                    auto s = std::make_unique<Session>(*np, shared, fresh.recordRoot);
                    // 重建出来的这一路同样要带上预览帧率上限（理由同上）。
                    s->SetPreviewFps(previewFps.load(std::memory_order_relaxed));
                    if (!s->Start()) {
                        LogError("session_reload_failed", "配置 " + id + " 的会话线程起不来。");
                        continue;
                    }
                    {
                        std::lock_guard<std::mutex> lk(sessionsMutex);
                        sessions.push_back(std::move(s));
                    }
                    LogLine("{\"type\":\"session_reloaded\",\"id\":\"" + JsonEscape(id) +
                            "\",\"active\":true,\"applied\":true}");
                }
            }));
            // 已经跑完的重建线程顺手回收（不 join 到底，免得主循环又卡在别人身上）。
            for (auto it = reloadThreads.begin(); it != reloadThreads.end();) {
                if (it->valid() &&
                    it->wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
                    it->get(); // 吞掉异常，别让后台线程的失败变成 terminate
                    it = reloadThreads.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // 再处理 flush 命令：把正在写的那一片立刻收尾落盘。
        //
        // 必须在导出之前处理（同一次被叫醒时 flush 先到就先做）—— 界面点导出
        // 时会先发 flush 再调插件的 exportStart，这里做完它，导出端才读得到
        // "点按钮那一刻"的分片。FlushSegment 会等喂帧线程把片写完，通常
        // 十几毫秒（它只是 Finalize 当前片再开新片），不会卡住状态上报。
        std::vector<std::string> flushIds;
        {
            std::lock_guard<std::mutex> lk(cmdMutex);
            flushIds.swap(pendingFlushes);
        }
        for (const std::string& id : flushIds) {
            Session* target = nullptr;
            {
                std::lock_guard<std::mutex> lk(sessionsMutex);
                for (auto& s : sessions) {
                    if (s->Id() == id) { target = &*s; break; }
                }
            }
            if (!target) {
                LogLine("{\"type\":\"flush_result\",\"id\":\"" + JsonEscape(id) +
                        "\",\"ok\":false,\"detail\":\"没有这个配置的会话。\"}");
                continue;
            }
            const bool ok = target->FlushRecording();
            LogLine("{\"type\":\"flush_result\",\"id\":\"" + JsonEscape(id) +
                    "\",\"ok\":" + (ok ? "true" : "false") + "}");
        }

        // 先处理导出命令（导一次要一秒多，不能占着状态上报的节拍）。
        std::vector<std::string> jobs;
        {
            std::lock_guard<std::mutex> lk(cmdMutex);
            jobs.swap(pendingExports);
        }
        for (const std::string& job : jobs) {
            const size_t tab = job.find('\t');
            const std::string id = job.substr(0, tab);
            const double secs = tab == std::string::npos ? 0.0 : std::strtod(job.c_str() + tab + 1, nullptr);

            Session* target = nullptr;
            {
                // 锁只用来"取出指针"，真正导出在锁外做 —— ExportNow 会跑一秒多。
                std::lock_guard<std::mutex> lk(sessionsMutex);
                for (auto& s : sessions) {
                    if (s->Id() == id) { target = &*s; break; }
                }
            }
            if (!target) {
                LogLine("{\"type\":\"export_result\",\"id\":\"" + JsonEscape(id) +
                        "\",\"ok\":false,\"detail\":\"没有这个配置的会话。\"}");
                continue;
            }

            // 时长：命令里给了就用它，否则用该配置的 replaySeconds
            // （"导出最近 N 秒"跟回放缓冲时长本来就是同一个语义）。
            // 这两项都从 doc 取，而 doc 会被 reload 线程改写，所以整体在锁里
            // 抄一份出来用（不要边读边用：锁外再读 doc 可能已经换了一份）。
            double useSeconds = secs;
            std::string exportDirNow;
            {
                std::lock_guard<std::mutex> lk(docMutex);
                if (useSeconds <= 0.0) {
                    for (const auto& p : doc.profiles) {
                        if (p.id == id) {
                            useSeconds = static_cast<double>(p.video.replaySeconds);
                            break;
                        }
                    }
                }
                exportDirNow = doc.exportDir;
            }

            const bool started = target->ExportNow(exportDirNow, useSeconds);
            LogLine("{\"type\":\"export_started\",\"id\":\"" + JsonEscape(id) +
                    "\",\"name\":\"" + JsonEscape(target->Name()) +
                    "\",\"ok\":" + (started ? "true" : "false") + ",\"seconds\":" +
                    std::to_string(useSeconds) + ",\"exportDir\":\"" +
                    JsonEscape(exportDirNow) + "\"}");
        }

        // ---- 状态上报：满 1 秒才做 ----------------------------------------
        //
        // 命令处理是"来就做"，状态上报是"到点才做"。两件事本来就在同一个循环里，
        // 但如果每次被命令叫醒都全量拼一遍 status JSON 再落盘，命令一密集
        // （点多几下保存）就会白白多写几份文件。用时间戳挡一下。
        const int64_t now = nowMs();
        const bool reportDue = (now - lastReportAt) >= 1000;
        if (!reportDue) continue;
        lastReportAt = now;

        std::string items;
        {
            // 状态上报要遍历整张表 —— 后台重载线程可能正在增删，必须加锁。
            // 这段只是拼字符串（每路一次 StatusJson），几十微秒，占用很短。
            std::lock_guard<std::mutex> lk(sessionsMutex);
            for (size_t i = 0; i < sessions.size(); ++i) {
                if (i) items += ",";
                items += sessions[i]->StatusJson();
            }
        }
        LogStatusLine("{\"type\":\"sessions\",\"items\":[" + items + "]}");

        // ---- 状态文件：一整份 JSON，不是 NDJSON ---------------------------
        //
        // stdout 那条流是 NDJSON（一行一个对象），但状态文件要的是**一份完整
        // 的 JSON 文档** —— 界面打开就读，不必自己按行拼。所以这里单独拼一份：
        // 类型 + 时间戳 + 全部会话 + 记录根/导出目录等固定信息。
        //
        // 固定信息也要放进去：界面光拿 sessions 是不知道导出到哪儿的，而它要显示。
        // profilesPath 尤其重要 —— 界面要靠它判断"正在跑的这个服务用的是不是
        // 我这份配置"（复用别人的服务时两边的 recordRoot 可能完全不同）。
        // 这两个字符串同样来自 doc，会被 reload 改写，所以在锁里抄出来。
        std::string statusRecordRoot;
        std::string statusExportDir;
        {
            std::lock_guard<std::mutex> lk(docMutex);
            statusRecordRoot = doc.recordRoot;
            statusExportDir = doc.exportDir;
        }
        PublishStatusFile(items, statusRecordRoot, statusExportDir, opt.profilesPath);

        // ---- 导出进度/结果事件 -------------------------------------------
        // 界面不能靠"对比两次 sessions 快照"来判断导出结束了没有 —— 那需要
        // 记住上一帧状态，而且中间态（准备->拼接->换封装）会漏掉。服务这边
        // 直接在有变化时补一条事件，界面只认事件就行。
        for (auto& s : [&]() -> std::vector<Session*> {
                 // 拷一份指针出来再遍历：ExportState 里有锁也很快，但不想在
                 // 持有 sessionsMutex 的时候跨进 Session 内部（锁序说不清）。
                 std::lock_guard<std::mutex> lk(sessionsMutex);
                 std::vector<Session*> out;
                 out.reserve(sessions.size());
                 for (auto& p : sessions) out.push_back(p.get());
                 return out;
             }()) {
            const ExportStatus es = s->ExportState();
            const auto it = lastExport.find(es.jobId);
            const ExportStatus prev = it == lastExport.end() ? ExportStatus{} : it->second;

            // jobId 从 0 变成 1 之后的第一次也要报，所以用 == 比较整个可见状态
            const bool same = it != lastExport.end() && prev.running == es.running &&
                              prev.ok == es.ok && prev.failed == es.failed &&
                              prev.phase == es.phase && prev.outPath == es.outPath;
            if (!same) {
                lastExport[es.jobId] = es;
                LogStatusLine("{\"type\":\"export_result\",\"id\":\"" + JsonEscape(s->Id()) +
                              "\",\"name\":\"" + JsonEscape(s->Name()) + "\",\"running\":" +
                              (es.running ? "true" : "false") + ",\"ok\":" +
                              (es.ok ? "true" : "false") + ",\"failed\":" +
                              (es.failed ? "true" : "false") + ",\"phase\":\"" +
                              JsonEscape(es.phase) + "\",\"outPath\":\"" +
                              JsonEscape(es.outPath) + "\",\"detail\":\"" +
                              JsonEscape(es.detail) + "\",\"segments\":" +
                              std::to_string(es.segmentCount) + ",\"durationSeconds\":" +
                              std::to_string(es.durationSeconds) + ",\"bytes\":" +
                              std::to_string(static_cast<unsigned long long>(es.bytes)) +
                              ",\"elapsedMs\":" +
                              std::to_string(static_cast<long long>(es.elapsedMs)) + "}");
            }
        }
    }

    // 电平表线程先收工：它每 33ms 抓一次 sessionsMutex，下面要动 sessions。
    meterStop.store(true, std::memory_order_release);
    if (meterThread.joinable()) meterThread.join();

    LogLine("{\"type\":\"stopping\"}");
    // 先等后台重建线程收尾：它们手里可能正攥着一个 Session（还没挂进表里），
    // 或者正在 Stop() 里等编码器把最后一片 flush 完。不等的话退出时会和它们
    // 抢同一批文件句柄。
    {
        std::lock_guard<std::mutex> lk(reloadMutex);
        for (auto& f : reloadThreads) {
            if (f.valid()) f.wait();
        }
        reloadThreads.clear();
    }
    // 再停会话：每个 Session 退出时会 Finish() 掉自己那个 MfEncoder，让当前
    // 分片的索引写完。顺序不能反 —— 先 CloseHandle(mutex) 只是放锁，
    // 不影响这里，但会话必须有机会跑完收尾。
    {
        std::lock_guard<std::mutex> lk(sessionsMutex);
        for (auto& s : sessions) s->Stop();
    }
    LogLine("{\"type\":\"bye\"}");

    // 状态文件也收工：让最后一份状态落盘，界面不会读到"停在很久以前"的旧状态。
    StopStatusMirror();

    timeEndPeriod(1);
    CloseHandle(mutex);
    return 0;
}

} // namespace

namespace {

// ---------------------------------------------------------------------------
// 一次性查询的实现（--status / --list-windows / --list-monitors 三个老模式）
//
// 抽成"返回字符串"而不是"直接打印"：编成 exe 时打印出去，编成 DLL 时交给
// JS 侧拿去用 —— 同一份逻辑，两个出口。
// ---------------------------------------------------------------------------
std::string BuildStatusJson(const std::string& session) {
    bool exists = false;
    HANDLE m = AcquireServiceMutex(exists);
    if (m) CloseHandle(m);
    return "{\"type\":\"service_status\",\"running\":" +
           std::string(exists ? "true" : "false") + ",\"session\":\"" +
           JsonEscape(session) + "\"}";
}

std::string BuildWindowsJson(const std::vector<HWND>& forceInclude = {}) {
    const auto windows = EnumerateWindows(forceInclude);
    std::string out = "{\"type\":\"windows\",\"count\":" + std::to_string(windows.size()) + "}\n";
    for (const auto& w : windows) {
        char buf[1024];
        std::snprintf(buf, sizeof(buf),
                      "{\"hwnd\":%u,\"title\":\"%s\",\"class\":\"%s\","
                      "\"process\":\"%s\",\"pid\":%u,"
                      "\"left\":%d,\"top\":%d,\"width\":%d,\"height\":%d,"
                      "\"foreground\":%s,\"minimized\":%s}",
                      // hwnd 一律按**无符号 32 位**输出：HWND 本身就是 32 位句柄，
                      // 按 64 位输出会被符号扩展成 1.8e19 那种值，JS 的 Number
                      // 超过 2^53 就不精确了 —— 界面拿它存进配置（match.hwnd）
                      // 再发回来，两边对不上，"换成同一个程序的另一个窗口"就失效。
                      static_cast<unsigned>(static_cast<uint32_t>(
                          reinterpret_cast<uintptr_t>(w.hwnd))),
                      JsonEscape(w.title).c_str(), JsonEscape(w.className).c_str(),
                      JsonEscape(w.processName).c_str(), w.pid, w.left, w.top, w.width,
                      w.height, w.foreground ? "true" : "false",
                      w.minimized ? "true" : "false");
        out += buf;
        out += '\n';
    }
    if (!out.empty() && out.back() == '\n') out.pop_back(); // 调用方自己补换行
    return out;
}

std::string BuildMonitorsJson() {
    const auto monitors = EnumerateMonitors();
    std::string out;
    for (const auto& m : monitors) {
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "{\"type\":\"monitor\",\"index\":%u,\"name\":\"%s\","
                      "\"left\":%d,\"top\":%d,\"width\":%u,\"height\":%u,"
                      "\"primary\":%s}",
                      m.index, JsonEscape(m.name).c_str(), m.left, m.top, m.width, m.height,
                      m.primary ? "true" : "false");
        out += buf;
        out += '\n';
    }
    if (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

int ServiceMainImpl(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);

    // 崩溃现场。0xC0000409 参数 7 就是 FAST_FAIL_FATAL_APP_EXIT ——
    // 说白了是 std::terminate 被叫了（未捕获的 C++ 异常、noexcept 里抛、
    // 或者某个线程对象在 joinable 状态下被销毁）。默认行为是静悄悄 abort，
    // 什么都不留；挂一个 handler 至少能把"当前有没有异常在飞"打出来。
    std::set_terminate([]() {
        const char* what = "（没有正在传播的异常：多半是某个线程对象在 joinable 状态被销毁）";
        std::string msg;
        if (std::current_exception()) {
            try {
                std::rethrow_exception(std::current_exception());
            } catch (const std::exception& e) {
                msg = std::string("（std::exception）") + e.what();
            } catch (...) {
                msg = "（非 std::exception 的异常）";
            }
            what = msg.c_str();
        }
        // stderr 被重定向成管道时是缓冲的，进程 abort 就丢了；直接写文件最稳。
        std::fprintf(stderr, "[fatal] std::terminate: %s\n", what);
        std::fflush(stderr);
        if (FILE* f = std::fopen("temp\\proofs\\terminate.txt", "a")) {
            std::fprintf(f, "[fatal] std::terminate: %s\n", what);
            std::fclose(f);
        }
        std::abort();
    });

    Options opt;
    if (!ParseCommandLine(argc, argv, opt)) {
        PrintUsage();
        return 2;
    }

    // ---- --status：只探测 ------------------------------------------------
    if (opt.statusOnly) {
        std::printf("%s\n", BuildStatusJson(opt.session).c_str());
        return QueryServiceRunning() ? 0 : 1;
    }

    // ---- --list-windows：列出可采集的顶层窗口 --------------------------
    if (opt.listWindows) {
        std::printf("%s\n", BuildWindowsJson(opt.includeHwnds).c_str());
        return 0;
    }

    // ---- --list-monitors：列出可采集的显示器 ----------------------------
    // 跟 --list-windows 一样是「跑一次就退」的枚举模式：界面点开下拉框时
    // 起一个临时实例问一遍，不打扰常驻服务，也不会去抢它的互斥体。
    // 序号与 ScreenCapture::Init(outputIndex) 用的是同一套排列
    // （CollectAllOutputs：只数已连接的输出，按上→下、左→右排）。
    if (opt.listMonitors) {
        std::printf("%s\n", BuildMonitorsJson().c_str());
        return 0;
    }

    // ---- --guard：守护模式 ------------------------------------------------
    // 必须排在获取服务互斥体**之前**：守护的职责是"服务没了就拉起它"，它自己
    // 绝不能持有那个互斥体 —— 否则服务死了互斥体也不销毁，守护永远看到
    // "服务还活着"，一次都不会重启。
    if (opt.guardian) {
        return RunGuardianMode(opt.profilesPath);
    }

    // ---- 服务互斥体 ------------------------------------------------------
    bool alreadyRunning = false;
    HANDLE mutex = AcquireServiceMutex(alreadyRunning);
    if (!mutex) {
        LogError("mutex_failed", "无法创建服务互斥体。");
        return 4;
    }
    if (alreadyRunning) {
        LogError("already_running",
                 "已经有一个采集服务在运行了（会话 " + opt.session +
                     "）。界面端直接连接即可，不需要重复启动。");
        CloseHandle(mutex);
        return 5;
    }

    // ---- 多配置模式 ------------------------------------------------------
    // 注意：互斥体在上面**已经获取过一次**了，这里绝对不能再获取第二次 ——
    // 同一进程对同一个命名互斥体调两次 CreateMutex，第二次必然返回
    // ERROR_ALREADY_EXISTS，于是服务会把自己刚创建的互斥体当成"别人在跑"，
    // 永远起不来（这个坑真踩过，诊断输出是 handle 不同但 gle=183）。
    if (!opt.profilesPath.empty()) {
        return RunProfilesMode(opt, mutex);
    }

    // ---- 采集初始化 ------------------------------------------------------
    // 把系统定时器精度提到 1ms。
    // 默认精度约 15.6ms，而我们要按 16.67ms 的节奏出帧；用 Sleep 去对齐时
    // 这么大的粒度会让实际间隔抖动到 30ms 左右，帧率直接腰斩。
    // 退出时（下面的 timeEndPeriod）再还回去，不会长期占用。
    timeBeginPeriod(1);

    ScreenCapture capture;
    // 采样超时给得很短：AcquireNextFrame 在有新帧时会立即返回，
    // 桌面静止时我们希望尽快回到循环里去重复发布最后一帧。
    // 用默认的 200ms 会让静止时的发布节奏掉到个位数 fps。
    if (!capture.Init(opt.output, 2)) {
        LogError("capture_init_failed",
                 "无法初始化桌面复制（显示器 " + std::to_string(opt.output) +
                     "）。可能原因：显示器序号不存在、正被独占全屏程序占用、"
                     "运行在远程桌面会话里、或显卡驱动不支持 DXGI 桌面复制。");
        CloseHandle(mutex);
        timeEndPeriod(1);
        return 3;
    }

    // ---- 共享内存帧环 ----------------------------------------------------
    FrameRing ring;

    // 本路预览自己的采集缓冲。Capture() 会把像素拷进来，返回后这帧归我们，
    // 不再指向采集器内部的 staging（那块内存下一次采集就被改写）。
    std::vector<uint8_t> previewBuf;

    if (!ring.Open(opt.session, capture.Width(), capture.Height())) {
        LogError("ring_create_failed", ring.LastError());
        CloseHandle(mutex);
        timeEndPeriod(1);
        return 4;
    }
    ring.SetMode(capture.Width(), capture.Height(), 0);

    LogLine("{\"type\":\"service_started\",\"session\":\"" + JsonEscape(opt.session) +
            "\",\"pid\":" + std::to_string(GetCurrentProcessId()) +
            ",\"width\":" + std::to_string(capture.Width()) +
            ",\"height\":" + std::to_string(capture.Height()) +
            ",\"attached\":" + (ring.Attached() ? "true" : "false") + "}");

    // 先按固定消费者 pid 跑一会儿（--once / 无界面自检用）
    if (opt.consumerPid != 0) {
        ring.ForceConsumer(opt.consumerPid);
        LogLine("{\"type\":\"consumer_forced\",\"pid\":" + std::to_string(opt.consumerPid) + "}");
    }

    const auto frameInterval = opt.fps > 0.0
                                   ? std::chrono::duration<double>(1.0 / opt.fps)
                                   : std::chrono::duration<double>(0.0);

    // 桌面静止时的重复发布节奏。
    //
    // DXGI 桌面复制只在桌面真的有变化时才返回新帧。如果用户就盯着预览窗口
    // 不动，桌面可能长时间保持静止，采集端一帧都不发，界面就停在一张静止
    // 画面上 —— 看起来像"只渲染了一帧"。
    //
    // 所以桌面静止时按这个节奏把最后那一帧重新发布一遍（不重新拷像素，
    // 只是把 counter 往前推），让下游始终有稳定的帧流。
    // 桌面静止时的重复发布节奏：没有显式上限时用 60fps。
    const auto republishInterval =
        opt.fps > 0.0 ? std::chrono::duration<double>(1.0 / opt.fps)
                      : std::chrono::duration<double>(1.0 / 60.0);

    auto lastPublish = std::chrono::steady_clock::now() - std::chrono::hours(1);
    auto lastRepublish = std::chrono::steady_clock::now();
    auto lastConsumerCheck = std::chrono::steady_clock::now();
    uint64_t framesInWindow = 0;
    uint64_t framesTotal = 0;
    double   fpsMeasured = 0.0;
    // 诊断用：窗口内实际发布间隔、每轮 Capture() 的耗时
    std::chrono::steady_clock::time_point firstPublish{};
    uint64_t loopsInWindow = 0;
    double   captureNsInWindow = 0.0;
    bool     connected = false;
    bool     reportedNoConsumer = false;
    int      statusTicks = 0;
    const int statusTicksNeeded = opt.once ? 2 : 0;

    // 状态窗口的起点单独记：fps 必须按**实际经过的时间**算。
    // 之前是假定"每满 1 秒就统计一次"，但 --once 的提前退出会让窗口变短，
    // 于是 fps 被算成 200+ 这种虚高值。
    auto lastStatus = lastConsumerCheck;

    // 每秒（或 --once 下每约 200ms）上报一次状态。
    // 抽成 lambda 是因为下面有两条路径都会 continue，状态统计必须两条都覆盖到 ——
    // 原来只在"真的采集到新帧"那条路径上统计，桌面静止时永远不会输出状态。
    auto reportStatusIfDue = [&](const std::chrono::steady_clock::time_point& now) -> bool {
        if (now - lastStatus < std::chrono::seconds(1)) return false;

        const double secs = std::chrono::duration<double>(now - lastStatus).count();
        fpsMeasured = secs > 0 ? static_cast<double>(framesInWindow) / secs : 0.0;
        // 平均发布间隔与实际计算出的 fps 对照：两者不一致就说明统计窗口不对齐
        const double gapMs = framesInWindow > 1
            ? (std::chrono::duration<double, std::milli>(now - firstPublish).count() /
               static_cast<double>(framesInWindow - 1))
            : 0.0;
        const double captureMs = loopsInWindow > 0
            ? captureNsInWindow / static_cast<double>(loopsInWindow) / 1e6 : 0.0;
        framesInWindow = 0;
        loopsInWindow = 0;
        captureNsInWindow = 0.0;
        lastStatus = now;

        char buf[768];
        std::snprintf(buf, sizeof(buf),
                      "{\"type\":\"status\",\"fps\":%.2f,\"frames\":%llu,"
                      "\"dropped\":%llu,\"width\":%u,\"height\":%u,"
                      "\"modeChanges\":%u,\"captureErrors\":%u,\"connected\":%s,"
                      "\"consumerPid\":%u,\"gapMs\":%.2f,\"captureMs\":%.2f}",
                      fpsMeasured,
                      static_cast<unsigned long long>(framesTotal),
                      static_cast<unsigned long long>(ring.Dropped()),
                      capture.Width(), capture.Height(),
                      static_cast<unsigned>(ring.ModeChanges()),
                      static_cast<unsigned>(capture.ErrorCount()),
                      connected ? "true" : "false",
                      ring.ConsumerPid(),
                      gapMs,
                      captureMs);
        LogLine(buf);

        return opt.once && statusTicks++ >= statusTicksNeeded;
    };

    // 没有消费者时不去抢 GPU 带宽：先等着。
    // 关键点：等待期间**不能**去检查 ConsumerAlive()——此时 consumerPid 还是 0，
    // 检查会一直失败，导致永远进不了"已连接"状态。消费者是否存在，只由
    // WaitForConsumer()（即 header.consumerPid != 0）判断。
    while (g_running) {
        if (!connected) {
            if (!ring.WaitForConsumer(200)) {
                if (opt.once && ++statusTicks > 15) {
                    // --once：等不到消费者也上报一次状态后退出
                    std::printf("{\"type\":\"status\",\"fps\":0,\"frames\":0,\"dropped\":0,"
                                "\"width\":%u,\"height\":%u,\"connected\":false}\n",
                                capture.Width(), capture.Height());
                    break;
                }
                if (!reportedNoConsumer) {
                    reportedNoConsumer = true;
                    LogLine("{\"type\":\"waiting_for_consumer\"}");
                }
                continue;
            }
            connected = true;
            reportedNoConsumer = false;
            lastConsumerCheck = std::chrono::steady_clock::now();
            ring.SetMode(capture.Width(), capture.Height(), 0);
            LogLine("{\"type\":\"consumer_connected\",\"pid\":" +
                    std::to_string(ring.ConsumerPid()) + "}");
        } else {
            // 已经连上了，每秒确认一次消费者还活着
            const auto now = std::chrono::steady_clock::now();
            if (now - lastConsumerCheck > std::chrono::milliseconds(1000)) {
                lastConsumerCheck = now;
                if (!ring.ConsumerAlive()) {
                    connected = false;
                    LogLine("{\"type\":\"consumer_lost\"}");
                    continue;
                }
            }
        }

        // 生产者每轮都替消费者刷新一次心跳。
        // 界面侧的 addon.poll() 也会写心跳，但 poll 只在帧循环里跑；
        // 界面最小化或暂停时不应该被误判成"消费者死了"。
        ring.TouchConsumerHeartbeat();

        // ---- 采集一帧 ----------------------------------------------------
        // CaptureInto 要求调用方给 vector 缓冲：它把像素同步拷进来，
        // 返回后这帧就完全归我们所有，不再指向采集器内部的 staging。
        // 走 vector 版还能让采集器把「最近一张」和我们的缓冲直接交换，
        // 省掉每拍一次整屏拷贝（见 ScreenCapture::SwapLastFrame）。
        FrameView view{};
        const auto captureT0 = std::chrono::steady_clock::now();
        CaptureStatus st = capture.CaptureInto(view, previewBuf);
        captureNsInWindow += std::chrono::duration<double, std::nano>(
            std::chrono::steady_clock::now() - captureT0).count();
        ++loopsInWindow;

        if (st == CaptureStatus::NoChange) {
            // 桌面这一瞬间没有变化。但仍然按目标帧率重复发布最后那一帧，
            // 保证界面拿到连续帧流（否则预览看起来就是一张静止图片）。
            const auto now = std::chrono::steady_clock::now();
            if (now - lastRepublish >= republishInterval) {
                lastRepublish = now;
                // 把采集端留存的「最近一张」重放到我们自己的缓冲，
                // 内容和上一帧相同（staging 早就 Unmap 了，不能再指它）。
                uint32_t rw = 0, rh = 0;
                int64_t rq = 0;
                bool ok = false;
                {
                    std::lock_guard<std::mutex> cap(capture.Mutex());
                    ok = capture.AcquireReplayLocked(previewBuf.data(), previewBuf.size(), rw, rh, rq);
                }
                if (ok && ring.RepublishLast(previewBuf.data(), rw * 4u)) {
                    if (framesInWindow == 0) firstPublish = now;
                    ++framesInWindow;
                    ++framesTotal;
                    lastPublish = now;
                }
            }
            if (reportStatusIfDue(now)) break;
            continue;
        }

        if (st == CaptureStatus::AccessLost || st == CaptureStatus::Error) {
            capture.ReleaseFrame();
            LogLine("{\"type\":\"capture_reset\"}");

            // UAC 弹窗 / 锁屏 / 分辨率切换 / 切换独显之后重建
            if (!capture.Init(opt.output)) {
                LogError("capture_reset_failed", "桌面复制重建失败，1 秒后重试。");
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                continue;
            }
            if (ring.NeedsGrow(capture.Width(), capture.Height())) {
                LogLine("{\"type\":\"ring_resize\",\"width\":" +
                        std::to_string(capture.Width()) + ",\"height\":" +
                        std::to_string(capture.Height()) + "}");
                if (!ring.Open(opt.session, capture.Width(), capture.Height())) {
                    LogError("ring_resize_failed", ring.LastError());
                    break;
                }
                connected = false; // 重新握手
            }
            ring.SetMode(capture.Width(), capture.Height(), 0);
            continue;
        }

        // ---- 帧率限制 ----------------------------------------------------
        // 采到了一帧新内容，但还没到目标发布时间：**睡到那一刻再发**。
        //
        // 这里有两个都踩过的坑：
        //   1) 原来是 `continue`（把刚采到的帧丢掉、回去重新轮询），
        //      结果是每轮 ~3.9ms（AcquireNextFrame 的 2ms 超时 + 拷贝）
        //      去凑 16.67ms，只能凑到 5 轮 ≈ 19.5ms —— 实测 19ms / 52fps，
        //      而且真正发出去的还是那份旧数据（新帧已经被丢掉重采了）。
        //   2) 改成"直接发"又会完全不设上限：连续有新帧时它跑到 ~90fps。
        // 所以要睡到点、然后用手里这帧发出去。这一帧最多等了一个间隔
        // （16.67ms），预览画面看不出来。
        const auto now = std::chrono::steady_clock::now();
        if (frameInterval.count() > 0.0 && (now - lastPublish) < frameInterval) {
            // frameInterval 是 duration<double>，直接相加会得到另一种 time_point；
            // 先算剩余时长、再转回 steady_clock 的原生刻度。
            const auto remain = frameInterval - (now - lastPublish);
            SkipUntil(std::chrono::steady_clock::now() +
                      std::chrono::duration_cast<
                          std::chrono::steady_clock::duration>(remain));
        }

        // 记时间点必须用**睡醒之后**的时刻。
        // 之前这里复用了睡觉前取的 now，于是 lastPublish 比真实发布时刻早了
        // 一个 skip（约 5.7ms），下一轮一算又"没到点"，帧率就顶在 ~90 不降。
        const auto publishedAt = std::chrono::steady_clock::now();

        if (ring.Publish(view.pixels, view.stride, view.width, view.height,
                         static_cast<uint64_t>(view.presentQpc))) {
            lastPublish = publishedAt;
            // 重复发布的节流点也跟着走，避免"刚发过真帧又立刻补一帧重复的"
            lastRepublish = publishedAt;
            if (framesInWindow == 0) firstPublish = publishedAt;
            ++framesInWindow;
            ++framesTotal;
        }

        // 不用再 ReleaseFrameKeepLast：Capture() 已经在拷完像素之后自己
        // 释放了 duplication 帧，view.pixels 现在指向 previewBuf（我们自己的内存）。

        // ---- 每秒状态 ----------------------------------------------------
        if (reportStatusIfDue(publishedAt)) break;
    }

    LogLine("{\"type\":\"bye\"}");
    if (mutex) CloseHandle(mutex);
    timeEndPeriod(1);
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// 对外入口（ServiceEntry.h）
//
// 这一层很薄，只做三件事：
//   1. 给 ServiceMain 套一层"异常不许逃出去"的壳 —— 异常一旦跑出这个边界
//      就会变成未捕获异常，进程直接 abort（0xC0000409），连收尾都做不了；
//   2. 提供 RequestServiceStop，让调用方能在不杀进程的前提下要它退出；
//   3. 把三个一次性查询暴露成"返回字符串"。
//
// 注意 g_running 是**进程级**的：同一个进程里跑了两个 ServiceMain（正常不会，
// 互斥体会挡住第二个）时，一个停机会连坐另一个。服务本身就是独立进程，
// 不会出现这种情形。
// ---------------------------------------------------------------------------
namespace shadow {

int ServiceMain(int argc, char** argv) {
    try {
        return ServiceMainImpl(argc, argv);
    } catch (const std::exception& e) {
        // 走到这儿说明有未预期的异常。打出去（stdout 是 NDJSON 通道，
        // 别混进非 JSON 行）——界面那边会把它当服务崩溃处理。
        LogError("service_crashed", std::string("服务内部异常：") + e.what());
        return 1;
    } catch (...) {
        LogError("service_crashed", "服务内部异常（非 std::exception）");
        return 1;
    }
}

void RequestServiceExit() {
    // 写停机哨兵：守护下一轮巡检看到它就自己退出、且不再拉服务。
    WriteShutdownSentinel();
    // 置退出标志并立刻唤醒主循环（不必等 100ms 节拍）。
    g_running.store(false, std::memory_order_release);
    if (g_wakeEvent) SetEvent(g_wakeEvent);
}

void RequestServiceStop() {
    // 置停止标志 + 叫醒主循环。收尾（停会话、Finalize 分片、落状态文件）
    // 仍然由 ServiceMain 自己按正常路径做完，所以这里不等待、不杀线程。
    g_running.store(false, std::memory_order_release);
    // wakeEvent 是 RunProfilesMode 里建的（栈上变量），单路旧模式没有它。
    // 这里不能直接摸那个变量，所以退一步：把标准输入的等待也用不上 ——
    // 主循环最坏 100ms 就会自己醒来一次看到 g_running 变了。
    //
    // 100ms 的收尾延迟对"关界面/停服务"这种操作完全无感，换来的好处是
    // 这一层不需要持有 RunProfilesMode 内部的任何句柄。
}

bool QueryServiceRunning() {
    bool exists = false;
    HANDLE m = AcquireServiceMutex(exists);
    if (m) CloseHandle(m);
    return exists;
}

std::string QueryStatusJson() { return BuildStatusJson(kServiceSessionId()); }

std::string QueryWindowsJson() { return BuildWindowsJson(); }

std::string QueryMonitorsJson() { return BuildMonitorsJson(); }

} // namespace shadow

