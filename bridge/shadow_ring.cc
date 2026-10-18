// shadow_ring.cc — Electron(N-API) 侧的共享内存帧环消费者。
//
// 这一层故意做得很薄：只负责"把后端的共享内存映射进 Electron 主进程"，
// 并把基地址包装成一个 ArrayBuffer 交给上层。取帧、渲染、缩放全部在 JS/WebGL 里做。
//
// 这里直接用 N-API 的 C 接口（没有用 node-addon-api 的 C++ 包装）：
// 本机既没有 node-gyp 也没有 node-addon-api/napi.h，而需要的功能很少，
// 直接调 C 接口反而更短、更透明。
//
// 暴露的 API：
//   waitReady(session) -> boolean               握手：服务是否已经把环建好
//   open(session, timeoutMs) -> object          打开并绑定，返回几何信息
//   hello(session) -> boolean                   渲染端 reload 后重新绑定
//   start(session) -> object { buffer, ... }    把整块共享内存包装成 ArrayBuffer
//   newBufferHandle(session) -> object          再要一个独立句柄（给另一个 JS 世界）
//   waitConsumed(session, timeoutMs) -> boolean 阻塞等待"有新帧"事件
//   consume(session) -> boolean                 非阻塞取走一个通知
//   stats(session) -> object                    生产端计数（诊断用）
//   close(session)                              断开并解除映射
//   exportStart(segmentsDir, exportDir, prefix?, maxSeconds?) -> object
//                                               发起一次导出（需求 6）
//   exportStatus() -> object                    导出进度/结果
//
// 导出为什么在插件里而不是服务里：见下面 ExportStart 上方的长注释。
//
// 名字必须和 native/backend/RingNames.h 完全一致。

#include "node_api.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "../native/ShadowRing.h"

// node-gyp 通常会在命令行上注入这个宏；我们直接手写编译，所以在这里补上。
#ifndef NODE_GYP_MODULE_NAME
#define NODE_GYP_MODULE_NAME shadow_ring
#endif

namespace {

// ---------------------------------------------------------------------------
// 名称规则（必须与 native/backend/RingNames.h 保持一致）
// ---------------------------------------------------------------------------
std::string RingName(const std::string& id)       { return "ShadowPlay.Frame."    + id; }
std::string ReadyEventName(const std::string& id) { return "ShadowPlay.Ready."    + id; }
std::string FrameEventName(const std::string& id) { return "ShadowPlay.FrameEvt." + id; }

// ---------------------------------------------------------------------------
// 一个会话对应的帧环映射
// ---------------------------------------------------------------------------
class Ring {
public:
    ~Ring() { Close(); }

    bool Open(const std::string& session, int timeoutMs) {
        session_ = session;
        const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeoutMs > 0 ? timeoutMs : 0);
        for (;;) {
            if (TryOpenOnce()) return true;
            if (timeoutMs <= 0 || GetTickCount() >= deadline) break;
            Sleep(30);
        }
        return false;
    }

    void Close() {
        if (frameEvent_) { CloseHandle(frameEvent_); frameEvent_ = nullptr; }
        if (readyEvent_) { CloseHandle(readyEvent_); readyEvent_ = nullptr; }
        if (view_)       { UnmapViewOfFile(view_);   view_ = nullptr; }
        if (mapping_)    { CloseHandle(mapping_);    mapping_ = nullptr; }
        header_ = nullptr;
        bound_ = false;
    }

    // 渲染进程 reload 后重新接入：清掉旧 consumerPid，让服务端重新握手
    void Rebind() {
        if (!header_) return;
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&header_->consumerPid), 0);
        bound_ = false;
        if (frameEvent_) {
            while (WaitForSingleObject(frameEvent_, 0) == WAIT_OBJECT_0) {
                // 丢掉积压的旧通知
            }
        }
    }

    bool Bind() {
        if (!header_) return false;
        if (bound_) return true;

        const LONG me = static_cast<LONG>(GetCurrentProcessId());
        const LONG prev = InterlockedCompareExchange(
            reinterpret_cast<volatile LONG*>(&header_->consumerPid), me, 0);

        if (prev != 0 && prev != me) {
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                   static_cast<DWORD>(prev));
            bool alive = false;
            if (h) {
                DWORD code = 0;
                alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
                CloseHandle(h);
            }
            if (alive) return false; // 真的还有别的消费者
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&header_->consumerPid), me);
        }
        bound_ = true;
        ClearStaleSlots();
        TouchHeartbeat();
        return true;
    }

    bool WaitFrame(DWORD timeoutMs) {
        if (!frameEvent_) return false;
        return WaitForSingleObject(frameEvent_, timeoutMs) == WAIT_OBJECT_0;
    }

    /**
     * 刷新消费者心跳。
     *
     * 生产者用它判断消费者是否还活着。只靠 pid 存活判断不够可靠：
     * 消费者进程崩溃后槽位会一直被占着，而且 pid 还可能被系统复用。
     * 消费者只要在跑就应当定期调用（poll() 里会调）。
     */
    void TouchHeartbeat() {
        if (!header_) return;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        InterlockedExchange64(
            reinterpret_cast<volatile LONG64*>(&header_->consumerHeartbeatQpc), now.QuadPart);
    }

    /**
     * 接管时清理僵尸槽：上一个消费者崩溃留下的占用会让新消费者一帧都拿不到。
     * 只清"既不是生产者、也不是我们自己"的占用。
     */
    void ClearStaleSlots() {
        if (!header_) return;
        const LONG me = static_cast<LONG>(GetCurrentProcessId());
        for (uint32_t i = 0; i < header_->slotCount && i < SHADOW_SLOT_COUNT; ++i) {
            const int64_t owner = InterlockedCompareExchange64(
                reinterpret_cast<volatile LONG64*>(&header_->slotOwners[i]), 0, 0);
            if (owner != -1 && owner != 0 && owner != static_cast<int64_t>(me)) {
                InterlockedExchange64(
                    reinterpret_cast<volatile LONG64*>(&header_->slotOwners[i]), -1);
            }
        }
    }

    ShadowRingHeader* header() const { return header_; }
    void*  view() const { return view_; }
    bool   valid() const { return header_ != nullptr; }

