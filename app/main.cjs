// app/main.cjs — Electron 主进程（CommonJS）
//
// 为什么是 .cjs 而不是 ESM：
//   Electron 主进程里 require('electron') 返回的是 CommonJS 对象，
//   在 ESM 下 `import { BrowserWindow } from 'electron'` 会因为
//   electron 包的 ESM 导出探测失败而报
//   "does not provide an export named 'BrowserWindow'"。
//   官方主进程示例一直是 CommonJS，这里保持一致，最稳。
//
// 这个文件只负责"窗口"和"采集服务状态"，不碰帧数据：
//   * 检测 C++ 采集服务是否在运行（读它的互斥体）
//   * 提供"启动采集服务"的入口（spawn 一个独立进程，不跟着界面生死）
//   * 创建窗口、加载前端
//
// 帧数据走共享内存，由 app/preload.cjs 里的 N-API 扩展直接映射，
// 再通过 MessagePort 把这块内存交给渲染进程。详见 native/ShadowRing.h。

const { app, BrowserWindow, ipcMain, dialog, shell } = require('electron')
const { spawn, spawnSync } = require('node:child_process')
const net = require('node:net')
const { copyFileSync, existsSync, mkdirSync, readFileSync, rmSync, statSync, writeFileSync } =
  require('node:fs')
const path = require('node:path')

// 控制台 UTF-8：中文 Windows 默认按 GBK(936) 解我们的 UTF-8 日志，于是所有
// 中文 console.log 都变成「宸叉竻鐞…」。launch.mjs 启动时已经 chcp 过一次，
// 但本文件是 Electron 主进程，单独启动（如直接 `electron .`）时要自己保命。
// chcp 改的是控制台对象的代码页（不是某个进程），对后续输出立即生效。
if (process.platform === 'win32') {
  try {
    spawnSync('cmd', ['/c', 'chcp', '65001'], { stdio: 'ignore', windowsHide: true })
  } catch {
    /* 切不了也不影响功能，只是日志仍是乱码 */
  }
}

const { SERVICE_SESSION_ID } = require('./service-const.cjs')
const { probe } = require('./probe-core.cjs')

const PROJECT_ROOT = path.resolve(__dirname, '..')

const IS_DEV = process.argv.includes('--dev')
const IS_PROBE = process.argv.includes('--probe')
const IS_SELFTEST = process.argv.includes('--selftest')
// --gui-export [等秒数]：需求 6 的端到端验收 —— 界面**自己起服务**，等它录出
// 分片，然后真的点一次"导出"，检查 MP4 落到磁盘上。默认等 12 秒。
const IS_GUI_EXPORT = process.argv.includes('--gui-export')
const guiExportArg = process.argv[process.argv.indexOf('--gui-export') + 1]
const GUI_EXPORT_WAIT =
  Number.isFinite(Number(guiExportArg)) && Number(guiExportArg) > 0 ? Number(guiExportArg) : 12
// --selftest [秒数]，默认跑 6 秒
const selftestArg = process.argv[process.argv.indexOf('--selftest') + 1]
const SELFTEST_SECONDS = Number.isFinite(Number(selftestArg)) && Number(selftestArg) > 0 ? Number(selftestArg) : 6
// --external-probe [秒数]：验收「服务是在界面**之外**启动的」这一场景。
// 用户报的 bug 就是它：在 cmd 里 npm run service 起了服务，界面却说没启动。
// 这条路径**绝不**自己拉服务，只读状态文件，看认不认得出来。默认等 10 秒。
const IS_EXTERNAL_PROBE = process.argv.includes('--external-probe')
const externalArg = process.argv[process.argv.indexOf('--external-probe') + 1]
const EXTERNAL_SECONDS =
  Number.isFinite(Number(externalArg)) && Number(externalArg) > 0 ? Number(externalArg) : 10
// --render-fps <n>：界面侧绘制上限，0 表示不限（跟随 rAF 刷新率）。
// 由 scripts/launch.mjs 解析成 SHADOW_RENDER_FPS，preload 读它并挂到 window 上
// 给页面用（contextIsolation 已关闭，见 README）。
const DEV_URL = process.env.DEV_SERVER_URL || 'http://127.0.0.1:5173'

const BACKEND_PATH = path.join(PROJECT_ROOT, 'native', 'build', 'shadow-capture.exe')

// 命令管道的名字。必须和 native/backend/RingNames.h 的 CommandPipeName()
// 逐字一致 —— 两边各写一份是没办法的事（N-API 插件和 .cjs 主进程不是同一个
// 构建单元），所以这里把出处标清楚，改的时候两边一起改。
const PIPE_NAME = '\\\\.\\pipe\\ShadowPlay.Cmd.v1'
const ARTIFACT_DIR = path.join(PROJECT_ROOT, 'native', 'build', 'artifacts')

// 导出的 mp4 落仓库根的 export/。
//
// 为什么不和录制分片一起放临时目录：分片是"滚动覆盖的中间产物"，跑完就该扔，
// 放 %TEMP% 正合适；导出的 mp4 是**用户要拿走的成品**，放进临时目录等于随
// 时可能被清理掉、也不好找。放仓库根的好处是一眼看得见，和源码同一个"当前
// 目录"。
const EXPORT_DIR = path.join(PROJECT_ROOT, 'export')

const { TEMP, TEMP_ROOT, sweepStaleRoots, markOwner } = require('./temp-const.cjs')
// 守护进程由采集服务自己拉起（--guard 模式），这里只读它的心跳：界面顶栏
// 显示"守护在岗/未运行"，不去管它的生死。详见 native/backend/GuardianMode.h。
const { guardianSnapshot } = require('./guardian.cjs')
const { cleanOnStartup } = require('./temp-clean.cjs')

// 先扫一遍 %TEMP%/shadow-play/ 下面的**旧实例**：每个实例的根目录里有一份
// .owner 标记（记录拥有者 pid），拥有者已经不在了就整棵删掉。
//
// 为什么需要这一步：临时根是"一次运行一份"的（见 temp-const.cjs 的 doc），
// 但被强杀、断电、崩溃的运行不会自己收尾，它们的根目录会一直躺在 %TEMP% 里。
// 没有这个扫描，用久了就是几十个 GB 的死目录。
sweepStaleRoots({ log: (m) => console.log('[main]', m) })

// 状态文件 / 录制目录 / 导出目录 / 日志全在临时目录里（见 app/temp-const.cjs）。
// 每次启动先把上一次运行留下的残留清掉：上次被强杀留下的孤儿分片会让界面
// 显示"有录像"，半截 mp4 会让导出报错，旧状态文件会让界面以为服务还活着。
// 不清理的话这些误判比垃圾本身难查得多。
//
// 例外是 records/：采集服务是独立进程，关掉界面它照样在录。这时删掉它就是
// 把编码器正在写的分片抽走，界面上会报「编码器提前退出（分片写入中断）」。
// 所以先看一眼状态文件：够新就说明服务活着，records/ 一个字节都不动。
//
// 这里不能调 serviceRunningFast()：它要判断 SERVICE_EXE 存不存在，而那个
// const 还在下面，模块执行到这一行时它处于暂时性死区（TDZ），直接抛
// "Cannot access 'SERVICE_EXE' before initialization" 把整个主进程打死。
// 所以只用不依赖后面任何东西的那几招，从最便宜的开始试：
//
//   1) 状态文件够新 —— 服务每秒写一次，最新鲜的时候也不过几百毫秒。
//   2) 状态文件不够新但存在 —— `npm start` 是 launch.mjs **先起服务、
//      再起界面**，界面起来时服务可能还没写出第一份状态（实测有这一瞬），
//      而它马上就要开始录了。这时候删 records/ 同样是抽走别人的分片。
//   3) service.pid 里记的那个进程还活着。
const startupMirror = readStatusFile()
let serviceAlive = Boolean(startupMirror && startupMirror.doc && !startupMirror.stale)
let aliveWhy = serviceAlive ? '状态文件新鲜' : ''
if (!serviceAlive && startupMirror && startupMirror.mtimeMs > 0) {
  serviceAlive = true
  aliveWhy = '状态文件存在（服务刚起来）'
}
if (!serviceAlive) {
  // 进程还在不在：process.kill(pid, 0) 只是探活，不会真发信号。
  try {
    const pid = Number(readFileSync(path.join(TEMP_ROOT, 'service.pid'), 'utf8').trim())
    if (Number.isFinite(pid) && pid > 0) {
      process.kill(pid, 0)
      serviceAlive = true
      aliveWhy = `service.pid 记着 pid ${pid}`
    }
  } catch {
    // 没有 pid 文件 / 进程已经没了：当作服务没在跑，正常清理
  }
}
cleanOnStartup(TEMP_ROOT, { log: (m) => console.log('[main]', m), keepLive: serviceAlive })
if (serviceAlive) {
  console.log(`[main] 采集服务在运行（${aliveWhy}），录像目录原样保留（不清分片）`)
}
// 写下本次运行的拥有者标记：下次启动扫到它时，本进程早就不在了，整棵删。
markOwner(TEMP_ROOT, 'app')
const log = (...a) => console.log('[main]', ...a)
const logErr = (...a) => console.error('[main]', ...a)

// 实际要跑的那份采集服务 exe：**就是编译产物本身**，native/build/shadow-capture.exe。
//
// 【为什么不复制到 %TEMP% 再跑 —— 那条路踩过两次，已经废掉】
// 以前这里走 ensureServiceExe()：把 exe 拷一份到 %TEMP%/shadow-play/app/bin/ 再
// 启动，理由是"仓库目录可能带低完整性标签，从那里起的进程会被 WGC 拒绝"。
// 实际代价远大于收益：
//
//   1. 副本是**第二个真实存在的可执行文件**，于是"服务在用哪一份"成了新的
//      状态。界面启动清理把 bin/ 删掉之后，SERVICE_EXE 永久指向一个不存在的
//      路径：探测不到 → 自动启动 → 还是那个不存在的路径 → 起不来。表现就是
//      界面一直「正在连接…」，任务管理器里也没有采集服务。
//   2. 服务已经在跑时，副本**不能刷新**（换文件会弄死正在跑的进程），于是
//      重新编译出来的 exe 在老服务活着时永远用不上，"改了没生效"的排查成本
//      全压在这上面。
//
// 只跑一份编译产物，上面两类问题一起消失：路径恒定、永远不会"指向不存在
// 的文件"、重编译后重启服务就一定生效。
const SERVICE_EXE = BACKEND_PATH

// 必须在任何 app.* 调用之前设置 userData：
//   1. 默认位置（%APPDATA%）在受限终端/沙箱下可能写不进去，
//      Electron 会直接因为 "Lock file can not be created" 而退出；
//   2. 放在临时目录里更干净 —— 它也只是"删了能重建"的缓存。
// 想保留它（比如查 DevTools 缓存）就设 SHADOW_USER_DATA 指到别处。
const userDataDir = process.env.SHADOW_USER_DATA || path.join(TEMP_ROOT, 'userdata')
try {
  mkdirSync(userDataDir, { recursive: true })
} catch {
  /* 忽略：交给 Electron 自己报错 */
}
app.setPath('userData', userDataDir)

// ---------------------------------------------------------------------------
// 采集服务探测 / 启动
// ---------------------------------------------------------------------------
/** 只看退出码：0 = 正在运行，1 = 没运行。避免依赖子进程管道。 */
function serviceRunning() {
  if (!existsSync(SERVICE_EXE)) return null
  try {
    // 探测也走运行副本：互斥体是按 exe 走的，用原件探测和用副本探测在
    // "有没有服务在跑"这件事上等价，但副本才是我们真正会 spawn 的那个。
    const r = spawnSync(SERVICE_EXE, ['--status'], { stdio: 'ignore', windowsHide: true })
    return r.status === 0
  } catch {
    return null
  }
}

/**
 * 同上，但**先看状态文件**。
 *
 * serviceRunning() 每次都 spawnSync 一个新进程 —— Windows 上光 CreateProcess
 * 就要几十毫秒，而且这是同步的，会卡住整个主进程。界面轮询 service:info、
 * 保存配置、点导出都会走到这里，GPU 高负载时机器本来就紧张，这几百毫秒的
 * 同步阻塞直接表现成"Electron 很卡"。
 *
 * 服务每秒都在写状态文件（见 native/backend/StatusFile.cpp），文件够新就等于
 * 服务活着 —— 这个判断是 O(读一个 JSON)，不产生进程。文件不够新时才退回
 * 真正的进程探测，并把结果缓存 1 秒，避免连续点击时反复 spawn。
 */
let lastProbe = { at: 0, value: null }
function serviceRunningFast() {
  if (!existsSync(SERVICE_EXE)) return null

  // 服务随时在写状态文件：新鲜 = 活着。
  const mirror = readStatusFile()
  if (mirror && mirror.doc && !mirror.stale) return true

  // 文件不可用：退回真探测，但 1 秒内不重复探测。
  const now = Date.now()
  if (now - lastProbe.at < 1000) return lastProbe.value
  const value = serviceRunning()
  lastProbe = { at: now, value }
  return value
}

// ---------------------------------------------------------------------------
// 应用设置（就是设置页里那几个开关）
//
// 为什么单独存一个文件，而不是塞进 profiles.json：那是"录什么"的配置，服务
// 会整个读进去；这里是"界面怎么显示"的偏好，服务只关心其中一两个值（预览
// 帧率上限）。混在一起的话，界面上随便拨个开关都要触发服务重读整份配置。
//
// 存在临时目录而不是仓库里：和 profiles.json 不同，这些设置丢了也能用（只是
// 回到默认值），不值得污染仓库。路径与 userData 同级，清理 TEMP 时一起清掉。
// ---------------------------------------------------------------------------
const SETTINGS_FILE = path.join(TEMP.root, 'settings.json')

// 帧率上限：录制侧与预览侧共用 120（后端 `MfEncoder::kMaxFps` 同值）。
//
// 以前这里是 240 —— 那时候还想着「240Hz 显示器 + 240 帧录制」。实测之后
// 这个想法站不住：1440p 的硬件编码器物理上限只有 172fps，165 那档的每帧
// 成本已经顶破帧预算、真实画面只剩一半。主人拍板「120 以上的帧率全删」。
const MAX_FPS = 120

const DEFAULT_SETTINGS = {
  // 预览画面最大帧率。0 = 不限（跟随采集帧率）。
  //
  // 默认 60：预览窗口就那么大，画 120 帧和 60 帧肉眼没区别，但每一帧都要从
  // 共享环 memcpy 一次全屏（1440p 是 14.7MB）。限到 60 直接把这份带宽砍半，
  // 省下的时间归采集循环自己用 —— 录制帧率反而更容易达标。
  //
  // **只管预览**：录制照旧按每路配置的帧率喂编码器，导出也照旧按配置帧率，
  // 这个值对成片没有任何影响。
  previewMaxFps: 60,
}

function settingsPath() {
  const override = process.env.SHADOW_SETTINGS_FILE
  return override || SETTINGS_FILE
}

/** 读设置。文件不存在 / 解析失败 / 字段离谱，一律退回默认值。 */
function loadSettings() {
  let raw = null
  try {
    if (existsSync(settingsPath())) raw = JSON.parse(readFileSync(settingsPath(), 'utf8'))
  } catch {
    /* 坏了就当没有 */
  }
  const out = { ...DEFAULT_SETTINGS }
  if (raw && typeof raw === 'object') {
    const fps = Number(raw.previewMaxFps)
    // 夹到 0..MAX_FPS：0 表示不限，负数/NaN/离谱的大数都当没填。
    // 上限跟录制侧一致（120）—— 预览再高也没有更多新帧可画。
    if (Number.isFinite(fps)) out.previewMaxFps = Math.max(0, Math.min(MAX_FPS, Math.round(fps)))
  }
  return out
}

function saveSettings(patch) {
  const cur = loadSettings()
  const next = { ...cur }
  if (patch && typeof patch === 'object') {
    if (patch.previewMaxFps !== undefined) {
      const fps = Number(patch.previewMaxFps)
      if (Number.isFinite(fps)) next.previewMaxFps = Math.max(0, Math.min(MAX_FPS, Math.round(fps)))
    }
  }
  try {
    mkdirSync(path.dirname(settingsPath()), { recursive: true })
    writeFileSync(settingsPath(), JSON.stringify(next, null, 2), 'utf8')
  } catch (err) {
    return { ok: false, error: `写入设置失败：${err.message}` }
  }
  return { ok: true, settings: next }
}

let serviceChild = null

// ---------------------------------------------------------------------------
// 状态文件镜像（native/backend/StatusFile.h）
//
// 采集服务的状态原来只走 stdout，而 **stdout 只有启动它的进程能读**。于是用
// `npm run service` / cmd 直接起服务时，界面看不到录制、看不到分片、导出按钮
// 永远是灰的 —— 顶栏还显示"外部服务"，可服务其实跑得好好的。
//
// 服务现在每秒把一份完整状态写进 test/.status/status-v1.json（原子替换）。这里
// 读那个文件，**不管服务是谁起的**都能拿到状态。两条通道合起来的规则：
//   * 服务是我们起的   -> 优先用 stdout（实时、带一次性事件），文件当备份；
//   * 服务是别人起的   -> 只有文件可用，就用文件。
// 两边都读不到了才算"没在运行"。
//
// 路径算法必须和 native/backend/StatusFile.cpp 里的 StatusFileDefaultPath()
// 一致：服务 exe 往上找带 scripts 目录的那一级 = 仓库根，再拼 temp/status/。
// 这里直接用 PROJECT_ROOT + TEMP，它就是同一个东西。
// ---------------------------------------------------------------------------
function statusFilePath() {
  const override = process.env.SHADOW_STATUS_FILE
  if (override) return override
  return path.join(TEMP.root, 'status', 'status-v1.json')
}

/**
 * 某个 pid 现在还活着吗。
 *
 * `process.kill(pid, 0)` 不投递信号，只做存在性/权限检查：进程不存在抛
 * ESRCH，存在但没权限抛 EPERM（那也是活着）。
 */
