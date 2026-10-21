// WindowCapture.cpp — Windows Graphics Capture 单窗口采集的实现
//
// 全部走**手写 COM**，不引 C++/WinRT：
//   * 项目一贯只依赖系统 SDK，为一个采集器把整个 WinRT 投影拖进来不值；
//   * 实际只用到 5 个接口、十来个方法，量是可控的；
//   * 手写还有个好处：老 SDK 上缺哪个接口编译期就能看出来，方便写降级分支。
//
// 用到的系统接口（头文件都在 Windows SDK 里，见下面的 include）：
//   IGraphicsCaptureItemInterop  —— 从 HWND 拿 item（WinRT 不暴露这个入口）
//   IGraphicsCaptureItem         —— 被捕获的目标
//   IDirect3DDxgiInterfaceAccess —— 把 WinRT 的 IDirect3DSurface 掰回 ID3D11Texture2D
//   IGraphicsCaptureSession      —— 一次捕获会话
//   IDirect3D11CaptureFramePool  —— DWM 往里投帧的池子
//
// 几个踩过的坑，改代码前先看：
//   1) `windows.graphics.capture.interop.h` 在 winrt\ 下**不存在**，它在
//      um\Windows.Graphics.Capture.Interop.h，而且是 __cplusplus_winrt 之外的
//      纯 C++ 声明。别去找 winrt\windows.graphics.capture.interop.h。
//   2) IGraphicsCaptureSession 只有 StartCapture()，**没有 Close()**。
//      要停录制得 QI 成 ABI::Windows::Foundation::IClosable 再 Close。
//   3) IDirect3DDxgiInterfaceAccess 在 um\windows.graphics.directx.direct3d11.interop.h。
//   4) add_FrameArrived 要的是 __FITypedEventHandler 那种委托指针，不能直接
//      传 lambda。用 WRL 的 Callback 造一个，并且**自己 AddRef 存住** ——
//      委托一析构回调就没了，下一帧进来直接崩。

#include "WindowCapture.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <inspectable.h>
#include <roapi.h>
#include <dispatcherqueue.h>
#include <wrl.h>
#include <wrl/implements.h>

#include <winrt/windows.graphics.capture.h>
#include <winrt/windows.graphics.directx.direct3d11.h>
#include <winrt/windows.system.h>
#include <Windows.Graphics.Capture.Interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <emmintrin.h> // SSE2：non-temporal 存储
#include <mutex>
#include <thread>
#include <vector>

using namespace Microsoft::WRL;

