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
  matchedTitle: string
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
}

export interface ExportEvent {
  type: string
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
 * 正在导出时的进度（null = 没有导出在跑）。
 *
 * 界面**不**再靠服务会话里的 exportRunning 判断了：导出现在跑在界面进程自己
 * 里（见 app/main.cjs 的 service:export），服务根本不知道这件事。进度由主进程
 * 每 400ms 广播一条 export_status 送过来。
 */
const exportBusy = ref<{ phase: string; outPath: string } | null>(null)

/** 服务进程退出过（预览会跟着断，界面要提示重启） */
const serviceExited = ref<number | null>(null)

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
      // 导出现在有两路事件：
      //   * export_result —— 服务自己跑的那条命令通道（仍然保留兼容）
      //   * export_status —— 界面进程里插件跑导出时，主进程每 400ms 广播一次
      // 只有"收尾了"（running=false 且 ok/failed 有一个为真）才算完成通知，
      // 否则会弹一堆"导出中"的 toast。
      const isResult = evt.type === 'export_result' || evt.type === 'export_status'
      if (isResult && !evt.running && (evt.ok || evt.failed)) {
        lastExport.value = evt
      }
      if (isResult && evt.running) {
        exportBusy.value = { phase: evt.phase ?? '', outPath: evt.outPath ?? '' }
      } else if (isResult) {
        exportBusy.value = null
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
    exportBusy: readonly(exportBusy),
    serviceExited: readonly(serviceExited),
    refresh: pull,
  }
}
