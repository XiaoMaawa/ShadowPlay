// ScreenCapture.h — 基于 DXGI 桌面复制的整屏采集（纯采集，不负责渲染）
#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace shadow {

// 一帧采集结果。pixels 指向 staging 纹理映射后的内存，只在本次循环内有效。
struct FrameView {
    const uint8_t* pixels = nullptr; // 第一行是屏幕最上面一行
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;    // 每行字节数 == width * 4
    int64_t  presentQpc = 0; // 该帧的呈现时间（QPC），来自 DXGI
    uint32_t accumulatedFrames = 0; // 自上次采集以来桌面累积的帧数
};

enum class CaptureStatus {
    Ok,           // 拿到一帧，out 有效
    NoChange,     // 目标帧率未到 或 桌面没有新内容
    AccessLost,   // UAC / 锁屏 / 分辨率切换 / 切换到独占全屏 —— 需要重建
    Error,        // 其它错误，已记录
};

// 一台可选来录的显示器。
//
// 为什么单独给界面一份：界面上「显示器捕获」是个下拉框，用户要看到
// 「显示器 1 · 2560×1440 · 主屏」这种字，而不是自己数序号。序号必须跟
// `ScreenCapture::Init(outputIndex)` 用的那套完全一致 —— 两者都走
// `CollectAllOutputs`（只数 AttachedToDesktop 的输出，按上→下、左→右排），
// 所以这里的 index 直接就是 Init 的 outputIndex。
struct MonitorInfo {
    uint32_t index = 0;        // 传给 ScreenCapture::Init 的显示器序号
    int32_t  left = 0;         // 桌面坐标（虚拟屏幕内）
    int32_t  top = 0;
    uint32_t width = 0;        // 物理像素
    uint32_t height = 0;
    bool     primary = false;  // 主显示器（原点所在的那块）
    std::string name;          // UTF-8，例如 \\.\DISPLAY1
};

// 枚举当前已连接的显示器。序号与 Init(outputIndex) 一一对应。
std::vector<MonitorInfo> EnumerateMonitors();

class ScreenCapture {
public:
    ScreenCapture() = default;
    ~ScreenCapture();

    ScreenCapture(const ScreenCapture&) = delete;
    ScreenCapture& operator=(const ScreenCapture&) = delete;

    bool Init(uint32_t outputIndex = 0, uint32_t timeoutMs = 200);
    void Cleanup();

    // 采集一帧并**同步把像素拷进 out 自己的缓冲**（见 Freeze）。
    //
    // 返回 Ok 时 out.pixels 指向调用方传进来的 buffer，内容在这次调用返回后
    // 依然有效 —— 这是线程安全的前提：多路会话共用一个 ScreenCapture 时，
    // 谁都不能拿指向内部 staging 的裸指针回去慢慢用（那样下一路一 CopyResource
    // 就把它盖了）。所以这里要求调用方给缓冲，我们拷完再放手。
    //
    // buffer 为 nullptr/太小时按 size 参数判断，返回 Error。
    CaptureStatus Capture(FrameView& out, uint8_t* buffer, size_t bufferSize);

    // 与 Capture() 同义，但要求 buffer 是一个 vector：会在内部把「最近一张」
    // 与它**交换**（见 SwapLastFrame），从而省掉一次整屏拷贝。
    //
    // 单路会话请走这个。用裸指针的 Capture() 只是为了兼容预览路径。
    CaptureStatus CaptureInto(FrameView& out, std::vector<uint8_t>& buffer);

