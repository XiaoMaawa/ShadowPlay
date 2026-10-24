// probe_file_mf.cpp —— 用 MF SourceReader 打开指定 mp4，打印结果。
//
// 用途：排查「导出时 probe: open fail hr=0xC00D36C4」到底是文件本身读不了，
// 还是副本/路径的问题。
//
//   node scripts/probe-build.mjs native/probe_file_mf.cpp native/build/probe_file_mf.exe --with-mf
//   native/build/probe_file_mf.exe <file>
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <cstdio>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: probe_file_mf.exe <file>\n");
        return 1;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_FULL);

    const char* utf8 = argv[1];
    wchar_t wide[MAX_PATH * 2] = {0};
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, MAX_PATH * 2);

    FILE* f = _wfopen(wide, L"rb");
    if (!f) {
        printf("open file failed\n");
        return 1;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fclose(f);
    printf("file size = %ld\n", size);

    IMFSourceReader* r = nullptr;
    HRESULT hr = MFCreateSourceReaderFromURL(wide, nullptr, &r);
    printf("MFCreateSourceReaderFromURL -> 0x%08lX\n", (unsigned long)hr);
    if (FAILED(hr) || !r) return 1;

    IMFMediaType* nt = nullptr;
    hr = r->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &nt);
    printf("GetNativeMediaType(video) -> 0x%08lX\n", (unsigned long)hr);
    if (SUCCEEDED(hr) && nt) {
        UINT32 w = 0, h = 0, num = 0, den = 1;
        MFGetAttributeSize(nt, MF_MT_FRAME_SIZE, &w, &h);
        MFGetAttributeRatio(nt, MF_MT_FRAME_RATE, &num, &den);
        printf("  video %ux%u @ %u/%u\n", w, h, num, den);
        nt->Release();
    }
    nt = nullptr;
    hr = r->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &nt);
    printf("GetNativeMediaType(audio) -> 0x%08lX\n", (unsigned long)hr);
    if (SUCCEEDED(hr) && nt) {
        UINT32 rate = 0, ch = 0;
        nt->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        nt->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
        printf("  audio %u Hz %u ch\n", rate, ch);
        nt->Release();
    }
    // 读一段视频样本，看看能不能真的解出帧。
    PROPVARIANT dur;
    PropVariantInit(&dur);
    hr = r->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &dur);
    if (SUCCEEDED(hr)) printf("  duration = %.3f s\n", (double)dur.uhVal.QuadPart / 1e7);
    PropVariantClear(&dur);

    DWORD flags = 0;
    LONGLONG ts = 0;
    IMFSample* s = nullptr;
    hr = r->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &s);
    printf("ReadSample(video) -> 0x%08lX flags=0x%lX sample=%p\n", (unsigned long)hr,
           (unsigned long)flags, (void*)s);
    if (s) s->Release();
    r->Release();
    MFShutdown();
    CoUninitialize();
    return 0;
}
