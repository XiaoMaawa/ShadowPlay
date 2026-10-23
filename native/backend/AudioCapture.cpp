// AudioCapture.cpp —— WASAPI 声音采集（系统混音 / 进程环回）。
//
// 细节与取舍都写在 AudioCapture.h 的文件头，这里只记几个实现上的坑：
//
// 1) 【进程环回必须走 ActivateAudioInterfaceAsync】
//    普通设备环回可以 IMMDeviceEnumerator::Activate(IID_IAudioClient) 直接拿
//    client；但"只抓某个进程"这件事没有设备可枚举，只能用
//    ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, ...)
//    带一个 AUDIOCLIENT_ACTIVATION_PARAMS(VT_BLOB) 去异步激活。这个 API 在
//    Win10 2004 (19041) 才有，旧系统上会失败 —— 那时退回系统混音并记下原因，
//    宁可录到"多了点别的窗口的声音"，也不要整路没有声音。
//
// 2) 【共享模式只能用设备混音格式】
//    IAudioClient::Initialize 在 AUDCLNT_SHAREMODE_SHARED 下，传进去的格式必须
//    和设备混音格式一致（引擎不给重采样）。所以先 GetMixFormat，再自己把
//    它转成 48k/16bit/立体声。设备是 44.1k 或 5.1 都能吃下。
//
// 3) 【时间戳不能用"每块取一次当前时间"】
//    那样每块抖动几毫秒，导出对齐视频时会一段一段地挪，听起来像咔哒。
//    这里用"开始那一刻的墙钟锚点 + 已输出采样数"推算，只会随音频时钟缓慢
//    漂移（每小时几十毫秒量级），听不出来。
#include "AudioCapture.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <tlhelp32.h>

#include <audioclient.h>
// AUDIOCLIENT_ACTIVATION_PARAMS / PROCESS_LOOPBACK_MODE_* / VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK
// 都在这个头里（Win10 2004 SDK 起才有；老 SDK 会在这个 include 上直接报错，
// 那时也能编译——只是进程环回不可用，见下面的退回逻辑）。
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <propsys.h>
#include <propvarutil.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace shadow {
namespace {

constexpr DWORD kBufferDuration100ns = 200000; // 20ms，够小够稳
constexpr DWORD kWaitMs = 200;

int64_t NowWallMs() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    // FILETIME 是 1601 起的 100ns；转 Unix 毫秒要减掉 11644473600 秒。
    return static_cast<int64_t>(u.QuadPart / 10000ull) - 11644473600000ll;
}

/** ActivateAudioInterfaceAsync 的完成通知（一个事件 + IUnknown）。 */
class ActivateHandler : public IActivateAudioInterfaceCompletionHandler {
public:
    ActivateHandler() { ev_ = CreateEventW(nullptr, FALSE, FALSE, nullptr); }
    ~ActivateHandler() {
        if (ev_) CloseHandle(ev_);
    }

    HRESULT Wait(DWORD ms) {
        if (!ev_) return E_FAIL;
        return WaitForSingleObject(ev_, ms) == WAIT_OBJECT_0 ? S_OK : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }

    // ---- IActivateAudioInterfaceCompletionHandler ----
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override {
        if (ev_) SetEvent(ev_);
        return S_OK;
    }

    // ---- IUnknown ----
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        // 【必须自称 IAgileObject，否则进程环回根本激活不了】
        //
        // ActivateAudioInterfaceAsync 的完成回调是从 MTA 的工作线程上打的，
        // 所以它要求这个 handler 是"单元无关"（agile）的。官方文档原话：
        // "The completionHandler ... needs to implement IAgileObject ...
        //  Otherwise, an E_ILLEGAL_METHOD_CALL will occur."
        // 而这个接口是个**空标记**（没有方法），不需要真的去聚合自由线程
        // 封送器：本对象只有一个事件句柄和一个原子引用计数，天生线程安全，
        // 直接承认 QI 就是正确的做法。
        // 症状：窗口捕获那一路的声音永远退回系统混音，detail 里写着
        // "激活 hr=0x8000000E"，翻遍文档也看不出少了什么。
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler) ||
            riid == __uuidof(IAgileObject)) {
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }

