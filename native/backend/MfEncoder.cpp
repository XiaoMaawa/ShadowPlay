// MfEncoder.cpp —— 见头文件里的设计与"为什么"。
#include "MfEncoder.h"

#include <windows.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <codecapi.h>
#include <mferror.h>
#include <emmintrin.h>  // SSE2：non-temporal 存储

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

#include "Utf8Path.h" // FromUtf8 / ToUtf8 / PathToUtf8 / PathFromUtf8

namespace shadow {
namespace fs = std::filesystem;

namespace {

/**
 * MF 的进程级引用计数。
 *
 * 为什么要计数：服务的每个录制线程、导出线程都要用 MF。MFStartup 可以
 * 多次调用（本身是引用计数的），但 CoInitializeEx 配对的 CoUninitialize
 * 必须同线程成对 —— 所以这里只在**第一次**真正 Startup，最后一次才
 * Shutdown。用 atomic 而不是 mutex：Acquire 会在每个导出线程里调，
 * 不值得为它抢锁。
 */
std::atomic<int> g_mfRefs{0};
std::mutex       g_mfMu;

/**
 * 本线程的 MF 记账。
 *
 * 【必须放在命名空间作用域，不能写成函数内的 thread_local】
 * 函数内的 thread_local 是**每个函数各自一个变量**：Acquire 里的 tRefs 和
 * Release 里的 tRefs 是两个完全无关的线程局部对象。写成那样的话 Release
 * 永远读到 0、直接把套间和 MFShutdown 全跳过（我第一版就是这么错的）。
 *
 * refs：本线程嵌套层数，0 → 1 时才真做 CoInitializeEx。
 * coOwned：本线程这一份套间是不是我们初始化的（RPC_E_CHANGED_MODE 时不是，
 * 那就不许 CoUninitialize，否则会把别人的套间关掉）。
 */
struct MfThreadState {
    int  refs = 0;
    bool coOwned = false;
};
thread_local MfThreadState g_tls;

/**
 * 进程级 MF 生命周期（MFStartup / MFShutdown）。
 *
 * 和线程套间**分开记账**是有原因的：一个 MfEncoder 可能在会话线程 Start()、
 * 在喂帧线程 Write()、在停止线程 Finish()。如果只按线程记 MFStartup，那么
 * Start 在 A 线程加、Finish 在 B 线程减，两边计数对不上：要么 B 线程减到 0
 * 时把 MF 关了（A 线程那边 SinkWriter 还活着，直接用崩溃），要么永远不关。
 *
 * 所以进程级按**对象生命周期**引用计数，线程套间按**每次调用**进出配对。
 */
void MfProcessRetain() {
    std::lock_guard<std::mutex> lk(g_mfMu);
    if (g_mfRefs.load() == 0) {
        MFStartup(MF_VERSION, MFSTARTUP_FULL); // 失败就让下面的对象创建去报错
    }
    g_mfRefs.fetch_add(1);
}

void MfProcessRelease() {
    std::lock_guard<std::mutex> lk(g_mfMu);
    if (g_mfRefs.load() == 0) return;
    if (g_mfRefs.fetch_sub(1) == 1) MFShutdown();
}

/** 进线程套间（嵌套计数）。返回本线程是否可用 MF。 */
bool MfThreadEnter() {
    if (g_tls.refs++ > 0) return true;
    // COINIT_MULTITHREADED：服务的喂帧/导出都是自己的工作线程，
    // 单线程套间（STA）会要求消息泵，我们没有。
    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hrCo)) {
        g_tls.coOwned = true; // S_OK / S_FALSE 都持有一份引用，要配对还回去
        return true;
    }
    --g_tls.refs;
    // RPC_E_CHANGED_MODE：这个线程已经用别的模式初始化过套间。不是致命错误
    // （MF 的 MTA 对象仍能用），此时不许 CoUninitialize，否则会把别人的
    // 套间关掉 —— 但我们这一层仍然算"能干活"。
    if (hrCo == RPC_E_CHANGED_MODE) {
        ++g_tls.refs;
        g_tls.coOwned = false;
        return true;
    }
    return false;
}

/** 出线程套间。与 MfThreadEnter 严格配对。 */
void MfThreadLeave() {
    if (g_tls.refs <= 0) return;
    if (--g_tls.refs > 0) return;
    if (g_tls.coOwned) {
        CoUninitialize();
        g_tls.coOwned = false;
    }
}

/** 一次调用内的 RAII：保证异常/提前 return 也能配对退出。 */
struct MfThreadScope {
    bool ok;
    MfThreadScope() : ok(MfThreadEnter()) {}
    ~MfThreadScope() { MfThreadLeave(); }
    MfThreadScope(const MfThreadScope&) = delete;
    MfThreadScope& operator=(const MfThreadScope&) = delete;
};

// ---------------------------------------------------------------------------
// 全进程共用的 D3D11 设备
//
// 见头文件里 MfSharedDeviceReady 的注释：不共用的话，每切一个分片 SinkWriter
// 都要新造一份设备交给硬件编码 MFT，实测 +35 个句柄/片且永不回收。
//
// 这个设备**只给编码器用**，不碰采集那条线（ScreenCapture 有自己的一套设备，
// 两边各自独立，互不影响 —— 采集端那份还牵着 IDXGIOutputDuplication）。
// ---------------------------------------------------------------------------
std::mutex            g_devMu;
void*                 g_devRaw = nullptr;   // ID3D11Device*
void*                 g_mgrRaw = nullptr;   // IMFDXGIDeviceManager*
int                   g_devState = 0;       // 0=没试过 1=就绪 -1=失败

bool CreateSharedDeviceLocked() {
    if (g_devState != 0) return g_devState > 0;

    IMFDXGIDeviceManager* mgr = nullptr;
    UINT resetToken = 0;
    HRESULT hr = MFCreateDXGIDeviceManager(&resetToken, &mgr);
    if (FAILED(hr) || !mgr) {
        g_devState = -1;
        return false;
    }

    // BGRA_SUPPORT 是 NVENC/QSV 的 MFT 的硬要求；VIDEO_SUPPORT 让它能建
    // 视频处理器（BGRA→NV12 转换走它，比 CPU 转快一个数量级）。
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    ID3D11Device* dev = nullptr;
    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
    const char* const kModules[] = {"d3d11.dll", "dxgi.dll"};
    for (const char* mod : kModules) {
        (void)mod;
    }
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                           static_cast<UINT>(sizeof(levels) / sizeof(levels[0])), D3D11_SDK_VERSION,
                           &dev, &got, nullptr);
    if (FAILED(hr) || !dev) {
        // 硬件设备起不来就不要退回 WARP：WARP 跑硬件编码 MFT 一样建不了，
        // 而且会把帧拷到 CPU 再拷回去，比"没有共用设备"更糟。
        mgr->Release();
        g_devState = -1;
        return false;
    }

    hr = mgr->ResetDevice(dev, resetToken);
    if (FAILED(hr)) {
        dev->Release();
        mgr->Release();
        g_devState = -1;
        return false;
    }

    g_devRaw = dev;
    g_mgrRaw = mgr;
    g_devState = 1;
    return true;
}

} // namespace

bool MfSharedDeviceReady() {
    std::lock_guard<std::mutex> lk(g_devMu);
    return CreateSharedDeviceLocked();
}

void MfApplySharedDevice(void* attributes) {
    if (!attributes) return;
    IMFAttributes* attrs = static_cast<IMFAttributes*>(attributes);
    {
        std::lock_guard<std::mutex> lk(g_devMu);
        if (!CreateSharedDeviceLocked()) return;
        attrs->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, static_cast<IUnknown*>(g_mgrRaw));
    }
}

