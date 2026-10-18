// EnvPath.h — 路径里的环境变量展开
//
// 只有一道 API：把 `%TEMP%\shadow-play\app\records` 这种带占位符的路径
// 换成真实路径。
//
// 为什么要单独一个编译单元：这个函数有两个地方要用，而且那两处编在**
// 不同的二进制**里 ——
//   * 采集服务（native/backend/main.cpp 走的 Profiles::Load）；
//   * 界面插件（addon，ExportStart 直接把配置里的原始串递进 Exporter）。
// 放进 Profiles.cpp 会让 addon 链接时找不到符号（Profiles.cpp 不参与 addon
// 的编译），而让 Exporter 反向依赖整个配置模块又太重。抽出来两边都能编。
#pragma once

#include <string>

namespace shadow {

// 展开路径里的环境变量引用。
//
// 支持两种写法，都是 Windows 上大家熟悉的：
//   %TEMP%\shadow-play\app\records
//   ${TEMP}/shadow-play/app/records
//
// 找不到的变量**原样保留**（不删不替换），这样写错一个字母时能一眼看出
// 是哪一段没展开 —— 而不是悄悄变成一个诡异的相对路径。
std::string ExpandEnv(const std::string& in);

} // namespace shadow
