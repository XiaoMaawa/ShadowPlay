// AudioRing.cpp —— 音频滚动落地与按窗口读取。设计说明见 AudioRing.h。
#include "AudioRing.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <system_error>

#include "Utf8Path.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace shadow {
namespace fs = std::filesystem;

namespace {

constexpr uint32_t kRate = 48000;
constexpr uint32_t kChannels = 2;
constexpr uint32_t kBytesPerFrame = 4; // S16 立体声

int64_t SysNowMs() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<int64_t>(u.QuadPart / 10000ull) - 11644473600000ll;
}

/**
 * 导出期间的下限（墙钟毫秒）：比它新的块一律不删。见 AudioRing::SetExportPin。
 * 全局一个就够 —— 服务里同一时刻只跑一次导出。
 */
std::atomic<int64_t> g_exportPinMs{0};

// 【路径一律走 Utf8Path】录像目录可能带中文（用户名、配置名）。直接用
// path::string() 会按系统 ANSI 代码页（GBK）转，中文一变乱码就找不到文件了。
fs::path FromU8(const std::string& s) { return PathFromUtf8(s); }
std::string U8(const fs::path& p) { return PathToUtf8(p); }

/** `a_1700000000000.pcm` -> 1700000000000；别的名字返回 0。 */
int64_t ParseStartMs(const std::string& fileName) {
    if (fileName.size() < 8) return 0;
    if (fileName[0] != 'a' || fileName[1] != '_') return 0;
    const size_t dot = fileName.rfind(".pcm");
    if (dot == std::string::npos || dot <= 2) return 0;
    int64_t v = 0;
    for (size_t i = 2; i < dot; ++i) {
        const char c = fileName[i];
        if (c < '0' || c > '9') return 0;
        v = v * 10 + (c - '0');
        if (v > 90000000000000ll) return 0; // 明显不是毫秒时间戳
    }
    return v > 0 ? v : 0;
}

} // namespace

std::string AudioDirForSegmentsDir(const std::string& segmentsDir) {
    if (segmentsDir.empty()) return {};
    fs::path seg = FromU8(segmentsDir);
    // 正常形态：<录像目录>/segments -> <录像目录>/audio
    if (seg.filename() == fs::u8path("segments")) {
        return U8(seg.parent_path() / fs::u8path("audio"));
    }
    // 兜底：传进来的不是 segments 目录（自检/探针会直接传录像目录）
    return U8(seg / fs::u8path("audio"));
}

// ---------------------------------------------------------------------------
// AudioRing
// ---------------------------------------------------------------------------

AudioRing::~AudioRing() { Close(); }

bool AudioRing::Open(const std::string& dir, uint32_t keepSeconds, uint32_t chunkSeconds) {
    std::lock_guard<std::mutex> lk(mu_);
    Close();
    dir_ = dir;
    keepSeconds_ = keepSeconds > 0 ? keepSeconds : 120;
    chunkSeconds_ = chunkSeconds >= 1 ? chunkSeconds : 2;
    ok_ = false;
    bytes_ = 0;
    chunks_ = 0;
    if (dir_.empty()) return false;
    std::error_code ec;
    fs::create_directories(FromU8(dir_), ec);
    if (ec) return false;
    ok_ = true;
    return true;
}

void AudioRing::Close() {
    // 调用方可能已经持锁（Open 里 Close 前没解锁），这里不重复加锁：
    // 只在 out_ 上做事，锁交给调用方保证。
    if (out_.is_open()) {
        out_.flush();
        out_.close();
    }
    chunkStartMs_ = 0;
    chunkFrames_ = 0;
}

bool AudioRing::Ok() const {
    std::lock_guard<std::mutex> lk(mu_);
    return ok_;
}

uint64_t AudioRing::Bytes() const {
    std::lock_guard<std::mutex> lk(mu_);
    return bytes_;
}

uint32_t AudioRing::Chunks() const {
    std::lock_guard<std::mutex> lk(mu_);
    return chunks_;
}

void AudioRing::Rotate(int64_t startWallMs) {
    if (out_.is_open()) {
        out_.flush();
        out_.close();
        ++chunks_;
    }
    chunkStartMs_ = startWallMs;
    chunkFrames_ = 0;
    char name[64];
    std::snprintf(name, sizeof(name), "a_%lld.pcm", static_cast<long long>(startWallMs));
    const fs::path p = FromU8(dir_) / fs::u8path(name);
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    out_.open(p, std::ios::binary | std::ios::trunc);
    if (!out_) {
        // 磁盘满/权限问题：停止写，但不影响录制（录制是主功能）。
        ok_ = false;
    }
}

