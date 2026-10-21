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
    // 停机时从待喂队列里补写进编码器的帧数（见 Stop 里的注释）。
    // 0 表示停机那一刻队列正好是空的；非 0 说明"最后这一两百毫秒"被救回来了。
    uint64_t drainedOnStop = 0;
    uint32_t segmentsOnDisk = 0;   // 当前目录里的分片数
    uint64_t diskBytes = 0;        // 分片目录总字节数
    // 码率（Mbps）。requested 是配置值（0 = 没配、走估算），effective 是编码器
    // 真正在用的值。两个都报出来，界面才能确认「我配的 5 Mbps 真的落地了」，
    // 而不是像以前那样被 SetFps/ResizeTo 偷偷换成估算值（m14071）。
    uint32_t requestedBitrateMbps = 0;
    uint32_t effectiveBitrateMbps = 0;
    // 帧率同上：requested 是配置值，effective 是编码器被硬件吞吐上限降档后的值。
    uint32_t requestedFps = 0;
    uint32_t effectiveFps = 0;

    // ---- 「到底丢没丢帧」的两个计数 --------------------------------------
    //
    // 用户的要求（m15478）：「我可以理解降帧率，但是最好不要丢帧」。
    // 「丢帧」其实有两种，表现完全不同，必须分开数才说得清：
    //
    //   repeatFrames  编码时间轴上的重复帧。时间轴必须严格等于 fps × 真实
    //                 时长，喂帧线程落后时只能拿"上一帧"补足；补出来的这些
    //                 就是重复帧 —— 帧数一个不少，但画面停住了几拍，观感
    //                 是"卡"。这个数除以 framesIn 就是重复率，0 表示每一
    //                 拍都是新画面。
    //
    //   captureDropped 采集到、但没进编码器的帧。采集比编码快时（240 帧
    //                 采集 vs 165 帧编码），待喂槽只有一格、新帧覆盖旧的，
    //                 被覆盖掉的那张就永远没编码 —— 这是"降帧率"必然的
    //                 代价，但它不破坏时间轴，只是运动细节少了。
    //
    // 两者都为 0 才叫真的"既不卡、也不变速"。
    uint64_t repeatFrames = 0;
    uint64_t captureDropped = 0;

    // 采集端一共投进来多少帧（Feed 成功次数）。
    //
    // 【为什么必须单独数】repeatFrames / framesIn 只能说明"编码时间轴上有多少
    // 拍是重复的"，但重复的成因有两种，修法完全不同：
    //   * 采集端本来就没给到那么多帧（采集循环跑不到配置帧率）→ 要高的是采集；
    //   * 采集端给了，但喂帧线程取不到（节拍相位错开）→ 要改的是取槽逻辑。
    // 有了 captureFrames 一比就清楚：captureFrames 远小于 framesIn 就是前者。
    uint64_t captureFrames = 0;
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

    /**
     * 就地改帧率（不重建录制器、不换轮次目录、分片序号继续递增）。
     *
     * 【为什么需要】界面把帧率从 144 改成 60 时，服务走的是 reload。原来
     * reload 判「配置变了」就整路拆掉重建：新轮次目录、recordSegments 归零、
     * 时间轴出现空洞 —— 于是用户看到「短暂的 no signal」「导出按钮被禁用」
     * 「刚切过去点导出报该路未在录制」（m13003）。
     *
     * 现在改帧率不再重建会话（见 Profile::SameSettings），由 Session 直接
     * 调这里：喂帧线程在两帧之间看到请求就调 encoder_->SetFps()，编码器收尾
     * 当前片、按新帧率开新片，时间轴靠 nextSegStartUnix_ 继续顺延。
     *
     * 返回 true = 已经按新帧率工作（或帧率本来就没变）；false = 没在录 / 超时。
     */
    bool SetFps(uint32_t fps, uint32_t timeoutMs = 3000);

    /**
     * 就地改码率（不重建录制器、不换轮次目录、分片序号继续递增）。
     *
     * 与 SetFps 完全同一套机制（序号 + 条件变量），只是改的是码率：
     * 喂帧线程在两帧之间调 encoder_->SetBitrate()，编码器收尾当前片、按新
     * 码率开新片，时间轴靠 nextSegStartUnix_ 继续顺延。
     *
     * 【为什么需要】码率原来只写在 MfEncoder::Start() 的入参里，改它只能靠
     * reload 把整路 Session 拆掉重建（新轮次目录、recordSegments 归零、导出
     * 按钮变灰）。用户的原话（m14071）：「有关编辑的东西一保存立刻落地，像
     * 配置改帧率，改码率什么的」。
     *
     * 返回 true = 已经按新码率工作（或码率本来就没变）；false = 没在录 / 超时。
     */
    bool SetBitrate(uint32_t bitrateMbps, uint32_t timeoutMs = 3000);
    bool Running() const;

    /**
     * 编码器**当前真正在用的**帧率（已经被硬件吞吐上限降档过的值）。
     *
     * 没在录、或还没起编码器时返回 0。
     *
     * 【为什么要暴露它】采集节拍必须夹到这个值上，否则采集比编码快的那部分
     * 帧全被待喂槽顶掉（实测 240 采集 / 165 编码，22 秒丢掉 2727 帧）。
     * 用户的要求是「可以降帧率，但最好不要丢帧」（m15478）。
     * 见 Session.cpp 采集循环里 wantFps 的夹取。
     */
    uint32_t CurrentFps() const;

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

    // 补一帧"和上一帧一样"的画面：让编码器复用样本缓冲里已有的 NV12，
    // 只换时间戳再 WriteSample 一次。返回 false 表示编码器坏了。
    //
    // 为什么单独一个函数而不是 EncodeFrame(nullptr)：两个函数的统计口径不同
    // （补帧不算 bytesIn，因为没有新的像素进来），而且调用点彼此独立、
    // 混在一起读代码的人容易误以为补帧也会拷像素。
    bool EncodeRepeat();

    std::string dir_;
    std::string segmentsDir_;   // <dir>/segments —— 分片根目录，下面按轮次分子目录
    std::string runDir_;        // <segmentsDir>/<轮次名> —— 本轮真正写的地方
    std::string listFile_;
    VideoSpec   video_;
    uint32_t    width_ = 0;
    uint32_t    height_ = 0;
    // 喂帧线程用的帧率**不在这里** —— 它改成了 fps_（见下面「改帧率请求」
    // 那段里的 atomic 成员）。改帧率是热操作：命令线程随时会写、喂帧线程
    // 每一拍都要读它算节拍，放普通成员会有数据竞争。

    // 编码器。用 unique_ptr 是为了让头文件不必包含 MfEncoder.h
    // （它会把 mfreadwrite.h 拖给所有包含者），也方便 Start() 失败后重建。
    std::unique_ptr<MfEncoder> encoder_;

    mutable std::mutex mu_;
    mutable RecorderStats stats_;
    bool started_ = false;
    bool stopped_ = false;

    // ---- 喂帧线程与待喂队列 ----------------------------------------------
    //
    // 【为什么不是"只留最新一帧"的单个槽】原来是槽容量 1：新帧直接覆盖旧的。
    // 那套在"偶尔掉一拍"时没问题，但切片轮转（每 2 秒一次）会把喂帧线程原地
    // 堵 150~250ms（开新分片要重建 SinkWriter，硬件 MFT 初始化 120~170ms；
    // 收旧分片 Finalize 37~62ms）。堵住期间采集端照常出帧，全被那一格槽顶掉，
    // 事后喂帧线程只能按"墙钟 × 帧率"补**重复帧**—— 用户看到的就是
    // 「每两秒画面顿一下」（实测 60fps 一次补 13~23 帧）。
    //
    // 现在改成一个小环形队列：堵住期间的真实画面留在队列里，恢复后按顺序
    // 补进去，画面是连续的，不是定格。
    //
    // 【深度怎么定】只要求盖住一次轮转的堵塞，不是给整段录像做缓冲。
    // 深度取「时间」和「内存」两个上限里小的那个：
    //   * 时间：约 0.6 秒（60fps 就是 36 帧）—— 轮转实测 150~250ms，
    //     再加上恢复后逐帧做 BGRA→NV12（1440p 一帧 8~9ms）的追赶时间；
    //   * 内存：kPendingBytes。
    // 两个都夹在 [4, 40] 里，再多就是白吃内存（队列越深，画面滞后越明显）。
    //
    // 【为什么从 96MB 加到 256MB】96MB 在 1440p 下一帧 14.7MB，深度只有 6
    // 帧 = 100ms，根本盖不住 250ms 的轮转 —— 剩下的 150ms 仍然补重复帧，
    // 实测 60fps 每 2 秒还有 3 帧重复（`need=10,take=7,rep=3`）。深度必须
    // 明显大于一次轮转的时长，那段动作戏才不会被定格。
    struct PendingFrame {
        std::vector<uint8_t> buf;   // 紧凑 BGRA
        uint32_t w = 0;
        uint32_t h = 0;
    };
    static constexpr size_t kPendingBytes = 256u * 1024u * 1024u;
    static constexpr double kPendingSeconds = 0.6;

    std::thread feeder_;
    std::mutex slotMu_;
    std::condition_variable slotCv_;
    std::vector<PendingFrame> ring_;   // 待喂帧环形队列（容量 ringDepth_）
    uint32_t ringDepth_ = 0;
    uint64_t ringWrite_ = 0;           // 采集端写入的总帧数
    uint64_t ringRead_ = 0;            // 喂帧线程取走的总帧数
    uint32_t ringW_ = 0;               // 最近一帧的尺寸（尺寸变了要重算深度）
    uint32_t ringH_ = 0;

    // 采集线程的私有暂存区（Feed() 先拷到这里，再在锁里 O(1) 换给队列）。
    //
    // 【为什么要有它】原来 Feed() 整段 memcpy 都在 slotMu_ 里面，1440p 一帧
    // 14.7MB、实测约 1.5ms；而喂帧线程取帧要的正是这把锁，等于每次投帧都把
    // 它堵住 1.5ms。165fps 每帧预算只有 6.06ms，一堵就吃掉四分之一，喂帧
    // 线程于是总"晚到"、槽里永远是上一拍那帧，只能补重复帧 —— 实测
    // repeatFrames 占到一半，真实画面只剩 50%。
    //
    // 只有一个生产者（Session 的采集线程按路各一个），所以这块缓冲不需要
    // 额外保护：换出去之后队列里那格是刚写满的，writeBuf_ 里是那格原来的
    // 旧缓冲（下次 resize/写会被直接覆盖），两边不重叠。
    std::vector<uint8_t> writeBuf_;
    bool feedStop_ = false;          // 通知喂帧线程收工

    // 采集端投进来、但没进编码器的总帧数（见 RecorderStats::captureDropped）。
    // 两个来源：① 队列满时顶掉最旧的；② 喂帧线程落后不多时只取最新那帧，
    // 中间几帧被跳过。两种都是"采到了但没进编码器"，口径一致。
    //
    // 【为什么是 atomic 而不是普通 uint64_t】Stats() 是 const 成员、拿不到
    // slotMu_（const 方法里不能锁非 mutable 的 mutex），而为了读一个统计数字去
    // 把 slotMu_ 改成 mutable 会引入一条新的锁顺序（mu_ → slotMu_），
    // 喂帧线程那边是 slotMu_ → mu_，容易死锁。计数只增不改，用原子最省事。
    std::atomic<uint64_t> slotOverwritten_{0};

    // 采集端投进来的总帧数（Feed 成功次数），只为把「采集端到底给了多少帧」
    // 报出来，跟 captureDropped 一起判成因。同样用原子，理由同上。
    std::atomic<uint64_t> slotFed_{0};

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

    // ---- 改帧率请求（界面改配置时用）--------------------------------------
    //
    // 与 flush 同一套「序号 + 条件变量」的机制：命令线程置 wantFps_/fpsWant_，
    // 喂帧线程在两帧之间看到就调 encoder_->SetFps()，完成后置 fpsDone_。
    //
    // 【为什么不在命令线程直接调】跟 FlushSegment 一样：SinkWriter 只由喂帧
    // 线程碰，从命令线程直接 Finalize 会和正在进行的 WriteSample 撞车。
    //
    // 用 atomic 是因为喂帧线程每一拍都要读 fps_ 算节拍，加锁不值得。
    std::atomic<uint32_t> fps_{0};        // 喂帧线程当前用的帧率（0 -> 60）
    std::mutex fpsMu_;
    std::condition_variable fpsCv_;
    uint32_t fpsWantValue_ = 0;           // 命令线程要改成多少
    uint64_t fpsWant_ = 0;                // 请求序号
    uint64_t fpsDone_ = 0;                // 已完成到的序号
    bool fpsOk_ = false;                  // 最近一次改帧率的成败

    // ---- 改码率请求（界面改配置时用）--------------------------------------
    //
    // 与改帧率同一套机制：命令线程置 brWantValue_/brWant_，喂帧线程在两帧
    // 之间看到就调 encoder_->SetBitrate()，完成后置 brDone_。
    //
    // 为什么不跟 fps 共用一组变量：两个请求可能同时到达（用户一次把帧率和
    // 码率都改了），共用序号会让后一个把前一个的「已完成」误当成自己的。
    std::mutex brMu_;
    std::condition_variable brCv_;
    uint32_t brWantValue_ = 0;            // 命令线程要改成多少 Mbps（0 = 自动估）
    uint64_t brWant_ = 0;                 // 请求序号
    uint64_t brDone_ = 0;                 // 已完成到的序号
    bool brOk_ = false;                   // 最近一次改码率的成败
};

} // namespace shadow
