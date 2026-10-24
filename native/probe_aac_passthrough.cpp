// probe_aac_passthrough.cpp —— 验证需求 1 的核心可行性：
//
//   1. 常驻 AAC 编码器 MFT（CLSID_AACMFTEncoder）能否把 PCM 连续编码成 AAC 样本；
//   2. 分片 SinkWriter 能否用「AAC 输出类型 + AAC 输入类型」做 passthrough，
//      把已编码的 AAC 样本直拷进 fMP4（不重压）；
//   3. 关闭一个分片 SinkWriter、再开一个新的，AAC 样本能否跨片无缝续上。
//
// 这是「音频编码进切片」的根基：如果 AAC passthrough 走不通（SinkWriter 拒绝
// AAC 输入类型），整个方案要重新设计。
//
// 编译（在仓库根目录）：
//   cl /nologo /utf-8 /std:c++17 /EHsc /O2 /MD native/probe_aac_passthrough.cpp \
//      /link mfplat.lib mfreadwrite.lib mfuuid.lib mf.lib wmcodecdspuuid.lib ole32.lib
//
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <codecapi.h>
#include <mferror.h>

#include <cstdio>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "wmcodecdspuuid.lib")
#pragma comment(lib, "ole32.lib")

// CLSID_AACMFTEncoder 在 wmcodecdsp.h 里定义，但那个头会拖进一堆东西。
// 直接写死这个 GUID（等价于 CLSID_AACMFTEncoder）。
// {93AF0C51-2275-45D2-A35B-F2BA21CAED00}
static const GUID CLSID_AACMFTEncoderLocal = { 0x93AF0C51, 0x2275, 0x45D2,
    { 0xA3, 0x5B, 0xF2, 0xBA, 0x21, 0xCA, 0xED, 0x00 } };

static const uint32_t kRate = 48000;
static const uint32_t kChannels = 2;

static void hrText(HRESULT hr, const char* what) {
    printf("    %s -> 0x%08lX", what, (unsigned long)hr);
    if (FAILED(hr)) printf("  <== FAIL");
    printf("\n");
}

// 生成一段 440Hz 正弦波 PCM，填满 frames 帧（每帧 2 个 int16）。
static void makeTone(std::vector<int16_t>& out, size_t frames, double freq) {
    out.resize(frames * kChannels);
    for (size_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / kRate;
        const int16_t v = static_cast<int16_t>(std::sin(2.0 * 3.141592653589793 * freq * t) * 12000.0);
        out[i * 2] = v;
        out[i * 2 + 1] = v;
    }
}