namespace shadow {
namespace {

using CaptureItem      = ::ABI::Windows::Graphics::Capture::IGraphicsCaptureItem;
using CaptureSession   = ::ABI::Windows::Graphics::Capture::IGraphicsCaptureSession;
// Session2 才有 put_IsCursorCaptureEnabled（Win10 2004 / 19041 起）。
// Session3 才有 put_IsBorderRequired（Win11 起），用来去掉捕获时那圈黄色边框。
// 两者都是**可选**的：老系统上 QI 拿不到就跳过，绝不因为缺接口而让整路失败。
using CaptureSession2  = ::ABI::Windows::Graphics::Capture::IGraphicsCaptureSession2;
using CaptureSession3  = ::ABI::Windows::Graphics::Capture::IGraphicsCaptureSession3;
using CaptureFramePool = ::ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePool;
using CaptureFrame     = ::ABI::Windows::Graphics::Capture::IDirect3D11CaptureFrame;
using DxgiAccess       = ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;
using Closable         = ::ABI::Windows::Foundation::IClosable;

// 帧到达/会话关闭的委托类型。
//
// 写成 __FITypedEventHandler_2_..._t 这个 MIDL 名字，它其实就是
// ABI::Windows::Foundation::ITypedEventHandler<Tsender, Targs> 的别名
// （见 windows.foundation.h 里的 typedef）。用别名而不是模板名，是为了
// 保证和 add_FrameArrived / add_Closed 参数里那个类型**逐字一致** ——
// 这两个名字看起来不同，编译器只认别名展开后的同一个类型。
//
// 另外它底层是 ITypedEventHandler_impl : IUnknown（不是 IInspectable），
// 所以 RuntimeClass 得写两个基类，只给一个会 static_assert 报
// "I has to derive from IInspectable"。
using FrameArrivedHandler =
    ::ABI::Windows::Foundation::__FITypedEventHandler_2_Windows__CGraphics__CCapture__CDirect3D11CaptureFramePool_IInspectable_t;
using ItemClosedHandler =
    ::ABI::Windows::Foundation::__FITypedEventHandler_2_Windows__CGraphics__CCapture__CGraphicsCaptureItem_IInspectable_t;

// 委托的**参数化 IID**，必须硬编码 —— 这是本文件最大的一个坑。
//
// 上面那两个 MIDL 别名展开后是 `ITypedEventHandler_impl<...>`，而带
// `__declspec(uuid(...))` 的是**模板特化** `ITypedEventHandler<Direct3D11CaptureFramePool*, IInspectable*>`
// （见 windows.graphics.capture.h:617 和 :645）。用别名取 `__uuidof` 拿到的
// 不是这个 IID，于是 WinRT 注册事件时拿真 IID 来 QI 我们的委托对象 →
// 我们的 QueryInterface 认不出来 → 返回 E_NOINTERFACE → 上层包装成
// **E_BOUNDS (0x8000001C = -2147483620)**，错误信息还写着"参数错误"，
// 完全指不到真正的原因。实测症状：CreateForWindow 成功、add_FrameArrived
// 失败，这一路会报"窗口独立采集（WGC）初始化失败：注册帧回调失败（hr=-2147483620）"。
//
// 取值来源（抄自本机 SDK 10.0.26100.0，属 Windows 公共 ABI，长期稳定）：
//   windows.graphics.capture.h:617
//     ITypedEventHandler<Direct3D11CaptureFramePool*, IInspectable*>  → 51a947f7-79cf-5a3e-a3a5-1289cfa6dfe8
//   windows.graphics.capture.h:645
//     ITypedEventHandler<GraphicsCaptureItem*, IInspectable*>         → e9c610c0-a68c-5bd9-8021-8589346eeee2
constexpr GUID kFrameArrivedIid =
    {0x51a947f7, 0x79cf, 0x5a3e, {0xa3, 0xa5, 0x12, 0x89, 0xcf, 0xa6, 0xdf, 0xe8}};
constexpr GUID kItemClosedIid =
    {0xe9c610c0, 0xa68c, 0x5bd9, {0x80, 0x21, 0x85, 0x89, 0x34, 0x6e, 0xee, 0xe2}};

constexpr ::ABI::Windows::Graphics::DirectX::DirectXPixelFormat kPixelFormat =
    ::ABI::Windows::Graphics::DirectX::DirectXPixelFormat::DirectXPixelFormat_B8G8R8A8UIntNormalized;

// 窗口尺寸要稳定这么久，才认定用户拖完了、该重建采集会话。
//
// 用户拖边框时窗口尺寸每几十毫秒变一次，每变一次就重建会话的话，一次重建
// 200ms 下来，整段拖拽会被拖成"一格一格"的卡顿。250ms 是折中：人手停顿
// 通常比这长，而重建延迟用户还能忍。
//
// 【现在还有用吗】这段判定已经不在 Capture 里了 —— 帧池按屏幕尺寸建、
// 永不重建，窗口缩放的尺寸跟随是零成本的（见 Init 里那段说明）。
// 常量留着是因为 TakeResizeNotice 这个接口还对外暴露着。
constexpr uint64_t kResizeSettleMs = 250;

/**
 * 把一行像素拷到目标，用非临时（streaming）存储绕开缓存。
 *
 * 为什么不用 memcpy：这块数据（窗口一帧几 MB）拷完之后只被下游读一次，
 * 走普通存储会把它塞进 L3、把采集线程正在用的东西挤出去。non-temporal
 * 写入直达内存，快且不污染缓存。
 *
 * 源不要求对齐（loadu 容忍），目标按 16 字节对齐后再 stream 存储。
 */
void CopyRowStreaming(uint8_t* dst, const uint8_t* src, size_t bytes) {
    const size_t misalign = (16u - (reinterpret_cast<uintptr_t>(dst) & 15u)) & 15u;
    const size_t head = misalign < bytes ? misalign : bytes;
    if (head) {
        std::memcpy(dst, src, head);
        dst += head;
        src += head;
        bytes -= head;
    }
    while (bytes >= 16) {
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
        _mm_stream_si128(reinterpret_cast<__m128i*>(dst), v);
        dst += 16;
        src += 16;
        bytes -= 16;
    }
    if (bytes) std::memcpy(dst, src, bytes);
    _mm_sfence();
}

// WGC 需要 Windows 10 1809（17763）以上；帧池的 CreateFreeThreaded 要 1903。
bool IsWgcAvailable(std::string& why) {
    using Fn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    if (!nt) { why = "拿不到 ntdll"; return false; }
    auto fn = reinterpret_cast<Fn>(
        reinterpret_cast<void*>(GetProcAddress(nt, "RtlGetVersion")));
    if (!fn) { why = "拿不到 RtlGetVersion"; return false; }
    RTL_OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (fn(&vi) != 0) { why = "RtlGetVersion 调用失败"; return false; }
    if (vi.dwMajorVersion < 10 ||
        (vi.dwMajorVersion == 10 && vi.dwBuildNumber < 17763)) {
        why = "系统版本低于 Windows 10 1809，没有 WGC";
        return false;
    }
    return true;
}

// COM 是按线程初始化的，而 RoGetActivationFactory 要求调用线程已经初始化过。
// 服务的每一路会话都跑在自己的 std::thread 里（main.cpp 的 N 个 Session），
// 在 main() 里初始化一点用都没有 —— 会话线程照样会拿到
// CO_E_NOTINITIALIZED（0x800401F0），WGC 静默失败退回裁剪法。
//
// 所以在这里按调用线程初始化。三条铁律：
//   1. 用 thread_local —— 每个线程只初始化一次。
//   2. 只认 S_OK / S_FALSE 才算"初始化成功"。RPC_E_CHANGED_MODE 表示该线程
//      已经用别的套间模型初始化过了，这时**绝不能**去 CoUninitialize，
//      而是直接复用（COM 已经可用，RoGetActivationFactory 能用）。
//   3. 初始化为 STA（APARTMENTTHREADED）：WGC 的帧到达回调是投递到线程
//      消息队列的，MTA 下窗口消息泵/事件投递的行为更容易出意外。
bool EnsureComInitialized(std::string& why) {
    static thread_local int state = 0;   // 0=没试过 1=自己初始化的 2=别人初始化过
    if (state != 0) return true;
    const HRESULT hr = ::RoInitialize(RO_INIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        state = 1;
        return true;
    }
    // COM 已经初始化过（模式不同）—— 能直接用，不要 Uninitialize。
    if (hr == RPC_E_CHANGED_MODE) {
        state = 2;
        return true;
    }
    why = "COM 初始化失败（hr=" + std::to_string(hr) + "）";
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Impl：所有 WinRT 相关的成员都藏在这儿
// ---------------------------------------------------------------------------
struct WindowCapture::Impl {
    ComPtr<CaptureItem>      item;
    ComPtr<CaptureFramePool> pool;
    ComPtr<CaptureSession>   session;

    // 帧到达标记。回调里只置位，真正的取帧/拷贝放在 Capture() 里同步做
    // —— 在 DWM 的回调线程上跑一大段 GPU 拷贝会跟渲染抢锁。
    std::atomic<bool> frameArrived{false};
    // 窗口被关掉时置位，Capture() 据此报 AccessLost。
    std::atomic<bool> closed{false};

    EventRegistrationToken frameToken{};
    EventRegistrationToken closedToken{};
    bool frameHooked = false;
    bool closedHooked = false;

    // 委托必须自己存住：注册完就析构的话，下一帧回调直接崩。
    ComPtr<FrameArrivedHandler> frameHandler;
    ComPtr<ItemClosedHandler>   closedHandler;

    // ---- 帧池回调的投递线程 ------------------------------------------------
    //
    // **为什么需要这个线程（踩过，别再走回头路）**：
    //   原本用的是 CreateFreeThreaded()，期望回调直接投到我们自己的线程，
    //   不用消息泵。但本机（Win11 build 26200）实测：
    //       CreateFreeThreaded            -> S_OK
    //       pool->add_FrameArrived(...)   -> E_BOUNDS (0x8000001C)  ← 必失败
    //   而普通 Create() + add_FrameArrived 返回 S_OK。
    //   CreateFreeThreaded 造出的池会先 QI 调用方的 IAgileObject，
    //   拒绝之后整条注册路径就断在 E_BOUNDS 上。
    //   界面上表现为「这一路没能用窗口独立采集…注册帧回调失败(hr=-2147483620)」，
    //   而且**和权限无关** —— 提权跑同样复现。
    //
    //   代价：普通 Create() 的帧回调要投到**创建池那个线程**的
    //   DispatcherQueue 上。所以我们自己起一个线程，在那儿建队列 + 泵消息，
    //   回调在此线程触发（只置 frameArrived 标志），Capture() 照旧从
    //   Session 线程 TryGetNextFrame 拉帧。
    HANDLE pumpThread = nullptr;
    DWORD  pumpThreadId = 0;
    std::atomic<bool> pumpStop{false};
    std::atomic<bool> pumpReady{false};
    std::atomic<bool> pumpFailed{false};
    // 泵线程跑完退出路径后置位（线程**不返回**，靠这个事件通知外面）。
    HANDLE pumpExited = nullptr;
    // 两阶段握手用：pumpDrained = 泵线程已停止 Dispatch（队列还活着）；
    // pumpTeardown = 主线程拆完了，泵线程可以去销毁队列；
    // pumpWake = 闲时等待用的小事件（避免 Sleep(1) 空转）。
    HANDLE pumpDrained = nullptr;
    HANDLE pumpTeardown = nullptr;
    HANDLE pumpWake = nullptr;
    // 阶段一是否真的把泵线程叫停了（没叫停就别指望后面那串能安全做）。
    bool pumpDrainedOk = false;
    std::string pumpError;
    // 这个队列控制器必须在泵线程上创建，而且必须存住到捕获结束，
    // 否则队列一销毁，注册过的回调全失效。
    ComPtr<::ABI::Windows::System::IDispatcherQueueController> dqController;

    // 起 / 停泵线程（实现在下面，避免 private 嵌套类型的访问麻烦）。
    //
    // 停止要分两步走，顺序不能错：
    //   1) StopPump()            —— 叫停泵线程，确认它不再 Dispatch（队列仍活着）
    //   2) FinishPumpTeardown()  —— 调用方拆完 handler/会话/帧池之后，才放它销毁队列
    bool StartPump(std::string& outError);
    void StopPump();
    void FinishPumpTeardown();

    // 建池的参数（在池线程上创建），由 Init 填好后调 CreatePoolOnPump。
    ComPtr<::ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePoolStatics> poolStatics;
    ComPtr<::ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice> poolDevice;
    ::ABI::Windows::Graphics::SizeInt32 poolSize{};
    HRESULT poolHr = E_PENDING;

    // 关光标那一步的返回码（S_OK = 成功关掉了）。
    //
    // 单独留一个字段是因为它**不影响**采集能不能起来：关不掉光标时
    // StartCapture 照样成功，画面里就是多一个鼠标箭头，看起来像"没实现"。
    // 把它记下来，配合 LastError() 就能一眼看出到底是没关还是接口缺失。
    HRESULT cursorOffHr = E_PENDING;

    // 在泵线程上建帧池（Init 已经填好 poolStatics/poolDevice/poolSize）。
    bool CreatePoolOnPump(std::string& outError);

    // 泵线程主体。
    static DWORD WINAPI PumpProc(LPVOID param);
};

WindowCapture::WindowCapture() : impl_(std::make_unique<Impl>()) {}

WindowCapture::~WindowCapture() {
    Cleanup();
}

// ---------------------------------------------------------------------------
// 帧池回调的投递线程
//
// 普通 Create() 造的 Direct3D11CaptureFramePool 把 FrameArrived 回调投递到
// **建池那个线程**的 DispatcherQueue 上。所以必须有一个线程：
//   1. 建自己的 DispatcherQueue（DQTYPE_THREAD_CURRENT）
//   2. 一直泵消息，让回调有机会跑
//   3. 活到整个捕获结束（队列一销毁回调就没了）
//
// 回调本身只置一个标志位，真正的取帧在 Capture() 里从 Session 线程做。
// ---------------------------------------------------------------------------

// DispatcherQueueController 的 CLSID（WinRT 用 COM 激活的类型，
// 它的 IID/CLSID 在 winrt\Windows.System.h 里，这里硬编码以免拉整个头）。
constexpr GUID kClsidDispatcherQueueController =
    {0xA96D0B38, 0x1DE8, 0x5A07, {0xAA, 0xA5, 0x10, 0x64, 0x0A, 0x7C, 0x04, 0x7D}};

// 泵线程主体：建队列 -> 通知就绪 -> 泵消息直到被叫停。
//
// **必须用 CreateDispatcherQueueController 而不是 RoActivateInstance**：
// DispatcherQueueController 是少数「WinRT 类型但走 COM 激活」的东西，
// 它的构造函数里带一个 DispatcherQueueOptions 参数，WinRT 的直觉
// 激活路径给不了这个参数。
//
// DQTAT_COM_NONE：这个线程不需要 COM 套间（回调只置标志位）。
//
// **这个线程必须自己 RoInitialize 一次、退出前 RoUninitialize。**
// 踩过的坑：原来这里完全没初始化 COM，靠"Session 线程已经初始化过"蒙。
// 但 RoInitialize 是 thread_local 语义 —— 别的线程初始化过跟本线程一点
// 关系都没有。于是 DispatcherQueueController 和帧池都是在一个没有套间的
// 线程上创建的：平时能用，但线程退出时系统跑 DllMain(THREAD_DETACH)，
// 那些 WindRT 对象释放残留状态时就崩了。
// 实测表现：热重载一路正在跑的 WGC 会话，trace 里能看到泵线程打印完
// "线程返回"，主线程 WaitForSingleObject 却再也没返回，进程
// 0xC0000005 静默退出。
DWORD WINAPI WindowCapture::Impl::PumpProc(LPVOID param) {
    auto* impl = static_cast<WindowCapture::Impl*>(param);

    // 本线程自己初始化 COM（MTA，和 CreateDevice 那边一致）。
    // RPC_E_CHANGED_MODE 说明外面已经用别的模式初始化过，那就复用，别去 Uninit。
    const HRESULT hrCom = ::RoInitialize(RO_INIT_MULTITHREADED);
    const bool ownCom = SUCCEEDED(hrCom);

    DispatcherQueueOptions opts{};
    opts.dwSize = sizeof(opts);
    opts.threadType = DQTYPE_THREAD_CURRENT;
    opts.apartmentType = DQTAT_COM_NONE;

    HRESULT hr = ::CreateDispatcherQueueController(
        opts, impl->dqController.GetAddressOf());
    if (FAILED(hr) || !impl->dqController) {
        impl->pumpError = "创建 DispatcherQueue 失败（hr=" + std::to_string(hr) + "）";
        impl->pumpFailed.store(true, std::memory_order_release);
        impl->pumpReady.store(true, std::memory_order_release);
        return 0;
    }

    // 队列就绪，立刻用**这个线程**的队列上下文建帧池。
    // 必须在这儿建：Create() 抓的是调用线程的 DispatcherQueue，
    // 在别的线程建出来的池，回调永远送不到。
    ComPtr<::ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePool> pool;
    impl->poolHr = impl->poolStatics->Create(
        impl->poolDevice.Get(), kPixelFormat, 2, impl->poolSize, pool.GetAddressOf());
    if (SUCCEEDED(impl->poolHr) && pool) {
        impl->pool = pool;
    } else {
        impl->pumpError = "创建帧池失败（hr=" + std::to_string(impl->poolHr) + "）";
        impl->pumpFailed.store(true, std::memory_order_release);
        impl->pumpReady.store(true, std::memory_order_release);
        return 0;
    }

    impl->pumpReady.store(true, std::memory_order_release);

    // 泵消息。帧到达回调会在 DispatchMessage 过程中跑。
    // PeekMessage 不阻塞（阻塞的是 GetMessage），所以能及时响应停止请求。
    //
    // **两阶段停止。** 这里是整个 WGC 拆解流程里最容易撞车的地方，本鱼
    // 在热重载 WGC 会话上反复崩了一晚上才理清顺序：
    //
    //   阶段一（pumpStop）：**只停止 DispatchMessage，绝不碰队列控制器。**
    //     置位后马上 SetEvent(pumpDrained)，主线程就会去 remove_FrameArrived、
    //     关会话、Reset 帧池。这些操作内部都要跟**创建帧池的那个队列**打交道，
    //     队列还活着它们才是安全的单线程操作。
    //   阶段二（pumpTeardown）：主线程把上面那一串做完之后才置位，这时才
    //     Reset dqController、RoUninitialize。
    //
    // 踩过的坑：原来一步到位，泵线程一看到 pumpStop 就 Reset dqController。
    // 结果主线程紧接着 pool->remove_FrameArrived() 就是 0xC0000005 —— 帧池
    // 的队列已经被销毁了，remove 内部访问到空指针。日志精确停在
    // `StopPump: 事件句柄已关闭` 和 `Cleanup: remove_FrameArrived 前` 之间。
    MSG msg;
    while (!impl->pumpStop.load(std::memory_order_acquire)) {
        bool got = false;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            got = true;
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        if (!got) {
            // 闲时等一个事件而不是 Sleep(1) 空转：唤醒靠 StopPump 里那个
            // 手动事件。带 1ms 超时兜底 —— 万一有消息投递不发事件（WinRT
            // 的某些内部路径就是这样），也不会漏掉。
            ::WaitForSingleObject(impl->pumpWake, 1);
        }
    }

    // 阶段一完成：不会再有回调跑起来了。通知主线程去拆会话。
    ::SetEvent(impl->pumpDrained);

    // 阶段二：等主线程把 remove/Close/Reset 全做完。
    ::WaitForSingleObject(impl->pumpTeardown, INFINITE);

    // 队列控制器要在**创建它的线程**上释放。
    impl->dqController.Reset();

    // **线程不要真的 return。**
    //
    // 这一步是本鱼花了半晚上才挖出来的。现象：泵线程明明已经跑完退出路径、
    // 打印完日志，主线程的 WaitForSingleObject 却再也没返回，进程直接
    // 0xC0000005 静默退出（连 CR T 的 terminate 处理都没进）。
    //
    // 原因在系统那边：线程函数返回时，Windows 会依次跑所有已加载 DLL 的
    // DllMain(THREAD_DETACH)。这个线程从头到尾在跟 DispatcherQueue、
    // Windows.Graphics.Capture、DWM 打交道，这些组件在 DETACH 时的清理
    // 并不保证对"宿主线程即将消失"这件事安全 —— 在仓库目录这种进程完整性
    // 被降低的环境里尤其容易踩到。
    //
    // 换个角度想：这个线程本来就不需要"退出"。它只做一件事 —— 让
    // DispatcherQueue 有人泵消息。队列控制器和帧池一旦释放，它就再无任何
    // 状态可丢。所以让它**停在这里、不返回**，DLL 的 DETACH 就不会发生，
    // 系统那边一切照旧；线程本身占用也就一个内核栈。
    //
    // 代价：每创建一个 WGC 会话就留一个不返回的线程，直到进程退出。会话
    // 数量是个位数，完全可以接受；比让整个服务崩掉强得多。
    // **RoUninitialize 不能调。**
    //
    // 泵线程是"永不返回"的，它的线程局部 WinRT 状态会一直挂到进程退出。
    // 在这里调 RoUninitialize 等于告诉系统"这个线程不用 WinRT 了"，而
    // 主线程此刻可能还有别的 WGC 会话的 WinRT 对象活着、还在用同一个
    // 进程级激活工厂表 —— 实测崩溃点就在这一句前后（0xC0000005，服务静默
    // 退出，日志一行都不多）。
    //
    // RoInitialize 是引用计数的，少调一次只是让这个计数不归零，进程退出时
    // 系统统一回收，没有任何后果。
    (void)ownCom;
    ::SetEvent(impl->pumpExited);
    for (;;) ::Sleep(1000);
}

bool WindowCapture::Impl::CreatePoolOnPump(std::string& outError) {
    // 真正建池的活已经在 PumpProc 里跟着队列一起做了，这里只是等它。
    if (!StartPump(outError)) return false;
    if (!pool) {
        outError = pumpError.empty() ? "帧池没建起来" : pumpError;
        return false;
    }
    return true;
}

bool WindowCapture::Impl::StartPump(std::string& outError) {
    if (pumpThread) return true;

    pumpStop.store(false, std::memory_order_release);
    pumpReady.store(false, std::memory_order_release);
    pumpFailed.store(false, std::memory_order_release);
    pumpError.clear();

    // **必须先建事件再起线程。** 踩过的坑：这段代码原来漏了 CreateEvent，
    // pumpExited 一直是 nullptr，于是 StopPump 里的 WaitForSingleObject
    // 直接返回 WAIT_FAILED(0xFFFFFFFF) 而不等待 —— 主线程一路往下把帧池、
    // 会话、回调全拆了，泵线程还在 DispatchMessage 里跑回调，撞上就是
    // 0xC0000005。日志里那行 `等待返回 (rc=4294967295)` 就是 WAIT_FAILED，
    // 不是 WAIT_TIMEOUT(258)。
    //
    // 手动重置事件：置位之后一直保持置位，直到这个 Impl 被销毁。泵线程
    // 一辈子只 SetEvent 一次，不存在需要自动复位的情况。
    if (!pumpExited) {
        pumpExited = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        pumpDrained = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        pumpTeardown = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        pumpWake = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!pumpExited || !pumpDrained || !pumpTeardown || !pumpWake) {
            outError = "创建泵线程同步事件失败";
            return false;
        }
    }
    ::ResetEvent(pumpExited);
    ::ResetEvent(pumpDrained);
    ::ResetEvent(pumpTeardown);
    ::ResetEvent(pumpWake);
    pumpDrainedOk = false;

    HANDLE th = ::CreateThread(nullptr, 0, &WindowCapture::Impl::PumpProc, this, 0,
                               &pumpThreadId);
    if (!th) {
        outError = "起帧回调线程失败";
        return false;
    }
    pumpThread = th;

    for (int i = 0; i < 300; ++i) {
        if (pumpReady.load(std::memory_order_acquire)) break;
        ::Sleep(10);
    }
    if (!pumpReady.load(std::memory_order_acquire)) {
        outError = "帧回调线程没起来（超时）";
        StopPump();
        return false;
    }
    if (pumpFailed.load(std::memory_order_acquire)) {
        outError = pumpError.empty() ? "帧回调线程初始化失败" : pumpError;
        StopPump();
        return false;
    }
    return true;
}

void WindowCapture::Impl::StopPump() {
    if (!pumpThread) return;
    pumpStop.store(true, std::memory_order_release);
    // 叫醒正在 pumpWake 上打盹的泵线程，别让阶段一等满超时。
    if (pumpWake) ::SetEvent(pumpWake);

    // ---- 只做阶段一：等泵线程停止 Dispatch ------------------------------
    //
    // **只等它停止 Dispatch，不销毁队列。** 调用方拿到控制权后还要做
    // remove_FrameArrived / Close / Reset 这一串，它们内部都要跟创建帧池的
    // 那个队列打交道 —— 队列得活着。
    //
    // 销毁队列是 FinishPumpTeardown() 的事，必须等调用方拆完之后再调。
    // 踩过的坑：原来把两段都塞在这儿，队列在 remove_FrameArrived 之前就没了，
    // 直接 0xC0000005。
    const DWORD drained = ::WaitForSingleObject(pumpDrained, 5000);
    pumpDrainedOk = (drained == WAIT_OBJECT_0);
}

void WindowCapture::Impl::FinishPumpTeardown() {
    if (!pumpThread) return;
    // 放泵线程去销毁队列控制器（必须在创建它的线程上做），然后收尸。
    //
    // 超时故意给得长：这一步是「等对面把活干完」，等不到只能说明对面卡死，
    // 那种情况下继续往下走必然是 0xC0000005，不如多等一会儿。
    ::SetEvent(pumpTeardown);
    ::WaitForSingleObject(pumpExited, 10000);

    // **线程句柄和同步事件都不关。**
    //
    // 泵线程被设计成永不返回（见 PumpProc 末尾那段说明），它会一直停在
    // Sleep 循环里，直到进程退出。既然线程还活着，它的句柄和它正要用的
    // 那几个事件就不能关 —— 关掉等于把内核对象从它脚下抽走。
    //
    // 代价是每个 WGC 会话泄漏一个 HANDLE 加几个事件对象，进程退出时系统
    // 统一回收；换来的是一整类竞态崩溃彻底消失。会话数量是个位数，无所谓。
    pumpThread = nullptr;
    pumpThreadId = 0;
}
bool WindowCapture::CreateDevice() {
    if (device_) return true;

    // **不加** D3D11_CREATE_DEVICE_SINGLETHREADED：WGC 的帧到达回调跑在
    // DWM 的线程上，加了单线程标志等于自己给自己埋雷。
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL got{};
    const UINT n = static_cast<UINT>(sizeof(levels) / sizeof(levels[0]));

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   flags, levels, n, D3D11_SDK_VERSION,
                                   &device_, &got, &context_);
    if (FAILED(hr)) {
        // 驱动异常/纯软渲染环境下退回 WARP，慢但能录。
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                               flags, levels, n, D3D11_SDK_VERSION,
                               &device_, &got, &context_);
    }
    if (FAILED(hr)) {
        lastError_ = "D3D11CreateDevice 失败（hr=" + std::to_string(hr) + "）";
        return false;
    }
    return true;
}

