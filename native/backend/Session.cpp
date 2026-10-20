// Session.cpp —— 单路采集会话的实现。
//
// 采集主循环刻意跟原来 main.cpp 里的单路循环保持一致的节拍策略，
// 因为那套参数是实测调出来的（见 DESIGN.md 第 9 节的实测基线）：
//
//   1. Capture() 的超时给得很短（2ms）。AcquireNextFrame 有帧就立刻返回，
//      桌面静止时我们希望尽快回到循环里去做"重复发布"，而不是傻等 200ms。
//   2. 桌面静止（NoChange）时按目标帧率把最后一帧重新发布一遍，
//      否则界面会停在一张静止画面上（看起来像"只渲染了一帧"）。
//   3. 发布节奏用"睡到该发的时刻"而不是"跳过这一轮"——后者会让有效
//      帧率掉到目标的一半（19ms 的间隔落到 33ms 的格子上）。
#include "Session.h"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "JsonUtil.h"
#include "RingNames.h"
#include "WindowEnum.h"

namespace shadow {

namespace {

constexpr double kDefaultFps = 60.0;
// AcquireNextFrame 的等待超时（毫秒）。
//
// 这个值直接吃掉节拍预算：它是在"干活"那一段里阻塞的，240fps 的整个间隔
// 才 4.17ms，等 2ms 就等于把一半预算白扔了。1ms 是 DXGI 能接受的最小实用值，
// 而且它只在桌面**没有新帧**时才等满 —— 有内容时立刻返回。
constexpr uint32_t kCaptureTimeoutMs = 1;
// 采集被中断（窗口关掉 / 锁屏 / 切分辨率）后，重建最多试几次、之间隔多久。
//
// 6 次 × 约 1 秒 是刻意的折中：窗口关掉再开通常几秒内完成，这段重试窗口足够
// 盖住它；而如果一个窗口再也不回来，也不该让它无限期地每秒重建一次
// （每次重建都要建 D3D 设备，白费）。超了之后会话置 Failed，但**不是死路** ——
// 外层的解析重试仍然每秒跑，窗口一出现就活。
constexpr int kRebuildAttempts = 6;
// 节拍积分修正的增益：每次残差（ms）乘上它累加到"干活"估计值里。
// 0.25 大约 4 轮收敛一格，既够快又不会自己振起来。
constexpr double kPacingGain = 0.25;
// deadline 往前挪的下限，保证任何情况下都留一点点睡眠时间。
constexpr auto kMinSleep = std::chrono::microseconds(500);

// 睡到指定时刻。用 1ms 精度的定时器等待（timeBeginPeriod(1) 之后
// Sleep 的精度就是 1ms 量级），这样 60fps 的节拍能稳在 16.7ms。
void SleepUntil(std::chrono::steady_clock::time_point when) {
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= when) return;
        const auto remain =
            std::chrono::duration_cast<std::chrono::milliseconds>(when - now).count();
        if (remain <= 0) return;
        // 留 1ms 余量，剩下的用自旋收尾（自旋很贵，但只占最后一点点时间）
        const DWORD ms = static_cast<DWORD>(remain > 1 ? remain - 1 : 0);
        if (ms > 0) Sleep(ms);
        else std::this_thread::yield();
    }
}

} // namespace

const char* ToString(SessionState s) {
    switch (s) {
        case SessionState::Starting:     return "starting";
        case SessionState::Waiting:      return "waiting";
        case SessionState::Live:         return "live";
        case SessionState::Reconnecting: return "reconnecting";
        case SessionState::Disabled:     return "disabled";
        case SessionState::Failed:       return "failed";
    }
    return "unknown";
}

const char* ToString(CaptureKind k) {
    switch (k) {
        case CaptureKind::None:    return "none";
        case CaptureKind::Desktop: return "desktop";
        case CaptureKind::Wgc:     return "wgc";
    }
    return "none";
}

Session::Session(const Profile& profile, ScreenCapture* sharedDesktop, std::string recordRoot)
    : profile_(profile),
      sharedDesktop_(sharedDesktop),
      recordRoot_(std::move(recordRoot)) {
    status_.id = profile.id;
    status_.name = profile.name.empty() ? profile.id : profile.name;
    status_.enabled = profile.enabled;
    status_.targetFps = profile.video.fps > 0.0 ? profile.video.fps : kDefaultFps;
}

Session::~Session() {
    // 各成员显式按「依赖越深越先拆」的顺序释放，别交给编译器生成的逆序析构。
    //
    // 顺序有讲究：窗口采集（WGC）背后挂着一个专职泵消息的线程，它要跟着
    // 会话一起停；录制器背后攥着 SinkWriter（还没 Finalize 的分片），要赶在
    // 帧环之前收尾。手工写死顺序比依赖成员声明位置可靠 —— 以后有人在
    // Session.h 里挪一行，析构顺序就不会跟着悄悄变。
    Stop();
    windowCapture_.reset();
    recorder_.reset();
    exporter_.reset();
    ownCapture_.reset();
}

bool Session::Start() {
    if (thread_.joinable()) return false;
    stopRequested_.store(false, std::memory_order_release);
    thread_ = std::thread(&Session::ThreadMain, this);
    return true;
}

void Session::Stop() {
    stopRequested_.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
}
SessionStatus Session::Status() const {
    std::lock_guard<std::mutex> lk(mu_);
    SessionStatus s = status_;
    // 从 ring_ 现取几个实时计数（这两个不需要锁，是原子的）
    return s;
}

std::string Session::StatusJson() const {
    // 采集方式单独拼：要能从外部一眼看出这一路到底是桌面源还是窗口（WGC）。
    std::string cap;
    {
        std::lock_guard<std::mutex> lk(mu_);
        cap = ",\"captureKind\":\"" + std::string(ToString(status_.captureKind)) + "\"";
    }

    // 录制字段单独拼：StatusJson 会被每秒调一次，而录制的实时数字要现问
    // 录制器（里面还带一次目录扫描），所以不走 SessionStatus 缓存。
    std::string record;
    {
        std::lock_guard<std::mutex> lk(mu_);
        record = ",\"recording\":" + std::string(status_.recording ? "true" : "false") +
                 ",\"recordFailed\":" + std::string(status_.recordFailed ? "true" : "false") +
                 ",\"recordDir\":\"" + JsonEscape(status_.recordDir) + "\"" +
                 ",\"recordDetail\":\"" + JsonEscape(status_.recordDetail) + "\"" +
                 ",\"recordSegments\":" + std::to_string(status_.recordedSegments) +
                 ",\"recordBytes\":" + std::to_string(status_.recordedBytes) +
                 ",\"recordFrames\":" + std::to_string(status_.recordedFrames) +
                 // 立刻切片完成计数：界面发完 flush 命令后轮询这个字段，
                 // 变了才说明"点按钮那一刻"的分片真的落盘了，可以开始导出。
                 // 管道是单向的，服务没法回话，这就是那条回执。
                 ",\"flushSeq\":" +
                 std::to_string(flushSeq_.load(std::memory_order_relaxed));
    }

    // 导出状态另拼：它来自 exporter_（自己带锁），而且字段里有字符串路径。
    // 没导过时也照样输出这组字段（running/failed 都是 false），
    // 好处是界面和自检不用判空 —— 字段永远在。
    std::string expo;
    {
        const ExportStatus es = ExportState();
        char ebuf[1400];
        std::snprintf(ebuf, sizeof(ebuf),
                      ",\"exportRunning\":%s,\"exportOk\":%s,\"exportFailed\":%s,"
                      "\"exportPhase\":\"%s\",\"exportOutPath\":\"%s\","
                      "\"exportDetail\":\"%s\",\"exportSegments\":%u,"
                      "\"exportDurationSeconds\":%.2f,\"exportBytes\":%llu,"
                      "\"exportElapsedMs\":%.0f,\"exportJobId\":%llu",
                      es.running ? "true" : "false", es.ok ? "true" : "false",
                      es.failed ? "true" : "false", JsonEscape(es.phase).c_str(),
                      JsonEscape(es.outPath).c_str(), JsonEscape(es.detail).c_str(),
                      es.segmentCount, es.durationSeconds,
                      static_cast<unsigned long long>(es.bytes), es.elapsedMs,
                      static_cast<unsigned long long>(es.jobId));
        expo = ebuf;
    }

    const SessionStatus s = Status();
    char buf[1024];
    std::snprintf(buf, sizeof(buf),
                  "{\"type\":\"session\",\"id\":\"%s\",\"name\":\"%s\","
                  "\"state\":\"%s\",\"detail\":\"%s\",\"enabled\":%s,"
                  "\"attached\":%s,\"hwnd\":%llu,\"matchedTitle\":\"%s\","
                  "\"width\":%u,\"height\":%u,\"targetFps\":%.2f,"
                  "\"actualFps\":%.2f,\"gapMs\":%.2f,\"captureMs\":%.2f,"
                  "\"produced\":%llu,\"dropped\":%llu,\"consumerPid\":%u,"
                  "\"previewFps\":%u,\"previewPublished\":%llu,\"previewSkipped\":%llu,"
                  "\"publishMs\":%.2f,\"feedMs\":%.2f",
                  JsonEscape(s.id).c_str(), JsonEscape(s.name).c_str(),
                  ToString(s.state), JsonEscape(s.detail).c_str(),
                  s.enabled ? "true" : "false", s.attached ? "true" : "false",
                  static_cast<unsigned long long>(s.hwnd),
                  JsonEscape(s.matchedTitle).c_str(),
                  s.width, s.height, s.targetFps,
                  s.actualFps, s.gapMs, s.captureMs,
                  static_cast<unsigned long long>(s.produced),
                  static_cast<unsigned long long>(s.dropped), s.consumerPid,
                  s.previewFps,
                  static_cast<unsigned long long>(s.previewPublished),
                  static_cast<unsigned long long>(s.previewSkipped),
                  s.publishMs, s.feedMs);
    return std::string(buf) + cap + record + expo + "}";
}

