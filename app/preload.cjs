// app/preload.cjs — 隔离世界里的桥（多会话版）
//
// 这个文件是整条链路的关键，做四件事：
//   1. 找到并"连上"已经在运行的 C++ 采集服务；
//   2. 问服务要配置列表，为**每一条启用的配置**各自映射一个帧环（零拷贝）；
//   3. 定期把每个环里最新一帧同步进 V8 缓冲区；
//   4. 把每块内存交给主世界（Vue + WebGL）去渲染。
//
// 架构要点：采集服务是独立常驻进程，界面不负责拉起它。
//   好处：关掉/重开界面不影响采集；界面崩溃不会留下孤儿进程；
//   采集端以默认权限运行，桌面复制和 GPU 访问都比从沙箱里拉起来更稳。
//
// 多会话为什么可行：addon 的每个导出都收**会话名**作第一个参数，
//   open/start/poll/stats 全是按会话名查表的，所以一个渲染进程可以同时
//   持有 N 个环 —— 前提是 JS 这边别像以前那样用一个全局变量存状态。
//
// 为什么每路是两个 ArrayBuffer：
//   preload（隔离世界）要读帧头才知道"哪一帧、多大、哪个槽"，
//   主世界要读像素才能上传纹理。如果只创建一个 ArrayBuffer 再 transfer，
//   preload 手里那个会立刻 detached，帧头就读不到了。
//   所以用 addon.newBufferHandle() 再要一个独立句柄给主世界 ——
//   两者指向同一块共享内存，互不影响，全程没有任何像素拷贝。

const { ipcRenderer } = require('electron')
const path = require('node:path')
const { SERVICE_SESSION_ID } = require('./service-const.cjs')

const ADDON_PATH = path.join(__dirname, '..', 'native', 'build', 'shadow_ring.node')

let addon = null
let addonError = null
try {
  addon = require(ADDON_PATH)
} catch (err) {
  addonError = err
}

// 必须与 native/ShadowRing.h 的 ShadowRingHeader 一致。
// 权威来源：npm run offsets（打印 offsetof 结果）。
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
  U64: {
    /** C++ 侧的发布序号 */
    counter: 32,
    /** 同步进 V8 缓冲区后的版本号（sequence lock），写的过程中为 0 */
    syncVersion: 192,
    producedFrames: 88,
    droppedFrames: 96,
    skippedFrames: 104,
  },
  OFFSETS: {
    slotOwners: 112,
    slotOwnQpc: 144,
    slotStride: 176,
    modeChangeCount: 184,
    captureErrors: 188,
    syncVersion: 192,
    consumerHeartbeatQpc: 72,
  },
  // sizeof(ShadowRingHeader)；权威来源：npm run offsets
  HEADER_SIZE: 432,
}

// ---------------------------------------------------------------------------
// 会话表
//
// 键是会话名（== 配置 id）。每条记录自己带着 buffer/view/ringInfo，
// 这样"哪一路出问题"能具体到某一条配置，而不是整个界面一起黑掉。
// ---------------------------------------------------------------------------
const conns = new Map()

let fatal = null
const eventListeners = new Set()
let retryTimer = null
let scanTimer = null
let scanning = false
let lastProfilesResponse = null

function emit(payload) {
  for (const fn of eventListeners) {
    try {
      fn(payload)
    } catch (err) {
      console.error('[preload] 事件回调抛错：', err)
    }
  }
}

/**
 * 整体状态。任何一路连上了就算 ready —— 前端按会话各自显示，
 * 某一路失败不该让别的几路也不能看。
 */
function overallState() {
  if (fatal) return 'failed'
  if (conns.size === 0) return 'waiting-service'
  return 'ready'
}

function snapshot(conn) {
  return {
    session: conn.session,
    state: conn.state,
    error: conn.error,
    ringInfo: conn.ringInfo,
    info: conn.profile,
  }
}