bool WindowCapture::Init(void* hwnd, std::string& outError) {
    std::lock_guard<std::mutex> lk(mu_);
    CleanupLocked();

    if (!hwnd || !IsWindow(static_cast<HWND>(hwnd))) {
        outError = "不是合法窗口";
        return false;
    }

    std::string why;
    if (!IsWgcAvailable(why)) {
        outError = why;
        return false;
    }

    // 必须在 RoGetActivationFactory 之前 —— 少了这一步，服务模式下
    // 每一路会话线程都会拿到 CO_E_NOTINITIALIZED，WGC 静默退回裁剪法。
    if (!EnsureComInitialized(why)) {
        outError = why;
        return false;
    }

    hwnd_ = hwnd;
    {
        DWORD ownerPid = 0;
        ::GetWindowThreadProcessId(static_cast<HWND>(hwnd), &ownerPid);
        hwndPid_ = ownerPid;
    }

    if (!CreateDevice()) {
        outError = lastError_;
        CleanupLocked();
        return false;
    }

    // 拿 interop 工厂。RoGetActivationFactory 要一个 HSTRING。
    HSTRING cls = nullptr;
    if (FAILED(::WindowsCreateString(
            L"Windows.Graphics.Capture.GraphicsCaptureItem",
            static_cast<UINT32>(wcslen(L"Windows.Graphics.Capture.GraphicsCaptureItem")),
            &cls))) {
        outError = "构造 WGC 类名失败";
        CleanupLocked();
        return false;
    }
    ComPtr<IGraphicsCaptureItemInterop> interop;
    HRESULT hr = ::RoGetActivationFactory(
        cls, __uuidof(IGraphicsCaptureItemInterop), reinterpret_cast<void**>(interop.GetAddressOf()));
    ::WindowsDeleteString(cls);
    if (FAILED(hr) || !interop) {
        outError = "这个系统没有 GraphicsCaptureItem 工厂（hr=" + std::to_string(hr) + "）";
        CleanupLocked();
        return false;
    }

    // ---- 从 HWND 造 item -------------------------------------------------
    hr = interop->CreateForWindow(
        static_cast<HWND>(hwnd), __uuidof(CaptureItem),
        reinterpret_cast<void**>(impl_->item.GetAddressOf()));
    if (FAILED(hr) || !impl_->item) {
        // E_ACCESSDENIED 在 CreateForWindow 上的真实含义（实测确认，别再瞎猜）：
        //   调用方进程的**完整性级别低于 Medium**。Windows 把抓屏当作高敏感
        //   能力，只有 Medium IL 及以上、非 AppContainer 的进程才能创建捕获目标。
        //   此时**任何**窗口都会失败，包括自己的控制台窗口，所以别让用户换窗口试。
        //
        //   最常见的触发方式不是沙箱，而是**可执行文件本身被打了低完整性标签**：
        //   文件带 `Mandatory Label\Low Mandatory Level` 时，从它启动的进程
        //   一律是 Low IL，哪怕父进程是 Medium。用
        //       icacls <exe>
        //   一眼就能看到。拷贝一份到不带标签的目录（例如 %TEMP%）再跑，
        //   进程就会恢复成 Medium IL —— 这是验证"到底是标签还是代码"最快的办法。
        if (hr == E_ACCESSDENIED) {
            outError = "系统拒绝抓屏权限（0x80070005）：进程完整性级别低于 Medium，"
                       "Windows 不允许低完整性进程做窗口采集。"
                       "常见原因是 shadow-capture.exe 被打了低完整性标签。"
                       "修复：在项目根目录执行 "
                       "`icacls native\\build\\shadow-capture.exe /setintegritylevel Medium` "
                       "后重启采集服务即可；"
                       "用 `icacls native\\build\\shadow-capture.exe` 可以确认是否已带 "
                       "\"Mandatory Label\\Low Mandatory Level\"。";
            CleanupLocked();
            return false;
        }
        outError = "无法为该窗口创建捕获目标（hr=" + std::to_string(hr) +
                   "），可能是受保护窗口";
        CleanupLocked();
        return false;
    }

    // ---- item 尺寸就是窗口尺寸（含边框） --------------------------------
    ::ABI::Windows::Graphics::SizeInt32 size{};
    if (FAILED(impl_->item->get_Size(&size)) || size.Width <= 0 || size.Height <= 0) {
        outError = "窗口尺寸为 0";
        CleanupLocked();
        return false;
    }
    width_ = static_cast<uint32_t>(size.Width);
    height_ = static_cast<uint32_t>(size.Height);

    // ---- 帧池尺寸：一次建成"屏幕那么大"，之后**永不重建** ---------------
    //
    // 【为什么不是窗口尺寸】这是本鱼花了两天、几百个泄漏线程换来的一条结论。
    //
    // 帧池的尺寸只是"每帧缓冲的容量"，不是"画面尺寸" —— WGC 按窗口当前的
    // ContentSize 往这个容量的纹理里画。原来的做法是用窗口尺寸建池，窗口
    // 一变就跟帧池对不上，只好 Cleanup + Init 重建整个会话（连 D3D11 设备、
    // DispatcherQueue、泵线程一起重建）。
    //
    // 实测那条路的代价极其惨重：每次重建泄漏 17 个线程（泵线程被设计成
    // 永不 return，见 PumpProc 末尾）加一份设备、一个队列、几个帧池。拖动
    // 窗口十几次就是 187 个线程、250MB 工作集，而且这些残留对象都还在跟
    // DWM 打交道。主人报的现象完全对得上：
    //     「被捕捉的资源管理器出现非常严重的掉帧卡顿」
    //     「shadow-capture.exe 的 CPU 占用率太高了（40%）」
    //     「内存 800M，隔壁英伟达回放才 200M 不到」
    //
    // 改成"按屏幕尺寸建一次"就全没了。探针实测（probe_pool_grow.cpp，
    // 池 2560x1440）：窗口从 900x650 放大到 1600x930 再缩回来，**全程持续
    // 投帧**，纹理恒为 2560x1440，ContentSize 精确跟随窗口尺寸。所以缩放
    // 期间一帧都不用丢，也不需要任何重建。
    //
    // 代价是每帧的 GPU 纹理按屏幕尺寸算（1440p 一帧 14.7MB × 2 个缓冲）。
    // 这是 WGC 自己的显存开销，不进我们的内存统计；换来的是零重建。
    ::ABI::Windows::Graphics::SizeInt32 poolSize = size;
    {
        // 用虚拟桌面总尺寸兜住"窗口比屏幕还大"（跨屏拉伸）的情况。
        const int32_t vx = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int32_t vy = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int32_t vw = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int32_t vh = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
        (void)vy;
        const uint32_t screenW = static_cast<uint32_t>(vw > 0 ? vw : 1920);
        const uint32_t screenH = static_cast<uint32_t>(vh > 0 ? vh : 1080);
        (void)vx;
        // 留 1/8 余量：窗口被拖到比屏幕还大（用户把窗口拉出画面边缘时，
        // 窗口的 ContentSize 会超过屏幕尺寸）。
        const uint32_t wantW = screenW + screenW / 8u;
        const uint32_t wantH = screenH + screenH / 8u;
        if (static_cast<uint32_t>(poolSize.Width) < wantW) poolSize.Width = static_cast<int32_t>(wantW);
        if (static_cast<uint32_t>(poolSize.Height) < wantH)
            poolSize.Height = static_cast<int32_t>(wantH);
    }
    // 上限兜一下：4K 屏幕的 1.125 倍就是 4320x2430，再大没意义。
    if (poolSize.Width > 5120) poolSize.Width = 5120;
    if (poolSize.Height > 3072) poolSize.Height = 3072;

    // ---- 把裸 D3D11 设备包成 WinRT 的 IDirect3DDevice ---------------------
    ComPtr<IDXGIDevice> dxgiDevice;
    hr = device_->QueryInterface(__uuidof(IDXGIDevice),
                                 reinterpret_cast<void**>(dxgiDevice.GetAddressOf()));
    if (FAILED(hr)) {
        outError = "拿不到 IDXGIDevice";
        CleanupLocked();
        return false;
    }
    ComPtr<IInspectable> inspectable;
    hr = ::CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.GetAddressOf());
    if (FAILED(hr) || !inspectable) {
        outError = "CreateDirect3D11DeviceFromDXGIDevice 失败";
        CleanupLocked();
        return false;
    }
    ComPtr<::ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice> rtDevice;
    hr = inspectable.As(&rtDevice);
    if (FAILED(hr)) {
        outError = "拿不到 WinRT IDirect3DDevice";
        CleanupLocked();
        return false;
    }

    // ---- 起帧回调的投递线程 + 在这个线程上建池 ----------------------------
    //
    // 帧池把 FrameArrived 回调投递到**创建池那个线程**的 DispatcherQueue 上。
    // 池若建在 Session 的采集线程上，那线程只跑采集循环、不泵消息，
    // 回调永远送不到。所以起一个专职泵线程：建队列 -> 在同一个线程建池
    // （Create 抓的就是调用线程的队列）-> 一直泵消息。
    //
    // **为什么不用 CreateFreeThreaded**（踩过，别再走回头路）：
    //   本机 Win11 build 26200 实测
    //       CreateFreeThreaded + add_FrameArrived -> E_BOUNDS 0x8000001C
    //       Create             + add_FrameArrived -> S_OK
    //   CreateFreeThreaded 造的池会先 QI 调用方的 IAgileObject，拒绝后
    //   整条注册路径就断在 E_BOUNDS。界面上表现为
    //   「注册帧回调失败(hr=-2147483620)」，**和权限无关**（提权跑同样复现）。
    HSTRING poolCls = nullptr;
    const wchar_t* kPoolName = L"Windows.Graphics.Capture.Direct3D11CaptureFramePool";
    ::WindowsCreateString(kPoolName, static_cast<UINT32>(wcslen(kPoolName)), &poolCls);
    ComPtr<::ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePoolStatics> poolStatics;
    hr = ::RoGetActivationFactory(
        poolCls, __uuidof(::ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePoolStatics),
        reinterpret_cast<void**>(poolStatics.GetAddressOf()));
    ::WindowsDeleteString(poolCls);
    if (FAILED(hr) || !poolStatics) {
        outError = "拿不到 Direct3D11CaptureFramePool 工厂（hr=" +
                   std::to_string(hr) + "）";
        CleanupLocked();
        return false;
    }

    // **类名坑**：帧池的 WinRT 类名是
    //     Windows.Graphics.Capture.Direct3D11CaptureFramePool
    // 而**不是** ...GraphicsCaptureFramePool。写错会得到
    // REGDB_E_CLASSNOTREG (0x80040154)。记法：接口名去掉前导 I 就是类名。
    impl_->poolStatics = poolStatics;
    impl_->poolDevice = rtDevice;
    impl_->poolSize = poolSize;
    if (!impl_->CreatePoolOnPump(outError)) {
        CleanupLocked();
        return false;
    }

    // ---- 注册「有新帧」 ---------------------------------------------------
    // 回调委托自己手写，不用 WRL 的 Callback<>/RuntimeClass<>：
    //
    //   * Callback<> 不支持捕获 this，而我们要把 Impl* 传进回调；
    //   * RuntimeClass<I> 要求 I 派生自 IInspectable，但 WGC 的
    //     ITypedEventHandler<...> 底层是 ITypedEventHandler_impl : **IUnknown**
    //     （见 windows.graphics.capture.h 的特化），直接拿它当 RuntimeClass
    //     的模板参数会 static_assert 报
    //     "'I' has to derive from 'IWeakReference', 'IWeakReferenceSource' or 'IInspectable'"。
    //
    // 手写反而最短：这俩委托只有 Invoke 一个方法，QueryInterface 只认自己的 IID。
    class FrameHandlerImpl final : public FrameArrivedHandler {
    public:
        explicit FrameHandlerImpl(Impl* owner) : owner_(owner) {}

        // ---- IUnknown ----
        STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
            if (!ppv) return E_POINTER;
            *ppv = nullptr;
            // 必须用硬编码的 kFrameArrivedIid —— 见文件上方那段注释，
            // __uuidof(FrameArrivedHandler) 取到的不是 WinRT 要的那个 IID。
            if (IsEqualIID(riid, kFrameArrivedIid) || IsEqualIID(riid, __uuidof(::IUnknown))) {
                *ppv = static_cast<FrameArrivedHandler*>(this);
                AddRef();
                return S_OK;
            }
            return E_NOINTERFACE;
        }
        STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
        STDMETHODIMP_(ULONG) Release() override {
            const ULONG n = --refs_;
            if (n == 0) delete this;
            return n;
        }

        // ---- ITypedEventHandler ----
        STDMETHODIMP Invoke(CaptureFramePool*, ::IInspectable*) override {
            owner_->frameArrived.store(true, std::memory_order_release);
            return S_OK;
        }

    private:
        ~FrameHandlerImpl() = default;
        std::atomic<ULONG> refs_{1};
        Impl* owner_;
    };

    class ClosedHandlerImpl final : public ItemClosedHandler {
    public:
        explicit ClosedHandlerImpl(Impl* owner) : owner_(owner) {}

        STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
            if (!ppv) return E_POINTER;
            *ppv = nullptr;
            if (IsEqualIID(riid, kItemClosedIid) || IsEqualIID(riid, __uuidof(::IUnknown))) {
                *ppv = static_cast<ItemClosedHandler*>(this);
                AddRef();
                return S_OK;
            }
            return E_NOINTERFACE;
        }
        STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
        STDMETHODIMP_(ULONG) Release() override {
            const ULONG n = --refs_;
            if (n == 0) delete this;
            return n;
        }

        STDMETHODIMP Invoke(CaptureItem*, ::IInspectable*) override {
            owner_->closed.store(true, std::memory_order_release);
            return S_OK;
        }

    private:
        ~ClosedHandlerImpl() = default;
        std::atomic<ULONG> refs_{1};
        Impl* owner_;
    };

    // 初始引用计数 1 直接交给 ComPtr 接管，不用 AddRef。
    impl_->frameHandler.Attach(new FrameHandlerImpl(impl_.get()));
    impl_->closedHandler.Attach(new ClosedHandlerImpl(impl_.get()));

    hr = impl_->pool->add_FrameArrived(impl_->frameHandler.Get(), &impl_->frameToken);
    if (FAILED(hr)) {
        outError = "注册帧回调失败（hr=" + std::to_string(hr) + "）";
        CleanupLocked();
        return false;
    }
    impl_->frameHooked = true;

    // ---- 建会话、注册关闭事件、开录 ---------------------------------------
    hr = impl_->pool->CreateCaptureSession(impl_->item.Get(), impl_->session.GetAddressOf());
    if (FAILED(hr) || !impl_->session) {
        outError = "创建捕获会话失败（hr=" + std::to_string(hr) + "）";
        CleanupLocked();
        return false;
    }
    hr = impl_->item->add_Closed(impl_->closedHandler.Get(), &impl_->closedToken);
    if (FAILED(hr)) {
        outError = "注册窗口关闭事件失败";
        CleanupLocked();
        return false;
    }
    impl_->closedHooked = true;

    // ---- 关掉光标与边框 -------------------------------------------------
    //
    // 必须在 **StartCapture 之前**设：这两个属性是会话启动时读的，启动后再
    // 改不生效（put_ 会返回成功但画面里照旧有光标，很容易误以为没生效）。
    //
    // 用 As() 拿 Session2/Session3 而不是硬转：老系统上这两个接口可能不存在，
    // 取不到就当"系统不支持"，跳过即可 —— 绝不能因为缺接口让整路采集失败。
    //
    // 关光标：WGC 默认会把鼠标指针一起合成进去（还带个投影），录屏里出现
    // 一个到处乱窜的箭头纯属干扰。桌面复制那一路本来就不含光标，这里对齐。
    {
        ComPtr<CaptureSession2> s2;
        const HRESULT hrAs = impl_->session.As(&s2);
        if (SUCCEEDED(hrAs) && s2) {
            const HRESULT hrCur = s2->put_IsCursorCaptureEnabled(FALSE);
            // 顺手把结果记进 cursorOffHr_，方便之后从 LastError 侧的诊断里看到。
            // 这个调用失败时 put_ 不会让 StartCapture 失败，画面里照旧有光标，
            // 事后只能靠这个值判断到底是不是没设上。
            impl_->cursorOffHr = hrCur;
        } else {
            // 拿不到 Session2 就关不掉光标。老系统（Win10 2004 之前）本来
            // 就没有这个接口，但 Win11 上出现这一条说明有问题，要能看见。
            impl_->cursorOffHr = hrAs;
        }
    }
    // 关黄框：Win11 起窗口捕获会在窗口四周画一圈黄色边框，那是给用户看的
    // "这个窗口正在被录"的隐私提示。录进分片里就是一个永久黄框，必须去掉。
    {
        ComPtr<CaptureSession3> s3;
        if (SUCCEEDED(impl_->session.As(&s3)) && s3) {
            s3->put_IsBorderRequired(FALSE);
        }
    }

    hr = impl_->session->StartCapture();
    if (FAILED(hr)) {
        outError = "StartCapture 失败（hr=" + std::to_string(hr) + "）";
        CleanupLocked();
        return false;
    }

    valid_ = true;
    outError.clear();
    return true;
}

