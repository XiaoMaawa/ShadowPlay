// SegmentRecorder.h —— 分片录制（需求 5）：滚动落地 TS 分片，自动删旧。
//
// 设计（见 DESIGN.md 5.2）：
//
//   test/
//     .index.json                 <- profileId -> 目录名 的分配表，保证子目录不冲突
//     desktop-01/
//       segments/
//         0001-fps60/             <- 每一轮录制一个子目录（记录当时的帧率）
//           000000.ts
//           000001.ts
//           index.csv             <- ffmpeg 维护的分片清单
//         0002-fps240/
//           000000.ts
//       meta.json
//
// 为什么要按轮次分子目录（别改回平铺）：
//   用户改帧率后希望"前面的录像也一起导出，用插帧/抽帧补到新帧率"，而不是
//   一改帧率就把上一段扔掉重录。但 `-f segment -segment_wrap N` 是**按槽位
//   循环复写**的 —— 新一轮从 000000.ts 开始写，会直接把上一轮的片子覆盖掉。
//   所以每一轮必须有自己的目录，导出时按 mtime 把所有轮次的分片串起来。
//   目录名里带 fps，导出端不跑 ffmpeg 就知道每一段是什么帧率。
//
// 为什么起 ffmpeg.exe 子进程，而不是链 Media Foundation：
//   本机 C:\Windows\system32\ffmpeg.exe 是 full build（带 nvenc/qsv/amf/mf），
//   用它的 `-f segment` muxer 能白拿「按固定时长切分 + 环形覆盖（-segment_wrap）
//   + 自动写清单」三件事。自己实现滚动删除要处理"正在写的那个文件不能被删"
//   的竞态，交给 muxer 就没有这个问题。
//
// 为什么喂 BGRA（不是 NV12）：
//   采集端拿到的就是 BGRA，直接喂省掉一次 CPU 侧色彩转换；
//   转换交给 ffmpeg 的 scale 滤镜（swscale）。
//
// 线程模型：Feed() 在采集会话线程里调用，但它**只把帧投进待喂槽就返回**，
// 真正写管道的是录制器自己的喂帧线程。
//
// 为什么必须解耦（实测数据，别改回去）：
//   原来 Feed() 是同步 WriteFile 到 ffmpeg 的 stdin。管道写满时 write 阻塞，
//   这个阻塞就发生在采集循环里 —— 实测 WGC 源 1545x773（一帧 4.78MB）时，
//   采集循环周期从 15.2ms 涨到 29.5ms，帧率 60 → 45，而且节拍器的
//   workEstimate_ 顶到上限也补不回来（阻塞在采样"发布时刻"之前发生）。
//   同一份配置不接录制器时是 59.97fps，所以瓶颈 100% 是管道背压。
//
// 解耦之后：采集循环只管自己的节拍，喂帧线程按严格 fps_ 出图（时间轴恒定），
// 槽容量 1 且覆盖式 —— ffmpeg 掉队时丢的是**旧帧**，不会拖慢采集、不会涨内存。
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Profiles.h"

namespace shadow {

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
    bool     active = false;       // ffmpeg 子进程还活着
    bool     failed = false;       // 起不来或中途挂了
    std::string detail;            // 失败原因 / 当前状态说明
    uint32_t segmentSeconds = 0;   // 每个分片目标时长
    uint32_t keepSegments = 0;     // 环形保留的分片数（= 回放时长 / 分片时长）
    uint64_t framesIn = 0;         // 成功喂进去的帧数
    uint64_t bytesIn = 0;          // 喂进去的字节数（未压缩）
    uint64_t droppedFrames = 0;    // 因为管道不通丢掉的帧
    uint32_t segmentsOnDisk = 0;   // 当前目录里的分片数
    uint64_t diskBytes = 0;        // 分片目录总字节数
};

/**
 * 一个配置对应一个录制器：一个 ffmpeg 子进程 + 一根 stdin 管道。
 *
 * 生命周期：构造 -> Start()（起进程，失败不抛异常，看 Stats()）-> Feed()...
 *  -> Stop()（关管道、等子进程收尾、写出可分片的尾部）-> 析构。
 */
class SegmentRecorder {
public:
    SegmentRecorder(std::string dir, VideoSpec video, uint32_t width, uint32_t height);
    ~SegmentRecorder();

    SegmentRecorder(const SegmentRecorder&) = delete;
    SegmentRecorder& operator=(const SegmentRecorder&) = delete;

    /**
     * 起 ffmpeg。返回是否成功；失败原因在 Stats().detail 里。
     * 成功后喂帧线程**自动**起好，调用方只管 Feed() / Stop()。
     */
    bool Start();

    /**
     * 投一帧 BGRA 进待喂槽。**立刻返回，不阻塞在管道上**（见文件头的线程模型）。
     * pixels 为 nullptr 或尺寸不符时返回 false 并计一次丢帧。
     */
    bool Feed(const void* pixels, uint32_t stride, uint32_t width, uint32_t height);

    /** 关管道并等 ffmpeg 写完（会 flush 出最后一个不完整分片）。可重复调用。 */
    void Stop();

    bool Running() const;

    /** 取状态快照（会顺手扫一遍分片目录，别用太高的频率调）。 */
    RecorderStats Stats() const;

    /** 该录制的落地目录（绝对路径），没有就是空串。 */
    const std::string& Dir() const { return dir_; }

    /** 分片清单文件（segment_list 的输出），导出时按它取顺序。 */
    std::string ListFilePath() const;

    /** 分片**根**目录（segments/），下面按轮次分子目录。导出从这层开始扫。 */
    std::string SegmentsDir() const;

private:
    void RefreshDiskStatsLocked() const;

    // 起录前清掉本轮的运行目录，并按回放窗口回收太老的轮次目录。
    void ClearStaleSlots();

    // 喂帧线程主体：按 fps_ 的节拍从槽里取最新帧写管道。
    void FeederLoop();
    // 起喂帧线程。Start() 成功之后内部调用。
    void StartFeeder();
    // 把槽里的帧写进管道。返回 false 表示管道断了（ffmpeg 提前退出）。
    // 不持 mu_ 调用（写管道会长时间阻塞，不能把 Stats() 一起堵死）。
    bool WriteFrameToPipe(const std::vector<uint8_t>& frame);

    std::string dir_;
    std::string segmentsDir_;   // <dir>/segments —— 分片根目录，下面按轮次分子目录
    std::string runDir_;        // <segmentsDir>/<轮次名> —— 本轮 ffmpeg 真正写的地方
    std::string listFile_;
    VideoSpec   video_;
    std::string ffmpegPath_;   // 解析出来的 ffmpeg.exe 绝对路径
    uint32_t    width_ = 0;
    uint32_t    height_ = 0;
    uint32_t    fps_ = 0;      // 喂帧声明的帧率（0 -> 用 60）

    // Win32 句柄。用 void* 而不是 HANDLE，免得头文件把 windows.h 拖给所有包含者。
    void* process_ = nullptr;
    void* thread_ = nullptr;
    void* stdinWrite_ = nullptr; // 只留写端；读端在 Start() 里关掉

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
};

} // namespace shadow