// ---------------------------------------------------------------------------
// 帧读取
// ---------------------------------------------------------------------------
function readFrameInfo(conn) {
  const { view, ringInfo } = conn
  if (!view || !ringInfo) return null
  const slot = view.getUint32(RING.U32.publishedSlot, true)
  if (slot >= ringInfo.slotCount) return null

  const base = ringInfo.headerSize + ringInfo.slotStride * slot
  const width = view.getUint32(base + RING.U32.width, true)
  const height = view.getUint32(base + RING.U32.height, true)
  const stride = view.getUint32(base + RING.U32.stride, true)
  if (!width || !height || !stride) return null
  if (stride !== width * 4) return null // 几何字段异常，当成没有帧

  return {
    syncVersion: view.getBigUint64(RING.U64.syncVersion, true).toString(),
    slot,
    width,
    height,
    stride,
    pixelOffset: base + ringInfo.headerSize,
    byteLength: stride * height,
  }
}

// ---------------------------------------------------------------------------
// 连接一路会话
// ---------------------------------------------------------------------------
function fail(stage, message) {
  fatal = { stage, message }
  emit({ type: 'fatal', ...fatal })
}

/**
 * 把一条配置接到它的帧环上。
 *
 * 失败**不抛异常**：窗口源匹配不到时服务会把这条配置标成 disabled，
 * 那种情况下环根本没建，这里安静地跳过就行（需求 4：匹配不到就关掉它）。
 */