function pidAlive(pid) {
  if (!Number.isFinite(pid) || pid <= 0) return false
  try {
    process.kill(pid, 0)
    return true
  } catch (e) {
    return Boolean(e) && e.code === 'EPERM'
  }
}

/** 读一份状态文件。读不到 / 解析失败 / 太旧，都返回 null（当"没有镜像"处理）。 */
function readStatusFile() {
  const file = statusFilePath()
  try {
    const st = statSync(file)
    // 服务每秒写一次。超过 8 秒没动过就当它已经不在了 —— 否则服务被强杀之后
    // 界面会永远显示最后一份"录制中"，比不显示更误导。
    const ageMs = Date.now() - st.mtimeMs
    if (ageMs > 8000) return { file, ageMs, mtimeMs: st.mtimeMs, stale: true, doc: null }
    const doc = JSON.parse(readFileSync(file, 'utf8'))
    // 【光看 mtime 不够】服务被杀掉之后文件还是刚写的、mtime 依然"新鲜"，
    // 于是 serviceRunningFast() 会误判"服务还在跑"、进而跳过自动拉起 ——
    // 表现就是"服务死了但界面一直说在跑、预览和录制都没有画面"。
    // 再核一下文件里记的 pid 是否还活着，才算数。
    if (!pidAlive(Number(doc && doc.pid))) {
      return { file, ageMs, mtimeMs: st.mtimeMs, stale: true, doc: null }
    }
    return { file, ageMs, mtimeMs: st.mtimeMs, stale: false, doc }
  } catch {
    return { file, ageMs: null, mtimeMs: 0, stale: false, doc: null }
  }
}

const live = {
  sessions: [], // 每个配置的最新一条 session 状态
  profilesLoaded: null, // 服务的 profiles_loaded 消息
  exportEvents: [], // 最近的导出事件（界面用来弹提示）
  lastExportPlan: '', // 最近一次导出的计划诊断行（export_plan 事件）
  updatedAt: 0,
  sawServiceOutput: false,
  mirrored: null, // 最近一次读到的状态文件（{file, ageMs, doc}），没有就是 null
  mirrorSessions: [], // 状态文件里的 sessions（外部服务时界面靠它）
  mirrorUpdatedAt: 0, // 状态文件的 mtime（用来判断"镜像是不是更新的那份"）
}

const EXPORT_EVENT_KEEP = 20

function recordExportEvent(msg) {
  live.exportEvents.push({ at: Date.now(), ...msg })
  if (live.exportEvents.length > EXPORT_EVENT_KEEP) {
    live.exportEvents.splice(0, live.exportEvents.length - EXPORT_EVENT_KEEP)
  }
}

/** 按行切 NDJSON。半行留在 buf 里等下一次数据。 */
function makeLineSplitter(onLine) {
  let buf = ''
  return (chunk) => {
    buf += chunk
    const lines = buf.split('\n')
    buf = lines.pop() ?? ''
    for (const line of lines) {
      const t = line.trim()
      if (t) onLine(t)
    }
  }
}

function ingestServiceLine(line) {
  let msg
  try {
    msg = JSON.parse(line)
  } catch {
    return // 非 JSON 行（服务偶尔会打中文提示）直接忽略
  }
  live.sawServiceOutput = true
  live.updatedAt = Date.now()

  if (msg.type === 'sessions' && Array.isArray(msg.items)) {
    // 整批替换：服务的每条 session 都是完整快照，差量合并反而更容易留下幽灵。
    live.sessions = msg.items
  } else if (msg.type === 'profiles_loaded') {
    live.profilesLoaded = msg
  } else if (msg.type === 'export_started' || msg.type === 'export_result') {
    recordExportEvent(msg)
    // 验收脚本（--gui-export）要等这一条，存一份最新结果做信号量
    if (msg.type === 'export_result') live.lastExportResult = msg
  } else if (msg.type === 'desktop_ready' || msg.type === 'no_sessions') {
    live.lastNotice = msg
  } else if (msg.type === 'export_plan') {
    // 导出计划的诊断行（哪个片、哪一段补了 NO SIGNAL）。
    //
    // 为什么要存下来：NO SIGNAL 的三种成因（分片真有空洞 / 计划器算漏 /
    // 某片 probe 失败被跳过）只看摘要分不清，而这行日志只在 --gui-export
    // 里靠 stdout 能看见 —— 服务是常驻进程、被别的界面复用时就看不到了。
    // 存进 report 之后，验收报告自己就带着"为什么补了遮罩"。
    live.lastExportPlan = msg.detail
    if (IS_GUI_EXPORT) log('[export_plan]', msg.detail)
  } else if (msg.type === 'export_status' && msg.plan) {
    // 界面点导出走的是插件（Electron 主进程内），服务不知道有这回事 ——
    // 那条 export_plan 只能由 exportStatus() 捎过来。两条路都收，谁先到用谁。
    live.lastExportPlan = msg.plan
    if (IS_GUI_EXPORT) log('[export_plan]', msg.plan)
  }
  // 事件转发给所有窗口（界面靠它知道导出结束，不用轮询）
  for (const w of BrowserWindow.getAllWindows()) {
    try {
      w.webContents.send('service:event', msg)
    } catch {
      /* 窗口正在关 */
    }
  }
}

/** 页面推上来的渲染统计（见 registerIpc 里的 'preview:stats'）。
 *  自检只用它，不再用 executeJavaScript 去读渲染进程。 */
const pushedSamples = []

/** 启动一个**独立**的采集服务进程（不随界面退出而终止） */
function launchService() {
  if (!existsSync(SERVICE_EXE)) {
    return { ok: false, error: `找不到采集服务：${SERVICE_EXE}\n请执行 npm run build:native` }
  }
  // 启动前清掉可能残留的停机哨兵（上次干净退出留下的），免得它挡住本次启动。
  try {
    rmSync(path.join(TEMP_ROOT, 'status', 'shutdown-requested'), { force: true })
  } catch {}
  if (serviceRunning() === true) {
    // 系统里已经有采集服务，复用它 —— 但**要说清楚是不是同一份配置**。
    //
    // serviceRunning() 只探测"有没有 shadow-capture 在跑"，它不知道那个进程
    // 加载的是哪份 profiles、录的是哪个目录。用户自己 `npm run service` 起了一份、
    // 界面又用 SHADOW_PROFILES 指到另一份时，两边看到的录制状态就对不上了：
    // 界面报的分片数是那个服务的目录，导出却按这份配置的 recordRoot 去找 ——
    // 表现成"界面上明明有 7 段，导出说没有可导出的分片"。
    //
    // 光看"有服务"不够，所以把当前要用的配置路径一并带出去，界面上能直接看出
    // 复用的这份是不是自己期望的那份（`service:status` 里的 profilesPath）。
    // 服务在 profiles_loaded 里报的字段就叫 path（native/backend/main.cpp:299），
    // 状态文件和 stdout 两条通道都有它，取到哪个用哪个。
    const wanted = profilesPath()
    const mirrored = readStatusFile()
    const actual =
      mirrored?.doc?.profilesPath ??
      mirrored?.doc?.path ??
      live.profilesLoaded?.path ??
      null
    const mismatch = !!actual && path.resolve(actual) !== path.resolve(wanted)
    // 复用的这份服务**没收到过**本机的设置。
    //
    // 预览帧率上限存在 Electron 这边的 settings.json 里，服务进程自己看不到；
    // 它只认命令行参数和配置文件，其它一切都靠命令通道下发。而"复用"这条
    // 路径以前是直接 return 的 —— 于是 `npm run service` 先起、界面后开的
    // 情形下，设置永远下发不到，界面上调了 60 也没用，预览照样按"不限"跑
    // （实测：设置 60，界面画出 80.8fps）。这里补一次下发。
    void pushSettingsToService()
    return {
      ok: true,
      alreadyRunning: true,
      profilesPath: wanted,
      runningProfilesPath: actual,
      profilesMismatch: mismatch,
    }
  }

  // 必须用 --profiles：不带这个参数服务走的是"单路旧模式"（一个环、没有录制
  // 也没有导出），跟界面上这份配置列表完全对不上。
  const file = profilesPath()
  serviceChild = spawn(SERVICE_EXE, ['--profiles', file], {
    cwd: PROJECT_ROOT,
    // 【stdin 必须是 ignore，不能是 pipe】
    //
    // 服务是"关掉界面也要继续录"的常驻进程。可它的 stdin 一旦是界面给的管道，
    // 界面进程没了、管道就断 —— 服务那边读到 EOF，直接把自己退了。实测：
    // 界面 spawn 的服务 pid 33616，界面一关，进程列表里立刻没有
    // shadow-capture.exe，用户看到的就是"后台服务不启动了"。
    //
    // 命令通道不依赖它：sendServiceCommand() 主通道是命名管道
    // （\\.\pipe\ShadowPlay.Cmd.v1，谁起的服务都能连），stdin 只是老版本的兜底。
    // 用 ignore 换来的是"界面生死与服务无关"。
    stdio: ['ignore', 'pipe', 'pipe'],
    // detached：服务必须在界面退出后继续常驻（用户要求：Electron 退出，
    // 服务不能退）。Windows 上 libuv 会在 detached 时带上
    // CREATE_BREAKAWAY_FROM_JOB —— 没有它，Electron（Chromium）把自己的
    // 子进程都塞进 kill-on-close 的作业对象里，界面一关作业对象一关，
    // 服务就被连带杀掉，录到一半的会话也跟着死。Breakaway 让服务
    // 脱离那个作业对象，界面生死与它无关。
    detached: true,
    windowsHide: true,
    // 托盘「打开主界面」要用它找到 node.exe（见 native/backend/TrayIcon.cpp）。
    env: { ...process.env, SHADOW_NODE_EXE: process.execPath },
  })
  const out = makeLineSplitter(ingestServiceLine)
  serviceChild.stdout.on('data', (d) => out(d.toString('utf8')))
  serviceChild.stderr.on('data', (d) => {
    const t = d.toString('utf8').trim()
    if (t) log('[service]', t.slice(0, 400))
  })
  serviceChild.on('exit', (code, sig) => {
    log('采集服务退出，code =', code, 'sig =', sig)
    live.sessions = []
    serviceChild = null
    for (const w of BrowserWindow.getAllWindows()) {
      try {
        w.webContents.send('service:event', { type: 'service_exit', code })
      } catch {
        /* 窗口正在关 */
      }
    }
  })
  // 让界面进程退出时不被这个子进程拖住。注意**不是**杀掉它——分离的
  // 意义就是让它在界面退出后继续录、继续常驻（见上面 detached 的注释）。
  // 停止服务的唯一途径：界面里的「停止采集服务」按钮或 npm run service:stop。
  serviceChild.unref?.()

  log('已启动采集服务，pid =', serviceChild.pid, '配置 =', file)

  // 启动成功后把设置下发一遍。
  //
  // 服务进程自己不知道 settings.json 在哪（它只认命令行参数和配置文件），
  // 预览帧率上限是通过命令通道告诉它的。刚 spawn 时命名管道还没建好，
  // 所以这里轮询重试：连上就发，连不上就等 —— 最多试约 10 秒，之后放弃
  // （服务起来后会自己按 0 = 不限 跑，用户再进一次设置页也能补上）。
  void pushSettingsToService()

  return { ok: true, pid: serviceChild.pid, profiles: file }
}

/**
 * 把当前设置推给正在跑的服务（预览帧率上限）。
 *
 * 什么时候需要它：
 *   * 服务刚起来时（见 launchService）；
 *   * 界面启动时发现服务是别人起的（比如 npm run service）——
 *     那份服务也没收到过设置，预览会按"不限"跑。
 *
 * 失败不抛错：设置本来就是"能生效最好，不生效也不影响录制"。
 */
async function pushSettingsToService() {
  const { previewMaxFps } = loadSettings()
  // 先把值推给页面，再去等管道。
  //
  // 踩过的坑：原来只有 `r.ok`（服务真的收到了）之后才调 applyPreviewFpsToPage，
  // 而服务没起 / 管道还没建好时这个函数会静默失败 —— 页面于是永远收不到
  // 存下来的值，界面停在默认的 60fps，用户看到的就是"设置没保存"。
  // 页面侧的值和"服务有没有收到"本来就是两件事：值先落地，服务随后补发。
  applyPreviewFpsToPage(previewMaxFps)
  for (let attempt = 0; attempt < 25; attempt++) {
    const r = await sendServiceCommand({ cmd: 'set-preview-fps', fps: previewMaxFps })
    if (r.ok) return true
    // 管道还没建好：等一下再来。100ms × 25 ≈ 2.5 秒的等待预算。
    await new Promise((res) => setTimeout(res, 100))
  }
  log('设置下发失败：服务没接上命令通道（预览帧率仍会是"不限"）')
  return false
}

/**
 * 把预览帧率上限同步给渲染进程。
 *
 * 采集端限流只减少"往共享环里写"的次数；渲染端自己也会按 60fps 画，
 * 两边用同一个值才不会出现"采集发 120、界面只画 60"这种各说各话。
 */
function applyPreviewFpsToPage(fps) {
  for (const w of BrowserWindow.getAllWindows()) {
    try {
      w.webContents.send('settings:preview-fps', fps)
    } catch {
      /* 窗口正在关 */
    }
  }
}

/** 把一条命令送进服务（命令通道见 native/backend/main.cpp）。异步。 */
async function sendServiceCommand(cmd) {
  // 主通道：命名管道。谁起的服务都能连，不受"是不是本进程 spawn 的"限制。
  //
  // 以前只有 stdin 一条路，而 stdin 只有启动服务的进程能写。`launch.mjs --bg`
  // 和"在 cmd 里直接跑 exe"这两种起法，服务端的 stdin 要么是 NUL 要么不可用，
  // 命令通道直接死掉 —— 于是"保存配置后让服务重载"只能退回"杀掉整个服务
  // 再拉一个"，把隔壁分路一起掐断。
  const pipeResult = await pipeSend(cmd)
  if (pipeResult.ok) return pipeResult

  // 兜底：管道没连上（老版本服务没有管道，或者服务刚起来还没建好）时
  // 退回 stdin。两条都失败才算失败。
  if (!serviceChild || !serviceChild.stdin || serviceChild.stdin.destroyed) {
    return { ok: false, error: pipeResult.error || '没有可用的命令通道。' }
  }
  try {
    serviceChild.stdin.write(JSON.stringify(cmd) + '\n')
    return { ok: true }
  } catch (err) {
    return { ok: false, error: `写命令失败：${err.message}` }
  }
}

/**
 * 通过命名管道发命令（服务端见 native/backend/main.cpp 的 pipeThread）。
 *
 * 异步返回 { ok, error }。
 *
 * 早期实现是 `spawnSync(node, ['-e', 连管道写一行的脚本])` —— 每发一条命令就
 * 起一个完整进程。Windows 上 CreateProcess + 加载 Node 至少要一两百毫秒，
 * 而且是**同步**的：保存一次配置要发 N 条 reload，主进程就卡 N × 200ms，
 * 用户看到的就是"点了保存要等两秒"。GPU 高负载时这段还会被拉得更长。
 *
 * 中间还试过用 `net.connect` 建一条常驻长连接复用。**这个做法是错的**：
 * Node 的 net.Socket 连命名管道时，写完之后内部会把管道客户端当成"可半关闭"，
 * 服务端那条 ReadFile 立刻拿到 ERROR_BROKEN_PIPE（109）就断开重建实例；客户端
 * 还以为连接活着，继续往里写就吃 EPIPE。实际表现是——**只有第一条命令能送到，
 * 之后的全部石沉大海**。
 *
 * 这个 bug 极其阴险：命令确实写进了管道（write 回调成功），服务端也确实连上了，
 * 但服务端只处理到第一条就掉线。于是"改帧率保存"看起来一切正常，帧率却纹丝不动。
 *
 * 现在每次命令都**单独打开一次管道**（`fs.promises.open` + write + close）。
 * 命名管道是文件，用文件方式打开写入在 Windows 上是可靠的；异步版本不占主线程，
 * 一次往返通常 1~5 毫秒，比 net.Socket 还快（省掉握手和事件循环调度）。
 *
 * 服务端是"一个连接处理完就重建实例等下一个"，所以一次连接发一条命令正合适。
 */
function pipeSend(cmd) {
  const fs = require('node:fs/promises')
  const line = JSON.stringify(cmd) + '\n'

  // 服务端 nMaxInstances = 1、一个连接服务一条命令就重建实例。连着发第二、第三条
  // 时，上一实例可能还没重建完，open 会拿到 EBUSY / ENOENT。等一下再试就好，
  // 这属于正常排队，不是失败。
  const RETRY_CODES = new Set(['EBUSY', 'ENOENT', 'EAGAIN', 'EPIPE'])
  const MAX_TRIES = 12

  return (async () => {
    let lastErr = '未知错误'
    for (let attempt = 0; attempt < MAX_TRIES; attempt++) {
      let handle
      try {
        handle = await fs.open(PIPE_NAME, 'w')
      } catch (err) {
        lastErr = err.code || err.message
        if (!RETRY_CODES.has(lastErr)) return { ok: false, error: lastErr }
        await new Promise((r) => setTimeout(r, 8 + attempt * 4))
        continue
      }
      try {
        await handle.write(line)
        return { ok: true }
      } catch (err) {
        lastErr = err.code || err.message
        if (!RETRY_CODES.has(lastErr)) return { ok: false, error: lastErr }
      } finally {
        try {
          await handle.close()
        } catch {
          /* 对端已经收了数据并断开，关不关都一样 */
        }
      }
      await new Promise((r) => setTimeout(r, 8 + attempt * 4))
    }
    return { ok: false, error: lastErr }
  })()
}