void WindowCapture::CleanupLocked() {
    if (impl_) {
        // **第一步永远是停泵线程**，不是解注册回调。
        //
        // 踩过的坑：原来先 remove_FrameArrived 再停泵。可那一刻泵线程还在
        // DispatchMessage 里跑 FrameArrived 回调，事件订阅表正被它读着；
        // 主线程同时去 remove，WinRT 内部那套表就撞了 —— 实测热重载一路
        // 正在跑的 WGC 会话，服务直接 0xC0000005 静默退出。
        //
        // StopPump() 只负责把泵线程叫停（确认它不再 Dispatch），**不销毁
        // 队列** —— 下面那串 remove / Close / Reset 全要跟创建帧池的队列
        // 打交道，队列必须活着。销毁队列放到最后 FinishPumpTeardown() 里。
        impl_->StopPump();

        if (impl_->frameHooked && impl_->pool) {
            impl_->pool->remove_FrameArrived(impl_->frameToken);
            impl_->frameHooked = false;
        }
        if (impl_->closedHooked && impl_->item) {
            impl_->item->remove_Closed(impl_->closedToken);
            impl_->closedHooked = false;
        }
        // IGraphicsCaptureSession 本身没有 Close()，得 QI 成 IClosable。
        if (impl_->session) {
            ComPtr<Closable> closable;
            if (SUCCEEDED(impl_->session.As(&closable)) && closable) {
                closable->Close();
            }
        }
        impl_->session.Reset();

        impl_->pool.Reset();
        impl_->item.Reset();
        impl_->frameHandler.Reset();
        impl_->closedHandler.Reset();
        impl_->frameArrived.store(false);
        impl_->closed.store(false);

        // ---- 最后一步：这时才可以销毁 DispatcherQueue ------------------
        //
        // 上面那一串（remove_FrameArrived / remove_Closed / session Close /
        // pool Reset）都做完了，帧池已经没人用，才有资格让泵线程去 Reset
        // dqController。
        //
        // 顺序反了就是 0xC0000005：本鱼实测过。
        impl_->FinishPumpTeardown();
    }
    if (staging_) { staging_->Release(); staging_ = nullptr; }
    stagingW_ = stagingH_ = 0;
    if (context_) { context_->Release(); context_ = nullptr; }
    if (device_)  { device_->Release();  device_ = nullptr;  }
    valid_ = false;
    hwnd_ = nullptr;
    hwndPid_ = 0;
    width_ = height_ = 0;
    lastPresentQpc_ = 0;
    // 尺寸稳定判定的中间状态跟着清掉：新会话的池尺寸是全新的，
    // 留着上一轮的"疑似新尺寸"会让第一拍就误判成又变了。
    pendingW_ = 0;
    pendingH_ = 0;
    pendingSince_ = 0;
    resizeNotice_ = false;
}

