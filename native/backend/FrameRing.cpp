// FrameRing.cpp — 生产者侧帧环实现
#include "FrameRing.h"

#include <windows.h>

#include <cstring>
#include <thread>

namespace shadow {

namespace {

// ShadowRingHeader / slotStride 都是 64 位对齐的，直接按指针读写即可。
// 这里全部使用 Interlocked* 系列，保证跨进程可见性与顺序。

inline void StoreU64(volatile uint64_t* p, uint64_t v) {
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(p),
                          static_cast<LONG64>(v));
}
inline void StoreI64(volatile int64_t* p, int64_t v) {
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(p),
                          static_cast<LONG64>(v));
}
inline uint64_t LoadU64(volatile uint64_t* p) {
    return static_cast<uint64_t>(InterlockedCompareExchange64(
        reinterpret_cast<volatile LONG64*>(p), 0, 0));
}
inline void StoreU32(volatile uint32_t* p, uint32_t v) {
    InterlockedExchange(reinterpret_cast<volatile LONG*>(p), static_cast<LONG>(v));
}
inline uint32_t LoadU32(volatile uint32_t* p) {
    return static_cast<uint32_t>(InterlockedCompareExchange(
        reinterpret_cast<volatile LONG*>(p), 0, 0));
}
// 不参与同步的统计字段，直接读写即可
inline uint64_t PlainInc(volatile uint64_t* p) {
    return static_cast<uint64_t>(InterlockedIncrement64(
        reinterpret_cast<volatile LONG64*>(p)));
}

inline uint64_t Align64(uint64_t v) {
    return (v + SHADOW_SLOT_ALIGN - 1) & ~static_cast<uint64_t>(SHADOW_SLOT_ALIGN - 1);
}

} // namespace

FrameRing::~FrameRing() {
    CloseAll();
}

void FrameRing::CloseAll() {
    if (readyEvent_)      { CloseHandle(readyEvent_);      readyEvent_ = nullptr; }
    if (frameEvent_)      { CloseHandle(frameEvent_);      frameEvent_ = nullptr; }
    if (disconnectEvent_) { CloseHandle(disconnectEvent_); disconnectEvent_ = nullptr; }
    if (base_)            { UnmapViewOfFile(base_);        base_ = nullptr; }
    if (mapping_)         { CloseHandle(mapping_);         mapping_ = nullptr; }
    header_ = nullptr;
}

bool FrameRing::CreateMapping(const std::string& name, uint64_t totalBytes) {
    // 安全描述符传 nullptr：内核会给对象一个“创建者 + 登录 SID 完全控制”的默认 DACL。
    // 生产者和消费者由同一个用户、同一个登录会话启动，因此恰好够用，
    // 也避免了在受限令牌下因为显式 DACL 写错而打不开对象。
    LARGE_INTEGER size{};
    size.QuadPart = static_cast<LONGLONG>(totalBytes);

    mapping_ = CreateFileMappingA(
        INVALID_HANDLE_VALUE, // 由系统分页文件支撑，不落盘
        nullptr,
        PAGE_READWRITE,
        static_cast<DWORD>(size.HighPart),
        static_cast<DWORD>(size.LowPart),
        name.c_str());

    if (!mapping_) {
        lastError_ = "CreateFileMapping failed: " + std::to_string(GetLastError());
        return false;
    }
    return true;
}

bool FrameRing::EnsureMap() {
    if (base_) return true;
    void* view = MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!view) {
        lastError_ = "MapViewOfFile failed: " + std::to_string(GetLastError());
        return false;
    }
    base_ = static_cast<uint8_t*>(view);
    header_ = reinterpret_cast<ShadowRingHeader*>(base_);
    return true;
}