// ---------------------------------------------------------------------------
// 配置文件的读写
//
// 权威定义在 native/backend/Profiles.h 的 ProfileDoc / ProfileEntry，
// 这里只做**形状校验 + 补默认值**，字段名一律以 C++ 侧为准。
// 两边不一致的表现是服务启动后报 profiles_load_failed，所以宁可这边先拦。
// ---------------------------------------------------------------------------
const PROFILE_SOURCE_KINDS = new Set(['desktop', 'monitor', 'window'])
const MATCH_FIELDS = new Set(['process', 'title', 'class'])
const MATCH_MODES = new Set(['exact', 'contains', 'prefix', 'regex'])
// 匹配方式（四个显式选项）：应用程序永远是必要条件，其余字段是叠加条件（AND）。
// 和 native/backend/Profiles.h 的 MatchRule、frontend 的 MATCH_RULE_OPTIONS
// 必须是同一套字符串，三处任何一处写错都会让"界面选了 A、服务按 B 跑"。
const MATCH_RULES = new Set(['process', 'title+process', 'class+process', 'title+class+process'])
const MATCH_RULE_FIELDS = {
  process: ['process'],
  'title+process': ['title', 'process'],
  'class+process': ['class', 'process'],
  'title+class+process': ['title', 'class', 'process'],
}
// 「同时命中多个窗口时挑哪个」的候选值已经不在界面上了（固定 largest），
// 这里不再保留 MATCH_PICKS —— 留着一个没人用的集合只会让人以为还能配置。

/**
 * 老配置没有 rule 键时推断一档。
 *
 * 【三处必须一致】native/backend/Profiles.cpp 的 WindowMatch::Normalize、
 * frontend/src/App.vue 的 deriveMatchRule、以及这里。任何一处不一致，用户就会
 * 遇到"界面显示按名字+应用程序、服务却按只匹配应用程序在跑"这种鬼故事。
 */
function deriveMatchRule(m) {
  const given = String(m?.rule ?? '').trim()
  if (MATCH_RULES.has(given)) return given
  const order = Array.isArray(m?.byOrder) && m.byOrder.length ? m.byOrder : [String(m?.by ?? 'title')]
  const primary = String(order[0] ?? 'title')
  if (primary === 'process') return 'process'
  if (primary === 'class') return 'class+process'
  if (String(m?.title ?? '').trim()) return 'title+process'
  if (String(m?.className ?? '').trim()) return 'class+process'
  return 'process'
}


function profilesPath() {
  // --gui-export 默认拿仓库里那份 profiles.json：界面上"看得见的配置"必须和
  // 服务真正加载的是同一份，否则验收测的是另一套东西。验收脚本会用
  // SHADOW_PROFILES 指到一份回放时长短得多的副本，免得为了攒几段等太久。
  return process.env.SHADOW_PROFILES || path.join(PROJECT_ROOT, 'profiles.json')
}

/**
 * 把界面给的配置整理成 C++ 侧要的形状。
 *
 * 字段名以 native/backend/Profiles.cpp 的解析器为准（它只认下面这些）：
 *   source: { kind: desktop|monitor|window, monitorIndex, clientAreaOnly }
 *   video:  { fps, bitrateMbps, replaySeconds, segmentSeconds }
 *   match:  { by: process|title|class, mode, pattern, pick, pollMs }  ← 顶层，不在 source 里
 *
 * 曾经把 match 塞进 source 里，服务直接解析不到匹配规则 —— 表现是
 * 窗口源永远"匹配不到窗口"，而配置看起来完全正常。所以这里特意做形状校验。
 */
function normalizeProfiles(payload) {
  const src = payload && typeof payload === 'object' ? payload : {}
  const rawList = Array.isArray(src.profiles) ? src.profiles : []
  const profiles = rawList.map((p, i) => {
    const kind = pick(p?.source?.kind, PROFILE_SOURCE_KINDS, 'desktop')
    const entry = {
      id: String(p?.id || `p-${i + 1}`),
      // 数字编号：录像目录就是 records/<num>。
      //
      // 这个号由界面在新建配置时分配、之后永不改（改名字、换源类型都不动），
      // 这样目录名和配置一一对应。0 / 缺失时下面统一补齐，所以老配置文件
      // 不需要手工迁移 —— 补出来的号和 C++ 侧补齐的算法一致（都从 1 往上找
      // 第一个空位），两边算出来是同一个号。
      num: Math.round(clampNum(p?.num, 0, 9999, 0)),
      name: String(p?.name || `配置 ${i + 1}`),
      enabled: p?.enabled === true,
      source: {
        kind,
        monitorIndex: Math.round(clampNum(p?.source?.monitorIndex, 0, 15, 0)),
        clientAreaOnly: p?.source?.clientAreaOnly === true,
        // wgc 必须原样带过去：C++ 侧 Profiles.cpp:695 的 Flag(src,"wgc",true)
        // 只在 source 里有这个键时读它，这里丢掉就等于每存一次都把配置里的
        // wgc 值抹回默认 —— 关掉 WGC 的配置保存一次就又被打开了。
        wgc: p?.source?.wgc !== false,
      },
      video: {
        // 上限跟录制侧一致（120）；老配置里手写的 240 在这里就被夹回来，
        // 不用等服务端 MfEncoder::kMaxFps 兜底（那样界面显示的还是 240）。
        fps: Math.round(clampNum(p?.video?.fps, 1, MAX_FPS, 60)),
        bitrateMbps: Math.round(clampNum(p?.video?.bitrateMbps, 1, 200, 20)),
        replaySeconds: Math.round(clampNum(p?.video?.replaySeconds, 5, 3600, 120)),
        segmentSeconds: Math.round(clampNum(p?.video?.segmentSeconds, 1, 60, 2)),
        // 缩放同样要透传。界面上已经没有这两个输入框了，但老配置文件里可能
        // 还留着非 0 的值；这里不透传的话，一次保存就会把它们悄悄清成 0，
        // 用户的录像尺寸在不知情的情况下变了。
        scaleWidth: Math.round(clampNum(p?.video?.scaleWidth, 0, 7680, 0)),
        scaleHeight: Math.round(clampNum(p?.video?.scaleHeight, 0, 4320, 0)),
      },
      createdAt: String(p?.createdAt || new Date().toISOString()),
    }
    if (kind === 'window') {
      // 匹配方式：从界面下拉来的一档（四个取值之一），老配置按首选字段推断。
      //
      // 以前这里写的是"标题 › 类名 › exe"三档降级顺序，那个模型会让标题一
      // 变就降到类名、进而录到同类的另一个窗口，已经整个换掉了。
      const rule = deriveMatchRule(p?.match)
      entry.match = {
        rule,
        // by 只为"不认识 rule 的老服务"留个能读的键
        by: pick(p?.match?.by, MATCH_FIELDS, MATCH_RULE_FIELDS[rule][0]),
        mode: pick(p?.match?.mode, MATCH_MODES, 'contains'),
        pattern: String(p?.match?.pattern ?? ''),
        // 同时命中多个窗口时一律取面积最大的那个 —— 界面上已经没有这个选项，
        // 写死在这里，免得老配置里残留的 foreground/first/oldest 继续生效。
        pick: 'largest',
        pollMs: Math.round(clampNum(p?.match?.pollMs, 100, 60000, 1000)),
        // 只匹配可见窗口：开关已从界面删掉，固定 true（最小化/隐藏的窗口不录）。
        requireVisible: true,
        // 用户从下拉里挑窗口时记下的三份指纹。C++ 侧按 rule 决定用其中哪几份，
        // 全部 AND —— 少一份就匹配不上，这一路保持未采集。
        title: String(p?.match?.title ?? ''),
        className: String(p?.match?.className ?? ''),
        process: String(p?.match?.process ?? ''),
        // 挑中的那个窗口（偏好，不参与条件比较）。必须透传：丢掉它的话，每次
        // 保存都会把"用户选的是同一个程序的哪个窗口"抹掉，只匹配应用程序时
        // 又退回"永远录面积最大的那个"。
        // 一律存无符号 32 位（和 C++ 侧 BuildWindowsJson 的输出、AsU32 的读取
        // 范围一致）：这个范围内 JS 的 Number 是精确的，不会在 JSON 里丢精度。
        hwnd: Math.min(4294967295, Math.max(0, Math.trunc(Number(p?.match?.hwnd ?? 0)) || 0)),
      }
    }
    return entry
  })
  // 补齐缺失的数字编号：先收集已用掉的号，再从 1 往上找第一个空位。
  //
  // 和 native/backend/Profiles.cpp 加载时那段补齐**必须一致**：服务端读到
  // num=0 时也算同一个号，否则界面写进 json 的号会和实际录像目录对不上。
  {
    const used = new Set(profiles.map((p) => p.num).filter((n) => n > 0))
    let next = 1
    for (const p of profiles) {
      if (p.num > 0) continue
      while (used.has(next)) next += 1
      p.num = next
      used.add(next)
      next += 1
    }
  }
  return {
    version: 1,
    recordRoot: expandEnvPath(src.recordRoot) || TEMP.records,
    exportDir: expandEnvPath(src.exportDir) || EXPORT_DIR,
    profiles,
  }
}

/**
 * 展开路径里的环境变量（`%TEMP%` / `${TEMP}`）。
 *
 * 为什么 Node 这边必须自己展开一遍：配置文件（profiles.json）是跟着仓库
 * 走的，里面写的是 `%TEMP%\shadow-play\app\records` 这种**占位**路径。
 * 服务端（C++ Profiles.cpp 的 ExpandEnv）读盘时会展开，可主进程这条链
 * 是直接 `JSON.parse(文件)` 拿到原始字符串就去用了 —— `%TEMP%` 在 Node
 * 里不是路径，`fs.mkdirSync` 会把它当成名字就叫 `%TEMP%` 的**相对目录**，
 * 于是在仓库根建出一个`%TEMP%`文件夹，里头的导出目录是空的。
 * （实际踩过：仓库根多了个 `%TEMP%\shadow-play\app\exports`。）
 *
 * 展开不了的变量原样保留，这样写错名字能一眼看出来。
 */
function expandEnvPath(p) {
  const raw = String(p ?? '').trim()
  if (!raw) return ''
  const lookup = (name) => process.env[name] ?? ''
  return raw
    .replace(/\$\{([^}]+)\}/g, (m, name) => lookup(String(name).trim()) || m)
    .replace(/%([^%]+)%/g, (m, name) => lookup(String(name).trim()) || m)
}

function pick(v, allowed, fallback) {
  const s = String(v ?? '').toLowerCase()
  return allowed.has(s) ? s : fallback
}

function clampNum(v, lo, hi, fallback) {
  const n = Number(v)
  if (!Number.isFinite(n)) return fallback
  return Math.min(hi, Math.max(lo, n))
}

// ---------------------------------------------------------------------------
// 导出模块（native/build/shadow_ring.node）
//
// 同一个 .node 文件被两个进程各加载一份：preload 用它的帧环 API 读预览，
// 主进程用它的导出 API 合并分片。两边的加载是独立的，互不影响。
// ---------------------------------------------------------------------------
let addonForExport = null
function loadAddon() {
  if (addonForExport) return addonForExport
  const p = path.join(PROJECT_ROOT, 'native', 'build', 'shadow_ring.node')
  addonForExport = require(p)
  return addonForExport
}

// ---------------------------------------------------------------------------
// 导出队列
//
// 【为什么要有队列】
//
// 插件里那个 Exporter 是**单例**：上一轮还在跑时 exportStart 直接回
// "上一轮导出还没结束"。以前界面只有一个全局的 exportBusy，A 路在导的时候
// B 路的按钮也跟着灰掉 —— 用户点了没反应，还以为坏了。
//
// 现在改成：每一路点导出只是**入队**，主进程串行地把队列里的活一个个干完；
// 界面按 profile id 各记一份状态，谁的活在跑谁自己灰自己的按钮，互不牵连。
//
// 为什么串行而不是并发：Exporter 是重 IO + Media Foundation 的重活，同时跑
// 两个只会互相拖慢，而且插件根本不支持（单例）。
// ---------------------------------------------------------------------------
/** 等待中的导出任务（不含正在跑的那个）。 */
const exportQueue = []
/** 正在跑的导出任务（null = 空闲）。 */
let exportRunningJob = null

/** 读一次插件侧的导出状态（失败不抛，返回 null）。 */
function readExportStatus() {
  try {
    return loadAddon().exportStatus?.() ?? null
  } catch (err) {
    logErr('读取导出状态失败：', err.message)
    return null
  }
}

/** 队列当前的样子（界面按 id 各取一份自己的状态）。 */
function exportQueueSnapshot() {
  const items = []
  if (exportRunningJob) {
    items.push({
      id: exportRunningJob.id,
      name: exportRunningJob.name,
      state: 'running',
      phase: exportRunningJob.phase || '导出中',
    })
  }
  for (const j of exportQueue) {
    // 排队的那几项也报"导出中"：队列顺序是内部实现，界面不展示"排队中"
    // （用户点了导出就该看到导出中，而不是"排第 3 位"）。
    items.push({ id: j.id, name: j.name, state: 'queued', phase: '导出中' })
  }
  return items
}

/** 广播一条事件给所有窗口，同时记进主进程的事件历史（自检要用）。 */
function broadcastServiceEvent(payload) {
  recordExportEvent(payload)
  for (const w of BrowserWindow.getAllWindows()) {
    try {
      w.webContents.send('service:event', payload)
    } catch {
      /* 窗口正在关 */
    }
  }
}

/** 队列有任何变化就整份播一次：界面据此决定每个导出按钮的禁用。 */
function broadcastExportQueue() {
  const items = exportQueueSnapshot()
  broadcastServiceEvent({
    type: 'export_queue',
    items,
    runningId: exportRunningJob?.id ?? null,
    queuedCount: exportQueue.length,
    at: Date.now(),
  })
}

/**
 * 播一次导出进度/结果。`st` 是插件 exportStatus() 的原始对象（可以只给部分字段）。
 *
 * 为什么要带 `id`：队列里可能排着好几路，界面必须知道这条进度属于**哪一路**，
 * 否则又回到"一个全局导出中把所有人一起灰掉"的老毛病。
 */
function broadcastExportStatus(job, st) {
  if (job && st && st.phase) job.phase = st.phase
  broadcastServiceEvent({
    type: 'export_status',
    id: job.id,
    name: job.name,
    running: st?.running === true,
    ok: st?.ok === true,
    failed: st?.failed === true,
    phase: st?.phase ?? '',
    outPath: st?.outPath ?? '',
    detail: st?.detail ?? '',
    // 两个名字都写出去：`segmentCount` 是插件那边的原始字段名（ExportStatusToJs
    // 报的就是它），`segments` 是服务经 stdout 发 export_result 时用的名字。
    segmentCount: st?.segmentCount ?? 0,
    segments: st?.segments ?? st?.segmentCount ?? 0,
    durationSeconds: st?.durationSeconds ?? 0,
    bytes: st?.bytes ?? 0,
    elapsedMs: st?.elapsedMs ?? 0,
    jobId: st?.jobId ?? 0,
    plan: st?.plan ?? '',
    queuedCount: exportQueue.length,
    at: Date.now(),
  })
}

/**
 * 跑一次导出任务：收尾分片 → exportStart → 等收尾。
 *
 * 这一段原来就是 `service:export` 的主体。改成任务形式之后唯一的区别是：
 * 结果不再 `return` 给 IPC 调用方（那时用户点一次就同步等结果），而是按
 * profile id 广播 `export_status` —— 队列里排着好几路，每路的结果只能分开通知。
 */