    // ---- 多路同源：一次采集，N 路共享 --------------------------------------
    //
    // 桌面源的硬约束：一个显示器只有一个 IDXGIOutputDuplication，而
    // AcquireNextFrame 在同一时刻只允许一路持有。多路各采各的时，实际行为是
    // 轮流抢锁 —— 每路都要等一个真实的新帧（60Hz 屏就是 16.7ms），三路同源
    // 实测帧率从 60.0 塌到 50~54，采集耗时从 3.6ms 涨到 16ms。
    //
    // 这里让每路带上自己的 seenEpoch：谁先醒谁去采（并推进 epoch），后醒的路
    // 一看 epoch 比自己的新，就直接把「最近一张」拷走，**duplication 一次都不碰**。
    // 于是同一帧只采一次、其余路纯 memcpy，同源路数不再互相拖累。
    //
    // 为什么不像 CaptureInto 那样 swap：那会让 lastFrame_ 被换走，
    // 第二个来吃现成的路就吃到垃圾。共享路径必须是只读的 memcpy。
    //
    // 返回值语义与 CaptureInto 相同。seenEpoch 由调用方持有，初始给 0。
    //
    // 和 CaptureInto 一样**不加锁**：调用方必须已经持有 Mutex()。
    CaptureStatus CaptureSharedInto(FrameView& out, std::vector<uint8_t>& buffer,
                                    uint64_t& seenEpoch);

    // 释放当前持有的 duplication 帧。必须在处理完 pixels 之后调用。
    //
    // 注意：Capture() 现在**内部**已经把它调掉了（它自己在用完 staging 之后
    // 就释放），所以走 Capture 的调用方不用再管。这两个留着是给那些不用
    // Capture、直接操作帧环的路径用的（main.cpp 的 --once 预览）。
    void ReleaseFrame();

    /**
     * 释放 duplication 帧，但保留最近一帧的内容。
     *
     * 桌面静止时（DXGI 返回 NoChange）我们需要把最后那一帧重复发布出去，
     * 让下游有连续的帧流。现在「最近一帧」是 ScreenCapture 自己攥的私有副本
     * （lastFrame_），不再依赖 staging 的映射还开着。
     */
    void ReleaseFrameKeepLast();

    // ---- 给"多路共享一个采集器"用的冻结快照 -------------------------------
    //
    // 每一路会话的循环从 AcquireSnapshot() 取一张**只属于自己**的帧：
    //   * 先看看自上次以来有没有新帧（有就用新的，没有就用上一张）；
    //   * 无论新旧，都把像素拷进 outBuffer（stride 统一 = width*4，无行距）。
    // 返回 false 表示当前没有可用帧。
    //
    // 为什么必须拷贝而不是返回指针：staging 是一块**共享**的纹理，下一路
    // 一 CopyResource 就把内容换了；把裸指针交出去，等于交出随时会变的内存。
    // 拷一次的成本（1080p 约 8MB）远小于让画面撕裂/读到半张新半张旧的代价。
    bool AcquireSnapshot(uint8_t* outBuffer, size_t outBufferSize, uint32_t& outW,
                         uint32_t& outH, int64_t& outPresentQpc, bool& outIsNew);

    // 把「最近一张」重放给调用方。**调用方必须已经持有 Mutex()。**
    // 给那些先自己 Capture()、遇到 NoChange 再想要一份重放帧的调用方用
    // （AcquireSnapshot 是它的封装版，会把 Capture 一起做掉）。
    bool AcquireReplayLocked(uint8_t* outBuffer, size_t outBufferSize, uint32_t& outW,
                             uint32_t& outH, int64_t& outPresentQpc);

    /** 最近一次成功采集的像素（staging 映射内存），没有则为 nullptr */
    const uint8_t* LastPixels() const { return lastPixels_; }
    bool HasLastFrame() const { return lastPixels_ != nullptr; }
    uint32_t LastStride() const { return lastStride_; }

    bool  HasFrame() const { return frameAcquired_; }
    uint32_t Width()  const { return width_; }
    uint32_t Height() const { return height_; }
    uint32_t Stride() const { return width_ * 4u; }

    // 采集端错误的累计数量（用于上报给前端）
    uint32_t ErrorCount() const { return errorCount_; }