bool FrameRing::Open(const std::string& sessionId, uint32_t maxWidth, uint32_t maxHeight,
                     uint32_t slotCount) {
    CloseAll();

    if (slotCount == 0 || slotCount > SHADOW_SLOT_COUNT) slotCount = SHADOW_SLOT_COUNT;
    if (slotCount < 2) slotCount = 2;
    slotCount_ = slotCount;
    session_ = sessionId;

    // 注意：槽位偏移是由 header->headerSize + header->slotStride 决定的，
    // 所以槽容量在共享内存创建的那一刻就固定了，之后不能改。
    // 因此这里按采集到的分辨率加一点余量来预留；分辨率变大时由调用方
    // 通过 NeedsGrow() 判断后重建整个环。

    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    qpcFrequency_ = freq.QuadPart ? freq.QuadPart : 1;

    // ---- 1) 命名事件 ------------------------------------------------------
    // CreateEvent 对同名的已存在对象会直接返回它的句柄（不会重建），
    // 所以消费者先启动或后启动都能拿到同一个事件。
    //   自动重置事件：生产者 SetEvent，消费者 Wait 返回后自动复位。
    const std::string readyName = ReadyEventName(sessionId);
    const std::string frameName = FrameEventName(sessionId);
    const std::string discName = DisconnectEventName(sessionId);

    readyEvent_ = CreateEventA(nullptr, FALSE, FALSE, readyName.c_str());
    frameEvent_ = CreateEventA(nullptr, FALSE, FALSE, frameName.c_str());
    disconnectEvent_ = CreateEventA(nullptr, FALSE, FALSE, discName.c_str());

    if (!readyEvent_ || !frameEvent_ || !disconnectEvent_) {
        lastError_ = "CreateEvent failed: " + std::to_string(GetLastError());
        CloseAll();
        return false;
    }

    // ---- 2) 共享内存 ------------------------------------------------------
    // 常驻服务可能被重复启动：如果同名映射已经存在且头部有效，就直接复用，
    // 而不是把正在运行的实例的帧环踩掉。
    HANDLE existing = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, RingName(sessionId).c_str());
    if (existing) {
        void* view = MapViewOfFile(existing, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        if (view) {
            ShadowRingHeader* h = static_cast<ShadowRingHeader*>(view);
            if (h->magic == SHADOW_RING_MAGIC && h->version == SHADOW_RING_VERSION) {
                // 复用现有的环
                if (mapping_) CloseHandle(mapping_);
                mapping_ = existing;
                base_ = static_cast<uint8_t*>(view);
                header_ = h;
                slotStride_ = static_cast<uint64_t>(h->slotStride);
                slotCount_ = h->slotCount;
                capWidth_ = maxWidth;
                capHeight_ = maxHeight;
                attached_ = true;
                SetEvent(readyEvent_);
                return true;
            }
            UnmapViewOfFile(view);
        }
        CloseHandle(existing);
    }

    // 共享内存容量 = 全局头 + slotCount 个槽。
    //
    // 每个槽的布局是 [槽头 sizeof(ShadowRingHeader)][像素 maxPixels]，
    // 所以槽间距（stride）必须把槽头也算进去：
    //     slotStride = sizeof(ShadowRingHeader) + Align64(pixels)
    //     total      = sizeof(ShadowRingHeader) + slotStride * slotCount
    //
    // 这里踩过一次坑：一开始把 slotStride 当成"纯像素字节数"，
    // 结果最后一个槽的像素区正好越过映射末尾 424 字节（一个槽头的大小），
    // 于是 slotCount-1 号槽的像素永远读不到，表现成 Uint8Array 越界。
    capWidth_ = maxWidth > 0 ? maxWidth : 1920;
    capHeight_ = maxHeight > 0 ? maxHeight : 1080;
    const uint64_t pixelsPerSlot =
        static_cast<uint64_t>(capWidth_) * 4ull * static_cast<uint64_t>(capHeight_);
    const uint64_t slotStride = static_cast<uint64_t>(sizeof(ShadowRingHeader)) +
                                Align64(pixelsPerSlot);
    const uint64_t total = static_cast<uint64_t>(sizeof(ShadowRingHeader)) +
                           slotStride * slotCount_;

    if (!CreateMapping(RingName(sessionId), total)) {
        CloseAll();
        return false;
    }
    if (!EnsureMap()) {
        CloseAll();
        return false;
    }

    std::memset(base_, 0, static_cast<size_t>(sizeof(ShadowRingHeader)));
    slotStride_ = slotStride;
    mapBytes_ = total;

    header_->headerSize = static_cast<uint32_t>(sizeof(ShadowRingHeader));
    header_->slotCount = slotCount_;
    header_->slotStride = static_cast<int64_t>(slotStride_);
    header_->producerPid = GetCurrentProcessId();
    header_->consumerPid = 0;
    header_->qpcFrequency = qpcFrequency_;
    header_->format = 0; // BGRA8, top-down
    for (uint32_t i = 0; i < SHADOW_SLOT_COUNT; ++i) {
        header_->slotOwners[i] = -1;
        header_->slotOwnQpc[i] = 0;
    }
    header_->width = capWidth_;
    header_->height = capHeight_;
    header_->stride = capWidth_ * 4u;
    header_->version = SHADOW_RING_VERSION;
    header_->magic = SHADOW_RING_MAGIC; // 最后写 magic，消费者据此判断初始化完成

    SetEvent(readyEvent_);
    return true;
}

