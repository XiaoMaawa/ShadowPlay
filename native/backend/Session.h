// Session.h —— 一路采集会话（一个配置 = 一路）。
//
// 设计要点（见 DESIGN.md 第 4 节）：
//
//   * 每个会话独占一个共享内存帧环，环的名字用**配置 id**，
//     界面端按 id 打开对应环，互不干扰。
//   * 每个会话跑在自己的线程里，目标帧率各配各的。
//   * 采集源目前支持 Desktop（整屏）和 Window（从桌面复制里裁出窗口矩形，
//     见 DESIGN.md 第 3 节对裁剪法与 WGC 的取舍说明）。
//   * 窗口匹配不到 → 会话进入 Disabled 并**主动拆掉帧环**，界面端看到的
//     就是"配置未生效"，这就是需求里的"强制关闭该配置"。
//
// 线程模型：一个 Session 一个 std::thread，主线程只负责启停和汇总状态。
// 所有跨线程读的字段都在 SessionStatus 里，用一把小锁保护。
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Exporter.h"
#include "AudioCapture.h"
#include "AudioRing.h"
#include "FrameRing.h"
#include "Profiles.h"
#include "ScreenCapture.h"
#include "SegmentRecorder.h"
#include "WindowCapture.h"
// WindowInfo（目标窗口的身份快照）在 WindowEnum.h 里。
#include "WindowEnum.h"