void AudioRing::Append(const int16_t* pcm, size_t frames, int64_t startWallMs) {
    if (!pcm || frames == 0) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (!ok_) return;

    const int64_t chunkCap = static_cast<int64_t>(chunkSeconds_) * kRate;
    // 换文件的两个条件：写满标称时长，或者和当前块接不上（采集停过一段）。
    const int64_t expectedEndMs = chunkStartMs_ + static_cast<int64_t>(chunkFrames_ * 1000 / kRate);
    const bool gap = out_.is_open() && startWallMs > expectedEndMs + 500;
    if (!out_.is_open() || chunkFrames_ >= static_cast<uint64_t>(chunkCap) || gap ||
        (out_.is_open() && startWallMs < chunkStartMs_)) {
        Rotate(startWallMs);
    }
    if (!out_.is_open()) return;

    const std::streamsize bytes = static_cast<std::streamsize>(frames) * kBytesPerFrame;
    out_.write(reinterpret_cast<const char*>(pcm), bytes);
    if (!out_) {
        ok_ = false;
        return;
    }
    chunkFrames_ += frames;
    bytes_ += static_cast<uint64_t>(bytes);

    // 每秒清一次旧文件（不用另起线程：Append 本身就在每秒几十次的节奏上）。
    const int64_t now = startWallMs;
    if (now - lastPurgeMs_ > 1000) {
        lastPurgeMs_ = now;
        PurgeLocked(now); // 已经持锁，不能再走会加锁的 Purge()
    }
}

void AudioRing::Purge(int64_t nowWallMs) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!ok_ || dir_.empty()) return;
    PurgeLocked(nowWallMs > 0 ? nowWallMs : SysNowMs());
}

void AudioRing::SetExportPin(int64_t floorMs) {
    g_exportPinMs.store(floorMs > 0 ? floorMs : 0, std::memory_order_relaxed);
}

