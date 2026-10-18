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

// 取窗口矩形（含边框，屏幕坐标）
bool WindowRect(HWND hwnd, RECT& out);

} // namespace shadow