void FrameRing::SetMode(uint32_t width, uint32_t height, uint32_t format) {
    if (!header_) return;
    // 头部字段消费者在读帧时并不使用（尺寸随帧一起从帧头读取），
    // 这里只做展示用途。
    StoreU32(&header_->width, width);
    StoreU32(&header_->height, height);
    StoreU32(&header_->stride, width * 4u);
    StoreU32(&header_->format, format);

    // modeChangeCount 只在**尺寸真的变了**的时候才加。
    //
    // 这个函数原来每拍都无条件 +1。而拖动窗口时 WGC 每帧报一个不同的
    // ContentSize，Session 就会每拍调一次 SetMode（哪怕只是 1 像素的抖动），
    // 于是这个计数器在拖拽期间疯涨。消费者拿它当"布局变了、要重建纹理"
    // 的信号，就变成了每帧重建一次 GL 纹理 —— 这正是"窗口大小改变的时候
    // 过渡不够丝滑"的一个来源。
    //
    // 现在只有 width/height/format 任一真的变化才计数。
    const bool changed = (width_ != width || height_ != height || modeFormat_ != format);
    if (changed) {
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(&header_->modeChangeCount));
    }

    width_ = width;
    height_ = height;
    modeFormat_ = format;

    // **不要在这里重置 nextSlot_**。
    //
    // 原来这里有一句 `nextSlot_ = 0;`，含义大概是"换尺寸了从头开始轮转"。
    // 但它和生产者轮转是同一个变量：拖拽期间每拍都调 SetMode，于是每一帧
    // 都被写进 0 号槽。消费者用 sequence lock 校验（上传前后 header.version
    // 必须一致），写同一个槽时消费者正好在读那个槽 —— 校验必然失败、这一帧
    // 被丢掉，表现就是拖拽期间预览一顿一顿（掉帧率能到一半以上）。
    //
    // 槽位轮转必须只由 Publish 推进。
}

