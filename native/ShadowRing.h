// ShadowRing.h — 共享内存帧环的契约（C 兼容）
//
// 这个头文件同时被两边包含，是唯一的数据布局定义：
//   * backend/  —— C++ 采集服务（生产者，创建共享内存 + 事件）
//   * bridge/   —— Electron 的 N-API 桥接层（消费者，打开共享内存 + 事件）
//
// 布局：
//   [ ShadowRingHeader ][ slot 0 ][ slot 1 ][ slot 2 ]
//
// 同步模型（跨进程）：
//   * 服务端把发布的帧写进共享内存，header.counter 就是"发布序号"。
//   * 消费端（Electron 的 addon）把最新一帧拷进一块 V8 管理的缓冲区，
//     **拷完之后**才把缓冲区里的 header.version 设成 counter；
//     写入过程中 version 为 0。这就是一个标准的 sequence lock：
//     消费者读像素前后各读一次 version，两次相同且非 0 就说明读到的是完整一帧。
//   * 因此服务端不需要知道消费者读到哪了，可以放心轮转覆盖全部槽位，
//     永远不会因为"消费者占着槽"而丢帧。
//   * slotOwners / slotOwnQpc 保留下来只用于诊断，不参与同步。
//
// 进程外通知：
//   frameEvent   —— 生产者写完一帧后 SetEvent，消费者 WaitForSingleObject。
//   readyEvent   —— 生产者初始化完成后 SetEvent，消费者用它做握手。
//   disconnectEvent —— 消费者关闭时 SetEvent，生产者用它感知消费者消失。

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// "SHRF" —— 小端字节序
#define SHADOW_RING_MAGIC 0x46524853u
#define SHADOW_RING_VERSION 1u

// 槽位对齐：像素数据必须按 64 字节对齐存放（D3D11 / WebGL 上传都受益）。
#define SHADOW_SLOT_ALIGN 64u

// 默认槽位数：
//   1 个给消费者（渲染进程正在显示的那一帧）+ 3 个给生产者轮转。
//   渲染进程在 WebGL 上传完成前一直占着自己那个槽，生产者靠多出来的 3 个
//   继续出帧，因此生产端几乎永远不会因为“槽不够”而丢帧。
#define SHADOW_SLOT_COUNT 4

// 一个槽位若被消费者占用超过这个毫秒数，生产者判定为泄漏并回收。
#define SHADOW_SLOT_LEAK_MS 4000

#pragma pack(push, 8)
typedef struct ShadowRingHeader {
    volatile uint32_t magic;        // SHADOW_RING_MAGIC，初始化完成后写入
    volatile uint32_t version;      // SHADOW_RING_VERSION（结构体/协议版本，不是帧序号）
    volatile uint32_t headerSize;   // sizeof(ShadowRingHeader)
    volatile uint32_t slotCount;    // 实际槽位数（<= SHADOW_SLOT_COUNT）
    volatile uint32_t width;        // 桌面宽（像素）
    volatile uint32_t height;       // 桌面高（像素）
    volatile uint32_t stride;       // 每行字节数 == width * 4
    volatile uint32_t format;       // 0 == DXGI_FORMAT_B8G8R8A8_UNORM（自上而下）
    volatile uint64_t counter;      // 发布序号，每帧 +1（从 1 开始）
    volatile uint32_t publishedSlot;// 最新完整帧所在的槽
    volatile uint32_t reserved0;

    volatile uint32_t producerPid;
    volatile uint32_t consumerPid;  // 0 表示未连接
    volatile int64_t  qpcFrequency;
    volatile int64_t  lastPublishQpc;

    // 消费者心跳（QPC）。消费者只需在空闲时也定期更新它，
    // 生产者据此判断"消费者是不是已经死了" —— 只靠 pid 存活判断不够：
    // 消费者崩溃后槽位会一直被占着，pid 还可能被系统复用。
    volatile int64_t  consumerHeartbeatQpc;
    volatile uint32_t reservedHeartbeat;
    volatile uint32_t reserved1;

    volatile uint64_t producedFrames;
    volatile uint64_t droppedFrames;   // 保留字段（当前不再递增，见上方同步模型说明）
    // 1 == 这一帧的像素与上一帧**完全相同**（桌面静止时按帧率重发的那一类）。
    //
    // 字段位置是从原来的 skippedFrames 沿用下来的（那个字段一直没被使用），
    // 所以布局没变、SHADOW_RING_VERSION 不用动：老生产者不写它（恒 0），
    // 消费者照常拷贝像素；新生产者写 1 时，消费者可以跳过像素 memcpy 和
    // GPU 纹理上传 —— 画面一像素都没变，重传纯属白烧带宽。
    volatile uint64_t repeatContent;

    volatile int64_t  slotOwners[SHADOW_SLOT_COUNT]; // -1 空闲，否则是占用它的 PID
    volatile int64_t  slotOwnQpc[SHADOW_SLOT_COUNT]; // 占用时刻（QPC），用于回收
    volatile int64_t  slotStride;                    // 每个槽的字节跨度

    volatile uint32_t modeChangeCount; // 分辨率/格式变化的次数
    volatile uint32_t captureErrors;   // DXGI 层错误计数

    // 同步进消费者缓冲区后的版本号（sequence lock）。
    // 写入过程中该字段为 0；数据全部写完才把它设成 counter。
    // 消费者读像素前后各读一次：两次相同且非 0 就说明读到的是完整一帧。
    //
    // 注意：它故意和上面的 version（协议版本）分开，两者语义完全不同。
    volatile uint64_t syncVersion;

    volatile uint32_t reserved[58];    // 预留，保持 header ≤ 512 字节
} ShadowRingHeader;

// 编译期检查：header 必须能塞进 512 字节，这样槽位从固定偏移开始。
typedef char shadow_ring_header_fits[sizeof(ShadowRingHeader) <= 512 ? 1 : -1];
#pragma pack(pop)

// 槽 s 的字节偏移：每个槽都以一个完整的 ShadowRingHeader 开头（复用同样的字段
// 存这一帧自己的几何信息，所以消费者不需要依赖全局字段，也不必加锁读两次）。
// slotStride 已经是对齐过的值。
static inline uint64_t ShadowRingSlotOffset(const ShadowRingHeader* h, uint32_t s) {
    return (uint64_t)h->headerSize + (uint64_t)h->slotStride * (uint64_t)s;
}

#ifdef __cplusplus
} // extern "C"
#endif