namespace shadow {

// 会话所处阶段。会被序列化成 NDJSON 里的 "state" 字段，
// 取值是稳定的英文小写串，前端按它决定显示什么。
enum class SessionState {
    Starting,    // 刚起来，还没连上消费者
    Waiting,     // 环建好了，等界面端连进来
    Live,        // 有消费者在读，正在出帧
    Reconnecting,// 消费者掉线，正在等它回来
    Disabled,    // 配置被关掉，或窗口匹配不到（强制关闭）
    Failed,      // 初始化失败（显示器不存在、共享内存建不起来等）
};

const char* ToString(SessionState s);

// 这一路会话实际用的是哪种采集方式。
//   Desktop —— DXGI 桌面复制整屏（显示器源）。
//   Wgc     —— Windows Graphics Capture 直取窗口画面，被挡住也能录。
//   None    —— 还没定下来（未启动 / 未匹配到目标 / 初始化失败）。
//
// 这里**没有**"桌面复制 + 按窗口矩形裁剪"那一档。窗口源只走 WGC：WGC 建不起来
// 就让这一路失败并说清原因，绝不悄悄退化成裁剪 —— 裁剪录到的是"窗口在屏幕上的
// 样子"，窗口一旦被别的窗口挡住，录下来的就是挡住它的那个窗口，画面是错的，
// 而用户根本看不出来。宁可明确失败。
enum class CaptureKind {
    None,
    Desktop,
    Wgc,
};

const char* ToString(CaptureKind k);

// 给外面看的一路会话快照。
struct SessionStatus {
    std::string id;
    std::string name;
    SessionState state = SessionState::Starting;
    std::string detail;          // 人类可读的补充说明（失败原因、匹配到的窗口标题）
    bool enabled = false;
    bool attached = false;       // 帧环是否已创建
    uint64_t hwnd = 0;           // 窗口源匹配到的窗口句柄（0 = 桌面源或未匹配）
    std::string matchedTitle;    // 匹配到的窗口标题，便于界面显示"正在录哪个"
    // 捕获的那个窗口**关掉了**、这一路在等它回来（应用还活着）。
    //
    // 有这个标记，界面才能说人话：「在等《xxx》这个窗口回来」，而不是
    // 把"没有画面"含糊成一句匹配失败。它是 windowMissing 而不是用 state 判，
    // 因为 state=disabled 也可能是"从没匹配到"——两者的用户动作不一样。
    bool windowMissing = false;
    uint32_t width = 0;
    uint32_t height = 0;
    double targetFps = 0.0;
    // 实测指标（每秒刷一次，见 ReportStatus）
    double actualFps = 0.0;
    double gapMs = 0.0;
    double captureMs = 0.0;
    uint64_t produced = 0;
    uint64_t dropped = 0;
    uint32_t consumerPid = 0;
    // 录制（需求 5）：分片落地与滚动删除的情况。
    bool     recording = false;
    bool     recordFailed = false;
    std::string recordDetail;
    std::string recordDir;        // 该配置的录像子目录（绝对路径）
    uint32_t recordedSegments = 0; // 当前保留的分片数
    uint64_t recordedBytes = 0;    // 分片总字节数
    uint64_t recordedFrames = 0;
    // 码率（Mbps）：配置值与编码器实际值。
    //
    // 【为什么两个都要报】用户报「每次改完东西码率像没生效」（m14071）。
    // 以前只有 bitrateBps_ 一个内部变量，用户配的 5 Mbps 会被 SetFps/ResizeTo
    // 用像素公式覆盖成估算值（1440p@144 估出 79 Mbps），界面却毫无显示。
    // 现在两者都写进状态，界面可以并排显示「配置 5 / 实际 5」。
    uint32_t requestedBitrateMbps = 0;
    uint32_t effectiveBitrateMbps = 0;
    // 帧率同上：requested 是配置值，effective 是硬件吞吐上限降档后的值。
    uint32_t requestedFps = 0;
    uint32_t effectiveFps = 0;
    // 「丢帧」的两个口径（见 RecorderStats 里同名字段的注释）。
    //
    // 用户的要求（m15478）：「我可以理解降帧率，但是最好不要丢帧」。降帧率时
    // 采集快于编码，必然有帧进不了编码器；时间轴落后时又必须拿重复帧顶上。
    // 这两个数报出来，界面才能回答「到底丢了多少、是不是在变速」。
    uint64_t repeatFrames = 0;
    uint64_t captureDropped = 0;
    // 停机时从待喂队列里补写进编码器的帧数（见 SegmentRecorder::Stop）。
    // 「点重启丢了几秒」这类问题就靠它判断：非 0 说明最后那一段救回来了。
    uint64_t drainedOnStop = 0;
    // 采集端一共投进编码器的帧数（Feed 成功次数）。
    // 与 recordedFrames 一比就能分辨「采集端没给够」和「取槽相位错开」。
    uint64_t captureFrames = 0;
    // 「立刻切片」完成的次数（只增不减）。
    //
    // 【为什么要有这个计数】界面点导出走的是插件（Electron 主进程里跑），
    // 服务完全不知道。而导出前必须先把正在写的那一片收尾落盘，否则那一片
    // （最多一整片 = 2 秒，含"点按钮那一刻"的画面）导不出来 —— 用户报的
    // 「导出的视频都是点击前一秒或两秒就停止了」就是这个（m12714）。
    //
    // 所以界面先发 `{"cmd":"flush","id":...}` 让服务切片。**但管道是单向的**
    // （PIPE_ACCESS_INBOUND），服务没法回话；`pipeSend` 写完就返回 ok，
    // 导出端于是抢在 flush 完成之前扫了目录，那一片照样读不到。
    //
    // 这个计数就是那个"回话"：flush 真的切完一片才 +1，界面轮询到它变了
    // 才去调 exportStart。比让主进程去猜 index.csv 路径、数行数、比 mtime
    // 都稳（那几种在槽位复用、轮次目录切换时都会误判）。
    uint64_t flushSeq = 0;
    // 这一路到底在用哪种方式拿画面。
    CaptureKind captureKind = CaptureKind::None;
    // 诊断：预览限流是否真的在起作用（发进环的帧数 / 被跳过拍数 / 当前上限）。
    uint64_t previewPublished = 0;
    uint64_t previewSkipped = 0;
    uint32_t previewFps = 0;
    // 诊断：预览这条路上三个尺寸 —— 源多大、面板报多大、实际发进环多大。
    //
    // 【为什么必须分开报】「画面糊」有三种完全不同的成因，只看 published
    // 分辨不出来：源本身就这么大（发原图也糊）、面板比源大（放大必然糊）、
    // 服务把源砍太小（这才是能修的）。三个数一起看，一眼就知道是哪种。
    uint32_t previewInW = 0;
    uint32_t previewInH = 0;
    uint32_t previewOutW = 0;
    uint32_t previewOutH = 0;
    uint32_t previewTargetW = 0;
    uint32_t previewTargetH = 0;
    // 诊断：一拍里三笔大头各花多久（captureMs 只量了采集那一段）。
    // 1440p 每帧 14.7MB，采集/Publish/Feed 三份加起来 44MB/拍，
    // 120fps 就是 5.3GB/s —— fps 上不去时靠这三个数分辨卡在哪一段。
    double publishMs = 0.0;
    double feedMs = 0.0;
    // ---- 声音（需求：声音只进导出成片，预览不发声）------------------------
    //
    // audioActive 是"这一路现在真的在抓声音"；audioPeak / audioRms 是线性
    // 0..1 的电平（界面画音量条用，转 dB 是界面的事）。audioDetail 说明抓的
    // 是哪条通道（系统混音 / 窗口进程环回），以及退回原因。
    bool audioActive = false;
    float audioPeak = 0.0f;
    float audioRms = 0.0f;
    std::string audioDetail;
    // 采不到声音的原因（空 = 没问题）。界面拿它在预览画面上盖一条红条。
    // 窗口源采不到**不再退回系统混音**：混进来的别的窗口的声音用户根本看不
    // 出来，那比明着报错更糟。
    std::string audioError;
    uint32_t audioChunks = 0;      // 磁盘上保留的音频块数
    uint64_t audioBytes = 0;       // 音频占用字节
    // 看门狗重建过几次采集。一直涨说明目标应用反复换音频会话（Chromium 系
    // 的 audio service 子进程就是按需起停的），出问题时这是第一个该看的数。
    uint32_t audioRestarts = 0;
    // 最近一次进程树扫描时"目标进程树里比建环回那一刻多出来的进程数"。
    // 0 = 树没变。它是上面那个看门狗的判据本身 —— 排查"明明在响却没声音"时，
    // 先看这个：>0 说明有新子进程跑到了环回外面，紧接着应该看到 audioRestarts 涨。
    uint32_t audioTreeGrown = 0;
};

// 一个采集会话。构造后调用 Start()，析构时会自动停线程。
class Session {
public:
    // desktop 捕获源：所有桌面源会话**共享**同一个 ScreenCapture，
    // 免得每路都去 CreateDXGIFactory 开一份桌面复制（那样很浪费，
    // 而且同一进程重复 Init 同一显示器会失败）。
    //
    // recordRoot 是分片落地根目录（需求 5）。空串 = 这一路不录，只预览。
    Session(const Profile& profile, ScreenCapture* sharedDesktop, std::string recordRoot);
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // 起线程。窗口源如果一开始就匹配不到，会直接标记 Disabled 但不报错。
    bool Start();

