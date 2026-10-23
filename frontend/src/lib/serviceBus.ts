// frontend/src/lib/serviceBus.ts
//
// 采集服务的**实时**状态（录制进度、导出进度、每路实际帧率）。
//
// 状态有两条通道，因为服务的 stdout 只有"启动它的进程"读得到：
//   * 本界面起的服务 -> 主进程读它的 stdout，实时且带一次性事件；
//   * 别人起的服务（cmd / npm run service） -> 服务每秒写一份状态文件，
//     主进程读那个文件。**这条是"界面显示服务没启动"那个 bug 的修法。**
// 主进程把两条通道合成一份快照（service:live），页面只看快照，不关心来源。
//
// 为什么不直接放进 usePreview：预览关心的是"拿到帧没有"，而这里是"录到哪了"。
// 两者生命周期不同 —— 预览停了录制照样在跑，混在一起会让状态互相影响。

import { readonly, ref, shallowRef } from 'vue'

/** 服务一条 session 状态里的录制部分（字段名对齐 native/backend/Session.cpp）。 */
export interface SessionLive {
  type: 'session'
  id: string
  name: string
  state: string
  detail: string
  enabled: boolean
  attached: boolean
  /**
   * 窗口源**此刻挂在哪个句柄上**（0 = 桌面源 / 还没匹到）。
   *
   * 界面拿它来回答"这一路到底在录哪个窗口"：标题会变（浏览器换个页面就变），
   * 句柄在同一次运行里是稳定的，所以句柄比标题可靠得多。
   */
  hwnd?: number
  matchedTitle: string
  /**
   * 捕获的那个窗口**关掉了**，这一路正在等它回来（应用还活着）。
   *
   * 有它才能说人话：「在等《xxx》这个窗口回来」。没有它的话"没画面"只能
   * 含糊成"匹配失败"，而这两件事用户该做的事完全不同 —— 前者是去把窗口
   * 重新打开，后者是改配置。
   */
  windowMissing?: boolean
  width: number
  height: number
  targetFps: number
  actualFps: number
  gapMs: number
  captureMs: number
  produced: number
  dropped: number
  consumerPid: number

  /** 采集方式：none / desktop / wgc（见 native/backend/Session.h 的 CaptureKind）。
   *  窗口源只走 wgc；没有"桌面裁剪退化"这一档了。 */
  captureKind: string

  recording: boolean
  recordFailed: boolean
  recordDir: string
  recordDetail: string
  recordSegments: number
  recordBytes: number
  recordFrames: number

  /**
   * 声音（只进导出成片，预览不发声）。
   *
   * audioPeak / audioRms 是**线性** 0..1（服务端直接给的峰值/有效值），界面
   * 自己转 dB 画条。audioActive = 这一路现在真的在抓声音；audioDetail 说明
   * 抓的是系统混音还是某个窗口的进程环回。
   *
   * audioError 非空 = 这一路采不到声音的原因（目前只有窗口捕获会报：窗口音频
   * 只抓那个进程，抓不到就直接失败，不再退化成"所有窗口混在一起"的系统混音）。
   * 界面拿它在预览画面上盖一条红条，空串表示没问题。
   */
  audioActive?: boolean
  audioPeak?: number
  audioRms?: number
  audioDetail?: string
  audioError?: string
  audioChunks?: number
  audioBytes?: number

  /**
   * 编码器**实际**在用的帧率 / 码率（和配置值分开）。
   *
   * 【为什么要分两套】硬件编码器有「分辨率 × 帧率」的吞吐上限（2560x1440 只到
   * 172fps），配 240 会被自动降到 165；码率以前还会被像素公式覆盖掉用户配的值。
   * 界面只显示配置值的话，用户会以为「我配的 240 帧正在录」（m13697 / m14071）。
   *
   * 老版本服务不报这四个字段，所以都可能是 undefined —— 显示层要判空。
   */
  requestedFps?: number
  effectiveFps?: number
  requestedBitrateMbps?: number
  effectiveBitrateMbps?: number