namespace {

/** 把 UTF-8 路径转成 MF 要的宽字符。 */
std::wstring ToWide(const std::string& s) { return FromUtf8(s); }

/**
 * 把 HRESULT 写成能看懂的样子：十六进制 + 已知含义 + 十进制。
 *
 * 为什么要这样：MF 的错误以前被我打成 `std::to_string((unsigned long)hr)`，
 * 报出来是 "hr=0x2147746288" 这种十进制串 —— 对着这个数去查 CO_E_NOTINITIALIZED
 * 得自己先做进制换算，白耽误了半小时。十六进制和文档/搜索一致，直接能搜。
 */
std::string HrText(HRESULT hr) {
    char buf[160];
    const unsigned long u = static_cast<unsigned long>(hr);
    const char* name = nullptr;
    switch (static_cast<unsigned int>(hr)) {
        case 0x800401F0u: name = "CO_E_NOTINITIALIZED（线程没有 COM 套间）"; break;
        case 0xC00D36B4u: name = "MF_E_INVALIDMEDIATYPE（媒体类型不被接受）"; break;
        case 0xC00D36B3u: name = "MF_E_INVALIDSTREAMNUMBER"; break;
        case 0xC00D6D60u: name = "MF_E_TRANSFORM_TYPE_NOT_SET（要先 SetOutputType 再 SetInputType）"; break;
        case 0xC00D3E85u: name = "MF_E_INVALIDREQUEST"; break;
        case 0xC00D4650u: name = "MF_E_UNSUPPORTED_BYTESTREAM_TYPE"; break;
        case 0x80070005u: name = "E_ACCESSDENIED（文件被占用或没权限）"; break;
        case 0x8007000Eu: name = "E_OUTOFMEMORY"; break;
        case 0x80070020u: name = "ERROR_SHARING_VIOLATION（文件正被别的进程占用）"; break;
        case 0x80004005u: name = "E_FAIL"; break;
        case 0x80070057u: name = "E_INVALIDARG"; break;
        default: break;
    }
    if (name) {
        std::snprintf(buf, sizeof(buf), "0x%08lX %s", u, name);
    } else {
        std::snprintf(buf, sizeof(buf), "0x%08lX", u);
    }
    return buf;
}

HRESULT MakeH264OutType(uint32_t w, uint32_t h, uint32_t fps, uint32_t bitrate,
                        IMFMediaType** out) {
    IMFMediaType* t = nullptr;
    HRESULT hr = MFCreateMediaType(&t);
    if (FAILED(hr)) return hr;

    hr = t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    // High profile：屏幕录制是"大片平坦色块 + 文字边缘"，High 的 8x8 变换
    // 和更细的量化明显更省码率，而解码端（任何一台能跑 Win10 的机器）都支持。
    if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(t, MF_MT_FRAME_SIZE, w, h);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(t, MF_MT_FRAME_RATE, fps, 1);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(t, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    if (FAILED(hr)) {
        t->Release();
        return hr;
    }
    *out = t;
    return S_OK;
}

/**
 * 输入类型：NV12。
 *
 * 【为什么不再用 RGB32/BGRA 直喂】见 MfEncoder.h 里 nv12Buf_ 的注释。实测
 * 自转 NV12 后 CPU 从 3.79~6.82 核降到 0.80~0.88 核，颜色还更准。
 *
 * 【为什么 SinkWriter 自己转那么贵】没有 D3D manager 时它只能用 msvproc.dll
 * 在 CPU 上做 BGRA→NV12，那一层既慢又抖。
 *
 * 【stride】NV12 的 Y 平面行距 = width（无对齐填充），这是 MF 的默认期望，
 * 所以不给 MF_MT_DEFAULT_STRIDE 也对；显式给正数更保险（BGRA 那边必须给，
 * 否则 RGB32 默认 bottom-up，画面会上下颠倒；NV12 没有这个歧义，但一致些好）。
 */
HRESULT MakeNv12InType(uint32_t w, uint32_t h, uint32_t fps, IMFMediaType** out) {
    IMFMediaType* t = nullptr;
    HRESULT hr = MFCreateMediaType(&t);
    if (FAILED(hr)) return hr;

    hr = t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_DEFAULT_STRIDE, w);
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(t, MF_MT_FRAME_SIZE, w, h);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(t, MF_MT_FRAME_RATE, fps, 1);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(t, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    if (FAILED(hr)) {
        t->Release();
        return hr;
    }
    *out = t;
    return S_OK;
}

/**
 * 输入类型：RGB32（就是 BGRA 的内存序）。
 *
 * 保留它只是为了探针对照（native/probe_opt.cpp 的 bgra 用例），生产路径
 * 已经换成 NV12 —— 见 MakeNv12InType 上面的注释。
 */
HRESULT MakeBgraInType(uint32_t w, uint32_t h, uint32_t fps, IMFMediaType** out) {
    IMFMediaType* t = nullptr;
    HRESULT hr = MFCreateMediaType(&t);
    if (FAILED(hr)) return hr;

    hr = t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    // 【负 stride】RGB32 在 MF 里默认是**自下而上**（bottom-up，stride 为负）。
    // 我们的采集数据是自上而下的标准顺序，所以这里显式给正的 stride，
    // 免得 SinkWriter 把画面上下颠倒。
    if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_DEFAULT_STRIDE, w * 4);
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(t, MF_MT_FRAME_SIZE, w, h);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(t, MF_MT_FRAME_RATE, fps, 1);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(t, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    if (FAILED(hr)) {
        t->Release();
        return hr;
    }
    *out = t;
    return S_OK;
}

/** CSD（SPS/PPS）要不要单独塞给 SinkWriter 的 out type。**不要**。
 *
 *  踩过的坑：探针里纠结过"fMP4 分片要不要手动带 SPS/PPS"。结论是不需要：
 *  SinkWriter 写 fMP4 时会把编码器吐的 CSD 塞进 moov 的 avcC 里，每个分片
 *  自成一体，SourceReader 打开单片就能拿到完整的解码参数。手动再塞一遍
 *  反而会让某些编码器报 MF_E_INVALIDMEDIATYPE。
 */

/**
 * BGRA → NV12（BT.601 有限范围，与 msvproc 默认一致）。
 *
 * 见 MfEncoder.h 里 nv12Buf_ 的注释：这一步是"自己转 0.8 核 vs 让 SinkWriter
 * 转 3.8~6.8 核"的差别，而且颜色更准。
 *
 * 布局：先是 height 行 Y（每行 width 字节），然后 (height/2) 行 UV 交错
 * （每行 width 字节，U0 V0 U1 V1 …）。
 *
 * 【为什么不用非临时存储】Y 平面写完立刻被编码器读（要算色度、要交给 NVENC
 * 上传），绕缓存反而让它每次都得回内存取。实测 probe_copy_cpu 里
 * "ntsse only" 比 "memcpy only" 并不更快，而 ntsse + 写样本反而慢一倍。
 */
void ConvertBgraToNv12Impl(const uint8_t* src, uint32_t stride, uint32_t width,
                           uint32_t height, uint8_t* dst) {
    if (!src || !dst || width == 0 || height == 0) return;

    uint8_t* yPlane = dst;
    uint8_t* uvPlane = dst + static_cast<size_t>(width) * height;

    // Y = ((66R + 129G + 25B + 128) >> 8) + 16
    const __m128i zero = _mm_setzero_si128();
    const __m128i cR = _mm_set1_epi16(66);
    const __m128i cG = _mm_set1_epi16(129);
    const __m128i cB = _mm_set1_epi16(25);
    const __m128i mask = _mm_set1_epi32(0x000000FF);
    const __m128i bias = _mm_set1_epi16(16);

    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* s = src + static_cast<size_t>(y) * stride;
        uint8_t* d = yPlane + static_cast<size_t>(y) * width;
        uint32_t x = 0;
        // 一次 4 像素（16 字节 = 4 个 BGRA）
        for (; x + 4 <= width; x += 4) {
            const __m128i px =
                _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + static_cast<size_t>(x) * 4u));
            const __m128i b = _mm_and_si128(px, mask);
            const __m128i g = _mm_and_si128(_mm_srli_epi32(px, 8), mask);
            const __m128i r = _mm_and_si128(_mm_srli_epi32(px, 16), mask);
            // packs_epi32 把 4 个 32 位压成 4 个 16 位（后 4 个是 zero）
            __m128i acc = _mm_mullo_epi16(_mm_packs_epi32(b, zero), cB);
            acc = _mm_add_epi16(acc, _mm_mullo_epi16(_mm_packs_epi32(g, zero), cG));
            acc = _mm_add_epi16(acc, _mm_mullo_epi16(_mm_packs_epi32(r, zero), cR));
            acc = _mm_srli_epi16(acc, 8);
            acc = _mm_add_epi16(acc, bias);
            const __m128i packed = _mm_packus_epi16(acc, acc);
            _mm_storel_epi64(reinterpret_cast<__m128i*>(d + x), packed);
        }
        for (; x < width; ++x) {
            const uint8_t* p = s + static_cast<size_t>(x) * 4u;
            d[x] = static_cast<uint8_t>(((66 * p[2] + 129 * p[1] + 25 * p[0] + 128) >> 8) + 16);
        }
    }

    // 色度：2x2 取平均。
    // U = ((-38R - 74G + 112B + 128) >> 8) + 128
    // V = ((112R - 94G - 18B + 128) >> 8) + 128
    for (uint32_t y = 0; y + 1 < height; y += 2) {
        const uint8_t* s0 = src + static_cast<size_t>(y) * stride;
        const uint8_t* s1 = s0 + stride;
        uint8_t* d = uvPlane + static_cast<size_t>(y / 2) * width;
        for (uint32_t x = 0; x + 1 < width; x += 2) {
            const uint8_t* p00 = s0 + static_cast<size_t>(x) * 4u;
            const uint8_t* p01 = p00 + 4u;
            const uint8_t* p10 = s1 + static_cast<size_t>(x) * 4u;
            const uint8_t* p11 = p10 + 4u;
            const int b = (p00[0] + p01[0] + p10[0] + p11[0]) >> 2;
            const int g = (p00[1] + p01[1] + p10[1] + p11[1]) >> 2;
            const int r = (p00[2] + p01[2] + p10[2] + p11[2]) >> 2;
            int u = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
            int v = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
            d[x] = static_cast<uint8_t>(u < 0 ? 0 : (u > 255 ? 255 : u));
            d[x + 1] = static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }
}

/**
 * 把一帧 BGRA 拷进编码器样本缓冲，用非临时存储绕开缓存。
 *
 * 为什么不用 memcpy：1440p 一帧 14.7MB，144fps 下 2.1GB/s。走普通存储
 * 会把 L3 全冲掉（采集线程刚写进来的下一帧立刻被挤出去），而这份数据
 * 拷完之后只被编码器读一次。non-temporal 写入直达内存、不经过缓存，
 * 同时保住采集端的缓存命中率。
 *
 * srcStride 允许大于 rowBytes（DXGI staging 按 256 字节对齐），此时逐行拷；
 * 相等就整块拷。
 */