    // 让线程尽快退出（不等它自然跑完）。可重复调用。
    void Stop();

    // 线程是否还在跑。
    bool Running() const { return running_.load(std::memory_order_acquire); }

    // 取当前快照。加锁拷贝，调用频率别太高（主线程每秒一次足够）。
    SessionStatus Status() const;

    const std::string& Id() const { return profile_.id; }

    // 这份新配置和本会话正在用的那份，采集 + 编码参数是否完全一致。
    // reload 用它决定「其实什么都没改、不必重建」（见 Profiles.h 的说明）。
    bool SameSettings(const Profile& other) const { return profile_.SameSettings(other); }

    // 同上，但忽略帧率 —— 帧率可以就地改（见 ApplyLiveSettings）。
    bool SameSettingsIgnoringFps(const Profile& other) const {
        return profile_.SameSettingsIgnoringFps(other);
    }

    /**
     * 同上，但忽略帧率**和码率** —— 这两个都能就地改（见 ApplyLiveSettings）。
     *
     * 【为什么需要】reload 原来只有两级判定：完全一样 → 什么都不做；只差帧率
     * → 就地改帧率。差码率就落到第三级「拆会话重建」—— 新轮次目录、
     * recordSegments 归零（导出按钮变灰）、时间轴出现空洞。用户只是把码率从
     * 20 改成 10，界面表现却像重启了一次（m14071）。
     *
     * 有了这一级，reload 可以「就地改帧率 + 就地改码率」，两者互不影响。
     */
    bool SameSettingsIgnoringFpsBitrate(const Profile& other) const {
        return profile_.SameSettingsIgnoringFpsBitrate(other);
    }
    const Profile& Config() const { return profile_; }

