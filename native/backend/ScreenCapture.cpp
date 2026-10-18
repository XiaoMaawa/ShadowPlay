// ScreenCapture.cpp — DXGI 桌面复制实现
#include "ScreenCapture.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include <wrl/client.h>

namespace shadow {

namespace {
using Microsoft::WRL::ComPtr;

inline void SafeRelease(IUnknown* p) {
    if (p) p->Release();
}

// 一个显示器条目的排序键：先按虚拟桌面左上角的 y、再按 x。
// 用坐标而不是枚举顺序，是因为 EnumAdapters1 / EnumOutputs 的返回顺序
// 在驱动更新、显示器插拔后会变，而坐标不会（同一套桌面布局下）。
struct OutputRef {
    ComPtr<IDXGIOutput1> output;
    LONG left = 0;
    LONG top = 0;
    LONG right = 0;
    LONG bottom = 0;
    int  attached = 0;
};

// 从某个适配器把它的输出条包成 OutputRef（跳过不是 IDXGIOutput1 的）。
void CollectOutputsOf(IDXGIAdapter1* adapter, std::vector<OutputRef>& out) {
    for (UINT i = 0;; ++i) {
        IDXGIOutput* raw = nullptr;
        if (adapter->EnumOutputs(i, &raw) == DXGI_ERROR_NOT_FOUND) break;
        if (!raw) break;
        DXGI_OUTPUT_DESC desc{};
        raw->GetDesc(&desc);
        IDXGIOutput1* o1 = nullptr;
        if (SUCCEEDED(raw->QueryInterface(__uuidof(IDXGIOutput1), (void**)&o1)) && o1) {
            OutputRef ref;
            ref.output.Attach(o1);
            ref.left = desc.DesktopCoordinates.left;
            ref.top = desc.DesktopCoordinates.top;
            ref.right = desc.DesktopCoordinates.right;
            ref.bottom = desc.DesktopCoordinates.bottom;
            ref.attached = desc.AttachedToDesktop ? 1 : 0;
            out.push_back(std::move(ref));
        }
        raw->Release();
    }
}

// 全局枚举：所有适配器（含 Microsoft Basic Render Driver）的所有输出，
// 按「已连接优先 → 上→下 → 左→右」排序。**只对 AttachedToDesktop 的计数**，
// 拔掉的显示器不该占走一个序号。
void CollectAllOutputs(std::vector<OutputRef>& out) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)factory.GetAddressOf())) ||
        !factory) {
        return;
    }
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        if (!adapter) break;
        CollectOutputsOf(adapter, out);
        adapter->Release();
    }
    std::sort(out.begin(), out.end(), [](const OutputRef& a, const OutputRef& b) {
        if (a.attached != b.attached) return a.attached > b.attached;
        if (a.top != b.top) return a.top < b.top;
        if (a.left != b.left) return a.left < b.left;
        return a.right < b.right;
    });
}

// 取第 n 个**已连接**的显示器。优先在 preferred 适配器上找（那是 D3D 设备
// 所在的那块卡，同卡通路的 DuplicateOutput 最稳妥），找不到再全局找。
HRESULT OpenNthOutput(IDXGIAdapter* preferred, uint32_t n, IDXGIOutput1** out) {
    *out = nullptr;

    // 1) 先在 preferred 这块卡上按顺序找
    if (preferred) {
        ComPtr<IDXGIAdapter1> a1;
        if (SUCCEEDED(preferred->QueryInterface(__uuidof(IDXGIAdapter1), (void**)a1.GetAddressOf())) &&
            a1) {
            std::vector<OutputRef> local;
            CollectOutputsOf(a1.Get(), local);
            std::stable_sort(local.begin(), local.end(),
                             [](const OutputRef& a, const OutputRef& b) {
                                 if (a.attached != b.attached) return a.attached > b.attached;
                                 if (a.top != b.top) return a.top < b.top;
                                 return a.left < b.left;
                             });
            uint32_t seen = 0;
            for (auto& r : local) {
                if (!r.attached) continue;
                if (seen == n) {
                    *out = r.output.Detach();
                    return S_OK;
                }
                ++seen;
            }
        }
    }

    // 2) preferred 上不够数，退到全局枚举
    std::vector<OutputRef> all;
    CollectAllOutputs(all);
    uint32_t seen = 0;
    for (auto& r : all) {
        if (!r.attached) continue;
        if (seen == n) {
            *out = r.output.Detach();
            return S_OK;
        }
        ++seen;
    }
    return DXGI_ERROR_NOT_FOUND;
}
} // namespace