    // ---- 鼠标指针开关 ------------------------------------------------------
    //
    // 只影响桌面源：窗口源用 WGC，那边由系统自己决定画不画（本项目对窗口源
    // 一律关掉光标，见 WindowCapture 的 put_IsCursorCaptureEnabled(FALSE)）。
    //
    // 默认开着 —— 录桌面时用户几乎总是希望画面里能看到鼠标。
    void SetDrawCursor(bool on) {
        drawCursor_ = on;
        if (!on) {
            cursorVisible_ = false;
            cursorBlends_ = 0;
        }
    }
    bool DrawCursor() const { return drawCursor_; }

    /** 累计把光标叠进画面的次数（诊断用：为 0 说明一次都没叠上） */
    uint64_t CursorBlends() const { return cursorBlends_; }

    /** 当前光标形状的尺寸（诊断用，未拿到形状时为 0x0） */
    void CursorShapeSize(uint32_t& w, uint32_t& h, uint32_t& type) const {
        w = cursorValid_ ? shape_.width : 0;
        h = cursorValid_ ? shape_.height : 0;
        type = cursorValid_ ? shape_.type : 0;
    }

    // **每一个**使用这个采集器的会话线程都必须先持有它。
    //
    // 踩过的坑：加锁之前，N 路配置共享同一个 ScreenCapture（main.cpp 起会话时
    // 传的是 desktop.get()），于是多线程同时对同一个 duplication_ 调
    // AcquireNextFrame、同时往同一块 staging_ 纹理 CopyResource、
    // frameAcquired_/lastPixels_ 互相覆盖。表现出来就是帧率忽高忽低（掉到 28）、
    // 画面花屏、喂给编码器的像素本身是另一路正在写的。
    std::mutex& Mutex() { return mu_; }

private:
    // 按当前源的描述创建/重建 CPU 可读的 staging 纹理
    bool EnsureStaging(const D3D11_TEXTURE2D_DESC& srcDesc);

    // 清理的**不加锁**版本。给 Init() 用（它自己持锁），外部一律调 Cleanup()。
    // 拆开是为了避开同一个非递归 mutex 的二次加锁 —— 那是直接死锁。
    void CleanupLocked();

    // 重放「最近一张」到调用方的缓冲。调用方必须已经持有 mu_。
    bool ReplayLastLocked(uint8_t* outBuffer, size_t outBufferSize, uint32_t& outW,
                          uint32_t& outH, int64_t& outPresentQpc, bool& outIsNew);

    std::mutex               mu_;
    ID3D11Device*            device_ = nullptr;
    ID3D11DeviceContext*     context_ = nullptr;
    IDXGIOutputDuplication*  duplication_ = nullptr;
    ID3D11Texture2D*         staging_ = nullptr;
    uint32_t outputIndex_ = 0;
    uint32_t timeoutMs_ = 200;
    uint32_t width_ = 0;
    uint32_t height_ = 0;

    // ---- 鼠标指针（只给桌面源用）-----------------------------------------
    //
    // DXGI 桌面复制**不会**把光标画进帧里 —— 屏幕上那个光标是 DWM 单独合成
    // 的一层。要录进去就得自己叠。Desktop Duplication 每帧会告诉我们光标在
    // 哪、显示还是隐藏（PointerPosition）、形状要不要换（PointerShapeBufferSize）。
    //
    // 实测定下来的三条语义（native/probe_cursor.cpp 跑出来的，别凭文档猜）：
    //   * PointerPosition.Position 是光标位图**左上角的桌面绝对坐标**，热区已经
    //     扣掉了：SetCursorPos(100,100) + HotSpot(10,0) 时报的是 (90,100)。
    //     所以不需要自己累加位移。
    //   * 这一帧没有光标更新时，LastMouseUpdateTime 为 0，同时 PointerPosition
    //     是 (0,0)/Visible=FALSE。**必须靠 LastMouseUpdateTime 判**，否则每帧都
    //     按"不可见"处理，录出来的光标会疯狂闪。
    //   * 形状只在变的时候才带（PointerShapeBufferSize > 0），平时是 0，得把上
    //     一次的形状留着继续用。
    struct CursorShape {
        std::vector<uint8_t> pixels;
        uint32_t type = 0;           // DXGI_OUTDUPL_POINTER_SHAPE_TYPE_*
        uint32_t width = 0;
        uint32_t height = 0;         // 单色时是**双倍**高度（AND + XOR 两段）
        uint32_t pitch = 0;          // 源行宽；COLOR 时恰好 = width*4
        int32_t  hotX = 0;           // 仅记录，位置不需要它
        int32_t  hotY = 0;
    };
    CursorShape shape_;
    bool        cursorValid_ = false;   // shape_ 里有没有可用形状
    bool        cursorVisible_ = false; // 最近一次光标更新说它可见
    int64_t     cursorX_ = 0;           // 光标位图左上角（桌面绝对坐标）
    int64_t     cursorY_ = 0;
    bool        drawCursor_ = true;     // false = 不叠光标
    uint64_t    cursorBlends_ = 0;      // 叠了多少次（诊断用）