private:
    bool TryOpenOnce() {
        Close();
        mapping_ = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, RingName(session_).c_str());
        if (!mapping_) return false; // 服务可能还没建好

        view_ = MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        if (!view_) { Close(); return false; }

        ShadowRingHeader* h = static_cast<ShadowRingHeader*>(view_);
        if (h->magic != SHADOW_RING_MAGIC || h->version != SHADOW_RING_VERSION) {
            Close();      // 头部还没写完，稍后重试
            return false;
        }
        if (h->slotCount == 0 || h->slotCount > SHADOW_SLOT_COUNT ||
            h->slotStride <= 0 || h->headerSize < sizeof(ShadowRingHeader)) {
            Close();
            return false;
        }
            header_ = h;

        readyEvent_ = OpenEventA(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE,
                                 ReadyEventName(session_).c_str());
        frameEvent_ = OpenEventA(SYNCHRONIZE, FALSE, FrameEventName(session_).c_str());
        if (!frameEvent_) { Close(); return false; }

        while (WaitForSingleObject(frameEvent_, 0) == WAIT_OBJECT_0) {
        }
        return true;
    }

    std::string session_;
    HANDLE mapping_ = nullptr;
    HANDLE readyEvent_ = nullptr;
    HANDLE frameEvent_ = nullptr;
    void*  view_ = nullptr;
    ShadowRingHeader* header_ = nullptr;
    bool   bound_ = false;
};

std::unordered_map<std::string, std::unique_ptr<Ring>>& Registry() {
    static std::unordered_map<std::string, std::unique_ptr<Ring>> instance;
    return instance;
}

Ring* Find(const std::string& session) {
    auto& reg = Registry();
    auto it = reg.find(session);
    return it == reg.end() ? nullptr : it->second.get();
}

// ---------------------------------------------------------------------------
// 参数辅助
// ---------------------------------------------------------------------------
bool GetStringArg(napi_env env, napi_value value, std::string& out) {
    napi_valuetype t = napi_undefined;
    if (napi_typeof(env, value, &t) != napi_ok || t != napi_string) return false;
    size_t len = 0;
    if (napi_get_value_string_utf8(env, value, nullptr, 0, &len) != napi_ok) return false;
    out.resize(len);
    size_t written = 0;
    if (napi_get_value_string_utf8(env, value, out.data(), len + 1, &written) != napi_ok) {
        return false;
    }
    out.resize(written);
    return true;
}

int32_t GetIntArg(napi_env env, napi_value value, int32_t fallback) {
    napi_valuetype t = napi_undefined;
    if (napi_typeof(env, value, &t) != napi_ok || t != napi_number) return fallback;
    int32_t v = fallback;
    if (napi_get_value_int32(env, value, &v) != napi_ok) return fallback;
    return v;
}

napi_value Undefined(napi_env env) {
    napi_value v = nullptr;
    napi_get_undefined(env, &v);
    return v;
}

napi_value Boolean(napi_env env, bool b) {
    napi_value v = nullptr;
    napi_get_boolean(env, b, &v);
    return v;
}

napi_value Number(napi_env env, double d) {
    napi_value v = nullptr;
    napi_create_double(env, d, &v);
    return v;
}

napi_value Str(napi_env env, const std::string& s) {
    napi_value v = nullptr;
    napi_create_string_utf8(env, s.c_str(), s.size(), &v);
    return v;
}

void SetProp(napi_env env, napi_value obj, const char* key, napi_value val) {
    napi_set_named_property(env, obj, key, val);
}