void CopyFrameStreaming(uint8_t* dst, const uint8_t* src, uint32_t rowBytes, uint32_t height,
                        uint32_t srcStride) {
    auto blit = [](uint8_t* d, const uint8_t* s, size_t bytes) {
        const size_t misalign = (16u - (reinterpret_cast<uintptr_t>(d) & 15u)) & 15u;
        const size_t head = misalign < bytes ? misalign : bytes;
        if (head) {
            std::memcpy(d, s, head);
            d += head;
            s += head;
            bytes -= head;
        }
        while (bytes >= 16) {
            const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s));
            _mm_stream_si128(reinterpret_cast<__m128i*>(d), v);
            d += 16;
            s += 16;
            bytes -= 16;
        }
        if (bytes) std::memcpy(d, s, bytes);
    };

    if (srcStride == rowBytes) {
        blit(dst, src, static_cast<size_t>(rowBytes) * height);
    } else {
        // 行距不一致：逐行拷。每行起点 = dst + y*rowBytes，rowBytes 是 4 的
        // 倍数但不一定是 16 的倍数，所以每行都要重新对齐检查。
        for (uint32_t y = 0; y < height; ++y) {
            blit(dst + static_cast<size_t>(y) * rowBytes,
                 src + static_cast<size_t>(y) * srcStride, rowBytes);
        }
    }
    _mm_sfence(); // stream 写入必须 sfence 之后才对其他路径可见
}

/**
 * 【已删除】ScaleRowNearestBg（最近邻横向缩放一行）。
 *
 * 过渡帧曾经用等比缩小来"一个像素都不丢地"塞进当前画布，但主人明确否决：
 * 「不能缩放」（m08329）。缩放会让导出端"按窗口内最大尺寸作画布"的语义变含混
 * —— 分片里存的到底是不是 1:1 像素就没法保证了。
 *
 * 现在的做法是相反的：帧比画布大就**立刻切一片**（新片按新尺寸开，画布就是
 * 精确尺寸，见 MfEncoder.h 的 kTransitionFactor 注释），一像素都不裁也不缩。
 */

} // namespace

// ---------------------------------------------------------------------------
// MF 生命周期
// ---------------------------------------------------------------------------

void MfEncoder::ConvertToNv12(const uint8_t* src, uint32_t stride, uint32_t width,
                              uint32_t height, uint8_t* dst) {
    ConvertBgraToNv12Impl(src, stride, width, height, dst);
}

bool MfAcquire() {
    // 公开的粗粒度接口：既要进程级 Startup，也要本线程套间。
    //
    // 踩过两次，都记在这里：
    //   ① 一开始只在"第一个调 MfAcquire 的线程"里做 CoInitializeEx，
    //      结果录制喂帧线程（SegmentRecorder 自己起的）建 SinkWriter 时
    //      拿到 hr=0x800401F0（CO_E_NOTINITIALIZED）—— 线程没有套间，
    //      COM 对象创建全部失败，而 MFStartup 明明成功了。
    //   ② 更隐蔽的一次：MfEncoder::Start() 压根没调 MfAcquire，只有
    //      main.cpp 的能力自检调过一次。自检线程的套间是好的、自检报
    //      ok:true，但真正干活的会话线程完全没有套间 —— 报错信息还误导
    //      成"系统可能缺 Media Feature Pack"。
    //
    // 结论：MfEncoder 的每个入口都必须自己进套间（见 MfThreadScope），
    // 不能指望"有人已经初始化过"。
    if (!MfThreadEnter()) return false;
    MfProcessRetain();
    return true;
}

void MfRelease() {
    MfProcessRelease();
    MfThreadLeave();
}

// ---------------------------------------------------------------------------
// MfEncoder
// ---------------------------------------------------------------------------

MfEncoder::MfEncoder() = default;

MfEncoder::~MfEncoder() { Finish(); }
std::string MfEncoder::ListFilePath() const {
    if (dir_.empty()) return {};
    return PathToUtf8(fs::u8path(dir_) / fs::u8path("index.csv"));
}

bool MfEncoder::Start(const std::string& dir, uint32_t width, uint32_t height, uint32_t fps,
                      uint32_t bitrateMbps, uint32_t segmentSeconds, uint32_t keepSegments) {
    // 【关键】这个对象的生命周期内一直持有 MF 引用。
    //
    // 之前这里什么都没做，靠 main.cpp 的能力自检"顺手"Startup 过 —— 而
    // 自检跑在另一个线程，那个线程的 COM 套间是好的，真正干活的会话线程
    // 却没有套间，于是建 SinkWriter 报 hr=0x800401F0（CO_E_NOTINITIALIZED），
    // 还被误译成"系统可能缺 Media Feature Pack"。
    //
    // 现在自己管：进程级引用到此为止（Finish 时释放），线程套间则每次调用
    // 进出（下面用 MfThreadScope）。
    if (!retained_) {
        MfProcessRetain();
        retained_ = true;
    }
    MfThreadScope threadScope;
    if (!threadScope.ok) {
        status_.failed = true;
        status_.detail = "初始化 COM 套间失败，这个线程没法用 Media Foundation";
        return false;
    }

    dir_ = dir;
    width_ = width;
    height_ = height;
    fps_ = fps ? fps : 60;
    if (fps_ > 240) fps_ = 240;
    // 记下用户真正要的帧率：StartWithFpsFallback 可能会把 fps_ 降下来，
    // 界面上的说明要写「已从 X 帧降到 Y 帧」。
    requestedFps_ = fps_;
    segmentSeconds_ = segmentSeconds ? segmentSeconds : 2;
    keepSegments_ = keepSegments < 2 ? 2 : keepSegments;

    if (width_ == 0 || height_ == 0 || dir_.empty()) {
        status_.failed = true;
        status_.detail = "录制参数不全（目录或分辨率是空的）";
        return false;
    }
    // H.264 的编码宏块是 16x16，宽高必须是偶数（奇数列会缺一条，某些
    // 解码器直接花屏）。采集端给的分辨率一般已经是偶数，这里兜一下底：
    // 向下取偶，比向上取偶安全（不会越界读源数据）。
    if (width_ & 1u) --width_;
    if (height_ & 1u) --height_;

    if (bitrateMbps == 0) {
        // 没配码率时按分辨率估：像素数 × 每像素 0.15 bit × 帧率。
        // 1080p60 大约是 18.7 Mbps，和常见录屏默认值（20）接近。
        const double px = static_cast<double>(width_) * height_;
        bitrateMbps = static_cast<uint32_t>(px * 0.15 * fps_ / 1e6 + 0.5);
        if (bitrateMbps < 2) bitrateMbps = 2;
        if (bitrateMbps > 100) bitrateMbps = 100;
    }
    bitrateBps_ = bitrateMbps * 1000000u;

    segmentFrames_ = fps_ * segmentSeconds_;
    if (segmentFrames_ < fps_) segmentFrames_ = fps_; // 每片至少 1 秒

    std::error_code ec;
    fs::create_directories(fs::u8path(dir_), ec);
    if (ec) {
        status_.failed = true;
        status_.detail = "建不了分片目录：" + ec.message();
        return false;
    }

    // 清掉这个目录里上一轮的残留分片和清单。
    //
    // 为什么必须清：槽位循环复写是**覆盖写**，不会先截断。上一轮这个槽位
    // 有 6.4MB、这一轮只写了 2MB，文件大小就还是 6.4MB，磁盘统计凭空翻几倍
    // （ffmpeg 时代实测过：120s 窗口显示 356MB，按码率算只该 120MB）。
    // index.csv 也一样，不删就会跨轮次累积时间轴。
    {
        std::error_code ec2;
        for (const auto& e : fs::directory_iterator(fs::u8path(dir_), ec2)) {
            std::error_code ec3;
            if (!e.is_regular_file(ec3)) continue;
            const std::string ext = e.path().extension().u8string();
            if (ext == u8".mp4") fs::remove(e.path(), ec3);
        }
        fs::remove(fs::u8path(ListFilePath()), ec2);
    }

    // 槽位数按"标称片数的 8 倍"给，不按片数本身给。见 MfEncoder.h 里 keepSlots_
    // 的注释：片不是等长的（尺寸一变就切），只按 keepSegments_ 算会让过渡片
    // 把槽位吃光 —— 20 秒窗口里只剩一两秒真实内容，导出几乎全程 NO SIGNAL。
    //
    // 8 倍 = 20 秒窗口里平均每片 0.25 秒还有余量。真超了也不丢新内容，
    // PurgeExpired() 会按时间从最旧的开始扔，磁盘上限始终等于回放窗口的内容量。
    {
        const uint64_t wanted = static_cast<uint64_t>(keepSegments_) * 8u;
        keepSlots_ = wanted > kMaxSlots ? kMaxSlots : static_cast<uint32_t>(wanted);
        if (keepSlots_ < 2) keepSlots_ = 2;
    }

    segmentSeq_ = 0;
    framesInSegment_ = 0;
    slotInUse_ = 0;
    segContentW_ = 0;
    segContentH_ = 0;
    lastResizeMs_ = 0;
    pendingW_ = 0;
    pendingH_ = 0;
    // 顺延链必须清零：这是新一次录制，第一片要用真墙钟（见 MfEncoder.h
    // nextSegStartUnix_ 的注释）。留着上一轮的值会让新录像的时间轴从
    // 上一轮结束那一刻接着排 —— 中间的停机时间被无声吞掉。
    nextSegStartUnix_ = 0;
    done_.clear();
    doneSeconds_ = 0.0;
    status_.segmentsWritten = 0;
    status_.framesWritten = 0;

    // 【帧率可能被自动降下来】硬件编码器对「分辨率 × 帧率」有吞吐上限，
    // 超了会拒绝输入类型。见 StartWithFpsFallback() 与 MfEncoder.h 里
    // EffectiveFps() 的注释。这里只是入口，具体降档在那边做。
    if (!StartWithFpsFallback()) return false;

    status_.running = true;
    status_.failed = false;
    // 降过档的说明已经在 status_.detail 里了，别覆盖掉 —— 界面上要能看到
    // 「已从 240 帧降到 165 帧」，否则用户只会觉得帧率莫名其妙对不上。
    if (fps_ == requestedFps_) status_.detail = "录制中";
    return true;
}

