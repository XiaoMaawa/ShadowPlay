// app/probe-core.cjs — 共享内存帧环的"只读自检"
//
// 不依赖 Electron，可以在纯 Node 里跑；main.js 的 --probe 模式用它验证
// 「后端 + 共享内存 + addon」这条链路，避免出问题时不知道是哪一层坏了。
//
// 用法：
//   const { probe } = require('./probe-core.cjs')
//   const r = probe(session, 8000)

const path = require('node:path')

// 两份 addon 是同一个 .obj 链出来的，只是宿主不同：
//   shadow_ring.node       ← electron.lib（N-API 符号从 electron.exe 里要）
//   shadow_ring.nodejs.node← node.lib（从 node.exe 里要）
// 用错宿主会加载失败（ERR_DLOPEN_FAILED），所以按当前进程类型挑。
const IS_ELECTRON = !!process.versions.electron
const ADDON_FILE = IS_ELECTRON ? 'shadow_ring.node' : 'shadow_ring.nodejs.node'
const ADDON_PATH = process.env.SHADOW_ADDON
  ? path.resolve(process.env.SHADOW_ADDON)
  : path.join(__dirname, '..', 'native', 'build', ADDON_FILE)

// 必须与 native/ShadowRing.h 的 ShadowRingHeader 一致。
// 权威来源：native\build\build-offsets.bat 打印的 offsetof 结果。
const RING = {
  MAGIC: 0x46524853,
  VERSION: 1,
  U32: {
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
  },
  U64: { counter: 32, syncVersion: 192, producedFrames: 88, droppedFrames: 96, skippedFrames: 104 },
  OFFSETS: {
    slotOwners: 112,
    slotOwnQpc: 144,
    slotStride: 176,
    modeChangeCount: 184,
    captureErrors: 188,
    syncVersion: 192,
  },
  // sizeof(ShadowRingHeader)；权威来源：npm run offsets
  HEADER_SIZE: 432,
}

function loadAddon() {
  const addon = require(ADDON_PATH)
  const missing = ['waitReady', 'open', 'hello', 'start', 'waitConsumed', 'consume', 'stats', 'close']
    .filter((f) => typeof addon[f] !== 'function')
  return { addon, missing }
}

function probe(session, timeoutMs = 8000) {
  const result = { ok: false, session }
  let addon
  try {
    const loaded = loadAddon()
    addon = loaded.addon
    result.addon = { path: ADDON_PATH, missing: loaded.missing, present: Object.keys(addon).sort() }
    if (loaded.missing.length) {
      result.error = 'addon 缺少导出：' + loaded.missing.join(', ')
      return result
    }
  } catch (err) {
    result.addon = { path: ADDON_PATH, error: err.message }
    result.error = '加载 addon 失败：' + err.message
    return result
  }

  try {
    result.waitReadyBeforeBackend = addon.waitReady('definitely-not-running')

    const ring = addon.open(session, timeoutMs)
    result.ring = ring

    // start() 返回的是 { buffer, byteLength, headerSize, slotCount, slotStride, ... }
    const handle = addon.start(session)
    const ab = handle && handle.buffer
    result.start = {
      handleKeys: handle ? Object.keys(handle) : null,
      handleType: typeof handle,
      bufferType: typeof ab,
      bufferTag: ab ? Object.prototype.toString.call(ab) : null,
      byteLength: handle?.byteLength,
      headerSize: handle?.headerSize,
      slotCount: handle?.slotCount,
      slotStride: handle?.slotStride,
      sourceWidth: handle?.sourceWidth,
      sourceHeight: handle?.sourceHeight,
      isArrayBuffer: ab instanceof ArrayBuffer,
      ctor: ab && ab.constructor ? ab.constructor.name : String(ab),
    }
    if (!(ab instanceof ArrayBuffer)) {
      result.error = `start() 没有返回 ArrayBuffer，而是 ${result.start.ctor}（keys=${result.start.handleKeys}）`
      return result
    }

    const view = new DataView(ab)
    const magic = view.getUint32(RING.U32.magic, true)
    const version = view.getUint32(RING.U32.version, true)
    result.header = {
      magic: '0x' + magic.toString(16),
      magicOk: magic === RING.MAGIC,
      version,
      versionOk: version === RING.VERSION,
      headerSize: view.getUint32(RING.U32.headerSize, true),
      slotCount: view.getUint32(RING.U32.slotCount, true),
      slotStride: Number(view.getBigInt64(RING.OFFSETS.slotStride, true)),
      sourceWidth: view.getUint32(RING.U32.width, true),
      sourceHeight: view.getUint32(RING.U32.height, true),
      producerPid: view.getUint32(RING.U32.producerPid, true),
      consumerPid: view.getUint32(RING.U32.consumerPid, true),
    }
    if (!result.header.magicOk) {
      result.error = `magic 校验失败：${result.header.magic}`
      return result
    }

    // 等后端真的发布一帧。注意：addon.poll() 负责把共享内存里的最新一帧
    // 同步进 V8 缓冲区，所以这里必须持续调用它，再读缓冲区里的 counter。
    const startCounter = view.getBigUint64(RING.U64.counter, true)
    const deadline = Date.now() + timeoutMs
    let counter = startCounter
    let pollCalls = 0
    while (Date.now() < deadline) {
      addon.poll(session)
      pollCalls++
      const now = view.getBigUint64(RING.U64.counter, true)
      if (now > startCounter) {
        counter = now
        break
      }
      addon.waitConsumed(session, 10)
    }
    result.pollCalls = pollCalls

    const slot = view.getUint32(RING.U32.publishedSlot, true)
    const slotCount = result.start.slotCount
    const slotStride = result.start.slotStride
    const base = result.start.headerSize + slotStride * slot
    const width = view.getUint32(base + RING.U32.width, true)
    const height = view.getUint32(base + RING.U32.height, true)
    const stride = view.getUint32(base + RING.U32.stride, true)

    result.frames = {
      startCounter: startCounter.toString(),
      counter: counter.toString(),
      produced: counter > startCounter,
      publishedSlot: slot,
      slotInRange: slot < slotCount,
      width,
      height,
      stride,
      pixelOffset: base + result.start.headerSize,
      producedFrames: view.getBigUint64(RING.U64.producedFrames, true).toString(),
      droppedFrames: view.getBigUint64(RING.U64.droppedFrames, true).toString(),
    }

    if (counter <= startCounter) {
      result.error = `${timeoutMs}ms 内后端没有发布任何帧`
      return result
    }

    // 采样几个像素，确认不是"全 0 的黑屏"
    const pixels = new Uint8Array(ab, base + result.start.headerSize, stride * height)
    let nonZero = 0
    let sampled = 0
    const stepBytes = Math.max(4, Math.floor(pixels.length / 2000 / 4) * 4)
    for (let i = 0; i < pixels.length; i += stepBytes) {
      sampled++
      if (pixels[i] !== 0) nonZero++
    }
    result.pixels = { sampled, nonZero, allZero: nonZero === 0 }

    result.stats = addon.stats(session)
    result.ok = true
    addon.close(session)
    return result
  } catch (err) {
    result.error = err.message
    try {
      addon.close(session)
    } catch {
      /* 忽略 */
    }
    return result
  }
}

module.exports = { probe, RING, ADDON_PATH, loadAddon }