  /**
   * 「立刻切片」的完成计数（服务端 Session::FlushRecording 成功一次 +1）。
   *
   * 点导出前必须让服务把正在写的那一片收尾落盘，否则含"点按钮那一刻"的那片
   * 导不出来（m12714）。命令管道是单向的，这个数就是那条回执。
   */
  flushSeq?: number

  exportRunning: boolean
  exportOk: boolean
  exportFailed: boolean
  exportPhase: string
  exportOutPath: string
  exportDetail: string
  exportSegments: number
  exportDurationSeconds: number
  exportBytes: number
  exportElapsedMs: number
  exportJobId: number
}

export interface ServiceLiveSnapshot {
  sessions: SessionLive[]
  /** 服务不是本界面启动的时候为 false（但仍然可能通过状态文件看得见状态） */
  owned: boolean
  updatedAt: number
  profileCount: number
  /** 服务到底在不在运行：本界面起的，或者状态文件还新鲜。 */
  running: boolean
  /** 这份状态从哪儿来的：'stdout'（本界面起的）/ 'file'（状态文件）/ 'none' */
  source: 'stdout' | 'file' | 'none'
  /** 状态文件路径（界面在"看不到服务"时把它显示出来，方便排查） */
  mirrorFile: string
  /** 状态文件多久没更新了（毫秒），null 表示没有这个文件 */
  mirrorAgeMs: number | null
  /** 界面这份配置的路径 */
  wantedProfilesPath?: string | null
  /** 正在跑的那个服务实际加载的配置路径（它没报就是 null） */
  runningProfilesPath?: string | null
  /** 两者不是同一份：界面上显示的分片/录制状态属于**另一个**服务的目录 */
  profilesMismatch?: boolean
  /** 守护进程（服务崩了由它拉起来，见 app/guardian.cjs） */
  guardian?: GuardianSnapshot
}

export interface GuardianSnapshot {
  /** 心跳新鲜且它记的 pid 还活着 */
  alive: boolean
  pid: number | null
  /** 守护已经把服务重新拉起来过几次 */
  restarts: number
  /** 最近一次拉起失败的原因（成功过就是空串） */
  lastError: string
  lastRestartAt: string | null
}

export interface ExportEvent {
  type: string
  /** 这条事件属于哪一路配置（导出队列按 id 分开记状态就靠它） */
  id?: string
  name?: string
  running?: boolean
  ok?: boolean
  failed?: boolean
  phase?: string
  outPath?: string
  detail?: string
  segments?: number
  segmentCount?: number
  durationSeconds?: number
  bytes?: number
  elapsedMs?: number
  jobId?: number
  code?: number
  /** export_queue 事件专用：整份队列快照 */
  items?: ExportQueueItem[]
  queuedCount?: number
}

/** 队列里的一条（主进程 exportQueueSnapshot 的形状） */
export interface ExportQueueItem {
  id: string
  name?: string
  state: 'queued' | 'running' | string
  phase?: string
}

/** 某一路的导出状态（键是配置 id） */
export interface ExportJobState {
  id: string
  name: string
  state: 'queued' | 'running'
  phase: string
}

const api = () => (window as unknown as { shadowPreview?: Record<string, unknown> }).shadowPreview

const live = shallowRef<ServiceLiveSnapshot>({
  sessions: [],
  owned: false,
  updatedAt: 0,
  profileCount: 0,
  running: false,
  source: 'none',
  mirrorFile: '',
  mirrorAgeMs: null,
})

/** 最近一条导出结果提示（界面顶部弹一下） */
const lastExport = ref<ExportEvent | null>(null)