async function runExportJob(job) {
  const id = job.id
  const failJob = (detail) => {
    broadcastExportStatus(job, {
      running: false,
      ok: false,
      failed: true,
      phase: '失败',
      detail,
      outPath: '',
    })
    logErr(`[export] ${id} 失败：${detail}`)
  }

  const doc = loadProfiles()
  const profile = (doc.profiles ?? []).find((p) => p.id === id)
  if (!profile) {
    failJob(`配置里没有 id = ${id} 这一路。`)
    return
  }

  // 分片目录：优先用服务在状态文件里报的 recordDir（那是它真正在写的目录）。
  const m = readStatusFile()
  const liveSession = Array.isArray(m.doc?.items) ? m.doc.items.find((s) => s.id === id) : null
  const recordDir = liveSession?.recordDir || ''
  if (!recordDir) {
    failJob(
      serviceChild || m.doc
        ? '这一路还没有开始录制，没有可导出的分片。'
        : '采集服务没有在运行，也没有可导出的分片。',
    )
    return
  }

  // 回放时长：界面点导出时带过来的那个（用户当时看到的值），没有再退回配置。
  const maxSeconds =
    job.seconds > 0 ? Math.round(job.seconds) : Number(profile.video?.replaySeconds) || 0
  // 再展开一次：doc.exportDir 在 normalizeProfiles 里已经展开过，但导出接口
  // 也可能从别的地方（旧配置、手工调用）拿到带占位符的串，兜一层不吃亏。
  const exportDir = expandEnvPath(doc.exportDir) || EXPORT_DIR
  const prefix = profile.name || profile.id || ''

  let addon
  try {
    addon = loadAddon()
  } catch (err) {
    failJob(`加载导出模块失败：${err.message}`)
    return
  }
  if (typeof addon.exportStart !== 'function') {
    failJob('导出模块里没有 exportStart，请重新执行 npm run build:native。')
    return
  }

  try {
    // 【第一步：让服务把正在写的那一片收尾落盘】
    //
    // 这一步不能省。分片是按固定片长切的，点导出的那一刻总有一片正在写：
    // 它没 Finalize、index.csv 里也没有它，导出端只能读到最后一片**已完成**
    // 的分片 —— 于是"点按钮之前那 1~2 秒"永远导不出来。用户报的就是这个
    // （m12714：「请你回放导出的视频一直到点击按钮的那一刻才停止，我看现在
    // 导出的视频都是点击前一秒或两秒就停止了」）。
    //
    // 【为什么必须发命令，而不是在这里直接切】导出走的是插件
    // （addon.exportStart，在 Electron 主进程里跑），服务完全不知道有这回事，
    // 所以 Session::ExportNow 里那句 FlushSegment() 从来没被执行过。只有
    // 服务自己知道"这一路现在写到哪一片"，切片的动作只能它来做。
    //
    // 失败不影响导出：服务没起 / 这一路没在录 / 等超时，都只是"没切成"，
    // 导出端照旧拿已完成的分片，最多还是丢那一小段（和以前一样）。
    //
    // 【发完必须等它真的切完，否则前面这些全白做】
    //
    // 命令管道是单向的（`PIPE_ACCESS_INBOUND`），服务没法回话；`pipeSend`
    // 写完就返回 ok。第一版就是"发完直接往下走"，实测（test-export 的
    // export_plan）里 `segs` 仍然只到 flush 之前的那一片 —— 导出端在服务
    // 还没开始 Finalize 时就把目录扫完了，那一片照样读不到。
    //
    // 回执走状态文件里的 `flushSeq`（Session::FlushRecording 成功一次 +1）：
    // 它变了才说明"点按钮那一刻"的分片真的落盘了。比让主进程去猜
    // index.csv 路径、数行数、比 mtime 都稳 —— 那几种在槽位复用和轮次
    // 目录切换时都会误判。
    broadcastExportStatus(job, { running: true, phase: '收尾分片' })
    if (serviceChild || serviceRunningFast()) {
      const before = readStatusFile()?.doc?.items?.find((s) => s.id === id)?.flushSeq ?? 0
      const flushed = await pipeSend({ cmd: 'flush', id })
      if (!flushed?.ok) {
        logErr(`[export] 收尾分片失败（${flushed?.error ?? '未知'}），照旧导出已完成的分片。`)
      } else {
        // 最多等 3 秒：FlushSegment 内部超时就是 2 秒（编码器彻底卡住时），
        // 再留 1 秒给状态文件的写盘节拍。
        const flushDeadline = Date.now() + 3000
        let seen = before
        while (Date.now() < flushDeadline) {
          const now = readStatusFile()?.doc?.items?.find((s) => s.id === id)?.flushSeq ?? 0
          if (now !== before) {
            seen = now
            break
          }
          await new Promise((r) => setTimeout(r, 40))
        }
        if (seen === before) {
          logErr('[export] 等了 3 秒也没看到 flush 完成（服务可能没在录这一路），照旧导出已完成的分片。')
        } else {
          log(`[export] 分片已收尾（flushSeq ${before} → ${seen}），开始导出。`)
        }
      }
    }

    // Exporter 要的是"分片所在目录"，服务报的 recordDir 是配置目录，
    // 分片在它下面的 segments/ 里（见 SegmentRecorder.h）。
    const segmentsDir = path.join(recordDir, 'segments')

    // **把这一路当前的帧率一起传下去**。
    //
    // 分片里存的是"录的那一刻的帧率"。用户把某一路从 60fps 改成 240fps 之后，
    // 已经录好的那批分片仍是 60fps；不传帧率的话 Exporter 只会 `-c copy` 直拼，
    // 导出来的回放就是 60fps —— 用户看到的是"我明明改了，导出来还是老样子"。
    //
    // 传了之后 Exporter 会按目标帧率重采样：目标更低就抽帧，更高就插帧补齐，
    // 时间轴始终按墙钟对齐，不会变快变慢。0/缺省表示保持源帧率。
    const wantFps = Math.round(Number(profile.video?.fps) || 0)
    // 单片标称时长：导出要靠它判"最后一片写完了没"。不传就按 2 秒，而配置里
    // 这个值能改 —— 改大了会把完整的 2 秒片误判成半片丢掉，成品就短一截。
    const segSecs = Math.round(Number(profile.video?.segmentSeconds) || 0) || 2
    const res = addon.exportStart(segmentsDir, exportDir, prefix, maxSeconds, wantFps, segSecs)
    if (!res?.ok) {
      failJob(res?.error ?? '导出没能开始。')
      return
    }
    log(`[export] ${id} 开跑：${segmentsDir} → ${exportDir}（${maxSeconds} 秒 / ${wantFps}fps）`)

    // 【第二步：等它收尾】
    //
    // 导出跑在插件的后台线程里，进度只能靠 exportStatus() 轮询。这里顺手把
    // 进度按 400ms 一次广播给界面（和以前那条全局轮询同样的节奏，区别是这次
    // 带上了 profile id）。
    //
    // 超时给到 15 分钟：正常 20 秒成片不到一分钟，这个上限只是防"永远等下去"。
    const deadline = Date.now() + 15 * 60 * 1000
    let st = null
    let lastEmit = 0
    while (Date.now() < deadline) {
      st = readExportStatus()
      if (!st) {
        failJob('读不到导出状态（导出模块里没有 exportStatus）。')
        return
      }
      if (st.running !== true) break
      if (Date.now() - lastEmit > 400) {
        lastEmit = Date.now()
        broadcastExportStatus(job, st)
      }
      await new Promise((r) => setTimeout(r, 100))
    }
    if (!st || st.running === true) {
      failJob('导出超时（15 分钟还没结束）。')
      return
    }
    broadcastExportStatus(job, st)
    if (st.ok) {
      const mb = ((st.bytes ?? 0) / 1024 / 1024).toFixed(1)
      log(`[export] ${id} 完成：${st.outPath}（${(st.durationSeconds ?? 0).toFixed(1)} 秒 / ${mb} MB）`)
    } else {
      logErr(`[export] ${id} 失败：${st.detail ?? '原因不明'}`)
    }
  } catch (err) {
    failJob(`发起导出失败：${err.message}`)
  }
}

/** 队列推进：空闲且有人排队就取一个跑，跑完自动接下一个。 */
async function pumpExportQueue() {
  if (exportRunningJob || exportQueue.length === 0) return
  const job = exportQueue.shift()
  job.phase = '准备'
  exportRunningJob = job
  broadcastExportQueue()
  try {
    await runExportJob(job)
  } catch (err) {
    logErr(`[export] ${job.id} 抛错：`, err?.message ?? String(err))
    broadcastExportStatus(job, {
      running: false,
      ok: false,
      failed: true,
      phase: '失败',
      detail: `导出出错：${err?.message ?? err}`,
      outPath: '',
    })
  } finally {
    exportRunningJob = null
    broadcastExportQueue()
    // 下一轮放到下一个事件循环里跑：让广播先送到界面，也避免 finally 里
    // 直接 await 形成一条很长的调用链。
    if (exportQueue.length > 0) {
      setTimeout(() => void pumpExportQueue(), 80).unref?.()
    }
  }
}

/**
 * 最近一次**成功**从磁盘读到的配置。
 *
 * 存在的理由是防"空数组把配置冲掉"：界面拿不到配置时（菜单还没加载完、
 * IPC 一时失败、文件正在被写）它的 profiles 是空数组，此时用户点一下
 * 任意开关都会把 [] 整体存回去，磁盘上那三路配置就没了。有了这份快照，
 * 保存时发现"新配置是空数组、而上一秒磁盘上明明有货"就直接拒绝。
 */
let lastGoodProfiles = null

function loadProfiles() {
  const file = profilesPath()
  if (!existsSync(file)) return { version: 1, profiles: [] }
  try {
    const { readFileSync } = require('node:fs')
    const doc = normalizeProfiles(JSON.parse(readFileSync(file, 'utf8')))
    lastGoodProfiles = doc
    return doc
  } catch (err) {
    logErr('读取配置失败：', err.message)
    return { version: 1, profiles: [], error: err.message }
  }
}

/**
 * 重启服务 —— **故意不实现，不要加回来**。
 *
 * 这里原来有一个 `restartService()`：taskkill 掉所有 shadow-capture.exe 再拉一个。
 * 保存配置时拿它当兜底，代价是所有分路的录制一起断、回放缓冲清空、主进程被
 * 同步等待卡住 3 秒。用户的要求很明确：**任何功能都不许重启服务，必须实时更新**。
 *
 * 现在所有"改了要生效"的动作都走命名管道（reload / set-preview-fps 等），
 * 服务端收到就地下发、下一拍生效，不需要重启。真要重启，只能由用户在命令行
 * 自己执行 npm run service:stop + npm run service —— 那是他的选择，不是程序
 * 偷偷替他做的。
 */
/**
 * 停掉整套（服务 + 守护），**先礼后兵**。
 *
 * 三步，顺序不能乱：
 *   1) 先立「停机哨兵」—— 守护每 2 秒巡检一次，不立的话杀掉服务的瞬间它就
 *      把服务拉起来，「停止」永远停不干净。
 *   2) 再请服务**自己优雅退出**（管道发 {"cmd":"shutdown"}）—— 它会把每个
 *      会话的 Stop() 跑完：待喂队列里已经采到的帧补写进编码器、正在写的那
 *      个分片 Finalize（没有这一步，最后那个分片是读不出来的半截文件）。
 *   3) 等不到才 taskkill 兜底。
 *
 * 【为什么非要这一步】以前只有 1 和 3，直接 taskkill /F 把进程当场打死，
 * Session::Stop() 根本没机会跑。用户在设置页点「重启服务」时，最后一两秒的
 * 分片就是这么丢的，而且画面在点按钮**之前**就断了（磁盘上的内容停在更早的
 * 时刻）。用户的原话：「我应该把一直录到重启服务那一刻才出现 no signal」。
 */
async function stopServiceGracefully() {
  const { execFileSync, spawnSync } = require('node:child_process')
  // 1) 立哨兵
  try {
    writeFileSync(path.join(TEMP_ROOT, 'status', 'shutdown-requested'), '', 'utf8')
  } catch {}

  // 2) 优雅退出
  //
  // 最多等 5 秒：Session::Stop() 要等喂帧线程退出、把队列里剩下的帧补写完、
  // 再把当前分片 Finalize（每路 0.1~0.5 秒），多路加起来可能要一两秒。这点
  // 等待换来的是"最后一个分片完整可读"，值得。
  let graceful = false
  try {
    await sendServiceCommand({ cmd: 'shutdown' })
    for (let i = 0; i < 25; i++) {
      if (serviceRunning() !== true) {
        graceful = true
        break
      }
      await new Promise((r) => setTimeout(r, 200))
    }
  } catch (err) {
    log('[service:stop] 优雅停机请求失败，改用强杀：', err?.message ?? err)
  }

  // 3) 循环强杀兜底（优雅退出成功的话不必再白等 15 秒）
  const deadline = Date.now() + (graceful ? 3000 : 15000)
  let everKilled = false
  const guardPid = (() => {
    try {
      const doc = JSON.parse(readFileSync(path.join(TEMP_ROOT, 'status', 'guardian-v1.json'), 'utf8'))
      const p = Number(doc?.pid)
      return Number.isFinite(p) && p > 0 ? p : null
    } catch {
      return null
    }
  })()
  const pids = new Set([serviceChild?.pid, guardPid].filter((p) => p))
  while (Date.now() < deadline) {
    for (const p of pids) {
      try {
        execFileSync('taskkill', ['/PID', String(p), '/F'], { stdio: 'ignore', windowsHide: true })
        everKilled = true
      } catch {}
    }
    try {
      const byName = spawnSync('taskkill', ['/IM', 'shadow-capture.exe', '/F'], {
        stdio: 'ignore',
        windowsHide: true,
      })
      if (byName.status === 0) everKilled = true
    } catch {}
    // 都清干净了就收工
    const r = spawnSync('tasklist', ['/FI', 'IMAGENAME eq shadow-capture.exe'], {
      encoding: 'utf8',
      windowsHide: true,
    })
    if (!/shadow-capture\.exe/i.test(r.stdout || '')) break
    await new Promise((r) => setTimeout(r, 800))
  }
  // 4) 停机完成，清掉哨兵，方便下次启动
  try {
    rmSync(path.join(TEMP_ROOT, 'status', 'shutdown-requested'), { force: true })
  } catch {}
  const stillRunning = serviceRunning() === true
  return { ok: !stillRunning, everKilled, stillRunning, graceful }
}

function stopService() {
  try {
    const { execFileSync } = require('node:child_process')
    // 优先只杀**本进程启动的那一个**（按 pid）。
    //
    // 以前一律用 `taskkill /IM shadow-capture.exe /F`，那是按映像名匹配 ——
    // 机器上要是有另一份配置起的采集服务（另一个工作目录、另一份
    // profiles.json），它也一起被杀了。这正是"影响隔壁分路"的一种形态。
    // 有 pid 就精确杀；没有（服务不是本界面起的）才退回按名字，并且界面
    // 那边会明确提示"会重启整个服务"。
    if (serviceChild?.pid) {
      execFileSync('taskkill', ['/PID', String(serviceChild.pid), '/T', '/F'], {
        stdio: 'ignore',
        windowsHide: true,
      })
    } else {
      execFileSync('taskkill', ['/IM', 'shadow-capture.exe', '/F'], {
        stdio: 'ignore',
        windowsHide: true,
      })
    }
    return true
  } catch {
    // 进程本来就不在 → taskkill 返回非 0，这不算失败
    return true
  }
}

/**
 * 列出可采集的顶层窗口。
 *
 * 服务是常驻进程，直接读它的 stdout 会把状态流搅乱，所以起一个**临时实例**
 * 带 --list-windows 跑完就退；它只枚举窗口、不建环，不会和常驻实例抢资源。
 */
function listWindows() {
  // 用的就是常驻服务那一个 exe（不再有"临时副本"这回事，见 SERVICE_EXE 那段
  // 注释）。起一个带 --list-windows 的临时实例跑完就退，它只枚举窗口、不建环，
  // 不会和常驻实例抢资源；因为是同一个文件，刚编译出来的新参数（--include-hwnd
  // 之类）立即生效，不存在"副本没刷新所以用不上"的情况。
  const exe = SERVICE_EXE
  if (!existsSync(exe)) {
    return { ok: false, error: `找不到采集服务：${exe}`, items: [] }
  }
  // 服务正在录的窗口要无条件进列表。
  //
  // 枚举为了不列垃圾窗口，排掉了最小化 / 小于 64x64 的窗口。可窗口被最小化
  // 之后服务**仍然握着那个 hwnd 继续录**，于是下拉里没有它，"按当前正在录制
  // 的窗口自动选中"就落空了。把这些 hwnd 交给枚举进程强制收录，下拉里就
  // 一定找得到（最小化/尺寸过小的过滤对它们不生效）。
  // 镜像每秒才刷一次，刚启动时还是空的 —— 那就自己读一遍状态文件，
  // 否则第一次打开编辑面板会漏掉正在录的窗口。
  let sessions = Array.isArray(live.mirrorSessions) ? live.mirrorSessions : []
  if (!sessions.length) {
    try {
      const m = readStatusFile()
      if (Array.isArray(m.doc?.items)) sessions = m.doc.items
    } catch {
      /* 读不到就算了，退化成"不强制收录" */
    }
  }
  const includeHwnds = []
  for (const s of sessions) {
    const h = Number(s?.hwnd ?? 0)
    if (Number.isFinite(h) && h > 0) includeHwnds.push(h)
  }
  const args = ['--list-windows']
  for (const h of includeHwnds) args.push('--include-hwnd', String(h))
  try {
    const { execFileSync } = require('node:child_process')
    const out = execFileSync(exe, args, {
      cwd: PROJECT_ROOT,
      encoding: 'utf8',
      windowsHide: true,
      maxBuffer: 8 * 1024 * 1024,
      timeout: 15000,
    })
    const items = []
    for (const line of out.split(/\r?\n/)) {
      const t = line.trim()
      if (!t.startsWith('{')) continue
      try {
        const obj = JSON.parse(t)
        // 服务端 --list-windows 吐两种行：
        //   1) 表头 {"type":"windows","count":N}
        //   2) 每个窗口一行，**没有 type 字段**，靠 hwnd 认
        //      例：{"hwnd":198460,"title":"...","class":"...","process":"...","pid":1,...}
        // 这里原来只认 obj.type === 'window'（服务端从不写这个值）和
        // obj.type === 'windows' 里的 items 数组（服务端也不发数组），
        // 于是无论有几个窗口都解析出 0 个 —— 下拉框永远空着。
        if (obj && obj.hwnd !== undefined) items.push(obj)
        else if (obj.type === 'windows' && Array.isArray(obj.items)) items.push(...obj.items)
      } catch {
        /* 半行/非 JSON 行直接跳过 */
      }
    }
    return { ok: true, items }
  } catch (err) {
    return { ok: false, error: err.message, items: [] }
  }
}

/**
 * 列出当前已连接的显示器。
 *
 * 跟 listWindows 一样走「临时实例 + --list-monitors」，序号与
 * ScreenCapture::Init(outputIndex) 是同一套排列（只数已连接的输出，
 * 按上→下、左→右排），所以下拉框里选第几个，服务就录第几个。
 */
function listMonitors() {
  if (!existsSync(SERVICE_EXE)) {
    return { ok: false, error: `找不到采集服务：${SERVICE_EXE}`, items: [] }
  }
  try {
    const { execFileSync } = require('node:child_process')
    const out = execFileSync(SERVICE_EXE, ['--list-monitors'], {
      cwd: PROJECT_ROOT,
      encoding: 'utf8',
      windowsHide: true,
      maxBuffer: 1024 * 1024,
      timeout: 15000,
    })
    const items = []
    for (const line of out.split(/\r?\n/)) {
      const t = line.trim()
      if (!t.startsWith('{')) continue
      try {
        const obj = JSON.parse(t)
        if (obj.type === 'monitor') items.push(obj)
      } catch {
        /* 半行/非 JSON 行直接跳过 */
      }
    }
    return { ok: true, items }
  } catch (err) {
    return { ok: false, error: err.message, items: [] }
  }
}

// ---------------------------------------------------------------------------
// 窗口
// ---------------------------------------------------------------------------
let mainWindow = null

/**
 * 界面产物缺失时自己补一次构建。
 *
 * 为什么要有这一步：dist/ 是**构建产物**，被清掉的理由多得是 —— 临时目录被
 * 清理软件扫了、换了 SHADOW_TEMP_ROOT、上次只跑了 build:native、git clean 顺手
 * 清了机器上的东西。以前这种情况界面直接弹一个「请先执行 npm run build:web」
 * 的框就退场了，用户看到的是"界面找不到"，明明心里想的是"我双击一下就能用"。
 *
 * 这里用 Electron 自己当 Node 跑构建脚本（ELECTRON_RUN_AS_NODE=1），不依赖
 * 系统 PATH 里有没有 node —— 打包成绿色版之后 PATH 里通常没有。
 *
 * 失败**不抛**：构建不成功就把原因返回给调用方，让它照旧弹出原来的提示，
 * 至少用户能看到真正发生了什么。
 *
 * @returns {{ok: boolean, error?: string, log?: string}}
 */