/** 抛出一个 JS Error 并返回 nullptr，便于 `return ThrowError(...)` 的写法 */
napi_value ThrowError(napi_env env, const std::string& msg) {
    napi_throw_error(env, nullptr, msg.c_str());
    return nullptr;
}

/** 取出待抛出的 JS 异常（交还给运行时，别把它吞掉） */
napi_value TakePendingException(napi_env env) {
    bool pending = false;
    if (napi_is_exception_pending(env, &pending) != napi_ok || !pending) return nullptr;
    napi_value err = nullptr;
    if (napi_get_and_clear_last_exception(env, &err) != napi_ok) return nullptr;
    return err;
}

// 前几个参数固定为 (session, timeoutMs?) 的取参
struct Args {
    napi_value argv[3] = {nullptr, nullptr, nullptr};
    size_t argc = 0;
    std::string session;
    bool hasSession = false;
};

bool ExtractArgs(napi_env env, napi_callback_info info, Args& args, size_t maxArgs) {
    args.argc = maxArgs;
    napi_value thisArg = nullptr;
    if (napi_get_cb_info(env, info, &args.argc, args.argv, &thisArg, nullptr) != napi_ok) {
        return false;
    }
    if (args.argc < 1) return false;
    args.hasSession = GetStringArg(env, args.argv[0], args.session);
    return args.hasSession;
}

// ---------------------------------------------------------------------------
// 把帧环暴露给 JS
// ---------------------------------------------------------------------------
// 这里**不用** napi_create_external_arraybuffer：
//
//   Electron（以及开了 V8 sandbox 的 Node）里，外部 ArrayBuffer 指向的是
//   内存笼之外的原生内存，运行时要么直接拒绝（napi_no_external_buffers_allowed），
//   要么返回一个在 JS 侧根本用不了的句柄 —— 我们在 Electron 42 上实测到的
//   是后者：调用不报错，但拿到的属性是 undefined。
//
// 改用 V8 自己管理的普通 ArrayBuffer：
//   * 整块缓冲区**只分配一次**，之后 start()/newBufferHandle() 都只创建
//     Uint8Array 视图指向它，于是 preload（隔离世界）和主世界看到的是同一块内存；
//   * 服务端每写一帧，我们只把那一个槽 memcpy 进缓冲区（4K 时 14.7MB，
//     1080p 时 8.3MB），然后同步头部标量、最后递增 counter；
//   * 消费者在缓冲区里把 slotOwners[slot] 置为 -1，这本身就是它"已读完"的信号
//     （见 native/ShadowRing.h 的槽位归属协议）。
//
// 代价：多一份帧环大小的常驻内存（4K 约 59MB，1080p 约 33MB），换来做主流程
// 在所有运行时上都能跑。
struct SharedRing {
    napi_ref bufferRef = nullptr; // 持有 ArrayBuffer，防止被 GC
    uint8_t* data = nullptr;      // 指向 V8 分配的存储
    size_t size = 0;
    Ring* ring = nullptr;
};

std::unordered_map<std::string, std::unique_ptr<SharedRing>>& SharedRings() {
    static std::unordered_map<std::string, std::unique_ptr<SharedRing>> instance;
    return instance;
}

napi_value GetOrCreateBuffer(napi_env env, SharedRing* shared, size_t* outSize) {
    if (shared->bufferRef) {
        napi_value buffer = nullptr;
        if (napi_get_reference_value(env, shared->bufferRef, &buffer) == napi_ok && buffer) {
            *outSize = shared->size;
            return buffer;
        }
        // 引用失效（一般不会发生），重建
        napi_delete_reference(env, shared->bufferRef);
        shared->bufferRef = nullptr;
        shared->data = nullptr;
    }

    ShadowRingHeader* h = shared->ring->header();
    const uint64_t total = static_cast<uint64_t>(h->headerSize) +
                           static_cast<uint64_t>(h->slotStride) * h->slotCount;
    if (total == 0 || total > (1ull << 32)) {
        ThrowError(env, "帧环尺寸异常。");
        return nullptr;
    }

    void* data = nullptr;
    napi_value buffer = nullptr;
    const napi_status st =
        napi_create_arraybuffer(env, static_cast<size_t>(total), &data, &buffer);
    if (st != napi_ok || !data || !buffer) {
        ThrowError(env, "无法创建帧环缓冲区（napi_create_arraybuffer = " +
                            std::to_string(static_cast<int>(st)) +
                            "），可能需要更大的内存。");
        return nullptr;
    }

    // 首次创建：整块拷过来（头部 + 所有槽），之后只需要拷被发布的那个槽
    std::memcpy(data, shared->ring->view(), static_cast<size_t>(total));
    shared->data = static_cast<uint8_t*>(data);
    shared->size = static_cast<size_t>(total);

    if (napi_create_reference(env, buffer, 1, &shared->bufferRef) != napi_ok) {
        ThrowError(env, "无法持有帧环缓冲区引用。");
        return nullptr;
    }

    *outSize = shared->size;
    return buffer;
}