private:
    std::atomic<ULONG> refs_{1};
    HANDLE ev_ = nullptr;
};

bool IsFloatFormat(const WAVEFORMATEX* wf) {
    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        // 【不用 IsEqualGUID + KSDATAFORMAT_SUBTYPE_IEEE_FLOAT】那个 GUID 在
        // ksmedia.h 里，而它和 windows.h 的某些宏会打架。子格式 GUID 的前
        // 4 字节就是 wFormatTag（WAVE_FORMAT_EXTENSIBLE 的约定），直接比它
        // 既省一个头文件，语义也一样。
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        return ext->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT;
    }
    return false;
}

int ChannelCount(const WAVEFORMATEX* wf) {
    const int ch = static_cast<int>(wf->nChannels);
    return ch > 0 ? ch : 2;
}

/**
 * 把一块设备格式的音频转成 48k / 16bit / 立体声。
 *
 * 分三步：解成 float 立体声（多于两声道只取前两路，单声道复制一份）→
 * 线性插值重采样到 48k（跨块保持相位连续）→ 量化成 S16。
 *
 * 重采样用线性插值而不是"丢/重复样本"：44.1k→48k 用丢样本会有明显失真，
 * 线性插值在语音/音乐上听感足够，且 CPU 成本可以忽略。
 */
class FormatConverter {
public:
    void Reset(uint32_t inRate, int inChannels, bool isFloat) {
        inRate_ = inRate ? inRate : 48000;
        inCh_ = inChannels > 0 ? inChannels : 2;
        floatIn_ = isFloat;
        // 重采样相位：0 表示"正停在上一块最后一帧上"。
        phase_ = 1.0;
        prev_[0] = prev_[1] = 0.0f;
        scratch_.clear();
    }

    /** 转换一块，产出 S16 交错立体声。 */
    void Convert(const BYTE* data, UINT32 frames, bool silent, std::vector<int16_t>* out) {
        out->clear();
        if (frames == 0) return;

        // ---- 1) 解码成 float 立体声 ------------------------------------
        scratch_.resize(static_cast<size_t>(frames) * 2);
        if (silent || !data) {
            std::fill(scratch_.begin(), scratch_.end(), 0.0f);
        } else if (floatIn_) {
            const float* src = reinterpret_cast<const float*>(data);
            for (UINT32 i = 0; i < frames; ++i) {
                const size_t base = static_cast<size_t>(i) * static_cast<size_t>(inCh_);
                const float l = src[base];
                const float r = inCh_ > 1 ? src[base + 1] : l;
                scratch_[static_cast<size_t>(i) * 2] = l;
                scratch_[static_cast<size_t>(i) * 2 + 1] = r;
            }
        } else {
            const int16_t* src = reinterpret_cast<const int16_t*>(data);
            for (UINT32 i = 0; i < frames; ++i) {
                const size_t base = static_cast<size_t>(i) * static_cast<size_t>(inCh_);
                const float l = static_cast<float>(src[base]) / 32768.0f;
                const float r = inCh_ > 1 ? static_cast<float>(src[base + 1]) / 32768.0f : l;
                scratch_[static_cast<size_t>(i) * 2] = l;
                scratch_[static_cast<size_t>(i) * 2 + 1] = r;
            }
        }

        // ---- 2) 重采样到 48k -------------------------------------------
        const double step = static_cast<double>(inRate_) / static_cast<double>(kAudioRate);
        // 扩展缓冲：index 0 = 上一块的最后一帧，index i>0 = 本块第 i-1 帧。
        double pos = phase_; // 在这个"扩展坐标系"里的读位置
        const double last = static_cast<double>(frames); // 本块最后一帧在扩展坐标里的下标
        while (pos <= last) {
            const size_t i0 = static_cast<size_t>(pos);
            const double frac = pos - static_cast<double>(i0);
            float l0, r0, l1, r1;
            if (i0 == 0) {
                l0 = prev_[0];
                r0 = prev_[1];
            } else {
                const size_t k = (i0 - 1) * 2;
                l0 = scratch_[k];
                r0 = scratch_[k + 1];
            }
            if (i0 + 1 > last) {
                // 越过本块末尾：用本块最后一帧外推（下一块会从 prev_ 接上）
                l1 = scratch_[(static_cast<size_t>(frames) - 1) * 2];
                r1 = scratch_[(static_cast<size_t>(frames) - 1) * 2 + 1];
            } else {
                const size_t k = i0 * 2;
                l1 = scratch_[k];
                r1 = scratch_[k + 1];
            }
            const float l = static_cast<float>(l0 + (l1 - l0) * frac);
            const float r = static_cast<float>(r0 + (r1 - r0) * frac);
            out->push_back(FloatToS16(l));
            out->push_back(FloatToS16(r));
            pos += step;
        }
        // 相位换算到下一块：本块消耗了 (frames) 个输入帧
        phase_ = pos - static_cast<double>(frames);
        if (phase_ < 0.0) phase_ = 0.0;
        prev_[0] = scratch_[(static_cast<size_t>(frames) - 1) * 2];
        prev_[1] = scratch_[(static_cast<size_t>(frames) - 1) * 2 + 1];
    }

private:
    static int16_t FloatToS16(float v) {
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        float s = v * 32767.0f;
        if (s >= 0.0f) s += 0.5f;
        else s -= 0.5f;
        if (s > 32767.0f) s = 32767.0f;
        if (s < -32768.0f) s = -32768.0f;
        return static_cast<int16_t>(s);
    }