function buildWebNow() {
  const script = path.join(PROJECT_ROOT, 'scripts', 'build-web.mjs')
  if (!existsSync(script)) return { ok: false, error: `找不到构建脚本：${script}` }
  log('界面产物缺失，先自动构建一次…')
  try {
    const r = spawnSync(process.execPath, [script], {
      cwd: PROJECT_ROOT,
      // 让 electron.exe 退化成纯 Node 跑脚本（见 scripts/launch.mjs 顶部说明）
      env: { ...process.env, ELECTRON_RUN_AS_NODE: '1' },
      encoding: 'utf8',
      windowsHide: true,
      timeout: 180000,
    })
    if (r.error) return { ok: false, error: r.error.message }
    const out = `${r.stdout ?? ''}${r.stderr ?? ''}`.trim()
    if (r.status !== 0) {
      return { ok: false, error: `构建脚本退出码 ${r.status}`, log: out }
    }
    if (!existsSync(path.join(TEMP_ROOT, 'dist', 'index.html'))) {
      return { ok: false, error: '构建脚本成功了，但产物还是不存在', log: out }
    }
    log('界面产物构建完成。')
    return { ok: true, log: out }
  } catch (err) {
    return { ok: false, error: err.message }
  }
}

function createWindow() {
  mainWindow = new BrowserWindow({
    width: 1360,
    height: 860,
    backgroundColor: '#07090f',
    show: true,
    autoHideMenuBar: true,
    title: 'ShadowPlay 预览',
    webPreferences: {
      preload: path.join(__dirname, 'preload.cjs'),
      // contextIsolation 必须关掉。
      //
      // 原因：C++ 采集端把帧放在共享内存里，preload 通过 N-API 把最新一帧
      // 同步进一块 V8 ArrayBuffer。这块内存要以**零拷贝**方式交给 WebGL，
      // 但 contextBridge 对返回值做结构化克隆 —— ArrayBuffer 会被整块拷一份
      // （59MB！），主世界拿到的是**死快照**，界面就只画出第一帧。
      // MessagePort transfer 也走不通：经 contextBridge 传过去的 MessagePort
      // 只剩一个空对象（postMessage 是 undefined），没法交接所有权。
      //
      // 关掉之后 preload 与页面在同一个 JS 世界里，页面直接拿到 preload 的
      // ArrayBuffer，读到的一直是同步泵正在写入的那块内存。
      //
      // 风险评估：本窗口只加载仓库里的本地 dist/index.html，不加载任何远程
      // 内容，也没有 nodeIntegration（页面拿不到 require）。因此这层隔离的
      // 实际收益很低，而它是实时预览能不能成立的前提。
      contextIsolation: false,
      nodeIntegration: false,
      // preload 需要 require 原生扩展
      sandbox: false,
      backgroundThrottling: false,
    },
  })

  mainWindow.on('closed', () => {
    mainWindow = null
  })

  const indexPath = path.join(TEMP_ROOT, 'dist', 'index.html')
  if (IS_DEV) {
    // 只有显式开发模式（npm run dev）才连 dev server。
    mainWindow.loadURL(DEV_URL).catch((err) => {
      logErr('加载开发服务器失败：', err.message)
      if (existsSync(indexPath)) mainWindow.loadFile(indexPath)
    })
  } else if (existsSync(indexPath)) {
    mainWindow.loadFile(indexPath)
  } else {
    // 生产模式**不**去连 127.0.0.1:5173。
    //
    // 以前这里会先 loadURL(DEV_URL)，连不上才弹框。问题是一旦 dist 缺了，
    // 用户看到的是一句 ERR_CONNECTION_REFUSED（看着像网络问题），真正的
    // 原因藏在后面 —— 而且非开发模式本来就不该去找一个没在跑的 dev server。
    logErr('找不到界面产物：' + indexPath)

    // 别急着弹「你自己去 npm run build:web」：dist 就是构建产物，删掉能重建，
    // 这里自己补一次。网络无关、不需要终端，双击就能回到可用状态。
    const built = buildWebNow()
    if (built.ok && existsSync(indexPath)) {
      mainWindow.loadFile(indexPath)
    } else {
      dialog.showErrorBox(
        '找不到界面',
        `缺少构建产物：\n${indexPath}\n\n自动构建也没成功：\n` +
          `${built.error ?? '(无错误信息)'}\n` +
          `${built.log ? `\n构建输出（末尾）：\n${built.log.slice(-1500)}\n` : ''}` +
          `\n请手动执行：\n  npm run build:web`,
      )
    }
  }

  return mainWindow
}

// ---------------------------------------------------------------------------
// 反向通知管道：采集服务 → 界面（\\.\pipe\ShadowPlay.Gui.v1）
//
// 【谁当服务端】**界面**。服务可能是界面拉起的，也可能是用户自己
// `npm run service` 起的，它发通知时不该去猜界面的 pid / 进程名（按名字杀进程
// 会误伤，按窗口标题找不可靠）。让界面守着一条管道，服务退出时连上写一句话，
// 界面自己走 app.quit() 正常关窗口。界面没开着时服务连不上，直接跳过
// （见 native/backend/TrayIcon.cpp 的 NotifyGuiQuit）。
//
// 现在只有一条命令：托盘「退出」时请界面一起退出。
// ---------------------------------------------------------------------------
const GUI_PIPE_NAME = '\\\\.\\pipe\\ShadowPlay.Gui.v1'
let guiPipeServer = null
/** 兜底标记的巡检定时器（主进程自己盯，见 startGuiPipe）。 */
let guiQuitTimer = null

// 兜底通道的标记文件（写入方见 native/backend/GuiNotify.cpp）。
//
// 为什么还要文件：命名管道的访问检查跟着**进程完整性级别**走。服务要是被
// 「以管理员身份运行」，它去连普通权限界面建的管道会吃 ERROR_ACCESS_DENIED，
// 界面明明开着却收不到通知。标记文件没有这个问题，代价只是最多慢 1 秒。
const GUI_QUIT_MARKER = path.join(TEMP_ROOT, 'status', 'gui-quit-requested')

// 界面启动时刻：只认这之后写的标记，避免上次运行留下的残留把新界面关掉。
const GUI_STARTED_AT = Date.now()

// ---------------------------------------------------------------------------
// 界面进程号（%TEMP%\shadow-play\app\gui.pid）
//
// 【谁在看它】采集服务的托盘（native/backend/TrayIcon.cpp 的 RequestOpenGui）。
// 托盘点一下要判断三种状态：界面在跑 / 界面正在启动 / 界面没开该拉一个。
// "在跑"靠反向通知管道就能判；但"正在启动"和"刚被用户关掉"在管道上看是
// 一模一样的（都连不上）—— 只靠时间窗口判断的话，用户在窗口里开完就叉掉、
// 再点托盘，会被当成"还在启动"而忽略。
//
// 所以这里落一个 pid 文件：写着界面主进程的 pid。托盘 OpenProcess 一下就知道
// 那个进程还在不在 —— 叉掉了（正常退出或被强杀）pid 立刻失效，下次点击就能
// 重新拉起；还在启动途中（管道还没建好）pid 是活的，就老实等它。
//
// launch.mjs 在 spawn 完 electron 会**先写一份**（省掉"进程刚起还没跑到这里"
// 的空窗），界面自己启动时再写一次（同值），退出时删掉。
// ---------------------------------------------------------------------------
const GUI_PID_FILE = path.join(TEMP_ROOT, 'gui.pid')

function writeGuiPid() {
  try {
    writeFileSync(GUI_PID_FILE, String(process.pid), 'utf8')
  } catch {
    /* 写不了不影响界面本身，最多是老服务重启后第一次点托盘要等管道 */
  }
}

function removeGuiPid() {
  try {
    // 只删自己写的：万一同时有另一份界面（单实例锁理论上不允许），别帮它清掉。
    if (readFileSync(GUI_PID_FILE, 'utf8').trim() === String(process.pid)) {
      rmSync(GUI_PID_FILE, { force: true })
    }
  } catch {
    /* 文件本来就不在 */
  }
}

/**
 * 检查（并消费）服务留下的退出标记。由主进程的定时器每 800ms 看一眼 ——
 * 不依赖页面在轮询 service:live（页面没加载出来、渲染进程崩了也得能退）。
 */
function consumeGuiQuitMarker() {
  let st = null
  try {
    st = statSync(GUI_QUIT_MARKER)
  } catch {
    return /* 文件不存在 = 常态 */
  }
  const stale = st.mtimeMs + 3000 < GUI_STARTED_AT
  try {
    rmSync(GUI_QUIT_MARKER, { force: true })
  } catch {
    /* 删不掉也无所谓，窗口马上要关了 */
  }
  if (stale) return
  log('[gui-pipe] 服务请求退出，界面一起关')
  shutdownFromService()
}

/**
 * 起管道。只在正常界面模式调用（--selftest / --gui-export 那些验收模式不需要）。
 */
function startGuiPipe() {
  if (guiPipeServer) return
  // 记下自己的 pid：托盘靠它区分"还在启动"和"已经被关掉"（见 GUI_PID_FILE）。
  writeGuiPid()
  // 先把可能残留的退出标记清掉：这一次是新界面，不该被上一次的退出请求带走。
  try {
    rmSync(GUI_QUIT_MARKER, { force: true })
  } catch {
    /* 没有就更好 */
  }
  try {
    guiPipeServer = net.createServer((sock) => {
      // 只有服务主动说话时才会进来（托盘点了「打开主界面」或退出），
      // 记一行方便排查"点了没反应"到底是没连上还是没处理。
      log('[gui-pipe] 收到连接')
      sock.setEncoding('utf8')
      let buf = ''
      sock.on('data', (chunk) => {
        buf += chunk
        // 按行切，只处理完整的行；剩下的等下一块（NDJSON，和命令通道一个风格）。
        for (;;) {
          const nl = buf.indexOf('\n')
          if (nl < 0) break
          const line = buf.slice(0, nl).trim()
          buf = buf.slice(nl + 1)
          if (!line) continue
          let msg = null
          try {
            msg = JSON.parse(line)
          } catch {
            continue
          }
          if (msg && msg.cmd === 'quit') {
            log('[gui-pipe] 采集服务正在退出，界面一起关')
            shutdownFromService()
          } else if (msg && msg.cmd === 'show') {
            // 托盘左键 / 「打开主界面」：界面已经开着，只需要把窗口弄到眼前。
            // 以前托盘每次点击都重新拉一个启动进程，连点就弹满屏黑框 —— 现在
            // 托盘先在这条管道上问一句，连得通就只发这条 show（见
            // native/backend/TrayIcon.cpp 的 RequestOpenGui）。
            log('[gui-pipe] 托盘请求显示窗口')
            showMainWindow()
          }
        }
      })
      // 对端写完就关，socket 上的 EPIPE 是正常结束，不要当错误。
      sock.on('error', () => {})
      sock.setTimeout(3000, () => sock.destroy())
    })
    guiPipeServer.on('error', (err) => {
      // 单实例锁已经保证只有一个界面，这里出错基本就是管道被占用或权限问题。
      // 不能崩：界面照常可用，只是托盘「退出」时关不掉它。
      log('[gui-pipe] 管道不可用：' + err.message)
    })
    guiPipeServer.listen(GUI_PIPE_NAME, () =>
      log('[gui-pipe] 已监听（服务退出时用它关界面）'),
    )

    // 兜底标记由**主进程**自己盯，不依赖页面在轮询 service:live：
    // 页面没加载出来、渲染进程崩了的时候，退出通知照样得生效。
    if (!guiQuitTimer) guiQuitTimer = setInterval(consumeGuiQuitMarker, 800)
  } catch (err) {
    log('[gui-pipe] 启动失败：' + err.message)
  }
}

/**
 * 把主窗口弄到用户眼前（恢复最小化 → 显示 → 聚焦）。
 *
 * 托盘「打开主界面」和第二个实例启动都走这里：窗口已经在的时候不该再新建任何
 * 进程，只要把这一份叫出来。setAlwaysOnTop 那一下是为了绕开 Windows 的
 * 前台锁 —— 后台进程调 focus() 经常只是任务栏闪一下，窗口并不会真的升上来。
 */
function showMainWindow() {
  const w = mainWindow
  if (!w || w.isDestroyed()) return
  try {
    if (w.isMinimized()) w.restore()
    if (!w.isVisible()) w.show()
    w.setAlwaysOnTop(true)
    w.focus()
    w.setAlwaysOnTop(false)
  } catch {
    /* 窗口正在关，忽略 */
  }
}

/** 服务请求退出：走正常退出路径（close → window-all-closed → app.quit）。 */
function shutdownFromService() {
  try {
    if (mainWindow && !mainWindow.isDestroyed()) {
      mainWindow.close()
      return
    }
  } catch {
    /* 窗口已经在关了，直接退 */
  }
  app.quit()
}

// ---------------------------------------------------------------------------
// 状态文件轮询
//
// 为什么不是"起了服务才轮询"：这条通道存在的意义恰恰是**服务不是本进程起的**
// 时候也能看见状态。所以它是常开的，每秒读一次。
//
// 每次都读完就广播。刻意不做"内容一样就不广播"的去重：判断"一样"要序列化
// 整个 sessions（每秒几十 KB），省下的那点带宽还不如让界面自己决定要不要重绘。
// 轮询是常开的，所以界面**不能**靠 service:event 来判断导出结束 ——
// 导出状态在 session 对象里（exportRunning / exportPhase / exportOutPath），
// 界面直接看 service:live 的快照。这条在 App.vue 里也是这么写的。
// ---------------------------------------------------------------------------
const MIRROR_POLL_MS = 1000

function startStatusMirrorPoll() {
  setInterval(() => {
    const m = readStatusFile()
    live.mirrored = m
    live.mirrorSessions = Array.isArray(m.doc?.items) ? m.doc.items : []
    live.mirrorUpdatedAt = m.mtimeMs
    const payload = {
      type: 'service_mirror',
      source: m.doc && !m.stale ? 'file' : 'none',
      file: m.file,
      ageMs: m.ageMs,
      stale: m.stale,
      sessionCount: live.mirrorSessions.length,
      updatedAt: Date.now(),
    }
    for (const w of BrowserWindow.getAllWindows()) {
      try {
        w.webContents.send('service:event', payload)
      } catch {
        /* 窗口正在关 */
      }
    }
  }, MIRROR_POLL_MS).unref?.()
}

