// FfmpegUtil.cpp —— 见头文件说明。
#include "FfmpegUtil.h"

#include <windows.h>

#include <filesystem>

namespace shadow {
namespace fs = std::filesystem;

namespace {

/** 这个字符要不要引号（用 CommandLineToArgvW 拆回来必须一模一样）。 */
bool NeedsQuotes(const std::wstring& s) {
    if (s.empty()) return true;
    for (wchar_t c : s) {
        if (c == L' ' || c == L'\t' || c == L'"') return true;
    }
    return false;
}

} // namespace

std::string ResolveFfmpeg() {
    char buf[MAX_PATH * 2] = {0};

    const DWORD n = GetEnvironmentVariableA("SHADOW_FFMPEG", buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) {
        std::error_code ec;
        if (fs::is_regular_file(fs::u8path(buf), ec)) return buf;
    }

    // 服务自己旁边的 ffmpeg.exe（便携部署时把 ffmpeg 跟 exe 放一起）
    if (GetModuleFileNameA(nullptr, buf, sizeof(buf)) > 0) {
        std::error_code ec;
        fs::path beside = fs::u8path(buf).parent_path() / "ffmpeg.exe";
        if (fs::is_regular_file(beside, ec)) return beside.string();
    }

    // PATH
    char found[MAX_PATH * 2] = {0};
    if (SearchPathA(nullptr, "ffmpeg.exe", nullptr, sizeof(found), found, nullptr) > 0) {
        return found;
    }
    return {};
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr,
                                      0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr,
                        nullptr);
    return s;
}

// 加引号并转义内部的 " 。末尾的反斜杠要加倍，否则会把收尾引号转义掉 ——
// 这正是 `CommandLineToArgvW` 的规则，子进程拆回去才能拿到原样的路径。
std::wstring QuoteArg(const std::wstring& s) {
    if (!NeedsQuotes(s)) return s;
    std::wstring out;
    out.reserve(s.size() + 8);
    out.push_back(L'"');
    size_t backslashes = 0;
    for (wchar_t c : s) {
        if (c == L'\\') {
            ++backslashes;
            out.push_back(c);
            continue;
        }
        if (c == L'"') {
            out.append(backslashes, L'\\'); // 引号前的反斜杠加倍
            out.push_back(L'\\');           // 引号本身再补一个反斜杠
            out.push_back(L'"');
            backslashes = 0;
            continue;
        }
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes, L'\\'); // 收尾引号前把反斜杠全加倍
    out.push_back(L'"');
    return out;
}

std::wstring BuildCommandLine(const std::string& exe, const std::vector<std::string>& args) {
    // argv[0] 按约定要重复一遍可执行文件路径
    std::wstring cmd = QuoteArg(FromUtf8(exe));
    for (const std::string& a : args) {
        cmd.push_back(L' ');
        cmd += QuoteArg(FromUtf8(a));
    }
    return cmd;
}

std::string PathToUtf8(const fs::path& p) { return ToUtf8(p.native()); }

fs::path PathFromUtf8(const std::string& s) { return fs::path(FromUtf8(s)); }

} // namespace shadow