    uint32_t inRate_ = 48000;
    int inCh_ = 2;
    bool floatIn_ = true;
    double phase_ = 1.0;
    float prev_[2] = {0.0f, 0.0f};
    std::vector<float> scratch_;
};

} // namespace

AudioCapture::AudioCapture() = default;

AudioCapture::~AudioCapture() { Stop(); }

void AudioCapture::SetSink(SinkFn fn) { sink_ = std::move(fn); }

float AudioCapture::TakePeak() {
    // 用 exchange 一步完成"取走并清零"。多个读者同时来也没关系：谁先取到谁
    // 拿到这段的峰值，另一个拿 0 —— 界面上只有一条读数路径，不会打架。
    return peakHold_.exchange(0.0f, std::memory_order_relaxed);
}

std::string AudioCapture::Detail() const {
    std::lock_guard<std::mutex> lk(detailMu_);
    return detail_;
}

int64_t AudioCapture::PacketGapMs() const {
    const int64_t last = lastPacketMs_.load(std::memory_order_relaxed);
    if (last <= 0) return -1; // 还没收到过任何包
    const int64_t gap = NowWallMs() - last;
    return gap > 0 ? gap : 0;
}

namespace {
/**
 * root 进程 + 它的所有后代（广度收敛，进程树深度一般不超过十几层）。
 *
 * 进程环回的 INCLUDE_TARGET_PROCESS_TREE 是**创建时**解析一次这张表，
 * 之后不再更新 —— 所以这张表既能当"当时抓到了谁"的记录，也能拿来和
 * 现在的树比，看有没有新进程跑到了环回之外。
 */
std::set<uint32_t> CollectProcessTree(uint32_t root) {
    std::set<uint32_t> out;
    if (root == 0) return out;
    out.insert(root);

    std::vector<std::pair<uint32_t, uint32_t>> rel; // (pid, ppid)
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                rel.emplace_back(static_cast<uint32_t>(pe.th32ProcessID),
                                 static_cast<uint32_t>(pe.th32ParentProcessID));
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }

    // 反复扫到不再增长为止（子进程可能比父进程先出现在快照里）。
    bool grew = true;
    while (grew) {
        grew = false;
        for (const auto& r : rel) {
            if (out.count(r.first)) continue;
            if (out.count(r.second)) {
                out.insert(r.first);
                grew = true;
            }
        }
    }
    return out;
}
} // namespace

