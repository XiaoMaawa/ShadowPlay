// JsonUtil.h —— 手写 JSON 输出用到的小工具。
//
// 为什么单独一个头：`JsonEscape` 原本在 main.cpp 的**匿名** namespace 里，
// 而 Profiles.cpp 又在 `namespace shadow` 里定义了一份。加上 Session.cpp
// 之后就是三份实现，其中两份同名不同链接性 —— 迟早撞出重复符号。
// 统一收在这里（inline），所有 .cpp 共用一份。
//
// 这里只做**输出**侧的转义，解析器在 Profiles.cpp 里（那边是自成一体的
// 小型状态机，跟输出无关，没必要合并）。
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace shadow {

// 把字符串转义成 JSON 字面量内容（不含两侧引号）。
// 控制字符走 \uXXXX —— 采集到的窗口标题里偶尔真的会有。
inline std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const char ch : s) {
        switch (ch) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned>(static_cast<unsigned char>(ch)));
                    out += buf;
                } else {
                    out += ch;
                }
        }
    }
    return out;
}

} // namespace shadow