/**
 * 每一路自己的导出状态（键是配置 id，没有键 = 这一路空闲）。
 *
 * 【为什么必须按 id 存】以前是一个全局的 exportBusy：A 路在导的时候，B 路的
 * 导出按钮也跟着灰掉 —— 用户切到 B 路想导出，看到按钮是灰的，以为坏了。
 * 导出现在是队列串行的，但"排队"这件事对用户是**不可见**的：谁的活在跑、
 * 谁还排在后面，按钮一律显示"导出中…"（见 App.vue 的 exportButtonText）。
 * 队列只影响按钮的禁用 —— 已经点了的那一路不能重复入队。
 */
const exportStates = ref<Record<string, ExportJobState>>({})

/** 服务进程退出过（预览会跟着断，界面要提示重启） */
const serviceExited = ref<number | null>(null)

/**
 * 实时电平（配置 id -> 线性 0..1）。
 *
 * 【为什么不放进 live】live 是每秒一份的整份快照，而这个是每秒 20~30 次的
 * 小更新。混在一起会让每个 tick 都去 diff 整个 sessions 数组（几十 KB），
 * 音量条还没动，主线程先卡了。所以单独一个浅层对象：一次只换掉 map，
 * 用到它的 computed 才重算。
 */
const meterLevels = shallowRef<Record<string, number>>({})

let meterUnlisten: (() => void) | null = null

/** 把主进程推来的一份电平表铺进 meterLevels（已按 id 摊平）。 */
function applyMeter(doc: { items?: { id?: unknown; lv?: unknown }[] } | null) {
  const items = Array.isArray(doc?.items) ? doc.items : []
  const next: Record<string, number> = {}
  for (const it of items) {
    const id = typeof it?.id === 'string' ? it.id : ''
    const lv = Number(it?.lv)
    if (!id || !Number.isFinite(lv)) continue
    next[id] = Math.min(1, Math.max(0, lv))
  }
  meterLevels.value = next
}

let timer: ReturnType<typeof setInterval> | null = null
let unlisten: (() => void) | null = null
let inflight = false

async function pull() {
  const p = api()
  if (!p || typeof p.serviceLive !== 'function' || inflight) return
  inflight = true
  try {
    const res = (await (p.serviceLive as () => Promise<Record<string, unknown>>)()) as {
      sessions?: SessionLive[]
      owned?: boolean
      updatedAt?: number
      running?: boolean
      source?: string
      mirrorFile?: string
      mirrorAgeMs?: number | null
      wantedProfilesPath?: string | null
      runningProfilesPath?: string | null
      profilesMismatch?: boolean
      guardian?: {
        alive?: boolean
        pid?: number | null
        restarts?: number
        lastError?: string
        lastRestartAt?: string | null
      }
    }
    const sessions = Array.isArray(res?.sessions) ? res.sessions : []
    live.value = {
      sessions,
      owned: res?.owned === true,
      updatedAt: Number(res?.updatedAt ?? 0),
      profileCount: live.value.profileCount,
      // running 的主进程没给（旧版本）就退化成"有会话算在跑"，
      // 免得界面因为一个缺失字段就显示服务没启动。
      running: res?.running === true || (res?.running === undefined && sessions.length > 0),
      source: res?.source === 'stdout' || res?.source === 'file' ? res.source : 'none',
      mirrorFile: String(res?.mirrorFile ?? ''),
      mirrorAgeMs: typeof res?.mirrorAgeMs === 'number' ? res.mirrorAgeMs : null,
      wantedProfilesPath: res?.wantedProfilesPath ?? null,
      runningProfilesPath: res?.runningProfilesPath ?? null,
      profilesMismatch: res?.profilesMismatch === true,
      guardian: {
        alive: res?.guardian?.alive === true,
        pid: res?.guardian?.pid ?? null,
        restarts: Number(res?.guardian?.restarts ?? 0),
        lastError: String(res?.guardian?.lastError ?? ''),
        lastRestartAt: res?.guardian?.lastRestartAt ?? null,
      },
    }
  } catch {
    // 主进程还没准备好 / 窗口正在关：保持上一份快照，别把界面抖成空的
  } finally {
    inflight = false
  }
}

