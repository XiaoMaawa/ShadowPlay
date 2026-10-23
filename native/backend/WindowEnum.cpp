// WindowEnum.cpp — 顶层窗口枚举 + 匹配规则求值

#include "WindowEnum.h"

#include <algorithm>
#include <cwctype>
#include <regex>
#include <utility>

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

// hwnd 的"可跨界比较"形态：低 32 位无符号数。
//
// HWND 本身是 32 位句柄，传到 64 位代码里会被**符号扩展**（0x...AABBCCDD 变成
// 0xFFFFFFFFAABBCCDD）。而交给界面 / 从命令行拿回来时我们按无符号 32 位传，
// 于是同一个窗口会有两个不同的 64 位写法。凡是跨边界的比较（偏好窗口、
// --include-hwnd）一律用这个键，不要直接比指针值。
inline uint32_t HwndKey(HWND h) {
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(h));
}

// ---------------------------------------------------------------------------
// 枚举回调
// ---------------------------------------------------------------------------

struct EnumCtx {
    std::vector<WindowInfo>* out = nullptr;
    HWND foreground = nullptr;
    const std::vector<HWND>* forceInclude = nullptr;
    bool ok = true;

    bool Forced(HWND hwnd) const {
        if (!forceInclude) return false;
        for (HWND h : *forceInclude) {
            // 按低 32 位比：外部传进来的 hwnd 是无符号 32 位，进程内的 HWND
            // 是符号扩展过的 64 位值，直接比指针值会永远不相等（见 HwndKey）。
            if (HwndKey(h) == HwndKey(hwnd)) return true;
        }
        return false;
    }
};

// 填充一个 WindowInfo（枚举到的 hwnd 必然还在；force 的那些可能已经销毁）
static void FillWindowInfo(HWND hwnd, WindowInfo& info, HWND foreground) {
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    RECT rc{};
    if (!::GetWindowRect(hwnd, &rc)) {
        rc = RECT{};
    }
    info.hwnd = hwnd;
    info.title = ReadWindowText(hwnd);
    info.className = ReadClassName(hwnd);
    info.pid = pid;
    info.left = rc.left;
    info.top = rc.top;
    info.width = rc.right - rc.left;
    info.height = rc.bottom - rc.top;
    info.minimized = ::IsIconic(hwnd) != FALSE;
    info.foreground = (hwnd == foreground);
}

BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lparam) {
    auto* ctx = reinterpret_cast<EnumCtx*>(lparam);
    if (!ctx || !ctx->out) return FALSE;

    // 服务正在录的窗口：无论最小化、多小、是不是 toolwindow、是不是顶层，
    // 都要列出来 —— 否则下拉框里找不到它（详见 WindowEnum.h 里 forceInclude
    // 的说明）。这一条必须排在"只取顶层"前面：Owned window 也可能在被录。
    if (ctx->Forced(hwnd)) {
        WindowInfo info;
        FillWindowInfo(hwnd, info, ctx->foreground);
        ctx->out->push_back(std::move(info));
        return TRUE;
    }

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

    WindowInfo info;
    FillWindowInfo(hwnd, info, ctx->foreground);
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

bool WindowPresent(HWND hwnd, uint32_t pid) {
    if (!hwnd) return false;
    if (!::IsWindow(hwnd)) return false;
    // 隐藏 = 没有画面。Chromium 系关窗口只 ShowWindow(SW_HIDE)，句柄还活着，
    // 只判 IsWindow 会让这一路永远停在最后一帧（详见头文件说明）。
    if (!::IsWindowVisible(hwnd)) return false;
    if (pid != 0) {
        DWORD actual = 0;
        ::GetWindowThreadProcessId(hwnd, &actual);
        if (actual != pid) return false;  // 句柄被系统回收后给了别的窗口
    }
    return true;
}

bool ProcessAlive(uint32_t pid) {
    if (pid == 0) return false;
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;  // 打不开：要么已经没了，要么根本没权限 → 当作不在
    DWORD code = 0;
    const bool alive = ::GetExitCodeProcess(h, &code) != FALSE && code == STILL_ACTIVE;
    ::CloseHandle(h);
    return alive;
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

std::vector<WindowInfo> EnumerateWindows(const std::vector<HWND>& forceInclude) {
    EnumCtx ctx;
    std::vector<WindowInfo> out;
    ctx.out = &out;
    ctx.foreground = ::GetForegroundWindow();
    ctx.forceInclude = &forceInclude;
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

std::vector<WindowInfo> EnumerateWindows() {
    return EnumerateWindows({});
}

namespace {

struct CountCtx {
    const std::string* process = nullptr;
    size_t count = 0;
    // pid → exe 名。一屏几十个顶层窗口，不去重的话每个都 OpenProcess 一次。
    std::vector<std::pair<uint32_t, std::string>>* cache = nullptr;
};

BOOL CALLBACK CountProc(HWND hwnd, LPARAM lparam) {
    auto* ctx = reinterpret_cast<CountCtx*>(lparam);
    if (!ctx || !ctx->process || !ctx->cache) return TRUE;

    if (::GetParent(hwnd) != nullptr) return TRUE;  // 只看顶层窗口

    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return TRUE;

    std::string name;
    bool hit = false;
    for (const auto& kv : *ctx->cache) {
        if (kv.first == pid) {
            name = kv.second;
            hit = true;
            break;
        }
    }
    if (!hit) {
        name = ProcessNameOf(pid);
        ctx->cache->emplace_back(pid, name);
    }
    if (name == *ctx->process) ++ctx->count;
    return TRUE;
}

} // namespace

size_t ProcessTopLevelWindowCount(const std::string& processName) {
    if (processName.empty()) return 0;
    std::vector<std::pair<uint32_t, std::string>> cache;
    CountCtx ctx;
    ctx.process = &processName;
    ctx.cache = &cache;
    ::EnumWindows(&CountProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.count;
}

std::vector<WindowInfo> MatchWindows(const std::vector<WindowInfo>& all,
                                     const WindowMatch& ruleIn) {
    std::vector<WindowInfo> hits;

    // 【新模型：全部 AND，应用程序必选】
    //
    // 以前这里是「按优先级逐字段降级，第一个能匹到的说了算」，于是标题一变就会
    // 悄悄降级到类名，进而选中同一类名（Chrome_WidgetWin_1）里面积最大的那个
    // 窗口 —— 用户看到的是"标题完全对不上，却录了别的窗口"。现在改成：
    //   RequiredFields() 里的每一份指纹**都必须命中**，少一条就不算匹配，
    //   这一路保持未采集（宁可空着，也不乱抓一个别的东西来录）。
    //
    // Normalize() 是幂等的，会顺手把老配置（只有 pattern / 只有 byOrder）
    // 补成新模型的形态，所以这里拿到的一定是完整规则。
    WindowMatch rule = ruleIn;
    rule.Normalize();

    if (!rule.Valid()) return hits;

    // 参与比较的字段：规则要求的字段里**真的有指纹**的那些。
    // 一个都没有 = 规则是空的，直接不匹配（不要"什么条件都没有所以全都算命中"）。
    const auto fields = rule.ActiveFields();
    if (fields.empty()) return hits;

    // 正则每字段各自编译一次（各字段文字不同，不能复用）。
    std::vector<std::regex> regexes(fields.size());
    std::vector<bool> hasRegex(fields.size(), false);
    if (rule.mode == MatchMode::Regex) {
        for (size_t i = 0; i < fields.size(); ++i) {
            try {
                regexes[i] = std::regex(rule.FieldPattern(fields[i]),
                                        std::regex::ECMAScript | std::regex::icase);
                hasRegex[i] = true;
            } catch (const std::regex_error&) {
                hasRegex[i] = false;  // 正则写错 → 这个字段永远不命中
            }
        }
    }

    for (const auto& w : all) {
        if (rule.requireVisible && w.minimized) continue;

        bool ok = true;
        for (size_t i = 0; i < fields.size() && ok; ++i) {
            const MatchField f = fields[i];
            const std::string pattern = ToLowerAscii(rule.FieldPattern(f));

            std::string subject;
            switch (f) {
                case MatchField::Process: subject = w.processName; break;
                case MatchField::Title:   subject = w.title; break;
                case MatchField::Class:   subject = w.className; break;
            }
            const std::string lowered = ToLowerAscii(subject);

            switch (rule.mode) {
                case MatchMode::Exact:
                    ok = (lowered == pattern);
                    break;
                case MatchMode::Contains:
                    ok = lowered.find(pattern) != std::string::npos;
                    break;
                case MatchMode::Prefix:
                    ok = StartsWith(lowered, pattern);
                    break;
                case MatchMode::Regex:
                    if (hasRegex[i]) {
                        try {
                            ok = std::regex_search(subject, regexes[i]);
                        } catch (const std::regex_error&) {
                            ok = false;
                        }
                    } else {
                        ok = false;
                    }
                    break;
            }
        }
        if (ok) hits.push_back(w);
    }

    if (hits.empty()) return hits;

    // 用户在下拉里明确挑过的那个窗口优先 —— 但仍然必须在上面那轮筛选里命中。
    //
    // 【为什么需要】「只匹配应用程序」下同一程序的多个窗口全部命中，按
    // pick:largest 只会一直录面积最大的那个。用户在下拉里换成同程序的另一个
    // 窗口时，exe 指纹一个字都没变，服务没理由换源，表现成"下拉能点、画面
    // 不换"。preferredHwnd 记下用户挑的那个，这里在**命中集合内**优先返回它；
    // 它不在了（关掉/销毁）就自然落到下面的 pick 逻辑，退回同程序的另一个
    // 窗口 —— 这正好是这一档该有的语义。
    //
    // 注意：偏好**不参与**上面的字段比较，所以它绝不会让不满足规则的窗口被选中
    // （"匹配不到就别乱选"这条仍然成立）。
    //
    // 只比低 32 位：HWND 本身是 32 位句柄，交给 JS 时也统一按无符号 32 位输出
    // （见 main.cpp 的 BuildWindowsJson）。直接用 64 位值比会因为符号扩展后的
    // 高位对不上，而且超过 2^53 的值在 JS 里根本存不准。
    if (rule.preferredHwnd != 0) {
        const uint32_t want = static_cast<uint32_t>(rule.preferredHwnd);
        for (const auto& w : hits) {
            if (HwndKey(w.hwnd) == want) return {w};
        }
    }

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