    /**
     * 把新配置里**可以就地生效**的部分应用到正在跑的会话上（帧率、码率）。
     *
     * 【为什么需要】用户把帧率从 144 改成 60 时，服务走 reload。原来 reload
     * 判「配置变了」就整路拆掉重建：新 Session -> 新录制器 -> 新轮次目录，
     * 于是 recordSegments 归零（导出按钮变灰）、时间轴出现空洞（被补成
     * NO SIGNAL）、刚切过去点导出报"该路未在录制"。用户的原话（m13003）：
     * 「降帧率的时候，会出现短暂的 no signal，和导出按钮被禁用，帧率刚切换
     * 那会点击会出现该路未在录制」。
     *
     * 码率同理（m14071：「有关编辑的东西一保存立刻落地，像配置改帧率，改
     * 码率什么的」）：它也写在 SinkWriter 的输出流上，也能靠"收掉当前片、
     * 按新参数开新片"就地生效，不该走重建。
     *
     * 现在 reload 先看 Profile::SameSettingsIgnoringFpsBitrate()：除了帧率、
     * 码率以外都一样就调这里，改完立刻生效，不换目录、不清分片、不动时间轴。
     *
     * 返回 true 表示真的就地应用了；false 表示这份配置不能只靠改帧率/码率，
     * 必须重建会话（分片时长、回放窗口、采集源、缩放变了都算）。
     */
    bool ApplyLiveSettings(const Profile& next);
    // 配置显示名（导出文件名前缀、事件里给界面显示用）。
    const std::string& Name() const { return profile_.name; }

    // 单次状态行（NDJSON，不带换行）。主线程用它拼输出。
    std::string StatusJson() const;

    /**
     * 实时电平（0..1 线性，快起慢落）。
     *
     * 单独一个接口而不是塞进 StatusJson：状态是每秒一次的，这个要 30Hz 地
     * 被采集器读走画音量条（见 main.cpp 的电平表写入线程）。
     *
     * 【为什么要衰减】这个值只在"收到音频包"时更新。声音一停，引擎往往
     * 就一个包都不再发 —— 不做衰减的话条子会永远停在最后一个值上，看起来
     * 像"已经不出声了但音量还是满的"。所以超过 120ms 没新包就按半衰期
     * 往下掉（见 Session.cpp）。
     */
    float MeterLevel() const;

    /**
     * 导出（需求 6）：把这一路的分片合并成一个 MP4 落到 exportDir。
     *
     * 内部起后台线程，立刻返回；进度和结果看 Status().export 那组字段。
     * 返回 false 表示"这一次没接"：没有录像器、上一轮还在导。
     */
    bool ExportNow(const std::string& exportDir, double maxSeconds);

    /**
     * 把正在写的那一片立刻收尾落盘（不中断录制）。
     *
     * 【为什么单独开一个方法】界面点导出走的是插件（bridge/shadow_ring.cc 的
     * exportStart，在 Electron 主进程里跑），不经过服务 —— 于是 ExportNow 里
     * 那句 FlushSegment() 从来没被执行过，用户看到的就是"导出总在点击前
     * 一两秒就停了"（m12714）。界面现在会在调 exportStart 之前先发一条
     * `{"cmd":"flush","id":...}` 让服务切一次片，走的就是这个方法。
     *
     * 返回 false 表示"没切"（没在录 / 当前片一帧都没有 / 等超时），
     * 导出端照旧拿已完成的分片，最多还是丢那一小段。
     */
    bool FlushRecording();

    // 导出状态（给操作日志/自检用）。
    ExportStatus ExportState() const;

