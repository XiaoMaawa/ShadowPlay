// native/ring_offsets_check.cpp — 打印 ShadowRingHeader 的真实字段偏移
//
// 用途：JS 侧（preload.cjs / ringLayout.ts / probe-core.cjs）里手写的偏移量必须和
// C++ 结构体逐字节一致。改完 ShadowRing.h 之后编译运行这个文件，
// 把输出抄回 JS 侧，就不会出现"能连上但一帧都读不到"这种隐蔽故障。
//
// 编译运行（见 native/build 下的 build-offsets.bat）：
//   cl /nologo /EHsc /std:c++17 /I"native" /Fe"native\build\offsets.exe" native\ring_offsets_check.cpp
//   native\build\offsets.exe

#include "ShadowRing.h"

#include <cstddef>
#include <cstdio>

int main() {
    std::printf("// 由 native/ring_offsets_check.cpp 生成，请勿手改\n");
    std::printf("sizeof(ShadowRingHeader) = %zu\n", sizeof(ShadowRingHeader));
    std::printf("SHADOW_SLOT_COUNT        = %d\n", SHADOW_SLOT_COUNT);
    std::printf("\n-- JS RING_U32 --\n");
    std::printf("magic         = %zu\n", offsetof(ShadowRingHeader, magic));
    std::printf("version       = %zu\n", offsetof(ShadowRingHeader, version));
    std::printf("headerSize    = %zu\n", offsetof(ShadowRingHeader, headerSize));
    std::printf("slotCount     = %zu\n", offsetof(ShadowRingHeader, slotCount));
    std::printf("width         = %zu\n", offsetof(ShadowRingHeader, width));
    std::printf("height        = %zu\n", offsetof(ShadowRingHeader, height));
    std::printf("stride        = %zu\n", offsetof(ShadowRingHeader, stride));
    std::printf("format        = %zu\n", offsetof(ShadowRingHeader, format));
    std::printf("publishedSlot = %zu\n", offsetof(ShadowRingHeader, publishedSlot));
    std::printf("producerPid   = %zu\n", offsetof(ShadowRingHeader, producerPid));
    std::printf("consumerPid   = %zu\n", offsetof(ShadowRingHeader, consumerPid));

    std::printf("\n-- JS RING_U64 --\n");
    std::printf("counter        = %zu\n", offsetof(ShadowRingHeader, counter));
    std::printf("qpcFrequency   = %zu\n", offsetof(ShadowRingHeader, qpcFrequency));
    std::printf("lastPublishQpc = %zu\n", offsetof(ShadowRingHeader, lastPublishQpc));
    std::printf("producedFrames = %zu\n", offsetof(ShadowRingHeader, producedFrames));
    std::printf("droppedFrames  = %zu\n", offsetof(ShadowRingHeader, droppedFrames));
    std::printf("skippedFrames  = %zu\n", offsetof(ShadowRingHeader, skippedFrames));

    std::printf("\n-- JS RING_OFFSETS --\n");
    std::printf("slotOwners        = %zu\n", offsetof(ShadowRingHeader, slotOwners));
    std::printf("slotOwnQpc        = %zu\n", offsetof(ShadowRingHeader, slotOwnQpc));
    std::printf("slotStride        = %zu\n", offsetof(ShadowRingHeader, slotStride));
    std::printf("modeChangeCount   = %zu\n", offsetof(ShadowRingHeader, modeChangeCount));
    std::printf("captureErrors     = %zu\n", offsetof(ShadowRingHeader, captureErrors));
    std::printf("syncVersion       = %zu\n", offsetof(ShadowRingHeader, syncVersion));
    std::printf("consumerHeartbeat = %zu\n",
                offsetof(ShadowRingHeader, consumerHeartbeatQpc));
    return 0;
}
