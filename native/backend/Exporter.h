// Exporter.h —— 导出（需求 6）：把分片目录里的一圈分片合并成一个 MP4。
//
// 为什么不是"拼字节"：
//   分片是 MPEG-TS（见 SegmentRecorder.h 的说明），本质上是**推流**封装 ——
//   每片都自带时间戳和独立的 IDR。要变成一个能拖动、能分享的 MP4，必须重新
//   换封装。`ffmpeg -f concat -c copy` 正是干这个的：视频流直接拷贝，不重新
//   编码，所以导出是秒级的（30 秒录像约 1.5 秒），也不会损失画质。
//
// 为什么导出前要先"快照"（把分片复制到临时目录）：
//   录制是活着的。分片在环形复用（`-segment_wrap N`），导出到一半时 ffmpeg
//   可能正好把我们正在读的那一片覆盖掉，导出的视频就会出现花屏或者时长缩短。
//   复制到临时目录虽然多花一次磁盘 IO，但换来的是"导出内容前后自洽"这件事，
//   而且复制时校验字节数，能把"正被写坏的片"挑出来重试。
//
// 命名参考 NVIDIA 即时重放：`Replay_2026.10.16_18-03-12.mp4`。
// 配置名可以做前缀（`desktop-01_Replay_...`），多路配置同秒导出就不会互相覆盖；
// 仍然重名时自动加 `-2`、`-3`。
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace shadow {

// 一次导出的状态。会被序列化给界面看（NDJSON 的 export 字段）。
struct ExportStatus {
    bool        running = false;
    bool        ok = false;         // 已完成且成功
    bool        failed = false;
    std::string phase;              // 人类可读："快照分片" / "合并" / ...
    std::string outPath;            // 成功后的 MP4 绝对路径
    std::string detail;             // 失败原因 / 摘要
    uint32_t    segmentCount = 0;   // 参与合并的分片数
    double      durationSeconds = 0.0;
    uint64_t    bytes = 0;          // 产物字节数
    double      elapsedMs = 0.0;
    uint64_t    jobId = 0;          // 递增；界面靠它区分"这是不是新的一次"
};

/**
 * 把诊断日志的出口接进来（服务启动时传 main.cpp 的 LogLine）。
 *
 * 不接就静默 —— Exporter 不直接依赖服务进程的 stdout，探针和单测才能复用。
 * 现在只用来打 export_plan：计划里到底铺了哪些片、哪些位置补了 NO SIGNAL。
 */
void SetExporterPlanLog(std::function<void(const std::string&)> sink);

class Exporter {
public:
    Exporter(std::string segmentsDir, std::string exportDir, std::string prefix,
             uint32_t maxSeconds, uint32_t targetFps = 0, uint32_t segmentSeconds = 2);
    ~Exporter();

    Exporter(const Exporter&) = delete;
    Exporter& operator=(const Exporter&) = delete;

    /**
     * 起一次导出（后台线程）。
     *
     * 返回 false 表示**这一次没接**：上一轮还在导，或者没有分片可导。
     * 返回 true 只是"开工了"，成败要看 Status()。
     */
    bool Start();

    /** 状态快照（加锁拷贝，频率别太高）。 */
    ExportStatus Status() const;

    /** 等这一轮结束，最多等 timeoutMs 毫秒。自检和退出流程用。 */
    bool WaitIdle(uint32_t timeoutMs) const;

private:
    void Run();
    void SetPhase(const std::string& phase);
    void Finish(bool ok, const std::string& detail, const std::string& outPath,
                const std::string& segmentsDirUsed, const std::string& listFile);

    std::string segmentsDir_;   // 源：正在录的分片目录
    std::string exportDir_;     // 目标：预设导出目录
    std::string prefix_;        // 文件名前缀（配置名）
    uint32_t    maxSeconds_;    // 最多导出多少秒（0 = 全部）
    // 导出成品的帧率（0 = 不重采样，直接用分片原本的帧率）。
    //
    // 为什么要指定：用户随时可能改帧率（60 -> 240），而分片里存的是**录制当时**
    // 的帧率。要让"改完立刻生效"，导出时就得按**当前配置的帧率**重建时间轴：
    // 60fps 的老分片按 240fps 输出，ffmpeg 会把每一帧重复 4 次（等价于重复
    // 插帧），成品就是 240fps 的流，播放器看到的时长和真实时长一致。
    uint32_t    targetFps_ = 0;
    // 单片标称时长（秒），默认 2。
    //
    // 用来判"最后一片写完了没"：ffmpeg 的 segment muxer 边写边切，磁盘上总有
    // 一片可能还在写。判据是**读这一片自己的时长**，再跟"标称时长 × 90%"比。
    // 以前靠 mtime 猜，把刚切完、其实完整的 2 秒整片也丢掉，于是 20 秒的缓冲
    // 导出来只有 18 秒 —— 用户报的"少两秒"就是它。
    uint32_t    segmentSeconds_ = 2;

    mutable std::mutex mu_;
    ExportStatus       status_;
    uint64_t           lastJobId_ = 0;
    std::thread        worker_;
};

/** 导出文件名里的时间戳：`2026.10.16_18-03-12`。
 *  独立成函数是为了让自检能直接验格式，不用真的导一次。 */
std::string ReplayTimeStamp();

/** 完整文件名：`<前缀>Replay_<时间戳>.mp4`，prefix 为空则不带前缀。
 *  独立成函数同上（自检可直接验命名规则）。 */
std::string ReplayFileName(const std::string& prefix, const std::string& stamp);

} // namespace shadow