function attachSession(profile) {
  const id = profile.id
  if (!addon || !id) return null
  if (conns.has(id) && conns.get(id).state === 'ready') return conns.get(id)

  const conn = conns.get(id) ?? {
    session: id,
    state: 'idle',
    error: null,
    ringInfo: null,
    buffer: null,
    view: null,
    profile: null,
    attachedAt: 0,
  }
  conn.profile = profile
  conns.set(id, conn)

  // 判据只看 enabled —— 这是**配置**文件，没有运行时状态。
  //
  // 曾经想在这里看 profile.attached（"服务有没有给它建环"）来**决定连不连**，
  // 但 attached 是服务的运行时字段，磁盘上的配置里根本没有，于是每一路都被
  // 判成"没建环"直接跳过，界面永远只有 0 路。所以这里是"enabled 就试着连"。
  //
  // 但 attached / detail 仍然有用 —— 只用来**决定等多久**（见下面 addon.open
  // 的注释）：服务说"匹配不到窗口"，那等多久都白等。区分清楚这两件事：
  // "要不要试"看 enabled，"要等多久"看服务给的运行时字段。
  if (profile.enabled === false) {
    conn.state = 'off'
    conn.error = '未启用'
    return conn
  }

  try {
    // 等待时长：**一律 0**。
    //
    // addon.open 是同步 N-API，它的 timeout 是在渲染进程主线程上用
    // `Sleep(30)` 空转实现的 —— 每多等 1ms 就是实打实少画 1ms 的帧。
    //
    // 曾经传过 3000（源不就绪时界面 6 秒只画 4 帧）和 250（一轮扫描堵
    // 250ms，而扫描是每 2 秒一次的 → 预览"播两秒定住一下"），根子都是
    // 同一个：**同步等待主线程**。
    //
    // 传 0 时 Ring::Open 里 `timeoutMs <= 0` 直接 break，只试一次，几乎不
    // 花时间。代价是"服务正在建环"的那一瞬间可能扑空 —— 但扫描本来就是
    // 每 2 秒一轮，最迟 2 秒后自然连上，而用户换回来的是**主线程一秒都不
    // 再被阻塞**。这笔账怎么算都划算。
    const waitMs = 0
    const ring = addon.open(id, waitMs)
    const handle = addon.start(id)
    conn.buffer = handle.buffer
    conn.view = new DataView(conn.buffer)

    // 头部自检：布局和 JS 侧对不上时宁可报清楚，也不要后面画出一屏花屏
    const magic = conn.view.getUint32(RING.U32.magic, true)
    const version = conn.view.getUint32(RING.U32.version, true)
    const jsHeaderSize = conn.view.getUint32(RING.U32.headerSize, true)
    if (magic !== RING.MAGIC || version !== RING.VERSION || jsHeaderSize !== RING.HEADER_SIZE) {
      conn.state = 'failed'
      conn.error =
        `帧环头部校验失败 magic=0x${magic.toString(16)} version=${version} ` +
        `headerSize=${jsHeaderSize}（期望 0x${RING.MAGIC.toString(16)}/${RING.VERSION}/${RING.HEADER_SIZE}）` +
        ` —— JS 与 C++ 的 ShadowRingHeader 布局不一致，改过 ShadowRing.h 请跑 npm run offsets`
      emit({ type: 'session-error', session: id, message: conn.error })
      return conn
    }

    conn.ringInfo = {
      session: id,
      byteLength: handle.byteLength,
      headerSize: handle.headerSize,
      slotCount: handle.slotCount,
      slotStride: handle.slotStride,
      sourceWidth: handle.sourceWidth,
      sourceHeight: handle.sourceHeight,
      producerPid: ring.producerPid,
      consumerPid: conn.view.getUint32(RING.U32.consumerPid, true),
    }
    conn.state = 'ready'
    conn.error = null
    conn.attachedAt = Date.now()

    console.log(
      `[preload] 会话 ${id} 已连接：${conn.ringInfo.sourceWidth}x${conn.ringInfo.sourceHeight} ` +
        `${conn.ringInfo.slotCount} 槽 stride=${conn.ringInfo.slotStride} ` +
        `producer=${conn.ringInfo.producerPid}`,
    )
    emit({ type: 'session-ready', session: id, info: snapshot(conn) })
    return conn
  } catch (err) {
    // 区分两种"连不上"：
    //   * 环不存在 → 这一路的源还没就绪（窗口没匹配到、服务还没起来），
    //     是**正常等待**，不该让界面报红；
    //   * 其它 → 真的出问题了（布局不一致、addon 坏了），要显眼。
    const msg = String(err?.message ?? err)
    const noRing = /not.?found|不存在|无法打开|帧环|open.*fail|timeout|超时/i.test(msg)
    conn.state = noRing ? 'unavailable' : 'failed'
    conn.error = noRing ? `等待源就绪（${msg}）` : msg
    // 连不上时把原因打出来。以前这里静默返回，界面上只显示"等待源就绪"，
    // 而 addon 报的**具体**原因（环不存在？被别的消费者占了？超时？）全被吞掉，
    // 排查只能靠猜。日志不花钱，留着。
    if (conn.state === 'unavailable') {
      console.warn(`[preload] 会话 ${id} 暂不可用：${msg}`)
    } else {
      console.warn(`[preload] 会话 ${id} 连接失败：${msg}`)
    }
    return conn
  }
}

function detachSession(id) {
  const conn = conns.get(id)
  if (!conn) return
  try {
    addon?.close?.(id)
  } catch {
    /* 关不掉就算了，服务端有自己的活性检测 */
  }
  conns.delete(id)
  emit({ type: 'session-gone', session: id })
}

// ---------------------------------------------------------------------------
// 问服务要配置列表
//
// 走 stdio 的 NDJSON：服务收到 {"type":"list_sessions"} 会回一条 sessions
// 快照。这样前端不用自己解析命令行，也不用猜有哪几路。
// ---------------------------------------------------------------------------
const SERVICE_READY_WAIT_MS = 0

async function fetchProfiles() {
  const res = await ipcRenderer.invoke('service:profiles')
  lastProfilesResponse = res
  return Array.isArray(res?.items) ? res.items : []
}