bool MfEncoder::NegotiateInputType(void* writerRaw, uint32_t streamIndex, void* encAttrsRaw) {
    IMFSinkWriter* writer = static_cast<IMFSinkWriter*>(writerRaw);
    IMFAttributes* encAttrs = static_cast<IMFAttributes*>(encAttrsRaw);
    if (!writer) {
        status_.detail = "内部错误：SinkWriter 为空";
        return false;
    }

    IMFMediaType* inT = nullptr;
    HRESULT hr = MakeNv12InType(width_, height_, fps_, &inT);
    if (SUCCEEDED(hr)) hr = writer->SetInputMediaType(streamIndex, inT, encAttrs);
    if (inT) inT->Release();
    // 返回 bool：false 表示这一档被拒。调用方（StartWithFpsFallback）靠
    // status_.detail 里的 HRESULT 文本判断"是不是帧率太高"。
    if (FAILED(hr)) {
        status_.detail = "编码器不接受 " + std::to_string(fps_) + " 帧的 NV12 输入（hr=" +
                         HrText(hr) + "）";
        return false;
    }
    return true;
}

bool MfEncoder::StartWithFpsFallback() {
    // 帧率阶梯：先用用户要的那个，被拒就往下走。
    //
    // 【为什么是阶梯而不是二分】每档失败都要重开 SinkWriter（约 300~400ms，全在
    // NVENC 的 MFT 初始化里）。阶梯从 144 往上一档就是 165、再往上 240，实际
    // 命中率很高；真落到 30 帧说明这个分辨率硬件压根不编码，那时帧率已经不是
    // 主要问题了。
    static const uint32_t kLadder[] = {240, 165, 144, 120, 90, 60, 30};

    const uint32_t requested = fps_;
    uint32_t candidate = requested;
    for (;;) {
        fps_ = candidate;
        // 分片按帧数切，帧率变了必须跟着重算，否则"每片 2 秒"会变成
        // 2×(原帧率/新帧率) 秒，回放窗口的片数预算全部错位。
        segmentFrames_ = fps_ * segmentSeconds_;
        if (segmentFrames_ < fps_) segmentFrames_ = fps_;

        // 清掉上一次失败留下的痕迹，否则 OpenSegment 一进门就 return false。
        status_.failed = false;
        if (OpenSegment()) {
            if (candidate != requested) {
                status_.detail = "录制中（硬件编码器最高接受 " + std::to_string(candidate) +
                                 " 帧，已从配置的 " + std::to_string(requested) + " 帧降下来）";
            }
            return true;
        }

        // 只有"媒体类型不被接受"才值得降帧率重试；权限、磁盘满、内存不足
        // 降帧率也没用，直接报上去。
        if (status_.detail.find("MF_E_INVALIDMEDIATYPE") == std::string::npos &&
            status_.detail.find("0xC00D36B4") == std::string::npos) {
            return false;
        }

        uint32_t next = 0;
        for (uint32_t v : kLadder) {
            if (v < candidate) {
                next = v;
                break;
            }
        }
        if (next == 0) {
            status_.failed = true;
            status_.detail = "硬件编码器不接受任何候选帧率（最后试到 " +
                             std::to_string(candidate) + " 帧）";
            return false;
        }
        candidate = next;
    }
}

bool MfEncoder::OpenSegment() {
    // 槽位不再用「序号取模」，改成按时间清理后再挑槽（见 PickSlot）。
    //
    // 【原来的写法为什么坏】`segmentSeq_ % keepSegments_` 假定每片都是标称长度，
    // 于是 keepSegments 个槽位就正好装下回放窗口。但尺寸一变就会切一片（用户
    // 明确要求的：过渡时立刻切片），拖动窗口十几次就切出十几片零点几秒的过渡片，
    // 槽位被它们占满 → 环里只剩一两秒真实内容 → 导出几乎全程 NO SIGNAL。
    //
    // 现在：先按**墙钟**把跌出回放窗口的片（连文件）扔掉，再从没用过的槽位里挑；
    // 槽位全被活片占着才复用最旧的那片。这样"能回放多久"由时间决定，不由片数。
    const int64_t nowMs = NowUnixMs();
    PurgeExpired(nowMs);
    const uint32_t slot = PickSlot();
    slotInUse_ = slot;
    char name[32];
    std::snprintf(name, sizeof(name), "%06u.mp4", slot);
    currentPath_ = PathToUtf8(fs::u8path(dir_) / fs::u8path(name));

    // 这一片的墙钟起点。写到 index.csv 里去，导出端靠它判断"这片属不属于
    // 回放窗口"，也靠它算中间断了几秒（补 NO SIGNAL）。见 DESIGN.md 9.3。
    //
    // 【为什么优先用 nextSegStartUnix_】见 MfEncoder.h 里 nextSegStartUnix_ 的
    // 长注释与 DESIGN.md 9.3.1。一句话：开片这一刻可能已经被 Finalize 拖晚了，
    // 而新片的第一帧在内容上紧接着上一片最后一帧 —— 用 NowUnixMs() 会凭空
    // 造出一段空洞（实测每片 1.1~1.4 秒），导出端把它补成 NO SIGNAL。
    if (nextSegStartUnix_ > 0) {
        segStartUnix_ = nextSegStartUnix_;
    } else {
        segStartUnix_ = nowMs;
    }

    // 覆盖前先删：SinkWriter 打开已存在文件会追加/出错，行为不好预测。
    // 反正是环形复写，旧的那片本来就要丢。
    std::error_code ec;
    fs::remove(fs::u8path(currentPath_), ec);

    // ---- 容器属性 --------------------------------------------------------
    IMFAttributes* attrs = nullptr;
    HRESULT hr = MFCreateAttributes(&attrs, 4);
    if (FAILED(hr)) {
        status_.failed = true;
        status_.detail = "MFCreateAttributes 失败";
        return false;
    }
    HRESULT typeHr = attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_FMPEG4);

    // 【关键】允许 SinkWriter 用硬件 MFT。**这个属性默认是关的。**
    //
    // 不设它的后果实测（本机 RTX 4070 + Iris Xe）：SinkWriter 只挑到微软自带的
    // 软件编码器 mfh264enc.dll，2560x1440@108fps 全靠 CPU 算，进程稳定吃
    // 6.8 个核（全机 34%）—— 主人看到的就是「shadow-capture.exe CPU 40%
    // 且资源管理器卡顿」。而机器上其实装着 nvEncMFTH264.dll（NVIDIA 的
    // H.264 编码 MFT），只是没被枚举进去。
    //
    // 硬件 MFT 把编码丢给显卡专用电路，同样的画面 CPU 几乎不占。
    // 拿不到硬件 MFT 时会自动回退软件编码，所以设它没有风险。
    if (SUCCEEDED(typeHr)) {
        typeHr = attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    }

    // 【为什么这里没有"开启 GPU 色彩转换"的开关】
    //
    // 试过 MF_READWRITE_ENABLE_ADVANCED_VIDEO_PROCESSING —— 这个属性根本不存在
    // （SDK 里只有 MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING，那是给
    // SourceReader 的）。SinkWriter 侧没有对应的开关：它在没有 D3D manager 时
    // 只能用 msvproc.dll 在 CPU 上把 BGRA 转成 NV12。
    //
    // 要绕开这一层，唯一的路是给 SinkWriter 一个 D3D manager + 直接把采集到的
    // 纹理喂进去（见下面那段注释，目前走不通）。在此之前，1440p 的 CPU 成本
    // 主要就是这一次转换 —— 想再降只能改数据通路，不能靠属性。

    // 【已知缺陷：每切一片 SinkWriter 都不完全回收资源】
    //
    // 实测句柄增长与"建拆 SinkWriter 的次数"成正比，与码率、预览、帧环无关：
    //   * 1440p@144、segmentSeconds=2 → 每秒 +10 个句柄，跑 3 分钟 974→2472；
    //   * 同样配置 segmentSeconds=10 → 每秒 +2~3 个，明显放缓。
    // 线程数则完全不涨（65~70 波动），内存也稳在 200~230MB —— 所以这不是
    // 线程泄漏，是 SinkWriter 内部（编码 MFT + 转换器 + 文件句柄）没释放干净。
    //
    // 用 MfApplySharedDevice 把 D3D manager 交给它并不解决问题，反而让
    // SetInputMediaType 报 MF_E_UNSUPPORTED_D3D_TYPE（见下条注释）。
    // 真正根治要把"每片一个 SinkWriter"换掉，属于后续改造，不在本次范围。

    // 【不要设 MF_SINK_WRITER_D3D_MANAGER —— 实测这条路走不通】
    //
    // 原本想用它把一份全进程共用的 D3D11 设备交给编码 MFT，省掉每片重建
    // 设备带来的句柄泄漏（+35 个/片）。但 SinkWriter 一旦拿到 D3D manager，
    // 就要求输入是 D3D 纹理；我们喂的是系统内存里的 RGB32，于是
    // SetInputMediaType 直接失败：
    //     MF_E_UNSUPPORTED_D3D_TYPE  0xC00D6D76
    // 症状极具欺骗性 —— 前 70 多帧正常写盘（转换器还有缓冲），随后
    // WriteSample 永久返回该错误，recordFrames 冻在 74、分片停在 1 个。
    //
    // 要真正吃下这条路径，得把每帧 BGRA 上传成 D3D 纹理、输入类型改成
    // 纹理类型、样本用 IMFDXGIBuffer 包装 —— 那是另一件事。当前先保住
    // 「能录」。句柄增长改用别的办法压（见 SegmentRecorder 的分片策略）。

    // 【关键】关掉实时节流。SinkWriter 默认按墙钟节流（它假设你在做转码
    // 播放），我们喂的本来就是实时流，再被节流一次会让 write 阻塞、把喂帧
    // 线程卡住，进而拖垮采集节拍。这个属性必须设在 SinkWriter 上（不是
    // 编码器上），所以只有这一处能设。
    if (SUCCEEDED(typeHr)) {
        typeHr = attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    }

    // 【关键】B 帧数量 = 0，低延迟优先。
    //
    // 不关 B 帧的话，分片**没法直拷拼接**：每片的 POC（picture order count）
    // 都从 0 重新开始，解码器跨片接不上，报 "co located POCs unavailable"，
    // 实测 18 秒成品只解出 312 帧（应该 2592 帧）、ffmpeg 直接报错退出。
    // 屏幕录制本来就是低延迟优先，不要 B 帧不亏。
    //
    // 【踩过的坑】这两个属性有两个投放口：① SinkWriter 的属性
    // MF_SINK_WRITER_ENCODER_CONFIG；② SetInputMediaType 的第三个参数。
    // 我一开始只用了 ①，**实测无效** —— 编码器照样出 B 帧，导出拼不起来。
    // 真正生效的是 ②：探针 probe_mf_seg.cpp 里就是靠它把 4 片 × 30 帧
    // 拼成 120/120 完美解码的。所以下面两处都要给（①只是给驱动层的提示，
    // ②才是真正传到编码器 MFT 的配置）。
    IMFAttributes* encAttrs = nullptr;
    // 【容量必须 >= 下面 Set 的条数】MFCreateAttributes 的第二个参数是
    // 「预计属性个数」，超出容量的 Set* 会**静默失败**（返回 S_OK 也不生效）。
    // 这里要设 4 条（B 帧数 / 低延迟 / 码控模式 / 质量速度），所以给 4。
    if (SUCCEEDED(MFCreateAttributes(&encAttrs, 4))) {
        encAttrs->SetUINT32(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        encAttrs->SetUINT32(CODECAPI_AVEncCommonLowLatency, TRUE);
        encAttrs->SetUINT32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
        // 【编码速度优先】NVENC 默认走「质量优先」档，1440p@144 时 videoencode
        // 引擎被顶到 44~51%。录制回放不需要电影级画质，把 QualityVsSpeed 压到
        // 低位（0=最快，100=最好）能显著降 GPU 占用。
        encAttrs->SetUINT32(CODECAPI_AVEncCommonQualityVsSpeed, 20);
        // 【不要关 CABAC】试过 CODECAPI_AVEncH264CABACEnable=FALSE 再快一点，
        // 但压缩率掉得离谱：20 秒 1440p 成片从 6.2MB 涨到 57MB（9 倍）。
        // 回放场景磁盘占用比那点 GPU 更重要，所以保持默认（CABAC 开）。
    }

    IMFSinkWriter* writer = nullptr;
    if (SUCCEEDED(typeHr)) {
        typeHr = MFCreateSinkWriterFromURL(ToWide(currentPath_).c_str(), nullptr, attrs, &writer);
    }
    attrs->Release();

    if (FAILED(typeHr) || !writer) {
        if (encAttrs) encAttrs->Release();
        if (writer) writer->Release();
        status_.failed = true;
        status_.detail = "建分片写入器失败（hr=" + HrText(typeHr) + "）";
        return false;
    }

    // ---- 出 stream：H.264 -------------------------------------------------
    IMFMediaType* outT = nullptr;
    DWORD stream = 0;
    hr = MakeH264OutType(width_, height_, fps_, bitrateBps_, &outT);
    if (SUCCEEDED(hr)) hr = writer->AddStream(outT, &stream);
    if (outT) outT->Release();
    if (FAILED(hr)) {
        writer->Release();
        status_.failed = true;
        status_.detail = "编码器不接受这个输出类型（可能是分辨率/码率超范围）";
        return false;
    }

    // ---- 入 stream：NV12（自己转，见 nv12Buf_ 的注释）-------------------
    //
    // 【关键】第三个参数 encAttrs 是真正传到编码器 MFT 的配置。
    // 只把它塞进 MF_SINK_WRITER_ENCODER_CONFIG（SinkWriter 的属性）实测
    // 无效 —— 编码器照样出 B 帧，分片拼接时 POC 对不上，导出废掉。
    //
    // 帧率协商放在 NegotiateInputType 里：硬件编码器有「分辨率 × 帧率」的
    // 吞吐上限，超了会拒绝输入类型。降档重开 writer 由 StartWithFpsFallback
    // 负责 —— 这里只报失败，并把 HRESULT 写进 status_.detail 让它认得出来。
    if (!NegotiateInputType(writer, stream, encAttrs)) {
        if (encAttrs) encAttrs->Release();
        writer->Release();
        status_.failed = true;
        return false;
    }
    if (encAttrs) encAttrs->Release();
    encAttrs = nullptr;

    hr = writer->BeginWriting();
    if (FAILED(hr)) {
        writer->Release();
        status_.failed = true;
        status_.detail = "BeginWriting 失败（hr=" + HrText(hr) + "）";
        return false;
    }

    writer_ = writer;
    streamIndex_ = stream;
    framesInSegment_ = 0;
    segContentW_ = 0;
    segContentH_ = 0;
    // 新片的样本缓冲要按（可能变了的）分辨率重建。
    ReleaseSample();
    return true;
}