/** 把服务端当前发布的槽同步进 V8 缓冲区 */
void SyncRingToBuffer(SharedRing* shared) {
    if (!shared || !shared->ring || !shared->data) return;
    Ring* ring = shared->ring;
    ShadowRingHeader* src = ring->header();
    auto* dst = reinterpret_cast<ShadowRingHeader*>(shared->data);

    // 快路径：没有新帧就什么都不做。
    // 同步泵跑在 ~240Hz，而服务端只在桌面有变化时发帧；桌面静止时
    // 这一步只比较一个整数，避免白白 memcpy 十几 MB。
    const uint64_t counter = src->counter;
    if (counter == dst->syncVersion) return;

    const uint32_t slotCount = src->slotCount;
    const uint32_t publishedSlot = src->publishedSlot;
    if (slotCount == 0 || publishedSlot >= slotCount) return;

    const uint64_t slotOffset = static_cast<uint64_t>(src->headerSize) +
                                static_cast<uint64_t>(src->slotStride) * publishedSlot;
    const size_t slotBytes = static_cast<size_t>(src->slotStride);
    if (slotOffset + slotBytes > shared->size) return;

    // 这是消费者/生产者之间唯一的同步字段（sequence lock）：
    //   syncVersion 只在"槽内容 + 头部标量都写完"之后才推进。
    // 消费者在拷像素前后各读一次 syncVersion，两次相同且非 0 就说明读到的是
    // 完整一帧；不同就丢掉这帧等下一帧。既不会读到半帧，也不需要额外的锁。
    //
    // 注意不要用 header.version —— 那是协议版本号，语义完全不同。
    dst->syncVersion = 0; // 声明"正在写"
    _ReadWriteBarrier();

    // 1) 槽内容（槽头 + 像素）
    std::memcpy(shared->data + slotOffset,
                static_cast<const uint8_t*>(ring->view()) + slotOffset, slotBytes);

    // 2) 头部标量
    dst->headerSize = src->headerSize;
    dst->slotCount = src->slotCount;
    dst->slotStride = src->slotStride;
    dst->width = src->width;
    dst->height = src->height;
    dst->stride = src->stride;
    dst->format = src->format;
    dst->producerPid = src->producerPid;
    dst->consumerPid = src->consumerPid;
    dst->qpcFrequency = src->qpcFrequency;
    dst->lastPublishQpc = src->lastPublishQpc;
    dst->producedFrames = src->producedFrames;
    dst->droppedFrames = src->droppedFrames;
    dst->skippedFrames = src->skippedFrames;
    dst->modeChangeCount = src->modeChangeCount;
    dst->captureErrors = src->captureErrors;
    dst->publishedSlot = publishedSlot;
    dst->counter = counter;

    // 3) 最后推进 syncVersion，读者以它为准
    _ReadWriteBarrier();
    dst->syncVersion = counter;
}

/** 创建指向共享缓冲区的一个新的 Uint8Array 视图 */
napi_value MakeRingView(napi_env env, napi_value buffer, size_t byteLength) {
    napi_value view = nullptr;
    if (napi_create_typedarray(env, napi_uint8_array, byteLength, buffer, 0, &view) == napi_ok) {
        return view;
    }
    return nullptr;
}

void FillStartResult(napi_env env, Ring* ring, napi_value buffer, napi_value view,
                     size_t byteLength, napi_value* out) {
    ShadowRingHeader* h = ring->header();
    napi_create_object(env, out);
    SetProp(env, *out, "buffer", buffer);
    if (view) SetProp(env, *out, "view", view);
    SetProp(env, *out, "byteLength", Number(env, static_cast<double>(byteLength)));
    SetProp(env, *out, "headerSize", Number(env, h->headerSize));
    SetProp(env, *out, "slotCount", Number(env, h->slotCount));
    SetProp(env, *out, "slotStride", Number(env, static_cast<double>(h->slotStride)));
    SetProp(env, *out, "sourceWidth", Number(env, h->width));
    SetProp(env, *out, "sourceHeight", Number(env, h->height));
}

/** start / newBufferHandle 共用的实现 */
napi_value BuildBufferHandle(napi_env env, const std::string& session) {
    Ring* ring = Find(session);
    if (!ring || !ring->valid()) {
        return ThrowError(env, "帧环不存在，请先调用 open()。");
    }
    if (!ring->Bind()) {
        return ThrowError(env, "帧环已被占用。");
    }

    auto& shared = SharedRings()[session];
    if (!shared) shared = std::make_unique<SharedRing>();
    shared->ring = ring;

    size_t byteLength = 0;
    napi_value buffer = GetOrCreateBuffer(env, shared.get(), &byteLength);
    if (!buffer) return TakePendingException(env);

    // 给调用方一份"最新的"数据，避免刚连上时看到还没同步的旧内容
    SyncRingToBuffer(shared.get());

    napi_value view = MakeRingView(env, buffer, byteLength);
    napi_value out = nullptr;
    FillStartResult(env, ring, buffer, view, byteLength, &out);
    return out;
}

