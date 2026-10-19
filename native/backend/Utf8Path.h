// Utf8Path.h —— UTF-8 字符串 / 文件路径转换。
//
// 为什么要单独一个文件：
//   这几个函数原来住在 FfmpegUtil.h 里（那时它们只是"拼 ffmpeg 命令行"的
//   帮手）。ffmpeg.exe 已经彻底移除了，但**路径里可能有中文**（用户名、
//   录像目录名），
//   `std::filesystem::path::string()` 会按系统 ANSI 代码页（GBK）转换，
//   中文路径一进去就变成乱码，建目录/开文件全部失败。
//   所以这些函数必须留着，只是不该再顶着 Ffmpeg 的名字 —— 谁看都以为
//   还要装 ffmpeg。
#pragma once

#include <filesystem>
#include <string>

namespace shadow {

/** UTF-8 窄串 -> 宽串（Windows 内部一律用 UTF-16）。 */
std::wstring FromUtf8(const std::string& s);

/** 宽串 -> UTF-8 窄串。 */
std::string ToUtf8(const std::wstring& w);

/**
 * fs::path -> UTF-8 窄串。
 *
 * 等价于 ToUtf8(p.native())，但路径本身通常是 UTF-16 的，native()
 * 在 Windows 上正好就是 wstring。
 */
std::string PathToUtf8(const std::filesystem::path& p);

/** UTF-8 窄串 -> fs::path。**不要**用 fs::u8path 之外的写法拼中文路径。 */
std::filesystem::path PathFromUtf8(const std::string& s);

} // namespace shadow
