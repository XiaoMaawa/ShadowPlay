// WindowEnum.h — 顶层窗口枚举 + 匹配规则求值
//
// 两处用到：
//   1. GUI 的「选择窗口」下拉框（列出当前存在的窗口）
//   2. 配置里 kind == window 时，周期性解析出实际要录的那个 HWND
//
// 枚举规则（尽量贴近"用户能看到的窗口"）：
//   * 只取顶层窗口（EnumWindows 本身就是顶层）
//   * 默认过滤掉：不可见、最小化、无标题、WS_EX_TOOLWINDOW、
//     以及 shell 的隐形窗口类（Progman / WorkerW / Shell_TrayWnd 等）
//   * 带上进程名和 pid，匹配规则可以按进程名来

#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "Profiles.h"

namespace shadow {

struct WindowInfo {
    HWND        hwnd = nullptr;
    std::string title;      // UTF-8
    std::string className;  // UTF-8
    std::string processName; // 例如 notepad.exe（小写）
    uint32_t    pid = 0;
    // 窗口矩形（屏幕坐标，含边框）
    int32_t     left = 0;
    int32_t     top = 0;
    int32_t     width = 0;
    int32_t     height = 0;
    bool        minimized = false;
    bool        foreground = false;

    int64_t Area() const {
        return static_cast<int64_t>(width) * static_cast<int64_t>(height);
    }
};

// 枚举当前所有候选窗口。excludeClass 里的类名会被丢掉。
std::vector<WindowInfo> EnumerateWindows();

// 同上，但 forceInclude 里的 hwnd **无条件进列表**：不看最小化、不看尺寸、
// 不看 WS_EX_TOOLWINDOW。
//
// 为什么需要：界面那个"选择窗口"下拉框是给"这一路正在录哪个窗口"用的。
// 枚举为了不列垃圾窗口，会排掉最小化 / 小于 64x64 的窗口 —— 可窗口被最小化
// 之后服务**仍然握着它继续录**（WGC 抓的是那个 hwnd），于是下拉里找不到它，
// 用户点开编辑只能看到"请选择一个窗口"。把服务正在录的 hwnd 传进来，
// 下拉就能按 hwnd 认回它。
std::vector<WindowInfo> EnumerateWindows(const std::vector<HWND>& forceInclude);

// 按规则筛选。返回空表示"匹配不到" —— 调用方据此强制关闭该配置。
// 如果 rule.pattern 为空或规则非法（例如正则写错），也返回空。
std::vector<WindowInfo> MatchWindows(const std::vector<WindowInfo>& all,
                                     const WindowMatch& rule);

// 便利函数：枚举并筛选，直接拿到选中的那一个。找不到返回 false。
bool ResolveWindow(const WindowMatch& rule, WindowInfo& out);

// HWND → UTF-8 标题（窗口已销毁时返回空）
std::string WindowTitle(HWND hwnd);
std::string WindowClassName(HWND hwnd);
std::string ProcessNameOf(uint32_t pid);

// 这个窗口现在还是不是"能被录到东西的窗口"：存在、可见、pid 对得上。
//
// 【为什么不能只判 IsWindow】Chromium 系（浏览器、Electron、一堆国产客户端）
// 关掉一个窗口时经常**不销毁 HWND**，只是 ShowWindow(SW_HIDE)：句柄一直有效、
// IsWindow 一直为真、pid 也没变，可是合成器不再给它投帧。只判 IsWindow 的话
// 这一路会永远停在 live + 最后一帧上 —— 界面看着就是"窗口都关了，画面定住，
// 也不弹 NO SIGNAL"（用户实测报的就是这个）。
//
// 最小化**不算**丢失：IsWindowVisible 对最小化窗口仍然为真，而按设计锁定之后
// 就要一直录同一个窗口，最小化恢复后继续录，不能借此换源。
bool WindowPresent(HWND hwnd, uint32_t pid = 0);

// 取窗口矩形（含边框，屏幕坐标）
bool WindowRect(HWND hwnd, RECT& out);

} // namespace shadow
