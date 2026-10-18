// WindowCapture.h — 基于 Windows Graphics Capture (WGC) 的单窗口采集
//
// 为什么需要它：
//   原先窗口源走的是「抓整屏 + 按窗口矩形裁剪」。那个做法有个绕不过去的
//   缺陷 —— 桌面复制拿到的是**合成后**的画面，窗口被别的窗口挡住时，
//   那块像素里根本就是遮挡物。用户的原话："我需要的是被挡住也能正常录制"。
//
//   WGC（Windows 10 1803 引入、1903 起直方可用）能向 DWM 直接要一个窗口
//   的独立画面。它不看遮挡、不看窗口是不是在屏幕外，甚至最小化的窗口
//   （1903+）也能拿到内容。
//
// 设计取舍：
//   * 全动态（COM 接口全靠手写 vtable），不引 C++/WinRT —— 这个项目
//     一直是「只依赖系统 SDK」，不想为了一个采集器把 WinRT 拖进来。
//   * 接口不可用时 Init 返回 false 并给原因，调用方安静退回裁剪法。
//     老系统上没有 WGC 是常态，不该让整个会话挂掉。
//   * 和 ScreenCapture 一样：像素**同步拷进调用方缓冲**，不把内部
//     纹理的裸指针交出去。
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "ScreenCapture.h" // CaptureStatus / FrameView

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct ID3D11RenderTargetView;
struct IDXGISwapChain1;
struct IMFSample;

namespace shadow {

class WindowCapture {
public:
    WindowCapture();
    ~WindowCapture();

    WindowCapture(const WindowCapture&) = delete;
    WindowCapture& operator=(const WindowCapture&) = delete;

    /**
     * 开始捕获 hwnd。
     *
     * 失败时 outError 带上人能看懂的原因（系统太老 / 不是合法窗口 /
     * 这一版 WGC 不支持该窗口类型），并把自身恢复到未初始化状态。
     */
    bool Init(void* hwnd, std::string& outError);

    void Cleanup();

    /**
     * 取一帧。像素同步拷进 buffer（stride 统一 = width*4，无行距）。
     *
     * 语义和 ScreenCapture::Capture 一致：
     *   * Ok        —— 有新内容，buffer 里是这一帧
     *   * NoChange  —— 自上次以来窗口没重绘，调用方该拿上一帧重复发
     *   * Error     —— 窗口没了 / 采集丢了，调用方应考虑重建
     *   * AccessLost—— 窗口被销毁，必须重新匹配
     */
    CaptureStatus Capture(FrameView& out, uint8_t* buffer, size_t bufferSize);

    bool     Valid() const { return valid_; }
    uint32_t Width()  const { return width_; }
    uint32_t Height() const { return height_; }
    void*    Hwnd()   const { return hwnd_; }

    uint32_t ErrorCount() const { return errorCount_; }
    const std::string& LastError() const { return lastError_; }

    // 多路共享时用不上（每个窗口各自一个实例），但保留同款语义，
    // 让 Session 里的加锁代码对两种采集器保持一致。
    std::mutex& Mutex() { return mu_; }

private:
    bool CreateDevice();
    bool CreateFramePool();
    // 从 IMFSample 里取出 D3D 纹理。
    bool TextureFromSample(IMFSample* sample, ID3D11Texture2D** outTex);
    void CleanupLocked();

    struct Impl; // 藏着 WinRT 那套 COM 细节，头文件不暴露

    // 被捕获窗口是不是还在（判 AccessLost 用）。调用方必须已经持有 mu_。
    //
    // 【为什么不能只靠 WGC 的 ItemClosed 事件】实测过：把一个资源管理器
    // 窗口关掉之后，ItemClosed 回调**没有触发**，closed 一直是 false，
    // 而 FrameArrived 也不再投帧。于是 Capture() 一直返回 NoChange，
    // 上层 Session 把 NoChange 理解成"窗口静止"，继续按目标帧率喂旧帧
    // 给录制器 —— 状态看着还是 live、fps 还是满的，实际画面和录像全都
    // 冻在窗口关闭前的那一帧，界面上表现为"画面停在最后一帧，也不重连"。
    // 所以每拍都自己查一遍窗口还在不在，别指望事件。
    bool TargetAliveLocked() const;

    std::mutex mu_;
    bool     valid_ = false;
    void*    hwnd_ = nullptr;
    // Init 时这个窗口所属的进程。光看 IsWindow 不够 —— HWND 会被系统复用，
    // 窗口关掉之后同一个句柄值可能已经属于别的程序了。
    unsigned long hwndPid_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t errorCount_ = 0;
    std::string lastError_;
    int64_t  lastPresentQpc_ = 0;

    ID3D11Device*        device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;

    // 把采到的帧拷进自己的纹理，避免帧池回收之后内容变掉。
    ID3D11Texture2D* staging_ = nullptr;
    uint32_t stagingW_ = 0;
    uint32_t stagingH_ = 0;

    std::unique_ptr<Impl> impl_;

    // 统计：WGC 是"有变化才给帧"的，靠 presentQpc 判断新旧。
};

} // namespace shadow