bool MfEncoder::EnsureSample() {
    if (sample_) return true;

    // NV12 一帧 = Y 平面（w×h）+ 交错色度平面（(w/2)×(h/2)×2 = w×h/2）
    //          = w × h × 3/2。width_/height_ 在 Start/ResizeTo 里已经取过偶。
    const DWORD rowBytes = width_;                     // Y 平面行距 = width
    const DWORD frameBytes = width_ * height_ * 3u / 2u;

    // 转换用的中间缓冲：源 BGRA 行距可能不等于 width*4（DXGI staging 对齐到
    // 256 字节），所以按画布尺寸分配、逐行转换，不直接原地转。
    nv12Buf_.assign(frameBytes, 0);

    IMFMediaBuffer* buf = nullptr;
    HRESULT hr = MFCreateMemoryBuffer(frameBytes, &buf);
    if (FAILED(hr)) {
        status_.failed = true;
        status_.detail = "MFCreateMemoryBuffer 失败（内存不足）";
        return false;
    }

    IMFSample* sample = nullptr;
    hr = MFCreateSample(&sample);
    if (FAILED(hr)) {
        buf->Release();
        status_.failed = true;
        status_.detail = "MFCreateSample 失败";
        return false;
    }
    sample->AddBuffer(buf);
    buf->Release();

    // 一次 Lock 拿到指针常驻，写帧时只 memcpy 不再进 COM。
    // Lock 出来的指针在 Unlock 之前一直有效，而我们在 ReleaseSample 之前
    // 不会 Unlock —— 每帧一次 Lock/Unlock 会带来可观的 COM 开销。
    BYTE* dst = nullptr;
    DWORD maxLen = 0;
    hr = sample->GetBufferByIndex(0, &buf);
    if (SUCCEEDED(hr)) {
        hr = buf->Lock(&dst, &maxLen, nullptr);
        buf->Release();
    }
    if (FAILED(hr) || !dst) {
        sample->Release();
        status_.failed = true;
        status_.detail = "锁缓冲区失败";
        return false;
    }

    sample_ = sample;
    sampleBytes_ = dst;
    samplePitch_ = rowBytes;  // Y 平面行距（= width），NV12 的样本缓冲按它排布
    return true;
}

void MfEncoder::ReleaseSample() {
    IMFSample* sample = static_cast<IMFSample*>(sample_);
    sample_ = nullptr;
    sampleBytes_ = nullptr;
    samplePitch_ = 0;
    if (!sample) return;

    IMFMediaBuffer* buf = nullptr;
    if (SUCCEEDED(sample->GetBufferByIndex(0, &buf)) && buf) {
        buf->Unlock();
        buf->Release();
    }
    sample->Release();
}

