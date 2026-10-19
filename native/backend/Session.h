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
#include "FrameRing.h"
#include "Profiles.h"
#include "ScreenCapture.h"
#include "SegmentRecorder.h"
#include "WindowCapture.h"

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
    // 这一路到底在用哪种方式拿画面。
    CaptureKind captureKind = CaptureKind::None;
    // 诊断：预览限流是否真的在起作用（发进环的帧数 / 被跳过拍数 / 当前上限）。
    uint64_t previewPublished = 0;
    uint64_t previewSkipped = 0;
    uint32_t previewFps = 0;
    // 诊断：一拍里三笔大头各花多久（captureMs 只量了采集那一段）。
    // 1440p 每帧 14.7MB，采集/Publish/Feed 三份加起来 44MB/拍，
    // 120fps 就是 5.3GB/s —— fps 上不去时靠这三个数分辨卡在哪一段。
    double publishMs = 0.0;
    double feedMs = 0.0;
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
    const Profile& Config() const { return profile_; }

    // 配置显示名（导出文件名前缀、事件里给界面显示用）。
    const std::string& Name() const { return profile_.name; }

    // 单次状态行（NDJSON，不带换行）。主线程用它拼输出。
    std::string StatusJson() const;

    /**
     * 导出（需求 6）：把这一路的分片合并成一个 MP4 落到 exportDir。
     *
     * 内部起后台线程，立刻返回；进度和结果看 Status().export 那组字段。
     * 返回 false 表示"这一次没接"：没有录像器、上一轮还在导。
     */
    bool ExportNow(const std::string& exportDir, double maxSeconds);

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
    // 把状态里的录制信息刷新一遍（每秒调一次，扫目录有开销）。
    void RefreshRecordStatus();

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
    // Capture() 的元数据出口（尺寸/QPC）。像素不用它——像素走 snap_。
    FrameView fvRaw_;
    // 本路已经吃过的「最近一张」版本号。多路同源时靠它决定要不要真的去
    // AcquireNextFrame —— 别人采过了就直接 memcpy，一个 duplication 都不碰。
    uint64_t seenEpoch_ = 0;
};

} // namespace shadow