bool FrameRing::WaitForConsumer(uint32_t timeoutMs) {
    const DWORD start = GetTickCount();
    for (;;) {
        if (!header_) return false;
        const uint32_t pid = LoadU32(&header_->consumerPid);
        if (pid != 0 && pid != GetCurrentProcessId()) {
            // 这个 pid 真的还活着吗？
            //
            // 踩过的坑：这里原来只看 `pid != 0` 就返回 true。消费者被杀掉
            // 之后没人会把 header.consumerPid 清零（进程是被强杀的，析构函数
            // 没机会跑），于是这个陈旧 pid 永远留着，服务端一直以为"有人在
            // 看"：会话状态卡在 Live、不转 Reconnecting/NO SIGNAL，重启界面
            // 之后旧 pid 也要等到新消费者覆盖它才会变。
            //
            // 判活必须用和 ConsumerAlive() 同一套标准（心跳优先、pid 兜底），
            // 否则两处会给出互相矛盾的答案：ConsumerAlive() 说死了要置
            // Reconnecting，WaitForConsumer() 下一秒又说活着把状态推回 Live，
            // 界面就在 NO SIGNAL 和最后一帧之间来回闪。
            if (!ConsumerAlive()) {
                // 死了：把陈旧的 pid 清掉，让下一次判断落到"没接入"分支。
                //
                // 只在真的确认它死了之后才清 —— ConsumerAlive() 对新接入的
                // 消费者（心跳还没写第一次）返回 true，不会误伤。
                InterlockedCompareExchange(
                    reinterpret_cast<volatile LONG*>(&header_->consumerPid), 0,
                    static_cast<LONG>(pid));
                if (GetTickCount() - start >= timeoutMs) return false;
                Sleep(20);
                continue;
            }
            const bool isNewConsumer = (pid != consumerPid_);
            consumerPid_ = pid;
            if (isNewConsumer) {
                // 新消费者接入：把上一个消费者崩溃时留下的僵尸槽全部释放，
                // 否则那几个槽会一直占着，新消费者一帧都拿不到。
                for (uint32_t i = 0; i < slotCount_; ++i) {
                    const int64_t owner = InterlockedCompareExchange64(
                        reinterpret_cast<volatile LONG64*>(&header_->slotOwners[i]), 0, 0);
                    if (owner != -1 && owner != 0) {
                        InterlockedExchange64(
                            reinterpret_cast<volatile LONG64*>(&header_->slotOwners[i]), -1);
                    }
                }
                TouchConsumerHeartbeat();
            }
            return true;
        }
        if (GetTickCount() - start >= timeoutMs) return false;
        Sleep(20);
    }
}

void FrameRing::TouchConsumerHeartbeat() {
    if (!header_) return;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    InterlockedExchange64(
        reinterpret_cast<volatile LONG64*>(&header_->consumerHeartbeatQpc), t.QuadPart);
}

bool FrameRing::ConsumerAlive(uint32_t staleMs) const {
    if (!header_) return false;
    const uint32_t pid = LoadU32(&header_->consumerPid);
    if (pid == 0) return false;

    // 以**心跳**为第一依据。
    //
    // 这里曾经反过来：先 OpenProcess 查 pid 是否存活，心跳只作为补充。
    // 结果在 Electron 下踩了坑 —— 渲染进程跑在受保护/较低完整性级别里，
    // OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION) 会失败，pidAlive=false，
    // 于是生产者每秒都误判"消费者已死"，反复断开重连；而重连时环是被
    // 重建/截断的，addon 手里那份映射就再也看不到新帧 —— 界面上表现为
    // "只画出一帧就卡住"。
    //
    // 心跳是消费者主动写的（preload 每 4ms 写一次），比 pid 探活可靠得多，
    // 也不会受进程权限影响。消费者真的死了，心跳必然停止。
    const int64_t beat = InterlockedCompareExchange64(
        reinterpret_cast<volatile LONG64*>(&header_->consumerHeartbeatQpc), 0, 0);

    if (beat == 0) {
        // 从没写过心跳 —— 几乎只可能是"刚握手、第一次心跳还没来得及写"。
        //
        // 消费者 Bind() 里的顺序是「先写 consumerPid，再 TouchHeartbeat()」，
        // 中间有几微秒的窗口。生产者是每 500ms 查一次，撞上这个窗口的概率
        // 不算低，而撞上的后果很重：判一次"消费者已死"就要走
        // connected=false → state=Reconnecting，界面那边立刻铺 NO SIGNAL，
        // 一秒钟后又自己好了 —— 用户看到的就是"启动时有画面，然后闪一下
        // 没信号"。
        //
        // 所以这里按"信它"处理。真正的死消费者一定写过心跳（Bind 成功就会
        // 写一次），beat==0 而 consumerPid!=0 的组合只出现在刚接入那一瞬。
        return true;
    }

    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const int64_t ageMs = (now.QuadPart - beat) * 1000 / qpcFrequency_;
        if (ageMs <= static_cast<int64_t>(staleMs)) return true;
    }

    // 心跳缺失或已过期：这时才去问操作系统，而且只把"确实活着"当成活着，
    // 查不到就按死了处理（此时心跳也过期了，两者一致）。
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    DWORD code = 0;
    const bool pidAlive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return pidAlive;
}