// ---------------------------------------------------------------------------
// 导出进度不再由一条常开的定时器广播
//
// 以前这里有一个 startExportStatusPoll()：每 400ms 读一次插件状态并广播。
// 现在导出是**队列**驱动的（见上面的 runExportJob / pumpExportQueue），
// 进度在任务自己等待收尾的那段时间里按同样 400ms 的节奏播，好处是：
//   * 每条进度天然带着 profile id，界面能按路各记一份状态；
//   * 没有导出在跑时一个定时器都不用开着。
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// IPC
// ---------------------------------------------------------------------------
function registerIpc() {
  startStatusMirrorPoll()

  // 页面每 500ms 主动推一笔渲染统计过来（见 app/preload.cjs 的 reportStats）。
  // 主进程只记录，绝不反过来用 executeJavaScript 去"拉" —— 拉一次要 2.5 秒，
  // 把渲染进程掐停之后量出来的帧率是假的（46fps vs 中位数达标 16.8ms）。
  ipcMain.on('preview:stats', (_evt, sample) => {
    if (!sample || typeof sample !== 'object') return
    pushedSamples.push({ at: Date.now(), ...sample })
    // 只留最近 240 笔（约 2 分钟），避免长时间运行时无上限堆积。
    if (pushedSamples.length > 240) pushedSamples.splice(0, pushedSamples.length - 240)
  })

  ipcMain.handle('service:info', () => {
    const running = serviceRunningFast()
    return {
      running,
      exists: existsSync(BACKEND_PATH),
      path: BACKEND_PATH,
      session: SERVICE_SESSION_ID,
      launcherPid: serviceChild?.pid ?? null,
      // 守护进程的状态一并带出去：界面上要能看出"服务有人在看着"，
      // 而不是服务崩了之后一片安静。
      guardian: guardianSnapshot(),
      versions: {
        electron: process.versions.electron,
        chrome: process.versions.chrome,
        node: process.versions.node,
      },
      isDev: IS_DEV,
    }
  })

  ipcMain.handle('service:launch', () => launchService())

  // 重启采集服务（设置页按钮）。
  //
  // 就是"停掉再拉起"。为什么现在可以有这个按钮：以前服务一停，正在录的
  // 所有分路全断、回放缓冲清空，所以当时的结论是"任何功能都不许重启服务"。
  // 现在两件事变了 ——
  //   * 分片是**滚动覆盖**地写在磁盘上的，服务停掉只是不再往后写，已经落盘
  //     的那几分钟照样能导出；
  //   * 有守护进程（shadow-capture --guard，由服务自己拉起）兜底：真要是这里
  //     拉不起来，守护几秒内也会把它拉回来。
  // 所以"重启"从一个危险操作变成了"改完 C++ 想让新 exe 生效时点一下"。
  ipcMain.handle('service:restart', async () => {
    // 走优雅停机：让服务把当前分片 Finalize 完再退。直接 taskkill 会丢掉
    // 最后那一片（用户报过「点重启前两秒的切片全部丢失」）。
    const stop = await stopServiceGracefully()
    // 等旧进程把互斥体放了。它是内核对象，进程一退出就没了，但也给它
    // 一点收尾时间（编码器 Finalize 分片）；等不到也照样往下走 ——
    // launchService() 自己会判断"还在跑就复用"，不会叠出第二个。
    for (let i = 0; i < 20; i++) {
      if (serviceRunning() !== true) break
      await new Promise((r) => setTimeout(r, 200))
    }
    const started = launchService()
    if (!started.ok) return { ok: false, error: started.error ?? '重启失败。' }
    // 新服务还没收到本机的预览帧率设置，补一次下发（它只认命令行和配置文件）。
    void pushSettingsToService()
    return {
      ok: true,
      pid: started.pid ?? null,
      alreadyRunning: started.alreadyRunning === true,
      graceful: stop.graceful === true,
    }
  })

  ipcMain.handle('service:reveal', () => {
    shell.showItemInFolder(BACKEND_PATH)
    return true
  })

  // 停止整套（服务 + 守护）。
  //
  // 真正的实现在 stopServiceGracefully()：先立哨兵、再请服务自己优雅退出
  // （收尾分片），等不到才 taskkill 兜底。这里只做一层通道。
  ipcMain.handle('service:stop', () => stopServiceGracefully())

  // 问服务要当前配置列表（preload 拿它决定要映射几个环）。
  // 注意服务是**常驻进程**，读它 stdout 会把它的状态流搅乱，所以这里不碰
  // 正在跑的那个实例：直接用 --profiles <文件> 让它"只加载一次然后退出"，
  // 我们读的是本地配置文件本身，语义上等价且没有副作用。
  ipcMain.handle('service:profiles', () => {
    const doc = loadProfiles()
    return {
      items: doc.profiles ?? [],
      path: profilesPath(),
      recordRoot: doc.recordRoot ?? '',
      exportDir: doc.exportDir ?? '',
      error: doc.error ?? null,
    }
  })

  // 列出当前可采集的顶层窗口（GUI 里"选窗口"下拉用）
  ipcMain.handle('service:list-windows', () => listWindows())
  ipcMain.handle('service:list-monitors', () => listMonitors())

  // 只保存**一路**配置：其余分路一律以磁盘上的现值为准。
  //
  // 【为什么需要这个】
  //
  // 原来的 `service:save-profiles` 收的是界面上的**整份**配置数组（App.vue 里
  // `JSON.parse(JSON.stringify(profiles.value))` 一次性全传过来），主进程再整份
  // 写回 profiles.json。这份数据是界面**打开那一刻**的副本，只要中间有任何一路
  // 被别的地方改过（服务端重载、另一个窗口编辑、手工改了 json），保存 A 路的
  // 帧率就会顺手把 B 路**回退成旧值** —— 录制中的 B 路下一拍 reload 就换了帧率，
  // 用户看到的就是"改一路，隔壁也跟着抖"。
  //
  // 现在单路保存：磁盘读出来 → 只替换这一路 → 写回。界面那份数据有多旧都不影响
  // 别的分路，因为它们根本不经过界面。
  //
  // payload: { id, patch }，patch 是这一路的完整新对象（缺 id 也没关系）。
  ipcMain.handle('service:save-profile', async (_evt, payload) => {
    const id = String(payload?.id ?? '')
    const patch = payload?.patch
    if (!id) return { ok: false, error: '缺少配置 id。' }
    if (!patch || typeof patch !== 'object') return { ok: false, error: '缺少配置内容。' }

    const file = profilesPath()
    // 以磁盘为准：读失败就不要写，宁可保存失败也不能拿半份数据去覆盖。
    const current = loadProfiles()
    if (current.error) return { ok: false, error: `读不到现有配置：${current.error}` }
    const list = Array.isArray(current.profiles) ? current.profiles.slice() : []

    // 这一路的新形状，交给和整份保存同一套 normalize（补默认值、纠字段名）。
    const normalizedOne = normalizeProfiles({
      recordRoot: current.recordRoot,
      exportDir: current.exportDir,
      profiles: [{ ...patch, id }],
    })
    const nextOne = normalizedOne.profiles?.[0]
    if (!nextOne) return { ok: false, error: '这一路配置校验没通过。' }

    const idx = list.findIndex((p) => String(p?.id ?? '') === id)
    const before = idx >= 0 ? JSON.stringify(list[idx]) : null
    if (idx >= 0) list[idx] = nextOne
    else list.push(nextOne)

    // 内容没变就不写盘、不重载：界面上点一下"保存"却没改任何东西时，
    // 不该把隔壁正在录的分片周期打断（虽然只影响自己那一路）。
    const changed = before !== JSON.stringify(nextOne)
    const doc = {
      ...current,
      recordRoot: current.recordRoot,
      exportDir: current.exportDir,
      profiles: list,
    }

    if (changed) {
      try {
        mkdirSync(path.dirname(file), { recursive: true })
        try {
          if (existsSync(file)) copyFileSync(file, `${file}.bak`)
        } catch {
          /* 备份失败不该挡住保存本身 */
        }
        writeFileSync(file, JSON.stringify(doc, null, 2), 'utf8')
        lastGoodProfiles = doc
      } catch (err) {
        return { ok: false, error: `写入配置失败：${err.message}` }
      }
    }

    if (!changed) return { ok: true, path: file, changed: false, reloaded: [], restarted: false }

    if (!serviceRunningFast()) return { ok: true, path: file, changed: true, reloaded: [], restarted: false }

    const r = await sendServiceCommand({ cmd: 'reload', id })
    return {
      ok: true,
      path: file,
      changed: true,
      reloaded: r.ok ? [id] : [],
      failed: r.ok ? [] : [{ id, error: r.error }],
      restarted: false,
    }
  })

  // 保存配置：写盘 + 只让**改动的那几路**重新加载。
  //
  // 以前这里是 `restartService()`：taskkill 掉所有 shadow-capture.exe 再拉一个新的。
  // 两个问题：
  //   1. 服务是多路的，为了改一路把所有分路的录制一起掐断。用户改的是"窗口A
  //      的帧率"，隔壁"整屏"那一路的回放缓冲也会被清空 —— 这就是"影响隔壁分路"。
  //   2. restartService 里有一段同步 ping 轮询（最多阻塞主进程 3 秒），Electron
  //      渲染进程的 IPC 全被堵住，界面看起来像卡死。
  //
  // 现在改成：写盘成功后，把**内容真的变了**的 profile id 逐个发给服务，
  // 服务收到 `{"cmd":"reload","id":...}` 只拆掉那一路的 Session 重建，
  // 其它路一个字节都不动。新增/删除配置也走同一条路。
  //
  // 【再后来】整份保存这条通道界面已经不用了（改成 `service:save-profile`
  // 一次只发一路，见它上面的注释）。这里保留只为兼容老调用方（自检脚本），
  // 而且**彻底去掉了重启服务这条兜底** —— 用户明确要求"所有功能都禁止重启
  // 服务，必须实时更新"。命令通道哪怕只送出去一部分，也只报哪几路没送到，
  // 绝不为了"让它生效"去杀进程：杀一次就是所有分路的录制一起断。
  ipcMain.handle('service:save-profiles', async (_evt, payload) => {
    const doc = normalizeProfiles(payload)
    const file = profilesPath()

    // 写之前先记下旧配置，用来比较"到底哪几路变了"
    const before = loadProfiles()
    const beforeById = new Map((before.profiles ?? []).map((p) => [p.id, JSON.stringify(p)]))
    const afterById = new Map((doc.profiles ?? []).map((p) => [p.id, JSON.stringify(p)]))

    // 空数组保护：磁盘上明明有配置，送来的却是空数组，说明界面那一侧的
    // profiles 还没载入完（或者中途出了岔子），不是用户真的把配置删光了。
    // 没有这道闸，一次误触就能把整份配置覆盖成 []。真要清空，得界面明确
    // 说明"这是清空操作"—— 目前界面没有这个入口，所以一律拒绝。
    const knownCount = beforeById.size || (lastGoodProfiles?.profiles?.length ?? 0)
    if (afterById.size === 0 && knownCount > 0) {
      return {
        ok: false,
        error: `拒绝保存：这一步会清空磁盘上的 ${knownCount} 路配置，但界面这边是空的（多半是配置还没载入完）。请等一下再试。`,
      }
    }

    const changed = []
    for (const [id, json] of afterById) if (beforeById.get(id) !== json) changed.push(id)
    for (const id of beforeById.keys()) if (!afterById.has(id)) changed.push(id)

    try {
      mkdirSync(path.dirname(file), { recursive: true })
      // 写盘前先留一份备份：配置是整个应用唯一的持久状态，一旦被写坏
      // （空数组、半截 JSON、编码错）就没有第二处可以还原。
      try {
        if (existsSync(file)) copyFileSync(file, `${file}.bak`)
      } catch {
        /* 备份失败不该挡住保存本身 */
      }
      writeFileSync(file, JSON.stringify(doc, null, 2), 'utf8')
      lastGoodProfiles = doc
    } catch (err) {
      return { ok: false, error: `写入配置失败：${err.message}` }
    }

    // 服务没在跑：写盘就够了，下次启动自然用新配置。
    if (!serviceRunningFast()) return { ok: true, path: file, restarted: false, reloaded: [] }

    // 逐路重载。命令走命名管道，所以**不管服务是谁起的**都能下发 ——
    // 这正是这条路能彻底取代"杀掉整个服务再拉一个"的前提。
    //
    // 并发发出去（连接复用，每条只是一个 write）。以前是串行 spawnSync，
    // 三路就要等三个进程启动。
    const results = await Promise.all(changed.map((id) => sendServiceCommand({ cmd: 'reload', id })))
    const reloaded = []
    const failed = []
    changed.forEach((id, i) => {
      const r = results[i]
      if (r.ok) reloaded.push(id)
      else failed.push({ id, error: r.error })
    })

    // 一路都没送出去（服务太旧没有管道，或者管道名对不上）也**不再整体重启**。
    // 用户明确要求禁止重启服务：重启一次所有分路录制一起断，代价远大于
    // "这一次没生效"。把失败如实报上去，界面上改完再存一次就会重试。
    if (reloaded.length === 0 && failed.length > 0) {
      return {
        ok: true,
        path: file,
        restarted: false,
        reloaded: [],
        failed,
        detail: `命令通道没送出去（${failed[0].error}）。已存盘但这一拍没生效，稍后再保存一次即可 —— 不会重启服务。`,
      }
    }

    return { ok: true, path: file, restarted: false, reloaded, failed }
  })

  // -------------------------------------------------------------------------
  // 设置页
  //
  // 目前只有一项：预览画面最大帧率。它有两个去处：
  //   1. 写进 settings.json 存下来（下次启动还生效）；
  //   2. 立刻发给正在跑的服务（{"cmd":"set-preview-fps"}），下一拍生效。
  //
  // 服务没在跑也要正常返回：值已经存下了，等服务起来时会读同一个文件下发。
  // -------------------------------------------------------------------------
  ipcMain.handle('settings:get', () => ({ ok: true, settings: loadSettings() }))

  ipcMain.handle('settings:set', async (_evt, patch) => {
    const saved = saveSettings(patch)
    if (!saved.ok) return saved

    // 立刻让服务生效。
    //
    // 服务没收到**不再往界面上抛话**：以前会把"服务这一拍没收到…"当 detail
    // 显示出来，用户看到的第一反应是"那到底是生效了还是没生效"，比不说更糟。
    // 现在只如实回报 applied 这个布尔值，提示文案由界面自己决定（成功就只说
    // 成功）。真失败的原因会进主进程日志，排查时看日志即可。
    let applied = false
    if (serviceRunningFast()) {
      const r = await sendServiceCommand({ cmd: 'set-preview-fps', fps: saved.settings.previewMaxFps })
      applied = !!r.ok
      if (!applied) log('预览帧率下发失败：', r?.error ?? r?.detail ?? '未知原因')
    }
    // 不管服务收没收到，界面这边立刻按新值走（采集端晚一两拍没关系）。
    applyPreviewFpsToPage(saved.settings.previewMaxFps)
    return { ok: true, settings: saved.settings, applied }
  })

  // -------------------------------------------------------------------------
  // 实时状态：服务的 stdout 只有本进程读得到；不是本进程起的服务，就读它写的
  // 状态文件。两条通道合起来，界面**在任何启动方式下**都看得见服务状态。
  // -------------------------------------------------------------------------
  ipcMain.handle('service:live', () => {
    const m = readStatusFile()
    const mirrorSessions = Array.isArray(m.doc?.items) ? m.doc.items : []
    const mirrorFresh = !!m.doc && !m.stale
    // 谁的更新？stdout 是我们自己起的服务的实时流；状态文件是"谁起的都行"的镜像。
    // 本进程起的服务永远优先用 stdout（它还有导出事件这种一次性消息），
    // 只有拿不到 stdout 或者它已经很久没动静了才退到文件。
    const stdoutFresh = !!serviceChild && Date.now() - live.updatedAt < 4000
    const useMirror = !stdoutFresh && mirrorFresh
    const sessions = useMirror ? mirrorSessions : live.sessions
    const updatedAt = useMirror ? m.mtimeMs || Date.now() : live.updatedAt

    // 复用别的服务时，服务加载的配置跟界面这份**可能不是同一份**（见 launchService
    // 里的注释）。把两边路径都带出去，界面才能提示"你看的不是同一个服务的录制状态"，
    // 而不是默默显示一份对不上的分片数。
    //
    // 字段名有两种拼法，都要认：
    //   * 状态文件里是 profilesPath（native/backend/main.cpp 的 PublishStatusFile）
    //   * stdout 的 profiles_loaded 事件里是 path（同文件 LogStatusLine 那条）
    // 只认一个的话，走文件那条路永远拿到 null，告警就永远不亮。
    const wantedProfiles = profilesPath()
    const runningProfiles =
      live.profilesLoaded?.path ?? m.doc?.profilesPath ?? m.doc?.path ?? null
    const profilesMismatch =
      !!runningProfiles && path.resolve(runningProfiles) !== path.resolve(wantedProfiles)

    return {
      ok: true,
      sessions,
      profilesLoaded: live.profilesLoaded,
      updatedAt,
      // 这些数字是不是新鲜的：本进程起的服务算新鲜；别人起的服务，
      // 只要状态文件还新，也算新鲜（这正是这次要修的东西）。
      owned: !!serviceChild,
      // 状态从哪儿来的，界面据此显示"实时流"还是"状态文件"。
      source: useMirror ? 'file' : stdoutFresh ? 'stdout' : 'none',
      mirrorFile: m.file,
      mirrorAgeMs: m.ageMs,
      mirrorStale: m.stale,
      running: stdoutFresh || mirrorFresh,
      wantedProfilesPath: wantedProfiles,
      runningProfilesPath: runningProfiles,
      profilesMismatch,
      // 守护进程的状态（界面顶栏显示"守护在岗"，服务崩了能被自动拉回）。
      // 心跳是守护每秒级写的，读一次文件很便宜，不额外起进程。
      guardian: guardianSnapshot(),
    }
  })
  // -------------------------------------------------------------------------
  // 导出：**直接调 C++**（native/build/shadow_ring.node 的 exportStart/exportStatus）
  //
  // 为什么不再走服务的 stdin 命令通道：
  //   Exporter 只吃四个纯字符串参数（分片目录 / 导出目录 / 前缀 / 秒数），分片
  //   是磁盘上已经写好的 .ts 文件，任何进程都能读 —— 导出这件事跟"谁在录"
  //   没有关系。原先放在服务里、靠 stdin 送命令，唯一原因是界面够不着 exe 里的
  //   类；代价是**界面复用用户自己起的服务时没有那个 stdin**，导出永远回
  //   "没有命令通道"。
  //   编进插件之后，服务是谁起的、在不在跑，都不影响导出。
  //
  // 导出目录/前缀从哪来：分片目录是服务分配的（不是配置里写的字段），
  // 所以从状态镜像的 recordDir 取；拿不到就退回配置的 recordRoot + 惯例目录名。
  // -------------------------------------------------------------------------
  // 导出：**直接调 C++**（native/build/shadow_ring.node 的 exportStart/exportStatus）
  //
  // 为什么不再走服务的 stdin 命令通道：
  //   Exporter 只吃几个纯字符串参数（分片目录 / 导出目录 / 前缀 / 秒数），分片
  //   是磁盘上已经写好的 .mp4 文件，任何进程都能读 —— 导出这件事跟"谁在录"
  //   没有关系。原先放在服务里、靠 stdin 送命令，唯一原因是界面够不着 exe 里的
  //   类；代价是**界面复用用户自己起的服务时没有那个 stdin**，导出永远回
  //   "没有命令通道"。
  //   编进插件之后，服务是谁起的、在不在跑，都不影响导出。
  //
  // 导出目录/前缀从哪来：分片目录是服务分配的（不是配置里写的字段），
  // 所以从状态镜像的 recordDir 取；拿不到就退回配置的 recordRoot + 惯例目录名。
  //
  // 【点导出 = 入队】真正的执行在 runExportJob 里，队列串行推进（见上面
  // 导出队列那一段）。所以这里的返回值只表示"排上了没有、前面还有几个"，
  // 进度和结果全部走 export_status 事件，并且带着 profile id。
  // -------------------------------------------------------------------------
  ipcMain.handle('service:export', (_evt, req) => {
    const id = String(req?.id ?? '')
    if (!id) return { ok: false, error: '没有指定要导出哪一路配置。' }

    const doc = loadProfiles()
    const profile = (doc.profiles ?? []).find((p) => p.id === id)
    if (!profile) return { ok: false, error: `配置里没有 id = ${id} 这一路。` }

    const seconds = Number(req?.seconds)
    const job = {
      id,
      name: profile.name || profile.id || id,
      seconds: Number.isFinite(seconds) && seconds > 0 ? Math.round(seconds) : 0,
      queuedAt: Date.now(),
    }

    // 同一路连点两下：不重复排队，告诉界面"已经在导出里了"就行。
    // 这不是错误 —— 按钮上写着"导出中"，本来也不会去点。
    if (exportRunningJob?.id === id || exportQueue.some((x) => x.id === id)) {
      return { ok: true, queued: true, alreadyQueued: true, ahead: 0 }
    }

    exportQueue.push(job)
    const ahead = exportQueue.length - 1 + (exportRunningJob ? 1 : 0)
    broadcastExportQueue()
    void pumpExportQueue()
    return { ok: true, queued: true, ahead, position: ahead + 1 }
  })

  // -------------------------------------------------------------------------
  // 删除一路配置
  //
  // 写盘 + 让服务把这一路的会话停掉（发 reload：服务读盘后发现这个 id 已经
  // 不在了，会把它那条 Session 停掉并回收，其它路一个字节都不动 —— 见
  // native/backend/main.cpp 的 reload 分支）。
  //
  // 为什么不在这里删录像目录：那是用户的素材，删配置不该顺手删录像。要清理
  // 由用户自己动手（或者等回放窗口自然滚动覆盖掉）。
  // -------------------------------------------------------------------------
  ipcMain.handle('service:delete-profile', async (_evt, payload) => {
    const id = String(payload?.id ?? '')
    if (!id) return { ok: false, error: '缺少配置 id。' }

    const file = profilesPath()
    const current = loadProfiles()
    if (current.error) return { ok: false, error: `读不到现有配置：${current.error}` }
    const list = Array.isArray(current.profiles) ? current.profiles.slice() : []
    const idx = list.findIndex((p) => String(p?.id ?? '') === id)
    if (idx < 0) return { ok: false, error: `磁盘上没有 id = ${id} 这一路配置。` }

    const [removed] = list.splice(idx, 1)
    const doc = {
      ...current,
      recordRoot: current.recordRoot,
      exportDir: current.exportDir,
      profiles: list,
    }
    try {
      mkdirSync(path.dirname(file), { recursive: true })
      try {
        if (existsSync(file)) copyFileSync(file, `${file}.bak`)
      } catch {
        /* 备份失败不该挡住删除本身 */
      }
      writeFileSync(file, JSON.stringify(doc, null, 2), 'utf8')
      lastGoodProfiles = doc
    } catch (err) {
      return { ok: false, error: `写入配置失败：${err.message}` }
    }

    // 服务在跑就让它把这一路停掉。发不出去也照样算删除成功（配置已经不在
    // 磁盘上了，下次重启服务自然就不会再起它）。
    let stopped = false
    if (serviceRunningFast()) {
      const r = await sendServiceCommand({ cmd: 'reload', id })
      stopped = !!r.ok
      if (!stopped) log(`[profile] 删除 ${id} 已写盘，但 reload 没送到服务：${r.error ?? '未知'}`)
    }
    log(`[profile] 已删除配置 ${id}（剩余 ${list.length} 路）`)
    return {
      ok: true,
      id,
      name: removed?.name ?? id,
      remaining: list.length,
      stopped,
    }
  })

  // 界面要主动问一次"服务现在到底怎么样"（比如刚重启完服务）
  ipcMain.handle('service:refresh', async () => {
    const res = await sendServiceCommand({ cmd: 'list_sessions' })
    return { ...res, owned: !!serviceChild }
  })

  // -------------------------------------------------------------------------
  // 预览面板尺寸 → 服务
  //
  // 服务用这个数字决定预览发原分辨率还是砍半（Session::SetPreviewTarget）。
  // 面板大就发全尺寸（1:1 最清楚），面板小就砍半（上传量降到 1/4）——
  // 这样"清晰"和"省 GPU"不再是对立的，省下来的都是面板看不见的像素。
  //
  // 失败一律静默：服务可能还没起来（管道没建好），这时候服务会退回旧阈值，
  // 预览照常出画面，用户不该看到任何报错。
  // -------------------------------------------------------------------------
  ipcMain.handle('preview:size', async (_evt, payload) => {
    const w = Math.round(Number(payload?.w) || 0)
    const h = Math.round(Number(payload?.h) || 0)
    if (w < 2 || h < 2) return { ok: false, error: '尺寸无效' }
    const res = await pipeSend({ cmd: 'set-preview-size', w, h })
    if (!res.ok) log('[preview] 上报面板尺寸失败（服务可能还没起来）：', res.error)
    return res
  })
}