// ---------------------------------------------------------------------------
// poll(session) —— 把服务端最新一帧同步进缓冲区
//
// 消费者（渲染进程）每帧调一次即可，返回当前发布序号。
// ---------------------------------------------------------------------------
napi_value Poll(napi_env env, napi_callback_info info) {
    Args a;
    if (!ExtractArgs(env, info, a, 1)) {
        return ThrowError(env, "poll(session: string)");
    }
    auto& shared = SharedRings();
    auto it = shared.find(a.session);
    if (it == shared.end() || !it->second || !it->second->ring) {
        return Boolean(env, false);
    }
    if (!it->second->ring->valid()) return Boolean(env, false);
    it->second->ring->TouchHeartbeat();
    SyncRingToBuffer(it->second.get());
    return Boolean(env, true);
}

// ---------------------------------------------------------------------------
// waitReady(session) -> boolean
// ---------------------------------------------------------------------------
napi_value WaitReady(napi_env env, napi_callback_info info) {
    Args a;
    if (!ExtractArgs(env, info, a, 1)) {
        return ThrowError(env, "waitReady(session: string)");
    }
    Ring* ring = Find(a.session);
    if (ring && ring->valid()) return Boolean(env, true);

    auto owned = std::make_unique<Ring>();
    if (!owned->Open(a.session, 0)) {
        return Boolean(env, false); // 服务还没起来，属于正常情况
    }
    Registry()[a.session] = std::move(owned);
    return Boolean(env, true);
}

// ---------------------------------------------------------------------------
// open(session, timeoutMs)
// ---------------------------------------------------------------------------
napi_value FillRingInfo(napi_env env, const std::string& session, ShadowRingHeader* h) {
    napi_value out = nullptr;
    napi_create_object(env, &out);
    SetProp(env, out, "session", Str(env, session));
    SetProp(env, out, "headerSize", Number(env, h->headerSize));
    SetProp(env, out, "slotCount", Number(env, h->slotCount));
    SetProp(env, out, "slotStride", Number(env, static_cast<double>(h->slotStride)));
    SetProp(env, out, "width", Number(env, h->width));
    SetProp(env, out, "height", Number(env, h->height));
    SetProp(env, out, "producerPid", Number(env, h->producerPid));
    return out;
}

napi_value Open(napi_env env, napi_callback_info info) {
    Args a;
    if (!ExtractArgs(env, info, a, 2)) {
        return ThrowError(env, "open(session: string, timeoutMs?: number)");
    }
    const int32_t timeout = a.argc >= 2 ? GetIntArg(env, a.argv[1], 5000) : 5000;

    // 已经打开过就直接复用
    Ring* existing = Find(a.session);
    if (existing && existing->valid()) {
        if (!existing->Bind()) {
            return ThrowError(env, "帧环已被另一个消费者占用。");
        }
        return FillRingInfo(env, a.session, existing->header());
    }

    auto owned = std::make_unique<Ring>();
    if (!owned->Open(a.session, timeout)) {
        return ThrowError(env, "无法打开共享内存帧环 '" + RingName(a.session) +
                                   "'：采集服务可能没有启动或启动失败。");
    }
    Ring* ring = owned.get();
    Registry()[a.session] = std::move(owned);

    if (!ring->Bind()) {
        return ThrowError(env, "共享内存帧环已被另一个消费者占用。");
    }
    return FillRingInfo(env, a.session, ring->header());
}

// ---------------------------------------------------------------------------
// hello(session) -> boolean   渲染端 reload 后重新绑定
// ---------------------------------------------------------------------------
napi_value Hello(napi_env env, napi_callback_info info) {
    Args a;
    if (!ExtractArgs(env, info, a, 1)) {
        return ThrowError(env, "hello(session: string)");
    }
    Ring* ring = Find(a.session);
    if (!ring || !ring->valid()) return Boolean(env, false);
    ring->Rebind();
    return Boolean(env, ring->Bind());
}

// ---------------------------------------------------------------------------
// start(session) / newBufferHandle(session)
//
// 两者都返回一个指向同一块共享内存的 ArrayBuffer，区别只是调用意图：
// transfer 掉其中一个之后，另一个仍然可读。
// ---------------------------------------------------------------------------
napi_value Start(napi_env env, napi_callback_info info) {
    Args a;
    if (!ExtractArgs(env, info, a, 1)) {
        return ThrowError(env, "start(session: string)");
    }
    return BuildBufferHandle(env, a.session);
}