    /**
     * 设置预览帧率上限（0 = 不限，跟随采集帧率）。
     *
     * 为什么需要它：采集循环的节拍是由**录制帧率**定的（profile.video.fps），
     * 而每一拍都会把整屏帧 memcpy 进共享环给界面预览。1440p 一帧 14.7MB，
     * 120fps 就是 1.77GB/s —— 实测这会把循环从 120fps 拖到 91fps。
     *
     * 预览窗口在屏幕上就那么大，画 120 帧和画 60 帧肉眼没区别，但带宽差一倍。
     * 所以给预览单独设上限：**录制照旧按配置帧率喂**，只有往共享环 Publish
     * 这一步被限流。导出读的是分片，不受任何影响。
     *
     * 可以随时改，下一拍就生效 —— 不需要重建会话。
     */
    void SetPreviewFps(uint32_t fps) { previewFps_.store(fps, std::memory_order_relaxed); }
    uint32_t PreviewFps() const { return previewFps_.load(std::memory_order_relaxed); }

    /**
     * 告诉服务"预览面板实际有多少设备像素"。
     *
     * 【为什么需要这个】预览画质由"发布的像素数"决定，而发布的像素数应该由
     * 面板的真实大小决定，不该由一条固定阈值决定：
     *   * 面板只有 900 设备像素宽时，发 2560 是白烧 8 倍带宽（还看不出区别）；
     *   * 面板有 2400 设备像素宽时，发 1280 是糊 1.9 倍（用户能一眼看出来）。
     * 以前用固定阈值（宽 > 1600 就砍半），于是"面板多大"这个唯一重要的信息
     * 被服务猜掉了：2560 的桌面砍成 1280，再被合成器放大回 1500+ —— 又糊又白费。
     *
     * 现在界面把自己 canvas 的设备像素尺寸报上来，服务据此决定 1:1 还是 1:2：
     * 只在"源明显大于面板"时才降，够用就发原分辨率。这样上传带宽只花在
     * 面板真的用得上的像素上 —— 高画质和低占用不再互相排斥。
     *
     * 传 0 表示"还没报过"，服务退回旧的固定阈值行为。
     */
    void SetPreviewTarget(uint32_t w, uint32_t h) {
        previewTargetW_.store(w, std::memory_order_relaxed);
        previewTargetH_.store(h, std::memory_order_relaxed);
    }

private:
    void ThreadMain();
    // 解析采集源，成功则填好 capture_ / windowCapture_，返回是否可用。
    bool ResolveSource();
    // 按当前画面尺寸建/重建录制器（尺寸变了就得重建编码器）。
    void EnsureRecorder(uint32_t w, uint32_t h);
    // 帧环的预留容量。窗口可以随用户拖大，而共享内存的槽容量建好就定死，
    // 所以不能只按当前尺寸预留 —— 这里按"当前尺寸 + 余量"给一份上限，
    // 到上限还可以再重建一次（见 Session.cpp 里的换环逻辑）。
    static uint32_t RingCapWidth(uint32_t w);
    static uint32_t RingCapHeight(uint32_t h);
    // 预览这一帧要不要砍半（见 SetPreviewTarget 的说明）。带迟滞，会更新
    // lastHalfDecision_/halfDecisionValid_，所以不是 const。
    bool PreviewNeedsHalf(uint32_t srcW, uint32_t srcH);
    // 这一路实际生效的预览帧率上限（在用户设置的基础上按分辨率再收一道）。
    uint32_t EffectivePreviewFps(uint32_t srcW, uint32_t srcH);
    // 把状态里的录制信息刷新一遍（每秒调一次，扫目录有开销）。
    void RefreshRecordStatus();
    /**
     * 起一路声音采集（录制的附属品，recorder 起来之后调）。
     *
     * 桌面/显示器源 → 系统混音；窗口源 → 被捕获窗口的进程环回（只抓那个
     * 进程树）。窗口源采不到就是采不到，**不退回系统混音**（退回会把别的
     * 窗口的声音混进成片），原因写进 audioError，界面在预览上盖红条。
     */
    void StartAudio(const std::string& recordDir);
    /** 关掉声音采集并收尾落盘的当前块。 */
    void StopAudio();
    /**
     * 只重开采集通道，**不动** audioRing_（落盘目录/文件句柄都不变）。
     *
     * 看门狗走这条：环回"看着正常但一个包都没有"时重建一次采集，重建会
     * 重新抓一次目标进程树，目标进程树里新冒出来的子进程（Chromium 的
     * audio.mojom.AudioService 就是按需创建、闲着几秒就被回收）才进得来。
     * 走 StartAudio 会把 ring 也重建一遍、顺手 Purge 掉老块，没必要。
     */
    bool RestartAudioCapture();
    /** 每秒调：刷新电平/统计，采集线程意外退出（换设备）时重开。 */
    void RefreshAudioStatus(int64_t nowMs);
    /**
     * 现在该抓哪个进程的声音：窗口源返回被捕获窗口所属 pid，桌面源/还没匹到
     * 窗口都返回 0（= 系统混音）。
     */
    uint32_t ResolveAudioPid();