void AudioRing::PurgeLocked(int64_t now) {
    const int64_t keepMs = static_cast<int64_t>(keepSeconds_) * 1000 +
                           static_cast<int64_t>(chunkSeconds_) * 2000 + 2000;
    const int64_t oldest = now - keepMs;
    const int64_t pin = g_exportPinMs.load(std::memory_order_relaxed);
    uint64_t bytes = 0;
    uint32_t count = 0;
    std::error_code ec;
    for (auto it = fs::directory_iterator(FromU8(dir_), ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const fs::path p = it->path();
        if (!p.has_filename() || p.extension() != fs::u8path(".pcm")) continue;
        const std::string fn = U8(p.filename());
        const int64_t start = ParseStartMs(fn);
        if (start <= 0) continue;
        std::error_code se;
        const auto sz = fs::file_size(p, se);
        if (se) continue;
        const int64_t framesInFile = static_cast<int64_t>(sz) / kBytesPerFrame;
        const int64_t endMs = start + framesInFile * 1000 / kRate;
        // 导出正在读（还没读完）的块不能删，否则成片里会少一段。
        if (endMs < oldest && (pin <= 0 || endMs < pin)) {
            std::error_code re;
            fs::remove(p, re);
            continue;
        }
        bytes += static_cast<uint64_t>(sz);
        ++count;
    }
    // 当前正在写的那个文件也算在里面（可能还没进上面的统计）
    if (out_.is_open()) {
        bytes += static_cast<uint64_t>(chunkFrames_) * kBytesPerFrame;
        ++count;
    }
    bytes_ = bytes;
    chunks_ = count;
}

// ---------------------------------------------------------------------------
// AudioClipReader
// ---------------------------------------------------------------------------

bool AudioClipReader::Open(const std::string& dir, int64_t windowStartMs, int64_t windowEndMs) {
    dir_ = dir;
    windowStartMs_ = windowStartMs;
    windowEndMs_ = windowEndMs;
    chunks_.clear();
    if (dir_.empty() || windowStartMs <= 0) return false;

    std::error_code ec;
    for (auto it = fs::directory_iterator(FromU8(dir_), ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const fs::path p = it->path();
        if (!p.has_filename() || p.extension() != fs::u8path(".pcm")) continue;
        const int64_t start = ParseStartMs(U8(p.filename()));
        if (start <= 0) continue;
        std::error_code se;
        const auto sz = fs::file_size(p, se);
        if (se || sz < kBytesPerFrame) continue;
        Chunk c;
        c.path = U8(p);
        c.startMs = start;
        c.frames = static_cast<int64_t>(sz) / kBytesPerFrame;
        chunks_.push_back(std::move(c));
    }
    std::sort(chunks_.begin(), chunks_.end(),
              [](const Chunk& a, const Chunk& b) { return a.startMs < b.startMs; });
    // 只留和窗口相交的
    if (!chunks_.empty()) {
        const int64_t winEnd = windowEndMs_ > 0 ? windowEndMs_ : (1ll << 60);
        std::vector<Chunk> keep;
        keep.reserve(chunks_.size());
        for (auto& c : chunks_) {
            if (c.endMs() < windowStartMs_ || c.startMs > winEnd) continue;
            keep.push_back(std::move(c));
        }
        chunks_ = std::move(keep);
    }
    return !chunks_.empty();
}

int64_t AudioClipReader::LastEndMs() const {
    if (chunks_.empty()) return windowStartMs_;
    return chunks_.back().endMs();
}

int AudioClipReader::FindChunk(int64_t wallMs) const {
    // 块不多（回放窗口 / 2 秒），线性找足够。
    for (size_t i = 0; i < chunks_.size(); ++i) {
        const Chunk& c = chunks_[i];
        if (wallMs >= c.startMs && wallMs < c.endMs()) return static_cast<int>(i);
        if (c.startMs > wallMs) break;
    }
    return -1;
}

int64_t AudioClipReader::NextChunkStartMs(int64_t wallMs) const {
    for (const Chunk& c : chunks_) {
        if (c.startMs > wallMs) return c.startMs;
    }
    return 0;
}

size_t AudioClipReader::Read(int64_t sampleIndex, int16_t* out, size_t frames) {
    if (!out || frames == 0) return 0;
    if (sampleIndex < 0) return 0;

    // 窗口右端（有的话）先夹一次
    int64_t want = static_cast<int64_t>(frames);
    if (windowEndMs_ > 0) {
        const int64_t maxFrames = (windowEndMs_ - windowStartMs_) * kRate / 1000;
        if (sampleIndex >= maxFrames) return 0;
        if (sampleIndex + want > maxFrames) want = maxFrames - sampleIndex;
    }
    if (want <= 0) return 0;

    int64_t written = 0;
    while (written < want) {
        const int64_t cur = sampleIndex + written;
        // 48 样本 = 1 毫秒（48000/1000 整除），这里不会累积舍入误差。
        //
        // 【但不足 1ms 的余数必须留住】下面按毫秒找块，块内偏移却要从**帧**
        // 算回去。以前这里直接 (wallMs - startMs) * 48，把 cur%48 丢掉，等于
        // 每次读都从"往下取整到毫秒"的位置开始 —— 调用方一次读 1024 帧，
        // 而 1024 不是 48 的倍数（1024 = 21*48 + 16），于是相邻两块最多重叠
        // 32 个采样。听感就是每秒几十次的"咔咔"，很像卡顿。
        const int64_t msIdx = cur / 48;
        const int64_t subFrame = cur - msIdx * 48; // 0..47
        const int64_t wallMs = windowStartMs_ + msIdx;
        const int ci = FindChunk(wallMs);
        if (ci < 0) {
            // 洞里：填到下一个块的起点（没有下一个块就填完并结束）
            const int64_t nextStart = NextChunkStartMs(wallMs);
            int64_t fill = want - written;
            if (nextStart > 0) {
                const int64_t nextIdx = (nextStart - windowStartMs_) * 48;
                if (nextIdx > cur && nextIdx - cur < fill) fill = nextIdx - cur;
            } else {
                // 最后一个块之后：窗口没给右端 → 到此为止；给了就补齐到右端
                if (windowEndMs_ <= 0) {
                    if (written == 0) return 0;
                    return static_cast<size_t>(written);
                }
            }
            std::memset(out + written * kChannels, 0,
                        static_cast<size_t>(fill) * kBytesPerFrame);
            written += fill;
            continue;
        }

        const Chunk& c = chunks_[static_cast<size_t>(ci)];
        const int64_t offsetFrames = (wallMs - c.startMs) * 48 + subFrame;
        int64_t avail = c.frames - offsetFrames;
        if (avail <= 0) {
            // 理论上不会走到（wallMs 落在块内），防御一下
            const int64_t fill = want - written;
            std::memset(out + written * kChannels, 0,
                        static_cast<size_t>(fill) * kBytesPerFrame);
            written += fill;
            continue;
        }
        int64_t take = want - written;
        if (take > avail) take = avail;

        std::ifstream in(FromU8(c.path), std::ios::binary);
        if (!in) {
            std::memset(out + written * kChannels, 0,
                        static_cast<size_t>(take) * kBytesPerFrame);
            written += take;
            continue;
        }
        in.seekg(static_cast<std::streamoff>(offsetFrames) * kBytesPerFrame, std::ios::beg);
        const std::streamsize wantBytes = static_cast<std::streamsize>(take) * kBytesPerFrame;
        in.read(reinterpret_cast<char*>(out + written * kChannels), wantBytes);
        const std::streamsize got = in.gcount();
        if (got < wantBytes) {
            const size_t gotFrames = static_cast<size_t>(got) / kBytesPerFrame;
            std::memset(out + (written + static_cast<int64_t>(gotFrames)) * kChannels, 0,
                        static_cast<size_t>(take - static_cast<int64_t>(gotFrames)) * kBytesPerFrame);
        }
        written += take;
    }
    return static_cast<size_t>(written);
}

} // namespace shadow