// 帧环的预留容量。
//
// 共享内存的槽容量是建映射那一刻定死的（FrameRing.h 里写得很清楚），而窗口
// 尺寸可以随时被用户拖大。两种做法：
//   a) 每次尺寸变化都重建环 —— 消费者必掉线，界面要等 2 秒重新握手；
//   b) 按"足够大"一次预留，只有真的超了才重建。
//
// 选 b)：桌面源的尺寸是固定的，窗口源同一次拖拽里也常常跨不过一个台阶。
// 余量给到 1.5 倍再向上取整到 64 像素 —— 1440p 一槽是 14.7MB，1.5 倍就是
// 22MB，四槽 88MB，对一个常驻录制服务是可以接受的；换来的是拖窗口时
// 绝大多数情况一次都不用重建。
//
// 上限只在这里设一次下限：太小的画面（比如 300x200 的小工具窗口）也按
// 720p 预留，免得用户随便拉一下就超。
uint32_t Session::RingCapWidth(uint32_t w) {
    const uint32_t grown = static_cast<uint32_t>(static_cast<uint64_t>(w) * 3u / 2u);
    const uint32_t base = grown < 1280u ? 1280u : grown;
    return (base + 63u) & ~63u;
}

uint32_t Session::RingCapHeight(uint32_t h) {
    const uint32_t grown = static_cast<uint32_t>(static_cast<uint64_t>(h) * 3u / 2u);
    const uint32_t base = grown < 720u ? 720u : grown;
    // 槽容量按 width*4*height 算，高度不用对齐到 64，但留个偶数省得
    // 和 H.264 的宏块对齐逻辑拌在一起。
    return (base + 1u) & ~1u;
}

void Session::EnsureRecorder(uint32_t w, uint32_t h) {
    if (recordRoot_.empty() || w == 0 || h == 0) return;

    // H.264 的宏块是 16x16，宽高必须取偶。录制器内部会自己向下取偶，但喂帧
    // 那一步要拿尺寸做校验 —— 如果这里存的还是原始奇数高（窗口高度经常是
    // 889 这种），Feed() 就会认为尺寸不符，安安静静把每一帧都当"坏帧"丢掉，
    // 表现为状态里 recording=true 而 recordFrames 永远是 0。
    w &= ~1u;
    h &= ~1u;
    if (w == 0 || h == 0) return;

    // 【尺寸变了不再重建录制器】见 DESIGN.md 9.5。
    //
    // 以前这里是 `recorderW_ == w && recorderH_ == h` 才算复用，尺寸一变就
    // Stop + 新建：新轮次目录、切片从 000000 重来、采集端要重开编码器。后果
    // 是「拖动改变窗口大小之后，回放里只剩调整之后的画面」，而且服务里其他
    // 路也跟着抖一下。现在录制器跨尺寸存活，编码器自己切分片。
    //
    // 代价：recorderW_/recorderH_ 只表示"建录制器时的尺寸"，之后可能和实际
    // 帧尺寸不同 —— 这正是 Feed() 不再拿尺寸做校验的原因。
    if (recorder_ && recorder_->Running()) return;

    if (recorder_) {
        recorder_->Stop();
        recorder_.reset();
    }

    std::string err;
    const std::string dir = AllocateProfileDir(recordRoot_, profile_, err);
    if (dir.empty()) {
        std::lock_guard<std::mutex> lk(mu_);
        status_.recordFailed = true;
        status_.recording = false;
        status_.recordDetail = err.empty() ? "录制目录分配失败" : err;
        return;
    }

    recorder_ = std::make_unique<SegmentRecorder>(dir, profile_.video, w, h);
    recorderW_ = w;
    recorderH_ = h;
    const bool ok = recorder_->Start();

    std::lock_guard<std::mutex> lk(mu_);
    status_.recordDir = dir;
    status_.recordFailed = !ok;
    status_.recording = ok;
    status_.recordDetail = ok ? "录制中" : recorder_->Stats().detail;
}

// 把新配置里可以就地生效的部分应用到正在跑的会话上。
bool Session::ApplyLiveSettings(const Profile& next) {
    // 只有「除了帧率以外全都一样」才允许就地改。别的字段都写死在采集器 /
    // 录制器的构造里（码率、分片时长、回放窗口、采集源），改不了。
    if (!profile_.SameSettingsIgnoringFps(next)) return false;

    const double oldFps = profile_.video.fps;
    const double newFps = next.video.fps;

    // 除了帧率之外的元数据（名字用于导出文件名前缀，改了要跟着走）
    profile_.name = next.name;

    {
        std::lock_guard<std::mutex> lk(mu_);
        status_.targetFps = newFps > 0.0 ? newFps : kDefaultFps;
    }

    if (oldFps == newFps) return true;

    profile_.video.fps = newFps;

    // 录制器那边也要跟着改。它内部会让编码器收掉当前片、按新帧率开新片
    // （见 SegmentRecorder::SetFps / MfEncoder::SetFps），**不换轮次目录、
    // 不归零分片序号、时间轴继续顺延**。
    //
    // 失败也不回滚：编码器可能被硬件吞吐上限卡住，但那时它会自己降档并
    // 把原因写进 recordDetail，比把会话拆了重建好得多。
    if (recorder_) {
        recorder_->SetFps(newFps > 0.0 ? static_cast<uint32_t>(newFps + 0.5) : 60u);
    }
    return true;
}

// 导出（需求 6）。
//
// 两个调用约束值得写下来：
//   1) 只在录制器活着的时候建 Exporter —— 它的分片目录就是录制器的 SegmentsDir()。
//   2) 导出目录和"最多导多少秒"每次导出时都重新传。界面可以随时改导出目录；
//      而时长上限（回放时长）跟着配置走，改了配置不该影响已经在导的那一次。
bool Session::FlushRecording() {
    if (!recorder_) return false;
    // 超时给 2 秒：FlushSegment 只是 Finalize 当前片（排空编码队列）+ 开新片，
    // 正常十几到几十毫秒；2 秒是"编码器彻底卡住"时的兜底，别让主循环无限等。
    const bool ok = recorder_->FlushSegment(2000);
    // 只有真的切出一片才推进计数：界面拿它当"flush 已生效"的信号，
    // 没在录 / 当前片一帧都没有 / 等超时 都不该让界面误以为可以导出了。
    if (ok) flushSeq_.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

bool Session::ExportNow(const std::string& exportDir, double maxSeconds) {
    if (!recorder_) return false;
    const std::string segDir = recorder_->SegmentsDir();
    if (segDir.empty()) return false;

    // 导出前把**正在写的那一片**收尾落盘。
    //
    // 【为什么这一步不能省】回放窗口的右端应该是用户按下导出按钮的那一刻。但切片
    // 是按固定片长的：点导出时总有一片正在写，它没 Finalize、index.csv 里也没有
    // 它，导出端只能读到最后一片**已完成**的分片 → "点导出之前那半秒到一秒"永远
    // 导不出来。用户报的就是这个（m09776：「我点击导出视频的时候，前一秒，或者说
    // 前半秒，画面都丢失了，应该一直录到我点击按钮才停止才对啊」）。
    //
    // 切完不中断录制：紧接着开新片继续录、切片序号继续递增、不换轮次目录。
    // 代价只是那一片比标称短，导出端本来就按每片真实时长铺时间轴。
    //
    // 失败不影响导出：FlushSegment 返回 false 只说明"没切"（没在录 / 当前片
    // 一帧都没有 / 等超时），导出端照旧拿已完成的分片，最多还是丢那一小段。
    recorder_->FlushSegment();

    // 文件名前缀用配置名，避免多路配置同一秒导出时互相覆盖。
    // 配置名里可能有目录分隔符/通配符，先洗一遍 —— 服务端不该把用户输入
    // 直接当路径用。
    std::string prefix;
    for (char c : profile_.name.empty() ? profile_.id : profile_.name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' ||
            c == '\\' || c == '|' || c == '?' || c == '*') {
            prefix.push_back('_');
        } else {
            prefix.push_back(c);
        }
    }

    const uint32_t secs = maxSeconds > 0.0 ? static_cast<uint32_t>(maxSeconds) : 0u;

    // 目标帧率：用**当前配置**里的 fps。改帧率后，缓冲里的老分片还是旧帧率，
    // 导出时靠它重采样（重复插帧）把成品对齐到新帧率 —— 这样"保存后立刻生效"
    // 才是真的生效，而不是要等老分片滚出去。
    const uint32_t wantFps =
        profile_.video.fps > 0.0 ? static_cast<uint32_t>(profile_.video.fps + 0.5) : 0u;

    // 单片标称时长：导出时要靠它判"最后一片写完了没"。以前写死 2 秒，而配置里
    // 是能改的 —— 用户把分片时长调成 5 秒之后，2 秒的完整片会被误判成半片丢掉，
    // 成品就短一大截（这也是"少两秒"的一类来源）。
    const uint32_t segSecs =
        profile_.video.segmentSeconds > 0 ? profile_.video.segmentSeconds : 2u;

    // 导出参数变了就重建 —— 但**正在导的那一轮不能被打断**，否则导出线程
    // 还在跑、Exporter 就被析构了（析构会等它，但白等一轮）。
    const std::string key = segDir + "|" + exportDir + "|" + std::to_string(secs) + "|" +
                            std::to_string(wantFps) + "|" + std::to_string(segSecs);
    if (key != exporterKey_) {
        if (exporter_) {
            const ExportStatus st = exporter_->Status();
            if (st.running) return false; // 等这一轮导完再换参数
        }
        exporter_ =
            std::make_unique<Exporter>(segDir, exportDir, prefix, secs, wantFps, segSecs);
        exporterKey_ = key;
    }
    if (!exporter_) {
        exporter_ =
            std::make_unique<Exporter>(segDir, exportDir, prefix, secs, wantFps, segSecs);
    }
    return exporter_->Start();
}