    Profile profile_;
    ScreenCapture* sharedDesktop_ = nullptr;
    std::string recordRoot_;

    // 分片录制（需求 5）。窗口源一开始匹配不到时它是空的，
    // 等窗口出现、尺寸定下来再建 —— 分辨率必须先知道才好起编码器。
    std::unique_ptr<SegmentRecorder> recorder_;
    uint32_t recorderW_ = 0;
    uint32_t recorderH_ = 0;

    // 导出（需求 6）。分片目录来自 recorder_，但**导出目录和时长上限每次导出
    // 都可能变**（界面能随时改），所以这三样都缓存下来，变了就重建 Exporter
    // —— 否则第一次导出的参数会被永远记住，用户改了目录还导出到老地方。
    std::unique_ptr<Exporter> exporter_;
    std::string exporterKey_;

    // 显示器源专用：本会话自己开一份桌面复制，只在没有共享实例时才用。
    std::unique_ptr<ScreenCapture> ownCapture_;
    ScreenCapture* capture_ = nullptr;

    // ---- 声音 ---------------------------------------------------------------
    //
    // 采集通道：桌面/显示器源走系统混音（默认渲染设备 loopback），窗口源走
    // 进程环回（只抓被捕获窗口那个进程树）。落盘走 AudioRing，按回放窗口滚
    // 动删除；导出端从 audio 目录按墙钟窗口读 PCM 写进成片。
    //
    // 为什么把音频和分片分开（不进 mp4）：见 AudioRing.h 文件头 —— 每片重开
    // AAC 编码器的 priming 会在成片里留下每 2 秒一次的 20ms 空隙。
    std::unique_ptr<AudioCapture> audio_;
    std::unique_ptr<AudioRing> audioRing_;
    int64_t audioRestartAtMs_ = 0;   // 下次允许重开音频的时刻（防抖）
    bool audioWarned_ = false;       // 退回系统混音的提示只报一次
    // 看门狗：连续多久没收到音频包才重建采集。静音时引擎本来就不发包，
    // 所以用"指数退避"（3s→6s→12s→…→60s），真在出声的场景 3 秒内就能救回来，
    // 一直没声音的场景也不会每秒重建一次。
    int64_t audioIdleSinceMs_ = 0;   // 从哪一刻开始"没包"
    int64_t audioIdleBackoffMs_ = 0; // 当前退避窗口（0 = 还没退避过）
    int64_t audioTreeCheckAtMs_ = 0; // 下次允许扫进程树（1 秒一次）
    int64_t audioSilentSinceMs_ = 0; // 从哪一刻开始"电平一直是 0"（0 = 此刻有声）
    uint32_t audioTreeGrown_ = 0;    // 最近一次扫树发现的新子进程数（诊断）
    uint32_t audioRestarts_ = 0;     // 重建次数（诊断用）
    /** 实时电平（采集线程写、电平表线程读）。 */
    std::atomic<float> meterLevel_{0.0f};
    /** meterLevel_ 最后一次被写入的墙钟毫秒（0 = 还没收到过包）。MeterLevel 靠它衰减。 */
    std::atomic<int64_t> meterUpdatedMs_{0};
    // 当前音频抓的是哪个进程（0 = 系统混音）。窗口源在运行中重新匹配到
    // **另一个进程**的窗口时，采集链路要跟着换，否则画面换了声音还是老的。
    uint32_t audioPid_ = 0;

