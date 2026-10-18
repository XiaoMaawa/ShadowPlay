// frontend/src/composables/usePreview.ts
//
// 预览的核心逻辑：拿帧缓冲区 -> 每帧检查有没有新帧 -> 上传 WebGL 纹理。
//
// 取帧协议（sequence lock）：
//   服务端把最新一帧拷进我们这块 V8 缓冲区后，才把 header.syncVersion 设成发布序号；
//   写的过程中 version 是 0。
//   这里在**上传纹理之前和之后各读一次 version**：两次相同且非 0，
//   说明期间同步器没有写入新数据，这一帧就是完整的；否则丢掉等下一帧。
//   这样既不会画出半帧，也不需要任何锁。

import { ref, shallowRef } from 'vue'
import { FrameRenderer } from '@/lib/frameRenderer'
import {
  RING_U32,
  readCounters,
  readPublishedFrame,
  type FrameInfo,
  type RingInfo,
} from '@/lib/ringLayout'

interface ShadowPreviewApi {
  addonStatus(): { loaded: boolean; error: string | null; path: string }
  status(): {
    state: string
    fatal: { stage?: string; message?: string } | null
    session: string | null
    ringInfo: RingInfo | null
  }
  /** 不传 sessionId 时回退到第一路已就绪的会话（兼容旧界面） */
  getRingBuffer(sessionId?: string): { buffer: ArrayBuffer; info: RingInfo; session?: string } | null
  frame(): { ok: true; [k: string]: unknown } | { ok: false; reason: string }
  release(slot: number): boolean
  stats(): Record<string, unknown> | null
  onEvent(cb: (payload: Record<string, unknown>) => void): () => void
  retry(): boolean
  serviceInfo(): Promise<Record<string, unknown>>
  listWindows?(): Promise<{ ok: boolean; items: unknown[]; error?: string }>
  saveProfiles?(doc: unknown): Promise<{ ok: boolean; path?: string; error?: string; restarted?: boolean }>
  /** 只保存一路（主进程从磁盘读现配，只替换这一路，其余分路不动） */
  saveProfile?(id: string, patch: unknown): Promise<{
    ok: boolean
    path?: string
    error?: string
    restarted?: boolean
    changed?: boolean
    reloaded?: string[]
    failed?: { id: string; error?: string }[]
  }>
  serviceLive?(): Promise<{ sessions: unknown[]; owned: boolean; updatedAt: number }>
  exportReplay?(id: string, seconds?: number): Promise<{ ok: boolean; error?: string }>
  onServiceEvent?(cb: (payload: Record<string, unknown>) => void): () => void
}

declare global {
  interface Window {
    shadowPreview?: ShadowPreviewApi
  }
}

export interface PreviewStats {
  renderFps: number
  /**
   * 显示用的平滑帧率。`renderFps` 是**瞬时值**（每 500ms 重算一次，
   * 60fps 下窗口里多一帧少一帧就是 61.5 / 59.7，几乎没法看），
   * 这里用滑动中位数再夹到最近的整数，顶栏显示它。
   */
  renderFpsShown: number
  backendFps: number
  rendered: number
  skipped: number
  produced: bigint
  uploadMs: number
  sourceWidth: number
  sourceHeight: number
  connected: boolean
}

/**
 * 单路预览的帧率账本。
 *
 * 为什么要按路各记一份：帧率是**那一路采集节奏**的属性，不是渲染循环的属性。
 * 以前所有路共用一份 500ms 窗口和 8 个样本的中位数历史，切到另一路时：
 *   * 那一窗口里混着上一路的帧，算出来的数是两路的混合值；
 *   * 中位数历史里全是上一路的样本，要等 4 秒（8 个窗口）才被彻底挤出，
 *     界面看起来就是"切过去了还在慢慢过渡到正确帧率"。
 * 按路各记一份之后，切换只是换一个指针，显示值当拍就是那一路自己的真值。
 */
interface FpsLedger {
  /** 本路 500ms 统计窗口的起点（performance.now） */
  windowStart: number
  /** 本窗口内已绘制帧数 */
  windowFrames: number
  /** 最近若干个窗口的瞬时帧率，用来算中位数 */
  history: number[]
  /** 最近一次算出的瞬时帧率（切回来时先顶着显示，避免闪 0） */
  lastRaw: number
  /** 最近一次算出的中位数（切回路时**立刻**用它，不等新窗口） */
  lastShown: number
  /** 最近一次的有效绘制间隔中位数（毫秒） */
  lastIntervalMedian: number
  /** 本路累计绘制帧数（切换不清零，用于"这条路到底动没动过"） */
  rendered: number
  /** 本路最近一次上传耗时 */
  uploadMs: number
}