napi_value NewBufferHandle(napi_env env, napi_callback_info info) {
    Args a;
    if (!ExtractArgs(env, info, a, 1)) {
        return ThrowError(env, "newBufferHandle(session: string)");
    }
    return BuildBufferHandle(env, a.session);
}

// ---------------------------------------------------------------------------
// waitConsumed(session, timeoutMs) -> boolean   阻塞等待"有新帧"
// ---------------------------------------------------------------------------
napi_value WaitConsumed(napi_env env, napi_callback_info info) {
    Args a;
    if (!ExtractArgs(env, info, a, 2)) {
        return ThrowError(env, "waitConsumed(session: string, timeoutMs?: number)");
    }
    const int32_t timeout = a.argc >= 2 ? GetIntArg(env, a.argv[1], 100) : 100;
    Ring* ring = Find(a.session);
    if (!ring || !ring->valid()) return Boolean(env, false);
    return Boolean(env, ring->WaitFrame(static_cast<DWORD>(timeout < 0 ? 0 : timeout)));
}

// ---------------------------------------------------------------------------
// consume(session) -> boolean   非阻塞
// ---------------------------------------------------------------------------
napi_value Consume(napi_env env, napi_callback_info info) {
    Args a;
    if (!ExtractArgs(env, info, a, 1)) {
        return ThrowError(env, "consume(session: string)");
    }
    Ring* ring = Find(a.session);
    if (!ring || !ring->valid()) return Boolean(env, false);
    return Boolean(env, ring->WaitFrame(0));
}

// ---------------------------------------------------------------------------
// stats(session)
// ---------------------------------------------------------------------------
napi_value Stats(napi_env env, napi_callback_info info) {
    Args a;
    if (!ExtractArgs(env, info, a, 1)) {
        return ThrowError(env, "stats(session: string)");
    }
    Ring* ring = Find(a.session);
    napi_value out = nullptr;
    napi_create_object(env, &out);
    if (!ring || !ring->valid()) {
        SetProp(env, out, "valid", Boolean(env, false));
        return out;
    }
    ShadowRingHeader* h = ring->header();
    SetProp(env, out, "valid", Boolean(env, true));
    SetProp(env, out, "produced", Number(env, static_cast<double>(h->producedFrames)));
    SetProp(env, out, "dropped", Number(env, static_cast<double>(h->droppedFrames)));
    SetProp(env, out, "counter", Number(env, static_cast<double>(h->counter)));
    SetProp(env, out, "width", Number(env, h->width));
    SetProp(env, out, "height", Number(env, h->height));
    SetProp(env, out, "modeChanges", Number(env, h->modeChangeCount));
    SetProp(env, out, "captureErrors", Number(env, h->captureErrors));
    return out;
}

// ---------------------------------------------------------------------------
// close(session)
// ---------------------------------------------------------------------------
napi_value Close(napi_env env, napi_callback_info info) {
    Args a;
    if (!ExtractArgs(env, info, a, 1)) {
        return ThrowError(env, "close(session: string)");
    }
    auto& reg = Registry();
    auto it = reg.find(a.session);
    if (it != reg.end()) {
        it->second->Close();
        reg.erase(it);
    }
    return Undefined(env);
}

} // namespace —— 临时关掉外层匿名 namespace，下面要用到 shadow:: 的公开名字

// ---------------------------------------------------------------------------
// 导出（需求 6）：把分片目录里的一圈分片合并成一个 MP4
//
// 【为什么这段必须在全局作用域里】
// 文件上面那个匿名 namespace 从这里一直开到文件末尾。如果把下面的
// `#include "../native/backend/Exporter.h"` 和用到 shadow::Exporter 的代码
// 写在它**里面**，`namespace shadow` 就被嵌进了匿名 namespace —— 于是
// Exporter 的真实符号名变成 `anonymous namespace'::shadow::Exporter::...`
// （修饰名里能看到 ?A0x770a5f14@ 这种匿名命名空间的哈希）。而 Exporter.cpp
// 是按 `shadow::Exporter` 编的，两边修饰名对不上，链接期稳定报 4 个 LNK2019。
// 所以这里先 `} // namespace` 把外层关掉，等这段写完再重新开一个。
// ---------------------------------------------------------------------------
//
// 为什么导出要放在这里、而不是让服务去干：
//   Exporter 只吃四个**纯字符串**参数（segmentsDir / exportDir / prefix /
//   maxSeconds），分片是已经落在磁盘上的 MPEG-TS 文件，**谁读都行** —— 它跟
//   会话、帧环、进程状态都没有关系。
//   原先导出放在 shadow-capture.exe 里、靠 stdin 把命令送进去，唯一原因是
//   "界面够不着 exe 里的类"。那带来一个死结：界面**复用用户自己起的服务**时
//   没有那个 stdin（stdio 不是它开的），导出永远回"没有命令通道"。
//   编进这个插件之后，界面直接调 C++，服务在不在、谁起的，都不影响导出。
//
// 跟录制的关系：分片还在被环形覆盖，所以 Exporter 内部会先快照到临时目录
// （见 Exporter.h 的说明）。跨进程做这件事一样安全 —— 它就是复制文件。
// ---------------------------------------------------------------------------