    // 窗口源专用：WGC（Windows Graphics Capture）直接抓窗口本体，这样窗口
    // 被挡住/被移出屏幕也能录到它自己。WGC 建不起来这一路就失败，
    // 没有"退回桌面复制再裁剪"的退路（见 Session.h 顶部 CaptureKind 的说明）。
    std::unique_ptr<WindowCapture> windowCapture_;

    // 节拍器状态（不是诊断字段，节流算法靠它工作）：
    // workEstimate_ 是"干活"时长的估计值，下一轮的 deadline 要从它往前挪，
    // 否则每轮都会多出「采集+拷贝」那一截时间、帧率稳定掉到 53。
    // previousPublish_ 用来量真实间隔，喂给积分修正项。
    std::chrono::steady_clock::duration workEstimate_{};
    double workEstimateMs_ = 0.0;
    std::chrono::steady_clock::time_point previousPublish_{};
    bool havePreviousPublish_ = false;

    // actualFps 的平滑状态（见 Session.cpp 每秒汇报那一段的注释）：
    // fpsEma_ 是实测帧率的指数滑动平均，haveFpsEma_ 表示第一窗已经灌进去了。
    // 这两个只被会话线程自己读写，不用进 mu_。
    double fpsEma_ = 0.0;
    bool haveFpsEma_ = false;

    FrameRing ring_;
    std::thread thread_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> running_{false};

    // 预览帧率上限（见 SetPreviewFps）。0 = 不限。
    // 用 atomic 而不是加锁：会话线程每一拍都要读它，而改它的频率极低
    // （用户点一次设置页），没必要为它引入一把锁去打断采集节拍。
    std::atomic<uint32_t> previewFps_{0};

    // 预览面板的设备像素尺寸（见 SetPreviewTarget）。0 = 界面还没报过。
    std::atomic<uint32_t> previewTargetW_{0};
    std::atomic<uint32_t> previewTargetH_{0};

    // 上一次的砍半决策（迟滞状态，见 PreviewNeedsHalf）。
    //
    // 【为什么需要】砍半与否由"面板宽度 vs 源宽"决定，面板宽度在临界点附近
    // 抖动时（滚动条出现/消失、dpr 取整、ResizeObserver 偶发触发）会让决策
    // 一拍拍地翻转。而 EffectivePreviewFps 又把"是否砍半"和"帧率是否收到 60"
    // 绑在一起 —— 决策翻转一次，预览帧率就在 60↔120 之间跳一次，用户看到
    // 的是"设置 120fps 却随机掉到 58"。这个 bool 记住上次结论，用两个不同
    // 的阈值进出，把边界抖动吸收掉。只在采集线程里读写，不用 atomic。
    bool lastHalfDecision_ = false;
    bool halfDecisionValid_ = false;

    // 「立刻切片」的完成计数（对外暴露成 SessionStatus::flushSeq）。
    // 命令线程在 FlushRecording() 里自增，界面轮询到变化才去启动导出。
    std::atomic<uint64_t> flushSeq_{0};

    mutable std::mutex mu_;
    SessionStatus status_;

    // 采集快照缓冲：**每一路会话各有一份**，装 AcquireSnapshot 拷出来的整屏帧。
    //
    // 为什么每路都要自己一份：多路共享同一个 ScreenCapture，采集器内部那块
    // staging 是共享的 —— 一路拿指针回去用，下一路一 CopyResource 就把它盖了。
    // 所以每路在自己的缓冲里收一份，之后所有裁剪/发布/喂录制器都读这份。
    std::vector<uint8_t> snap_;
    uint32_t snapW_ = 0;
    uint32_t snapH_ = 0;
    // 帧环当前**生效**的尺寸（= 最近一次 SetMode 给它的那对值）。
    //
    // 和 snapW_/snapH_ 分开记：snap 是"这一拍采到多大"，环是"对外宣称多大"。
    // 窗口缩放后两者会短暂不一致（池刚重建、新尺寸那张还没到），需要一个
    // 地方记住环的真实状态，才不会被反复 SetMode。
    uint32_t ringW_ = 0;
    uint32_t ringH_ = 0;