void MfEncoder::CloseSegment() {
    IMFSinkWriter* writer = static_cast<IMFSinkWriter*>(writer_);
    if (!writer) return;

    // Finalize 会把 moov/mfra 索引写完 —— 这是分片能被读出来的前提。
    // 失败也不影响已经写进去的 moof，SourceReader 仍然能读到内容。
    writer->Finalize();
    writer->Release();
    writer_ = nullptr;
    // 这一片的复用样本到此为止；下一片会按当时的分辨率重建。
    ReleaseSample();

    ++status_.segmentsWritten;

    // 【零帧的空片：连文件一起删掉】拖窗口时尺寸会逐像素连续变，每变一次都
    // 会走到切片的路径，于是产生一堆"开出来还一帧没写就被换掉"的空片。
    // 它们不进 done_（下面那个 if 挡住了），但 SinkWriter 已经在磁盘上建了
    // 文件 —— 不删就永久占着一个槽位的文件名，导出端还会把它当成可读分片去
    // 探测（虽然 <4096 字节会被 ListSegments 丢掉，但目录会越来越脏）。
    if (framesInSegment_ == 0) {
        std::error_code ec;
        if (!currentPath_.empty()) fs::remove(fs::u8path(currentPath_), ec);
        return;
    }

    {
        // 记进时间顺序表。槽位复写时，同一个文件名会再次出现 —— 那是
        // 同一槽位的**新**内容，旧记录必须丢掉，否则清单里会有两行同名文件、
        // 时间轴串到重复内容。
        const std::string leaf = fs::u8path(currentPath_).filename().u8string();
        for (auto it = done_.begin(); it != done_.end();) {
            if (it->name == leaf) {
                it = done_.erase(it);
            } else {
                ++it;
            }
        }
        DoneSegment d;
        d.name = leaf;
        d.frames = framesInSegment_;
        d.startSec = doneSeconds_;
        d.startUnixMs = segStartUnix_;
        d.slot = slotInUse_;
        // 真实画面尺寸（过渡期画布会补出黑边，导出端要按这个定画布，
        // 否则用户的画面会缩成左上角一小块）。0 = 这一片从没走过渡路径。
        d.contentW = segContentW_ ? segContentW_ : width_;
        d.contentH = segContentH_ ? segContentH_ : height_;
        doneSeconds_ += static_cast<double>(framesInSegment_) / static_cast<double>(fps_);
        done_.push_back(std::move(d));

        // 下一片第一帧的墙钟 = 这一片起点 + 这一片的真实时长（帧数算出来）。
        //
        // 【为什么必须这么接，而不是等开新片时现取 NowUnixMs()】见 MfEncoder.h
        // 里 nextSegStartUnix_ 的长注释与 DESIGN.md 9.3.1。Finalize 排空编码
        // 队列期间墙钟一直在跑，新片内容却紧接着上一片 —— 用现取时间会在
        // index.csv 里凭空造出 1 秒以上的空洞，导出端把它补成 NO SIGNAL。
        //
        // 帧率取 fps_（喂帧线程按这个补帧，帧数与墙钟严格对应），不做四舍五入：
        // 误差会随片数累积，宁可留着小数让导出端按墙钟裁。
        const double secPerFrame = 1.0 / static_cast<double>(fps_ ? fps_ : 60);
        nextSegStartUnix_ =
            segStartUnix_ + static_cast<int64_t>(static_cast<double>(framesInSegment_) * secPerFrame * 1000.0);
        RewriteIndex();
    }
}

/**
 * 按墙钟扔掉跌出回放窗口的分片（连文件一起）。
 *
 * 【为什么必须按时间清，不能只按片数轮转】见 DESIGN.md 9.9。
 * 「保留最后 N 片」只在每片等长时才等价于「保留最后 N 秒」。尺寸一变就切片
 * （用户明确要求的），片长就变得参差不齐：一次拖拽能切出七八片零点几秒的
 * 过渡片，N 个槽位被它们吃光，环里真实内容只剩一两秒，导出于是几乎全程
 * NO SIGNAL —— 主人报的就是这个。
 *
 * 改成按时间清之后，"能回放多久"只由墙钟决定：片多片少都无所谓，只要
 * 覆盖到回放窗口就行。槽位数量则给足余量（keepSlots_ = 标称片数 × 8）。
 */
