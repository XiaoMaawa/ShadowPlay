// MfEncoder.h —— 进程内 Media Foundation 分片编码器（取代 ffmpeg.exe 子进程）。
//
// 为什么要有这个文件（需求：不要 ffmpeg.exe）：
//   原来录像是起一个 ffmpeg.exe 子进程，靠 `-f segment -segment_wrap N` 让
//   ffmpeg 帮我们做「按固定时长切分 + 环形覆盖 + 写清单」三件事。现在要求
//   不许有子进程，这三件事得自己来。Media Foundation 是 Windows 自带的，
//   SinkWriter 一行 FFmpeg 都不用装。
//
// 【为什么分片是 .mp4（fMP4）而不是 .ts】
//   查过 SDK（10.0.26100.0 的 mfidl.h），MFTranscodeContainerType_* 里
//   **根本没有 MPEG2TS** —— Media Foundation 的 SinkWriter 不支持写 TS 容器。
//   所以 ffmpeg 那套 .ts 分片没法照搬，只能走 MP4 系。
//   选 fMP4（fragmented MP4）而不是普通 MP4：普通 MP4 的 moov 索引是
//   Finalize 时才写的，进程被杀/断电时那一片就是读不出来的废文件。分片录像
//   最怕这个。fMP4 每个 moof 自带索引，写多少就多少能读 —— 和 TS 的
//   「流式自描述」是同一个道理。
//
// 【为什么必须关 B 帧】（实测定论，别改）
//   探针 native/probe_mf_seg.cpp 验证过：多分片直拷拼接时，如果每片都有
//   B 帧，每片的 picture order count 都从 0 重新开始，解码器跨分片接不上，
//   报 "co located POCs unavailable"，120 帧只解出 30 帧。关掉 B 帧后
//   120/120 完美。屏幕录制要的是低延迟不是压缩率，B 帧本来也没价值。
//   设的地方是 MF_SINK_WRITER_ENCODER_CONFIG（SinkWriter 唯一能往编码器
//   传属性的口子），代码见 OpenSegment()。
//
// 【为什么必须关节流】
//   SinkWriter 默认 **按实时速度节流** 写入（它假设你在做转码播放）。
//   我们喂的是采集流，本来就按实时产生，再被节流一次会让喂帧线程卡住、
//   进而把采集节拍拖垮。必须设 MF_SINK_WRITER_DISABLE_THROTTLING。
//
// 线程模型：本类**不是线程安全**的，也不自带线程。调用方（SegmentRecorder
// 的喂帧线程）串行调用 Write() / Finish()。所有 MF 调用都在同一线程上。
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace shadow {

/**
 * 在当前线程上初始化 Media Foundation（进程级只做一次 MFStartup）。
 *
 * 每个要用 MF 的线程都得先调它 —— 内部会 CoInitializeEx(MULTITHREADED)。
 * 返回 false 表示 MF 起不来（系统太老或缺 Media Feature Pack）。
 * 可重复调用，引用计数到 0 时才真正 MFShutdown（见 MfRelease）。
 */
bool MfAcquire();

/** 与 MfAcquire() 配对。 */
void MfRelease();

/**
 * 一路编码器的状态。
 *
 * 和 RecorderStats 分开：RecorderStats 是给界面看的（含环形窗口、丢帧等
 * 业务语义），MfEncoder 只管"这台编码器现在怎么样"。
 */
struct MfEncoderStatus {
    bool        running = false;
    bool        failed = false;
    std::string detail;
    uint32_t    segmentsWritten = 0;   // 本轮写出去的分片总数
    uint64_t    framesWritten = 0;     // 编码进分片的帧数
};

/**
 * 【已弃用】全进程共用的 D3D11 设备 + IMFDXGIDeviceManager。
 *
 * 原本打算用它把一份常驻设备交给 SinkWriter，省掉每片重建设备带来的句柄
 * 泄漏。探针 native/probe_seg_handles.cpp 量到的那组数据（硬件编码 +34
 * 句柄/片、共用设备后 +3 句柄/片）确实成立 —— 但那是**只建管线不喂系统内存
 * 样本**的前提下量出来的。
 *
 * 一旦真的开始喂帧，SinkWriter 拿到 D3D manager 就要求输入是 D3D 纹理，
 * 我们喂的是系统内存 RGB32，于是 SetInputMediaType 直接失败：
 *     MF_E_UNSUPPORTED_D3D_TYPE  0xC00D6D76
 * 更坑的是它不立刻失败 —— 前 70 多帧照常写盘（转换器还有缓冲），随后
 * WriteSample 永久报这个错，recordFrames 冻住、分片停在 1 个。
 *
 * 所以这两个函数**目前没有任何调用点**，保留只为记下这条路为什么走不通。
 * 真要根治句柄增长，得把整条通路改成「采集纹理 → 编码器」全程留在 GPU 上，
 * 那是独立的一次改造。
 */
bool MfSharedDeviceReady();

/** 【已弃用】原本给 SinkWriter 的 attributes 里填 MF_SINK_WRITER_D3D_MANAGER。 */
void MfApplySharedDevice(void* attributes);

/**
 * 把 BGRA 帧流编码成 fMP4 分片，按固定时长滚动，**按墙钟**保留最近 N 秒。
 *
 * 目录结构（与 ffmpeg 时代保持一致，导出端不用改）：
 *   <dir>/000000.mp4
 *   <dir>/000001.mp4
 *   ...
 *   <dir>/index.csv        <- 自维护的清单，格式同 ffmpeg 的 segment_list
 *
 * 【保留策略：按时间，不按片数】见 DESIGN.md 9.9。
 *
 * 早期是「第 n 片写在 n % keepSegments 号槽位」，等价于 ffmpeg 的
 * `-segment_wrap`。那套只在每片等长时成立。现在尺寸一变就立刻切片（用户
 * 明确要求：过渡时不要录满 2 秒，马上把画面切出去），拖动窗口时 Windows 是
 * 逐像素连续改尺寸的，一次拖拽能切出十几片零点几秒的过渡片 —— 槽位被它们
 * 占满，环里真实内容只剩一两秒，导出于是几乎全程 NO SIGNAL（主人实测报的）。
 *
 * 现在：PurgeExpired() 按**墙钟**把跌出回放窗口的片连文件一起扔掉；槽位数量
 * 给到标称片数的 8 倍（keepSlots_）当文件名空间；PickSlot() 先挑空槽，全占满
 * 才复用最旧的那片。这样"能回放多久"只由时间决定，与片长分布无关。
 *
 * 尺寸过渡期间还有一层保护：当前片**一帧都没写**时，新尺寸直接换掉这一片
 * （见 ResizeTo），根本不留分片文件。所以真正的过渡碎屏连槽位都碰不到。
 */
class MfEncoder {
public:
    MfEncoder();
    ~MfEncoder();

    MfEncoder(const MfEncoder&) = delete;
    MfEncoder& operator=(const MfEncoder&) = delete;

    /**
     * 帧率硬上限：**120fps**。超过它的一律夹回来。
     *
     * 【为什么不再给 144/165/240】这条是主人拍板的（「直接把 120 以上的帧率
     * 全部删除，不包括 120fps」）。在此之前这套代码为了够到 240 付了一串
     * 代价，而 120 以上每一档都是负收益：
     *
     *   * 1440p 的硬件 H.264 编码器物理上限是 172fps（探针实测 173 起就
     *     `SetInputMediaType` 报 `0xC00D36B4`），所以「240」这一档从来录不
     *     到 240，只能降档；
     *   * 165fps 那一档的每帧成本 6.09ms（转换 3.24 + WriteSample 2.25）
     *     已经顶破 6.06ms 的帧预算，喂帧线程每拍都欠一帧，真实画面只剩
     *     50%；
     *   * 120 及以下是实测的稳定区间：真实画面 89~93%、落盘 100%、PTS 零空洞。
     *
     * 保留这条夹取是因为**手改过 profiles.json 的老配置还在磁盘上**，不能让
     * 一个 `"fps": 240` 继续把硬件顶到降档 + 看门狗降频那一串连锁里。
     */
    static constexpr uint32_t kMaxFps = 120u;

    /**
     * 开始录制。dir 必须是已存在的绝对路径。
     *
     * @param fps            喂进来的帧率（时间轴按它算，必须和实际喂帧速率一致）
     * @param bitrateMbps    目标码率；0 表示按分辨率估一个
     * @param segmentSeconds 每片目标时长；0 -> 2
     * @param keepSegments   回放窗口的标称片数（决定保留多少**秒**，不决定槽位数）；0 -> 2
     *
     * 【keepSegments 为什么只是下限】见 DESIGN.md 9.9。槽位是第 n 片落在
     * `n % keepSegments` 号，所以保留能力恒等于 keepSegments 片 —— 而"片"
     * 不是等长的：用户拖一次窗口，ResizeTo 就会连切两片（缩小一片、恢复一片），
     * 每片可能只有零点几秒。10 个槽位被这种过渡片占满时，环里实际只剩一两秒
     * 内容，导出就只能整段补 NO SIGNAL（主人实测："明明录了 20 多秒，导出
     * 几乎全程 no signal"）。
     *
     * 所以 Start 会按「回放窗口 ÷ 片长」算出一个时间预算下限，实际槽位数取
     * max(keepSegments, 时间预算)。这样即使全是 0.5 秒的过渡片，也仍然装得下
     * 一整个回放窗口。
     *
     * 失败不抛异常，看 Status()。成功后调用方只管 Write()。
     */
    bool Start(const std::string& dir, uint32_t width, uint32_t height, uint32_t fps,
               uint32_t bitrateMbps, uint32_t segmentSeconds, uint32_t keepSegments);

    /**
     * 编码一帧 BGRA（自上而下，stride 可能 != width*4）。
     *
     * **同步**：会等编码器收下这一帧才返回。调用方必须在专用线程里跑
     * （SegmentRecorder 的喂帧线程就是为这个存在的），不能放在采集线程。
     *
     * 返回 false 表示编码器坏了（Status().detail 有原因），调用方应停止喂帧。
     *
     * 【pixels == nullptr = 再来一帧上一帧的画面】
     * 喂帧线程补帧时不用把同一份 BGRA 再喂一遍 —— 传 nullptr 就直接复用样本
     * 缓冲里已经转好的 NV12，只换时间戳再 WriteSample 一次。省下的正是
     * ConvertToNv12 + memcpy 那 8~9ms（1440p）。width/height 仍要传当前画布
     * 尺寸（补帧不会触发换尺寸）。此时 stride 参数被忽略。
     */
    bool Write(const void* pixels, uint32_t stride, uint32_t width, uint32_t height);

    /**
     * 编码器**实际**跑在多少帧率上（可能低于 Start 时要求的）。
     *
     * 【为什么会有差别】硬件 H.264 编码器对「分辨率 × 帧率」有吞吐上限，超出
     * 就直接拒绝输入类型。实测本机 NVENC（`nvEncMFTH264x.dll`）：
     *
     *   2560x1440 → 172 帧通过，174 帧起 `SetInputMediaType` 报
     *               0xC00D36B4 MF_E_INVALIDMEDIATYPE
     *   1920x1080 → 240 帧通过，300 帧失败
     *   1280x720  → 300 帧仍通过
     *   3840x2160 → 165 帧通过，180 帧失败
     *
     * 也就是上限按分辨率阶梯式变化，跟宏块吞吐走。以前遇到这种情况整路录制
     * 直接失败（`recordDetail` 写着「编码器不接受 NV12 输入」、分片 0 字节），
     * 界面上的表现就是主人说的「帧率设的太高，导出的视频卡成 ppt」—— 其实
     * 是一帧都没录进去。现在改成**自动降到编码器能接受的最高帧率**继续录，
     * 宁可帧率不是用户设的那个，也不能整路废掉。
     *
     * 时间轴、分片切分、index.csv 全部按这个实际帧率算，所以导出的时长仍然
     * 是真实的 —— 只是每秒的帧数比配置里少。
     */
    uint32_t EffectiveFps() const { return fps_; }

    /**
     * 用户配置里写的帧率（可能高于 EffectiveFps()，被降过档）。
     *
     * 【为什么需要单独一个】run.json 要把「配置的」和「实际用的」都写下来：
     * 配置 240、实际 165 时，喂帧线程必须按 165 灌帧（否则时间轴错位，见
     * SegmentRecorder::Start 里那段注释），而界面上仍要能告诉用户"你配的
     * 240 录成了 165"。只有 fps_ 一个字段时，喂帧线程按它算节拍，就没法再
     * 记配置值了。
     */
    uint32_t RequestedFps() const { return requestedFps_; }

    /** 收尾：finalize 当前分片。可重复调用。 */
    void Finish();

    /**
     * 把**正在写的那一片**立刻收尾落盘，然后接着开一片新的。
     *
     * 【为什么需要】用户点"导出"时，回放窗口的右端应该是**他按下按钮的那一
     * 刻**。但录制器是按固定片长切片的：点导出时总有一片正在写，它还没
     * Finalize，fMP4 的 moov/moof 索引不全，index.csv 里也没有它 —— 导出端
     * 只能读到最后一片**已完成**的分片，于是"点导出之前的那半秒到一秒"
     * 永远导不出来。用户的原话：「我点击导出视频的时候，前一秒，或者说
     * 前半秒，画面都丢失了，应该一直录到我点击按钮才停止才对啊」。
     *
     * 这里同步收掉当前片（Finalize + 写 index.csv），紧接着开新片继续录 ——
     * 录制不中断、不换轮次目录、切片序号继续递增。代价是那一片会比标称短
     * （0~2 秒），导出端本来就按每片**真实时长**铺时间轴，短片不影响拼接。
     *
     * 返回值：真的切了一片返回 true；没有在录 / 当前片一帧都没有（切了也
     * 没意义）返回 false —— 调用方据此决定要不要等一小会儿重试。
     */
    bool FlushSegment();

    /**
     * 就地改帧率（不中断录制、不换轮次目录、切片序号继续递增）。
     *
     * 【为什么需要】用户在界面里把帧率从 144 改成 60（或反过来）时，服务收到
     * 的是 reload。原来的 reload 判定「配置变了」就把整路 Session 拆掉重建：
     * 新 Session -> 新 SegmentRecorder -> 新 MfEncoder -> 新轮次目录，于是
     *   1. 旧片刚收尾、新片用**真实墙钟**开（nextSegStartUnix_ 被清零），
     *      中间那段重建耗时（实测 300~500ms，WGC 更久）变成时间轴空洞，
     *      超过 gapFloor（0.5 秒）就被导出端补成 NO SIGNAL；
     *   2. recordSegments 从 0 重新数，界面的导出按钮变灰；
     *   3. 刚切过去那一瞬间点导出，那一路还没有任何分片 —— 报"该路未在录制"。
     *
     * 用户的原话：「降帧率的时候，会出现短暂的 no signal，和导出按钮被禁用，
     * 帧率刚切换那会点击会出现该路未在录制」。
     *
     * 【为什么必须重开分片】帧率写在 SinkWriter 的**输出流**上
     * （MFSetAttributeRatio(MF_MT_FRAME_RATE)），改它只能新建 SinkWriter。
     * 但重开分片 ≠ 重建会话：segmentSeq_ 继续递增、nextSegStartUnix_ 继续
     * 顺延（墙钟连续）、轮次目录不变，所以导出端看到的是一条连续的时间轴。
     *
     * 返回 true 表示已经按新帧率开好新片（或者新帧率跟当前一致、什么都不用做）。
     */
    bool SetFps(uint32_t fps);

    /**
     * 就地改码率（不中断录制、不换轮次目录、切片序号继续递增）。
     *
     * 【为什么需要】用户报「配置改帧率、改码率什么的，一保存就该立刻落地」
     * （m14071）。帧率已经能就地改了（SetFps），但码率原来只写在
     * MfEncoder::Start() 的入参里，改它只能靠 reload 把整路 Session 拆掉重建：
     * 新轮次目录、recordSegments 归零（导出按钮变灰）、时间轴出现空洞。
     *
     * 码率和帧率一样写在 SinkWriter 的**输出流**上
     * （MakeH264OutType 里的 MF_MT_AVG_BITRATE），所以也必须重开一片才生效 ——
     * 但重开分片 ≠ 重建会话：segmentSeq_ 继续递增、nextSegStartUnix_ 继续
     * 顺延、轮次目录不变，导出端看到的还是一条连续的时间轴。
     *
     * 【0 的含义】0 = 没配，按「像素数 × 0.15 bit × 帧率」估（与 Start 同口径）。
     * 注意：以前 SetFps/ResizeTo 会**无条件**用那个公式覆盖码率，导致用户在
     * 界面里配的 5 Mbps 一改帧率就被冲掉。现在配了就照配的来，没配才估。
     */
    bool SetBitrate(uint32_t bitrateMbps);

    /**
     * 就地改回放窗口（保留多少秒），不中断录制、不重建会话。
     *
     * 【为什么需要】用户把回放时长从 120 秒改成 10 秒时，以前只有 "重建整路
     * Session" 这一条路：新的录制器 = 新轮次目录 + 分片序号归零 + 帧环清空，
     * 界面上就是"预览突然 NO SIGNAL、像是重启了一次"；如果当时正在导出，
     * reload 线程还会等导出（或把会话放回去），导出跟着变慢。
     *
     * 回放窗口只决定"保留多久的内容"，和编码器参数无关 —— 改它只需要换一个
     * 保留预算，下一次分片轮转（几十毫秒到 2 秒内）就会把跌出窗口的旧片清掉。
     * 录制时间轴、轮次目录、分片序号全部原地不动。
     *
     * @param keepSegments 保留的分片数（= ceil(回放秒数 / 分片秒数)），至少 2
     */
    void SetKeepSegments(uint32_t keepSegments);

    /** 当前实际生效的码率（Mbps，四舍五入）。界面用它核对「配置的值真的落地了」。 */
    uint32_t EffectiveBitrateMbps() const { return (bitrateBps_ + 500000u) / 1000000u; }

    /**
     * 喂一块 PCM 给常驻 AAC 编码器（音频采集线程调用）。
     *
     * 【为什么这里不直接编码】AAC 编码器（IMFTransform）不是线程安全的，而且它
     * 需要 COM 套间 + 进程级 MF 引用。音频采集线程是另一条独立线程，让它在这里
     * 碰 MF 会引入跨线程 COM 的麻烦。所以这个函数只把 PCM **投进一个线程安全
     * 的队列**就返回（微秒级），真正的编码在喂帧线程里做（见 PumpAudio）。
     *
     * @param pcm         交错的 S16LE 采样（立体声，48kHz），与 AudioCapture 同格式
     * @param frames      采样帧数（每帧 2 个 int16）
     * @param startWallMs 这一块第一帧对应的墙钟毫秒（用于把 AAC 样本归属到分片）
     */
    void FeedPcm(const int16_t* pcm, size_t frames, int64_t startWallMs);

    /** 常驻 AAC 编码器是否可用（创建成功且类型协商通过）。 */
    bool AudioEncoderReady() const;

    /** 用户/配置要的码率（Mbps，0 = 没配、走估算）。 */
    uint32_t RequestedBitrateMbps() const { return requestedBitrateMbps_; }

    bool Running() const { return status_.running; }

    /** 状态快照。 */
    MfEncoderStatus Status() const { return status_; }

    /** 本轮分片的落地目录。 */
    const std::string& Dir() const { return dir_; }

    /** 自维护的清单文件（index.csv），导出端读它拿顺序。 */
    std::string ListFilePath() const;

private:
    /**
     * 把音频队列里的 PCM 编码成 AAC，并按墙钟缓存带时间戳的 AAC 样本。
     *
     * 只在喂帧线程里调用（见 Write 的线程模型）。AAC 编码器、输出样本缓存都
     * 只被这一条线程碰，无需加锁；只有从队列取 PCM 那一下需要锁。
     */
    void PumpAudio();
    /**
     * 建常驻 AAC 编码器并协商类型。返回 false = 建不起来（没有声音，录制照常）。
     * 在 Start() 里、第一个分片开出来之前调一次；OpenSegment() 里逐片重试。
     */
    bool InitAudioEncoder();
    /** 销毁常驻 AAC 编码器 + 排空缓存。在 Finish() 里调。 */
    void CloseAudioEncoder();

    /**
     * 音频时间轴：以「采样帧号」为唯一时钟，时间戳全部由它算出来。
     *
     * 【为什么不能用毫秒累加】原来是 `wallMs += consumed * 1000 / 48000`，
     * 每个 1024 帧块 21.333ms 被截断成 21ms —— 每秒凭空少 333µs，采样的
     * 时间戳步长（21ms）就小于它的时长（21.333ms），**相邻 AAC 样本在 MP4 里
     * 互相重叠**。录制端还看不出来，但导出端一直拷这些样本时会得到自相矛盾的
     * stts，解码出来就是满屏"滋滋"的噪声（用户实测）。
     *
     * 现在：时间轴 = 起点墙钟ticks + 采样帧号 × 1e7 / 48000，逐块首尾相接、
     * 零重叠零空洞（跨块误差锁在 1 tick 内，40 分钟不漂）。
     */
    int64_t TsOfOffset(int64_t frames) const {
        return audioStartTicks_ + frames * 10000000ll / 48000ll;
    }
    /** 时间轴已经写到哪一刻（ticks，绝对墙钟）。还没开始返回 0。 */
    int64_t AudioEndTicks() const {
        return audioStartTicks_ > 0 ? TsOfOffset(audioBufferStart_ + BufferedFrames()) : 0;
    }
    /** 待编码 PCM 缓冲里有多少采样帧。 */
    int64_t BufferedFrames() const { return static_cast<int64_t>(aacPcmBuf_.size() / 2); }
    /** 往待编码 PCM 缓冲追加静音，让时间轴前进 ticks。 */
    void AppendSilenceTicks(int64_t ticks);
    /** 把时间轴补静音一直推进到 endTicks（tick 精度，不再取整到毫秒）。 */
    void FillSilenceToTicks(int64_t endTicks);
    /** 缓存里有没有落在 [startTicks, endTicks) 的 AAC 样本。 */
    bool HasAudioInRange(int64_t startTicks, int64_t endTicks) const;

    /** 把缓存里属于 [startTicks, endTicks) 墙钟范围的 AAC 样本直拷进当前分片的音频流。 */
    void FlushAudioToSegment(int64_t startTicks, int64_t endTicks);

    /** 一个已编码的 AAC 样本（一份独立内存拷贝 + 墙钟 + 时长，单位 100ns）。 */
    struct AudioSample {
        std::vector<uint8_t> data;
        int64_t wallTicks = 0;   // 样本起始墙钟（100ns）
        int64_t durTicks = 0;    // 样本时长（100ns）
    };

    // ---- 常驻 AAC 编码器 ---------------------------------------------------
    void* aacEncoder_ = nullptr;        // IMFTransform*（CLSID_AACMFTEncoder）
    uint32_t aacOutBufSize_ = 0;        // GetOutputStreamInfo 拿到的 cbSize
    std::vector<uint8_t> asc_;          // AudioSpecificConfig（MF_MT_USER_DATA）
    // 已编码、还没写进分片的 AAC 样本（按墙钟递增）。只在喂帧线程碰。
    std::vector<AudioSample> aacPending_;
    bool aacEncoderStarted_ = false;    // 是否已发 BEGIN_STREAMING 消息序列
    // 已喂给编码器的每个 1024 帧块的时间戳（100ns）。输出样本按顺序和它们
    // 一一对应 —— 不信任编码器回填的 sts（有的 MFT 会给 0 或按自己的时基）。
    std::deque<int64_t> aacInTicks_;
    int64_t aacLastOutTicks_ = 0;       // 兜底：队列空时用它顺延
    // 音频采集线程投进来的原始 PCM（喂帧线程消费）。格式：S16 立体声交错。
    struct PcmBlock {
        std::vector<int16_t> data;
        int64_t startWallMs = 0;
    };
    std::mutex pcmMu_;
    std::deque<PcmBlock> pcmQueue_;
    // 音频时间轴（单位 100ns / 采样帧）：见 TsOfOffset 的注释。
    //
    //   audioStartTicks_   时间轴 0 号采样对应的墙钟（0 = 还没开始采到声音）
    //   audioBufferStart_  待编码缓冲头部在时间轴上的采样帧号
    //   aacPcmBuf_         还没编码的 PCM（尾部 = 时间轴末端）
    // 时间轴末端 = audioBufferStart_ + BufferedFrames()，所以"缓冲开头"永远是
    // 精确的，编码消耗 / 溢出丢弃只要推进 audioBufferStart_ 就行。
    int64_t audioStartTicks_ = 0;
    int64_t audioBufferStart_ = 0;
    std::vector<int16_t> aacPcmBuf_;

    /** 开一个新分片（轮转）。返回 false 表示开不出来。 */
    bool OpenSegment();

    /**
     * 把输入类型（NV12 @ 当前 fps_）交给 SinkWriter。
     *
     * 只做一件事，不降档：降档需要重开 SinkWriter（输出流上还挂着旧的帧率
     * 属性），所以那件事在 StartWithFpsFallback 里做。
     */
    bool NegotiateInputType(void* writer, uint32_t streamIndex, void* encAttrs);

    /**
     * 起录：按帧率阶梯逐档试 OpenSegment，直到硬件编码器接受为止。
     *
     * 为什么必须整段重来：`SetInputMediaType` 失败之后，输出流上的
     * `MF_MT_FRAME_RATE` 还是旧的，只换输入类型会让输出流的时间基对不上。
     * 所以每一档都重新 MFCreateSinkWriterFromURL + AddStream + 交输入类型。
     */
    bool StartWithFpsFallback();
    /** 收掉当前分片：Finalize 它，把行追加进 index.csv。 */
    void CloseSegment();
    /** 把 index.csv 整份重写一遍（内容取自 done_）。 */
    void RewriteIndex();
    /** 建/重建这一片复用的样本缓冲；尺寸没变就什么都不做。 */
    bool EnsureSample();
    /** 扔掉复用的样本（切分片、收尾、尺寸变化时）。 */
    void ReleaseSample();

    /**
     * 当前墙钟 Unix 秒。
     *
     * 每个分片开片时记一次，导出端据此判断它属不属于回放窗口、以及相邻两片
     * 之间断了多久（好补 NO SIGNAL）。必须是墙钟而不是单调时钟 —— 要能跨
     * 进程重启和昨天的分片比较。
     */
    static int64_t NowUnixMs();

    /**
     * 按「用户配的码率」或「像素数 × 0.15 bit × 帧率」重算 bitrateBps_。
     *
     * 三种场景都调它：开片前、改帧率后、换尺寸后。区别只在 requestedBitrateMbps_
     * 是否为 0 —— 不为 0 就照用户配的走（帧率翻倍也不动），为 0 才按尺寸估算。
     *
     * 【以前是两段复制粘贴的估算公式】SetFps 与 ResizeTo 各写了一遍，都是
     * 无条件估算，把用户显式配的码率冲掉了。收敛成一个函数，口径不会再分叉。
     */
    void RefreshBitrate();

    std::string dir_;
    uint32_t    width_ = 0;
    uint32_t    height_ = 0;
    uint32_t    fps_ = 60;            // 编码器**实际**跑的帧率（可能被降过档）
    uint32_t    requestedFps_ = 60;   // 用户/配置要的帧率，只用于状态说明
    uint32_t    bitrateBps_ = 0;
    /**
     * 用户在配置里填的码率（Mbps，0 = 没配）。
     *
     * 【为什么必须单独记住】以前只有 bitrateBps_，而它是「按当时的尺寸/帧率
     * 估出来的结果」，分不清「用户配的 5 Mbps」和「估出来恰好 5 Mbps」。
     * SetFps()/ResizeTo() 于是无条件用像素公式把它覆盖掉 —— 用户配 5 Mbps，
     * 一改帧率就变成估算值（1440p@144 估算 79 Mbps），文件暴涨而用户毫无感知。
     *
     * 记下原值之后：配了就一直按配的走（只夹到 [2,200]），没配才按公式估。
     */
    uint32_t    requestedBitrateMbps_ = 0;
    uint32_t    segmentSeconds_ = 2;
    /**
     * 回放窗口的分片预算 —— **可以在录制中改**（SetKeepSegments）。
     *
     * 用 atomic 而不是普通成员：改它的线程是会话线程（reload），读它的是
     * 喂帧线程（PurgeExpired/PickSlot）。两者同时发生完全可能，而这里保护
     * 的只是一个"保留多少"的预算，不需要更强的同步。
     */
    std::atomic<uint32_t> keepSegments_{2};
    uint32_t    segmentFrames_ = 0;   // 每片多少帧（= fps * segmentSeconds）

    /**
     * 开片时画布给多大余量（"过渡期"才用）。
     *
     * 【历史：这条路上试过"画布放大 2 倍"来省开片】用户要求「不能缩放」
     * （原话：不能缩放，不能每变化一次尺寸就立刻切出来一片吗，编码需要时间
     * 你就用多线程啊）。缩放这条路因此彻底封死，只能靠"变大就补黑边 / 变小
     * 就切新片"来适配。而"变大就切"撞上一条硬事实：每次 Finalize + 重开
     * SinkWriter 要 **385ms**（全部花在 SetInputMediaType 的硬件 MFT 初始化，
     * 与分辨率无关）。拖窗口时 Windows 逐像素连续改尺寸，每帧一个新尺寸 = 每
     * 帧付 385ms，一次拖拽 40 次就 15 秒，喂帧线程彻底堵死。多线程也救不了
     * （385ms 是初始化不是排队，8 worker 也只有 21 片/秒）。
     *
     * 【为什么最终关掉】过渡画布把分片文件的帧尺寸变成真实画面的 2 倍
     * （窗口 1499x822 → 文件 4096x2912），而导出端按「20 秒内最大尺寸」定
     * 画布，一旦某个分片的 index.csv 行读不到（槽位复写会把旧行整份重写掉，
     * 文件却还在盘上），contentW 只能退回文件帧尺寸 4096x2912 → 用户导出比
     * 显示器还大的视频、画面缩在左上角一小块。更糟的是 4096x2912 超出 NVENC
     * 的 60fps 吞吐，WriteSample 背压把喂帧线程拖死 —— 预览（走帧环）正常、
     * **录制冻结**：分片 duration 停在旧时刻、内容全部定格，正是 m12438/
     * m12447 报的「录像一直定格中」。
     *
     * 用户的原话把取舍讲清楚：「原来那样就是改变一下窗口大小就立刻切一次片
     * 啊，然后按照 20 秒内最大的视频尺寸作为最终的视频尺寸，刚刚这样挺好的
     * 啊」。所以 1:1 画布 + 宁可多切几片，才是对的。剩下的重复开片由
     * kSizeStableMs 兜底（见 Session/MfEncoder 相关注释）。
     */
    static constexpr uint32_t kTransitionFactorNum = 1;  // 1:1，画布 = 帧尺寸
    static constexpr uint32_t kTransitionFactorDen = 1;

    /**
     * 过渡画布的边长上限。
     *
     * 【历史遗留】kTransitionFactor 已经关成 1:1（见上面的注释），这个上限
     * 实际不会再被触发。保留常量只为让旧引用编译通过、且万一以后要恢复
     * 过渡余量时有个护栏。
     */
    static constexpr uint32_t kTransitionMaxDim = 4096;

    /**
     * 距上次切片多久之内算"尺寸正在抖"。
     *
     * 用墙钟而不是帧数：喂帧线程被硬件编码器堵住时帧数会落后于墙钟，用帧数
     * 判断会让"过渡期"在卡顿期间提前结束。取 2 秒 = 一个标称片长，正好覆盖
     * "用户还在拖"的整个时段。
     */
    static constexpr int64_t kTransitionWindowMs = 2000;

    /**
     * 实际槽位数。
     *
     * 【为什么不能只按片数】见 DESIGN.md 9.9。片不是等长的：ResizeTo 每见一次
     * 尺寸变化就切一片，拖一次窗口能切出好几片零点几秒的过渡片。槽位只按
     * keepSegments_（20s/2s = 10）算时，10 个槽位被过渡片占满，环里只剩一两秒
     * 真实内容 —— 导出于是几乎全程 NO SIGNAL。
     *
     * 所以按「标称片数的 8 倍」给，够 20 秒窗口里平均每片 0.25 秒还有余量；
     * 真超了也不丢新内容，PurgeExpired 按时间从最旧的开始扔。
     */
    uint32_t keepSlots_ = 2;
    static constexpr uint32_t kMaxSlots = 512;

    /** 回放窗口的毫秒数（= keepSegments_ × 标称片长）。 */
    int64_t WindowMs() const {
        return static_cast<int64_t>(keepSegments_.load(std::memory_order_relaxed)) *
               static_cast<int64_t>(segmentSeconds_) * 1000;
    }

    /** 按时间清掉已经跌出回放窗口的分片（连文件一起），并重写 index.csv。 */
    void PurgeExpired(int64_t nowMs);

    /** 给新分片挑一个槽位：优先没被活分片占用的槽，全占满就复用最旧的那片。 */
    uint32_t PickSlot();

    // SinkWriter 用 void* 顶着，头文件不把 mfreadwrite.h 拖给所有人。
    // 实际类型是 IMFSinkWriter*。
    void* writer_ = nullptr;
    uint32_t streamIndex_ = 0;
    // 当前分片 SinkWriter 里的音频流下标（AAC passthrough）。0xFFFFFFFF = 没加
    // （AAC 编码器没起来，这一片只有视频）。
    uint32_t audioStreamIndex_ = 0xFFFFFFFFu;

    /**
     * 样本池（每片一组，开新片时重建）。
     *
     * 【为什么要池化】原来每写一帧都 MFCreateMemoryBuffer(14.7MB) +
     * MFCreateSample，2560x1440@144 时等于每秒分配 2.1GB 内存、再整块
     * memcpy 一遍。实测进程峰值工作集 854MB、CPU 里有一大截耗在堆分配
     * 和页错误上（主人报的「内存 800M」就是它）。
     *
     * 【为什么是"池"而不是"一个"】—— 这是本类最隐蔽的一个坑，别改回单例。
     *
     * 曾经只复用**一个** IMFSample：每帧把 NV12 写进同一个缓冲、改时间戳、
     * WriteSample。低帧率下完全没问题（60fps 实测 240/240 帧全落盘），
     * 但帧率上去以后 SinkWriter 还没来得及把上一帧下发完，我们就把下一帧
     * 写进**同一块内存**了 —— 结果 WriteSample 照样返回 S_OK，帧却被**静默
     * 丢弃**。探针 native/probe_sw_drop.cpp 的对照数据（2560x1440，4 秒）：
     *
     *     165fps 复用单样本   提交 660 → 落盘 207 帧（31%）
     *     165fps 每帧新建     提交 660 → 落盘 660 帧（100%）
     *     144fps 复用单样本   提交 576 → 落盘 130 帧（23%）
     *     144fps 每帧新建     提交 576 → 落盘 576 帧（100%）
     *      60fps 复用单样本   提交 240 → 落盘 240 帧（100%）
     *
     * 落盘帧率不足的后果就是主人报的「240fps 录出来卡成 PPT」：帧数少、
     * PTS 有空洞（avg_frame_rate 变成 51810/329 这种怪分数）、画面几秒才动一下。
     *
     * 修法是轮转一个够大的池：只要池深 > 编码器在途帧数，就不会踩到还没
     * 消费完的那一份。每片分配一次，代价可以接受（1440p 一帧 NV12 约 5.5MB）。
     *
     * 【池深为什么是 16 而不是 8】实测 8 份时 165fps 已经 100%，但 144fps 仍
     * 只有 96%（每片少 8~17 帧，文件里能直接看到 PTS 空洞）。编码器在途帧数
     * 随「片刚开、首帧还没出编码队列」那一刻最高，16 份之后 90/120/144/165
     * 四档全部 100%。代价是 1440p 一片 88MB —— 与录制端 200MB 级工作集同量级，
     * 可接受（切片时才重建）。
     *
     * 类型用 void*：头文件不把 mfapi.h / mfidl.h 拖给所有包含者。
     * 实际类型是 IMFSample*，它持有的 IMFMediaBuffer 可以 Lock 后直接写。
     */
    std::vector<void*> samplePool_;
    /** 池里每一份样本各自 Lock 出来的常驻写指针（与 samplePool_ 一一对应）。 */
    std::vector<uint8_t*> samplePtrs_;
    /** 下一帧用池里的哪一份（取模轮转）。 */
    uint32_t sampleNext_ = 0;
    /**
     * 池里最近一次**装满真实画面**的那一份的下标（补帧从这里复制内容）。
     *
     * 补帧（`Write(nullptr)`）不能再"改写同一份样本" —— 那正是让 SinkWriter
     * 静默丢帧的原因（见上面 samplePool_ 的注释）。所以补帧也走池轮转，
     * 内容从这一份 memcpy 过去：一次连续拷贝约 0.5ms（1440p NV12 5.5MB），
     * 比重新跑一遍 ConvertToNv12 便宜得多。
     */
    uint32_t sampleLastFilled_ = 0;
    /** 样本缓冲的行距（Y 平面 = width_）。 */
    uint32_t samplePitch_ = 0;

    /**
     * 样本缓冲里现在装的是不是一帧**已经写进过编码器**的画面。
     *
     * 补帧（`Write(nullptr)`）要直接复用这份内容，但刚开片、刚换尺寸、
     * 刚切分片之后样本是空的（`ReleaseSample` 把 sample_ 扔了，`EnsureSample`
     * 重新锁出来的是全零缓冲）—— 这时候补帧没有内容可复用，只能空过这一拍。
     *
     * 注意「写进过编码器」这个限定没写在名字里但很重要：`EnsureSample` 成功
     * 只代表有缓冲可用，`sampleHasFrame_` 才代表里面是真实画面。
     * `ReleaseSample()` 里必须置 false，否则换尺寸后补帧会把上一片的旧画面
     * 贴到新片里（尺寸都不一样，画面必然错乱）。
     */
    bool sampleHasFrame_ = false;

    /**
     * 自己把 BGRA 转成 NV12 再喂编码器 —— **这是本轮最大的性能修复**。
     *
     * 【为什么必须自己转】原来直接把 RGB32(BGRA) 喂给 SinkWriter，指望它内部
     * 完成 BGRA→NV12。实测（native/probe_opt.cpp，2560x1440@144 真实桌面帧、
     * NVENC、实时节奏、交替 3 轮取中位数）：
     *
     *     bgra 直喂（旧实现）   3.79 / 3.97 / 6.82 核
     *     nv12 自转            0.80 / 0.86 / 0.88 核   ← 稳得像块石头
     *
     * 也就是说 SinkWriter 在没有 D3D manager 时只能用 msvproc.dll 在 CPU 上
     * 做色彩转换，那一层不但慢，而且抖动极大（同一份数据 3.79~6.82 核）。
     * 自己用 SSE2 转一遍只要 0.8 核，还稳。
     *
     * 【顺带修好了颜色】同一份测试图（8 色带 + 灰阶）解码回来逐点比：
     *     bgra 路径  黄=21,255,250   绿=18,255,5    红=2,0,243
     *     nv12 路径  黄=0,254,254    绿=0,253,0     红=0,0,252   ← 正确
     * msvproc 那条路把纯色都算偏了（黄里掺绿、绿里掺红），自转的准。
     * 灰阶两端都对得上（10→242），说明是 BT.601 有限范围，两边一致。
     *
     * 【代价】每帧多一次全画面读 + 半画面写。实测就是上面那个 0.8 核里的一
     * 部分；相比省下来的 3 核完全值得。
     */
    void ConvertToNv12(const uint8_t* src, uint32_t stride, uint32_t width, uint32_t height,
                       uint8_t* dst);

    uint32_t segmentSeq_ = 0;         // 已经开了第几个分片（全局序号，不取模）
    uint32_t slotInUse_ = 0;          // 当前分片占的槽位号（0..keepSlots_-1）
    uint32_t framesInSegment_ = 0;    // 当前分片里已写多少帧
    // 当前分片里真实画面的最大尺寸（不算过渡画布补出来的黑边）。
    uint32_t segContentW_ = 0;
    uint32_t segContentH_ = 0;
    std::string currentPath_;         // 当前分片文件
    int64_t  segStartUnix_ = 0;       // 当前分片第一帧对应的墙钟（Unix 毫秒，已按帧数顺延）
    /**
     * 上一片收尾时算出来的「下一片第一帧应该在什么墙钟」。
     *
     * 【为什么不能直接用 NowUnixMs()】见 DESIGN.md 9.3.1。原来 OpenSegment()
     * 一进来就把 `segStartUnix_ = NowUnixMs()`，可是**开片的这一刻可能已经
     * 被编码拖晚了一段**：喂帧线程在 CloseSegment() 里等 Finalize 排空
     * （1440p@144 实测每片要几十毫秒到几百毫秒，两路并发时更久），等它
     * 真正去开新片时墙钟已经跑过去了。而新片的第一帧紧接着上一片最后一帧，
     * 内容上是**连续的**。两边一错位，index.csv 里就凭空多出一段空洞：
     *
     *   实测（主人机器，2560x1440@144，两路并发，segmentSeconds=2）：
     *   每片内容都是 2.000 秒，但相邻两片的墙钟起点差 3.11~3.40 秒，
     *   即**每片之间凭空多出 1.1~1.4 秒空洞**。导出端把这些空洞当成
     *   「没录到」，一律补成 NO SIGNAL —— 主人报的「调整窗口大小时显示器
     *   捕获出现 no signal」就是这么来的（拖动资源管理器会让 p-win 那一路
     *   反复重建 NVENC，把同一进程里 p-desk 的编码也拖慢）。
     *
     * 现在：收片时用「起点 + 帧数/帧率」算出这一片的真实结尾，存进
     * nextSegStartUnix_，下一片就用它当起点 —— 时间轴严格首尾相接，
     * 编码慢只影响延迟，不再凭空制造空洞。
     *
     * 0 = 还没有上一片（第一片只能用 NowUnixMs()）。
     */
    int64_t  nextSegStartUnix_ = 0;
    int64_t  lastResizeMs_ = 0;       // 最近一次因为尺寸变化切成新片的墙钟（0 = 还没切过）
    uint32_t pendingW_ = 0;           // 上一次 Write 看到的原始尺寸，用来判"尺寸稳定了吗"
    uint32_t pendingH_ = 0;
    int64_t  pendingSinceMs_ = 0;     // 这个原始尺寸是从什么时候开始没变过的

    /**
     * 分辨率变了：把当前片收尾，按新尺寸接着录（**不**重开轮次目录）。
     *
     * 【为什么不能"录制中不允许改分辨率"】见 DESIGN.md 9.5。原来 Write() 遇到
     * 尺寸不符直接报错，上层只好 Stop + 新建 SegmentRecorder，于是分配到一个
     * 新的轮次目录、切片从 000000 重来 —— 用户看到的就是「调一下窗口大小，
     * 回放里前面的画面全没了」。
     *
     * 现在改成同一路录制器跨尺寸存活：收掉当前片（它已经完整可读），用新尺寸
     * 开下一片，切片序号继续递增。混合尺寸的片子交给导出端按「最大尺寸画布 +
     * 左上角对齐补黑边」拼。
     */
    bool ResizeTo(uint32_t newW, uint32_t newH);

    /**
     * 在尺寸抖动期间，把这一帧适配进当前片的画布 —— 不切片、不重开编码器。
     *
     * 两种情形：
     *   帧比画布小（窗口被拖小）→ 1:1 贴左上角，右侧/下方补黑；
     *   帧比画布大（窗口被拖大）→ 1:1 贴左上角，**超出画布的部分裁掉**。
     *
     * 【为什么允许裁掉右边】画布在过渡期是按新尺寸放大过的（见
     * kTransitionFactor），所以正常的拖拽根本到不了"装不下"这一步；真到了，
     * 说明用户一次把窗口拉大了一倍以上，那一步的右侧内容会缺 —— 但代价只
     * 落在过渡的几百毫秒里，尺寸一稳定立刻切精确尺寸的片。
     *
     * 【为什么绝不缩放】主人明确要求（m08329：「不能缩放」）。而且缩放会让
     * 导出端"按窗口内最大尺寸作画布"的语义变得含混 —— 分片里存的到底是不是
     * 1:1 的像素就没法保证了。
     *
     * 【为什么不能改成片内 SetInputMediaType】
     * 探针实测：片内跨尺寸换输入类型虽然返回 S_OK，却**完全不生效** ——
     * 编码器照旧按老尺寸编码新分辨率的数据，后写的 30 帧被静默丢弃
     * （探针 B：成品只有前 30 帧 640x480）。
     */
    bool WritePadded(const void* pixels, uint32_t stride, uint32_t width, uint32_t height);

    /** 尺寸抖动专用的一帧画布（BGRA，尺寸 = width_ × height_）。 */
    std::vector<uint8_t> padBuf_;

    /**
     * WritePadded 补完黑边回头调 Write 时置位。
     *
     * 那次调用传的尺寸是**画布**尺寸，不是真实画面尺寸 —— Write 里给
     * segContentW_/segContentH_ 记账时必须跳过它，否则「真实画面尺寸」
     * 会被补出来的黑边污染，导出端又按画布定画布（用户的画面缩成小方块）。
     */
    bool padRecurse_ = false;

    // 已完成分片的时间顺序表（文件名 + 该片帧数 + 该片起始秒 + 该片墙钟时刻）。
    //
    // 【为什么必须记在内存里，不能靠扫目录排序】
    // 槽位是循环复写的：第 61 片落在 0 号槽位，文件名又是 000000.mp4。
    // 按文件名排序会把它排到最前面，时间顺序就完全错了。文件 mtime 也不可靠
    // （复写时 mtime 会被刷新，但顺序信息和精度都不足）。
    // 所以每收一片就把 (文件名, 帧数, 起始秒) 记进这张表，写 index.csv 时
    // 直接照抄 —— 这是唯一准确的时间来源。
    //
    // 【startUnix 是必须的，不是可选装饰】见 DESIGN.md 9.3。
    // 文件名和时间轴都是相对的：文件不写、mtime 不变，昨天的分片今天还躺在
    // 目录里，按"最后几个文件"取就把它当成了最近 20 秒的一部分。只有把每片
    // 开始的**墙钟 Unix 秒**记下来，导出端才能判断"这片属不属于回放窗口"，
    // 也才能知道中间断了几秒（好补 NO SIGNAL）。
    struct DoneSegment {
        std::string name;
        uint32_t    frames = 0;
        double      startSec = 0.0;
        int64_t     startUnixMs = 0;   // 该片开始写时的墙钟（Unix 毫秒）
        uint32_t    slot = 0;          // 它占着哪个槽位（0..keepSlots_-1）
        // 这一片里真实画面（不含贴图路径补的右边/下边黑边）的最大尺寸。
        //
        // 【为什么必须记】WritePadded 路径（窗口拖小）画布比画面大、右下是黑，
        // 文件帧尺寸不等于真实画面尺寸。导出端如果按文件帧尺寸定画布，就会
        // 被黑边撑大，用户的画面缩成左上角一个小方块。所以这里单独记"真正的
        // 画面有多大"，导出端据此裁剪 + 定画布。0 表示不知道（老数据），导出
        // 端会退回文件帧尺寸。
        uint32_t    contentW = 0;
        uint32_t    contentH = 0;
        // 这一片里**最多有多少帧**（"这一片的内容有多长"的分子）。
        //
        // 【为什么不能只记 frames】分片是按 fps_ × segmentSeconds_ 切出来的，
        // 但 fps_ 可以被就地改（见 SetFps）。改帧率前那一片有 144×2=288 帧、
        // 改完那一片只有 60×2=120 帧，如果只记帧数、再用"当前 fps_"去换算
        // 时长，前一片会被算成 288/60=4.8 秒（实测就是 4.80，时间轴直接错乱）。
        // 所以帧数必须连它的"分母"一起记下来。
        uint32_t    fps = 0;           // 这一片录制时的编码器帧率
        uint32_t    segmentFrames = 0; // 这一片录制时的 segmentFrames_
    };
    std::vector<DoneSegment> done_;
    double doneSeconds_ = 0.0;        // 已完成分片累计时长（秒）

    /**
     * 一片的真实时长（毫秒）。
     *
     * 【为什么不能用"当前 fps_"换算】帧率可以被就地改（见 SetFps）：改之前的
     * 片有 144×2=288 帧、改之后的片只有 60×2=120 帧。拿当前 fps_ 去除所有
     * 片的帧数，前面的片会被算成 288/60=4.8 秒 —— 实测就是这个数字，时间轴
     * 当场错乱。所以每片自带 fps 与 segmentFrames，时长只按它自己的算。
     */
    static int64_t SegmentDurationMs(const DoneSegment& d);

    MfEncoderStatus status_;

    /**
     * 这个对象是否持有进程级 MF 引用（Start 成功时置位，Finish 时释放）。
     *
     * 按**对象**而不是按线程记账：Start 在会话线程、Write 在喂帧线程、
     * Finish 在停止线程，三者通常不是同一个线程，按线程记会对不上。
     */
    bool retained_ = false;
};

} // namespace shadow
