// AudioRing.h —— 声音的滚动落地 / 读出。
//
// 【为什么不把音频塞进分片 mp4 里】
//   分片是"顺着录、随时可能被环形覆盖"的东西，而导出端会跨片拼一条连续
//   时间轴。音频要是也按分片切，每片都得重开一个 AAC 编码器，AAC 每次启动
//   都带一段 priming（约 20ms）—— 2 秒一片的话，成片里每 2 秒就多一个
//   20ms 的空隙，听感上就是规律性的"顿一下"。所以音频独立落地：
//
//     <录像目录>/audio/a_<起始墙钟毫秒>.pcm
//
//   格式固定 **raw PCM，48000Hz / 16bit / 立体声 / 小端**（见 AudioCapture.h），
//   不带 wav 头 —— 文件里每个字节都是采样，断电/被杀时留下的是"短一点的音频"
//   而不是"头坏了整段读不出来"。时长靠文件大小算（每帧 4 字节），起始时刻
//   直接写在文件名里，导出端按名字排序就知道时间轴。
//
// 【滚动删除】和分片同一套语义：只保留回放窗口内的内容，Purge() 按墙钟删。
//   48k/16bit/立体声 = 192KB/s，20 秒窗口稳态只占 ~4MB，可以忽略。
//
// 线程模型：Append() 在音频采集线程里调，Purge() 在会话的状态刷新线程里调，
// 两者用同一把锁串行化（单块写入是几微秒，不会拖慢采集）。
#pragma once

#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace shadow {

/** 音频目录相对分片目录的位置：与 `segments` 平级的 `audio`。 */
std::string AudioDirForSegmentsDir(const std::string& segmentsDir);

/**
 * 把 PCM 按固定时长切成文件、按回放窗口滚动删除。
 */
class AudioRing {
public:
    AudioRing() = default;
    ~AudioRing();

    AudioRing(const AudioRing&) = delete;
    AudioRing& operator=(const AudioRing&) = delete;

    /** dir 为空或建不出来时返回 false（录制照常，只是没有声音）。 */
    bool Open(const std::string& dir, uint32_t keepSeconds, uint32_t chunkSeconds = 2);

    /** 追加一块 PCM（立体声 S16/48k）。startWallMs 是这块第一帧的墙钟毫秒。 */
    void Append(const int16_t* pcm, size_t frames, int64_t startWallMs);

    /** 删掉跌出回放窗口的文件。nowWallMs 传 0 表示用系统当前时间。 */
    void Purge(int64_t nowWallMs = 0);

    /**
     * 导出期间"钉住"某时刻之后的内容（0 = 取消）。
     *
     * 导出不是一次性把音频读完的：AudioMux 跟着视频进度一块一块地读
     * （见 Exporter.cpp 的背压说明）。一条两分钟的成片要读几十秒，这期间
     * 滚动删除照跑 —— 要是把"还没读到"的块删了，成片里就会突然静音一整块
     * （2 秒），听起来就是卡了一下。所以导出开始时钉个下限，结束再放开。
     * 钉住期间多占的空间 = 导出时长 * 192KB/s，可以忽略。
     */
    static void SetExportPin(int64_t floorMs);

    /** 收尾：把当前文件关掉（下次 Append 会重新开）。 */
    void Close();

    bool Ok() const;
    uint64_t Bytes() const;
    uint32_t Chunks() const;

private:
    void Rotate(int64_t startWallMs);

    /**
     * 删除跌出回放窗口的块，并重算 bytes_/chunks_。
     *
     * 调用方必须已经持有 mu_。Append 的"每秒清理一次"和 Purge() 都走这里 ——
     * 以前两份代码是复制粘贴的，改一处忘另一处就会一边删一边不删。
     */
    void PurgeLocked(int64_t nowWallMs);

    mutable std::mutex mu_;
    std::string dir_;
    uint32_t keepSeconds_ = 120;
    uint32_t chunkSeconds_ = 2;
    bool ok_ = false;

    std::ofstream out_;
    int64_t chunkStartMs_ = 0;
    uint64_t chunkFrames_ = 0;
    int64_t lastPurgeMs_ = 0;
    uint64_t bytes_ = 0;
    uint32_t chunks_ = 0;
};

/**
 * 导出端用的音频读取器：把某个墙钟窗口里的 PCM 按顺序吐出来。
 *
 * 窗口里没有音频文件的位置一律吐**静音** —— 这样调用方可以当成"这段一直有
 * 音频"来写，不用自己处理空洞（录制刚开始、切设备、进程还没出声都会缺）。
 */
class AudioClipReader {
public:
    /**
     * @param dir            audio 目录
     * @param windowStartMs  导出时间轴 0 点对应的墙钟毫秒
     * @param windowEndMs    时间轴右端（不含）；0 = 不限，读到最后一个块
     */
    bool Open(const std::string& dir, int64_t windowStartMs, int64_t windowEndMs = 0);

    bool HasAudio() const { return !chunks_.empty(); }

    /** 窗口里最后一个块的结束墙钟（没有音频时返回 windowStartMs）。 */
    int64_t LastEndMs() const;

    /**
     * 读第 sampleIndex 帧起的 frames 帧（48k 立体声 S16，交错）。
     *
     * @return 实际写进 out 的帧数；0 表示已经越过窗口右端（调用方该收尾了）。
     */
    size_t Read(int64_t sampleIndex, int16_t* out, size_t frames);

private:
    struct Chunk {
        std::string path;
        int64_t startMs = 0;
        int64_t frames = 0;
        int64_t endMs() const { return startMs + frames * 1000 / 48000; }
    };

    /** 返回包含 wallMs 的块下标；没有就返回 -1。 */
    int FindChunk(int64_t wallMs) const;
    /** 下一个块的起始墙钟；没有就返回 0。 */
    int64_t NextChunkStartMs(int64_t wallMs) const;

    std::string dir_;
    int64_t windowStartMs_ = 0;
    int64_t windowEndMs_ = 0;
    std::vector<Chunk> chunks_;
};

} // namespace shadow
