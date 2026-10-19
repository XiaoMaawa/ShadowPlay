// SegmentRecorder.h —— 分片录制（需求 5）：滚动落地 fMP4 分片，自动删旧。
//
// 设计（见 DESIGN.md 5.2）：
//
//   test/
//     .index.json                 <- profileId -> 目录名 的分配表，保证子目录不冲突
//     desktop-01/
//       segments/
//         1/                      <- 每一轮录制一个子目录（代次号）
//           000000.mp4
//           000001.mp4
//           index.csv             <- 录制端自维护的分片清单
//            run.json              <- 本轮的 fps / 尺寸 / 分片时长
//         2/
//           ...
//       meta.json
//
// 为什么要按轮次分子目录（别改回平铺）：
//   用户改帧率后希望"前面的录像也一起导出，用插帧/抽帧补到新帧率"，而不是
//   一改帧率就把上一段扔掉重录。但槽位循环复写是**按槽位号**覆盖的 ——
//   新一轮从 000000.mp4 开始写，会直接把上一轮的片子覆盖掉。
//   所以每一轮必须有自己的目录，导出时按 mtime 把所有轮次的分片串起来。
//   帧率记在 run.json 里（目录名只留代次数字）。
//
// 【为什么现在没有 ffmpeg 子进程了】
//   需求明确要求"不要 exe、不要子进程"。原来靠 `ffmpeg -f segment -segment_wrap N`
//   白拿「按固定时长切分 + 环形覆盖 + 写清单」三件事，现在这三件事由
//   MfEncoder（Media Foundation SinkWriter）在进程内做，一个外部依赖都没有。
//   分片容器从 .ts 换成 .mp4（fMP4）：MF 的 SinkWriter 支持写 fMP4 但**不支持
//   写 TS**（见 MfEncoder.h 里的 SDK 查证）。fMP4 每个 moof 自带索引，进程被
//   杀掉也能读，和 TS 的流式自描述等价。
//
// 【为什么喂 BGRA 而不是 NV12】
//   采集端拿到的就是 BGRA。实测 SinkWriter 内部把 BGRA 转 NV12（必要时落到
//   GPU）1920x1080 一帧 5.87ms、等效 170fps，够 1080p60 用；自己写一份 CPU
//   转换只会更慢，还容易把 BT.601/709 搞错导致颜色发绿。
//
// 线程模型：Feed() 在采集会话线程里调用，但它**只把帧投进待喂槽就返回**，
// 真正编码的是录制器自己的喂帧线程。
//
// 为什么必须解耦（实测数据，别改回去）：
//   原来 Feed() 是同步写 ffmpeg 的 stdin。管道写满时 write 阻塞，这个阻塞
//   就发生在采集循环里 —— 实测 WGC 源 1545x773（一帧 4.78MB）时，采集循环
//   周期从 15.2ms 涨到 29.5ms，帧率 60 → 45。现在编码是同步的（WriteSample
//   会等编码器收下），如果放在采集线程里会重现同一个问题，所以解耦照旧。
//
// 解耦之后：采集循环只管自己的节拍，喂帧线程按严格 fps_ 出图（时间轴恒定），
// 槽容量 1 且覆盖式 —— 编码掉队时丢的是**旧帧**，不会拖慢采集、不会涨内存。
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Profiles.h"