std::vector<MonitorInfo> EnumerateMonitors() {
    std::vector<MonitorInfo> out;

    // 走的还是 CollectAllOutputs —— 界面下拉的序号与 Init(outputIndex) 必须是
    // 同一个排列，否则界面上写"显示器 2"而实际录的是第一块屏。
    std::vector<OutputRef> all;
    CollectAllOutputs(all);

    uint32_t n = 0;
    for (const auto& r : all) {
        if (!r.attached) continue; // 拔掉的输出不该占走一个序号
        MonitorInfo m;
        m.index = n++;
        m.left = r.left;
        m.top = r.top;
        m.width = static_cast<uint32_t>(r.right > r.left ? r.right - r.left : 0);
        m.height = static_cast<uint32_t>(r.bottom > r.top ? r.bottom - r.top : 0);
        // 主显示器 = 桌面原点所在的那一块（左上角在 (0,0) 屏幕上）
        m.primary = (r.left <= 0 && r.top <= 0 && r.right > 0 && r.bottom > 0);
        m.name = "\\\\.\\DISPLAY" + std::to_string(m.index + 1);
        out.push_back(std::move(m));
    }
    return out;
}

ScreenCapture::~ScreenCapture() {
    Cleanup();
}

// 真正的清理逻辑，**不加锁**。调用方要么自己持锁，要么确定没有并发。
//
// 为什么拆成两层：Cleanup() 是公开接口，多路会话都可能从自己的线程调它
// （采集 AccessLost 之后重建），所以它必须自己加锁；而 Init() 内部要在
// 持锁状态下清场再建 —— 如果它去调 Cleanup()，就会对同一个非递归 mutex
// 二次加锁，直接死锁。所以 Init 调这个下划线版本。
void ScreenCapture::CleanupLocked() {
    if (frameAcquired_ && duplication_) {
        duplication_->ReleaseFrame();
        frameAcquired_ = false;
    }
    SafeRelease(staging_);      staging_ = nullptr;
    SafeRelease(duplication_);  duplication_ = nullptr;
    SafeRelease(context_);      context_ = nullptr;
    SafeRelease(device_);       device_ = nullptr;

    stagingW_ = stagingH_ = 0;
    stagingSrcFormat_ = DXGI_FORMAT_UNKNOWN;
    width_ = height_ = 0;
    // 关键：把 lastPixels_ 一起清掉。它指的是 staging 的映射内存，
    // 上面已经把 staging_ 释放了 —— 留着它，静止时那次「重复发布最后一帧」
    // 就是拿一个野指针去读。
    lastPixels_ = nullptr;
    lastStride_ = 0;
}

void ScreenCapture::Cleanup() {
    // 为什么连「析构 / 重建」也要加锁：多路会话共享一个采集器时，一路在
    // AccessLost 里调 Cleanup() 重建，另一路正在 Capture() 里读 staging_ ——
    // 不加锁就是标准的 use-after-free。
    std::lock_guard<std::mutex> lk(mu_);
    CleanupLocked();
}

