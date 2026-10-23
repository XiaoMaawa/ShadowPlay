// AudioCapture.h —— 声音采集（WASAPI）。
//
// 需求（主人原话）：
//   「显示器捕获采用的是系统全局声音，就是各个窗口混合在一起的声音，而窗口
//     捕获，只需要捕获当前窗口 hwnd 的窗口的声音，只对当前捕获着的窗口捕获
//     声音。其他窗口不进行捕获。声音只需要导出到视频里，预览不需要播放声音。」
//
// 所以这里有两套通道：
//   * 系统混音（system loopback）：默认渲染设备的 loopback。整屏/显示器源用它，
//     录到的是所有正在出声的程序混在一起的声音。
//   * 进程环回（process loopback）：Win10 2004 (19041) 起提供的
//     AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK，按**目标进程树**抓声音。
//     窗口源用它，只录被捕获那个窗口（及其子进程）的声音，别的窗口一律不进来。
//
// 【统一格式】不管设备混音格式是什么（常见 48k/32bit float/立体声，也可能是
// 44.1k 或 5.1），这里都转成 **48000Hz / 16bit / 立体声 / 小端** 再交出去。
// 理由：录制端写文件、导出端喂 AAC 都只需要认一种格式；设备中途换格式（拔耳机、
// 切默认设备）也不会让已经录下来的音频对不上。
//
// 【时间戳】每个数据块带一个**墙钟起点（毫秒）**，由采集开始那一刻的墙钟锚点 +
// 已输出的采样数推算。导出端靠它把音频对到视频时间轴上，所以锚点比"实时取
// 当前时间"更稳（后者每块抖动几毫秒，会长出咔哒声）。
//
// 线程模型：Start() 起一条采集线程，线程内自己 CoInitialize（MTA）+ 建 client，
// 按事件驱动收包，转格式后回调 sink。Stop() 会等线程退出。Peak()/Rms() 是
// 给界面用的原子量，任何时候读都安全。
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace shadow {

/** 采集输出格式（固定，见文件头说明）。 */
constexpr uint32_t kAudioRate = 48000;
constexpr uint32_t kAudioChannels = 2;
constexpr uint32_t kAudioBytesPerFrame = kAudioChannels * 2; // S16

class AudioCapture {
public:
    /**
     * 一块 PCM 的去处。
     *
     * @param pcm         交错的 S16LE 采样（立体声，48kHz）
     * @param frames      采样帧数（每帧 2 个 int16）
     * @param startWallMs 这一块第一帧对应的墙钟毫秒
     */
    using SinkFn = std::function<void(const int16_t* pcm, size_t frames, int64_t startWallMs)>;

    AudioCapture();
    ~AudioCapture();

    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    /** 数据出口。必须在 Start() 之前设好（Start 之后设也生效，但会丢前面几块）。 */
    void SetSink(SinkFn fn);

    /**
     * 开始采集。
     *
     * @param processLoopback true = 只抓 pid 那个进程树的声音；false = 系统混音
     * @param targetPid       进程环回时的目标进程（0 会被当成"退回系统混音"）
     * @param outDetail       **实际**走的是哪条通道、或者退回的原因（给界面/日志看）
     *
     * 返回 false 时 outError 有原因。失败是可恢复的：调用方可以之后重试
     * （设备切换、默认设备暂时不可用都会让首次 Activate 失败）。
     */
    bool Start(bool processLoopback, uint32_t targetPid, std::string* outError,
               std::string* outDetail = nullptr);

    /** 停止并等采集线程退出。可重复调用。 */
    void Stop();

    bool Running() const { return running_.load(std::memory_order_relaxed); }

    /** 最近一块的峰值（0..1，线性）。带快速起、缓慢落的包络，界面用它画音量条。 */
    float Peak() const { return peak_.load(std::memory_order_relaxed); }
    /**
     * 取走「自上次取走以来的最大峰值」并清零。
     *
     * 界面是每秒读一次状态的，而 Peak() 的包络在几十毫秒内就衰减掉了 ——
     * 直接读它，每秒采到的几乎都是 0，音量条会一动不动。这个"峰值保持"版本
     * 记录的是这一秒里最响的那一刻，读一次清一次，画出来的条才是"这段时间
     * 有多响"。
     */
    float TakePeak();
    /** 最近若干块的 RMS（0..1，线性），比峰值更能反映"响不响"。 */
    float Rms() const { return rms_.load(std::memory_order_relaxed); }

    /**
     * 实时电平（0..1，线性，**不做**峰值保持）。
     *
     * 和 Peak() 的区别只有一点：这个是给"每秒几十次"的音量条喂数据的，
     * 拿的就是最近一块的包络值；Peak() 那份语义留给每秒一次的状态上报。
     */
    float Level() const { return peak_.load(std::memory_order_relaxed); }

    /**
     * 距离"上一次真的收到音频包"过去了多少毫秒（-1 = 还没收到过）。
     *
     * 进程环回有个讨厌的脾气：目标进程树里**新出现**的子进程（Chromium 的
     * audio.mojom.AudioService 就是按需创建、闲置几秒就被回收的）不一定能被
     * 已经建好的那条环回流看到 —— 那种情况不报错、running_ 还是 true、
     * 拿到的缓冲全是 0 或者干脆没有包。界面只能看到"有声音设备但一直静音"。
     * Session 用它做看门狗：太久没包就重建一次采集（重建时重新抓一次进程树）。
     */
    int64_t PacketGapMs() const;

    /**
     * 目标进程树里「启动之后才冒出来」的子进程个数（0 = 树没变，系统混音恒为 0）。
     *
     * 进程环回在**创建那一刻**就把进程树定死了（PROCESS_LOOPBACK_MODE_
     * INCLUDE_TARGET_PROCESS_TREE）。Chromium 系应用却会在播放音频时才懒加载
     * 一个 audio.mojom.AudioService 子进程 —— 它要是晚于环回出现，就永远进不来，
     * 表现是"这个窗口明明在响，录出来却没有声音"，而且关掉重开一次可能又好了。
     *
     * 这里每秒数一次新子进程，Session 拿它做"树变了就重建环回"的判据：
     * 比等到"连续 3 秒没包"再重建快得多，能少丢一两秒声音。
     */
    int NewChildCount() const;

    /** 累计交付的采样帧数（自检/诊断用）。 */
    uint64_t FramesDelivered() const { return framesOut_.load(std::memory_order_relaxed); }

    /** 采集通道说明（"系统混音" / "窗口进程环回" / 退回原因）。 */
    std::string Detail() const;

private:
    void ThreadProc();

    SinkFn sink_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<float> peak_{0.0f};
    /** 峰值保持（读一次清一次，见 TakePeak）。 */
    std::atomic<float> peakHold_{0.0f};
    std::atomic<float> rms_{0.0f};
    std::atomic<uint64_t> framesOut_{0};
    /** 最近一次收到音频包的墙钟毫秒（0 = 还没收到过），见 PacketGapMs。 */
    std::atomic<int64_t> lastPacketMs_{0};

    // 这些只在采集线程里写，Start/Stop 之间读也无所谓（字符串只在启动阶段
    // 写一次、之后只读，用一把小锁保护字符串本身）。
    mutable std::mutex detailMu_;
    std::string detail_;
    bool  useProcessLoopback_ = false;
    uint32_t targetPid_ = 0;
    /** 建环回那一刻目标进程树里的所有 pid（见 NewChildCount）。 */
    mutable std::mutex treeMu_;
    std::set<uint32_t> treeIds_;
    void* eventHandle_ = nullptr; // HANDLE，隐藏 windows.h 不在头文件里扩散
};

} // namespace shadow