int main() {
    printf("== 探针启动 ==\n");
    fflush(stdout);
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    printf("CoInitializeEx -> 0x%08lX\n", (unsigned long)hr);
    fflush(stdout);
    hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    printf("MFStartup -> 0x%08lX\n", (unsigned long)hr);
    fflush(stdout);
    if (FAILED(hr)) return 1;

    // ---- 1. 建常驻 AAC 编码器 MFT ----------------------------------------
    fprintf(stderr, "[probe] 准备 CoCreateInstance AAC encoder\n");
    IMFTransform* enc = nullptr;
    hr = CoCreateInstance(CLSID_AACMFTEncoderLocal, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&enc));
    fprintf(stderr, "[probe] CoCreateInstance 返回 0x%08lX, enc=%p\n", (unsigned long)hr, (void*)enc);
    printf("\n[1] 创建 AAC 编码器 MFT\n");
    hrText(hr, "CoCreateInstance(AAC Encoder)");
    if (FAILED(hr) || !enc) {
        printf("  !! AAC 编码器建不起来，方案需调整（可能缺 Media Feature Pack）\n");
        return 1;
    }

    // 输入类型：PCM 16bit/48k/立体声
    IMFMediaType* inT = nullptr;
    MFCreateMediaType(&inT);
    inT->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    inT->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    inT->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    inT->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
    inT->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
    inT->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, kChannels * 2);
    inT->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kRate * kChannels * 2);
    inT->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, 1);
    fprintf(stderr, "[probe] 准备 SetInputType(PCM)\n");
    hr = enc->SetInputType(0, inT, 0);
    fprintf(stderr, "[probe] SetInputType(PCM) -> 0x%08lX\n", (unsigned long)hr);
    hrText(hr, "SetInputType(PCM)");
    inT->Release();
    if (FAILED(hr)) return 1;

    // 输出类型：AAC-LC raw，192kbps
    IMFMediaType* outT = nullptr;
    MFCreateMediaType(&outT);
    outT->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    outT->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    outT->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    outT->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
    outT->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
    outT->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);
    outT->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
    outT->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
    fprintf(stderr, "[probe] 准备 SetOutputType(AAC)\n");
    hr = enc->SetOutputType(0, outT, 0);
    fprintf(stderr, "[probe] SetOutputType(AAC) -> 0x%08lX\n", (unsigned long)hr);
    hrText(hr, "SetOutputType(AAC)");
    if (FAILED(hr)) return 1;

    // 读回输出类型：拿到 MF_MT_USER_DATA（AudioSpecificConfig），这是 passthrough
    // 时必须交给 SinkWriter 的。
    fprintf(stderr, "[probe] 准备 GetOutputCurrentType\n");
    IMFMediaType* gotOut = nullptr;
    hr = enc->GetOutputCurrentType(0, &gotOut);
    fprintf(stderr, "[probe] GetOutputCurrentType -> 0x%08lX, gotOut=%p\n", (unsigned long)hr, (void*)gotOut);
    UINT32 userDataSize = 0;
    uint8_t* userData = nullptr;
    if (gotOut) {
        hr = gotOut->GetBlobSize(MF_MT_USER_DATA, &userDataSize);
        fprintf(stderr, "[probe] GetBlobSize(MF_MT_USER_DATA) = 0x%08lX, %u\n", (unsigned long)hr, userDataSize);
        if (userDataSize > 0) {
            userData = new uint8_t[userDataSize];
            gotOut->GetBlob(MF_MT_USER_DATA, userData, userDataSize, nullptr);
        }
        printf("    输出类型 MF_MT_USER_DATA 大小 = %u 字节\n", userDataSize);
        gotOut->Release();
    }
    fprintf(stderr, "[probe] 读回输出类型完成，准备编码循环\n");
    fflush(stderr);

    // ---- 2. 连续编码 PCM -> AAC（模拟常驻编码器） ------------------------
    // AAC 每帧 = 1024 样本。喂 10 帧（约 213ms）的 PCM。
    printf("\n[2] 连续编码 PCM -> AAC\n");
    const uint32_t kFrame = 1024;
    const uint32_t kNumInputs = 10;
    std::vector<int16_t> pcm;
    fprintf(stderr, "[probe] 准备 makeTone\n");
    makeTone(pcm, kFrame * kNumInputs, 440.0);
    fprintf(stderr, "[probe] makeTone 完成, pcm=%zu 样本\n", pcm.size());

    // 收集编码好的 AAC 样本（每份是一个独立的 IMFSample，含时间戳）。
    struct EncSample {
        std::vector<uint8_t> data;
        LONGLONG ts = 0;       // 100ns，从 0 开始
        LONGLONG dur = 0;
    };
    std::vector<EncSample> encoded;

    // 【关键】AAC MFT 需要标准的消息序列，否则内部状态未初始化，ProcessOutput 会崩。
    enc->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    enc->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    // 先问输出流的缓冲区大小（OBS / MSDN 官方示例都是自己分配输出样本，
    // 而不是传 pSample=nullptr 让 MFT 自己分配 —— 后者是崩溃的嫌疑）。
    MFT_OUTPUT_STREAM_INFO outInfo = {};
    hr = enc->GetOutputStreamInfo(0, &outInfo);
    fprintf(stderr, "[probe] GetOutputStreamInfo -> 0x%08lX, cbSize=%u, flags=0x%X\n",
            (unsigned long)hr, outInfo.cbSize, outInfo.dwFlags);

    // 喂输入 + 排空输出交替进行（AAC 编码器输入缓冲有限，必须边喂边取）
    for (uint32_t i = 0; i < kNumInputs; ++i) {
        IMFMediaBuffer* buf = nullptr;
        MFCreateMemoryBuffer(kFrame * kChannels * 2, &buf);
        BYTE* dst = nullptr;
        buf->Lock(&dst, nullptr, nullptr);
        memcpy(dst, pcm.data() + i * kFrame * kChannels, kFrame * kChannels * 2);
        buf->Unlock();
        buf->SetCurrentLength(kFrame * kChannels * 2);

        IMFSample* s = nullptr;
        MFCreateSample(&s);
        s->AddBuffer(buf);
        buf->Release();
        s->SetSampleTime(static_cast<LONGLONG>(i) * kFrame * 10000000ll / kRate);
        s->SetSampleDuration(static_cast<LONGLONG>(kFrame) * 10000000ll / kRate);
        hr = enc->ProcessInput(0, s, 0);
        s->Release();
        fprintf(stderr, "[probe] ProcessInput[%u] -> 0x%08lX\n", i, (unsigned long)hr);
        if (hr == MF_E_NOTACCEPTING) {
            fprintf(stderr, "[probe] ProcessInput[%u] NOTACCEPTING，先排空输出\n", i);
        } else if (FAILED(hr)) {
            return 1;
        }

        // 排空输出
        for (;;) {
            DWORD outFlags = 0;
            fprintf(stderr, "[probe]   准备 GetOutputStatus\n");
            hr = enc->GetOutputStatus(&outFlags);
            fprintf(stderr, "[probe]   GetOutputStatus -> 0x%08lX flags=0x%X\n", (unsigned long)hr, outFlags);
            if (FAILED(hr) || !(outFlags & MFT_OUTPUT_STATUS_SAMPLE_READY)) break;

            // 自己分配输出样本（MSDN 官方示例 + OBS 的做法）
            IMFMediaBuffer* obuf = nullptr;
            DWORD obSize = outInfo.cbSize ? outInfo.cbSize : 4096;
            hr = MFCreateMemoryBuffer(obSize, &obuf);
            if (FAILED(hr)) { fprintf(stderr, "[probe] 分配输出缓冲失败\n"); return 1; }
            IMFSample* os = nullptr;
            MFCreateSample(&os);
            os->AddBuffer(obuf);
            obuf->Release();

            MFT_OUTPUT_DATA_BUFFER outBuf = {};
            outBuf.dwStreamID = 0;
            outBuf.pSample = os;
            DWORD status = 0;
            hr = enc->ProcessOutput(0, 1, &outBuf, &status);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) { os->Release(); break; }
            if (FAILED(hr)) { os->Release(); fprintf(stderr, "[probe] ProcessOutput -> 0x%08lX\n", (unsigned long)hr); break; }

            if (outBuf.pSample) {
                IMFMediaBuffer* ob = nullptr;
                outBuf.pSample->GetBufferByIndex(0, &ob);
                BYTE* od = nullptr;
                DWORD len = 0;
                ob->Lock(&od, nullptr, &len);
                EncSample es;
                es.data.assign(od, od + len);
                ob->Unlock();
                ob->Release();
                outBuf.pSample->GetSampleTime(&es.ts);
                outBuf.pSample->GetSampleDuration(&es.dur);
                encoded.push_back(std::move(es));
            }
            if (outBuf.pEvents) outBuf.pEvents->Release();
            os->Release();
        }
    }

    // 最后排空：把尾部样本逼出来（AAC 有 priming 延迟，尾部还有余留样本）。
    // 【必须和主循环一样自己分配输出样本，pSample=nullptr 会让 AAC MFT 崩溃】
    enc->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    for (int drain = 0; drain < 16; ++drain) {
        DWORD outFlags = 0;
        hr = enc->GetOutputStatus(&outFlags);
        if (FAILED(hr) || !(outFlags & MFT_OUTPUT_STATUS_SAMPLE_READY)) break;

        IMFMediaBuffer* obuf = nullptr;
        DWORD obSize = outInfo.cbSize ? outInfo.cbSize : 4096;
        MFCreateMemoryBuffer(obSize, &obuf);
        IMFSample* os = nullptr;
        MFCreateSample(&os);
        os->AddBuffer(obuf);
        obuf->Release();

        MFT_OUTPUT_DATA_BUFFER outBuf = {};
        outBuf.dwStreamID = 0;
        outBuf.pSample = os;
        DWORD status = 0;
        hr = enc->ProcessOutput(0, 1, &outBuf, &status);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) { os->Release(); break; }
        if (FAILED(hr)) { os->Release(); break; }
        if (outBuf.pSample) {
            IMFMediaBuffer* ob = nullptr;
            outBuf.pSample->GetBufferByIndex(0, &ob);
            BYTE* od = nullptr;
            DWORD len = 0;
            ob->Lock(&od, nullptr, &len);
            EncSample es;
            es.data.assign(od, od + len);
            ob->Unlock();
            ob->Release();
            outBuf.pSample->GetSampleTime(&es.ts);
            outBuf.pSample->GetSampleDuration(&es.dur);
            encoded.push_back(std::move(es));
        }
        if (outBuf.pEvents) outBuf.pEvents->Release();
        os->Release();
    }
    fprintf(stderr, "[probe] 编码完成，得到 %zu 个样本\n", encoded.size());

    printf("    编码出 %zu 个 AAC 样本\n", encoded.size());
    if (encoded.empty()) {
        printf("  !! 一个 AAC 样本都没出，ProcessOutput 可能返回 MF_E_TRANSFORM_NEED_MORE_INPUT\n");
        printf("  （这是正常的：编码器有 priming 延迟，前几帧输入不产生输出）\n");
    }
    for (size_t i = 0; i < encoded.size() && i < 4; ++i) {
        printf("    样本[%zu]: %zu 字节, ts=%lld, dur=%lld\n",
               i, encoded[i].data.size(), encoded[i].ts, encoded[i].dur);
    }

    // ---- 3. 验证 AAC passthrough 写进 fMP4 分片 --------------------------
    printf("\n[3] AAC passthrough 写进 fMP4 分片\n");

    // 输出类型（给 SinkWriter AddStream）：AAC
    IMFMediaType* swOut = nullptr;
    MFCreateMediaType(&swOut);
    swOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    swOut->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    swOut->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    swOut->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
    swOut->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
    swOut->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);
    swOut->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
    swOut->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
    if (userData) {
        swOut->SetBlob(MF_MT_USER_DATA, userData, userDataSize);
    }

    // 输入类型（给 SinkWriter SetInputMediaType）：也是 AAC —— 这就是 passthrough。
    // 如果 SinkWriter 拒绝 AAC 输入（返回 MF_E_INVALIDMEDIATYPE），说明不能直拷。
    IMFMediaType* swIn = nullptr;
    MFCreateMediaType(&swIn);
    swIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    swIn->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    swIn->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    swIn->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
    swIn->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
    swIn->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);
    swIn->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
    swIn->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
    if (userData) {
        swIn->SetBlob(MF_MT_USER_DATA, userData, userDataSize);
    }

    IMFAttributes* attrs = nullptr;
    MFCreateAttributes(&attrs, 2);
    attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_FMPEG4);
    attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);

    IMFSinkWriter* writer = nullptr;
    hr = MFCreateSinkWriterFromURL(L"probe_aac_passthrough_out.mp4", nullptr, attrs, &writer);
    attrs->Release();
    hrText(hr, "MFCreateSinkWriterFromURL");
    if (FAILED(hr) || !writer) return 1;

    DWORD stream = 0;
    hr = writer->AddStream(swOut, &stream);
    hrText(hr, "AddStream(AAC)");
    if (FAILED(hr)) return 1;

    hr = writer->SetInputMediaType(stream, swIn, nullptr);
    printf("    stream=%u\n", stream);
    hrText(hr, "SetInputMediaType(AAC passthrough)");
    swOut->Release();
    swIn->Release();

    if (FAILED(hr)) {
        printf("\n  !! AAC passthrough 被拒 —— SinkWriter 不接受 AAC 输入类型\n");
        printf("     这意味着「分片直拷已编码 AAC」走不通，需要换方案：\n");
        printf("     方案 B：分片 SinkWriter 用 PCM 输入自己编码（但会每片 priming）\n");
        printf("     方案 C：不用 SinkWriter 管音频，导出端统一 AAC 编码（现状）\n");
        return 2; // 退出码 2 = passthrough 不通
    }

    printf("    ✓ AAC passthrough 被接受！可以直拷已编码样本\n");

    hr = writer->BeginWriting();
    hrText(hr, "BeginWriting");
    if (FAILED(hr)) return 1;

    // 把编码好的 AAC 样本直拷进去（时间戳从 0 重新排）
    for (size_t i = 0; i < encoded.size(); ++i) {
        IMFMediaBuffer* buf = nullptr;
        MFCreateMemoryBuffer(static_cast<DWORD>(encoded[i].data.size()), &buf);
        BYTE* dst = nullptr;
        buf->Lock(&dst, nullptr, nullptr);
        memcpy(dst, encoded[i].data.data(), encoded[i].data.size());
        buf->Unlock();
        buf->SetCurrentLength(static_cast<DWORD>(encoded[i].data.size()));

        IMFSample* s = nullptr;
        MFCreateSample(&s);
        s->AddBuffer(buf);
        buf->Release();
        s->SetSampleTime(encoded[i].ts);
        s->SetSampleDuration(encoded[i].dur);
        hr = writer->WriteSample(stream, s);
        s->Release();
        if (FAILED(hr)) {
            hrText(hr, "WriteSample(AAC)");
            return 1;
        }
    }
    printf("    直拷了 %zu 个 AAC 样本\n", encoded.size());

    writer->Finalize();
    writer->Release();
    printf("    Finalize 完成\n");

    // ---- 4. 用 SourceReader 读回验证（fMP4 里的音频能读出来） -----------
    printf("\n[4] 读回验证\n");
    IMFSourceReader* reader = nullptr;
    hr = MFCreateSourceReaderFromURL(L"probe_aac_passthrough_out.mp4", nullptr, &reader);
    hrText(hr, "MFCreateSourceReaderFromURL");
    if (SUCCEEDED(hr) && reader) {
        // 有没有音频流？
        IMFMediaType* t = nullptr;
        hr = reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &t);
        if (SUCCEEDED(hr) && t) {
            GUID sub = {};
            t->GetGUID(MF_MT_SUBTYPE, &sub);
            printf("    ✓ 音频流存在，subtype 读回成功\n");
            t->Release();
        } else {
            printf("    !! 读不到音频流（hr=0x%08lX）\n", (unsigned long)hr);
        }
        reader->Release();
    }

    if (userData) delete[] userData;
    enc->Release();
    MFShutdown();
    CoUninitialize();
    printf("\n[probe] 结论：%s\n",
           encoded.empty() ? "编码器没出样本（需进一步排查）" : "通过（AAC 常驻 + passthrough 可行）");
    return 0;
}