bool ScreenCapture::Init(uint32_t outputIndex, uint32_t timeoutMs) {
    std::lock_guard<std::mutex> lk(mu_);
    CleanupLocked();
    outputIndex_ = outputIndex;
    timeoutMs_ = timeoutMs;

    // 注意：使用 D3D11_CREATE_DEVICE_SINGLETHREADED，所有对 device_/context_ 的
    // 调用都必须来自同一个线程 —— 也就是必须在 mu_ 保护下串行化。
    //
    // 这段注释原来写的是「本服务把采集固定跑在 capture 线程上」，那是错的，
    // 而且错得很危险：GUI 的多路配置**每一路都有自己的会话线程**
    // （main.cpp 里 N 个 Session，共用一个 ScreenCapture）。
    // SINGLETHREADED 只表示「D3D11 自己不加内部锁」，不表示「调用方可以多线程」。
    // 真正的线程安全由 mu_ + AcquireSnapshot 的拷贝语义提供，见头文件。
    // 保留 SINGLETHREADED 是为了省掉 D3D11 每次调用的内部加锁开销。
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };
    D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_11_0;

    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_SINGLETHREADED | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels,
        _countof(levels),
        D3D11_SDK_VERSION,
        &device_,
        &obtained,
        &context_);

    if (FAILED(hr) || !device_ || !context_) {
        CleanupLocked();
        return false;
    }

    IDXGIDevice* dxgiDevice = nullptr;
    if (FAILED(device_->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice)) || !dxgiDevice) {
        CleanupLocked();
        return false;
    }

    IDXGIAdapter* adapter = nullptr;
    hr = dxgiDevice->GetAdapter(&adapter);
    dxgiDevice->Release();
    if (FAILED(hr) || !adapter) {
        CleanupLocked();
        return false;
    }

    // 找「第 outputIndex 个显示器」。
    //
    // 关键：不能直接 adapter->EnumOutputs(outputIndex)，那样只看得到
    // **当前这块显卡上**的输出。GetAdapter() 给的是设备所在的适配器 ——
    // 在多显卡机器上（笔记本核显 + 独显、外接显卡坞、虚拟显示驱动），
    // 另一块屏接在别的适配器上，就永远枚举不到。表现出来就是"配了显示器 1
    // 却还是录到主屏"，而序号越界时干脆失败。
    //
    // 所以这里做**全局展平**：先在自己这块适配器上找，找不到就顺着
    // IDXGIFactory1 把所有适配器（含软件渲染器）的输出按顺序排一遍。
    // 排序键用 DesktopCoordinates，保证同一台机器多次枚举的顺序稳定 ——
    // 用户名下的"显示器 1"必须每次都指同一块屏，否则切一遍配置就换屏了。
    IDXGIOutput1* output1 = nullptr;
    hr = OpenNthOutput(adapter, outputIndex, &output1);
    adapter->Release();
    // OpenNthOutput 找不到时会自己退到全局枚举，所以这里只需判结果。
    if (FAILED(hr) || !output1) {
        CleanupLocked();
        return false;
    }

    hr = output1->DuplicateOutput(device_, &duplication_);
    output1->Release();
    if (FAILED(hr) || !duplication_) {
        CleanupLocked();
        return false;
    }

    DXGI_OUTDUPL_DESC desc{};
    duplication_->GetDesc(&desc);
    width_ = desc.ModeDesc.Width;
    height_ = desc.ModeDesc.Height;

    if (width_ == 0 || height_ == 0) {
        CleanupLocked();
        return false;
    }
    return true;
}

bool ScreenCapture::EnsureStaging(const D3D11_TEXTURE2D_DESC& srcDesc) {
    const bool needNew =
        (staging_ == nullptr) ||
        (stagingW_ != srcDesc.Width) ||
        (stagingH_ != srcDesc.Height) ||
        (stagingSrcFormat_ != srcDesc.Format);

    if (!needNew) return true;

    SafeRelease(staging_);
    staging_ = nullptr;

    D3D11_TEXTURE2D_DESC sd = srcDesc;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.BindFlags = 0;
    sd.MiscFlags = 0;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;

    HRESULT hr = device_->CreateTexture2D(&sd, nullptr, &staging_);
    if (FAILED(hr) || !staging_) {
        staging_ = nullptr;
        ++errorCount_;
        return false;
    }

    stagingW_ = srcDesc.Width;
    stagingH_ = srcDesc.Height;
    stagingSrcFormat_ = srcDesc.Format;
    return true;
}

