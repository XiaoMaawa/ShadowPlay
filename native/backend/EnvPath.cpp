// EnvPath.cpp — 路径里的环境变量展开（实现在这里，声明见 EnvPath.h）
#include "EnvPath.h"

#include <windows.h>

#include <string>

#include "FfmpegUtil.h" // FromUtf8 / ToUtf8

namespace shadow {

std::string ExpandEnv(const std::string& in) {
    if (in.find('%') == std::string::npos && in.find("${") == std::string::npos) return in;

    auto lookup = [](const std::wstring& name) -> std::wstring {
        if (name.empty()) return {};
        const DWORD n = GetEnvironmentVariableW(name.c_str(), nullptr, 0);
        if (n == 0) return {};
        std::wstring buf(n, L'\0');
        const DWORD got = GetEnvironmentVariableW(name.c_str(), buf.data(), n);
        if (got == 0 || got >= n) return {};
        buf.resize(got);
        return buf;
    };

    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
        // ${NAME}
        if (in[i] == '$' && i + 1 < in.size() && in[i + 1] == '{') {
            const size_t close = in.find('}', i + 2);
            if (close != std::string::npos) {
                const std::string name = in.substr(i + 2, close - i - 2);
                const std::wstring val = lookup(FromUtf8(name));
                if (!val.empty()) {
                    out += ToUtf8(val);
                    i = close + 1;
                    continue;
                }
                out.append(in, i, close + 1 - i); // 展开不了就原样留着
                i = close + 1;
                continue;
            }
        }
        // %NAME%
        if (in[i] == '%') {
            const size_t close = in.find('%', i + 1);
            if (close != std::string::npos) {
                const std::string name = in.substr(i + 1, close - i - 1);
                const std::wstring val = lookup(FromUtf8(name));
                if (!val.empty()) {
                    out += ToUtf8(val);
                    i = close + 1;
                    continue;
                }
                out.append(in, i, close + 1 - i);
                i = close + 1;
                continue;
            }
        }
        out.push_back(in[i]);
        ++i;
    }
    return out;
}

} // namespace shadow