int AudioCapture::NewChildCount() const {
    std::set<uint32_t> base;
    bool processLoopback = false;
    uint32_t root = 0;
    {
        std::lock_guard<std::mutex> lk(treeMu_);
        base = treeIds_;
        processLoopback = useProcessLoopback_;
        root = targetPid_;
    }
    if (!processLoopback || root == 0 || base.empty()) return 0;
    const std::set<uint32_t> now = CollectProcessTree(root);
    int n = 0;
    for (uint32_t pid : now) {
        if (!base.count(pid)) ++n;
    }
    return n;
}

bool AudioCapture::Start(bool processLoopback, uint32_t targetPid, std::string* outError,
                         std::string* outDetail) {
    Stop();

    if (processLoopback && targetPid == 0) {
        // 没有进程 id 就退系统混音（窗口还没匹到时会出现）。
        processLoopback = false;
    }
    useProcessLoopback_ = processLoopback;
    targetPid_ = targetPid;
    // 记下创建这一刻的进程树（NewChildCount 拿它做基准）。
    {
        const std::set<uint32_t> tree =
            processLoopback ? CollectProcessTree(targetPid) : std::set<uint32_t>{};
        std::lock_guard<std::mutex> lk(treeMu_);
        treeIds_ = tree;
    }

    {
        std::lock_guard<std::mutex> lk(detailMu_);
        detail_.clear();
    }
    if (eventHandle_) {
        CloseHandle(static_cast<HANDLE>(eventHandle_));
        eventHandle_ = nullptr;
    }
    eventHandle_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!eventHandle_) {
        if (outError) *outError = "建不了采集事件对象";
        return false;
    }

    stopping_.store(false, std::memory_order_relaxed);
    running_.store(true, std::memory_order_relaxed);
    peak_.store(0.0f, std::memory_order_relaxed);
    rms_.store(0.0f, std::memory_order_relaxed);
    framesOut_.store(0, std::memory_order_relaxed);
    lastPacketMs_.store(0, std::memory_order_relaxed);
    thread_ = std::thread(&AudioCapture::ThreadProc, this);

    // 等线程把通道建好（或者失败），最多 1.5 秒 —— 界面第一次读状态时
    // detail 就已经是准的，不用等下一轮。
    for (int i = 0; i < 150; ++i) {
        if (!running_.load(std::memory_order_relaxed)) break;
        if (!Detail().empty()) break;
        Sleep(10);
    }
    const std::string d = Detail();
    if (d.rfind("ERR:", 0) == 0) {
        if (outError) *outError = d.substr(4);
        Stop();
        return false;
    }
    if (outDetail) *outDetail = d;
    return true;
}

void AudioCapture::Stop() {
    stopping_.store(true, std::memory_order_relaxed);
    if (thread_.joinable()) thread_.join();
    running_.store(false, std::memory_order_relaxed);
    peak_.store(0.0f, std::memory_order_relaxed);
    rms_.store(0.0f, std::memory_order_relaxed);
    if (eventHandle_) {
        CloseHandle(static_cast<HANDLE>(eventHandle_));
        eventHandle_ = nullptr;
    }
}