CaptureStatus ScreenCapture::Capture(FrameView& out, uint8_t* buffer, size_t bufferSize) {
    out = FrameView{};

    if (!duplication_) return CaptureStatus::AccessLost;

    DXGI_OUTDUPL_FRAME_INFO info{};
    IDXGIResource* resource = nullptr;

    HRESULT hr = duplication_->AcquireNextFrame(timeoutMs_, &info, &resource);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return CaptureStatus::NoChange;
    }
    if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_SESSION_DISCONNECTED) {
        return CaptureStatus::AccessLost;
    }
    if (FAILED(hr)) {
        ++errorCount_;
        return CaptureStatus::Error;
    }

    // 从这一刻起必须调用 ReleaseFrame()
    frameAcquired_ = true;

    ID3D11Texture2D* tex = nullptr;
    hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
    resource->Release();
    if (FAILED(hr) || !tex) {
        ++errorCount_;
        ReleaseFrame();
        return CaptureStatus::Error;
    }

    D3D11_TEXTURE2D_DESC srcDesc{};
    tex->GetDesc(&srcDesc);

    if (!EnsureStaging(srcDesc)) {
        tex->Release();
        ReleaseFrame();
        return CaptureStatus::Error;
    }

    context_->CopyResource(staging_, tex);
    tex->Release();

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context_->Map(staging_, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        ++errorCount_;
        ReleaseFrame();
        return CaptureStatus::Error;
    }

    out.pixels = static_cast<const uint8_t*>(mapped.pData);
    out.width = srcDesc.Width;
    out.height = srcDesc.Height;
    out.stride = mapped.RowPitch;   // 注意：行间距可能 > width*4，调用方必须按行拷贝
    out.accumulatedFrames = info.AccumulatedFrames;

    // 记下这块 staging 内存：桌面静止时要用它重复发布最后那一帧。
    // staging 纹理在下次成功采集（或分辨率变化重建）之前不会被改写，
    // 所以这个指针在中间这段时间一直有效。
    lastPixels_ = out.pixels;
    lastStride_ = out.stride;

    // LastPresentTime 是 QPC 单位；为 0 表示 DXGI 没有给出时间戳
    if (info.LastPresentTime.QuadPart != 0) {
        out.presentQpc = info.LastPresentTime.QuadPart;
    }

    // ---- 把像素拷进调用方自己的缓冲 --------------------------------------
    //
    // 上面的 out.pixels 指向 staging 的映射内存，**下一路会话一进来就会把它盖掉**。
    // 多路共享一个采集器时这就是花屏/撕裂的根源，所以必须在这里同步拷走。
    //
    // 统一压成紧密排布（stride = width*4）：源的行距 RowPitch 常常比 width*4 大
    // （D3D 按 256 字节对齐），下游要是照 RowPitch 去算会得到一条斜掉的画面。
    const uint32_t w = srcDesc.Width;
    const uint32_t h = srcDesc.Height;
    const size_t need = static_cast<size_t>(w) * 4u * h;
    if (!buffer || bufferSize < need) {
        context_->Unmap(staging_, 0);
        lastPixels_ = nullptr;
        lastStride_ = 0;
        ReleaseFrame();
        return CaptureStatus::Error;
    }
    const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
    for (uint32_t y = 0; y < h; ++y) {
        std::memcpy(buffer + static_cast<size_t>(y) * w * 4u, src + static_cast<size_t>(y) * mapped.RowPitch,
                    static_cast<size_t>(w) * 4u);
    }
    context_->Unmap(staging_, 0);

    // staging 已经 Unmap 了，lastPixels_ 不能再留着 —— 它的生命周期就到这里。
    lastPixels_ = nullptr;
    lastStride_ = 0;

    // 让 out 指回调用方那块刚拷好的内存，语义变成「调用方持有」。
    out.pixels = buffer;
    out.stride = w * 4u;

    ReleaseFrame();
    return CaptureStatus::Ok;
}