async function scanSessions() {
  if (scanning || !addon) return
  scanning = true
  try {
    const profiles = await fetchProfiles()
    if (!profiles.length) {
      // 服务还没起来：界面只显示"等待采集服务"的遮罩（界面会自动拉服务），
      // 不再让人去终端敲 npm run service —— 现在不需要了。
      emit({ type: 'waiting-service', message: '采集服务还没就绪，正在等待…' })
      return
    }

    // 服务里没有的会话：界面开过、服务重启过，这里清掉
    const live = new Set(profiles.map((p) => p.id))
    for (const id of [...conns.keys()]) {
      if (!live.has(id)) detachSession(id)
    }

    let changed = false
    for (const p of profiles) {
      const before = conns.get(p.id)?.state
      const conn = attachSession(p)
      if (conn && conn.state !== before) changed = true
    }

    if (changed || conns.size !== profiles.length) {
      emit({ type: 'sessions-changed', items: listSessions() })
    }
  } catch (err) {
    console.warn('[preload] 枚举会话失败：', err.message)
  } finally {
    scanning = false
  }
}

function listSessions() {
  return [...conns.values()].map(snapshot)
}

// ---------------------------------------------------------------------------
// 帧同步泵
//
// 服务端写的是共享内存，我们手里这块是 V8 内存，所以需要定期把最新一帧
// 拷过来（addon.poll 内部只拷被发布的那一个槽）。
//
// 关键：contextIsolation 是关掉的，所以这些 ArrayBuffer 和主世界拿到的是
// **同一个对象**，泵每 4ms 推进一次 syncVersion，渲染进程立刻就能看到。
// （开隔离时它会被结构化克隆成死快照，界面只会画出一帧。）
//
// 多会话下这个泵是所有会话共用的一个定时器：addon.poll 在没有新帧时只比
// 一个整数，所以空转很便宜；有 N 路时每轮多比 N 次，仍然可以忽略。
// ---------------------------------------------------------------------------
let pumpTimer = null
let pumpBusy = false
/**
 * 【诊断】同步泵占了多少主线程。
 *
 * 泵是同步 N-API 调用，跑在主线程上；它跑得太勤就会把 rAF 挤掉。这两个
 * 计数用来回答"泵到底跑了多快、每次多贵"，而不是靠猜。
 */
let pumpTicks = 0
let pumpPollMs = 0
let pumpBusySkips = 0
let pumpLastAt = 0
let pumpIntervalMs = 0

/**
 * 帧同步泵的运行计数。给出问题时的第一手数字：
 * ticks 是一共跑了多少轮，busySkips 是有多少轮因为上一轮还没跑完被跳过，
 * pollMs 是累计花在 addon.poll 上的毫秒数。
 */
function pumpDiagnose() {
  return {
    ticks: pumpTicks,
    busySkips: pumpBusySkips,
    pollMs: pumpPollMs,
    intervalMs: pumpIntervalMs,
    sessions: [...conns.values()].filter((c) => c.state === 'ready').length,
  }
}

function startPump() {
  if (pumpTimer) return
  // 不能再用固定 setInterval(4)：
  //
  // 一个 tick 的成本 ≈ 每路一次"拷最新槽"（都 1440p 时每路 ~1-3ms，两个环就是
  // 三份像素内存要拷）。两路时 tick 已经接近甚至超过 4ms，setInterval 会把
  // 它们排成队列 —— 结果是 setInterval 回调连轴转，把渲染进程饿死，
  // 界面表现成"只画了 2 帧"（实测就是这个数字）。
  //
  // 改成"一次排一次"：一个 tick 做完了才排下一个。这样无论几路都不会堆积，
  // 两路的目标节奏仍然是 ~100Hz，远超预览需要的 60Hz。
  const schedule = () => {
    pumpTimer = setTimeout(tick, 4)
    pumpTimer.unref?.()
  }
  const tick = () => {
    if (pumpBusy) {
      pumpBusySkips++
      schedule()
      return
    }
    pumpBusy = true
    try {
      if (addon) {
        const t0 = performance.now()
        for (const conn of conns.values()) {
          if (conn.state !== 'ready') continue
          try {
            // 内部快路径：没有新帧时只比一个整数
            addon.poll(conn.session)
          } catch (err) {
            // 服务端把这一路关了（比如窗口消失）—— 下次扫描会重新评估
            conn.state = 'failed'
            conn.error = err.message
          }
        }
        const t1 = performance.now()
        pumpTicks++
        pumpPollMs = t1 - t0
        if (pumpLastAt) pumpIntervalMs = t0 - pumpLastAt
        pumpLastAt = t0
      }
    } finally {
      pumpBusy = false
      schedule()
    }
  }
  schedule()
}