uint32_t FrameRing::ReclaimDeadSlots() {
    if (!header_) return 0;
    const uint32_t consumerPid = LoadU32(&header_->consumerPid);
    const DWORD me = GetCurrentProcessId();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    uint32_t reclaimed = 0;

    for (uint32_t i = 0; i < slotCount_; ++i) {
        const int64_t owner = InterlockedCompareExchange64(
            reinterpret_cast<volatile LONG64*>(&header_->slotOwners[i]), 0, 0);
        if (owner == -1 || owner == 0 || owner == static_cast<int64_t>(me)) continue;
        if (consumerPid != 0 && owner == static_cast<int64_t>(consumerPid)) {
            // 归当前消费者所有，不能抢
            continue;
        }

        // 被别人占着：看占用时间是否已经远超合理范围（消费者崩在中途）
        const int64_t ownedAt = InterlockedCompareExchange64(
            reinterpret_cast<volatile LONG64*>(&header_->slotOwnQpc[i]), 0, 0);
        const int64_t ageMs =
            ownedAt != 0 ? (now.QuadPart - ownedAt) * 1000 / qpcFrequency_ : INT64_MAX;
        if (ageMs > SHADOW_SLOT_LEAK_MS) {
            InterlockedExchange64(
                reinterpret_cast<volatile LONG64*>(&header_->slotOwners[i]), -1);
            ++reclaimed;
        }
    }
    return reclaimed;
}

int FrameRing::AcquireConsumerSlot() {
    // 直接轮转后台缓冲即可。
    //
    // 早期版本还在这里做"槽位归属"判断（消费者占着就不复用），但改用
    // sequence lock 之后这层就不需要了：消费者靠 header.version 判断
    // 自己读到的是不是完整一帧，生产者可以放心覆盖任意一个槽。
    // 去掉归属判断的另一个好处是生产端永远不会因为"槽不够"而丢帧。
    if (!header_ || slotCount_ == 0) return -1;
    const int s = nextSlot_;
    nextSlot_ = (nextSlot_ + 1) % static_cast<int>(slotCount_);
    return s;
}

bool FrameRing::Publish(const uint8_t* src, uint32_t srcStride, uint32_t width, uint32_t height,
                        uint64_t presentQpc) {
    if (!header_ || !src || width == 0 || height == 0) return false;

    const uint32_t pid = LoadU32(&header_->consumerPid);
    if (pid == 0) return false; // 还没有消费者，不必付出拷贝成本

    const uint32_t rowBytes = width * 4u;
    if (srcStride < rowBytes) return false;
    const uint64_t needBytes = static_cast<uint64_t>(rowBytes) * height;
    if (needBytes > slotStride_) return false; // 超出预留容量，理论上不会发生

    const int slot = AcquireConsumerSlot();
    if (slot < 0) return false;

    // ---- 先把像素拷进槽里 ------------------------------------------------
    //
    // 重要顺序约束：每个槽的布局是 [槽头][像素...]，槽头大小就是
    // sizeof(ShadowRingHeader)，像素区紧跟在它后面。
    //
    // 这里必须先拷像素、最后写槽头。之前反过来写（先写槽头再 memcpy），
    // memcpy 的目的地址算成了槽头地址，于是把刚写好的几何信息整片覆盖掉，
    // 消费者读到的 width/height/stride 全是像素字节（0xFF161616 这种）。
    uint8_t* slotBase = base_ + ShadowRingSlotOffset(header_, static_cast<uint32_t>(slot));
    uint8_t* dst = slotBase + sizeof(ShadowRingHeader);

    if (srcStride == rowBytes) {
        std::memcpy(dst, src, static_cast<size_t>(needBytes));
    } else {
        // 显卡的行间距常常大于 width*4，必须按行拷贝
        const uint8_t* s = src;
        uint8_t* d = dst;
        for (uint32_t y = 0; y < height; ++y) {
            std::memcpy(d, s, rowBytes);
            s += srcStride;
            d += rowBytes;
        }
    }

    // ---- 像素写完之后再写槽头 -------------------------------------------
    // 槽头是"这一帧已经就绪"的标志，必须最后写。
    ShadowRingHeader* sh = reinterpret_cast<ShadowRingHeader*>(slotBase);
    sh->width = width;
    sh->height = height;
    sh->stride = rowBytes;
    sh->format = 0;
    sh->lastPublishQpc = static_cast<int64_t>(presentQpc);
    sh->producerPid = static_cast<uint32_t>(GetCurrentProcessId());

    PublishInternal(slot, width, height, rowBytes, presentQpc);
    // 记住这一帧的真实尺寸：RepublishLast 要按它去重发（见 FrameRing.h 里
    // lastPublishedWidth_ 的注释 —— 不能用建环时的 width_/height_，那会越界）。
    lastPublishedWidth_ = width;
    lastPublishedHeight_ = height;
    return true;
}