namespace shadow {

class MfEncoder;

/**
 * 把 profileId 映射到一个互不冲突的录像子目录（需求 5）。
 *
 * 目录名规则（DESIGN.md 7）：`<源类型>-<简短名字>-<id 后4位>`，重名加 `-2`、`-3`。
 * 分配结果记在 `<recordRoot>/.index.json` 里，重启服务后同一个配置复用同一个
 * 目录（录像连续），删配置也不删目录（只标记归档）。
 *
 * 返回子目录的**绝对路径**；失败时返回空串并填 outError。
 */
std::string AllocateProfileDir(const std::string& recordRoot, const Profile& profile,
                               std::string& outError);

// 一路会话的录制状态（会被序列化给界面看）。
struct RecorderStats {
    bool     active = false;       // 编码器还在工作
    bool     failed = false;       // 起不来或中途挂了
    std::string detail;            // 失败原因 / 当前状态说明
    uint32_t segmentSeconds = 0;   // 每个分片目标时长
    uint32_t keepSegments = 0;     // 环形保留的分片数（= 回放时长 / 分片时长）
    uint64_t framesIn = 0;         // 成功编码进去的帧数
    uint64_t bytesIn = 0;          // 喂进去的字节数（未压缩）
    uint64_t droppedFrames = 0;    // 因为采不到/尺寸不符丢掉的帧
    uint32_t segmentsOnDisk = 0;   // 当前目录里的分片数
    uint64_t diskBytes = 0;        // 分片目录总字节数
};

/**
 * 一个配置对应一个录制器：一个 MfEncoder + 一个喂帧线程。
 *
 * 生命周期：构造 -> Start()（起编码器，失败不抛异常，看 Stats()）-> Feed()...
 *  -> Stop()（收尾最后一个分片）-> 析构。
 */
class SegmentRecorder {
public:
    SegmentRecorder(std::string dir, VideoSpec video, uint32_t width, uint32_t height);
    ~SegmentRecorder();

    SegmentRecorder(const SegmentRecorder&) = delete;
    SegmentRecorder& operator=(const SegmentRecorder&) = delete;

    /**
     * 起编码器。返回是否成功；失败原因在 Stats().detail 里。
     * 成功后喂帧线程**自动**起好，调用方只管 Feed() / Stop()。
     */
    bool Start();

    /**
     * 投一帧 BGRA 进待喂槽。**立刻返回，不等编码**（见文件头的线程模型）。
     *
     * 尺寸和当前录制尺寸不同时**不丢帧**：把新尺寸一起投进槽里，喂帧线程
     * 会通知编码器切一片新的、按新尺寸接着录（见 DESIGN.md 9.5）。
     * 这样窗口拖大拖小不会中断录制、也不会换轮次目录。
     *
     * pixels 为 nullptr 时返回 false 并计一次丢帧。
     */
    bool Feed(const void* pixels, uint32_t stride, uint32_t width, uint32_t height);

    /** 收尾最后一个分片并等喂帧线程退出。可重复调用。 */
    void Stop();

    /**
     * 让录制器**立刻**把正在写的那一片收尾落盘，然后接着开新片继续录。
     *
     * 【为什么需要】用户点"导出"时，回放窗口的右端应该是他按下按钮的那一刻。
     * 但切片是按固定片长的：点导出时总有一片正在写，它没 Finalize、index.csv
     * 里也还没有它，导出端只能读到最后一片**已完成**的分片 —— 于是"点导出
     * 之前那半秒到一秒"永远导不出来。用户原话：「我点击导出视频的时候，
     * 前一秒，或者说前半秒，画面都丢失了，应该一直录到我点击按钮才停止才对啊」。
     *
     * 【为什么不在调用方线程直接切片】MfEncoder 的 SinkWriter 只由喂帧线程
     * 碰（Write / CloseSegment / OpenSegment）。从命令线程直接调 Finalize
     * 会和正在进行的 WriteSample 撞车。所以这里只**置请求**，真正切片的动作
     * 由喂帧线程在两帧之间执行，调用方等它完成（最多 timeoutMs）。
     *
     * 返回 true = 真的切了一片；false = 没在录 / 当前片一帧都没有 / 等超时。
     */
    bool FlushSegment(uint32_t timeoutMs = 2000);

    bool Running() const;

    /** 取状态快照（会顺手扫一遍分片目录，别用太高的频率调）。 */
    RecorderStats Stats() const;

    /** 该录制的落地目录（绝对路径），没有就是空串。 */
    const std::string& Dir() const { return dir_; }