function startConnecting() {
  if (!addon) {
    fail(
      'addon',
      `原生扩展加载失败：${addonError?.message ?? '未知错误'}\n` +
        `请执行  npm run build:native  重新编译。`,
    )
    return
  }

  startPump()
  void scanSessions()

  // 每 2 秒重扫一次：
  //   * 服务还没起来 → 起来后自动接上；
  //   * 服务里改了配置（新增/删除/重新勾选）→ 这里自动跟上。
  if (!scanTimer) {
    scanTimer = setInterval(() => {
      void scanSessions()
    }, 2000)
    scanTimer.unref?.()
  }
}

// ---------------------------------------------------------------------------
// 主世界 API
// ---------------------------------------------------------------------------
const api = {
  addonStatus() {
    return { loaded: !!addon, error: addonError ? addonError.message : null, path: ADDON_PATH }
  },

  /** 所有会话的快照 */
  sessions() {
    return listSessions()
  },

  state() {
    return overallState()
  },

  fatal() {
    return fatal
  },

  /**
   * 兼容旧的单会话接口。
   *
   * 多会话下"整体状态"没有唯一答案，所以这里选第一路已就绪的会话作为
   * 代表 —— 老界面（只画一路）能继续跑，新界面请用 sessions()。
   */
  status() {
    const conn = firstReady()
    return {
      state: overallState(),
      fatal,
      session: conn?.session ?? null,
      ringInfo: conn?.ringInfo ?? null,
    }
  },

  /**
   * 取某一路的帧环句柄。
   *
   * contextIsolation 关掉之后，这里返回的 ArrayBuffer 和同步泵写入的是
   * **同一个对象**，所以主世界读到的 syncVersion 会一直更新 —— 这正是
   * 界面能实时预览的前提。（曾经开隔离时它会被结构化克隆成死快照。）
   *
   * 传空/不传时回退到第一路，兼容旧的单会话调用。
   */
  getRingBuffer(sessionId) {
    const conn = sessionId ? conns.get(sessionId) : firstReady()
    if (!conn || conn.state !== 'ready') return null
    try {
      const handle = addon.newBufferHandle(conn.session)
      return { buffer: handle.buffer, info: conn.ringInfo, session: conn.session }
    } catch (err) {
      console.error(`[preload] getRingBuffer(${conn.session}) 失败：`, err)
      return null
    }
  },

  /** 某一路当前发布帧的描述（几何信息 + 版本号） */
  frame(sessionId) {
    const conn = sessionId ? conns.get(sessionId) : firstReady()
    if (!conn) return { ok: false, reason: 'no-session' }
    if (conn.state !== 'ready') return { ok: false, reason: conn.state }
    const info = readFrameInfo(conn)
    if (!info) return { ok: false, reason: 'no-frame' }
    return { ok: true, ...info }
  },

  /** 兼容旧接口；新模型不需要归还槽位 */
  release() {
    return true
  },

  stats(sessionId) {
    const conn = sessionId ? conns.get(sessionId) : firstReady()
    if (!addon || !conn || conn.state !== 'ready') return null
    try {
      return {
        ...addon.stats(conn.session),
        counter: conn.view ? conn.view.getBigUint64(RING.U64.counter, true).toString() : '0',
        producedFrames: conn.view
          ? conn.view.getBigUint64(RING.U64.producedFrames, true).toString()
          : '0',
        droppedFrames: conn.view
          ? conn.view.getBigUint64(RING.U64.droppedFrames, true).toString()
          : '0',
      }
    } catch (err) {
      return { error: err.message }
    }
  },

  onEvent(callback) {
    if (typeof callback !== 'function') return () => {}
    eventListeners.add(callback)
    return () => eventListeners.delete(callback)
  },

  /** 重新尝试连接（界面上"重试"按钮用） */
  retry() {
    fatal = null
    if (!addon) {
      startConnecting()
      return false
    }
    void scanSessions()
    return overallState() === 'ready'
  },

  serviceInfo() {
    return ipcRenderer.invoke('service:info')
  },

  listWindows() {
    return ipcRenderer.invoke('service:list-windows')
  },

  /** 列出当前已连接的显示器（序号与采集服务的 outputIndex 一一对应）。 */
  listMonitors() {
    return ipcRenderer.invoke('service:list-monitors')
  },

  saveProfiles(profiles) {
    return ipcRenderer.invoke('service:save-profiles', profiles)
  },

  /**
   * 只保存**一路**配置。
   *
   * 界面改的永远只是一路（`saveEdits` 里 `profiles.value.map(...)` 只换一个），
   * 所以走这条单路通道：主进程从磁盘读现配、只替换这一路再写回，别的分路
   * 一个字节都不碰。以前发整份数组，界面那份只要旧了一秒，保存这一路就会
   * 把隔壁回退成旧值 —— 隔壁正在录的那一路下一拍就换了帧率。
   */
  saveProfile(id, patch) {
    return ipcRenderer.invoke('service:save-profile', { id, patch })
  },

  /** 配置文档（主进程会补齐缺省值，界面拿到的就是服务真正会用的那份） */
  serviceProfiles() {
    return ipcRenderer.invoke('service:profiles')
  },

  openServiceConsole() {
    // 曾经写的是 'service:spawn-detached' —— 主进程里根本没有这个通道，
    // 点了就抛 "No handler registered for 'service:spawn-detached'"。
    return ipcRenderer.invoke('service:launch')
  },

  /**
   * 服务的实时状态（录制进度、导出进度、各配置的采集帧率）。
   *
   * 这些数字来自采集服务的 stdout，而那条流只有启动服务的进程读得到，
   * 所以页面只能从主进程转一道手。owned=false 表示服务不是本界面启动的，
   * 读不到它的输出 —— 此时界面必须明说"看不到录制状态"，
   * 不能拿一堆 0 冒充"没在录"。
   */
  serviceLive() {
    return ipcRenderer.invoke('service:live')
  },

  /** 让服务立刻汇报一次状态（服务每秒也会自己推，这里用于操作后立刻刷新） */
  refreshService() {
    return ipcRenderer.invoke('service:refresh')
  },

  /**
   * 导出最近一段（需求 6）。
   *
   * 故意不 await 结果：导一次要几百毫秒到几秒，结果通过 onServiceEvent 的
   * export_result 事件回来。这里只回"命令发出去没有"。
   */
  exportReplay(id, seconds) {
    return ipcRenderer.invoke('service:export', { id, seconds })
  },

  /** 订阅服务事件（sessions / export_result / service_exit / desktop_ready） */
  onServiceEvent(callback) {
    if (typeof callback !== 'function') return () => {}
    const wrapped = (_evt, payload) => {
      try {
        callback(payload)
      } catch (err) {
        console.error('[preload] 服务事件处理出错：', err)
      }
    }
    ipcRenderer.on('service:event', wrapped)
    return () => ipcRenderer.removeListener('service:event', wrapped)
  },

  // 页面主动把统计推给主进程。
  //
  // 为什么不反过来让主进程用 executeJavaScript 去"拉"：跨进程读一次要
  // 2.5 秒（序列化整棵对象树），一拉就把渲染进程掐停，量出来的帧率是假的
  // （实测 46fps，而"中位帧间隔"却是达标的 16.8ms —— 这个矛盾就是它造成的）。
  // 推送是 send（不等回包），开销可忽略，量出来的才是真实帧率。
  reportStats(sample) {
    try {
      ipcRenderer.send('preview:stats', sample)
    } catch {
      // 上报失败绝不能把渲染循环带崩
    }
  },

  environment() {
    return { renderFps: process.env.SHADOW_RENDER_FPS ?? null }
  },

  // ---- 设置页 --------------------------------------------------------------

  /** 读应用设置（目前只有预览帧率上限）。 */
  getSettings() {
    return ipcRenderer.invoke('settings:get')
  },

  /**
   * 改设置。返回 { ok, settings, applied }。
   *
   * applied=false 只表示"服务这一拍没收到"，设置本身照样存下来了，
   * 界面不该把它当失败弹错误。
   */
  setSettings(patch) {
    return ipcRenderer.invoke('settings:set', patch)
  },

  /**
   * 订阅"预览帧率被主进程改了"的通知。
   *
   * 除了设置页自己改，服务启动时主进程也会把存下来的值推一遍（见
   * app/main.cjs 的 pushSettingsToService）—— 所以界面必须能收到外部改动，
   * 不能只信自己那次调用的返回值。
   */
  onPreviewFps(callback) {
    if (typeof callback !== 'function') return () => {}
    const wrapped = (_evt, fps) => {
      try {
        callback(fps)
      } catch (err) {
        console.error('[preload] 预览帧率通知处理出错：', err)
      }
    }
    ipcRenderer.on('settings:preview-fps', wrapped)
    return () => ipcRenderer.removeListener('settings:preview-fps', wrapped)
  },

  /**
   * 同步泵的节奏（纯标量）。
   *
   * 页面每 500ms 把它塞进一笔采样里推给主进程，所以这里只能给标量 ——
   * 高频路径上带大对象，跨进程序列化本身就会拖慢主线程。
   */
  pumpStats() {
    return {
      ticks: pumpTicks,
      intervalMs: Math.round(pumpIntervalMs * 100) / 100,
      pollMs: Math.round(pumpPollMs * 100) / 100,
      busySkips: pumpBusySkips,
    }
  },

  /** 诊断用：看看 preload 到底从主进程拿到了什么 */
  diagnose() {
    return {
      state: overallState(),
      addon: { loaded: !!addon, error: addonError ? addonError.message : null },
      scanTimerRunning: !!scanTimer,
      conns: listSessions(),
      lastProfilesResponse,
      // 关键：同步泵自己那块 view 里读到的版本号。
      // 如果它和主世界（usePreview）读到的版本号不一样，说明两边**不是同一块
      // 内存** —— 那主世界就永远看不到新帧。
      liveSync: [...conns.values()].map((c) => ({
        session: c.session,
        state: c.state,
        pumpSyncVersion: c.view ? c.view.getBigUint64(RING.U64.syncVersion, true).toString() : null,
        pumpCounter: c.view ? c.view.getBigUint64(RING.U64.counter, true).toString() : null,
      })),
      // 泵自己的节奏：主线程有没有被同步拷贝占满，看这几个数就知道。
      pump: pumpDiagnose(),
    }
  },
}

function firstReady() {
  for (const conn of conns.values()) {
    if (conn.state === 'ready') return conn
  }
  return null
}

// contextIsolation 是关掉的（见 app/main.cjs 里的说明），preload 与页面在
// 同一个 JS 世界里，所以直接把 API 挂到 window 上 —— 不走 contextBridge。
// 这样帧环那些 ArrayBuffer 都是**同一个对象**，主世界读到的就是实时内存。
window.shadowPreview = api

// 界面侧绘制上限（来自 --render-fps），交给页面里的 usePreview 读。
// 空字符串或非法值一律当"不限"，页面那边自己有兜底判断。
if (process.env.SHADOW_RENDER_FPS !== undefined) {
  window.shadowRenderFps = process.env.SHADOW_RENDER_FPS
}

startConnecting()