void AudioCapture::ThreadProc() {
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool needCoUninit = SUCCEEDED(coHr);

    auto setDetail = [&](const std::string& s) {
        std::lock_guard<std::mutex> lk(detailMu_);
        detail_ = s;
    };
    auto fail = [&](const std::string& why) {
        setDetail("ERR:" + why);
        running_.store(false, std::memory_order_relaxed);
    };

    IAudioClient* client = nullptr;
    IAudioCaptureClient* capture = nullptr;
    WAVEFORMATEX* mixFmt = nullptr;
    // 进程环回用的格式是**我们自己给**的，不能 CoTaskMemFree（见下面）。
    WAVEFORMATEX ownFmt{};
    bool ownMixFmt = false;
    std::string channelName;

    // ---- 拿 IAudioClient -------------------------------------------------
    if (useProcessLoopback_) {
        AUDIOCLIENT_ACTIVATION_PARAMS params{};
        params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        params.ProcessLoopbackParams.TargetProcessId = targetPid_;
        params.ProcessLoopbackParams.ProcessLoopbackMode =
            PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

        PROPVARIANT pv;
        PropVariantInit(&pv);
        pv.vt = VT_BLOB;
        pv.blob.cbSize = sizeof(params);
        pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

        ActivateHandler* handler = new ActivateHandler();
        IActivateAudioInterfaceAsyncOperation* op = nullptr;
        HRESULT ahr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
                                                  __uuidof(IAudioClient), &pv, handler, &op);
        // 【这里绝对不能 PropVariantClear(&pv)】
        //
        // pv 是 VT_BLOB、pBlobData 指向**栈上**的 params，而 PropVariantClear
        // 对 VT_BLOB 会拿 pBlobData 去 CoTaskMemFree —— 对栈地址释放堆块，
        // 当场把堆的簿记打坏。之后随便一次分配就会以
        //   0xC0000374（STATUS_HEAP_CORRUPTION）
        // 崩在 ntdll 的堆检查里，崩点和这里毫无关系，极难查。
        // 现象：窗口捕获一开，服务几秒后静默退出，守护把它拉起来，界面就永远
        // 停在"未录制 / NO SIGNAL"。
        // （官方 ProcessLoopback 示例同样从不清这个 PROPVARIANT。）
        // 只要不动它就行：pv 本身没有所有权，params 是栈对象，作用域一到就没了。
        HRESULT actHr = E_FAIL;
        HRESULT waitHr = E_FAIL;
        if (SUCCEEDED(ahr) && op) {
            waitHr = handler->Wait(4000);
            IUnknown* unk = nullptr;
            op->GetActivateResult(&actHr, &unk);
            if (SUCCEEDED(actHr) && unk) {
                unk->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void**>(&client));
                unk->Release();
            }
            op->Release();
        }
        handler->Release();

        if (!client) {
            // 【窗口源失败就失败，不退回系统混音】
            //
            // 退回系统混音录到的是"所有窗口混在一起的声音"，和用户要的
            // "只录这个窗口"完全相反 —— 悄悄退回去，用户根本不知道成片里
            // 混进了别的程序的声音。所以这里直接报错，界面会在预览画面上
            // 盖一条红色提示说明原因（见 frontend 的 audioError）。
            // Session 会每 5 秒重试，问题消失（窗口开始出声/系统支持了）
            // 就自己恢复。
            char why[256];
            std::snprintf(why, sizeof(why),
                          "按窗口抓声音失败（激活 hr=0x%08lX / 等待 hr=0x%08lX / 取客户端 "
                          "hr=0x%08lX）",
                          static_cast<unsigned long>(ahr), static_cast<unsigned long>(waitHr),
                          static_cast<unsigned long>(actHr));
            fail(why);
            if (needCoUninit) CoUninitialize();
            return;
        }
        channelName = "窗口进程环回";
    }

    if (!client) {
        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDevice* device = nullptr;
        const HRESULT chr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                             __uuidof(IMMDeviceEnumerator),
                                             reinterpret_cast<void**>(&enumerator));
        if (SUCCEEDED(chr) && enumerator) {
            if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) &&
                device) {
                device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                 reinterpret_cast<void**>(&client));
                device->Release();
            }
            enumerator->Release();
        }
        if (!client) {
            fail("拿不到默认播放设备（没有播放设备，或者被占用了）");
            if (needCoUninit) CoUninitialize();
            return;
        }
        channelName = "系统混音";
    }

    // ---- 格式与初始化 ----------------------------------------------------
    //
    // 【两条路拿格式的方式不一样】
    //   系统混音：设备有自己的混音格式，必须用 GetMixFormat（共享模式不收
    //             别的格式）。
    //   进程环回：VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK 这个虚拟设备**没有**
    //             混音格式，GetMixFormat 直接回 E_NOTIMPL(0x80004001)。
    //             它的规矩反过来：调用方给什么格式，它就按什么格式吐
    //             （引擎内部转换）。所以这里直接要 48k/16bit/立体声 ——
    //             正好等于我们的统一格式，后面的重采样器变成直通。
    //             万一引擎只收 float（少数驱动），再试一次 float32 立体声。
    HRESULT hr = S_OK;
    DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;

    auto buildOwnFmt = [&](bool asFloat) {
        ownFmt = WAVEFORMATEX{};
        ownFmt.wFormatTag = asFloat ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
        ownFmt.nChannels = 2;
        ownFmt.nSamplesPerSec = kAudioRate;
        ownFmt.wBitsPerSample = static_cast<WORD>(asFloat ? 32 : 16);
        ownFmt.nBlockAlign = static_cast<WORD>(ownFmt.nChannels * ownFmt.wBitsPerSample / 8);
        ownFmt.nAvgBytesPerSec = ownFmt.nSamplesPerSec * ownFmt.nBlockAlign;
        ownFmt.cbSize = 0;
        ownMixFmt = true;
        mixFmt = &ownFmt;
    };

    if (useProcessLoopback_) {
        buildOwnFmt(false);
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kBufferDuration100ns, 0, mixFmt,
                                nullptr);
        if (FAILED(hr)) {
            buildOwnFmt(true);
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kBufferDuration100ns, 0,
                                    mixFmt, nullptr);
        }
    } else {
        hr = client->GetMixFormat(&mixFmt);
        if (FAILED(hr) || !mixFmt) {
            fail("读不出设备混音格式（hr=" + std::to_string(static_cast<long>(hr)) + "）");
            client->Release();
            if (needCoUninit) CoUninitialize();
            return;
        }
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kBufferDuration100ns, 0, mixFmt,
                                nullptr);
    }
    if (FAILED(hr)) {
        fail("音频引擎初始化失败（hr=" + std::to_string(static_cast<long>(hr)) +
             "，可能没有播放设备）");
        if (ownMixFmt) { /* 自己给的格式，不释放 */ } else CoTaskMemFree(mixFmt);
        client->Release();
        if (needCoUninit) CoUninitialize();
        return;
    }

    hr = client->SetEventHandle(static_cast<HANDLE>(eventHandle_));
    if (SUCCEEDED(hr)) {
        hr = client->GetService(__uuidof(IAudioCaptureClient),
                                reinterpret_cast<void**>(&capture));
    }
    if (FAILED(hr) || !capture) {
        fail("取不到采集客户端（hr=" + std::to_string(static_cast<long>(hr)) + "）");
        if (!ownMixFmt) CoTaskMemFree(mixFmt);
        client->Release();
        if (needCoUninit) CoUninitialize();
        return;
    }

    hr = client->Start();
    if (FAILED(hr)) {
        fail("启动音频采集失败（hr=" + std::to_string(static_cast<long>(hr)) + "）");
        capture->Release();
        if (!ownMixFmt) CoTaskMemFree(mixFmt);
        client->Release();
        if (needCoUninit) CoUninitialize();
        return;
    }

    FormatConverter conv;
    conv.Reset(mixFmt->nSamplesPerSec, ChannelCount(mixFmt), IsFloatFormat(mixFmt));

    // 通道说明：让界面/日志一眼看出"这一路到底在抓什么"。
    {
        std::string d = channelName;
        d += " · " + std::to_string(mixFmt->nSamplesPerSec) + "Hz/" +
             std::to_string(mixFmt->nChannels) + "ch";
        setDetail(d);
    }

    // ---- 时间轴锚点 -------------------------------------------------------
    //
    // 时间戳**不能**按"每块截断到毫秒再累加"算：blockMs = 帧数*1000/48000
    // 是向下取整，块大小只要不是 48 的整数倍（44.1k 设备转成 48k 就是 480/481
    // 交替，还有些设备给 512 帧的包），每块就少算零点几毫秒。帧数越攒越多，
    // 欠下的时间越来越多 —— 攒到 100ms 就会触发一次"补静音"，听感就是规律性
    // 地顿一下（一秒钟一次，很像卡顿）。
    //
    // 现在换成：anchorWallMs = "已交付帧数 == 0" 那一刻的墙钟，每块起点一律
    // 由 anchorWallMs + 已交付帧数 反算。误差永远不超过 1ms，不会累积。
    // 墙钟只用来发现"真的断流了"（设备停发、被独占、切设备），那种情况才补
    // 静音或重新锚定。
    int64_t anchorWallMs = 0;
    uint64_t framesOut = 0;        // 累计交付帧数（含补的静音），用于统计
    int64_t deliveredFrames = 0;   // 同上，时间轴用
    constexpr int64_t kGapMs = 300;      // 超过它才算"真的断了"
    constexpr int64_t kBigGapMs = 1000;  // 超过它就不补静音，直接重新锚定

    std::vector<int16_t> pcm;
    pcm.reserve(8192);

    while (!stopping_.load(std::memory_order_relaxed)) {
        const DWORD w = WaitForSingleObject(static_cast<HANDLE>(eventHandle_), kWaitMs);
        // 【超时也要读】事件驱动在环回上不是绝对可靠的：引擎偶尔不再 SetEvent
        // （系统里多路音频会话来回切换时更容易遇到），而缓冲区其实一直在填。
        // 以前 `if (w != WAIT_OBJECT_0) continue;` 会就此"等下去"，会话看着
        // 一切正常、detail 也对，但一个字节都收不到 —— 用户看到的就是
        // 「捕获的窗口没有声音」。GetBuffer 拿不到数据只会返回 EMPTY，
        // 每 200ms 白问一次，代价可以忽略，换来的是不会永久哑掉。
        (void)w;

        for (;;) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD bufFlags = 0;
            HRESULT ghr = capture->GetBuffer(&data, &frames, &bufFlags, nullptr, nullptr);
            if (ghr == AUDCLNT_S_BUFFER_EMPTY) break; // 暂时没数据：等下一个事件
            if (ghr == AUDCLNT_E_DEVICE_INVALIDATED) {
                // 默认设备换了/被拔了：结束这一轮，Session 会在下一秒重开。
                setDetail("ERR:音频设备被切换（采集已停止，等待重开）");
                running_.store(false, std::memory_order_relaxed);
                goto done;
            }
            if (FAILED(ghr)) break;
            if (frames == 0) {
                capture->ReleaseBuffer(0);
                break;
            }

            conv.Convert(data, frames, (bufFlags & AUDCLNT_BUFFERFLAGS_SILENT) != 0, &pcm);
            // ReleaseBuffer 的参数是**帧数**，不是字节数（MSDN: NumFramesRead）。
            // 以前这里传的是 frames * nBlockAlign，等于告诉引擎"我读走了 4 倍的
            // 数据"，内部的读指针被推过头，后面拿到的包会越来越少直到出错。
            capture->ReleaseBuffer(frames);
            // 看门狗用的时间戳：只要引擎真的给了包（哪怕内容是静音）就记一笔，
            // 长时间没有 = 这条环回已经看不到目标了，Session 会重建它。
            lastPacketMs_.store(NowWallMs(), std::memory_order_relaxed);

            const size_t outFrames = pcm.size() / 2;
            if (outFrames > 0) {
                // ---- 时间戳 -------------------------------------------------
                //
                // 起点一律由 "锚点 + 已交付帧数" 反算（见上面的说明），墙钟只
                // 用来判断"是不是真的断流了"：
                //
                //   * |偏差| <= 300ms：正常（设备时钟和系统时钟本来就有一点点
                //     差，回放窗口只有几分钟，这点偏差根本不用管）→ 直接续上；
                //   * 迟到 300ms~1s：真的断过一小段（被独占、被暂停）→ 补等长
                //     静音，文件内部保持连续；
                //   * 迟到 >1s：补静音会是一大坨空白 → 重新锚定，让这块结束在
                //     "现在"。AudioRing 看到起点跳变会换新文件。
                const int64_t outFramesI = static_cast<int64_t>(outFrames);
                const int64_t nowMs = NowWallMs();
                if (anchorWallMs == 0) {
                    // 第一块：让它正好结束在"现在"
                    anchorWallMs = nowMs - outFramesI * 1000 / kAudioRate;
                } else {
                    const int64_t expectedEnd = anchorWallMs + deliveredFrames * 1000 / kAudioRate;
                    const int64_t lag = nowMs - expectedEnd; // >0 = 我们落后于墙钟
                    if (lag > kGapMs) {
                        if (lag > kBigGapMs) {
                            // 重新锚定：等价于把这 1 秒以上的空白"跳过"。
                            anchorWallMs +=
                                (nowMs - outFramesI * 1000 / kAudioRate) - expectedEnd;
                        } else {
                            const int64_t pad = lag * static_cast<int64_t>(kAudioRate) / 1000;
                            if (pad > 0) {
                                const int64_t padStart =
                                    anchorWallMs + deliveredFrames * 1000 / kAudioRate;
                                std::vector<int16_t> silence(static_cast<size_t>(pad) * 2, 0);
                                if (sink_) {
                                    sink_(silence.data(), static_cast<size_t>(pad), padStart);
                                }
                                framesOut += static_cast<uint64_t>(pad);
                                deliveredFrames += pad;
                            }
                        }
                    } else if (lag < -kGapMs) {
                        // 音频跑到墙钟前面了（系统时间被校准过 / 设备时钟快）。
                        // 把锚点往后挪，让这一块接在"现在"，已经交付的不动。
                        anchorWallMs += -lag;
                    }
                }
                const int64_t startWall = anchorWallMs + deliveredFrames * 1000 / kAudioRate;

                // ---- 电平计量 ---------------------------------------------
                float peak = 0.0f;
                double sumsq = 0.0;
                const size_t n = pcm.size();
                for (size_t i = 0; i < n; ++i) {
                    const float v = static_cast<float>(pcm[i]) / 32768.0f;
                    const float a = v < 0 ? -v : v;
                    if (a > peak) peak = a;
                    sumsq += static_cast<double>(v) * v;
                }
                const float blockRms = n > 0 ? static_cast<float>(std::sqrt(sumsq / n)) : 0.0f;
                // 峰值：快速起、缓慢落（0.8 每块），界面上的条看起来才不抽。
                float prevPeak = peak_.load(std::memory_order_relaxed) * 0.8f;
                if (peak < prevPeak) peak = prevPeak;
                if (peak < 0.0008f) peak = 0.0f;
                peak_.store(peak, std::memory_order_relaxed);
                // 峰值保持：界面每秒取一次（见 TakePeak）。
                float hold = peakHold_.load(std::memory_order_relaxed);
                while (peak > hold && !peakHold_.compare_exchange_weak(hold, peak)) {
                    /* 重试 */
                }
                const float prevRms = rms_.load(std::memory_order_relaxed);
                rms_.store(prevRms * 0.85f + blockRms * 0.15f, std::memory_order_relaxed);

                // ---- 交给上层落盘 ---------------------------------------
                if (sink_) sink_(pcm.data(), outFrames, startWall);
                framesOut += outFrames;
                framesOut_.store(framesOut, std::memory_order_relaxed);
                deliveredFrames += outFramesI;
            }
        }
    }

done:
    client->Stop();
    capture->Release();
    // 自己给的格式（进程环回）是栈对象，只能释放 GetMixFormat 那份。
    if (!ownMixFmt) CoTaskMemFree(mixFmt);
    client->Release();
    running_.store(false, std::memory_order_relaxed);
    peak_.store(0.0f, std::memory_order_relaxed);
    rms_.store(0.0f, std::memory_order_relaxed);
    if (needCoUninit) CoUninitialize();
}

} // namespace shadow