void MfEncoder::PurgeExpired(int64_t nowMs) {
    if (done_.empty()) return;
    const int64_t windowMs = WindowMs();
    if (windowMs <= 0) return;
    const double secPerFrame = 1.0 / static_cast<double>(fps_ ? fps_ : 60);
    // 【"现在"要用时间轴上的现在，不是墙钟的现在】
    //
    // 分片的 startUnixMs 是**内容时间轴**：第一片开片时取一次真实墙钟，之后
    // 每片按 `帧数 × 单片秒数` 顺延。编码器跟不上（1440p@144 实测常常只跑到
    // 106~135fps）时，写满 2 秒内容要花 2.7~3 秒墙钟，于是时间轴越走越落后
    // 于墙钟 —— 实测 40 秒后落后 16 秒。
    //
    // 拿墙钟 nowMs 当 cutoff 就会把时间轴上还很"新"的片清掉：环里只剩 6~8 片
    // （12~16 秒），`recordSegments` 永远到不了 10，用户真点导出时窗口里总差
    // 几秒被补成 NO SIGNAL。
    //
    // 导出端本来就是时间轴口径（cutoffEnd 取最新一片的起点+真实时长，
    // windowStart = cutoffEnd - 回放秒数），所以这里也统一到时间轴，两边一致。
    int64_t axisNow = nowMs;
    if (!done_.empty()) {
        const DoneSegment& newest = done_.back();
        const int64_t newestEnd =
            newest.startUnixMs + static_cast<int64_t>(static_cast<double>(newest.frames) * secPerFrame * 1000.0);
        // 时间轴只会落后墙钟（编码慢）或持平，正常不会超前；万一超前也夹回墙钟。
        if (newestEnd > 0 && newestEnd < axisNow) axisNow = newestEnd;
    }
    // 留一点余量：刚好卡在窗口边界上的那一两片也留着，导出端按时间裁会更准。
    //
    // 【余量必须是"两片"而不是"一片"】环形缓冲里必须始终能拼出完整的
    // 回放窗口。存活的片集合是 `[axisNow - window - 余量, axisNow]`，其中最新
    // 那一片还在写（最长 segmentSeconds_ 秒）。要让"已完成"的部分也覆盖
    // window，余量至少得是一片（补正在写的那片）+ 一片（兜住裁掉的半片）。
    const int64_t keepSlack = static_cast<int64_t>(segmentSeconds_) * 2000;
    const int64_t cutoff = axisNow - windowMs - keepSlack;

    bool changed = false;
    for (auto it = done_.begin(); it != done_.end();) {
        // 【起点也要判，不能只判终点】分片的时间戳是**按帧序号推**出来的
        // （见 CloseSegment 里的 nextSegStartUnix_），编码器跟不上时墙钟会
        // 跑在帧序号前面。只判终点的话，一片"起点在 18 秒前、终点刚进窗口"
        // 的片会被留着 —— 它的起点早就跌出窗口了，清单里于是出现一条
        // 18 秒前的记录，而窗口是 20 秒，看起来"只留了 7 片"。
        // 判终点保证不留早于窗口的尾巴，判起点保证清单不会倒着长。
        const int64_t endMs =
            it->startUnixMs + static_cast<int64_t>(static_cast<double>(it->frames) * secPerFrame * 1000.0);
        if (endMs < cutoff || it->startUnixMs < cutoff) {
            // 已经彻底跌出窗口了：删除文件、从清单里去掉，槽位随之释放。
            std::error_code ec;
            fs::remove(fs::u8path(dir_) / fs::u8path(it->name), ec);
            it = done_.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    if (changed) RewriteIndex();
}

/**
 * 给新分片挑一个槽位。
 *
 * 先找没被任何活分片占用的槽位（顺带把 KeepSlot 之外的旧记录挤掉）；
 * 全被占满时，说明窗口里全是短过头的片 —— 这时复用**最旧**的那片，
 * 相当于丢掉最老的内容，语义与环形缓冲一致。
 */
uint32_t MfEncoder::PickSlot() {
    std::vector<bool> used(keepSlots_, false);
    for (const DoneSegment& d : done_) {
        if (d.slot < keepSlots_) used[d.slot] = true;
    }
    for (uint32_t i = 0; i < keepSlots_; ++i) {
        if (!used[i]) return i;
    }

    // 一个空槽都没有：扔掉最旧的那一片，腾一个出来。
    size_t oldest = 0;
    for (size_t i = 1; i < done_.size(); ++i) {
        if (done_[i].startUnixMs < done_[oldest].startUnixMs) oldest = i;
    }
    const uint32_t slot = done_[oldest].slot;
    {
        std::error_code ec;
        fs::remove(fs::u8path(dir_) / fs::u8path(done_[oldest].name), ec);
    }
    done_.erase(done_.begin() + static_cast<std::ptrdiff_t>(oldest));
    RewriteIndex();
    return slot < keepSlots_ ? slot : 0;
}

/**
 * 把 index.csv 整份重写一遍，内容取自 done_（时间顺序表）。
 *
 * 为什么是"整份重写"而不是"追加"：槽位复写意味着第 61 片会覆盖第 1 片的
 * 文件。追加写会让同一文件名在清单里出现两次（一行旧时间、一行新时间），
 * 导出端按清单串时间轴就会串到重复内容。整份重写最省事，而且 done_ 已经
 * 按真实收片顺序排好，不用再猜。
 *
 * 格式刻意保持和 ffmpeg 的 `-segment_list`（csv）一致：三列
 * `文件名,起始秒,结束秒`，导出端为两种录制方式写一套解析就够。
 *
 * 时长用**帧数**算而不是文件 mtime：mtime 精度只有 ~15ms 且写文件期间一直
 * 在变（ffmpeg 时代踩过，把完整的 2 秒整片误判成不完整，20 秒缓冲只剩 18 秒）。
 */
void MfEncoder::RewriteIndex() {
    if (dir_.empty()) return;

    const double secPerFrame = 1.0 / static_cast<double>(fps_);

    std::ostringstream out;
    for (const DoneSegment& d : done_) {
        const double end = d.startSec + static_cast<double>(d.frames) * secPerFrame;
        char line[512];
        // 六列：文件名,起始秒,结束秒,起始墙钟Unix毫秒,真实宽,真实高。
        //
        // 前三列沿用 ffmpeg `-segment_list` 的格式（导出端一套解析就够）；
        // 第四列是本项目加的，见 DESIGN.md 9.3 —— 没有它，昨天的分片今天还在
        // 目录里就会被当成"最近 20 秒"的一部分。
        //
        // 第五、六列也是本项目加的：WritePadded 路径（窗口拖小）画布比真实
        // 画面大、右下是补的黑边，文件帧尺寸不等于真实画面尺寸。导出端必须
        // 按这两列裁掉黑边再定画布，否则用户的画面会被黑边撑成小方块。
        // 老数据没有这两列，读到就退回按文件帧尺寸（0 表示不知道）。
        std::snprintf(line, sizeof(line), "%s,%.6f,%.6f,%lld,%u,%u\n", d.name.c_str(), d.startSec,
                      end, static_cast<long long>(d.startUnixMs), d.contentW, d.contentH);
        out << line;
    }

    const fs::path indexPath = fs::u8path(ListFilePath());
    std::ofstream f(PathToUtf8(indexPath), std::ios::binary | std::ios::trunc);
    if (f) f << out.str();
}

bool MfEncoder::Write(const void* pixels, uint32_t stride, uint32_t width, uint32_t height) {
    if (!pixels || !writer_) return false;

    // 喂帧线程通常和 Start() 那个线程不同，套间要自己进。
    //
    // 【为什么必须放在尺寸分支之前】换尺寸要走 ResizeTo，它会 Finalize 当前
    // SinkWriter、再 OpenSegment 建一个新的 —— 这两步都在 SinkWriter/编码器
    // 这些 MTA 对象上调用。线程没进套间就裸指针调用，实测直接 0xC0000005
    // 把整个服务打死（WER 里 fault offset 0x55a30，崩在第一次拖窗口变宽时）。
    // 早先这一句写在尺寸分支后面，只在"尺寸没变"的路径上生效 —— 是个地雷。
    MfThreadScope threadScope;
    if (!threadScope.ok) {
        status_.failed = true;
        status_.detail = "喂帧线程拿不到 COM 套间";
        return false;
    }

    // 分辨率变了：收掉当前片，用新尺寸接着录（同一轮次目录、切片序号继续）。
    // 见 DESIGN.md 9.5 —— 以前这里直接报错，上层只好重建整个录制器，于是
    // 新开一个轮次目录、切片从 000000 重来，回放里就只剩调整尺寸之后的画面。
    //
    // 【两种情形分开处理】
    //   帧比画布大（窗口被拖大）→ **立刻切一片**，新片按新尺寸精确开
    //     （kTransitionFactor 已关成 1:1，见 MfEncoder.h 的注释）。
    //     绝不裁掉右边 —— 主人报过"窗口一往右变大，右边就被截断了"。
    //   帧比画布小（窗口被拖小）→ 1:1 贴左上角补黑边继续写，一个像素都不丢；
    //     拖拽停稳后由 ResizeTo 的 kSizeStableMs 兜底切一片精确尺寸的。
    //
    // 【为什么切片之后还要再判一次尺寸】WritePadded 的递归调用传的是**画布**
    // 尺寸，仍可能不等于 width_/height_ 之外的情形（历史教训：早先写成
    // `if 大/else if 不等`，切片那条路就不进 WritePadded，下面按画布行距读源
    // 缓冲逐行越界，0xC0000005，Fault offset 0x55A30）。两个独立的 if：只要
    // 尺寸不一致就走贴图路径，那条路径里的 width_/height_ 一定装得下这一帧。
    if (width > width_ || height > height_) {
        if (!ResizeTo(width, height)) return false;
    }
    if (width != width_ || height != height_) {
        // WritePadded 会递归回 Write（画布尺寸已匹配），那次再进套间是嵌套
        // 计数，无害。
        return WritePadded(pixels, stride, width, height);
    }

    IMFSinkWriter* writer = static_cast<IMFSinkWriter*>(writer_);
    const DWORD rowBytes = width_;                       // NV12 的 Y 平面行距
    const DWORD frameBytes = width_ * height_ * 3u / 2u;  // Y + UV
    HRESULT hr = S_OK;

    // 记下这一片里真实画面的最大尺寸（过渡期的画布比画面大，右下是补的黑边）。
    // 导出端按它定输出画布，否则用户的画面会缩成左上角一小块。
    //
    // 递归进来的那次（WritePadded 补完黑边回来）传的是**画布**尺寸，不能记账
    // —— 真实尺寸已经由 WritePadded 自己记过了。
    if (!padRecurse_) {
        if (width > segContentW_) segContentW_ = width;
        if (height > segContentH_) segContentH_ = height;
    }

    // 【复用样本，不再每帧分配】见 MfEncoder.h 里 sample_ 的注释：
    // 每帧 MFCreateMemoryBuffer(14.7MB) 让 1440p@144 每秒分配 2.1GB，
    // 实测进程峰值工作集 854MB。这里改成每片分配一次。
    if (!EnsureSample()) return false;
    IMFSample* sample = static_cast<IMFSample*>(sample_);
    uint8_t* dst = sampleBytes_;

    // 【自己转 NV12，不再让 SinkWriter 转】见 MfEncoder.h 里 nv12Buf_ 的注释：
    // 这一步把 1440p@144 的 CPU 从 3.8~6.8 核压到 0.8 核，颜色还更准。
    //
    // 转换读源、写 nv12Buf_，然后整块 memcpy 进样本缓冲。不直接转进样本缓冲
    // 的原因：DXGI staging 的行距会对齐到 256 字节（常大于 width*4），逐行
    // 转换的源步长和目标步长不是一回事，分开写更清楚也不慢（多一次连续写）。
    //
    // 【不用 non-temporal】见 ConvertToNv12 的注释：Y 平面写完立刻被编码器
    // 读回来算色度/上传，绕缓存反而更慢（probe_copy_cpu 实测 ntsse 版本
    // 在"拷贝+写样本"里比 memcpy 还慢一倍）。
    ConvertToNv12(static_cast<const uint8_t*>(pixels), stride, width_, height_, nv12Buf_.data());
    std::memcpy(dst, nv12Buf_.data(), frameBytes);
    IMFMediaBuffer* buf = nullptr;
    if (SUCCEEDED(sample->GetBufferByIndex(0, &buf)) && buf) {
        buf->SetCurrentLength(frameBytes);
        buf->Release();
    }

    // 时间轴：**每个分片内部从 0 开始**。这是分片直拷能拼接的前提 ——
    // 每一片都是一个独立的、从 0 起算的短片，导出时再按顺序重编号
    // （见 Exporter.cpp）。如果这里用全局时间戳，分片的容器时长会带上
    // 前面所有片的偏移，单片拿出来播放会一开始就是空白/卡住。
    //
    // 【为什么是"乘除"而不是"每帧累加一个整数间隔"】
    // 1e7/fps 在常见帧率下除不尽：144fps 是 69444.44…，取整成 69444 之后
    // 每帧少 0.44 tick，144 帧就差 64 tick。实测 2592 帧（18 秒）累积成
    // 17.999882 秒，连 avg_frame_rate 都变成 23328000/161999 这种怪分数。
    // 改成「帧号 × 1e7 / fps 再四舍五入」，误差永久锁在 ±0.5 tick（约 50ns），
    // 且不会累积 —— 帧率显示回到干净的 144/1。
    const long long ts = static_cast<long long>(
        static_cast<double>(framesInSegment_) * 10000000.0 / static_cast<double>(fps_) + 0.5);
    // 时长取"下一帧时间戳 - 这一帧"：这样每帧 duration 自动吸收取整误差，
    // 全片各帧时长加起来严格等于总时长，不会在末尾攒出空白。
    const long long nextTs = static_cast<long long>(
        static_cast<double>(framesInSegment_ + 1) * 10000000.0 / static_cast<double>(fps_) + 0.5);
    sample->SetSampleTime(ts);
    sample->SetSampleDuration(nextTs - ts);

    hr = writer->WriteSample(streamIndex_, sample);
    if (FAILED(hr)) {
        status_.failed = true;
        status_.detail = "写帧失败（hr=" + HrText(hr) + "），编码器可能已经挂了";
        return false;
    }

    ++framesInSegment_;
    ++status_.framesWritten;

    // 这一片够了就切下一片。判据是"帧数"而不是"墙钟"：
    // 时间轴由帧数保证，按帧数切分才能让每片时长严格等于帧数/fps，
    // 导出端按标称时长累加才不会漂。
    if (framesInSegment_ >= segmentFrames_) {
        CloseSegment();
        ++segmentSeq_;
        if (!OpenSegment()) return false;
    }
    return true;
}

bool MfEncoder::WritePadded(const void* pixels, uint32_t stride, uint32_t width,
                            uint32_t height) {
    if (!pixels || !writer_) return false;
    if (width_ == 0 || height_ == 0 || width == 0 || height == 0) return false;

    // 画布尺寸就是当前片已协商好的尺寸，绝不能变 —— 变了编码器就不认了
    // （探针实测：片内跨尺寸 SetInputMediaType 返回 S_OK 但完全不生效）。
    // padBuf_ 仍是 BGRA：ConvertToNv12 在 Write() 里统一做，这里只负责贴图。
    const size_t canvasRow = static_cast<size_t>(width_) * 4u;
    const size_t canvasBytes = canvasRow * height_;
    if (padBuf_.size() != canvasBytes) padBuf_.assign(canvasBytes, 0);

    const uint8_t* src = static_cast<const uint8_t*>(pixels);
    uint8_t* dst = padBuf_.data();

    // 先记真实画面尺寸（导出端按它定画布，见 DoneSegment::contentW）。
    if (width > segContentW_) segContentW_ = width;
    if (height > segContentH_) segContentH_ = height;

    // 先整体清黑，下面只覆盖"这一帧真正占了"的那一块，其余自然是黑边。
    std::memset(dst, 0, canvasBytes);

    if (width > width_ || height > height_) {
        // 到不了这里：Write() 已经把"帧比画布大"拦去切片了。留着兜底 ——
        // 真到了说明画布尺寸算错，宁可裁掉右边也不能让编码器吃错数据。
        const uint32_t copyCols = width_ < width ? width_ : width;
        const uint32_t copyRows = height_ < height ? height_ : height;
        const size_t copyBytes = static_cast<size_t>(copyCols) * 4u;
        for (uint32_t y = 0; y < copyRows; ++y) {
            std::memcpy(dst + static_cast<size_t>(y) * canvasRow,
                        src + static_cast<size_t>(y) * stride, copyBytes);
        }
    } else {
        // 装得下：1:1 贴左上角，右侧/下方补黑（与导出端的语义完全一致 ——
        // 小画面贴左上角、不拉伸，见 DESIGN.md 9.5）。
        //
        // **绝不缩放**：主人明确要求（m08329「不能缩放」）。缩放会让导出端
        // "按窗口内最大尺寸作画布"的语义变得含混 —— 分片里存的到底是不是
        // 1:1 的像素就没法保证了。
        const uint32_t copyCols = width;
        const uint32_t copyRows = height;
        const size_t copyBytes = static_cast<size_t>(copyCols) * 4u;
        // 整块一次拷的前提是**源和目标的行距都等于 copyBytes**：
        //   - 源行距 = stride（DXGI staging 会对齐到 256 字节，常大于紧凑行距）；
        //   - 目标行距 = canvasRow（= width_*4）。
        if (stride == copyBytes && canvasRow == copyBytes) {
            std::memcpy(dst, src, copyBytes * copyRows);
        } else {
            for (uint32_t y = 0; y < copyRows; ++y) {
                std::memcpy(dst + static_cast<size_t>(y) * canvasRow,
                            src + static_cast<size_t>(y) * stride, copyBytes);
            }
        }
    }

    // 补好的画布交给正常的写帧路径。传 width_/height_ 让它不会再触发尺寸分支。
    //
    // padRecurse_ 置位的原因：那次 Write 收到的是画布尺寸，Write 里给
    // segContentW_/segContentH_ 记账时必须跳过，真实尺寸已经在这里记过了。
    padRecurse_ = true;
    const bool ok = Write(padBuf_.data(), static_cast<uint32_t>(canvasRow), width_, height_);
    padRecurse_ = false;
    return ok;
}

bool MfEncoder::FlushSegment() {
    // 没在录就没有"正在写的那一片"可收。
    if (!writer_ || !status_.running) return false;

    // 当前片一帧都没有：收它只会留下一个空文件，没有任何时间轴内容。
    // 返回 false 让调用方知道"这一刻还不需要 flush"（稍等再来即可）。
    if (framesInSegment_ == 0) return false;

    // 【哪怕只有几帧也必须真收】这正是本函数存在的理由：点导出那一刻
    // 正在写的那片还压在 SinkWriter 里，index.csv 里没有它，导出端只认到
    // 上一片完成的分片 —— 用户看到的"点导出时前一秒/前半秒画面丢失"就是它。
    // 收掉会留下一个零点几秒的碎片片，那**不是**垃圾，正是按钮之前的画面。

    // CloseSegment / OpenSegment 都要碰 SinkWriter 和 COM 对象，必须在本线程
    // 有套间。喂帧线程已经在 Write 里进过套间，这里是嵌套，计数无害；
    // 但命令线程直接调本函数时就没有外层兜底，所以不能省。
    MfThreadScope threadScope;
    if (!threadScope.ok) return false;

    // 收尾顺序与 Write() 里"写满一片"、ResizeTo() 里"换尺寸"完全一致：
    // CloseSegment 落盘 + 记账 + 重写 index.csv，序号递增，再用**当前画布
    // 尺寸**开下一片继续录。
    //
    // 注意这里刻意**不**走 ResizeTo：尺寸一点没变，走 ResizeTo 会重算画布
    // 逻辑，没必要；直接按当前尺寸重开就行。
    CloseSegment();
    ++segmentSeq_;
    if (!OpenSegment()) {
        status_.failed = true;
        status_.detail = "收尾分片后重开编码器失败";
        return false;
    }
    return true;
}

void MfEncoder::Finish() {
    if (writer_) {
        MfThreadScope threadScope; // 停止线程也要有套间才能 Finalize
        CloseSegment();
    }
    if (status_.running) {
        status_.running = false;
        if (status_.detail == "录制中") status_.detail = "已停止";
    }
    // 样本缓冲也跟着收掉（CloseSegment 里已经 Release 过一次，
    // 这里是兜底：万一没关成功片也不能把 14.7MB 漏在内存里）。
    ReleaseSample();
    // 释放这个对象持有的进程级 MF 引用（配 Start 里的 MfProcessRetain）。
    if (retained_) {
        MfProcessRelease();
        retained_ = false;
    }
}

int64_t MfEncoder::NowUnixMs() {
    // 必须是墙钟（system_clock），不能用 steady_clock：要跨进程重启比较
    // ——今天的服务和昨天留下的分片得能在同一个坐标系里说清谁老谁新。
    //
    // 单位是**毫秒不是秒**：秒级分辨率下，两片紧挨着的 2 秒分片墙钟差可能
    // 是 2 也可能是 3（截断误差），导出端据此判"断档"就会凭空补出 1 秒
    // NO SIGNAL。毫秒级才够分辨"真的断了"和"只是取整"。
    return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count());
}

bool MfEncoder::ResizeTo(uint32_t newW, uint32_t newH) {
    // H.264 宏块 16x16：向下取偶，与 Write 里的口径必须一致，
    // 否则下一次 Write 又会被判"尺寸变了"，无限切片。
    newW &= ~1u;
    newH &= ~1u;
    if (newW == 0 || newH == 0) {
        status_.failed = true;
        status_.detail = "采集尺寸变成 0 了";
        return false;
    }

    // 收掉当前片。它已经写了几十上百帧，Finalize 之后是一个完整可读的分片，
    // 导出端照样能用 —— 这就是"调整尺寸之前的画面还在回放里"的关键。
    //
    // 【唯一的例外】当前片一帧都还没写（刚开出来就赶上尺寸变化）：它没有任何
    // 内容，直接按新尺寸重开，不占槽位、不推进 segmentSeq_。
    //
    // 这两个分支都会碰 SinkWriter（Finalize / CloseSegment），必须自己也进
    // 套间 —— Write 已经进过一遍，这里是嵌套计数，无害；但 ResizeTo 也可能
    // 被别处直接调用（不经 Write），所以不能指望外层。
    MfThreadScope threadScope;
    if (!threadScope.ok) {
        status_.failed = true;
        status_.detail = "换分辨率时拿不到 COM 套间";
        return false;
    }

    const bool emptySegment = (framesInSegment_ == 0 && writer_ != nullptr);
    if (emptySegment) {
        IMFSinkWriter* w = static_cast<IMFSinkWriter*>(writer_);
        w->Finalize();
        w->Release();
        writer_ = nullptr;
        ReleaseSample();
        std::error_code ec;
        if (!currentPath_.empty()) fs::remove(fs::u8path(currentPath_), ec);
    } else if (newW == width_ && newH == height_) {
        return true; // 取偶之后其实没变
    } else {
        CloseSegment();
        ++segmentSeq_;
    }

    // 【画布尺寸】kTransitionFactor 已关成 1:1（见 MfEncoder.h 的长注释：
    // 过渡画布会把文件尺寸撑成真实画面的 2 倍，导出画布被孤儿分片污染成
    // 4096x2912，还会把 NVENC 拖过吞吐上限冻住录制）。保留这段结构是为了
    // 万一以后要恢复余量，改 kTransitionFactorNum 就行。
    {
        const int64_t nowMs = NowUnixMs();
        const bool transitioning =
            lastResizeMs_ > 0 && (nowMs - lastResizeMs_) < kTransitionWindowMs;
        uint32_t canvasW = newW;
        uint32_t canvasH = newH;
        if (transitioning) {
            const uint64_t wideW = static_cast<uint64_t>(newW) * kTransitionFactorNum /
                                   kTransitionFactorDen;
            const uint64_t wideH = static_cast<uint64_t>(newH) * kTransitionFactorNum /
                                   kTransitionFactorDen;
            canvasW = static_cast<uint32_t>(wideW > kTransitionMaxDim ? kTransitionMaxDim : wideW);
            canvasH = static_cast<uint32_t>(wideH > kTransitionMaxDim ? kTransitionMaxDim : wideH);
        }
        canvasW &= ~1u;
        canvasH &= ~1u;
        if (canvasW < newW) canvasW = newW; // 夹上限之后不能比帧本身还小
        if (canvasH < newH) canvasH = newH;
        width_ = canvasW;
        height_ = canvasH;
        // 记住"这一帧的原始尺寸"，WritePadded 要在画布里居中/贴角对齐时用。
        pendingW_ = newW;
        pendingH_ = newH;
        lastResizeMs_ = nowMs;
    }

    // 码率按新分辨率重估（原来按旧尺寸算的，放大之后会糊）。显式配了码率的
    // 场景这里也重算 —— 因为 bitrateBps_ 本来就是"按当时的尺寸估"的结果，
    // 没有单独记住用户的原值。分段编码本来就不追求跨片码率一致。
    {
        const double px = static_cast<double>(width_) * height_;
        uint32_t mbps = static_cast<uint32_t>(px * 0.15 * fps_ / 1e6 + 0.5);
        if (mbps < 2) mbps = 2;
        if (mbps > 100) mbps = 100;
        bitrateBps_ = mbps * 1000000u;
    }

    if (!OpenSegment()) return false;
    return true;
}

} // namespace shadow
