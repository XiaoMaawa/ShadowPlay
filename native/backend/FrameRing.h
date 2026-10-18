// FrameRing.h — 生产者侧的共享内存帧环。
//
// 职责：
//   1. 创建（或复用）共享内存 + 三个命名事件，并设置只允许当前用户访问的安全描述符。
//   2. 在整个环上保持一个 FILE_MAPPING，用 offset 写入映射，避免为每个槽都调用一次
//      MapViewOfFile（进程的映射视图数量是有限的，桌面分辨率变化时槽位跨度还会变）。
//   3. 把采集到的帧按“轮转后台缓冲”的方式发布出去，消费者读到的永远是完整帧。
#pragma once

#include <windows.h>

#include "ShadowRing.h"
#include "RingNames.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace shadow {

class FrameRing {
public:
    FrameRing() = default;
    ~FrameRing();

    FrameRing(const FrameRing&) = delete;
    FrameRing& operator=(const FrameRing&) = delete;

    // 创建成功后 readyEvent 会被置位，消费者据此完成握手。
    // maxWidth/maxHeight 决定每个槽的容量：共享内存一旦创建容量就固定，
    // 所以按“采集到的分辨率 + 余量”预留，分辨率变小时继续用，变大时重建环。
    bool Open(const std::string& sessionId, uint32_t maxWidth, uint32_t maxHeight,
              uint32_t slotCount = SHADOW_SLOT_COUNT);

    // 分辨率超出预留容量时，需要重建整个环（消费者会重新握手）。
    bool NeedsGrow(uint32_t width, uint32_t height) const {
        // 复用了别人的环时容量不归我们管，交给采集端自行裁剪
        if (attached_) return false;
        return width > capWidth_ || height > capHeight_;
    }
    uint32_t CapWidth() const { return capWidth_; }
    uint32_t CapHeight() const { return capHeight_; }

    /** 是否复用了一个已经存在的环（说明服务被重复启动） */
    bool Attached() const { return attached_; }

    // 等待消费者接入（consumerPid 由消费者写入）。
    bool WaitForConsumer(uint32_t timeoutMs);

    // 消费者是否仍然连着（pid 存活 + 心跳新鲜）
    bool ConsumerAlive(uint32_t staleMs = 3000) const;

    // 刷新消费者心跳（消费者侧调用；生产者也可代消费者刷新用于自检）
    void TouchConsumerHeartbeat();

    // 清掉"消费者已经崩溃但槽位还占着"的僵尸槽，返回回收数量。
    // 只回收既不是生产者、也不是当前消费者、且占用时间超过 SHADOW_SLOT_LEAK_MS 的槽。
    uint32_t ReclaimDeadSlots();

    // 手工把某个 pid 登记为消费者。
    // 用于自检/无界面测试：不需要真的起一个消费者进程，就能让生产者开始出帧。
    void ForceConsumer(uint32_t pid) {
        if (!header_ || pid == 0) return;
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&header_->consumerPid),
                            static_cast<LONG>(pid));
        consumerPid_ = pid;
        TouchConsumerHeartbeat();
    }

    // 分辨率/格式变化时更新头部几何信息，并复位轮转状态。
    void SetMode(uint32_t width, uint32_t height, uint32_t format);

    // 发布一帧。
    //   src 每行至少 width*4 字节，行间距为 srcStride。
    // 返回 true 表示已发布。
    // 由于消费者用 sequence lock 自检完整性，生产端永远不会因为
    // "消费者还没读完"而丢帧，所以这里基本不返回 false（仅参数非法时）。
    bool Publish(const uint8_t* src, uint32_t srcStride, uint32_t width, uint32_t height,
                 uint64_t presentQpc);

    /**
     * 重复发布最近一帧，把 header.counter 往前推。
     *
     * DXGI 桌面复制只在桌面真的有变化时才返回新帧；桌面静止时（例如用户
     * 就看着预览窗口不动），采集端会长时间没有新内容。如果这段时间完全不
     * 发布，界面也就收不到新帧，"实时预览"看起来就变成了一张静止图片。
     *
     * 所以桌面静止时按目标帧率重复发布，让下游始终有稳定的帧流：
     *   - 传了 src（采集端缓存的 staging 像素）就走正常发布，内容与上一帧相同；
     *   - src 为空则只推进 counter，复用槽里已有的像素。
     * 返回 false 表示还没有可复用的帧（从未发布过）。
     */
    bool RepublishLast(const uint8_t* src = nullptr, uint32_t srcStride = 0);

    /** 最近一帧的发布时间（QPC），没有则为 0 */
    int64_t LastPublishQpc() const { return lastPublishedQpc_; }

    // 消费者断开时置位，生产者用来感知“前端关了”。
    HANDLE DisconnectEvent() const { return disconnectEvent_; }

    uint32_t ConsumerPid() const { return consumerPid_; }
    uint32_t Width() const { return width_; }
    uint32_t Height() const { return height_; }
    uint64_t Produced() const { return produced_; }
    uint64_t Dropped() const { return dropped_; }
    uint32_t ModeChanges() const {
        return header_ ? header_->modeChangeCount : 0u;
    }

    const std::string& LastError() const { return lastError_; }

private:
    bool  CreateMapping(const std::string& name, uint64_t totalBytes);
    bool  EnsureMap();
    void  CloseAll();
    int   AcquireConsumerSlot();
    void  PublishInternal(int slot, uint32_t width, uint32_t height, uint32_t stride,
                          uint64_t presentQpc);
    bool  ValidateHeader() const;

    ShadowRingHeader* Header() const { return header_; }
    ShadowRingHeader* SlotHeader(int slot) const;

    HANDLE mapping_ = nullptr;
    HANDLE readyEvent_ = nullptr;
    HANDLE frameEvent_ = nullptr;
    HANDLE disconnectEvent_ = nullptr;

    uint8_t* base_ = nullptr;            // 整个环的映射视图
    ShadowRingHeader* header_ = nullptr; // == base_

    uint32_t slotCount_ = SHADOW_SLOT_COUNT;
    uint64_t slotStride_ = 0;
    uint64_t mapBytes_ = 0;
    uint32_t capWidth_ = 0;
    uint32_t capHeight_ = 0;
    bool     attached_ = false;
    std::string session_;

    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t consumerPid_ = 0;
    int      nextSlot_ = 0;
    int      lastPublishedSlot_ = -1;   // 最近一次发布的槽，用于"重复发布最后一帧"
    int64_t  lastPublishedQpc_ = 0;
    uint64_t produced_ = 0;
    uint64_t dropped_ = 0;
    int64_t  qpcFrequency_ = 1;

    std::string lastError_;
};

} // namespace shadow