void WindowCapture::Cleanup() {
    std::lock_guard<std::mutex> lk(mu_);
    CleanupLocked();
}

bool WindowCapture::TakeResizeNotice() {
    std::lock_guard<std::mutex> lk(mu_);
    const bool had = resizeNotice_;
    resizeNotice_ = false;
    return had;
}

bool WindowCapture::CreateFramePool() { return true; } // 实际在 Init 里做，留着占位

bool WindowCapture::TargetAliveLocked() const {
    if (!hwnd_) return false;
    const HWND h = static_cast<HWND>(hwnd_);
    if (!::IsWindow(h)) return false;
    // 【关掉窗口 ≠ 销毁窗口】
    //
    // Chromium 系（浏览器 / Electron 客户端）关窗口常常只是 ShowWindow(SW_HIDE)：
    // 句柄一直有效、pid 也没变，但合成器不再投帧。只判 IsWindow 的话这里会
    // 永远返回"活着"，上层就一直重发最后一帧、状态永远 live —— 用户看到的是
    // "窗口都关了，画面定在那不动，也不弹 NO SIGNAL"。
    //
    // 最小化不受影响：IsWindowVisible 对最小化窗口仍然为真。
    if (!::IsWindowVisible(h)) return false;
    // 句柄还活着，但可能早就是别人的窗口了（HWND 会复用）。
    // 只比 pid 不比较线程：explorer.exe 的窗口会被不同 UI 线程创建。
    if (hwndPid_ != 0) {
        DWORD pid = 0;
        ::GetWindowThreadProcessId(h, &pid);
        if (pid != hwndPid_) return false;
    }
    return true;
}

