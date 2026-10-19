// native/backend/StatusFile.cpp —— 见 StatusFile.h 顶部的设计说明。

#include "StatusFile.h"

#include "Utf8Path.h" // ToUtf8

#include <windows.h>
#include <shlobj.h>

#include <filesystem>
#include <vector>

namespace shadow {
namespace {

namespace fs = std::filesystem;

constexpr DWORD kPollMs = 200; // 没有新状态时的空转间隔

std::string Win32Message(DWORD code) {
    if (code == 0) return {};
    LPWSTR buf = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                       FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::string out;
    if (n && buf) {
        std::wstring w(buf, n);
        while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n')) w.pop_back();
        out = ToUtf8(w);
    }
    if (buf) LocalFree(buf);
    if (out.empty()) out = "Win32 错误 " + std::to_string(code);
    return out;
}

// 临时根目录（%TEMP%\shadow-play\app，或 SHADOW_TEMP_ROOT 覆盖）。
//
// 必须和 app/temp-const.cjs 里的 TEMP_ROOT 算出同一个值 —— 界面和服务是
// 两个进程，谁也不告诉谁自己在哪，靠的就是这条约定。
//
// 为什么放系统 temp 而不是仓库里：仓库目录可能被 DSH 沙箱、同步盘、杀毒
// 软件盯着，写文件既慢又会被打上奇怪的安全标签（本鱼被 Low 完整性标签
// 坑过一次，WGC 直接 0x80070005）。系统 temp 就是给这种东西用的。
//
// 环境变量 SHADOW_TEMP_ROOT 可以整体改位置，测试脚本用它把产物引到自己的
// 隔离目录里；界面侧必须实现同样的覆盖，否则两边会各写各的。
fs::path TempRoot() {
    // 覆盖优先
    if (const DWORD n = GetEnvironmentVariableW(L"SHADOW_TEMP_ROOT", nullptr, 0); n > 1) {
        std::wstring buf(n, L'\0');
        const DWORD got = GetEnvironmentVariableW(L"SHADOW_TEMP_ROOT", buf.data(), n);
        if (got > 0 && got < n) {
            buf.resize(got);
            if (!buf.empty()) return fs::path(buf);
        }
    }
    wchar_t tmp[MAX_PATH]{};
    const DWORD n = GetTempPathW(MAX_PATH, tmp);
    fs::path base = (n > 0 && n < MAX_PATH) ? fs::path(tmp) : fs::temp_directory_path();
    return base / L"shadow-play" / L"app";
}

// 状态文件放在 <临时根>/status/status-v1.json。
//
// 为什么界面能用同一个算法算出来：这条路径完全由 %TEMP% 决定，不依赖
// "服务 exe 在哪"、"谁起的服务"，所以界面和服务必然指向同一个文件。
//
// 环境变量 SHADOW_STATUS_FILE 仍然可以单独覆盖状态文件位置（优先级最高），
// 测试脚本用它把状态引到别处。
fs::path StatusFileDefaultPath() {
    return TempRoot() / L"status" / L"status-v1.json";
}

} // namespace

std::string StatusFilePath() {
    static const std::string cached = []() {
        // 测试覆盖优先：脚本把状态引到自己的临时目录，跑完就删。
        if (const DWORD n = GetEnvironmentVariableW(L"SHADOW_STATUS_FILE", nullptr, 0); n > 1) {
            std::wstring buf(n, L'\0');
            const DWORD got = GetEnvironmentVariableW(L"SHADOW_STATUS_FILE", buf.data(), n);
            if (got > 0 && got < n) {
                buf.resize(got);
                if (!buf.empty()) return ToUtf8(buf);
            }
        }
        // 注意用 PathToUtf8 而不是 p.string()：路径里可能有中文用户名，
        // 走 ANSI 会变成乱码（这个坑在导出那边踩过一次，见 Utf8Path.h）。
        return PathToUtf8(StatusFileDefaultPath());
    }();
    return cached;
}

StatusFile::StatusFile()
    : mu_(new std::mutex()),
      pending_(new std::string()),
      pubSeq_(new uint64_t(0)),
      lastError_(new std::string()) {}

StatusFile::~StatusFile() {
    Stop();
    delete mu_;
    delete pending_;
    delete pubSeq_;
    delete lastError_;
}

void StatusFile::Start() {
    if (running_.load(std::memory_order_acquire)) return;
    stop_.store(false, std::memory_order_release);

    // 先把目录建出来。建不出来只记错误，不影响服务本身。
    try {
        fs::create_directories(fs::path(FromUtf8(StatusFilePath())).parent_path());
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lk(*mu_);
        *lastError_ = std::string("建不了状态文件目录：") + e.what();
    }

    HANDLE h = CreateThread(nullptr, 0,
                            [](LPVOID self) -> DWORD {
                                static_cast<StatusFile*>(self)->ThreadMain();
                                return 0;
                            },
                            this, 0, nullptr);
    if (!h) {
        std::lock_guard<std::mutex> lk(*mu_);
        *lastError_ = "起不了状态写线程：" + Win32Message(GetLastError());
        return;
    }
    thread_ = h;
    running_.store(true, std::memory_order_release);
}

void StatusFile::Publish(std::string payload) {
    if (!running_.load(std::memory_order_acquire)) return;
    if (payload.empty()) return;
    if (payload.back() != '\n') payload.push_back('\n');
    {
        std::lock_guard<std::mutex> lk(*mu_);
        *pending_ = std::move(payload);
        ++(*pubSeq_);
    }
    // 不在这里做任何阻塞 IO —— 主循环的节拍不能被"写文件快不快"影响。
}

void StatusFile::Stop() {
    if (!running_.load(std::memory_order_acquire)) return;
    stop_.store(true, std::memory_order_release);
    if (HANDLE h = static_cast<HANDLE>(thread_)) {
        WaitForSingleObject(h, 2000);
        CloseHandle(h);
        thread_ = nullptr;
    }
    running_.store(false, std::memory_order_release);
}

std::string StatusFile::LastError() const {
    std::lock_guard<std::mutex> lk(*mu_);
    return *lastError_;
}

void StatusFile::ThreadMain() {
    uint64_t writtenSeq = 0;
    for (;;) {
        if (stop_.load(std::memory_order_acquire)) break;

        std::string payload;
        {
            std::lock_guard<std::mutex> lk(*mu_);
            if (*pubSeq_ != writtenSeq && !pending_->empty()) {
                payload = *pending_;
                writtenSeq = *pubSeq_;
            }
        }
        if (payload.empty()) {
            Sleep(kPollMs);
            continue;
        }

        // 整份原子替换：先写 <path>.tmp，再 MoveFileEx 换过去。界面任何时刻
        // 打开 status-v1.json 都能读到一份完整的 JSON。
        const fs::path target = fs::path(FromUtf8(StatusFilePath()));
        const fs::path tmp = target.wstring() + L".tmp";
        DWORD err = ERROR_SUCCESS;
        {
            HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f == INVALID_HANDLE_VALUE) {
                err = GetLastError();
            } else {
                DWORD written = 0;
                const BOOL ok = WriteFile(f, payload.data(),
                                          static_cast<DWORD>(payload.size()), &written, nullptr);
                if (!ok) err = GetLastError();
                CloseHandle(f);
                if (err == ERROR_SUCCESS) {
                    if (!MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                        err = GetLastError();
                    }
                } else {
                    DeleteFileW(tmp.c_str());
                }
            }
        }

        if (err != ERROR_SUCCESS) {
            std::lock_guard<std::mutex> lk(*mu_);
            *lastError_ = "写状态文件失败：" + Win32Message(err);
        } else {
            writes_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    running_.store(false, std::memory_order_release);
}

} // namespace shadow