// ---------------------------------------------------------------------------
// 自检：验证「服务 + 共享内存 + addon」链路（需要服务已在运行）
// ---------------------------------------------------------------------------
function runProbe() {
  const report = { steps: [], ok: false, at: new Date().toISOString() }

  // 【被测会话 id 怎么定】
  //
  // 老写法写死 SERVICE_SESSION_ID（`shadowplay-service-v1`），那只是**单路模式**
  // 的固定环名。现在服务默认跑多会话模式（`--profiles`），每一路配置各自建一个
  // 环，名字是配置 id（`p-desk` / `p-win` …），根本没有 `shadowplay-service-v1`
  // 这个环 —— 于是 `npm run probe` 永远报「无法打开共享内存帧环，采集服务可能
  // 没有启动」，而服务其实活得好好的（本鱼踩过）。
  //
  // 现在的优先级：
  //   1. `--session <id>` 显式指定（对照实验用）；
  //   2. 状态文件里第一路**在录制/已连接**的配置 id（多会话模式的真实环名）；
  //   3. 退回 SERVICE_SESSION_ID（真正的单路模式，或服务没跑时给出准确报错）。
  let sessionId = SERVICE_SESSION_ID
  let sessionSource = 'fixed-default'
  const argIdx = process.argv.indexOf('--session')
  if (argIdx >= 0) {
    const v = process.argv[argIdx + 1]
    if (v && !v.startsWith('-')) {
      sessionId = v
      sessionSource = 'argv'
    }
  }
  if (sessionSource !== 'argv') {
    const pickFromStatus = () => {
      const items = readStatusFile()?.doc?.items
      if (!Array.isArray(items)) return null
      // 优先挑真的在出画面的那一路。状态文件里 items 的顺序不固定，
      // 所以不靠"第一条"碰运气。
      const usable = items.filter(
        (it) => it && typeof it.id === 'string' && it.id && it.state !== 'disabled',
      )
      return (
        usable.find((it) => it.state === 'live') ??
        usable.find((it) => it.state === 'waiting') ??
        usable[0] ??
        null
      )
    }

    // 启动清理（`cleanOnStartup`）在 main.cjs 顶部就把 status/ 整棵删掉了，
    // 而服务要等最多一秒才写出下一份。自检如果正好踩在这个空窗上，就只能退回
    // 固定 id —— 那是单路模式的名字，多会话模式下压根没有这个环。
    // 所以这里短暂轮询等它出现，最多 2.5 秒（服务每秒写一次，够赶上两轮）。
    let best = pickFromStatus()
    if (!best) {
      const deadline = Date.now() + 2500
      while (!best && Date.now() < deadline) {
        // 同步小睡：runProbe() 是启动期的自检，整条路径都是同步的，
        // 这里不能 await（函数不是 async，改了会连带动 applyProbe 那条）。
        const until = Date.now() + 120
        while (Date.now() < until) {
          /* 空转等状态文件刷新 */
        }
        best = pickFromStatus()
      }
    }
    if (best) {
      sessionId = best.id
      sessionSource = 'status-file'
    }
  }
  report.session = sessionId
  report.sessionSource = sessionSource

  try {
    const result = probe(sessionId, 8000)
    report.steps.push({ name: 'ring', ...result })
    report.ok = result.ok
    if (!result.ok) {
      report.error =
        sessionSource === 'status-file'
          ? result.error
          : `${result.error}（用的是固定会话 id「${sessionId}」；` +
            `如果服务跑在多会话模式下，环名是各路配置的 id，用 --session <id> 指定，或先让服务写出一份状态文件）`
    }
  } catch (err) {
    report.error = err.message
  }

  // Electron 是 GUI 子系统程序，从受限终端启动时控制台输出经常拿不到，
  // 所以自检结果一律写文件。
  const outFile = path.join(ARTIFACT_DIR, 'probe-report.json')
  try {
    mkdirSync(ARTIFACT_DIR, { recursive: true })
    writeFileSync(outFile, JSON.stringify(report, null, 2), 'utf8')
  } catch (err) {
    logErr('[probe] 写报告失败：', err.message)
  }
  log(`[probe] ok=${report.ok} 报告：${outFile}`)
  return report.ok ? 0 : 1
}

// ---------------------------------------------------------------------------
// 界面自检：验证「服务 + 共享内存 + addon + WebGL 渲染」整条链路的真实表现
//
// 和 runProbe 的区别：runProbe 只验证主进程能不能拿到帧；这个模式会真的
// 打开界面、跑一段时间，然后询问渲染进程画了多少帧、帧间隔多少 ——
// 用来确认"实时预览"确实在动，以及 60fps 限流有没有生效。
// ---------------------------------------------------------------------------
async function runSelfTest(seconds) {
  const report = { steps: [], ok: false, at: new Date().toISOString() }
  const outFile = path.join(ARTIFACT_DIR, 'selftest-report.json')

  // 先让采集服务由**本进程**拉起来，原因有二：
  //   ① 界面侧「录制中 / 分片 / 导出按钮」这些状态全走服务的 stdout，而那条流
  //      只有启动服务的进程读得到。不自起服务的话 liveOwned 永远是 false，
  //      自检就永远验不到需求 2 / 需求 6 的界面接线。
  //   ② 上一次跑崩（或上一个验收脚本被强杀）会留下一个孤儿 shadow-capture，
  //      它抢着建环，于是本次报告里"2 路都连上了"是真的、但没有 owner。
  //      这份脏状态会让自检时好时坏，所以启动前先清干净。
  // --fresh（scripts/launch.mjs）会把录制根目录指到 test/_fresh 并要求清空，
  // 这样界面上的"第几段 / 多少 MB"只反映这一次运行。
  if (process.env.SHADOW_RECORD_ROOT) {
    try {
      const { rmSync } = require('node:fs')
      const root = process.env.SHADOW_RECORD_ROOT
      rmSync(root, { recursive: true, force: true })
      // 服务只认配置文档里的 recordRoot，环境变量它根本看不见。所以这里改的是
      // **文档**：复制一份、只换 recordRoot，再让本次服务加载这份副本。
      const doc = loadProfiles()
      doc.recordRoot = root
      mkdirSync(ARTIFACT_DIR, { recursive: true })
      const fresh = path.join(ARTIFACT_DIR, 'selftest-profiles.json')
      writeFileSync(fresh, JSON.stringify(doc, null, 2), 'utf8')
      process.env.SHADOW_PROFILES = fresh
      log('已清空录制根目录并改用本次配置：', root, '→', fresh)
    } catch (err) {
      logErr('[selftest] 准备本次录制根目录失败：', err.message)
    }
  }
  stopService()
  await new Promise((r) => setTimeout(r, 400))
  const started = launchService()
  report.steps.push({ name: 'launch', ...started, owned: !!serviceChild })
  if (!started.ok) {
    report.error = started.error ?? '启动采集服务失败'
    report.reason = report.error
    try {
      mkdirSync(ARTIFACT_DIR, { recursive: true })
      writeFileSync(outFile, JSON.stringify(report, null, 2), 'utf8')
    } catch (err) {
      logErr('[selftest] 写报告失败：', err.message)
    }
    return 1
  }
  // 服务起来到建环/建录制器要几百毫秒，让页面在服务就位之后再开始计时
  await new Promise((r) => setTimeout(r, 900))

  const win = createWindow()
  await new Promise((resolve) => win.webContents.once('did-finish-load', resolve))

  // 让页面自己跑满整个测试窗口，**全程不打断它**。
  //
  // 这里以前用 executeJavaScript 去"拉"渲染状态，那是错的：跨进程读一次要
  // 2.5 秒（序列化整棵对象树），一拉就把渲染进程掐停，量出来的 46fps 是测量
  // 行为自己造成的假象（同一份数据里"帧间隔中位数"却是达标的 16.8ms，这个
  // 自相矛盾就是证据）。现在改成页面每 500ms 用 ipcRenderer.send 主动推过来
  // （见 app/preload.cjs 的 reportStats 与 frontend 的 setupHistory），
  // 主进程只在最后看一眼已经收好的样本。
  pushedSamples.length = 0
  await new Promise((r) => setTimeout(r, seconds * 1000))

  const samples = pushedSamples.slice()
  // 只留样本里出现过的字段名，避免把 undefined 也写进报告。
  const last = samples.length ? samples[samples.length - 1] : null

  report.sampleCount = samples.length
  report.samples = samples
  report.render = last

  // 多会话断言用的计数由页面在每笔推送里带上来（snapshot() 里的 readySessions），
  // 主进程不再跨进程去读 —— 那正是把渲染进程掐停的原因。
  report.sessions = null
  report.readySessionCount = last?.readySessions ?? 0

  // 首尾两次采样之间画了多少帧 —— 这才是"有没有真的在实时预览"的证据。
  const hist = samples
  report.renderedDuringTest = hist.length ? Number(hist[hist.length - 1].rendered ?? 0) : undefined

  // 自检不能只看"画过至少一帧"：那正是"只出一帧"这个 bug 的样子。
  // 要求测试窗口内持续出帧，并核对帧间隔确实落在 60fps 预算附近。
  report.ok = Boolean(last && last.rendered > 0)
  if (report.ok) {
    const drawn = report.renderedDuringTest ?? 0
    const minFrames = Math.max(3, Math.floor(seconds * 5))
    if (drawn < minFrames) {
      report.ok = false
      report.reason = `测试窗口内只画了 ${drawn} 帧（至少应有 ${minFrames} 帧）——看起来又停在单帧上了`
    } else {
      const median = last.intervalMedianMs ?? 0
      if (median > 0 && median < 8) {
        report.ok = false
        report.reason = `帧间隔中位数只有 ${median}ms，60fps 限流没生效`
      }
    }
  }
  // 多会话断言：preload 手里必须真的有 ≥2 路连上（配置文件里有 2 路 enabled
  // 且都能匹配到源）。这条不满足说明"多路预览"其实没实现。
  // 对照实验会用 SHADOW_EXPECT_SESSIONS=1 单独跑单路，那时跳过这条。
  const expectSessions = Number(process.env.SHADOW_EXPECT_SESSIONS ?? 2)
  if (report.ok && report.readySessionCount < expectSessions) {
    report.ok = false
    report.reason =
      `只有 ${report.readySessionCount} 路会话连上（应 ≥${expectSessions}）——多会话映射没生效`
  }
  // 保持宽高比断言（需求 3）：画布内实际画面的宽高比必须等于源的宽高比。
  // 拉伸变形时这两个值会明显不同（比如 16:9 的源被拉成画布的 1.4:1）。
  if (report.ok && last) {
    const src = Number(last.sourceAspect)
    const fit = Number(last.fittedAspect)
    if (Number.isFinite(src) && Number.isFinite(fit) && src > 0) {
      const err = Math.abs(fit - src) / src
      report.aspectError = err
      if (err > 0.02) {
        report.ok = false
        report.reason =
          `画面宽高比 ${fit.toFixed(3)} 与源宽高比 ${src.toFixed(3)} 差了 ` +
          `${(err * 100).toFixed(1)}%——预览被拉伸了，没有保持纵横比`
      }
    }
  }
  report.steps.push({ name: 'render', ...(last ?? {}) })

  // 界面接线断言（需求 2 的配置表单 + 需求 6 的导出按钮）。
  //
  // 这一段不碰渲染进程：字段是页面自己在每 500ms 的推送里捎上来的
  // （见 frontend/src/App.vue 的 guiSnapshot + usePreview 的 setupHistory(extra)）。
  // 「配置文档能读进界面、四项参数真的绑到了表单上」光看帧数证明不了。
  report.gui = {
    profileCount: last?.profileCount ?? 0,
    enabledCount: last?.enabledCount ?? 0,
    selectedName: last?.selectedName ?? null,
    liveCount: last?.liveCount ?? 0,
    liveRecording: last?.liveRecording ?? 0,
    liveSegments: last?.liveSegments ?? 0,
    liveBytes: last?.liveBytes ?? 0,
    liveOwned: last?.liveOwned === true,
    formFps: last?.formFps ?? null,
    formBitrate: last?.formBitrate ?? null,
    formReplaySeconds: last?.formReplaySeconds ?? null,
    matchBy: last?.matchBy ?? null,
    matchPattern: last?.matchPattern ?? null,
    canExport: last?.canExport ?? false,
  }
  if (report.ok) {
    if (!(report.gui.profileCount > 0) || last?.hasForm !== true) {
      report.ok = false
      report.reason = '界面上没有读到任何配置——配置文档没进 GUI（需求 2）'
    } else if (!(report.gui.formFps > 0) || !(report.gui.formBitrate > 0) || !(report.gui.formReplaySeconds > 0)) {
      report.ok = false
      report.reason =
        `配置表单的核心参数是空的（fps=${report.gui.formFps} ` +
        `bitrate=${report.gui.formBitrate} replay=${report.gui.formReplaySeconds}）——表单没绑上`
    } else if (last?.liveOwned !== true || !(report.gui.liveCount > 0)) {
      // 需求 5/6 的界面证据（录制段数、磁盘占用、导出按钮可用）全部来自
      // 服务 stdout 经主进程转发的那条线。这条线断了界面上就是一片 0，
      // 而帧还是照画 —— 光看 fps 完全看不出来，必须单独断言。
      report.ok = false
      report.reason =
        `界面读不到采集服务的实时状态（liveOwned=${last?.liveOwned} ` +
        `liveCount=${report.gui.liveCount}）——录制/导出状态那条线没接上`
    } else if (!(report.gui.liveRecording > 0)) {
      report.ok = false
      report.reason = `服务在跑，但没有一路报"录制中"（liveRecording=${report.gui.liveRecording}）——录制没起来`
    } else if (!(report.gui.liveSegments > 0) || !(report.gui.liveBytes > 0)) {
      // 界面上的"第几段 / 占多少磁盘"是需求 5 唯一可见的证据。服务虽然在录，
      // 但这几个数要真的经 stdout → 主进程 → 页面走一遍才有值；全是 0 就说明
      // 那条链子断了，而帧照样在画，光看 fps 看不出来。
      report.ok = false
      report.reason =
        `界面上的录制证据是空的（segments=${report.gui.liveSegments} ` +
        `bytes=${report.gui.liveBytes}）——服务状态没走到界面`
    } else if (report.gui.canExport !== true) {
      report.ok = false
      report.reason = '导出按钮是灰的（需求 6）——界面没把"有分片可导"判成可用'
    }
  }
  report.steps.push({ name: 'gui', ...report.gui })

  if (!report.ok && !report.reason) report.reason = '没有收到任何页面推送的渲染统计'

  try {
    mkdirSync(ARTIFACT_DIR, { recursive: true })
    writeFileSync(outFile, JSON.stringify(report, null, 2), 'utf8')
  } catch (err) {
    logErr('[selftest] 写报告失败：', err.message)
  }
  return report.ok ? 0 : 1
}

