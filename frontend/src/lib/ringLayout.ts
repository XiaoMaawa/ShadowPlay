// frontend/src/lib/ringLayout.ts
//
// 共享内存帧环的字段偏移 —— 必须与 native/ShadowRing.h 里的 ShadowRingHeader 逐字节一致。
// 这些常量在 preload（隔离世界）和渲染进程（主世界）里共用；改 C++ 结构体时这里也要改。
//
// 偏移量的权威来源是 native/ring_offsets_check.cpp 的输出：
//   native\build\build-offsets.bat
//   native\build\offsets.exe
// 不要靠数结构体成员手工推算 —— 对齐填充很容易算错。

export const RING_MAGIC = 0x46524853 // 'SHRF'
export const RING_VERSION = 1

/** uint32 字段（用 DataView.getUint32(v, true) 读） */
export const RING_U32 = {
  magic: 0,
  version: 4,
  headerSize: 8,
  slotCount: 12,
  width: 16,
  height: 20,
  stride: 24,
  format: 28,
  publishedSlot: 40,
  producerPid: 48,
  consumerPid: 52,
} as const

/** uint64 字段（用 DataView.getBigUint64(v, true) 读） */
export const RING_U64 = {
  /** C++ 侧的发布序号（服务端每次发帧 +1） */
  counter: 32,
  /**
   * 同步进 V8 缓冲区后的"版本号"（sequence lock）。
   * 同步器在**数据全部写完**之后才把它设成 counter，写的过程中为 0。
   * 读像素前后各读一次：两次相等且非 0，就说明读到的是完整一帧。
   */
  syncVersion: 192,
  qpcFrequency: 56,
  lastPublishQpc: 64,
  producedFrames: 88,
  droppedFrames: 96,
  repeatContent: 104,
} as const

/** int64 数组 / 其它。slotOwners 等数组的真实字节偏移由 offsetof 决定。 */
export const RING_OFFSETS = {
  slotOwners: 112,
  slotOwnQpc: 144,
  slotStride: 176,
  modeChangeCount: 184,
  captureErrors: 188,
  syncVersion: 192,
} as const

/** sizeof(ShadowRingHeader)：槽位从对齐后的偏移开始，这个值是 addon 报告的 headerSize */
export const RING_HEADER_SIZE = 432

/** C++ 侧写入的"该槽空闲"标记 */
export const SLOT_FREE = -1n
/** C++ 侧写入的"生产者正在写"标记 */
export const SLOT_PRODUCER = 0n

export interface RingInfo {
  session: string
  byteLength: number
  headerSize: number
  slotCount: number
  slotStride: number
  sourceWidth: number
  sourceHeight: number
  producerPid: number
}

export interface FrameInfo {
  counter: bigint
  slot: number
  width: number
  height: number
  stride: number
  pixelOffset: number
  byteLength: number
  /**
   * true == 这一帧的像素和上一帧完全相同（桌面静止时服务端按帧率重发）。
   *
   * 拿到这样的帧不需要 `texSubImage2D`：纹理里装的就是同一张图。1440p 全
   * 分辨率下一次上传是 14.7MB，静止画面按 60/s 重发就是 880MB/s 的纯浪费。
   */
  repeat: boolean
}

/** 读取当前已发布的帧。不检查槽位归属，调用方必须自己校验。 */
export function readPublishedFrame(view: DataView, info: RingInfo): FrameInfo | null {
  const slot = view.getUint32(RING_U32.publishedSlot, true)
  if (slot >= info.slotCount) return null

  // 每个槽自己的头部里带着这一帧的几何信息
  const base = info.headerSize + info.slotStride * slot
  const width = view.getUint32(base + RING_U32.width, true)
  const height = view.getUint32(base + RING_U32.height, true)
  const stride = view.getUint32(base + RING_U32.stride, true)
  if (!width || !height || !stride) return null

  return {
    counter: view.getBigUint64(RING_U64.counter, true),
    slot,
    width,
    height,
    stride,
    pixelOffset: base + info.headerSize,
    byteLength: stride * height,
    // 服务端标记"和上一帧像素完全相同"。注意它和 counter 是分开写的两个字段，
    // 中间可能被一次新的发布插进来 —— 所以只在"槽号和上次一样"时才敢用
    // （槽换了说明像素确实换过），这个判断在 usePreview 里做。
    repeat: view.getBigUint64(RING_U64.repeatContent, true) === 1n,
  }
}

export function readCounters(view: DataView) {
  return {
    syncVersion: view.getBigUint64(RING_U64.syncVersion, true),
    produced: view.getBigUint64(RING_U64.producedFrames, true),
    dropped: view.getBigUint64(RING_U64.droppedFrames, true),
  }
}

/**
 * 启动自检：把头部里几个关键字段和预期值对一遍。
 * 偏移量写错时，这里会立刻报出来，而不是等到画面上出现花屏。
 */
export function verifyRingHeader(view: DataView, info: RingInfo): string | null {
  const magic = view.getUint32(RING_U32.magic, true)
  if (magic !== RING_MAGIC) {
    return `magic 校验失败：0x${magic.toString(16)}，期望 0x${RING_MAGIC.toString(16)}`
  }
  const version = view.getUint32(RING_U32.version, true)
  if (version !== RING_VERSION) {
    return `帧环版本不匹配：${version}，期望 ${RING_VERSION}`
  }
  const headerSize = view.getUint32(RING_U32.headerSize, true)
  if (headerSize !== RING_HEADER_SIZE) {
    return `headerSize 不一致：JS 读到 ${headerSize}，期望 ${RING_HEADER_SIZE}（ShadowRing.h 改过？）`
  }
  const slotStride = Number(view.getBigInt64(RING_OFFSETS.slotStride, true))
  if (slotStride <= 0) {
    return `slotStride 读取异常：${slotStride}（RING_OFFSETS.slotStride 偏移可能是错的）`
  }
  if (slotStride !== info.slotStride) {
    return `slotStride 不一致：JS ${slotStride} vs addon ${info.slotStride}`
  }
  const slotCount = view.getUint32(RING_U32.slotCount, true)
  if (slotCount !== info.slotCount) {
    return `slotCount 不一致：JS ${slotCount} vs addon ${info.slotCount}`
  }
  const producerPid = view.getUint32(RING_U32.producerPid, true)
  if (producerPid === 0) {
    return 'producerPid 为 0（RING_U32.producerPid 偏移可能是错的）'
  }
  return null
}
