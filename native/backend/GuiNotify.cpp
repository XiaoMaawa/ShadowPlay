// GuiNotify.cpp — 见 GuiNotify.h
#include "GuiNotify.h"

#include "RingNames.h"
#include "StatusFile.h"  // StatusFilePath（标记文件要和状态文件放同一目录）

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <cstdio>
#include <string>

namespace shadow {
namespace {

namespace fs = std::filesystem;

// 写一行 NDJSON 就断开。界面侧是 net.createServer 的 socket，按行解析。
bool SendLine(const std::string& line) {
    const std::string name = GuiPipeName();
    for (int attempt = 0; attempt < 6; ++attempt) {
        HANDLE pipe =
            ::CreateFileA(name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            const BOOL ok =
                ::WriteFile(pipe, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
            if (ok) ::FlushFileBuffers(pipe);
            ::CloseHandle(pipe);
            return ok != FALSE;
        }
        const DWORD err = ::GetLastError();
        if (err == ERROR_PIPE_BUSY) {
            // 界面正忙着处理上一条命令（或者刚 accept 完还没读），等一小下。
            ::WaitNamedPipeA(name.c_str(), 100);
            continue;
        }
        if (err != ERROR_FILE_NOT_FOUND) {
            // 管道存在但打不开：权限/参数问题。重试没有意义。
            return false;
        }
        // 界面没在跑（或管道还没建好）。给它一点时间，但必须有上限 ——
        // 这是托盘线程，退出流程不能被卡住。
        ::Sleep(60);
    }
    return false;
}

} // namespace

// 兜底通道：写标记文件，界面轮询状态时看到就自己退出。
//
// 只放"请求退出"这一个事实，不带任何需要解析的结构 —— 界面那边看文件在
// 不在就够了。界面对旧标记有防护（只认界面启动之后写的），所以这里不用
// 自己清理上一次的残留。
bool WriteQuitMarker(const char* reason) {
    std::error_code ec;
    const fs::path dir = fs::u8path(StatusFilePath()).parent_path();
    fs::create_directories(dir, ec);
    std::ofstream f(dir / "gui-quit-requested", std::ios::trunc);
    if (!f) return false;
    // 内容是给人排查用的（时间 + 原因），界面不解析。
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    char ts[64]{};
    std::snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond);
    f << ts << " " << (reason ? reason : "") << "\n";
    f.flush();
    return static_cast<bool>(f);
}

bool NotifyGuiShutdown(const char* reason) {
    // JSON 里的原因字段只放我们自己写的短 token（tray-exit 之类），
    // 不含引号，不需要转义。
    std::string line = "{\"cmd\":\"quit\"";
    if (reason && *reason) {
        line += ",\"reason\":\"";
        line += reason;
        line += "\"";
    }
    line += "}\n";

    const bool viaPipe = SendLine(line);
    // 管道通了就是通了；没通再写标记（两种方式都试过才算失败）。
    const bool viaFile = viaPipe ? false : WriteQuitMarker(reason);
    return viaPipe || viaFile;
}

} // namespace shadow
