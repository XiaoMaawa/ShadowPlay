// FfmpegUtil.h —— 跟 ffmpeg.exe 打交道时两边都要用的小工具。
//
// 抽出来的原因：分片录制（SegmentRecorder）和导出合并（Exporter）都要
//   ① 找到 ffmpeg.exe   ② 按 Windows 的引号规则拼命令行   ③ 在 UTF-8 和
//   UTF-16 之间转换。这三件事原来只有录制端有一份，导出端要再抄一遍就
//   迟早会不一致（尤其是路径里带中文时，两边引号规则不一样就会挂）。
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace shadow {

/** ffmpeg.exe 全路径。优先环境变量 SHADOW_FFMPEG，其次服务自己旁边，最后 PATH。
 *  找不到返回空串。 */
std::string ResolveFfmpeg();

/** UTF-8 ↔ UTF-16（Win32 宽字符 API 用）。 */
std::wstring FromUtf8(const std::string& s);
std::string ToUtf8(const std::wstring& w);

/** 把可执行文件路径 + 参数拼成 CreateProcessW 能用的命令行（内部会把 exe 作为
 *  argv[0] 重复一遍，所以 args 里**不要**再放 exe 路径）。 */
std::wstring BuildCommandLine(const std::string& exe, const std::vector<std::string>& args);

/** 单个 argv 元素的 Windows 引号规则（CommandLineToArgvW 拆回来必须一模一样）。
 *  需要"argv[0] 已经在列表里"时直接用它逐个拼。 */
std::wstring QuoteArg(const std::wstring& s);

/** fs::path → UTF-8 窄字符串。**路径要变成 std::string 时一律用它，不要用
 *  path::string()**：string() 走本地 ANSI 代码页（简中机器上是 GBK），而路径里
 *  往往带 UTF-8 的配置名（"整屏-主显示器"）。UTF-8 字节被按 GBK 反过来解释 →
 *  乱码文件名；若含有 GBK 编不出的字节，还会直接变成一串 U+FFFD（实测产出
 *  "????-????ʾ??_Replay_....mp4"）。 */
std::string PathToUtf8(const std::filesystem::path& p);

/** UTF-8 窄字符串 → fs::path。**路径从 std::string 来时一律用它，不要用
 *  fs::path(s)**：那会把 UTF-8 字节当成 ANSI 解释，然后 CreateFileW 找不到文件
 *  （实测：明明写出了 16MB 的 MP4，file_size 却报"系统找不到指定的文件"）。 */
std::filesystem::path PathFromUtf8(const std::string& s);

} // namespace shadow