    /** 分片清单文件（`<轮次目录>/index.csv`），导出时按它取顺序。 */
    std::string ListFilePath() const;

    /** 分片**根**目录（segments/），下面按轮次分子目录。导出从这层开始扫。 */
    std::string SegmentsDir() const;

private:
    void RefreshDiskStatsLocked() const;

    // 起录前清掉本轮的运行目录，并按回放窗口回收太老的轮次目录。
    void ClearStaleSlots();

    // 喂帧线程主体：按 fps_ 的节拍从槽里取最新帧送进编码器。
    void FeederLoop();
    // 起喂帧线程。Start() 成功之后内部调用。
    void StartFeeder();
    // 把槽里的帧送进编码器。返回 false 表示编码器坏了。
    bool EncodeFrame(const std::vector<uint8_t>& frame);

    std::string dir_;
    std::string segmentsDir_;   // <dir>/segments —— 分片根目录，下面按轮次分子目录
    std::string runDir_;        // <segmentsDir>/<轮次名> —— 本轮真正写的地方
    std::string listFile_;
    VideoSpec   video_;
    uint32_t    width_ = 0;
    uint32_t    height_ = 0;
    uint32_t    fps_ = 0;      // 喂帧声明的帧率（0 -> 用 60）

    // 编码器。用 unique_ptr 是为了让头文件不必包含 MfEncoder.h
    // （它会把 mfreadwrite.h 拖给所有包含者），也方便 Start() 失败后重建。
    std::unique_ptr<MfEncoder> encoder_;

    mutable std::mutex mu_;
    mutable RecorderStats stats_;
    bool started_ = false;
    bool stopped_ = false;

    // ---- 喂帧线程与待喂槽 ------------------------------------------------
    // 槽容量 1：新帧直接覆盖旧的，只保留"最新的那一张"。录像要的是时间轴
    // 连续，不是每一帧都非到不可 —— 掉队时丢旧帧比积压一队帧好得多
    // （积压会让画面滞后于真实时间，回放和实际对不上）。
    std::thread feeder_;
    std::mutex slotMu_;
    std::condition_variable slotCv_;
    std::vector<uint8_t> slot_;      // 当前待喂帧（紧凑排布，stride = width*4）
    bool slotFull_ = false;
    bool feedStop_ = false;          // 通知喂帧线程收工
    uint64_t slotSeq_ = 0;           // 投进来的序号，用来判断有没有取到新的
    uint64_t slotTakenSeq_ = 0;      // 喂帧线程已经取走的序号

    // 槽里那一帧的尺寸。
    //
    // 【为什么要跟着帧走，而不是用成员 width_/height_】窗口拖大拖小之后，
    // 采集端给的尺寸就变了。以前 Feed() 拿它跟固定的 width_ 比，不相等就
    // 直接丢 —— 上层唯一的办法是重建整个录制器，那会换轮次目录、切片从
    // 000000 重来（用户报的「调完窗口回放里前面全没了」）。现在尺寸跟着
    // 每一帧走，编码器自己负责切尺寸。
    uint32_t slotW_ = 0;
    uint32_t slotH_ = 0;

    // ---- 立刻切片请求（导出前用）----------------------------------------
    // 命令线程置 flushWant_，喂帧线程在两帧之间看到就调 encoder_->FlushSegment()，
    // 完成后置 flushDone_ 并唤醒等着的命令线程。
    //
    // 用"序号"而不是布尔：连续点两次导出时，第二次请求不会被第一次的完成
    // 信号误当成"已经做过"（布尔置回 false 的时序很容易漏掉一次请求）。
    std::mutex flushMu_;
    std::condition_variable flushCv_;
    uint64_t flushWant_ = 0;   // 请求序号
    uint64_t flushDone_ = 0;   // 已完成到的序号
    bool flushOk_ = false;     // 最近一次切片的成败
};

} // namespace shadow