    // 这块输出的桌面原点。光标位置是全局桌面坐标，要减掉它才是输出内坐标。
    int64_t originX_ = 0;
    int64_t originY_ = 0;

    // 从帧信息里更新光标状态（位置 / 可见性 / 形状）。
    void UpdateCursorState(const DXGI_OUTDUPL_FRAME_INFO& info);

    // 把光标按当前状态叠进 dst（紧密排布 BGRA，stride = dstW*4）。
    void BlendCursorInto(uint8_t* dst, uint32_t dstW, uint32_t dstH) const;

    uint32_t stagingW_ = 0;   // staging 纹理当前尺寸（用于判断是否需要重建）
    uint32_t stagingH_ = 0;
    uint32_t stagingFmt_ = 0;
    DXGI_FORMAT stagingSrcFormat_ = DXGI_FORMAT_UNKNOWN;

    bool     frameAcquired_ = false;
    uint32_t errorCount_ = 0;

    // 最近一帧的 staging 映射信息（供桌面静止时重复发布）
    const uint8_t* lastPixels_ = nullptr;
    uint32_t lastStride_ = 0;

    // ---- 共享采集器用的「最近一张」私有副本 -------------------------------
    //
    // 桌面静止（NoChange）的那一拍必须让**每一路**都拿到完整一帧，而 staging
    // 只有一份、还会被下一路覆盖 —— 所以留这份副本供重放。
    //
    // 关键省钱点：这份副本**不是重新拷一遍**得来的。Capture() 本来就要把像素
    // 逐行拷进调用方的 buffer，而内容与"最近一张"完全相同；于是这里直接用
    // lastFrame_ 接管调用方那块缓冲（swap），把调用方的 buffer 换成原来
    // lastFrame_ 持有的那块内存继续用。
    //
    // 为什么能行：每次采集都在同一把锁里，且每次交换后 lastFrame_ 的实际内容
    // 都等于刚拷进去的像素 —— 语义完全等价，却省掉了每拍一次整屏
    // （2560x1440 就是 14.7MB）的「读+写」。三路并发时这一项就是数 GB/s 的差别，
    // 实测把 captureMs 从 16.3ms 压回 7ms 量级。
    //
    // 代价：lastFrame_ 实际持有的内存块会在两处之间轮换，所以它的 data()
    // 不稳定 —— 但没有任何调用方依赖这个指针跨调用存活。
    std::vector<uint8_t> lastFrame_;
    uint32_t lastW_ = 0;
    uint32_t lastH_ = 0;
    int64_t  lastEpochQpc_ = 0;
    // 采集成功次数。诊断用：每路会话看到的 epoch 应该整体一致地增长。
    uint64_t epoch_ = 0;

    // 供 Capture() 在锁内使用：把调用方的 buffer 收进 lastFrame_，
    // 同时把 lastFrame_ 原来的内存换回给调用方。需要 buffer 是一个 vector，
    // 所以走这个入口的调用方用 CaptureInto()。
    void SwapLastFrame(std::vector<uint8_t>& buffer, uint32_t w, uint32_t h, int64_t qpc);
};

} // namespace shadow