CaptureStatus WindowCapture::Capture(FrameView& out, uint8_t* buffer, size_t bufferSize) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!valid_ || !impl_ || !impl_->pool) return CaptureStatus::Error;

    if (impl_->closed.load(std::memory_order_acquire)) {
        lastError_ = "被捕获的窗口已关闭";
        return CaptureStatus::AccessLost;
    }

    // WGC 的 ItemClosed 不可靠（实测关掉资源管理器窗口时它不触发），
    // 所以每拍自己查一遍窗口还在不在。
    //
    // 这个判据必须放在"没有新帧就返回 NoChange"**之前**：窗口关掉之后
    // FrameArrived 也停了，如果先判 frameArrived，永远走的是 NoChange，
    // 上层就会把"窗口没了"当成"窗口静止"，一直重发最后一帧。
    if (!TargetAliveLocked()) {
        lastError_ = "被捕获的窗口已关闭";
        return CaptureStatus::AccessLost;
    }

    // 没有新帧就直接说没有 —— WGC 只在窗口重绘时投帧，
    // 窗口静止时这里持续返回 NoChange，由调用方重复上一帧。
    if (!impl_->frameArrived.exchange(false, std::memory_order_acq_rel)) {
        return CaptureStatus::NoChange;
    }

    ComPtr<CaptureFrame> frame;
    HRESULT hr = impl_->pool->TryGetNextFrame(frame.GetAddressOf());
    if (FAILED(hr) || !frame) return CaptureStatus::NoChange;

    // ---- 这一帧里真正的画面尺寸（窗口内容尺寸）---------------------------
    //
    // 帧池是按屏幕尺寸建的（见 Init 里那段说明），纹理永远是池子那么大；
    // 窗口只有左上角 content 那么大一块是真内容，其余是没动过的填充。
    // 所以**每一帧**都要问一次 ContentSize，只把左上角这块交出去。
    //
    // 这就是"窗口缩放不需要重建会话"的关键：窗口拖大拖小只是 content 变，
    // 纹理和池子都不动，投帧一分钟都不中断，尺寸零成本跟随。
    uint32_t contentW = 0;
    uint32_t contentH = 0;
    {
        ::ABI::Windows::Graphics::SizeInt32 content{};
        if (SUCCEEDED(frame->get_ContentSize(&content)) && content.Width > 0 &&
            content.Height > 0) {
            contentW = static_cast<uint32_t>(content.Width);
            contentH = static_cast<uint32_t>(content.Height);
        }
    }
    // ContentSize 拿不到就退回上次知道的尺寸（宁可交旧尺寸的帧，也别丢帧）。
    if (contentW == 0 || contentH == 0) {
        contentW = width_.load(std::memory_order_relaxed);
        contentH = height_.load(std::memory_order_relaxed);
    }
    if (contentW == 0 || contentH == 0) return CaptureStatus::NoChange;

    // 尺寸变了就更新对外宣称的宽高。**不重建任何东西**，下一次读 Width()
    // 就是新值 —— Session 那边据此重建帧环和录制器（那两样是真改不了）。
    const uint32_t prevW = width_.load(std::memory_order_relaxed);
    const uint32_t prevH = height_.load(std::memory_order_relaxed);
    if (prevW != contentW || prevH != contentH) {
        width_.store(contentW, std::memory_order_relaxed);
        height_.store(contentH, std::memory_order_relaxed);
    }

    ComPtr<::ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface> surface;
    hr = frame->get_Surface(surface.GetAddressOf());
    if (FAILED(hr) || !surface) {
        ++errorCount_;
        return CaptureStatus::Error;
    }

    int64_t qpc = 0;
    ::ABI::Windows::Foundation::TimeSpan ts{};
    if (SUCCEEDED(frame->get_SystemRelativeTime(&ts))) qpc = ts.Duration;
    out.presentQpc = qpc;
    lastPresentQpc_ = qpc;

    // ---- 把 WinRT surface 掰成 ID3D11Texture2D ---------------------------
    ComPtr<DxgiAccess> access;
    hr = surface.As(&access);
    if (FAILED(hr) || !access) {
        ++errorCount_;
        return CaptureStatus::Error;
    }
    ComPtr<ID3D11Texture2D> tex;
    hr = access->GetInterface(__uuidof(ID3D11Texture2D),
                              reinterpret_cast<void**>(tex.GetAddressOf()));
    if (FAILED(hr) || !tex) {
        ++errorCount_;
        return CaptureStatus::Error;
    }

    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);

    // 纹理是帧池那么大（屏幕尺寸），但只有左上角 content 那块是真内容。
    // 超出纹理就不能拷（池子被别的尺寸撑过之后可能不够），按纹理兜回来。
    const uint32_t w = std::min(contentW, desc.Width);
    const uint32_t h = std::min(contentH, desc.Height);

    const size_t need = static_cast<size_t>(w) * 4u * h;
    if (!buffer || bufferSize < need) {
        ++errorCount_;
        lastError_ = "调用方缓冲不够大";
        return CaptureStatus::Error;
    }

    // ---- 拷进自己的 staging（CPU 可映射） --------------------------------
    // 不能直接 Map 帧池给的纹理：它是 DWM 的，且下一帧回收后内容就变了。
    //
    // staging 按**容量**建（不是按这一次的内容尺寸），容量向上取整到 128
    // 像素并对齐，只在装不下时才重建。
    //
    // 原来这里是"内容尺寸变了就重建一张"。听起来很省，但拖动窗口时 WGC
    // 每帧报一个不同的 ContentSize（1 像素的抖动也算），于是每帧都
    // CreateTexture2D 一次 —— 那是 GPU 资源创建，在拖拽那条路径上相当于
    // 每帧多一次设备分配/销毁，实测就是"过渡不够丝滑"。
    // 取整到 128 之后，正常拖动（几百像素以内）一张纹理都不必重建。
    const uint32_t capW = ((w + 127u) / 128u) * 128u;
    const uint32_t capH = ((h + 127u) / 128u) * 128u;
    if (!staging_ || stagingW_ < w || stagingH_ < h) {
        if (staging_) { staging_->Release(); staging_ = nullptr; }
        D3D11_TEXTURE2D_DESC sd = desc;
        sd.Width = capW < w ? w : capW;
        sd.Height = capH < h ? h : capH;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        // MipLevels 必须跟着 desc 走（帧池纹理是 1），否则 CreateTexture2D
        // 可能返回 E_INVALIDARG。
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.SampleDesc.Count = 1;
        sd.SampleDesc.Quality = 0;
        if (FAILED(device_->CreateTexture2D(&sd, nullptr, &staging_))) {
            ++errorCount_;
            lastError_ = "创建 staging 纹理失败";
            return CaptureStatus::Error;
        }
        stagingW_ = sd.Width;
        stagingH_ = sd.Height;
    }

    // 只拷左上角那一块：D3D11_BOX 的 right/bottom 是**开区间**（right 不包含
    // 在内），写成 w-1/h-1 会少一列一行，画面右边和下边各留一条黑线。
    D3D11_BOX box{};
    box.left = 0;
    box.top = 0;
    box.right = w;
    box.bottom = h;
    box.front = 0;
    box.back = 1;
    context_->CopySubresourceRegion(staging_, 0, 0, 0, 0, tex.Get(), 0, &box);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(staging_, 0, D3D11_MAP_READ, 0, &mapped))) {
        ++errorCount_;
        return CaptureStatus::Error;
    }

    // 统一压成紧密排布：源行距通常比 w*4 大（D3D 按 256 字节对齐），
    // 照着 RowPitch 拷下游会得到一条斜掉的画面。用 non-temporal 存储
    // 避免把这份一次性数据灌进 L3 挤掉采集线程正在用的东西。
    const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
    const size_t rowBytes = static_cast<size_t>(w) * 4u;
    if (mapped.RowPitch == rowBytes) {
        CopyRowStreaming(buffer, src, rowBytes * h);
    } else {
        for (uint32_t y = 0; y < h; ++y) {
            CopyRowStreaming(buffer + static_cast<size_t>(y) * rowBytes,
                             src + static_cast<size_t>(y) * mapped.RowPitch, rowBytes);
        }
    }
    context_->Unmap(staging_, 0);

    out.pixels = buffer;
    out.width = w;
    out.height = h;
    out.stride = w * 4u;
    out.accumulatedFrames = 1;
    return CaptureStatus::Ok;
}

} // namespace shadow