ExportStatus Session::ExportState() const {
    if (!exporter_) return ExportStatus{};
    return exporter_->Status();
}

void Session::RefreshRecordStatus() {    if (!recorder_) return;
    const RecorderStats rs = recorder_->Stats();
    std::lock_guard<std::mutex> lk(mu_);
    status_.recording = rs.active;
    status_.recordFailed = rs.failed;
    status_.recordDetail = rs.detail;
    status_.recordedSegments = rs.segmentsOnDisk;
    status_.recordedBytes = rs.diskBytes;
    status_.recordedFrames = rs.framesIn;
}

bool Session::ResolveSource() {
    // ---- 桌面源 ----------------------------------------------------------
    if (profile_.source.kind != SourceKind::Window) {
        capture_ = sharedDesktop_;
        if (!capture_) {
            // 没有共享的桌面采集（单路 --profiles 之外的用法），自己开一份。
            // 注意：同一进程里对同一个显示器重复 Init 会失败，所以只有
            // 在确实没有共享实例时才走这条路。
            ownCapture_ = std::make_unique<ScreenCapture>();
            if (!ownCapture_->Init(profile_.source.monitorIndex, kCaptureTimeoutMs)) {
                std::lock_guard<std::mutex> lk(mu_);
                status_.state = SessionState::Failed;
                status_.detail = "无法初始化桌面复制（显示器 " +
                                 std::to_string(profile_.source.monitorIndex) + "）";
                return false;
            }
            capture_ = ownCapture_.get();
        } else {
            // 共享实例由主线程保证已 Init，这里只做防御性检查。
            if (!capture_->HasFrame() && capture_->Width() == 0) {
                std::lock_guard<std::mutex> lk(mu_);
                status_.state = SessionState::Failed;
                status_.detail = "共享桌面采集尚未初始化";
                return false;
            }
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            status_.width = capture_->Width();
            status_.height = capture_->Height();
            status_.detail = "整屏（显示器 " + std::to_string(profile_.source.monitorIndex) + "）";
            status_.captureKind = CaptureKind::Desktop;
        }
        return true;
    }

    // ---- 窗口源 ----------------------------------------------------------
    //
    // 只走 WGC（Windows 10 1903+）：直接向合成器要这个窗口的**独立**画面。
    // 窗口被别的窗口挡住、被移出屏幕、被最小化，拿到的都是窗口自己。
    // 这是「被挡住也能正常录制」的唯一正解，没有桌面裁剪退化方案。
    if (!profile_.hasMatch) {
        std::lock_guard<std::mutex> lk(mu_);
        status_.state = SessionState::Disabled;
        status_.detail = "窗口源但没有配匹配规则";
        return false;
    }

    WindowInfo win;
    if (!ResolveWindow(profile_.match, win)) {
        std::lock_guard<std::mutex> lk(mu_);
        status_.state = SessionState::Disabled;
        status_.detail = "匹配不到窗口";
        return false;
    }
    if (win.width <= 0 || win.height <= 0) {
        std::lock_guard<std::mutex> lk(mu_);
        status_.state = SessionState::Disabled;
        status_.detail = "匹配到的窗口尺寸为 0";
        return false;
    }

    // ---- 窗口源：只走 WGC -------------------------------------------------
    //
    // 建不起来就让这一路失败。以前这里会"安静地退回桌面复制 + 按窗口矩形
    // 裁剪"，但裁剪法录到的是**窗口在屏幕上的样子**：窗口一旦被别的窗口挡住，
    // 录下来的就是挡住它的那个窗口，画面是错的，而用户完全看不出来。
    // 宁可明确失败并说清原因，也不给一帧错误的内容。
    if (profile_.source.wgc) {
        if (!windowCapture_) windowCapture_ = std::make_unique<WindowCapture>();
        std::string err;
        if (windowCapture_->Init(win.hwnd, err)) {
            capture_ = nullptr;
            {
                std::lock_guard<std::mutex> lk(mu_);
                status_.hwnd = reinterpret_cast<uint64_t>(win.hwnd);
                status_.matchedTitle = win.title;
                status_.width = windowCapture_->Width();
                status_.height = windowCapture_->Height();
                status_.detail = "窗口（WGC）：" + win.title;
                status_.captureKind = CaptureKind::Wgc;
            }
            return true;
        }
        std::lock_guard<std::mutex> lk(mu_);
        status_.state = SessionState::Failed;
        status_.detail = "窗口独立采集（WGC）初始化失败：" + err;
        return false;
    }

    // 配置里显式关掉了 WGC —— 这种配置没有任何可用的窗口采集方式。
    {
        std::lock_guard<std::mutex> lk(mu_);
        status_.state = SessionState::Failed;
        status_.detail = "窗口源必须用 WGC 采集，但这一路把 WGC 关掉了";
    }
    return false;
}