    // ---- 窗口锁定 + 同应用窗口黑名单 --------------------------------------
    //
    // 用户报的现象：录哔哩哔哩的**播放器窗口**，把播放器关掉，画面变成了它的
    // **主界面**在继续录；而编辑面板的窗口下拉里还写着播放器的标题。
    //
    // 根因是"锁定了 hwnd，但 hwnd 一失效就退回按规则重新匹配"：主界面和播放器
    // 同 exe、同 class（Chrome_WidgetWin_1），天然满足规则，于是被 pick:largest
    // 选中 —— 画面悄悄换了源，配置里写的却还是播放器。
    //
    // 现在：选定窗口的那一刻，把**这个窗口的身份**和**同应用里其它窗口**一起
    // 记下来（黑名单）。
    //   * 目标还活着（可见；最小化也算活着）→ 一直录它，不重新解析；
    //   * 目标不见了 → 只接受"目标自己回来"（新 hwnd 也行，只要不在黑名单
    //     里），找不到就 Disabled —— 界面盖 NO SIGNAL，绝不改录同应用的别处；
    //   * 这个 exe 一个顶层窗口都不剩 = 应用退了 → 黑名单作废，按规则重来；
    //   * 用户在编辑面板点「确定」→ 会话重建，这些状态自然清空，重新解一次。
    // 这些都是会话线程自己读写，不用进 mu_。
    struct TargetWindow {
        uint32_t hwnd = 0;
        uint32_t pid = 0;      // 窗口所属进程。"应用是不是退了"就靠它，比数窗口可靠
        std::string process;   // exe（小写），黑名单按它筛同应用的窗口
        std::string klass;
        std::string title;     // 原样保留，界面要显示"在等哪个窗口回来"
        bool valid() const { return hwnd != 0; }
    };
    struct SiblingWindow {
        uint32_t hwnd = 0;
        std::string process;
        std::string klass;
        std::string title;
    };
    TargetWindow target_;
    std::vector<SiblingWindow> siblings_;
    // 上一次"目标窗口还在不在"的检查时刻（每秒一次）。
    std::chrono::steady_clock::time_point lastTargetCheck_{};
    // 目标的解析结果（见 ResolveTarget）。
    enum class TargetResolve {
        Locked,   // out 里是能录的窗口（目标本身，或目标回来了）
        Waiting,  // 目标关着、应用还在：这一路要 Disabled，等它回来
        Reset,    // 还没锁过 / 应用退了：调用方按规则重新解
    };
    /** 复用锁定的目标；不满足就返回 Waiting（不许改录同应用的别的窗口）。 */
    TargetResolve ResolveTarget(WindowInfo& out);
    /** 记下这一路要录的窗口，并把同应用的其它窗口拉进黑名单。 */
    void LockTarget(const WindowInfo& win);
    /** 清空目标与黑名单（应用退出 / 会话重来）。 */
    void ClearTargetLock();
    /** 这个窗口是不是黑名单里的（同应用、但不是我们要录的那个）。 */
    bool IsSibling(const WindowInfo& win) const;
    /**
     * 刷新黑名单：把同 exe 里"不是目标"的窗口都记进来。
     *
     * 只在目标活着的时候调用 —— 否则目标关着时新出现的窗口（很可能就是回来
     * 的播放器）会被当成"别的窗口"拉黑，永远等不到它。
     */
    void RefreshSiblings();

    // Capture() 的元数据出口（尺寸/QPC）。像素不用它——像素走 snap_。
    FrameView fvRaw_;
    // 本路已经吃过的「最近一张」版本号。多路同源时靠它决定要不要真的去
    // AcquireNextFrame —— 别人采过了就直接 memcpy，一个 duplication 都不碰。
    uint64_t seenEpoch_ = 0;
};

} // namespace shadow