void FrameRing::PublishInternal(int slot, uint32_t width, uint32_t height, uint32_t stride,
                                uint64_t presentQpc) {
    (void)width; (void)height; (void)stride;

    // 顺序很重要：
    //   1) 保证像素数据已经写完（下面每个步骤本身就带 full barrier，
    //      但显式写一次更清楚）；
    //   2) 发布槽号；
    //   3) 递增发布序号 —— 消费者以"序号变了"作为有完整新帧的信号。
    _ReadWriteBarrier();
    StoreU32(&header_->publishedSlot, static_cast<uint32_t>(slot));
    StoreU64(&header_->counter, LoadU64(&header_->counter) + 1);

    if (presentQpc) StoreI64(&header_->lastPublishQpc, static_cast<int64_t>(presentQpc));

    ++produced_;
    PlainInc(&header_->producedFrames);

    // 把槽交给消费者：owner 写成"消费者的 pid"，并记下交接时刻 ——
    // 回收僵尸占用时要靠这个时间戳判断"占太久了"。
    const uint32_t pidNow = LoadU32(&header_->consumerPid);
    InterlockedExchange64(
        reinterpret_cast<volatile LONG64*>(&header_->slotOwners[slot]),
        static_cast<LONG64>(pidNow != 0 ? pidNow : 0));
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        lastPublishedQpc_ = now.QuadPart;
        InterlockedExchange64(
            reinterpret_cast<volatile LONG64*>(&header_->slotOwnQpc[slot]), now.QuadPart);
    }

    // 记住这一帧，供桌面静止时重复发布（见 RepublishLast）
    lastPublishedSlot_ = slot;

    SetEvent(frameEvent_);
}

bool FrameRing::RepublishLast(const uint8_t* src, uint32_t srcStride, uint32_t w, uint32_t h) {
    if (!header_) return false;
    if (lastPublishedSlot_ < 0) return false;

    const uint32_t pid = LoadU32(&header_->consumerPid);
    if (pid == 0) return false;

    // 有像素就直接走正常发布（会换一个槽，内容一致）；
    // 没有就只把 counter 往前推，让下游知道"有一帧可以画"。
    //
    // 尺寸必须用**上一帧真实发布过的**尺寸，不能用建环时的 width_/height_：
    // 预览帧可能是降采样过的（见 Session.cpp），按采集尺寸读会越界，而且
    // 槽头会写回一个和像素对不上的几何，界面那边的纹理就得重建（表现为闪）。
    const uint32_t rw = w > 0 ? w : (lastPublishedWidth_ > 0 ? lastPublishedWidth_ : width_);
    const uint32_t rh = h > 0 ? h : (lastPublishedHeight_ > 0 ? lastPublishedHeight_ : height_);

    if (src && rw > 0 && rh > 0) {
        return Publish(src, srcStride, rw, rh, 0);
    }

    PublishInternal(lastPublishedSlot_, rw, rh, rw * 4u, 0);
    return true;
}

ShadowRingHeader* FrameRing::SlotHeader(int slot) const {
    if (!base_ || !header_) return nullptr;
    if (slot < 0 || slot >= static_cast<int>(slotCount_)) return nullptr;
    return reinterpret_cast<ShadowRingHeader*>(
        base_ + ShadowRingSlotOffset(header_, static_cast<uint32_t>(slot)));
}

} // namespace shadow
