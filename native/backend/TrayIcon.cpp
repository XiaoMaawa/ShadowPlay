// TrayIcon.cpp — 采集服务的系统托盘图标，见 TrayIcon.h
#include "TrayIcon.h"

#include "GuardianMode.h"  // WriteShutdownSentinel / ShutdownRequested
#include "GuiNotify.h"      // NotifyGuiShutdown（退出时请界面一起关）
#include "RingNames.h"      // GuiPipeName（判断界面在不在跑）
#include "StatusFile.h"    // StatusFilePath（用来推导 logs 目录）

#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace shadow {
namespace {

namespace fs = std::filesystem;

// 托盘是"看不见的 UI"：图标没出来、右键没反应，界面上什么都看不到。
// 所以给几个关键节点写一行日志（%TEMP%\shadow-play\app\logs\tray.log），
// 下次出问题能直接看出是"没挂上图标"还是"挂上了但菜单没弹"。
void TrayLog(const std::string& msg) {
    std::error_code ec;
    const fs::path logs = fs::u8path(StatusFilePath()).parent_path().parent_path() / "logs";
    fs::create_directories(logs, ec);
    std::ofstream f(logs / "tray.log", std::ios::app);
    if (!f) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char ts[32]{};
    std::snprintf(ts, sizeof(ts), "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    f << ts << " " << msg << "\n";
}

// 托盘回调消息（自定义，不能和系统 WM_* 撞）。
constexpr UINT WM_TRAYICON = WM_APP + 1;

// 菜单命令 ID。
enum { ID_TRAY_OPEN = 1, ID_TRAY_EXIT = 2 };

NOTIFYICONDATAW g_nid{};
HWND g_hwnd = nullptr;
UINT g_taskbarCreated = 0;
std::string g_profilesPath;  // UTF-8 绝对路径

// 前置声明：MakeAppIcon 定义在文件后面，TrayWndProc / TrayThread 先用到它。
HICON MakeAppIcon();

// 主进程里定义的「请求服务退出」（写哨兵 + 置 g_running=false）。
// 声明见 TrayIcon.h（namespace shadow，外部链接）。

// 造一个蓝底白圆图标（32x32），和系统通用图标区分开，一眼认得出是录制服务。
HICON MakeAppIcon() {
    const int sz = 32;
    HDC hdc = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP hbmp = CreateCompatibleBitmap(hdc, sz, sz);
    const HGDIOBJ old = SelectObject(mem, hbmp);

    const HBRUSH brBlue = CreateSolidBrush(RGB(33, 118, 243));
    const HBRUSH brOld = static_cast<HBRUSH>(SelectObject(mem, brBlue));
    Rectangle(mem, 0, 0, sz, sz);
    const HBRUSH brWhite = CreateSolidBrush(RGB(245, 245, 245));
    SelectObject(mem, brWhite);
    Ellipse(mem, 7, 7, sz - 7, sz - 7);
    SelectObject(mem, brOld);
    DeleteObject(brBlue);
    DeleteObject(brWhite);

    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, hdc);

    // 掩码：1x1 黑位图 → 整个图标都可见。
    HBITMAP hMask = CreateBitmap(1, 1, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = hbmp;
    ii.hbmMask = hMask;
    const HICON icon = CreateIconIndirect(&ii);
    DeleteObject(hbmp);
    DeleteObject(hMask);
    return icon;
}

// ---------------------------------------------------------------------------
// 拉起 / 唤起 Electron 主界面
//
// 【两条硬要求，都是踩出来的】
//   1. 绝不能经过 cmd.exe。以前这里是 `cmd.exe /c node launch.mjs >nul 2>&1`：
//      cmd 是控制台程序，CreateProcess 一给就是屏幕上一个黑框，`>nul` 只挡
//      输出、挡不住窗口。用户连点几下托盘，桌面上就摆满黑框。现在直接起
//      node.exe，并且用 DETACHED_PROCESS 彻底不要控制台。
//   2. 已经开着 / 正在启动就别再拉。以前每次点击都无条件 CreateProcess，
//      连点 N 次就起 N 个启动进程（各自还会再去 spawn 一个 electron），
//      其中只有第一个能拿到单实例锁，剩下的全在抢锁后退化成一堆无主进程。
//
// 界面在不在跑，看的是反向通知管道（界面 createWindow 之后立刻监听，见
// app/main.cjs 的 startGuiPipe）。比按进程名 / 窗口标题找可靠得多：机器上
// 的 electron.exe 可能是编辑器的、也可能是别的项目，按名字判断必然误判。
// ---------------------------------------------------------------------------

// 上一次成功拉起界面的时刻（GetTickCount，0 = 从没拉过）。只在托盘线程里读写。
DWORD g_guiLaunchedAt = 0;

// 双击保护窗口：只防"手抖连点两下"（第二下要赶在 node 把 gui.pid 写出来之前）。
// 以前这里是 20 秒的"启动宽限"，用它来代表"正在启动" —— 这是错的：用户在
// 20 秒内开界面、叉掉、再点托盘，会被当成"还在启动"而忽略，界面再也起不来。
// 现在"在不在启动"看 gui.pid 里的进程是否活着，时间窗只负责防连点。
constexpr DWORD kGuiClickDebounceMs = 2000;

/**
 * 界面进程还活着吗（读 %TEMP%\shadow-play\app\gui.pid 里的 pid）。
 *
 * 【为什么需要这个文件】界面"正在启动"和"已经被用户叉掉"在管道上是一回事：
 * 都连不上。只看时间窗口判断不了 —— 用户在窗口里开完就关，再点托盘时时间窗
 * 还没过，界面就拉不起来了。
 *
 * gui.pid 由两边写、同值：launch.mjs spawn 完 electron 立刻写（省掉"还没跑
 * 到主进程"的空窗），界面自己启动后再写一次；正常退出时删掉。
 *
 * 进程被强杀 / 崩溃时文件会残留，所以**必须**用 pid 是否存活来判，不能只看
 * 文件在不在；pid 复用的情况极罕见，最坏后果也只是一次点击被忽略。
 */
bool GuiProcessAlive() {
    std::error_code ec;
    const fs::path appDir = fs::u8path(StatusFilePath()).parent_path().parent_path();
    std::ifstream f(appDir / "gui.pid");
    if (!f) return false;
    long long pid = 0;
    f >> pid;
    if (pid <= 0) return false;

    HANDLE h = ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!h) return false;
    const DWORD w = ::WaitForSingleObject(h, 0);
    ::CloseHandle(h);
    return w == WAIT_TIMEOUT;  // 还没退出 = 活着
}

/**
 * 给界面发一条 NDJSON 命令。
 *
 * @param existed 管道是否存在过（存在 = 界面进程确实活着，哪怕这一句没写出去）。
 * @return 是否真的写成功了。
 */
bool SendGuiCommand(const std::string& line, bool* existed) {
    const std::string name = GuiPipeName();
    if (existed) *existed = false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        HANDLE pipe =
            ::CreateFileA(name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            if (existed) *existed = true;
            DWORD written = 0;
            const BOOL ok = ::WriteFile(pipe, line.data(), static_cast<DWORD>(line.size()),
                                        &written, nullptr);
            if (ok) ::FlushFileBuffers(pipe);
            ::CloseHandle(pipe);
            return ok != FALSE;
        }
        const DWORD err = ::GetLastError();
        if (err == ERROR_PIPE_BUSY) {
            // 界面正在处理上一条命令。托盘线程最多等 100ms，不能卡住菜单。
            if (!::WaitNamedPipeA(name.c_str(), 100)) {
                if (existed) *existed = true;
                return false;
            }
            continue;
        }
        if (err == ERROR_ACCESS_DENIED) {
            // 管道存在但打不开（完整性级别不同）：界面确实在跑，别再拉一个。
            if (existed) *existed = true;
            return false;
        }
        if (err != ERROR_FILE_NOT_FOUND) return false;
        // 管道不存在：界面没开，或者刚拉起还没建好。短等一下再试，
        // 但必须有上限 —— 这是托盘线程。
        ::Sleep(50);
    }
    return false;
}