CaptureStatus ScreenCapture::CaptureInto(FrameView& out, std::vector<uint8_t>& buffer) {
    const uint32_t w = width_;
    const uint32_t h = height_;
    const size_t need = static_cast<size_t>(w) * 4u * h;
    if (need == 0) return CaptureStatus::Error;
    if (buffer.size() < need) buffer.resize(need);

    const CaptureStatus cs = Capture(out, buffer.data(), buffer.size());
    if (cs != CaptureStatus::Ok) return cs;

    // 刚拷进 buffer 的像素和"最近一张"内容完全一致，所以直接把所有权换过来用，
    // 不要再拷一遍。buffer 拿回 lastFrame_ 原来的内存继续当工作缓冲 —— 下一拍
    // 会往它里面写新像素，而那时 lastFrame_ 已经指向这一拍的像素了。
    //
    // 这一项在 2560x1440 下每拍省 14.7MB 读写；三路 60fps 就是 5.3GB/s。
    SwapLastFrame(buffer, out.width, out.height, out.presentQpc);

    out.pixels = lastFrame_.data(); // 内容就是 buffer 刚拷进去的那份
    out.stride = out.width * 4u;
    return CaptureStatus::Ok;
}

void ScreenCapture::SwapLastFrame(std::vector<uint8_t>& buffer, uint32_t w, uint32_t h, int64_t qpc) {
    // 只交换存储，不搬字节。交换完两者内容互换，但 buffer 的内容此刻**就是**
    // 我们要留存的那张，所以 lastFrame_ 得到正确内容、buffer 得到可复用的内存。
    lastFrame_.swap(buffer);
    lastW_ = w;
    lastH_ = h;
    lastEpochQpc_ = qpc;
    ++epoch_;
}

CaptureStatus ScreenCapture::CaptureSharedInto(FrameView& out, std::vector<uint8_t>& buffer,
                                               uint64_t& seenEpoch) {
    out = FrameView{};

    // 与 CaptureInto 一样，**不加锁** —— 调用方必须已经持有 Mutex()。
    //
    // 踩过的坑：这里原本自己加了一次 mu_，而 Session 那边本来就已经拿着
    // capture_->Mutex() 了，于是同一个非递归 mutex 被同一线程锁两次 ——
    // 直接死锁，表现是服务起来了、日志停在 service_started，
    // 之后一条状态都不再输出（连 ffmpeg 都没开始写分片）。
    //
    // 判 epoch、读 lastFrame_、采新帧必须是一个原子操作（否则两路会同时
    // 判定"没有新帧"然后一起冲进 duplication 抢锁），这个互斥由调用方那把
    // 锁负责。

    // ---- 已经有人替本路采过了 --------------------------------------------
    //
    // epoch_ 只在采集**成功**时推进（SwapLastFrame），所以它同时是
    // "最近一张"的版本号。本路看过的版本和它不一样，就说明这一帧本路还没拿过。
    if (epoch_ != 0 && epoch_ != seenEpoch && !lastFrame_.empty()) {
        const uint32_t w = lastW_;
        const uint32_t h = lastH_;
        const size_t need = static_cast<size_t>(w) * 4u * h;
        if (need == 0) return CaptureStatus::Error;
        if (buffer.size() < need) buffer.resize(need);

        // 必须是 memcpy，**不能**像 CaptureInto 那样 swap：
        // swap 会把 lastFrame_ 换成本路的旧缓冲，下一个来吃现成的路就吃到垃圾。
        // 这里是只读共享，代价只是一次整屏拷贝 —— 相比它省掉的
        // AcquireNextFrame + CopyResource + Map/Unmap（GPU 往返 + 驱动同步），
        // 这笔账是划算的，而且同源路数不再互相拖累。
        std::memcpy(buffer.data(), lastFrame_.data(), need);

        seenEpoch = epoch_;
        out.pixels = buffer.data();
        out.width = w;
        out.height = h;
        out.stride = w * 4u;
        out.presentQpc = lastEpochQpc_;
        return CaptureStatus::Ok;
    }

    // ---- 没有更新的帧：本路自己去采 --------------------------------------
    //
    // 两种结果都可能：真采到一帧新的（epoch 推进，其他路会来吃），
    // 或者超时（NoChange，桌面这一拍真的没动）。
    const uint32_t needW = width_;
    const uint32_t needH = height_;
    const size_t need = static_cast<size_t>(needW) * 4u * needH;
    if (need == 0) return CaptureStatus::Error;
    if (buffer.size() < need) buffer.resize(need);

    const CaptureStatus cs = Capture(out, buffer.data(), buffer.size());
    if (cs != CaptureStatus::Ok) return cs;

    SwapLastFrame(buffer, out.width, out.height, out.presentQpc);
    // 本路现在拥有的就是最新那一版，记下来，下一拍才不会又去采一次。
    seenEpoch = epoch_;
    out.pixels = lastFrame_.data();
    out.stride = out.width * 4u;
    return CaptureStatus::Ok;
}