#include "../native/backend/Exporter.h"

// Exporter.cpp 只依赖这一个外部符号（它把 ffmpeg 子进程挂进作业对象，
// 免得界面退出后留一堆孤儿 ffmpeg）。服务和插件各有一份自己的作业对象。
//
// 这里**必须**是 extern "C" 且签名完全一致：Exporter.cpp:21 就是这么声明的。
extern "C" void* ShadowChildJob();

namespace {
// 插件侧的作业对象：第一次导出时建，进程退出时由内核自动收（KILL_ON_JOB_CLOSE）。
void* PluginChildJob() {
    static void* job = []() -> void* {
        HANDLE h = CreateJobObjectW(nullptr, nullptr);
        if (!h) return nullptr;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jl{};
        jl.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(h, JobObjectExtendedLimitInformation, &jl, sizeof(jl));
        return h;
    }();
    return job;
}
} // namespace

// 定义 Exporter.cpp 期待的那个符号。注意不能放在匿名 namespace 里 ——
// 那样就是内部链接，链接器找不到，表现为"未解析的外部符号 ShadowChildJob"。
extern "C" void* ShadowChildJob() { return PluginChildJob(); }

namespace {

// 插件里同时只允许一个导出在跑。界面那边一次只导一路，而且 Exporter 自己
// 也拒绝并发（Start() 在上一轮没结束时返回 false），这里再兜一层是为了
// 让状态查询有个稳定的落点。
std::mutex g_exportMu;
std::unique_ptr<shadow::Exporter> g_exporter;

napi_value ExportStatusToJs(napi_env env, const shadow::ExportStatus& st) {
    napi_value out = nullptr;
    napi_create_object(env, &out);
    SetProp(env, out, "running", Boolean(env, st.running));
    SetProp(env, out, "ok", Boolean(env, st.ok));
    SetProp(env, out, "failed", Boolean(env, st.failed));
    SetProp(env, out, "phase", Str(env, st.phase));
    SetProp(env, out, "outPath", Str(env, st.outPath));
    SetProp(env, out, "detail", Str(env, st.detail));
    SetProp(env, out, "segmentCount", Number(env, st.segmentCount));
    SetProp(env, out, "durationSeconds", Number(env, st.durationSeconds));
    SetProp(env, out, "bytes", Number(env, static_cast<double>(st.bytes)));
    SetProp(env, out, "elapsedMs", Number(env, st.elapsedMs));
    SetProp(env, out, "jobId", Number(env, static_cast<double>(st.jobId)));
    return out;
}

// ---------------------------------------------------------------------------
// exportStart(segmentsDir, exportDir, prefix, maxSeconds, targetFps)
//   -> {ok, error?, jobId?}
//
// 只是**发起**。真正的进度/结果靠 exportStatus() 轮询 —— 一次导出要几百毫秒
// 到几秒，同步等会把界面主进程卡住（主进程一卡，所有窗口的 IPC 都停）。
//
// 【targetFps 是干什么的】
//
// 分片里存的是"录的那一刻的帧率"。用户把某一路从 60fps 改成 240fps 之后，
// 已经录好的那批分片仍是 60fps —— 直接 `-c copy` 拼出来的回放也还是 60fps，
// 用户看到的就是"我明明改了帧率，导出来的还是老样子"。
//
// 把当前的配置帧率传进来，Exporter 就会在导出时做一次重采样：
//   * 目标帧率 < 源帧率 → 抽帧（`-vf fps=N`）
//   * 目标帧率 > 源帧率 → 插帧补齐（同样用 `-vf fps=N`，ffmpeg 会复制邻近帧）
// 时间轴始终按墙钟对齐，不会因为抽/插帧而变快变慢。
//
// targetFps <= 0 表示"保持源帧率"，此时走 `-c copy`，零损失、最快。
// ---------------------------------------------------------------------------
napi_value ExportStart(napi_env env, napi_callback_info info) {
    napi_value argv[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    size_t argc = 6;
    napi_value thisArg = nullptr;
    if (napi_get_cb_info(env, info, &argc, argv, &thisArg, nullptr) != napi_ok) {
        return ThrowError(env, "exportStart 取参失败");
    }

    std::string segmentsDir, exportDir, prefix;
    if (argc < 2 || !GetStringArg(env, argv[0], segmentsDir) ||
        !GetStringArg(env, argv[1], exportDir)) {
        return ThrowError(env, "exportStart(segmentsDir: string, exportDir: string, "
                               "prefix?: string, maxSeconds?: number, targetFps?: number, "
                               "segmentSeconds?: number)");
    }
    if (argc >= 3 && !GetStringArg(env, argv[2], prefix)) prefix.clear();
    const int32_t maxSeconds = argc >= 4 ? GetIntArg(env, argv[3], 0) : 0;
    // 0 或负数 = 不改帧率，保持分片原样（Exporter 内部也夹到 <=240）
    const int32_t targetFps = argc >= 5 ? GetIntArg(env, argv[4], 0) : 0;
    // 单片标称时长。导出时要靠它判"最后一片写完了没" —— 不传的话默认 2 秒，
    // 而配置里这个值是可以改的，改大了就会把完整的 2 秒片误判成半片。（缺省 2）
    const int32_t segmentSeconds = argc >= 6 ? GetIntArg(env, argv[5], 2) : 2;

    std::lock_guard<std::mutex> lk(g_exportMu);

    // 上一轮还在跑就直接说清楚，别悄悄排一个队 —— 界面要能提示"上一轮还没好"。
    if (g_exporter && g_exporter->Status().running) {
        napi_value out = nullptr;
        napi_create_object(env, &out);
        SetProp(env, out, "ok", Boolean(env, false));
        SetProp(env, out, "error", Str(env, "上一轮导出还没结束。"));
        return out;
    }

    auto ex = std::make_unique<shadow::Exporter>(
        segmentsDir, exportDir, prefix,
        maxSeconds > 0 ? static_cast<uint32_t>(maxSeconds) : 0u,
        targetFps > 0 ? static_cast<uint32_t>(targetFps) : 0u,
        segmentSeconds > 0 ? static_cast<uint32_t>(segmentSeconds) : 2u);
    if (!ex->Start()) {
        const shadow::ExportStatus st = ex->Status();
        napi_value out = nullptr;
        napi_create_object(env, &out);
        SetProp(env, out, "ok", Boolean(env, false));
        SetProp(env, out, "error",
                Str(env, st.detail.empty() ? std::string("导出没能开始（没有可导出的分片？）")
                                           : st.detail));
        return out;
    }

    g_exporter = std::move(ex);
    napi_value out = nullptr;
    napi_create_object(env, &out);
    SetProp(env, out, "ok", Boolean(env, true));
    SetProp(env, out, "jobId", Number(env, static_cast<double>(g_exporter->Status().jobId)));
    return out;
}

// ---------------------------------------------------------------------------
// exportStatus() -> 状态对象（没导过就返回 running/ok/failed 全 false）
// ---------------------------------------------------------------------------
napi_value ExportStatusOf(napi_env env, napi_callback_info info) {
    (void)info;
    std::lock_guard<std::mutex> lk(g_exportMu);
    if (!g_exporter) {
        napi_value out = nullptr;
        napi_create_object(env, &out);
        SetProp(env, out, "running", Boolean(env, false));
        SetProp(env, out, "ok", Boolean(env, false));
        SetProp(env, out, "failed", Boolean(env, false));
        SetProp(env, out, "phase", Str(env, ""));
        SetProp(env, out, "outPath", Str(env, ""));
        SetProp(env, out, "detail", Str(env, ""));
        SetProp(env, out, "segmentCount", Number(env, 0));
        SetProp(env, out, "durationSeconds", Number(env, 0));
        SetProp(env, out, "bytes", Number(env, 0));
        SetProp(env, out, "elapsedMs", Number(env, 0));
        SetProp(env, out, "jobId", Number(env, 0));
        return out;
    }
    return ExportStatusToJs(env, g_exporter->Status());
}

} // namespace

// ---------------------------------------------------------------------------
// 模块注册
// ---------------------------------------------------------------------------
napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor props[] = {
        {"waitReady",       nullptr, WaitReady,       nullptr, nullptr, nullptr, napi_default, nullptr},
        {"open",            nullptr, Open,            nullptr, nullptr, nullptr, napi_default, nullptr},
        {"hello",           nullptr, Hello,           nullptr, nullptr, nullptr, napi_default, nullptr},
        {"start",           nullptr, Start,           nullptr, nullptr, nullptr, napi_default, nullptr},
        {"newBufferHandle", nullptr, NewBufferHandle, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"poll",            nullptr, Poll,            nullptr, nullptr, nullptr, napi_default, nullptr},
        {"waitConsumed",    nullptr, WaitConsumed,    nullptr, nullptr, nullptr, napi_default, nullptr},
        {"consume",         nullptr, Consume,         nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stats",           nullptr, Stats,           nullptr, nullptr, nullptr, napi_default, nullptr},
        {"close",           nullptr, Close,           nullptr, nullptr, nullptr, napi_default, nullptr},
        {"exportStart",     nullptr, ExportStart,     nullptr, nullptr, nullptr, napi_default, nullptr},
        {"exportStatus",    nullptr, ExportStatusOf,  nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(props) / sizeof(props[0]), props);
    return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