// ---------------------------------------------------------------------------
// 外部服务可见性验收（用户报的那个 bug）
//
// 场景：服务是在界面**之外**启动的（用户在 cmd 里跑 npm run service）。
// 这条路径的验收绝不能自己拉服务 —— 一旦自己拉了，就变成"界面自己起的服务"
// 那条老路，恰恰绕开了要验的东西。
//
// 判据全在界面的 __shadowGui() 上，最重要的是 liveRunning：
// 它必须为 true，而且 liveSource 必须是 'file'（证明状态是从状态文件读到的，
// 因为外部服务的 stdout 界面根本拿不到）。
// ---------------------------------------------------------------------------
async function runExternalProbe(seconds) {
  const report = { ok: false, at: new Date().toISOString() }
  const outFile = path.join(ARTIFACT_DIR, 'external-service-report.json')

  const win = createWindow()
  await new Promise((resolve) => win.webContents.once('did-finish-load', resolve))
  // 页面挂载后才会去读配置、开服务总线
  await sleep(1500)

  // 等界面把外部服务的状态读进来。它每 2 秒轮询一次状态文件，给足时间。
  const deadline = Date.now() + Math.max(4, seconds) * 1000
  let gui = null
  while (Date.now() < deadline) {
    gui = await win.webContents.executeJavaScript(
      'window.__shadowGui && window.__shadowGui()',
    )
    if (gui && gui.liveRunning === true && gui.liveSegments > 0) break
    await sleep(600)
  }

  Object.assign(report, gui ?? {})
  report.reason = gui?.liveRunning === true ? null : '界面没有认出界面之外启动的服务'
  report.ok = gui?.liveRunning === true
  report.steps = [{ name: 'external', ...(gui ?? {}) }]

  try {
    mkdirSync(ARTIFACT_DIR, { recursive: true })
    writeFileSync(outFile, JSON.stringify(report, null, 2), 'utf8')
  } catch (err) {
    logErr('[external-probe] 写报告失败：', err.message)
  }
  log(`[external-probe] ok=${report.ok} liveRunning=${report.liveRunning} source=${report.liveSource}`)
  try {
    win.destroy()
  } catch {
    /* 窗口可能已经关了 */
  }
  return report.ok ? 0 : 1
}

// ---------------------------------------------------------------------------
// 导出端到端验收（需求 6）
//
// 为什么必须让**界面自己起服务**：导出命令走的是服务 stdin，而这条通道只有
// 启动服务的那个进程拿得到（`serviceChild`）。外部 `npm run service` 起的服务
// 界面只能看到环、看不到录制/导出状态，导出按钮是灰的 —— 那是设计如此，不是 bug。
// 所以这条链路的验收必须覆盖"界面启动服务"这条路。
// ---------------------------------------------------------------------------
function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms))
}

async function runGuiExport(waitSeconds) {
  const report = { steps: [], ok: false, at: new Date().toISOString() }
  const win = createWindow()
  await new Promise((resolve) => win.webContents.once('did-finish-load', resolve))
  // 页面挂载后才会去读配置、开服务总线
  await sleep(1200)

  // 页面里的自检钩子（见 frontend/src/App.vue 的 __shadowGui / doExport）
  const gui = () => win.webContents.executeJavaScript('window.__shadowGui && window.__shadowGui()')
  const click = () =>
    win.webContents.executeJavaScript('window.__shadowExport && window.__shadowExport()')

  const started = launchService()
  report.steps.push({ name: 'launch', ...started, owned: !!serviceChild })
  if (!started.ok) {
    report.error = started.error ?? '启动采集服务失败'
    return finish(report, win)
  }

  // 等录制真的开始：界面上的"第几段"必须涨起来，光 connected 不够。
  //
  // 【判据必须是"选中的那一路录满了整个回放窗口"】以前是"任意路加起来 ≥3 段"：
  // 三路配置里 p-win 出两段就能把总数顶到 3，主路只录了 2 秒就点导出，20 秒
  // 回放窗口里 18 秒"还没录到"被按设计补成 NO SIGNAL —— 导出「成功」，成片
  // 却是黑的。现在按 selectedSegments × 单片秒数 ≥ 回放窗口来等。
  //
  // 【为什么必须多等一片】`recordSegments` 是服务**扫盘**数出来的
  // （见 SegmentRecorder::RefreshDiskStatsLocked），它把"正在写的那一片"
  // 也算进去了 —— 那一片还在 SinkWriter 手里、没 Finalize、index.csv 里
  // 也没有它，导出端根本读不出来。于是"10 片"实际上只有 9 片可用，
  // 9 × 2 秒 = 18 秒，剩下 2 秒只能补 NO SIGNAL。
  // 实测（test-export 反复复现）：`selectedSegments=10` 恒定导成
  // "9 个分片 / 20.0 秒 / 其中 2.0 秒没录到"，就是这一片之差。
  //
  // 环形缓冲现在保留 `回放窗口 + 两片`（见 MfEncoder::PurgeExpired 的
  // keepSlack），20 秒窗口 + 2 秒片 = 最多 12 片，所以等 11 片（10 片已完成
  // + 1 片在写）是安全的，不会像只留一片余量时那样"等到反而变少"。
  const first = (await gui()) ?? {}
  const replaySec = Number(first.formReplaySeconds) > 0 ? Number(first.formReplaySeconds) : 20
  const segSec = Number(first.formSegmentSeconds) > 0 ? Number(first.formSegmentSeconds) : 2
  const wantSegments = Math.ceil(replaySec / segSec) + 1
  // 【等待时长要按"内容秒数"算，不能按墙钟给】分片按帧数切（每片
  // `fps × 单片秒数` 帧），而喂帧线程在编码器跟不上时会补帧、时间轴按帧数
  // 推进 —— 1440p@144 实测 actualFps 常常只有 120~130，于是 20 秒"内容"
  // 要花 23~24 秒墙钟才录得出来。按 26 秒墙钟去等 10 片正好卡在边上，
  // 机器一忙就超时。这里按 1.8 倍内容时长再加 10 秒兜住，别让它随机失败。
  const waitWallSec = Math.max(waitSeconds, Math.ceil(replaySec * 1.8) + 10)
  const deadline = Date.now() + Math.max(4, waitWallSec) * 1000
  let last = null
  while (Date.now() < deadline) {
    last = await gui()
    if (last && last.selectedSegments >= wantSegments) break
    await sleep(700)
  }
  report.steps.push({ name: 'recording', wantSegments, wantReplaySec: replaySec, ...(last ?? {}) })
  if (!last || !(last.selectedSegments >= wantSegments)) {
    report.error =
      `等了 ${waitSeconds} 秒也没给选中的那一路录满回放窗口（需要 ${wantSegments} 段 ≈ ` +
      `${wantSegments * segSec} 秒，实际 selectedSegments=${last?.selectedSegments}；` +
      `全部路加起来=${last?.liveSegments}）`
    return finish(report, win)
  }
  if (last.canExport !== true) {
    report.error = `有 ${last.liveSegments} 段了，导出按钮却是灰的（title: ${last.exportTitle}）`
    return finish(report, win)
  }

  // 真的点一次导出 —— 走的是页面 → preload → ipcMain('service:export') → 服务 stdin
  const id = last.selectedId
  const fired = await click()
  report.steps.push({ name: 'export_click', ...(fired ?? {}) })

  // 结果由服务经 stdout 报回来，主进程转发成 service:event 的 export_result。
  //
  // 超时给到 180 秒：2560x1440@144 的 20 秒成片是 2880 帧，导出要把每片解码成
  // RGB32 再重新编码，实测 20 秒上下；30 秒的旧值在机器一忙就会误报超时。
  const res = await waitForExportResult(180000)
  report.steps.push({ name: 'export_result', ...(res ?? {}) })
  if (!res) {
    report.error = '点了导出，180 秒内没等到 export_result 事件'
    return finish(report, win)
  }
  if (res.failed || !res.ok) {
    report.error = `导出失败：${res.detail ?? '原因不明'}`
    return finish(report, win)
  }

  // 最后一步：磁盘上真的有这个文件，而且是个能认出 box 的 MP4。
  const outPath = String(res.outPath ?? '')
  report.outPath = outPath
  report.exportBytes = res.bytes ?? 0
  // 【两个字段名都要认】界面点导出走的是**插件**（`bridge/shadow_ring.cc` 的
  // `ExportStatusToJs`），它报的是 `segmentCount`；而服务自己经 stdout 发的
  // `export_result` 用的是 `segments`。以前这里只读 `segments`，于是自检报告
  // 里 `exportSegments` 恒为 0 —— 而成片明明是 11 个分片拼的，看上去像"导出
  // 没读到任何分片"。两边都认，报告才和实际一致。
  report.exportSegments = res.segments ?? res.segmentCount ?? 0
  report.exportDurationSeconds = res.durationSeconds ?? 0
  report.exportProfile = id
  // 计划诊断行：报告里带着"为什么补了 NO SIGNAL"，以后不必再靠猜。
  //
  // 【两个来源都要看】`live.lastExportPlan` 只在收到 `export_plan` 事件时才会
  // 被填上，而界面点导出走的是插件（Electron 主进程内），服务根本不知道有这
  // 回事 —— 那条诊断是插件通过 `exportStatus().plan` 捎过来的。这里优先用
  // 结果对象自带的，再退回事件缓存。
  if (res?.plan) report.exportPlan = res.plan
  else if (live.lastExportPlan) report.exportPlan = live.lastExportPlan
  if (!outPath || !existsSync(outPath)) {
    report.error = `服务说导出成功了，但磁盘上没有这个文件：${outPath}`
    return finish(report, win)
  }
  const head = readFirstBytes(outPath, 12)
  report.fileHead = head.toString('latin1')
  const isMp4 = head.length >= 12 && head.toString('latin1').slice(4, 8) === 'ftyp'
  if (!isMp4) {
    report.error = `导出的文件不是 MP4（头部 ${JSON.stringify(report.fileHead)}）`
    return finish(report, win)
  }
  if (!(Number(res.durationSeconds) > 0)) {
    report.error = `导出的 MP4 时长是 ${res.durationSeconds}，等于没内容`
    return finish(report, win)
  }

  // 时长达标**不等于**有画面：回放窗口里没录到的那段会被补成 NO SIGNAL，
  // 于是"20 秒 / 3.4 MB / 只有 1 个分片"这种全黑成片也能一路通过。
  // 摘要里那句「（其中 X 秒没录到，用 NO SIGNAL 补齐）」是唯一的口径，
  // 抠出来当硬判据 —— 回放窗口 20 秒里补掉一半以上就没测到东西。
  const detail = String(res.detail ?? '')
  const m = /其中\s*([0-9.]+)\s*秒没录到/.exec(detail)
  const noSignalSec = m ? Number(m[1]) : 0
  report.noSignalSeconds = noSignalSec
  const allowedNoSignal = Math.max(0.5, Number(res.durationSeconds) * 0.5)
  if (noSignalSec > allowedNoSignal) {
    report.error =
      `成片里有 ${noSignalSec} 秒是补的 NO SIGNAL（允许 ${allowedNoSignal.toFixed(1)} 秒）` +
      `，分片只用上了 ${res.segments ?? 0} 个。detail: ${detail}`
    return finish(report, win)
  }

  report.ok = true
  return finish(report, win)
}

/** 等一次导出收尾（自检 --gui-export 用）。
 *
 *  认两种事件：
 *    * `export_result` —— 服务自己发起的导出（老路径，服务 stdin 命令通道）。
 *    * `export_status` —— 主进程轮询插件报的导出进度（界面点导出走这条，
 *      因为导出已经搬进插件、不再经过服务，服务根本不知道有这回事）。
 *  漏认 export_status 会让自检永远等不到结果 —— 这正是它当时报
 *  "点了导出，30 秒内没等到 export_result 事件" 的原因。
 *
 *  注意不能只认 `type === 'export_result'`：服务在有状态变化时都会补一条，
 *  而 jobId 从 0 变成 1 的那次比较会产出一条**内容全空的**结果
 *  （ok=false、failed=false、outPath=""），把它当成"导出结束"会误判成失败。
 *  所以这里只认"这次导出真的收尾了"的两条：ok 或 failed 之一为真。
 */
function waitForExportResult(timeoutMs) {
  return new Promise((resolve) => {
    const t0 = Date.now()
    const tick = setInterval(() => {
      const r = live.exportEvents
        .filter((e) => e.type === 'export_result' || e.type === 'export_status')
        .filter((e) => e.running !== true)
        .at(-1)
      if (r && (r.ok === true || r.failed === true)) {
        clearInterval(tick)
        resolve(r)
        return
      }
      if (Date.now() - t0 > timeoutMs) {
        clearInterval(tick)
        resolve(null)
      }
    }, 200)
  })
}

function readFirstBytes(file, n) {
  const { openSync, readSync, closeSync } = require('node:fs')
  const buf = Buffer.alloc(n)
  const fd = openSync(file, 'r')
  try {
    readSync(fd, buf, 0, n, 0)
  } finally {
    closeSync(fd)
  }
  return buf
}

function finish(report, win) {
  report.ok = Boolean(report.ok)
  report.steps.push({ name: 'summary', ok: report.ok, error: report.error ?? null })
  try {
    stopService()
  } catch {
    /* 已经退了 */
  }
  try {
    mkdirSync(ARTIFACT_DIR, { recursive: true })
    const outFile = path.join(ARTIFACT_DIR, 'gui-export-report.json')
    writeFileSync(outFile, JSON.stringify(report, null, 2), 'utf8')
    report.reportPath = outFile
  } catch (err) {
    logErr('[gui-export] 写报告失败：', err.message)
  }
  log(`[gui-export] ok=${report.ok} ${report.error ?? ''}`)
  try {
    win.destroy()
  } catch {
    /* 窗口可能已经关了 */
  }
  return report.ok ? 0 : 1
}

// ---------------------------------------------------------------------------
// 启动
// ---------------------------------------------------------------------------
if (!app.requestSingleInstanceLock()) {
  app.quit()
} else {
  app.on('second-instance', () => {
    // 已经有界面在跑：只把这一份叫到前台（不新建窗口、不重启）。
    showMainWindow()
  })

  app.whenReady().then(async () => {
    if (IS_PROBE) {
      app.exit(runProbe())
      return
    }

    registerIpc()

    if (IS_SELFTEST) {
      const code = await runSelfTest(SELFTEST_SECONDS)
      app.exit(code)
      return
    }

    if (IS_GUI_EXPORT) {
      const code = await runGuiExport(GUI_EXPORT_WAIT)
      app.exit(code)
      return
    }

    if (IS_EXTERNAL_PROBE) {
      const code = await runExternalProbe(EXTERNAL_SECONDS)
      app.exit(code)
      return
    }

    // -------------------------------------------------------------------
    // 自动拉起采集服务（用户要求：界面启动时如果服务没跑，直接把它起好，
    // 不要再让人"另开一个终端 npm run service"）。
    //
    // launchService() 自己就会判断"已经在跑就复用"，所以这里无条件调是安全的：
    //   * 服务没在跑  -> 它 spawn（detached，界面退出也常驻，见那边的注释）；
    //   * 服务已在跑  -> 它走 alreadyRunning 分支，只把设置下发一遍，什么也不动。
    //
    // 只在**正常界面模式**做。--selftest / --gui-export 那些验收模式自己管
    // 服务生死（finish() 里会 stopService），插一脚反而会把它们的干净环境搅乱。
    // -------------------------------------------------------------------
    if (!IS_SELFTEST && !IS_GUI_EXPORT && !IS_EXTERNAL_PROBE) {
      const started = launchService()
      if (!started.ok) {
        // exe 没编译出来之类：日志里说清楚，界面那边还会显示原有的错误遮罩，
        // 不弹窗也不退界面 —— 设置页/停止按钮仍然可用。
        log('自动启动采集服务失败：', started.error ?? '(无错误信息)')
      }
    }

    // 守护进程**不在这里拉起**：它是采集服务自己 spawn 的（shadow-capture
    // --guard，见 native/backend/GuardianMode.h）—— 服务拉守护、守护盯服务，
    // 两条边都在服务端，关掉界面之后照样成立。界面不再显示它的状态（那是服务
    // 端的实现细节），只是仍然依赖它把崩溃的服务拉回来。
    createWindow()

    // 守着反向通知管道：服务托盘「退出」时通过它请界面一起退出。
    // 必须在 createWindow 之后起 —— 服务连上来就是要关窗口，窗口得先在。
    startGuiPipe()

    // 界面一开就把设置推给在跑的服务，**不管服务是谁起的**。
    //
    // 之前只在两条路径上下发：launchService 刚 spawn 完、以及用户点保存时。
    // 于是最常用的那条路（`npm start` / `npm run service` 先把服务起来，
    // 界面再连上）永远不会下发 —— 设置页里调成 60，服务那边根本不知道，
    // 预览照样按"不限"跑（实测：设 60，界面画出 80.8fps）。
    // 这里补一次兜底下发：拿不到管道就静默放弃，界面照常可用。
    void pushSettingsToService()

    app.on('activate', () => {
      if (BrowserWindow.getAllWindows().length === 0) createWindow()
    })
  })

  app.on('window-all-closed', () => {
    // 采集服务是 detached 的常驻进程：界面退出**不**停服、不杀它。
    // 停服走界面里的「停止采集服务」或 npm run service:stop。
    app.quit()
  })

  // 退出时摘掉自己的 pid 文件：托盘下次点击才知道"界面已经不在了，该拉一个"。
  // 放在 will-quit 而不是 window-all-closed —— app.quit() 之后还有收尾流程，
  // 这里删掉也只是让托盘的判断早一点点，不影响界面继续退出。
  app.on('will-quit', removeGuiPid)
}
