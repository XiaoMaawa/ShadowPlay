// Utf8Path.cpp —— 见头文件说明。原样搬自原来的 FfmpegUtil.cpp（那几个函数和
// ffmpeg 没关系，只是当时顺手放在那儿了）。
#include "Utf8Path.h"

#include <windows.h>

namespace shadow {
namespace fs = std::filesystem;

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

std::string PathToUtf8(const fs::path& p) { return ToUtf8(p.native()); }

fs::path PathFromUtf8(const std::string& s) { return fs::path(FromUtf8(s)); }

} // namespace shadow
