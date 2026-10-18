// WindowEnum.cpp — 顶层窗口枚举 + 匹配规则求值

#include "WindowEnum.h"

#include <algorithm>
#include <cwctype>
#include <regex>

namespace shadow {
namespace {

// shell / 系统里那些"看得见但没人想录"的窗口类。
// 不排除的话下拉框里会塞满 Progman、WorkerW、任务栏、输入法候选框之类。
const char* const kSkipClasses[] = {
    "Progman",
    "WorkerW",
    "Shell_TrayWnd",
    "Shell_SecondaryTrayWnd",
    "Windows.UI.Core.CoreWindow",
    "DV2ControlHost",
    "MsgrIMEWindowClass",
    "SysShadow",
    "Button",
    "tooltips_class32",
    "TaskListThumbnailWnd",
    "ForegroundStaging",
    "MultitaskingViewFrame",
    "XamlExplorerHostIslandWindow",
    "Windows.Internal.Shell.TabProxyWindow",
};

bool IsSkippedClass(const std::string& cls) {
    for (const char* skip : kSkipClasses) {
        if (cls == skip) return true;
    }
    return false;
}

// UTF-16 → UTF-8
std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                           static_cast<int>(w.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out(static_cast<size_t>(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                          out.data(), need, nullptr, nullptr);
    return out;
}

// 读窗口标题。标题长度可能变化，所以先试一个小缓冲，装不下再放大。
std::string ReadWindowText(HWND hwnd) {
    const int len = ::GetWindowTextLengthW(hwnd);
    if (len <= 0) return {};
    std::wstring buf(static_cast<size_t>(len) + 1, L'\0');
    const int got = ::GetWindowTextW(hwnd, buf.data(), len + 1);
    if (got <= 0) return {};
    buf.resize(static_cast<size_t>(got));
    return ToUtf8(buf);
}

std::string ReadClassName(HWND hwnd) {
    wchar_t buf[256] = {};
    const int got = ::GetClassNameW(hwnd, buf, 256);
    if (got <= 0) return {};
    return ToUtf8(std::wstring(buf, static_cast<size_t>(got)));
}

std::string ToLowerAscii(std::string v) {
    for (char& c : v) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return v;
}

// 以 a 是否以 b 开头（都当小写）
bool StartsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// ---------------------------------------------------------------------------
// 枚举回调
// ---------------------------------------------------------------------------

struct EnumCtx {
    std::vector<WindowInfo>* out = nullptr;
    HWND foreground = nullptr;
    bool ok = true;
};

BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lparam) {
    auto* ctx = reinterpret_cast<EnumCtx*>(lparam);
    if (!ctx || !ctx->out) return FALSE;

    // 只看顶层。EnumWindows 天然只给顶层，但 GetParent 能挡掉 owned window。
    if (::GetParent(hwnd) != nullptr) return TRUE;

    if (!::IsWindowVisible(hwnd)) return TRUE;
    if (::IsIconic(hwnd)) return TRUE; // 最小化：录出来是黑的，直接排除

    const LONG_PTR exStyle = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) return TRUE;

    const std::string cls = ReadClassName(hwnd);
    if (cls.empty() || IsSkippedClass(cls)) return TRUE;
    // ApplicationFrameWindow 是 UWP 宿主，真正的内容在子窗口里；
    // 保留它（用户能看见），但要记录一下。这里只跳过明确不需要的。
    if (cls == "Windows.UI.Core.CoreWindow") return TRUE;

    const std::string title = ReadWindowText(hwnd);
    if (title.empty()) return TRUE;

    RECT rc{};
    if (!::GetWindowRect(hwnd, &rc)) return TRUE;
    const int32_t w = rc.right - rc.left;
    const int32_t h = rc.bottom - rc.top;
    // 太小的窗口（工具栏、浮标）不列出来
    if (w < 64 || h < 64) return TRUE;

    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);

    WindowInfo info;
    info.hwnd = hwnd;
    info.title = title;
    info.className = cls;
    info.pid = pid;
    info.left = rc.left;
    info.top = rc.top;
    info.width = w;
    info.height = h;
    info.foreground = (hwnd == ctx->foreground);
    ctx->out->push_back(std::move(info));
    return TRUE;
}

} // namespace

std::string WindowTitle(HWND hwnd) {
    return hwnd ? ReadWindowText(hwnd) : std::string();
}

std::string WindowClassName(HWND hwnd) {
    return hwnd ? ReadClassName(hwnd) : std::string();
}

bool WindowRect(HWND hwnd, RECT& out) {
    if (!hwnd) return false;
    return ::GetWindowRect(hwnd, &out) != FALSE;
}