export function usePreview() {
  const api = window.shadowPreview

  const canvas = shallowRef<HTMLCanvasElement | null>(null)
  const running = ref(false)
  const fatal = ref<string | null>(null)
  const notices = ref<string[]>([])
  const ringInfo = ref<RingInfo | null>(null)
  const transport = ref<'none' | 'port' | 'bridge'>('none')
  const waitingService = ref(false)
  const serviceHint = ref('')
  const stats = ref<PreviewStats>({
    renderFps: 0,
    renderFpsShown: 0,
    backendFps: 0,
    rendered: 0,
    skipped: 0,
    produced: 0n,
    uploadMs: 0,
    sourceWidth: 0,
    sourceHeight: 0,
    connected: false,
  })

  let renderer: FrameRenderer | null = null
  let view: DataView | null = null
  let rafId = 0
  /** 上一次渲染过的 version（sequence lock 的版本号） */
  let lastCounter = -1n
  /**
   * 每路一份帧率账本。key 是配置 id；key `''` 是没有选中任何一路时的
   * 兜底账本（旧界面/单路模式下 preload 会自动回退到第一路）。
   */
  const ledgers = new Map<string, FpsLedger>()
  /**
   * 显示值的中位数历史长度。
   *
   * 顶栏原来直接显示 `renderFps.toFixed(0)`，而 renderFps 是每 500ms 用
   * 「窗口内帧数 ÷ 窗口实际时长」现算的：60fps 下一个窗口多收一帧就是 62，
   * 少收一帧就是 58，数字一直在跳，看起来像"帧数不稳"，其实采集和录制都是
   * 稳的。取最近 8 个窗口（约 4 秒）的**中位数**再去掉小数位，偶发的抖动
   * 就不会漏到界面上；中位数比平均数更能抗极端值（切窗口、卡一下的尖峰）。
   * 这份历史现在按路各存一份（见 FpsLedger），切换时不会互相污染。
   */
  const kFpsHistorySize = 8
  let unlisten: (() => void) | null = null
  let pixelView: Uint8Array | null = null
  let pixelViewOffset = -1
  /**
   * 画布是不是已经被清成空底了（见 renderStep 里 `!view` 那一段）。
   *
   * WebGL 的 canvas 不会自己变空：不调 draw() 就一直是上一帧。切到一路暂时
   * 没画面的配置时，必须主动清一次，否则会停着上一路的画面；但每帧都清又是
   * 白烧 GPU，所以用这个标记把"清一次"和"每帧"分开。
   */
  let blankCanvas = false
  /** rAF 回调里抛出的第一个异常（抛异常会静默掐断整条渲染循环） */
  let loopError: string | null = null
  let loopErrorCount = 0
  /**
   * 【诊断】渲染循环的执行计数。
   *
   * `rendered` 只在"真画了一帧"时才加，所以它不动既可能是循环没跑，
   * 也可能是循环在跑但每轮都被跳过 —— 这两种情况的修法完全不同。
   * 这几个计数器把每一条提前返回的路径分开记账，多量一次就知道了。
   */
  let rafTicks = 0
  let skipNoView = 0
  let skipNoRingInfo = 0
  let skipNoRenderer = 0
  let skipBudget = 0
  let skipNoNewFrame = 0

  /**
   * 渲染帧率上限。
   *
   * 采集端可能以更高频率发布（桌面复制本身可到 200Hz+），但我们没必要
   * 跟着它把每个新帧都画一遍 —— 屏幕也就 60Hz，多画纯粹是白烧 GPU。
   * 这里主动限流：离上次绘制不足一帧的时间就直接跳过这一轮 rAF。
   *
   * 想解锁就在启动时加 `--render-fps 0`（不限，跟随显示器刷新率）
   * 或 `--render-fps 120`；见 scripts/launch.mjs。
   */
  function resolveTargetFps(): number {
    const raw = (window as unknown as { shadowRenderFps?: string }).shadowRenderFps
    const n = Number(raw)
    return Number.isFinite(n) && n > 0 ? n : 0
  }
  const TARGET_FPS = resolveTargetFps()
  // 预览**不再按时间限流**（原因见 renderStep 里的长注释：墙上时间限流会
  // 和采集端 60Hz 拍频，实测把 60fps 的数据卡成 38~55fps）。
  // TARGET_FPS 现在只作为"元数据"报给自检，不再影响绘制节奏。
  const FRAME_BUDGET_MS = TARGET_FPS > 0 ? 1000 / TARGET_FPS : 0
  /** 最近若干次绘制的时间戳，用于自检时核对真实帧间隔 */
  const drawStamps: number[] = []
  /**
   * 当前预览的是哪一路配置（null = 让 preload 用第一路，兼容旧的单会话界面）。
   * 界面里"选中左侧某个配置"时改它。
   */
  let activeSessionId: string | null = null

  function notify(text: string) {
    notices.value = [...notices.value.slice(-4), text]
  }

  // ---- 按路帧率账本 -------------------------------------------------------
  //
  // 这些函数只碰 ledgers / stats，不碰 view 和像素缓冲 —— 换路时像素侧的
  // 重置在 switchSession 里做，这里只负责"数字归谁"。

  function ledgerKey(): string {
    return activeSessionId ?? ''
  }

  /**
   * 账本版本号：每算出一个新的帧率就 +1。
   *
   * `fpsOf()` 读的是 Map 里的普通对象，Vue 追踪不到它的变化，所以顶栏那边
   * 的 computed 必须顺带依赖这个响应式计数器，否则数字永远停在第一帧。
   */
  const ledgerRev = ref(0)

  /** 取（或新建）某一路的账本 */
  function ledgerFor(key: string): FpsLedger {
    let l = ledgers.get(key)
    if (!l) {
      l = {
        windowStart: 0,
        windowFrames: 0,
        history: [],
        lastRaw: 0,
        lastShown: 0,
        lastIntervalMedian: 0,
        rendered: 0,
        uploadMs: 0,
      }
      ledgers.set(key, l)
    }
    return l
  }

  /** 把某一路账本里的显示值贴到响应式 stats 上（切换路时立刻调用） */
  function applyLedger(key: string) {
    const l = ledgerFor(key)
    stats.value.renderFps = l.lastRaw
    stats.value.renderFpsShown = l.lastShown
    stats.value.uploadMs = l.uploadMs
  }

  /**
   * 用采集服务上报的实测帧率，给**还没攒出自己历史**的那几路先垫一个数。
   *
   * 为什么需要：账本的历史是靠页面自己画帧攒的，一路刚被第一次打开时
   * 账本是空的（lastShown = 0），要等统计窗口走满 500ms 才有值 —— 表现
   * 成"切到某一路，帧率先显示 0，一两秒后才跳出真值"。而服务端本来就
   * 每秒上报每一路的 actualFps，白扔了可惜，正好拿来当兜底。
   *
   * 只垫空账本：已经攒出自己历史的（history 非空）一律不动，那些是真值，
   * 服务端那个数是整条采集链路的，和页面实际画出来的帧率不是一回事。
   */
  function seedLedgerFromService(items: unknown) {
    if (!Array.isArray(items)) return
    let touched = false
    for (const raw of items) {
      if (!raw || typeof raw !== 'object') continue
      const it = raw as Record<string, unknown>
      const id = typeof it.id === 'string' ? it.id : ''
      if (!id) continue
      const fps = Number(it.actualFps)
      if (!Number.isFinite(fps) || fps <= 0) continue
      const l = ledgers.get(id)
      // 有历史就别动；没被看过的那一路还没建账本，这里顺手建一个
      if (l && l.history.length > 0) continue
      const target = ledgerFor(id)
      if (target.history.length > 0) continue
      target.lastRaw = fps
      target.lastShown = fps
      touched = true
    }
    if (!touched) return
    ledgerRev.value++
    // 当前正看着的那一路也要立刻贴上去，否则要等下一次开窗才有变化
    applyLedger(ledgerKey())
  }

  function attachBuffer(info: RingInfo, buffer: ArrayBuffer) {
    view = new DataView(buffer)
    ringInfo.value = info
    pixelView = null
    pixelViewOffset = -1
    lastCounter = -1n
  }

  /** 建立到帧环的通道：直接问 preload 要那块 ArrayBuffer */
  async function connectBuffer(info: RingInfo): Promise<boolean> {
    if (!api) return false

    try {
      // 明确传 sessionId：preload 不传时会回退到"第一路已就绪的会话"，
      // 那在预览多路的时候会永远只画同一路（而且画的是哪一路还取决于
      // 连接建立的先后顺序，看起来就像"切换预览没反应"）。
      const handle = api.getRingBuffer(activeSessionId ?? undefined)
      if (handle?.buffer) {
        attachBuffer(handle.info ?? info, handle.buffer)
        transport.value = 'bridge'
        return true
      }
      notify('preload 还没准备好帧环')
    } catch (err) {
      notify(`取帧环失败：${String(err)}`)
    }

    return false
  }

  /**
   * 换一路预览（需求 3：每个配置都要有自己的实时预览）。
   *
   * 换的是"连哪一块帧环"，不是"再开一个循环" —— 整个界面只跑一条 rAF，
   * 逐路各开一条的话每路都会自己限流到 60fps，多路一起看就会互相抢时间。
   */
  async function switchSession(id: string | null) {
    if (id === activeSessionId) return
    activeSessionId = id
    view = null
    pixelView = null
    pixelViewOffset = -1
    lastCounter = -1n
    ringInfo.value = null
    serviceHint.value = ''
    waitingService.value = false

    // 把画布清掉，别让上一路画面留在那儿。
    //
    // WebGL canvas 不清空就一直显示上一帧，而切到的那一路要是暂时没有环
    // （窗口还没匹到、服务刚重启），renderStep 会一直 early-return，于是
    // 旧画面就永久停在那里 —— 上层 NO SIGNAL 一撤，露出来的就是**别的分路**
    // 的画面（踩过：切到窗口源那一路，遮罩消失那一下闪出显示器捕获的整屏）。
    renderer?.clear()

    // 立刻换成这一路自己的帧率，不等它攒满一个统计窗口。
    // 累计绘制帧数等"总量"计数器不跟着换 —— 它们是整个预览的累计值，
    // 按路清零会让"画面到底动没动"这个判据失效；帧率才是按路的属性。
    applyLedger(ledgerKey())

    const info = api?.status()?.ringInfo ?? null
    if (!info) {
      await connect()
      return
    }
    await connectBuffer(info as RingInfo)
  }

  async function connect(): Promise<boolean> {
    if (!api) {
      fatal.value = 'window.shadowPreview 不存在：preload 没加载成功'
      return false
    }

    const addon = api.addonStatus()
    if (!addon.loaded) {
      fatal.value = `原生扩展加载失败：${addon.error ?? '未知错误'}\n路径：${addon.path}`
      return false
    }

    const status = api.status()
    waitingService.value = status.state === 'waiting-service'
    if (status.state === 'failed') {
      fatal.value = status.fatal?.message ?? '初始化失败'
      return false
    }

    const info = (status.ringInfo ?? ringInfo.value) as RingInfo | null
    if (!info) {
      // preload 还在轮询等采集服务
      serviceHint.value =
        '还没有检测到采集服务。请在项目根目录另开一个终端执行：npm run service'
      waitingService.value = true
      return false
    }

    const ok = await connectBuffer(info)
    if (!ok) {
      fatal.value = '帧环没能通过 MessagePort 交给渲染进程'
      return false
    }
    waitingService.value = false
    return true
  }

  function renderLoop() {
    if (!running.value) return
    rafId = requestAnimationFrame(renderLoop)
    rafTicks++
    try {
      renderStep()
    } catch (err) {
      // 循环外的 catch 是必须的：rAF 回调里抛异常会**静默终止整条 rAF 链**，
      // 表现成"画面只动了两下就不动了"，而且控制台之外什么都看不到。
      // 记录第一次异常，然后在 __shadowStats 里暴露出来。
      loopError ??= err instanceof Error ? `${err.name}: ${err.message}` : String(err)
      loopErrorCount++
    }
  }

  function renderStep() {
    // 每一步都单独计数：只统计"画了几帧"无法区分"循环没在跑"和
    // "循环在跑但每轮都被跳过"，而这两种情况要修的地方完全不同。
    if (!view) {
      skipNoView++
      // 这一路暂时没有环（窗口没匹到 / 服务刚重启）。只清一次就够，
      // 每帧都清等于白烧 GPU —— 用 blankCanvas 标记"已经清过了"。
      // 不清的话 canvas 上会一直挂着上一路的最后一帧，看起来像"画面卡死"
      // 或者干脆就是**别人家的画面**。
      if (renderer && !blankCanvas) {
        renderer.clear()
        blankCanvas = true
      }
      return
    }
    // 有帧了，解除"已清空"标记，下面的绘制才有意义
    blankCanvas = false
    if (!ringInfo.value) {
      skipNoRingInfo++
      return
    }
    if (!renderer) {
      skipNoRenderer++
      return
    }

    // ---- 去掉"按时间限流" -------------------------------------------------
    // 这里原来有两层节流，实测都会把真实帧吃掉：
    //   1. `nowMs < nextFrameAt` 的绝对时间轴限流；
    //   2. `nowMs - lastDrawAt < 预算` 的最小间隔限流。
    //
    // 问题的本质：采集端固定 60Hz 产出（实测共享环 59.9~60.0/s），而 rAF 在
    // 这台机器上是 **239Hz**。无论用哪个阈值去"限"60fps，rAF 的相位都会
    // 不断漂移 —— 新帧到达的那一轮有时距离上次绘制只有 13ms，被阈值误杀，
    // 而下一轮 rAF 已经没有新帧了（被下面的 syncVersion 判据挡掉）。
    // 实测：59.7 个可画机会里有 21.7 个被限流白白丢掉，界面显示 38~55fps。
    //
    // 正确做法是：**只按"有没有新帧"决定画不画**，不按时间去限。
    // 因为新帧的产出本身就是 60fps —— 这就是天然的 60fps 上界，多一层
    // 时间限流除了制造抖动没有别的作用。高刷屏上同一帧会被 rAF 多轮碰到，
    // 但下面的 syncVersion 判据让第二轮直接返回，不会重复上传纹理。
    //
    // 如果以后确实需要压低预览帧率（比如笔记本省电），正确做法是在
    // "新帧"计数上做抽样（每 N 帧画 1 帧），而不是按墙上时间去卡 —— 后者
    // 会和采集时钟拍频，产生周期性掉帧。
    renderOnce()
  }

  /** 本轮渲染：有完整新帧就上传纹理并绘制 */
  function renderOnce() {
    // view 在 renderStep 里已经确认非空
    const current = view
    if (!current) {
      skipNoView++
      return
    }

    const info = ringInfo.value
    if (!info || !renderer) {
      skipNoRingInfo++
      return
    }

    const counters = readCounters(current)
    stats.value.produced = counters.produced

    // 没有新帧（同步器还没写入、或和上一帧同一个序号）就什么都不做，
    // 画面仍然留在 canvas 上。
    if (counters.syncVersion === 0n || counters.syncVersion === lastCounter) {
      skipNoNewFrame++
      return
    }

    const frame: FrameInfo | null = readPublishedFrame(current, info)
    if (!frame) {
      skipNoNewFrame++
      return
    }

    const t0 = performance.now()

    if (
      !pixelView ||
      pixelViewOffset !== frame.pixelOffset ||
      pixelView.byteLength !== frame.byteLength
    ) {
      pixelView = new Uint8Array(current.buffer, frame.pixelOffset, frame.byteLength)
      pixelViewOffset = frame.pixelOffset
    }

    const ok = renderer.upload(pixelView, {
      width: frame.width,
      height: frame.height,
      stride: frame.stride,
    })
    if (ok) renderer.draw()

    // sequence lock 校验：上传前后 version 必须一致。
    // texSubImage2D 是同步调用（会在返回前把像素取走），所以只要期间
    // 同步器没有写入新的槽，这一帧就是完整的。
    const after = readCounters(current).syncVersion
    if (after !== counters.syncVersion) {
      stats.value.skipped++
      return
    }
    lastCounter = counters.syncVersion
    const uploadMs = performance.now() - t0
    stats.value.uploadMs = uploadMs
    stats.value.rendered++
    stats.value.sourceWidth = frame.width
    stats.value.sourceHeight = frame.height

    // 记录绘制间隔，供 --selftest 核对真实帧节奏（期望间隔 ≈ 1000/60 ms）
    const drawnAt = performance.now()
    drawStamps.push(drawnAt)
    if (drawStamps.length > 120) drawStamps.shift()

    // ---- 帧率按路记账 ------------------------------------------------
    //
    // 只往**当前这一路**的账本里记。这样切到另一路时，那一份历史原封不动
    // 等着它，显示值当拍就是它自己的真值，不会串味。
    const key = ledgerKey()
    const l = ledgerFor(key)
    l.rendered++
    l.uploadMs = uploadMs
    // 窗口起点第一次用时才落，否则第一条会把"从页面打开到现在"的时间算进去
    if (l.windowStart === 0) l.windowStart = drawnAt
    l.windowFrames++
    if (drawnAt - l.windowStart >= 500) {
      l.lastRaw = (l.windowFrames * 1000) / (drawnAt - l.windowStart)
      l.windowStart = drawnAt
      l.windowFrames = 0
      // ---- 显示值：滑动中位数，见 kFpsHistorySize 的注释 ----
      l.history.push(l.lastRaw)
      if (l.history.length > kFpsHistorySize) l.history.shift()
      const sorted = [...l.history].sort((a, b) => a - b)
      const mid = sorted.length >> 1
      l.lastShown = sorted.length % 2 ? sorted[mid] : (sorted[mid - 1] + sorted[mid]) / 2
      // 只有当前正在看的那一路才往响应式 stats 上写；后台那一路只更新账本
      if (key === ledgerKey()) {
        stats.value.renderFps = l.lastRaw
        stats.value.renderFpsShown = l.lastShown
      }
      // 通知依赖 fpsOf() 的 computed 重新求值（见 ledgerRev 的注释）
      ledgerRev.value++
    }
  }

  function onEvent(payload: Record<string, unknown>) {
    switch (payload.type) {
      case 'status':
        stats.value.backendFps = Number(payload.fps ?? 0)
        stats.value.connected = Boolean(payload.connected)
        if (typeof payload.width === 'number') stats.value.sourceWidth = payload.width
        if (typeof payload.height === 'number') stats.value.sourceHeight = payload.height
        break
      case 'ready':
        ringInfo.value = payload.info as RingInfo
        break
      case 'waiting-service':
        waitingService.value = true
        serviceHint.value = String(payload.message ?? '')
        break
      case 'fatal':
        fatal.value = String(payload.message ?? '未知错误')
        break
      default:
        break
    }
  }

  async function start() {
    if (running.value) return
    if (!canvas.value) {
      fatal.value = 'canvas 还没挂载'
      return
    }

    if (!renderer) {
      try {
        renderer = new FrameRenderer(canvas.value)
      } catch (err) {
        fatal.value = `WebGL 初始化失败：${String(err)}`
        return
      }
    }

    // 事件监听先挂上，这样 preload 在等采集服务时我们也能收到通知
    if (api && !unlisten) {
      unlisten = api.onEvent(onEvent)
    }

    fatal.value = null

    // running 先置位：它表示"用户想预览"，而不是"已经连上"。
    // 连不上时下面的定时器会一直重试，所以 running 必须是 true 才会重试。
    running.value = true
    // 统计窗口的起点在 renderOnce 里"第一次真正画帧时"才落（见 FpsLedger），
    // 这里不预置 —— 预置成 start() 的时刻会把"等待服务"的那段空转算进帧率里。
    rafId = requestAnimationFrame(renderLoop)

    if (await connect()) return

    // 没连上：可能是采集服务还没起来。挂个定时器等它。
    scheduleRetry()
  }

  /** 采集服务此刻没在跑时的"再试一次" */
  async function retry() {
    fatal.value = null
    waitingService.value = false
    api?.retry()
    await new Promise((r) => setTimeout(r, 150))
    if (!running.value) await start()
    else if (await connect()) {
      // 已经连上，什么都不用做
    }
  }

  function stop() {
    running.value = false
    if (rafId) cancelAnimationFrame(rafId)
    rafId = 0
    stopRetryTimer()
    unlisten?.()
    unlisten = null
    view = null
    pixelView = null
    pixelViewOffset = -1
    renderer?.dispose()
    renderer = null
  }

  /**
   * 【诊断】读同步泵的节奏。
   *
   * preload 与页面在同一个 JS 世界里，所以直接调它暴露的诊断函数就行；
   * 它还没就绪时返回 null，别让诊断本身把渲染循环搞崩。
   *
   * 注意：这个对象很大（含全部会话信息 + 每块 buffer 的版本号快照）。**不要**
   * 把它塞进高频调用的 reportStats() —— 主进程用 executeJavaScript 读一次要
   * 跨进程序列化，实测要 2.5 秒，反过来会把渲染进程饿死，自检结果全部失真。
   * 只在需要深挖时单独调它。
   */
  function readPumpDiag(): unknown {
    try {
      const api = (window as unknown as { shadowPreview?: { diagnose?: () => unknown } })
        .shadowPreview
      return api?.diagnose ? api.diagnose() : null
    } catch {
      return null
    }
  }

  /** 【诊断】同步泵的标量指标（轻量，可以放进高频路径） */
  function readPumpScalars(): unknown {
    try {
      const api = (window as unknown as { shadowPreview?: { pumpStats?: () => unknown } })
        .shadowPreview
      return api?.pumpStats ? api.pumpStats() : null
    } catch {
      return null
    }
  }

  /**
   * 【诊断】页面侧自采样累积器。
   *
   * 自检不能每 500ms 去 executeJavaScript 读一次 —— 那一次往返要 2.5 秒，
   * 会把渲染进程掐停，量出来的帧率是"测量行为"造成的假象。
   * 改成让页面自己每 500ms 记一笔，测试结束时**只读一次**。
   */
  const history: Array<Record<string, unknown>> = []
  let historyTimer: ReturnType<typeof setInterval> | null = null

  function snapshot() {
    const counters = view ? readCounters(view) : null
    const pump = readPumpScalars() as {
      ticks?: number
      intervalMs?: number
      pollMs?: number
      busySkips?: number
    } | null
    // 连上了几路会话 —— 自检的多会话断言要用。
    // 只看"画了多少帧"无法区分"一路通了"和"三路全通"，而后者才是需求要的结果。
    // 会话连上了几路 —— 自检的多会话断言要用。
    // 只看"画了多少帧"无法区分"一路通了"和"三路全通"，而后者才是需求要的结果。
    //
    // 这里必须把**每一路的状态**都带出去，不能只数一个 ready 个数：
    // 只报"1 路 ready"时，另外两路是 unavailable（源没就绪，正常）还是 failed
    // （真的坏了）完全看不出来，排查只能靠猜。实测就吃过这个亏。
    let readySessions = 0
    let sessionStates: string[] = []
    try {
      const list = (
        window as unknown as {
          shadowPreview?: { sessions?: () => Array<{ session?: string; state?: string }> }
        }
      ).shadowPreview?.sessions?.()
      if (Array.isArray(list)) {
        readySessions = list.filter((s) => s.state === 'ready').length
        sessionStates = list.map((s) => `${s.session ?? '?'}:${s.state ?? '?'}`)
      }
    } catch {
      // 读不到就当 0，不影响渲染
    }
    return {
      t: Math.round(performance.now()),
      rendered: stats.value.rendered,
      renderFps: stats.value.renderFps,
      renderFpsShown: stats.value.renderFpsShown,
      rafTicks,
      skipNoNewFrame,
      // 【诊断】uploadMs 是 texSubImage2D（每帧 14.7MB）的耗时；skipped 是
      // sequence lock 前后版本变了被丢弃的帧数；skipBudget 是被 60fps 限流挡掉的轮次。
      // 这三者能把"上传太慢"和"压根没轮到画"分开。
      uploadMs: stats.value.uploadMs,
      skipped: stats.value.skipped,
      skipBudget,
      readySessions,
      sessionStates,
      // 帧间隔统计：自检靠它核对 60fps 限流真的生效了（中位数不该小于 8ms）。
      // 这里必须和 reportStats() 一样给出这两个字段，否则那条断言会空转。
      intervalMedianMs: medianDrawInterval(),
      targetIntervalMs: FRAME_BUDGET_MS,
      // 保持宽高比的证据（需求 3）。没有它，"预览没被拉伸"这条无法被验证。
      ...aspectEvidence(),
      pumpTicks: pump?.ticks ?? null,
      pumpIntervalMs: pump?.intervalMs ?? null,
      // 【诊断】拆开"泵慢"和"主线程被别的东西堵住"：
      // pollMs 是 addon.poll() 本身花了多久（每路一次 14.7MB memcpy），
      // busySkips 是泵自己重叠的次数。两者都小但 intervalMs 很大，就说明
      // 卡住主线程的另有其人 —— 而不是像素拷贝。
      pumpPollMs: pump?.pollMs ?? null,
      pumpBusySkips: pump?.busySkips ?? null,
      syncVersion: counters ? counters.syncVersion.toString() : null,
      produced: counters ? counters.produced.toString() : null,
    }
  }

  /**
   * 【自检】计算"保持宽高比"的证据。
   *
   * 只看渲染帧数无法证明画面没被拉伸，所以直接把画布尺寸、源尺寸、按比例
   * 拟合后的画面尺寸和两者的宽高比都报出来，自检就能核对 source 的宽高比
   * 与 letterbox 后的宽高比是否一致（差值应该在浮点误差内）。
   */
  function aspectEvidence(): Record<string, unknown> {
    const el = canvas.value
    if (!el || !renderer) return { aspect: null }
    const cssW = el.clientWidth || 0
    const cssH = el.clientHeight || 0
    const box = renderer.letterboxRect(cssW, cssH)
    const sw = renderer.sourceWidth
    const sh = renderer.sourceHeight
    return {
      canvasWidth: cssW,
      canvasHeight: cssH,
      fittedWidth: Math.round(box.width),
      fittedHeight: Math.round(box.height),
      sourceAspect: sh > 0 ? sw / sh : null,
      fittedAspect: box.height > 0 ? box.width / box.height : null,
    }
  }

  /** 已绘制帧的间隔中位数（毫秒）；样本不足时返回 0 */
  function medianDrawInterval(): number {
    if (drawStamps.length < 2) return 0
    const intervals: number[] = []
    for (let i = 1; i < drawStamps.length; i++) {
      intervals.push(drawStamps[i] - drawStamps[i - 1])
    }
    intervals.sort((a, b) => a - b)
    return intervals[Math.floor(intervals.length / 2)]
  }

  /**
   * 某一路的帧率（显示值）。
   *
   * 顶栏按"当前选中哪一路"来取，取到的是那一路自己攒下的数（见 FpsLedger），
   * 所以切换配置时数字当拍就跳过去，不会先显示上一路的残影再慢慢过渡。
   * `id` 传 null 取兜底账本（单路模式）。
   *
   * 读 `ledgerRev` 是为了让外层的 computed 能追踪到账本变化 —— Map 里那些
   * 普通对象本身不是响应式的。
   */
  function fpsOf(id: string | null): number {
    void ledgerRev.value
    return ledgerFor(id ?? '').lastShown
  }

  /** 某一路上一次的原始（未平滑）帧率，诊断/自检用 */
  function rawFpsOf(id: string | null): number {
    void ledgerRev.value
    return ledgerFor(id ?? '').lastRaw
  }

  /**
   * 开始页面侧自采样（自检用；多次调用只生效一次）。
   *
   * `extra` 用来把"界面侧的接线状态"（配置表单有没有填上、导出按钮可不可用）
   * 一并带进这笔推送里 —— 主进程每 500ms 就白拿一次，不用再跨进程去读
   * （读一次要 2.5 秒，会把渲染进程掐停，正是之前量出假帧率的元凶）。
   */
  function setupHistory(extra?: () => Record<string, unknown>) {
    if (historyTimer) return
    const withExtra = (snap: Record<string, unknown>) => {
      try {
        const more = extra?.()
        if (more) Object.assign(snap, more)
        else snap.extraState = 'empty'
      } catch (err) {
        // 界面侧取数失败不能把采样循环带崩 —— 但必须留下痕迹，
        // 否则现象是"字段一片空白"，查不出是取数炸了还是本来就没值。
        snap.extraState = `throw: ${String(err)}`
      }
      return snap
    }
    history.push(withExtra(snapshot()))
    let expect = 500
    historyTimer = setInterval(() => {
      // 【诊断】记下"这次本该在什么时刻跑"，外部就能直接读出主线程被堵了多久。
      // 只看相邻两条 t 的差会被采样周期本身干扰，用预期时刻最干净。
      expect += 500
      const drift = Math.round(performance.now() - expect)
      const snap = withExtra(snapshot())
      snap.driftMs = drift
      // 别让累积器无限长：自检最多几十秒，60 笔足够看出趋势
      if (history.length > 600) history.shift()
      history.push(snap)
      // 顺手推给主进程。主进程本来是用 executeJavaScript 来"拉"的，但拉一次
      // 要 2.5 秒、会把渲染进程掐停，量出来的帧率是假的（46fps，而帧间隔中位数
      // 却是达标的 16.8ms）。改成推送之后量到的才是真实帧率。
      try {
        ;(window as unknown as {
          shadowPreview?: { reportStats?: (s: Record<string, unknown>) => void }
        }).shadowPreview?.reportStats?.(snap)
      } catch {
        // 上报失败不能影响渲染
      }
    }, 500)
    historyTimer.unref?.()
  }

  function getHistory() {
    return history
  }

  /**
   * 自检用：导出当前渲染状态快照（主进程 --selftest 会读它）。
   *
   * 这里刻意保持**小**（不碰 readPumpDiag），因为它会被主进程反复读取。
   */
  function reportStats() {
    const intervals: number[] = []
    for (let i = 1; i < drawStamps.length; i++) {
      intervals.push(drawStamps[i] - drawStamps[i - 1])
    }
    const sorted = [...intervals].sort((a, b) => a - b)
    const median = sorted.length ? sorted[Math.floor(sorted.length / 2)] : 0
    const mean = intervals.length
      ? intervals.reduce((a, b) => a + b, 0) / intervals.length
      : 0
    // 原始帧环计数：用来区分"生产端没发新帧"和"渲染端没看到新帧"
    const counters = view ? readCounters(view) : null
    return {
      rendered: stats.value.rendered,
      renderFps: stats.value.renderFps,
      renderFpsShown: stats.value.renderFpsShown,
      backendFps: stats.value.backendFps,
      uploadMs: stats.value.uploadMs,
      produced: stats.value.produced.toString(),
      skipped: stats.value.skipped,
      width: stats.value.sourceWidth,
      height: stats.value.sourceHeight,
      // 【自检】保持宽高比的证据：画布内实际画面尺寸 + 它的宽高比。
      // 需求 (3) 要求"保持纵横比的预览"，光看"画了多少帧"证明不了这一条。
      ...aspectEvidence(),
      transport: transport.value,
      connected: stats.value.connected,
      waitingService: waitingService.value,
      fatal: fatal.value,
      syncVersion: counters ? counters.syncVersion.toString() : null,
      ringProduced: counters ? counters.produced.toString() : null,
      renderedCounter: lastCounter.toString(),
      intervalMedianMs: median,
      intervalMeanMs: mean,
      intervalSamples: intervals.length,
      targetIntervalMs: FRAME_BUDGET_MS,
      loopError,
      loopErrorCount,
      // 【诊断】每条跳过路径各自记账，用来区分"循环没跑"和"每轮都被跳过"
      rafTicks,
      skipNoView,
      skipNoRingInfo,
      skipNoRenderer,
      skipBudget,
      skipNoNewFrame,
      // 【诊断】页面可见性。rAF 被 Chromium 掐停时它必然不是 'visible'。
      visibility: typeof document !== 'undefined' ? document.visibilityState : null,
      // 【诊断】同步泵的节奏（只取标量，千万别在这里调 diagnose()）。
      pump: readPumpScalars(),
    }
  }

  // ---- 连接失败/服务未起时的自动重连 -------------------------------------
  //
  // 界面一打开就自动开始预览，但采集服务不一定是先起的（也可能正在重启）。
  // 所以连不上时挂一个定时器一直试，服务一起来就自动接上，不需要用户点任何东西。
  let retryTimer: ReturnType<typeof setTimeout> | null = null
  let retryBusy = false

  function stopRetryTimer() {
    if (retryTimer !== null) {
      clearTimeout(retryTimer)
      retryTimer = null
    }
  }

  function scheduleRetry() {
    if (retryTimer !== null || !running.value) return
    retryTimer = setTimeout(async () => {
      retryTimer = null
      if (!running.value || retryBusy) return
      retryBusy = true
      try {
        if (await connect()) {
          waitingService.value = false
          fatal.value = null
        } else {
          scheduleRetry()
        }
      } finally {
        retryBusy = false
      }
    }, 1000)
  }

  return {
    canvas,
    running,
    fatal,
    notices,
    ringInfo,
    transport,
    waitingService,
    serviceHint,
    stats,
    start,
    stop,
    retry,
    switchSession,
    fpsOf,
    rawFpsOf,
    seedLedgerFromService,
    ledgerRev,
    reportStats,
    setupHistory,
    getHistory,
  }
}