/**
 * 开始跟踪服务状态。
 *
 * 轮询是主力（1 秒一次）：无论服务是谁起的，主进程都能从"stdout 或状态文件"
 * 合成一份快照。事件只是"立刻拉一次"的加速器，不是唯一来源 —— 服务不是本界面
 * 启动时根本没有 stdout 事件，只有主进程每秒读状态文件产生的事件。
 */
export function startServiceBus(pollMs = 1000) {
  if (timer !== null) return

  const p = api()
  if (p && typeof p.onServiceEvent === 'function') {
    unlisten = (p.onServiceEvent as (cb: (e: ExportEvent) => void) => () => void)((evt) => {
      if (!evt) return

      // ---- 导出队列 ----
      //
      // 事件有两类：
      //   * export_queue  —— 队列整份快照（入队 / 出队 / 完成时各播一次）；
      //   * export_status —— 某一路的进度（带 id），以及它的收尾结果。
      // 状态按配置 id 分开存，界面据此决定"哪一路的导出按钮该灰"。
      if (evt.type === 'export_queue' && Array.isArray(evt.items)) {
        const next: Record<string, ExportJobState> = {}
        for (const it of evt.items) {
          if (!it?.id) continue
          next[it.id] = {
            id: it.id,
            name: String(it.name ?? ''),
            state: it.state === 'running' ? 'running' : 'queued',
            phase: String(it.phase ?? ''),
          }
        }
        exportStates.value = next
      }

      const isResult = evt.type === 'export_result' || evt.type === 'export_status'
      if (isResult && evt.id) {
        const id = evt.id
        if (evt.running) {
          exportStates.value = {
            ...exportStates.value,
            [id]: {
              id,
              name: String(evt.name ?? ''),
              state: 'running',
              phase: String(evt.phase ?? ''),
            },
          }
        } else {
          const next = { ...exportStates.value }
          delete next[id]
          exportStates.value = next
        }
      }
      // 只有"收尾了"（running=false 且 ok/failed 有一个为真）才算完成通知，
      // 否则会弹一堆"导出中"的 toast。
      if (isResult && !evt.running && (evt.ok || evt.failed)) {
        lastExport.value = evt
      }

      if (evt.type === 'service_exit') {
        serviceExited.value = Number(evt.code ?? -1)
        live.value = { ...live.value, sessions: [], updatedAt: Date.now() }
      } else if (evt.type === 'service_started') {
        serviceExited.value = null
      }
      // 事件本身就说明状态变了，立刻拉一次比等下一个轮询周期更跟手
      void pull()
    })
  }

  void pull()
  timer = setInterval(() => void pull(), Math.max(250, pollMs))

  // 实时电平：单独一条通道（见 onMeter 的说明），不触发 pull，也不进 live。
  if (p && typeof p.onMeter === 'function') {
    meterUnlisten = (p.onMeter as (cb: (d: unknown) => void) => () => void)((doc) => {
      applyMeter(doc as { items?: { id?: unknown; lv?: unknown }[] })
    })
  }
}

export function stopServiceBus() {
  if (timer !== null) {
    clearInterval(timer)
    timer = null
  }
  if (unlisten) {
    unlisten()
    unlisten = null
  }
  if (meterUnlisten) {
    meterUnlisten()
    meterUnlisten = null
  }
  meterLevels.value = {}
}

/** 界面上的"配置列表"变了以后调一下，让 profileCount 立刻跟上 */
export function noteProfileCount(n: number) {
  live.value = { ...live.value, profileCount: n }
}

export function clearExportNotice() {
  lastExport.value = null
}

export function useServiceBus() {
  return {
    live: readonly(live),
    lastExport: readonly(lastExport),
    exportStates: readonly(exportStates),
    serviceExited: readonly(serviceExited),
    meterLevels,
    refresh: pull,
  }
}