std::string ProcessNameOf(uint32_t pid) {
    if (pid == 0) return {};
    // PROCESS_QUERY_LIMITED_INFORMATION 就够了，而且对更高完整性的进程
    // 也更容易成功（PROCESS_QUERY_INFORMATION 会被拒）。
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t buf[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    std::string result;
    if (::QueryFullProcessImageNameW(h, 0, buf, &size)) {
        const std::wstring full(buf, size);
        const size_t slash = full.find_last_of(L"\\/");
        const std::wstring base =
            slash == std::wstring::npos ? full : full.substr(slash + 1);
        result = ToLowerAscii(ToUtf8(base));
    }
    ::CloseHandle(h);
    return result;
}

std::vector<WindowInfo> EnumerateWindows() {
    EnumCtx ctx;
    std::vector<WindowInfo> out;
    ctx.out = &out;
    ctx.foreground = ::GetForegroundWindow();
    ::EnumWindows(&EnumProc, reinterpret_cast<LPARAM>(&ctx));

    // 补齐进程名（OpenProcess 有点开销，所以放在枚举之后单独一遍，
    // 而且同一个 pid 只查一次）
    std::vector<std::pair<uint32_t, std::string>> cache;
    for (auto& w : out) {
        std::string name;
        bool hit = false;
        for (const auto& kv : cache) {
            if (kv.first == w.pid) {
                name = kv.second;
                hit = true;
                break;
            }
        }
        if (!hit) {
            name = ProcessNameOf(w.pid);
            cache.emplace_back(w.pid, name);
        }
        w.processName = name;
    }
    return out;
}

std::vector<WindowInfo> MatchWindows(const std::vector<WindowInfo>& all,
                                     const WindowMatch& rule) {
    std::vector<WindowInfo> hits;
    if (!rule.Valid()) return hits;

    // 按用户排的优先级逐字段尝试：第一个能匹配到窗口的字段说了算。
    //
    // 为什么不把所有命中的字段混在一起取并集：那样"标题像、但根本不是同一个
    // 程序"的窗口也会被录进来。优先级的意义就是「标题最准，标题匹不上才降级
    // 用类名，再降级用 exe 名」，一旦降级成功就不再往后看。
    // 代价是：标题能匹到时，同 exe 的另一窗口不会被录进来 —— 这正是要的。
    //
    // 每个字段用**自己那份指纹**去比（rule.PatternFor）而不是共用 pattern：
    // 以前只有 pattern 一个值、装的又是标题，于是降级到 class 时等于拿整串标题
    // 去比 "CabinetWClass"，永远不命中 —— 降级链其实一直是断的，表现就是
    // "标题一变（换个文件夹）这一路就报匹配不到窗口"。
    for (MatchField f : rule.FieldOrder()) {
        const std::string& own = rule.PatternFor(f);
        if (own.empty()) continue; // 这个字段没有指纹，直接看下一个

        const std::string pattern = ToLowerAscii(own);

        // 正则也按字段各自编译（每字段文字不同，不能复用上面那份）
        std::regex fieldRe;
        bool hasFieldRegex = false;
        if (rule.mode == MatchMode::Regex) {
            try {
                fieldRe = std::regex(own, std::regex::ECMAScript | std::regex::icase);
                hasFieldRegex = true;
            } catch (const std::regex_error&) {
                hasFieldRegex = false;
            }
        }

        for (const auto& w : all) {
            if (rule.requireVisible && w.minimized) continue;

            // 取要匹配的字段（统一转小写，做大小写不敏感比较）
            std::string subject;
            switch (f) {
                case MatchField::Process: subject = w.processName; break;
                case MatchField::Title:   subject = w.title; break;
                case MatchField::Class:   subject = w.className; break;
            }
            const std::string lowered = ToLowerAscii(subject);

            bool matched = false;
            switch (rule.mode) {
                case MatchMode::Exact:
                    matched = (lowered == pattern);
                    break;
                case MatchMode::Contains:
                    matched = lowered.find(pattern) != std::string::npos;
                    break;
                case MatchMode::Prefix:
                    matched = StartsWith(lowered, pattern);
                    break;
                case MatchMode::Regex:
                    if (hasFieldRegex) {
                        try {
                            matched = std::regex_search(subject, fieldRe);
                        } catch (const std::regex_error&) {
                            matched = false;
                        }
                    }
                    break;
            }
            if (matched) hits.push_back(w);
        }
        if (!hits.empty()) break; // 这个优先级匹到了，后面的字段不再参战
    }

    if (hits.empty()) return hits;

    // 多个命中时挑一个
    switch (rule.pick) {
        case MatchPick::Foreground: {
            for (const auto& w : hits) {
                if (w.foreground) return {w};
            }
            // 没有前台命中就退回 Largest
            [[fallthrough]];
        }
        case MatchPick::Largest: {
            auto best = std::max_element(
                hits.begin(), hits.end(),
                [](const WindowInfo& a, const WindowInfo& b) { return a.Area() < b.Area(); });
            return {*best};
        }
        case MatchPick::First:
            return {hits.front()};
        case MatchPick::Oldest: {
            // 枚举顺序基本是 Z 序，这里没有真实创建时间可用，
            // 折中取枚举顺序里最后一个（通常是最早创建的）。
            return {hits.back()};
        }
    }
    return {hits.front()};
}

bool ResolveWindow(const WindowMatch& rule, WindowInfo& out) {
    const auto all = EnumerateWindows();
    const auto hits = MatchWindows(all, rule);
    if (hits.empty()) return false;
    out = hits.front();
    return true;
}

} // namespace shadow