void Session::ThreadMain() {
    running_.store(true, std::memory_order_release);

    // 让本线程的 Sleep 有 1ms 精度（60fps 的节拍靠它）。
    timeBeginPeriod(1);

    // 整个"解析源 → 建环 → 起录制器 → 收帧"包在一个外层循环里。
    //
    // 【为什么要外层循环】采集链路会因为**环境变化**彻底断开，而环境还会变回来：
    // 用户把被录的窗口关了又开、锁屏再解锁、拔插显示器。这些情况下线程不该退出，
    // 更不该让整个 Session 死掉（死了就只能重启服务，隔壁分路跟着遭殃）。
    // 内层主循环用 `break` 表示"这一轮采集彻底没了"，回到这里重新解析一遍，
    // 解析成功就接着录，失败就每秒再试。
    for (;;) {
            if (stopRequested_.load(std::memory_order_acquire)) break;

        const bool ok = ResolveSource();
        if (!ok) {
            // 没解析出可用源：Disabled/Failed 已经写进 status_。
            //
            // 【这里必须能一直重试，不能"硬错误就直接躺死"】
            // 典型场景：配置里写着「录制记事本」，服务比记事本先起来 →
            // 匹配不到窗口（Disabled）；或者窗口刚出现、WGC 那一瞬间还抓不到
            // （Failed）。两种情况的共同点是**用户随后做了什么，环境就会变好**。
            // 以前只有 Disabled 会重试，Failed 直接把线程 return 掉 —— 于是
            // "启动时没抓到，后面打开了窗口也一样抓不到"，只能手动重启服务。
            // 现在一律重试，直到成功或者被 Stop()。
            //
            // 重试间隔取 1 秒：窗口从创建到 WGC 能抓通常要几百毫秒，1 秒足够快，
            // 又不会让"永远不存在的窗口"把 CPU 烧掉。
            const auto retryInterval = std::chrono::milliseconds(1000);
            while (!stopRequested_.load(std::memory_order_acquire)) {
                Sleep(static_cast<DWORD>(retryInterval.count()));
                if (stopRequested_.load(std::memory_order_acquire)) break;
                if (ResolveSource()) break;
            }
            // 两条源至少得有一条活着：桌面源看 capture_，WGC 源看 windowCapture_。
            // 不能只判 capture_ —— WGC 模式下 capture_ 本来就是 nullptr（见
            // ResolveSource 里「优先走 WGC」那段），只判它会把好好的 WGC 会话判死。
            if (!capture_ && !windowCapture_) {
                if (stopRequested_.load(std::memory_order_acquire)) break;
                // 理论上不会到这里（上面那个 while 只在成功或 Stop 时才退出），
                // 留着当保险：真到了就再来一轮解析，而不是把线程关掉。
                continue;
            }
        }

        // 输出尺寸：WGC 用窗口自己的大小，桌面源用整屏大小。
        // 没有"裁剪"这一档，所以这里只有两条分支。
        const bool wgcActive = (windowCapture_ != nullptr && capture_ == nullptr);
        const uint32_t w = wgcActive ? windowCapture_->Width() : capture_->Width();
        const uint32_t h = wgcActive ? windowCapture_->Height() : capture_->Height();

        if (!ring_.Open(profile_.id, RingCapWidth(w), RingCapHeight(h))) {
            // 帧环建不起来多半是名字被占着（上一个自己还没完全退出）。这也不是
            // "这一路彻底完了"，回到外层重试一遍更合理。
            {
                std::lock_guard<std::mutex> lk(mu_);
                status_.state = SessionState::Failed;
                status_.detail = "帧环创建失败，正在重试：" + ring_.LastError();
            }
            Sleep(1000);
            continue;
        }
        ring_.SetMode(w, h, 0);
        ringW_ = w;
        ringH_ = h;
        {
            std::lock_guard<std::mutex> lk(mu_);
            status_.attached = true;
            status_.state = SessionState::Waiting;
        }

        // 分辨率到这里才确定，录制器得现在才能起（见 EnsureRecorder 的说明）。
        EnsureRecorder(w, h);

        // 帧率是可热改的（见 ApplyLiveSettings）：用户把 144 改成 60 时不能
        // 拆掉这一路重建 —— 那会换轮次目录、recordSegments 归零、时间轴出现
        // 空洞（用户报的「短暂的 no signal」「导出按钮被禁用」「该路未在录制」
        // 就是它，m13003）。这里每一拍从 profile_ 里重新读，改完下一拍生效。
        auto currentFps = [this]() {
            const double f = profile_.video.fps > 0.0 ? profile_.video.fps : kDefaultFps;
            return f > 0.0 ? f : kDefaultFps;
        };
        double fps = currentFps();
        // 目标帧间隔。统一用 steady_clock 的原生 duration（整数 tick），
        // 这样后面跟 time_point 相加不用反复转换。
        auto frameInterval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(1.0 / fps));
        auto republishInterval = frameInterval;

        auto lastPublish = std::chrono::steady_clock::now() - std::chrono::hours(1);
        // 上一帧**预览**（Publish 进共享环）的时刻。和 lastPublish 是两回事：
        // lastPublish 记的是每一拍的节拍点，这个只记真的发了预览帧的那几拍。
        // 默认值要放在很久以前，否则第一拍会被"距上次不够一个间隔"卡住。
        auto lastPreviewPublish_ = std::chrono::steady_clock::time_point{};
        // 诊断用：实际发进共享环的预览帧数、被限流跳过的拍数。
        // 这俩只在状态上报里露出来，方便分辨"限流没生效"和"界面没画"。
        uint64_t previewPublished_ = 0;
        uint64_t previewSkipped_ = 0;
        // 下一拍该发布的**目标时刻**（自成一体的时间轴，见下面 nextDue 的注释）。
        auto nextPublish_ = std::chrono::steady_clock::now();
        auto lastRepublish = std::chrono::steady_clock::now();
        auto lastConsumerCheck = std::chrono::steady_clock::now();
        auto windowStart = std::chrono::steady_clock::now();
        uint64_t framesInWindow = 0;
        uint64_t captureNsInWindow = 0;
        // 分项计时：captureMs 只量了"采集"那一段，而一拍里还有 Publish（往共享环
        // memcpy 一整屏）和 Feed（拷进待编码槽）两笔大头。只报 captureMs
        // 的时候看不出 fps 卡在 80 到底卡在哪 —— 实测 1440p 每帧 14.7MB，这三笔
        // 加起来是 44MB/拍，120fps 就是 5.3GB/s，正是内存带宽的天花板。
        uint64_t publishNsInWindow = 0;
        uint64_t feedNsInWindow = 0;
        uint64_t loopsInWindow = 0;
        bool connected = false;
        // 上一拍采集到底有没有真的拿到画面。
        //
        // 用来判断"现在能不能说自己是 Live"。放在循环外是为了让 !connected
        // 那一支（在循环顶部）能看到上一拍的结果 —— frameAction 在循环中部
        // 才算得出来。
        //
        // 判据必须是"采集健康"而不是"消费者在不在"：消费者在不在只决定
        // 要不要往环里 Publish，采集断没断才是画面有没有的前提。两者混用
        // 就是那个"NO SIGNAL 撤不掉"的 bug。
        bool lastFrameOk = false;
        // 本轮的"没人在看"状态。等待消费者那一段里已经 declare 过它了。
        bool waitingSkip = false;

        while (!stopRequested_.load(std::memory_order_acquire)) {
            const auto loopStart = std::chrono::steady_clock::now();
            waitingSkip = false;

            // ---- 等消费者接入 -------------------------------------------------
            if (!connected) {
                // 注意这里**不**动 status_.state。
                //
                // 踩过的坑：这里原本在 !connected 时把状态写成 Waiting，
                // 而 Waiting 在界面侧是"正常、别盖遮罩"。可 !connected 有两种
                // 成因：(a) 界面还没连进来，(b) 界面掉线了正在等它回来。
                // 这两种都写 Waiting 的话，界面刷新（渲染进程换了个 pid）那一
                // 瞬间状态会从 Reconnecting 跳回 Waiting，画面在"NO SIGNAL"和
                // "上一帧定格"之间闪一下 —— 用户看到的就是"启动时有画面，
                // 然后闪一下没信号"。
                //
                // 状态该由"采集链路到底通不通"决定，见下面 WaitForConsumer
                // 成功和采集失败两个分支。
                //
                // 超时必须给 0（=只查一次立刻返回）。
                //
                // 踩过的坑：这里原本写的是 WaitForConsumer(100)，它内部每 20ms 轮询
                // 一次、最多睡满 100ms。在"没消费者就 continue"的旧逻辑下这无所谓，
                // 反正整轮都在空转；但改成"没消费者也照常采集 + 录制"之后，
                // 这个 100ms 直接叠进了采集周期 —— 实测帧间隔 37.7ms（同时段目标
                // 33.3ms，而且录制自报只有 8.7fps，217 帧 / 25 秒），录像时间轴
                // 因此被拉长，10 秒回放窗口在磁盘上膨胀成二十多秒都不滚动。
                //
                // 采集节拍只能由下一段的 deadline 决定，不能被"有没有人看"左右。
                if (ring_.WaitForConsumer(0)) {
                    connected = true;
                    {
                        std::lock_guard<std::mutex> lk(mu_);
                        // 消费者接上了 —— 只要采集链路是好的，就该是 Live。
                        //
                        // 【踩过的坑】这里原来只允许 Starting/Waiting 推进到 Live，
                        // 理由是"别把 AccessLost 分支刚设的 Reconnecting 冲掉"。
                        // 但那个顾虑的正确解法是"采集没恢复时别写 Live"，
                        // 而不是"永远不给 Reconnecting 翻身的机会" —— 后者会让
                        // 状态**永久**卡在 Reconnecting：界面刷新、渲染进程换了
                        // 一个 pid，服务端判一次心跳过期置 Reconnecting，此后
                        // 消费者再连回来也没人把状态改回去，NO SIGNAL 就再也
                        // 撤不掉了（实测就是这个表现：画面明明在动，遮罩一直在）。
                        //
                        // 现在改成看"本次采集是否健康"：采集循环正在正常出帧
                        // （lastFrameOk 表示上一拍拿到了帧）才允许回到 Live。
                        // AccessLost 那一支重建失败时 lastFrameOk 是 false，
                        // 状态留在 Reconnecting，遮罩照样盖得上。
                        if (lastFrameOk) status_.state = SessionState::Live;
                        status_.consumerPid = ring_.ConsumerPid();
                    }
                } else {
                    // 没接入也**不能**在这里 continue 掉整轮。
                    //
                    // 踩过的坑：原来的写法是"没消费者就 continue"，于是没人预览时
                    // 帧环一次 Publish 都不发生，而录制是挂在采集循环上的 ——
                    // 结果就是"界面没开 = 录像是空的"。需求 5 要的是服务一直录，
                    // 预览只是可选的观察窗口。
                    //
                    // 现在的行为：没有消费者时照常采集 + 喂录制器，只是不往环里写
                    // （Publish 本来就会被环拒绝，见下面 published 的处理）。
                    waitingSkip = true;
                }
            } else if (loopStart - lastConsumerCheck >= std::chrono::milliseconds(500)) {
                lastConsumerCheck = loopStart;
                if (!ring_.ConsumerAlive()) {
                    connected = false;
                    std::lock_guard<std::mutex> lk(mu_);
                    status_.state = SessionState::Reconnecting;
                    status_.consumerPid = 0;
                    continue;
                }
                std::lock_guard<std::mutex> lk(mu_);
                status_.consumerPid = ring_.ConsumerPid();
            }

            // ---- 帧率可能被就地改过（见 ApplyLiveSettings）---------------------
            // 每一拍重新读一次配置里的帧率。用户把 144 改成 60 时不能拆掉
            // 这一路重建 —— 那会换轮次目录、recordSegments 归零、时间轴出现
            // 空洞（m13003 报的「短暂的 no signal」「导出按钮被禁用」
            // 「该路未在录制」）。改完这里立刻换节拍，录制器那边由
            // ApplyLiveSettings 直接通知编码器。
            {
                const double wantFps = currentFps();
                if (wantFps != fps) {
                    fps = wantFps;
                    frameInterval =
                        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                            std::chrono::duration<double>(1.0 / fps));
                    republishInterval = frameInterval;
                    // nextPublish_ 不能按新间隔重排历史：它可能已经落在未来
                    // 好几拍之后（从 240 降到 30 时尤其明显）。夹到"现在"，
                    // 让节拍器从这一拍重新起步，不追赶也不停顿。
                    if (nextPublish_ > loopStart) nextPublish_ = loopStart;
                }
            }

            // ---- 到点才动手 ---------------------------------------------------
            // 还没到该发布的时刻就睡到那一刻，**不要** continue 跳过整轮。
            // 跳过会让实际间隔变成目标间隔的两倍（19ms 落到 33ms 的格子上）。
            //
            // deadline 从**上一轮的目标时刻**累加，不是从上一轮的实际完成时刻。
            //
            // 踩过的坑（两个，方向相反）：
            //
            // 1) 这里原本是 `lastPublish + (frameInterval - workEstimate_)`，把
            //    lastPublish 当成基准。但 lastPublish 是在 Feed 录制器**之后**才采的
            //    （见下面 publishedAt 的位置），而 Feed 是一帧几 MB 的整屏拷贝。
            //    那段耗时被算了两次，实测间隔 11.8ms、帧率钉在 43。
            //    改成从 nextPublish_ 累加后时间轴才自洽。
            //
            // 2) 补偿量的符号。workEstimate_ 是"开工之后还要干多久"，所以应该
            //    **提前**醒来：睡到 (deadline - workEstimate_)，干完活正好落在
            //    deadline 上。原来写成了 (deadline + workEstimate_)，那是往后推，
            //    等于把干活时间加了两遍 —— 每拍实际走 interval + work，三路并发
            //    时 work=18.8ms、interval=16.67ms，实测周期 23ms、帧率掉到 43，
            //    而且怎么调都上不去。
            //
            //    补偿量必须夹在 [0, interval) 之间：负数会变成"睡到过去"从而空转
            //    烧 CPU，超过一个 interval 说明这一拍根本干不完（采集已经饱和），
            //    此时不该再提前，直接连轴转、让节拍器自然落后即可。
            const auto ahead = workEstimate_ < frameInterval ? workEstimate_ : frameInterval;
            const auto nextDue = nextPublish_ - ahead;
            if (loopStart < nextDue) {
                SleepUntil(nextDue);
            }
            // 基准往目标线上推一格。落后太多（被抢占了 / 断了很久）就重新对齐，
            // 否则会攒出一串"欠债"的 deadline 把循环打成突发。
            nextPublish_ += frameInterval;
            if (nextPublish_ < loopStart - frameInterval) {
                nextPublish_ = loopStart;
            }
            const auto actStart = std::chrono::steady_clock::now();
            // 一拍只数一次。这里原来在采集段结束后又数了一次，导致 gapMs/captureMs
            // 的分母是循环次数的两倍 —— 两个数都显示成实际值的一半
            // （实测 60fps 时 gapMs 报 8.33，而真实周期是 16.67ms）。
            loopsInWindow++;

            // ---- 采集 ---------------------------------------------------------
            //
            // 走自己的快照缓冲而不是采集器内部指针：多路配置共用一个 ScreenCapture，
            // 直接拿采集器内部的指针回去用会被别的会话线程覆盖 ——
            // 那正是帧率忽高忽低（掉到 28）、画面花屏、编码器收到脏像素的真因。
            //
            // 两条源必须分派：WGC 模式下 capture_ 是 **nullptr**（见上面
            // 「优先走 WGC」那段），无脑解引用必崩。
            const bool useWgc = (windowCapture_ != nullptr && capture_ == nullptr);
            const uint32_t expectW = useWgc ? windowCapture_->Width() : capture_->Width();
            const uint32_t expectH = useWgc ? windowCapture_->Height() : capture_->Height();
            if (snap_.size() < static_cast<size_t>(expectW) * 4u * expectH) {
                snap_.resize(static_cast<size_t>(expectW) * 4u * expectH);
            }

            // ---- 窗口尺寸变了：换帧环、换录制器 --------------------------------
            //
            // WGC 的帧池会跟着窗口重建（见 WindowCapture::Capture），所以同一路
            // 会话在不同时刻拿到的帧尺寸可以完全不同。这里处理随之而来的两件事：
            uint32_t gotW = 0;
            uint32_t gotH = 0;
            int64_t  gotQpc = 0;
            bool     isNew = false;
            // snap_ 里这一刻装的是不是 gotW×gotH 的**真实像素**（而不是"沿用旧尺寸"）。
            //
            // 这个标记是必需的：窗口换尺寸后，采集会话会重建，而新尺寸的第一帧
            // 可能晚几拍才到。这期间 windowCapture_->Width() 已经是新尺寸，但
            // snap_ 里还是旧尺寸的图。以前这里没有这个标记，直接把 gotW 当成
            // 沿用的旧值继续往下走，于是「gotW 是旧的、ringW_ 是新的」持续不等，
            // 每拍都触发一次"尺寸变了"—— 实测表现为录制器反复重建、
            // recordFrames 归零后再也涨不起来，画面看着就是卡死。
            bool     frameValid = false;
            CaptureStatus cs = CaptureStatus::Error;
            {
                if (useWgc) {
                    // WGC 自己带锁（WindowCapture::mu_），而且是**这一路独占**的实例，
                    // 不需要外层的共享采集器锁。
                    cs = windowCapture_->Capture(fvRaw_, snap_.data(), snap_.size());
                    if (cs == CaptureStatus::Ok) {
                        gotW = fvRaw_.width;
                        gotH = fvRaw_.height;
                        gotQpc = fvRaw_.presentQpc;
                        isNew = true;
                        frameValid = true;
                    } else if (cs == CaptureStatus::NoChange) {
                        // 窗口这一拍没重绘（WGC 只在窗口重绘时投帧）。
                        //
                        // 关键：WGC 的 Capture 在 NoChange 时**一个字节都没往 snap_ 写**，
                        // 所以这里绝不能像桌面源那样"重放上一张"就完事 ——
                        // 那样 gotW/gotH 会留在 0，后面 snapStride 算出 0、
                        // 录进去的是零宽帧，实测帧率从 60 掉到 52。
                        //
                        // snap_ 里本来就还留着上一拍拷进来的像素（这一路自己的缓冲），
                        // 直接沿用它的尺寸即可 —— 但**只有尺寸对得上**才能沿用。
                        // 窗口刚换过尺寸时 snap_ 里是旧尺寸的图，这时候报 Ok 会让
                        // 上层以为"有新的一帧"，实际上像素是错的尺寸、录制器也会
                        // 被反复重建。所以这种情形老实认 NoChange，让上层等下一帧。
                        gotW = snapW_;
                        gotH = snapH_;
                        if (gotW > 0 && gotH > 0) {
                            cs = CaptureStatus::Ok;
                            frameValid = true;
                        }
                    }
                } else {
                    // 采集器是共享的，取快照期间必须独占它。
                    //
                    // 走 CaptureSharedInto 而不是 CaptureInto：多路同源时它会让**一次**
                    // AcquireNextFrame 服务所有同源会话 —— 谁先醒谁去采，后醒的一看
                    // epoch 比自己的新就把「最近一张」拷走，duplication 一次都不碰。
                    // 各采各的版本会让三路轮流抢锁，实测帧率从 60.0 塌到 50~54。
                    std::lock_guard<std::mutex> cap(capture_->Mutex());
                    cs = capture_->CaptureSharedInto(fvRaw_, snap_, seenEpoch_);
                    if (cs == CaptureStatus::Ok) {
                        gotW = fvRaw_.width;
                        gotH = fvRaw_.height;
                        gotQpc = fvRaw_.presentQpc;
                        // isNew 现在只能说明"本路这一拍拿到了内容"，不能说明"桌面变了"，
                        // 但这正是下游要的口径：喂录制器的是连续帧流，不是变化事件。
                        isNew = true;
                        // 桌面源也一样要置 frameValid：snapW_/snapH_ 是喂录制器和
                        // 发布预览用的尺寸来源，不置它就一直停在 0，录制器一帧都
                        // 收不到（症状：recording=true、recordFrames 永远 0）。
                        frameValid = gotW > 0 && gotH > 0;
                    } else if (cs == CaptureStatus::NoChange) {
                        // 这一拍桌面没动。把采集器攥着的上一张重放到**本路自己的**缓冲里，
                        // 然后当成一帧正常的帧继续走 —— 录像要的是连续帧流，
                        // 「没变化」不等于「这一轮不产出」。
                        //
                        // 优化：如果本路缓冲区里已经有了同尺寸的上一张（snapW_/snapH_
                        // 没变），就没有任何必要再从采集器拷一遍 —— 内容完全一样。
                        // 240fps 下这一份全屏 memcpy 是每拍最大的一笔开销（1440p 一帧
                        // 14.7MB × 240 = 3.5GB/s），GPU 高负载时正是它把带宽吃干净、
                        // 让 AcquireNextFrame 拿不到新帧，表现为"帧数不达标"。
                        const bool sameSize = (snapW_ > 0 && snapH_ > 0);
                        if (sameSize) {
                            // 直接沿用本路缓冲里的上一帧（尺寸也沿用，NoChange 不会填）。
                            gotW = snapW_;
                            gotH = snapH_;
                            cs = CaptureStatus::Ok;
                            isNew = false;
                            frameValid = true;
                        } else if (capture_->AcquireReplayLocked(snap_.data(), snap_.size(), gotW,
                                                                 gotH, gotQpc)) {
                            cs = CaptureStatus::Ok;
                            isNew = false;
                            frameValid = gotW > 0 && gotH > 0;
                        }
                    }
                }
            }
            // ---- 只有真的拿到像素时才更新"上一帧尺寸" ---------------------
            //
            // 踩过的坑（非常隐蔽，症状是"改一次窗口大小之后录制就再也不出帧"）：
            // 这里原来是无条件 `snapW_ = gotW; snapH_ = gotH;`。而 AccessLost /
            // Error 那一拍 gotW/gotH 都是 0，于是 snapW_ 被改成 0；下一拍 WGC
            // 报 NoChange，代码走"沿用上一帧"分支，看到 snapW_ == 0 就只能放弃
            // —— 此后每一拍都放弃，frameAction 永远 false，录制器再也收不到帧，
            // 而状态里 actualFps 正常（节拍在跑）、recordBytes 还在（旧的），
            // 只有 recordFrames 归零不动。查了很久。
            if (frameValid) {
                snapW_ = gotW;
                snapH_ = gotH;
            }
            const uint32_t snapStride = snapW_ * 4u;

            // ---- 画面尺寸变了：预览和录制都得跟着改 ----------------------
            //
            // 【为什么这件事必须在这里做】窗口被用户拖大/拖小时，WGC 的帧
            // 尺寸会跟着变（WindowCapture 内部会重建帧池），而下游有两处
            // 尺寸是**建的时候定死的**：
            //
            //   1) 帧环的槽容量（共享内存一旦创建容量就固定，见 FrameRing.h）。
            //      窗口超出预留容量后 Publish 会直接返回 false，界面就永远停在
            //      旧尺寸上 —— 用户看到的就是"窗口放大了，预览还是小小一块"。
            //   2) 录制器的编码器（分片宽高定了就不能改，H.264 流中间不能换尺寸）。
            //
            // 所以尺寸一变就得：超容量就换环、然后确保录制器也换了。
            //
            // 【为什么只对 WGC 触发】桌面源的尺寸是显示器分辨率，会话期间不会变；
            // 而且多路同源共享一个采集器，跟着一路去重建环会把别路一起打断。
            //
            // 【为什么比较 ringW_ 而不是直接比 snapW_】静止的窗口每一拍都走
            // NoChange，snapW_ 是沿用值，不会误触发。真正要防的是"已经 SetMode
            // 过了还反复 SetMode" —— 那会每拍都复位一次轮转状态，消费者读到的
            // 帧序号乱跳，界面表现为闪。
            if (useWgc && gotW > 0 && gotH > 0 && (gotW != ringW_ || gotH != ringH_)) {
                if (ring_.NeedsGrow(gotW, gotH)) {
                    // 容量不够了：整个环重建（消费者会掉线一次，界面端 2 秒
                    // 扫描兜住重连，见 preload.cjs 的 attachSession）。
                    if (!ring_.Open(profile_.id, RingCapWidth(gotW), RingCapHeight(gotH))) {
                        std::lock_guard<std::mutex> lk(mu_);
                        status_.detail = "帧环重建失败：" + ring_.LastError();
                    } else {
                        ring_.SetMode(gotW, gotH, 0);
                    }
                } else {
                    // 容量够，只需要把头部几何信息改掉。
                    ring_.SetMode(gotW, gotH, 0);
                }
                ringW_ = gotW;
                ringH_ = gotH;
                // 录制器按新尺寸重开一轮分片（内部会 Stop 旧的再起新的，
                // 旧的那一轮留在磁盘上照常可导出）。
                EnsureRecorder(gotW, gotH);
            }

            const auto actEnd = std::chrono::steady_clock::now();
            captureNsInWindow += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(actEnd - actStart).count());

            bool published = false;
            // 这一拍做 Publish 判定的时刻。放在这里而不是用后面的 publishedAt：
            // 预览限流要的是"现在能不能发"，晚一拍量出来的时间点会被录制拷贝
            // 的耗时污染。
            const auto publishedNow = std::chrono::steady_clock::now();
            // 这一轮是否"有帧可录"。和 published **不是**一回事：published 只在
            // 真的把帧交到共享环里才为真，而环的 Publish 在没有消费者时会拒绝
            // （返回 false）。录制不能依赖有没有人预览 —— 服务该一直录。
            bool frameAction = false;
            if (cs == CaptureStatus::Ok || cs == CaptureStatus::NoChange) {
                // 快照本身就是整屏（桌面源）或窗口本体（WGC），没有裁剪这一步。
                const uint8_t* src = snap_.data();
                const uint32_t pubW = snapStride / 4u;
                const uint32_t pubH = gotH;
                // 没人看的时候不往环里写：环在没消费者时会拒绝 Publish，
                // 白拷一遍像素。录制不受影响（下面按 frameAction 喂）。
                if (!waitingSkip) {
                    // ---- 预览限流 ------------------------------------------
                    //
                    // 录制帧率（fps）和预览帧率是**两件不同的事**，不能绑在一起：
                    //   * 录制要按配置帧率喂编码器，少一帧时间轴就歪；
                    //   * 预览只要够顺眼就行，1440p 每帧 14.7MB 的 memcpy 在
                    //     120fps 下是 1.77GB/s，实测把循环从 120 拖到 91fps。
                    //
                    // 所以这里单独按 previewFps 抽样：够一个预览间隔才 Publish，
                    // 不够就跳过这一拍（画面照旧留在界面上，看不出来）。
                    // 录制完全不受影响 —— 它走下面的 frameAction，每拍都喂。
                    //
                    // previewFps = 0 表示不限，退回"每拍都发"的老行为。
                    const uint32_t pvFps = previewFps_.load(std::memory_order_relaxed);
                    bool allowPublish = true;
                    if (pvFps > 0) {
                        // 判据留 20% 容差，别按"必须够满一个间隔"卡。
                        //
                        // 采集节拍是 1/录制帧率，预览间隔是 1/previewFps，两个
                        // 周期一旦成整数倍关系，"够不够"就卡在等号边缘：120fps
                        // 采集 + 60fps 预览，标称间隔正好是两拍，但两拍的浮点/
                        // 时钟抖动常常是 16.66 < 16.67 差那么一点点，于是这一拍
                        // 被判跳过、只能等第三拍 —— 平均三拍发一次，60fps 实测
                        // 只有 48fps（本鱼量到过 Publish 48/s、界面 46.5fps）。
                        // 留出 20% 容差后稳定落回"两拍一发"。
                        const auto pvInterval =
                            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                std::chrono::duration<double>(0.8 / pvFps));
                        // 用"距上次发布够不够一个间隔"判，而不是累加绝对时间轴：
                        // 预览不需要严格恒定帧率，宽松判断反而不会和采集拍频打架。
                        if (lastPreviewPublish_.time_since_epoch().count() != 0 &&
                            (publishedNow - lastPreviewPublish_) < pvInterval) {
                            allowPublish = false;
                        }
                    }
                    if (allowPublish) {
                        const auto pubT0 = std::chrono::steady_clock::now();
                        published = ring_.Publish(src, snapStride, pubW, pubH, gotQpc);
                        if (published) {
                            lastPreviewPublish_ = publishedNow;
                            ++previewPublished_;
                        }
                        publishNsInWindow += static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - pubT0).count());
                    }
                    if (!allowPublish) ++previewSkipped_;
                }
                frameAction = true;
                (void)isNew;
            } else if (cs == CaptureStatus::AccessLost) {
                // 锁屏 / UAC / 分辨率切换 / 窗口关闭 / 窗口改尺寸：重建采集。
                //
                // 这里必须**一直重试**，不能试一次就判死。WGC 这一路最典型：
                // 用户关掉了被录的窗口，过几秒又开了一个同样的窗口（记事本、VSCode
                // 这种应用常见），此时按匹配规则重新解一次就该能接上。以前这里是
                // "重建一次失败 → status 置 Failed → 后面再也不会试"，于是只能重启服务。
                //
                // 【窗口改尺寸也走这条路】
                // WGC 的帧池是 Init 时按当时窗口尺寸建的，不会自己跟随；而重建
                // 帧池（IDirect3D11CaptureFramePool::Recreate）实测会把投帧渠道
                // 弄断 —— 之后再也不来 FrameArrived，画面永远停在旧内容上。
                // 唯一可靠的办法是重建整个采集会话，正好就是这条路径。
                // 用 resizeNotice 区分：尺寸变了的话窗口还在、句柄还是那个，
                // 应该原地重开，不能重新按规则匹配（那可能抓到另一个同类窗口）。
                const bool byResize = useWgc && windowCapture_->TakeResizeNotice();
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    // 状态必须一起改成 Reconnecting，不能只改 detail。
                    //
                    // 踩过的坑：这里以前只写 detail，state 还是上一拍的 Live。
                    // 界面判断"有没有画面"看的是 state，于是窗口已经关掉、
                    // 采集早就断了，界面照样显示"录制中 / 有画面"，NO SIGNAL
                    // 永远盖不上来 —— 用户看到的就是"最后一帧卡死在那儿"。
                    // 重建那 6 次一共要等五六秒，这段时间足够长，状态必须诚实。
                    status_.state = SessionState::Reconnecting;
                    status_.detail = byResize ? "窗口尺寸变了，正在重建采集" : "采集被中断，正在重建";
                    status_.recording = false;
                }
                // 先停掉录制器：画面已经断了，继续往里喂旧帧会把时间轴拉长。
                // 重建成功后 EnsureRecorder 会按新尺寸重新起一份。
                bool rebuilt = false;
                uint32_t newW = 0;
                uint32_t newH = 0;
                for (int attempt = 0; attempt < kRebuildAttempts; ++attempt) {
                    if (stopRequested_.load(std::memory_order_acquire)) break;
                    // 改尺寸引起的重建：窗口还在，没必要等，第一轮立刻干。
                    // 窗口关了引起的重建：等 200ms 给用户/系统一点时间。
                    if (attempt > 0 || !byResize) Sleep(attempt == 0 ? 200 : 1000);
                    if (stopRequested_.load(std::memory_order_acquire)) break;
                    if (useWgc) {
                        // 重新 Init 之前必须 Cleanup：泵线程、帧池、会话都得收干净，
                        // 否则老会话的 FrameArrived 还会往老的 frameArrived 上写。
                        // 句柄要**先**取下来 —— Cleanup 会把 hwnd_ 清空。
                        const HWND keep = static_cast<HWND>(windowCapture_->Hwnd());
                        windowCapture_->Cleanup();
                        if (byResize && keep) {
                            // 尺寸变化：句柄还是那个窗口，原地重开就行。
                            // 窗口在重建期间又关了的话 Init 会失败，下一轮
                            // 退化成"重新匹配"。
                            std::string werr;
                            if (windowCapture_->Init(keep, werr)) {
                                rebuilt = true;
                                newW = windowCapture_->Width();
                                newH = windowCapture_->Height();
                                break;
                            }
                            // 句柄失效了：落到下面的重新匹配逻辑。
                        }
                        // WGC 这一路：窗口关了就得**重新按匹配规则解一个**再 Init。
                        // （WindowCapture 会置 closed；沿用旧句柄是死路。）
                        WindowInfo again;
                        if (!profile_.match.Valid()) break;  // 没规则，再试也没用
                        if (!ResolveWindow(profile_.match, again) || !again.hwnd) continue;
                        std::string werr;
                        if (windowCapture_->Init(again.hwnd, werr)) {
                            rebuilt = true;
                            newW = windowCapture_->Width();
                            newH = windowCapture_->Height();
                            std::lock_guard<std::mutex> lk(mu_);
                            status_.hwnd = reinterpret_cast<uint64_t>(again.hwnd);
                            status_.matchedTitle = again.title;
                            break;
                        }
                        // 窗口变了但 WGC 还抓不到（新窗口刚创建、合成器还没上）：
                        // 状态说清楚，等下一轮继续试。
                        {
                            std::lock_guard<std::mutex> lk(mu_);
                            status_.detail = "窗口已找到（" + again.title + "），画面还没就绪：" + werr;
                        }
                    } else {
                        capture_->Cleanup();
                        if (capture_->Init(profile_.source.monitorIndex, kCaptureTimeoutMs)) {
                            rebuilt = true;
                            newW = capture_->Width();
                            newH = capture_->Height();
                            break;
                        }
                    }
                }
                if (rebuilt) {
                    // 窗口被拉大之后可能超出帧环的预留容量（建映射时按 1.5 倍
                    // 预留，见 RingCapWidth）。超了 Publish 会静默失败 —— 预览
                    // 变成"有状态没画面"，特别难查。所以这里显式判一次。
                    if (ring_.NeedsGrow(newW, newH)) {
                        if (!ring_.Open(profile_.id, RingCapWidth(newW), RingCapHeight(newH))) {
                            std::lock_guard<std::mutex> lk(mu_);
                            status_.detail = "帧环重建失败：" + ring_.LastError();
                        } else {
                            ring_.SetMode(newW, newH, 0);
                        }
                    } else {
                        ring_.SetMode(newW, newH, 0);
                    }
                    ringW_ = newW;
                    ringH_ = newH;
                    // 分辨率可能变了：录制器尺寸固定，得重建一份才能继续录。
                    EnsureRecorder(newW, newH);
                    std::lock_guard<std::mutex> lk(mu_);
                    status_.state = SessionState::Live;
                    // detail 必须在这里重写回正常描述。
                    //
                    // 踩过的坑：只恢复 state 不恢复 detail，界面上的提示就永远
                    // 停在"窗口尺寸变了，正在重建采集"—— 画面明明已经在正常
                    // 刷新了，用户却以为一直在重建。
                    if (useWgc && newW > 0) {
                        status_.detail = "窗口（WGC）：" + status_.matchedTitle;
                    } else if (!useWgc && capture_) {
                        status_.detail = "整屏（显示器 " +
                                         std::to_string(profile_.source.monitorIndex) + "）";
                    }
                } else {
                    // 重试用完还是重建不起来：别在这里"判死"。
                    //
                    // 置一下 Failed 让界面看到真实状态，然后把控制权交回**外层**那个
                    // "没源就每秒重试 ResolveSource" 的循环 —— 它会让这一路在窗口
                    // 重新出现时自己活过来。以前这里是 continue 回主循环，主循环
                    // 每拍都会撞一次 AccessLost，等于没有节制的重试；而如果 ws
                    // 真的坏了，线程就永久卡在 Failed 上，窗口再开也接不回来。
                    std::lock_guard<std::mutex> lk(mu_);
                    status_.state = SessionState::Failed;
                    status_.detail = useWgc ? "目标窗口不见了，正在等它回来" : "桌面采集中断，正在等它恢复";
                    break;  // 跳出主循环 → 走外层的解析重试
                }
                lastPublish = actEnd;
                continue;
            }

            // 桌面静止：按目标帧率重复发布**这一路自己的**最后一帧，让下游始终有稳定帧流。
            if (!published) {
                // 重发也要服从预览上限。
                //
                // 这里漏过一次，症状很难猜：上面那条正常的 Publish 已经按
                // previewFps 限流了，但桌面一静止就走进这个分支、按**采集**
                // 帧率重发（120fps 的采集就是 120 次/秒），把上限整个绕过。
                // 实测设置页填 60，界面画出 83fps，而服务端诊断显示
                // "预览 Publish 39/s" —— 差的那些正是从这里出去的。
                const uint32_t rvFps = previewFps_.load(std::memory_order_relaxed);
                bool allowRepublish = true;
                if (rvFps > 0) {
                    // 重发的间隔取「预览上限间隔」和「采集帧间隔」里**更慢**的那个。
                    //
                    // 只按预览上限判是不够的：预览上限是"最多多快"，不是"应该多快"。
                    // 30fps 采集的一路配 60fps 上限时，桌面一静止就按 60/s 重发同一
                    // 张图 —— 界面上那一路的数字就变成 60fps，用户看到的是"我录的是
                    // 30fps，凭什么旁边那路显示 60fps"。画面内容其实一帧没变，只是
                    // 同一帧被反复塞进环里，纯粹白烧带宽和 GPU 上传。
                    // 按采集帧率封顶后，30fps 的源最多也就发 30/s，数字才对得上。
                    const double slowerFps = std::min<double>(static_cast<double>(rvFps), fps);
                    // 和上面那条 Publish 用同一套容差判据，理由见那里的注释。
                    const auto rvInterval =
                        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                            std::chrono::duration<double>(0.8 / slowerFps));
                    if (lastPreviewPublish_.time_since_epoch().count() != 0 &&
                        (actStart - lastPreviewPublish_) < rvInterval) {
                        allowRepublish = false;
                    }
                }
                if (allowRepublish && actStart - lastRepublish >= republishInterval &&
                    snapW_ > 0 && !snap_.empty()) {
                    const uint32_t snapStride = snapW_ * 4u;
                    const uint8_t* src = snap_.data();
                    published = ring_.RepublishLast(src, snapStride);
                    // 桌面静止也算一帧：录像要连续帧流，缺帧会让时间轴变短。
                    frameAction = true;
                    lastRepublish = actStart;
                    if (published) {
                        lastPreviewPublish_ = actStart;
                        ++previewPublished_;
                    }
                }
            } else {
                lastRepublish = actStart;
            }

            // ---- 喂录制器 ------------------------------------------------------
            // 数据源是 snap_（这一路自己的快照），不再是采集器内部的 staging ——
            // 后者会被别的会话线程覆盖。
            //
            // 判据用 frameAction 而**不是** published：published 只在帧真的进了
            // 共享环时才为真，而环在「没有消费者」时会拒绝写入。用 published
            // 的话，界面没开时录像是空的 —— 但需求 5 要的是服务一直录。
            if (recorder_ && frameAction && snapW_ > 0 && !snap_.empty()) {
                const uint32_t snapStride = snapW_ * 4u;
                const auto feedT0 = std::chrono::steady_clock::now();
                // 传的必须是**这一帧的真实尺寸**（snapW_/snapH_），不是建录制器
                // 时的 recorderW_/recorderH_：窗口拖大拖小之后两者不同，而
                // 编码器靠这个尺寸决定要不要切一片新的（见 DESIGN.md 9.5）。
                recorder_->Feed(snap_.data(), snapStride, snapW_, snapH_);
                feedNsInWindow += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - feedT0).count());
            }

            // 关键：发布时刻在**动作之后**采样。早采（循环开头那次）会把
            // "睡到该发的时刻"之前的时间也算进去，节流器于是每轮都以为自己
            // 早到了、继续往后推 —— 表现就是帧率一路掉到 50 出头。
            const auto publishedAt = std::chrono::steady_clock::now();
            lastPublish = publishedAt;
            // 帧率统计按 frameAction 算，**不能**按 published 算。
            //
            // 踩过的坑：这里原来是 `if (published) framesInWindow++;`，而 published
            // 只有在真的往共享环里写过帧时才为真。环的 Publish 在没有消费者时
            // 直接返回 false（FrameRing.cpp: `if (pid == 0) return false;`），
            // 于是**没人预览时 actualFps 永远显示 0.00**，哪怕录制一直在正常出帧。
            // 服务是常驻录制的，不接界面才是常态，这个 0 极具误导性。
            //
            // frameAction 的含义才是"这一拍确实产生了一帧"（新帧或静止重发），
            // 录制器也是按它喂的，两者口径一致。
            if (frameAction) framesInWindow++;
            // 把这一拍的结果留给下一轮的"能不能回到 Live"判断。
            lastFrameOk = frameAction;
            // "干活"估计值，用来把下一轮的 deadline 往前挪（见 nextDue 的注释）。
            //
            // 用**积分修正**而不是纯 EMA：EMA 有稳态偏差 —— 它只会从下面慢慢逼近，
            // 结果欠补，实测跑成 62.6fps（比要求快 5%）。积分项直接把
            // 「实测间隔 - 目标间隔」的残差积掉，残差归零后估计值才停下来。
            const auto intervalNow =
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    publishedAt - previousPublish_);
            if (havePreviousPublish_) {
                const double errMs =
                    static_cast<double>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            intervalNow - frameInterval)
                            .count()) /
                    1e6;
                workEstimateMs_ += kPacingGain * errMs;
                if (workEstimateMs_ < 0.0) workEstimateMs_ = 0.0;
                // 上限必须**跟得上目标帧率**，不能写死。
                //
                // 踩过的坑：这里原来是 8.0ms，在 60fps（16.67ms 间隔）下够用，
                // 于是没人发现。但 240fps 的间隔只有 4.17ms —— 干活时间一旦超过
                // 8ms 就被截断，"扣掉干活时间"这件事永远补不到位，实际帧率必然
                // 塌到 1/(8ms+干活) 上，而且怎么调都上不去。
                // 取目标间隔的一半做上限：既允许补偿真实开销，又不至于把 deadline
                // 推到过去（那会让节流器空转烧 CPU）。
                //
                // 这里**不能**再套一个"至少 8ms"的下限。改成本形式之前的写法是
                // max(8.0, 0.5*interval)，看起来是"给小帧率留余量"，实际后果是：
                // 240fps 的 0.5*interval 只有 2.08ms，被 8.0 顶上去，于是补偿上限
                // 又变回 8ms —— 和上面那个写死 8.0 的坑一模一样，只是换了个位置，
                // 高帧率照样补不到位。0.5*interval 本身就是安全上界（提前量最多
                // 半个间隔，不会把 deadline 推到过去）。
                const double workCapMs =
                    0.5 * std::chrono::duration<double, std::milli>(frameInterval).count();
                if (workEstimateMs_ > workCapMs) workEstimateMs_ = workCapMs;
            }
            previousPublish_ = publishedAt;
            havePreviousPublish_ = true;
            workEstimate_ = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double, std::milli>(workEstimateMs_));

            // ---- 每秒汇报一次 -------------------------------------------------
            const auto sinceWindow = publishedAt - windowStart;
            if (sinceWindow >= std::chrono::seconds(1)) {
                RefreshRecordStatus();
                const double secs = std::chrono::duration<double>(sinceWindow).count();
                const double instant = static_cast<double>(framesInWindow) / secs;
                std::lock_guard<std::mutex> lk(mu_);
                // ---- actualFps 平滑 -------------------------------------------
                //
                // 原来直接把"这一窗帧数 ÷ 这一窗实际时长"写进 actualFps。窗口是
                // 从上一个 publishedAt 量起的，长度在 1 秒上下浮动 ±半帧，于是
                // 稳定 60fps 时会交替报出 61.5 / 58.9 这种数字 —— 界面一会儿
                // 61 一会儿 62，看着像帧数不稳，其实采集一拍没丢。
                //
                // 这里做两件事：
                //   1) 用 EMA 平滑（0.6 权重给历史），单窗的量化抖动被摊掉；
                //   2) 落到目标帧率的整数上。人眼要的是"跑满没有"，60 就是 60，
                //      没必要把 -- 也显示出来。差超过 5% 才认为真的掉帧，照实报
                //      （高负载掉到 167 时用户要看得见），此时取整到整数即可。
                const double kFpsEma = 0.6;
                if (!haveFpsEma_) {
                    fpsEma_ = instant;
                    haveFpsEma_ = true;
                } else {
                    fpsEma_ = kFpsEma * fpsEma_ + (1.0 - kFpsEma) * instant;
                }
                double shown = fpsEma_;
                const double target = status_.targetFps;
                if (target > 0.5) {
                    // 和目标帧率相差 5% 以内 -> 认定跑满，显示目标整数值；
                    // 超了说明真掉帧，按实际值显示（仍取整，小数没意义）。
                    shown = (std::fabs(fpsEma_ - target) <= target * 0.05)
                                ? target
                                : std::floor(fpsEma_ + 0.5);
                }
                status_.actualFps = shown;
                status_.gapMs = loopsInWindow > 0
                                    ? secs * 1000.0 / static_cast<double>(loopsInWindow)
                                    : 0.0;
                status_.captureMs = loopsInWindow > 0
                                        ? static_cast<double>(captureNsInWindow) / 1e6 /
                                              static_cast<double>(loopsInWindow)
                                        : 0.0;
                status_.produced = ring_.Produced();
                status_.dropped = ring_.Dropped();
                status_.previewPublished = previewPublished_;
                status_.previewSkipped = previewSkipped_;
                status_.previewFps = previewFps_.load(std::memory_order_relaxed);
                status_.publishMs = loopsInWindow > 0
                                        ? static_cast<double>(publishNsInWindow) / 1e6 /
                                              static_cast<double>(loopsInWindow)
                                        : 0.0;
                status_.feedMs = loopsInWindow > 0
                                     ? static_cast<double>(feedNsInWindow) / 1e6 /
                                           static_cast<double>(loopsInWindow)
                                     : 0.0;
                // 同上：WGC 模式下 capture_ 是 nullptr，必须分派，否则每秒这里崩一次。
                if (useWgc) {
                    status_.width = expectW;
                    status_.height = expectH;
                } else {
                    status_.width = capture_->Width();
                    status_.height = capture_->Height();
                }
                windowStart = publishedAt;
                framesInWindow = 0;
                captureNsInWindow = 0;
                publishNsInWindow = 0;
                feedNsInWindow = 0;
                loopsInWindow = 0;
            }
        }  // 内层主循环：exit 只有两种原因 —— 收到 Stop，或者采集链路彻底断了

        // 内层循环退出的原因：如果**不是** Stop，说明这一轮采集没了（目标窗口
        // 关了、显示器没了），那就回到外层重新解析，等着它回来。
        if (!stopRequested_.load(std::memory_order_acquire)) {
            // 这一轮的帧环/录制器要收干净再重来，否则新的解析会撞上自己。
            if (recorder_) {
                recorder_->Stop();
                recorder_ = nullptr;
                recorderW_ = 0;
                recorderH_ = 0;
            }
            // 帧环**不拆**：消费者可能正读着它，拆掉会让对端直接崩（见下面
            // "不主动销毁帧环"那段说明）。重连时直接复用同一个环，Open 会拿回
            // 已有的映射。
            windowCapture_.reset();
            ownCapture_.reset();
            capture_ = nullptr;
            {
                std::lock_guard<std::mutex> lk(mu_);
                status_.state = SessionState::Reconnecting;
                status_.detail = "采集链路中断，正在重新连接";
                status_.recording = false;
                status_.consumerPid = 0;
            }
            continue;  // 回到外层，重新解析
        }
        break;  // 真的是 Stop
    }  // 外层循环

    // 收录制器：让 MfEncoder 把当前分片 Finalize 掉（写完索引再关文件）。
    // 放在这里（线程末尾）而不是析构里，是为了让"停止"这件事有个确定的
    // 收尾点：析构可能发生在服务被强杀之后，那时没 Finalize 的分片就留在
    // 磁盘上（fMP4 自带 moof，前面的部分仍可读）。
    if (recorder_) {
        recorder_->Stop();
        RefreshRecordStatus();
    }

    // 注意这里**不**主动销毁帧环。
    //
    // 消费者（Electron 渲染进程）手里的内存是 CreateFileMapping + MapViewOfFile
    // 得来的。只要消费者还有人映射着，我们这边关句柄也只是少一个引用，
    // 段不会被拆掉，它那边的视图依然有效。但如果我们 UnmapView + CloseHandle
    // 之后恰好是最后一个引用，段就没了 —— 消费者再读那点内存就是访问冲突（直接崩）。
    //
    // 所以环的句柄交给 Session 析构时收（那时配置已下线、界面端也该重连了）。
    // 会话线程自己退出时让它留着，宁可多占一点，也比让对端崩掉强。
    {
        std::lock_guard<std::mutex> lk(mu_);
        status_.attached = false;
        status_.state = SessionState::Disabled;
        status_.detail = "已停止";
    }
    timeEndPeriod(1);
    running_.store(false, std::memory_order_release);
}

} // namespace shadow