/**
 * 找 node.exe。
 *
 * 服务是 launch.mjs 用 node 起的，那边会把 node 的绝对路径塞进环境变量
 * SHADOW_NODE_EXE（见 scripts/launch.mjs），优先用它 —— 对 nvm、便携版
 * Node 这些 PATH 里没有的场景才靠谱。没有才退回 PATH 和常见安装位置。
 */
std::wstring FindNodeExe() {
    constexpr DWORD kBuf = 2048;
    wchar_t buf[kBuf]{};

    DWORD n = ::GetEnvironmentVariableW(L"SHADOW_NODE_EXE", buf, kBuf);
    if (n > 0 && n < kBuf && ::GetFileAttributesW(buf) != INVALID_FILE_ATTRIBUTES) return buf;

    n = ::SearchPathW(nullptr, L"node.exe", nullptr, kBuf, buf, nullptr);
    if (n > 0 && n < kBuf && ::GetFileAttributesW(buf) != INVALID_FILE_ATTRIBUTES) return buf;

    // NVM for Windows 的符号链接目录。
    n = ::GetEnvironmentVariableW(L"NVM_SYMLINK", buf, kBuf);
    if (n > 0 && n < kBuf) {
        std::wstring p(buf);
        p += L"\\node.exe";
        if (::GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    }

    struct EnvPart { const wchar_t* env; const wchar_t* tail; };
    const EnvPart candidates[] = {
        { L"ProgramFiles", L"\\nodejs\\node.exe" },
        { L"ProgramFiles(x86)", L"\\nodejs\\node.exe" },
        { L"LOCALAPPDATA", L"\\Programs\\nodejs\\node.exe" },
    };
    for (const auto& c : candidates) {
        wchar_t dir[kBuf]{};
        const DWORD m = ::GetEnvironmentVariableW(c.env, dir, kBuf);
        if (m == 0 || m >= kBuf) continue;
        std::wstring p(dir);
        p += c.tail;
        if (::GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    }
    return std::wstring();
}

/**
 * 真正把界面拉起来：node.exe scripts/launch.mjs --bg。
 *
 * 拿 --bg 是因为托盘只负责"叫起来"，不需要盯着它：--bg 分支 spawn 完界面
 * 就自己退出，不会留下一个挂在后台的 node 父进程。
 */
bool LaunchGuiProcess(const fs::path& root) {
    const std::wstring node = FindNodeExe();
    if (node.empty()) {
        TrayLog("找不到 node.exe（SHADOW_NODE_EXE / PATH / Program Files 都没有），无法拉起界面");
        return false;
    }
    const fs::path script = root / L"scripts" / L"launch.mjs";
    if (!fs::is_regular_file(script)) {
        TrayLog("找不到 scripts\\launch.mjs，无法拉起界面");
        return false;
    }

    std::wstring cmd = L"\"" + node + L"\" \"" + script.wstring() + L"\" --bg";
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');

    // node 的 stdout/stderr 落到 artifacts\gui.log（没有控制台可写，也不能
    // 让 Node 对着无效句柄写 —— 那会变成未处理的 stream error）。
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    std::error_code ec;
    const fs::path logPath = root / L"native" / L"build" / L"artifacts" / L"gui.log";
    fs::create_directories(logPath.parent_path(), ec);
    HANDLE log = ::CreateFileW(logPath.wstring().c_str(), FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) {
        log = ::CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    if (log != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        // stdin 也给同一个句柄：--bg 那条路不读 stdin，给个有效句柄就够了。
        si.hStdInput = log;
        si.hStdOutput = log;
        si.hStdError = log;
    }

    PROCESS_INFORMATION pi{};
    // DETACHED_PROCESS：不要控制台。它和 CREATE_NO_WINDOW 互斥（后者会被
    // 忽略），所以这里只给前者 —— 给两个等于白发一个。
    const BOOL ok = ::CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr,
                                     log != INVALID_HANDLE_VALUE,
                                     DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr,
                                     root.wstring().c_str(), &si, &pi);
    if (log != INVALID_HANDLE_VALUE) ::CloseHandle(log);

    if (!ok) {
        TrayLog("拉起界面失败，GetLastError=" + std::to_string(::GetLastError()));
        return false;
    }
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return true;
}

// 托盘左键 / 菜单「打开主界面」：已经有界面就唤到前台，没有才拉一个。
void RequestOpenGui() {
    if (g_profilesPath.empty()) return;
    // profiles.json 所在目录就是项目根。
    const fs::path root = fs::u8path(g_profilesPath).parent_path();
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return;

    // 1. 界面在跑 —— 只把窗口叫到前台，绝不再起一个进程。
    bool existed = false;
    const bool sent = SendGuiCommand("{\"cmd\":\"show\"}\n", &existed);
    if (sent) {
        TrayLog("托盘：界面已在运行，已唤到前台");
        return;
    }
    if (existed) {
        // 管道在但写不进去（权限/忙）。界面确实开着，重复拉只会多一个抢锁进程。
        TrayLog("托盘：界面在运行但唤不出，跳过拉起");
        return;
    }

    // 2. 界面进程还在（刚拉起、管道还没建好）—— 忽略这次点击，等它自己起来。
    //    用户把界面叉掉之后 pid 就不再存活，这里会直接往下走到"拉一个"。
    if (GuiProcessAlive()) {
        TrayLog("托盘：界面正在启动，忽略本次点击");
        return;
    }

    // 3. 防连点：刚发起过启动，给 node 一点时间把 gui.pid 写出来。
    const DWORD now = ::GetTickCount();
    if (g_guiLaunchedAt != 0 && now - g_guiLaunchedAt < kGuiClickDebounceMs) {
        TrayLog("托盘：刚刚已经拉过一次，忽略本次点击");
        return;
    }

    // 4. 真的没开：拉起来，并记下时刻给下一次点击做去重。
    if (LaunchGuiProcess(root)) {
        g_guiLaunchedAt = now;
        TrayLog("托盘：已拉起主界面");
    }
}

// 请 Electron 主界面一起退出（托盘「退出」= 整个程序退出）。
//
// 【为什么走管道而不是去杀进程】
// 服务不认识界面的 pid：界面可能是用户用 npm start 起的、也可能是上次托盘
// 「打开主界面」拉起来的，还可能是别人手动起的另一个实例。按名字 taskkill
// 会误杀编辑器里的 electron，按窗口标题找又不可靠。而界面本来就在
// \\.\pipe\ShadowPlay.Gui.v1 上等着（见 app/main.cjs 的 startGuiPipe），
// 发一句话过去，让界面自己走正常的 app.quit() 关窗口 —— 干净、也不会杀错。
//
// 连不上就放弃：界面没在跑是完全正常的状态（用户关了窗口，服务继续录）。
void NotifyGuiQuit() {
    TrayLog(NotifyGuiShutdown("tray-exit") ? "已通知界面退出" : "界面没在运行，跳过退出通知");
}

// 干净退出：撤图标 → 立哨兵 → 让主循环收尾（守护看到哨兵就不拉了）。
void DoTrayExit() {
    TrayLog("菜单：点了退出");
    // 先请界面一起退：界面收到会自己 app.quit()，服务这边继续按正常流程收尾。
    // 顺序放在最前面 —— 万一通知那 300ms 里界面向服务发了命令，服务还在。
    NotifyGuiQuit();
    if (g_hwnd) Shell_NotifyIconW(NIM_DELETE, &g_nid);
    WriteShutdownSentinel();
    RequestServiceExit();
    if (g_hwnd) DestroyWindow(g_hwnd);  // → WM_DESTROY → 本线程消息循环退出
}

// 弹出右键菜单。用 TPM_RETURNCMD 直接在返回值里拿菜单项，不绕 WM_COMMAND。
void ShowTrayMenu(POINT pt) {
    TrayLog("菜单：右键弹出");
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_TRAY_OPEN, L"打开主界面");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"退出");

    // 【这两句是 TrackPopupMenu 的经典配套，缺一样菜单就不正常】
    //   · SetForegroundWindow：否则点菜单外面它不消失（菜单像黏住了一样）；
    //   · 菜单关掉后 PostMessage(WM_NULL)：把焦点从菜单状态里拉回来。
    // 也正因为要 SetForegroundWindow，这个窗口**不能**做成 HWND_MESSAGE ——
    // 消息专用窗口不是顶层窗口，永远成不了前台窗口，上面那句就是白调。
    SetForegroundWindow(g_hwnd);
    const UINT cmd =
        TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN,
                       pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(menu);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);

    if (cmd == ID_TRAY_EXIT) DoTrayExit();
    else if (cmd == ID_TRAY_OPEN) RequestOpenGui();
}

LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreated) {
        // 资源管理器重启过：图标会丢，重新挂上（别忘了再设一次版本）。
        if (g_hwnd) {
            Shell_NotifyIconW(NIM_ADD, &g_nid);
            Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
        }
        return 0;
    }

    if (msg == WM_TRAYICON) {
        // 【消息版本 v4 的坑 —— 之前右键没反应就是这里】
        // 设了 NOTIFYICON_VERSION_4 之后，回调参数的布局变了：
        //   wParam  低 16 位 = x、高 16 位 = y
        //   lParam  低 16 位 = **事件消息**（WM_CONTEXTMENU / NIN_SELECT / …）、
        //           高 16 位 = 图标 id
        // 也就是说鼠标消息在 LOWORD(lParam) 里，不在 lParam 本身。以前拿整个
        // lParam 去比 WM_RBUTTONUP，高 16 位还挂着图标 id，永远不相等 → 右键
        // 点了毫无反应。
        const UINT evt = LOWORD(lp);
        POINT pt{ static_cast<long>(static_cast<short>(LOWORD(wp))),
                  static_cast<long>(static_cast<short>(HIWORD(wp))) };

        if (evt == WM_CONTEXTMENU || evt == WM_RBUTTONUP) {
            // wParam 里没有坐标时（老版本行为）退回光标位置。
            if (pt.x == 0 && pt.y == 0) GetCursorPos(&pt);
            ShowTrayMenu(pt);
        } else if (evt == NIN_SELECT || evt == NIN_KEYSELECT || evt == WM_LBUTTONUP ||
                   evt == WM_LBUTTONDBLCLK) {
            // 左键单击/双击：把界面拉起来（托盘没有主窗口，点一下就是"回到 App"）。
            RequestOpenGui();
        }
        return 0;
    }

    // 兼容路径：菜单若走到 WM_COMMAND（某些输入法/键盘激活场景）
    if (msg == WM_COMMAND) {
        const int id = LOWORD(wp);
        if (id == ID_TRAY_EXIT) {
            DoTrayExit();
            return 0;
        }
        if (id == ID_TRAY_OPEN) {
            RequestOpenGui();
            return 0;
        }
        return 0;
    }

    if (msg == WM_DESTROY) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void TrayThread() {
    const wchar_t* cls = L"ShadowPlayTrayWnd";
    WNDCLASSW wc{};
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = cls;
    RegisterClassW(&wc);

    // 普通顶层窗口但**不带 WS_VISIBLE** → 存在且是顶层窗口，屏幕上看不见、
    // 任务栏里也没有。用它而不是 HWND_MESSAGE，是因为消息专用窗口成不了前台
    // 窗口，SetForegroundWindow 会失败，右键菜单点外面就不消失（见 ShowTrayMenu）。
    g_hwnd = CreateWindowW(cls, L"ShadowPlayTray", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr,
                           GetModuleHandleW(nullptr), nullptr);
    if (!g_hwnd) {
        TrayLog("创建托盘窗口失败，托盘不可用");
        return;
    }

    // TaskbarCreated 要在挂图标**之前**注册好，否则资源管理器重启的瞬间可能漏事件。
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = MakeAppIcon();
    wcscpy_s(g_nid.szTip, L"Shadow Play 录制服务");
    g_nid.uVersion = NOTIFYICON_VERSION_4;

    // NIM_ADD 会失败的情况真的存在（通知区域还没就绪、资源管理器刚重启）。
    // 失败就退避重试几次，不要一次失败就永远没有图标。
    bool added = false;
    for (int attempt = 0; attempt < 10; ++attempt) {
        added = Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
        if (added) break;
        Sleep(500);
    }
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
    TrayLog(std::string("图标已挂上=") + (added ? "是" : "否（重试 10 次仍失败）"));

    MSG m{};
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_nid.hIcon) DestroyIcon(g_nid.hIcon);
    g_nid.hIcon = nullptr;
}

} // namespace

void StartTrayIcon(const std::string& profilesPath) {
    g_profilesPath = profilesPath;
    TrayLog("托盘线程启动，profiles=" + profilesPath);
    std::thread(TrayThread).detach();
}

} // namespace shadow