bool ScreenCapture::AcquireSnapshot(uint8_t* outBuffer, size_t outBufferSize, uint32_t& outW,
                                    uint32_t& outH, int64_t& outPresentQpc, bool& outIsNew) {
    outIsNew = false;
    // 整段都在锁里：这一路读完之前，别的会话线程不能动 staging_。
    std::lock_guard<std::mutex> lk(mu_);

    FrameView fv;
    const CaptureStatus cs = Capture(fv, outBuffer, outBufferSize);

    if (cs == CaptureStatus::Ok) {
        outW = fv.width;
        outH = fv.height;
        outPresentQpc = fv.presentQpc;
        outIsNew = true; // 这一拍桌面真的动了
        return true;
    }

    if (cs == CaptureStatus::NoChange) {
        // 桌面这一拍没有新内容。这不代表「没有帧」—— 采集器手里还留着上一张，
        // 而**每一路**会话都需要拿到属于自己的一份拷贝（不能共享指针）。
        // 所以这里把上一张重放一遍。
        return ReplayLastLocked(outBuffer, outBufferSize, outW, outH, outPresentQpc, outIsNew);
    }

    // AccessLost / Error：把上一次的尺寸原样带出去，由调用方决定要不要重建。
    outW = lastW_;
    outH = lastH_;
    outPresentQpc = lastEpochQpc_;
    return false;
}

bool ScreenCapture::AcquireReplayLocked(uint8_t* outBuffer, size_t outBufferSize, uint32_t& outW,
                                        uint32_t& outH, int64_t& outPresentQpc) {
    bool isNew = false;
    return ReplayLastLocked(outBuffer, outBufferSize, outW, outH, outPresentQpc, isNew);
}

bool ScreenCapture::ReplayLastLocked(uint8_t* outBuffer, size_t outBufferSize, uint32_t& outW,
                                     uint32_t& outH, int64_t& outPresentQpc, bool& outIsNew) {
    const uint32_t w = lastW_;
    const uint32_t h = lastH_;
    if (w == 0 || h == 0) return false;
    const size_t need = static_cast<size_t>(w) * 4u * h;
    if (!outBuffer || outBufferSize < need) return false;
    if (!lastFrame_.empty()) {
        // 这里没法省：调用方的缓冲和 lastFrame_ 是两块不同的内存，而重放要
        // 让**它**拿到内容。不过 NoChange 只在桌面真的静止时发生，不是稳态。
        std::memcpy(outBuffer, lastFrame_.data(), need);
    }
    outW = w;
    outH = h;
    outPresentQpc = lastEpochQpc_;
    outIsNew = false; // 是重复帧，不是新内容
    return true;
}

void ScreenCapture::ReleaseFrame() {
    if (frameAcquired_ && duplication_) {
        // Map 之后必须先 Unmap 才能 ReleaseFrame
        context_->Unmap(staging_, 0);
        duplication_->ReleaseFrame();
        frameAcquired_ = false;
    }
}

void ScreenCapture::ReleaseFrameKeepLast() {
    // 和 ReleaseFrame 的区别：保留 lastPixels_/lastStride_。
    // Unmap 之后那块内存的内容本身还在（staging 纹理还活着），
    // 只是「映射」这个动作结束了 —— 所以重复发布仍然能读到正确的像素。
    ReleaseFrame();
}

} // namespace shadow
