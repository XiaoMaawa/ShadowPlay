// scripts/launch.mjs — 启动 Electron 界面（并确保采集服务已就绪）
//
//   npm start                   前台运行，日志直接打在当前控制台（Ctrl+C 退出）
//   npm run start:bg            后台启动，脚本立刻返回
//   npm run probe               只跑共享内存链路自检，不弹窗口
//
// 为什么不用 electron CLI：
//   本机 npm 包里的 electron 二进制是通过 ELECTRON_OVERRIDE_DIST_PATH 指过去的，
//   而且某些环境（例如带沙箱的终端）会注入 ELECTRON_RUN_AS_NODE=1 ——
//   那会让 electron.exe 退化成纯 Node（require('electron') 只返回一个路径字符串）。
//   这里统一把环境摆正再直接调 electron.exe。
//
// ELECTRON_RUN_AS_NODE 必须**删除**，不能置空：置成空字符串同样会退化成 Node 模式。

import { spawn, spawnSync } from 'node:child_process'
import { existsSync, readFileSync, writeFileSync, rmSync, openSync, statSync } from 'node:fs'
import { open as fsOpen } from 'node:fs/promises'
import path from 'node:path'
import { createRequire } from 'node:module'
import { projectRoot, tempRoot, electronDistPath, assertElectronDist } from './electron-path.mjs'

const require = createRequire(import.meta.url)
const { TEMP_ROOT } = require('../app/temp-const.cjs')

// ---------------------------------------------------------------------------
// 控制台中文乱码
//
// 现象：`[main] 宸叉竻鐞嗕笂娆¤繍琛岀殑涓存椂浜х墿` —— 这是"已清理上次运行的
// 临时产物"的 UTF-8 字节被控制台按 GBK(936) 解出来的样子。
//
// 根因：
//   · Node 自己写控制台走 WriteConsoleW（UTF-16），**不看代码页**，所以
//     `[launch] 采集服务已启动` 这类行一直是正常的 —— 千万别拿它当"代码页已经
//     是 UTF-8"的证据；
//   · Electron 主进程是直接 WriteFile 到控制台句柄的**原始 UTF-8 字节**，
//     控制台按当前代码页（中文 Windows 默认 936）解码 → 中文全成乱码。
//
// 两道保险，缺一不可：
//   1. 这里把控制台代码页切到 65001（对第 2 步之外的原生程序也有用）；
//   2. 启动界面时把 stdout/stderr 接成管道，用 Node 转写一遍（见文件末尾）——
//      这一步不依赖代码页，是真正保证不乱码的那道。
//
// 【windowsHide 千万别加】加上它 Node 会用 CREATE_NO_WINDOW 给 cmd.exe 开一个
// **新的隐藏控制台**，chcp 改的是那个一次性控制台，父控制台纹丝不动 —— 之前
// "改了还是乱码"就是这个原因。不带它时 cmd 会继承父控制台，才改得动。
// ---------------------------------------------------------------------------
if (process.platform === 'win32' && process.stdout.isTTY) {
  try {
    spawnSync('cmd', ['/c', 'chcp', '65001'], { stdio: 'ignore' })
  } catch {
    /* 切不了也不影响功能：下面还有管道转写那道保险 */
  }
}

const exe = assertElectronDist()
// 直接跑编译产物本身，不复制副本。
//
// 以前这里（和 app/main.cjs）会把 exe 拷到 %TEMP%/shadow-play/app/bin/ 再启动，
// 理由是"仓库目录可能带低完整性标签"。实际那条路带来的是两个更难查的问题：
// 副本被启动清理删掉后路径永久失效（界面一直「正在连接…」、服务起不来），
// 以及服务在跑时副本不能刷新（重编译后"改了没生效"）。只跑一份编译产物就没
// 这些事：路径恒定，重编译后重启服务必生效。
const BACKEND = path.join(projectRoot, 'native', 'build', 'shadow-capture.exe')
const ARTIFACT_DIR = path.join(projectRoot, 'native', 'build', 'artifacts')
const REPORT = path.join(ARTIFACT_DIR, 'probe-report.json')

const passThrough = process.argv.slice(2).filter((a) => a !== '--bg')
const isProbe = passThrough.includes('--probe')
const isSelftest = passThrough.includes('--selftest')
const detach = process.argv.includes('--bg') || process.env.SHADOW_DETACH === '1'
const backendMissing = !existsSync(BACKEND)
const SELFTEST_REPORT = path.join(ARTIFACT_DIR, 'selftest-report.json')
const GUI_LOG = path.join(ARTIFACT_DIR, 'gui.log')

/**
 * 打开一个日志文件用于 detached 子进程的 stdout/stderr。
 * Electron 是 GUI 子系统程序，控制台输出经常拿不到，所以落到文件里方便排查。
 */
function openLogFile(append = true) {
  try {
    return openSync(GUI_LOG, append ? 'a' : 'w')
  } catch {
    return 'ignore'
  }
}

// 传给 Electron 的固定开关：
//   --disable-breakpad / --disable-crash-reporter
//       受限终端里崩溃上报组件会去写它拿不到的位置，表现为进程直接以
//       0x80000003 退出、什么日志都看不到。
//   --no-sandbox（SHADOW_NO_SANDBOX=1 时追加）
//       某些容器/沙箱环境下渲染进程创建子进程需要管道会被拒绝。
const CHILD_FLAGS = ['--disable-breakpad', '--disable-crash-reporter']
if (process.env.SHADOW_NO_SANDBOX === '1') CHILD_FLAGS.push('--no-sandbox')

// --cdp <port>：打开 Chromium 调试端口，让脚本能通过 CDP 直接驱动界面。
//
// 【为什么需要】`scripts/test-reload.mjs` 靠 CDP 走"界面那条单路保存通道"
// （`window.shadowPreview.saveProfile`）来验证"保存只重载改动的那一路、绝不
// 重启服务"。以前这里从来没传过 `--remote-debugging-port`，所以那个测试
// 一连 9222 就 ECONNREFUSED、永远跑不起来 —— 不是产品坏了，是测试缺了这个开关。
//
// 【默认不开】调试端口等于把界面完全暴露给本机任何进程（能读写页面、能调
// 所有 IPC）。只在显式传 `--cdp` 时才开，日常 `npm start` 保持关闭。
const cdpIdx = passThrough.indexOf('--cdp')
if (cdpIdx >= 0) {
  const value = passThrough[cdpIdx + 1]
  if (value !== undefined && Number.isFinite(Number(value)) && Number(value) > 0) {
    CHILD_FLAGS.push(`--remote-debugging-port=${Number(value)}`)
    console.log(`[launch] 已打开调试端口 ${Number(value)}（CDP 测试用）`)
  } else {
    console.warn('[launch] --cdp 后面要跟一个端口号，本次忽略')
  }
  passThrough.splice(cdpIdx, value === undefined ? 1 : 2)
}

const env = { ...process.env }
delete env.ELECTRON_RUN_AS_NODE
env.ELECTRON_OVERRIDE_DIST_PATH = electronDistPath

// --render-fps <n>：界面侧绘制上限（0 = 不限，跟随刷新率）。默认 60。
// 从 argv 里摘出来单独传给 Electron，不让它当作 Electron 自己的开关解析。
const renderFpsIdx = passThrough.indexOf('--render-fps')
if (renderFpsIdx >= 0) {
  const value = passThrough[renderFpsIdx + 1]
  if (value !== undefined && Number.isFinite(Number(value))) {
    env.SHADOW_RENDER_FPS = value
  } else {
    console.warn('[launch] --render-fps 后面要跟一个数字（0 = 不限），本次忽略')
  }
  passThrough.splice(renderFpsIdx, value === undefined ? 1 : 2)
}
if (env.SHADOW_RENDER_FPS === undefined) env.SHADOW_RENDER_FPS = '60'

// --producer-fps <n>：采集端发布上限（0 = 不限制）。默认交给服务自己的 60。
// 注意：界面侧再怎么解锁，最终也超不过采集端发布的帧率；想真的跑满高刷屏，
// 采集端这边也得跟着提上去（实测 4K 桌面复制能出到 ~90fps）。
const producerFpsIdx = passThrough.indexOf('--producer-fps')
let producerFps = null
if (producerFpsIdx >= 0) {
  const value = passThrough[producerFpsIdx + 1]
  if (value !== undefined && Number.isFinite(Number(value))) {
    producerFps = value
  } else {
    console.warn('[launch] --producer-fps 后面要跟一个数字（0 = 不限），本次忽略')
  }
  passThrough.splice(producerFpsIdx, value === undefined ? 1 : 2)
}

// --profiles <path>：指定配置文档。
//
// 主要给对照实验用：不同的配置集合（单路/多路）能直接对比行为，不必改
// 项目根目录的 profiles.json。设进 env 后下面拉起服务时也会跟着用它。
const profilesIdx = passThrough.indexOf('--profiles')
if (profilesIdx >= 0) {
  const value = passThrough[profilesIdx + 1]
  if (value !== undefined) {
    env.SHADOW_PROFILES = path.resolve(projectRoot, value)
    console.log(`[launch] 指定配置文档：${env.SHADOW_PROFILES}`)
  } else {
    console.warn('[launch] --profiles 后面要跟一个路径，本次忽略')
  }
  passThrough.splice(profilesIdx, value === undefined ? 1 : 2)
}

// --fresh：把这次运行要用的录制根目录清空。
//
// 为什么需要：分片是**按回放窗口环形覆盖**的，目录里的旧分片会让"界面上第几段"
// 这类证据带上一次运行的残留（实测自检一开就是 60 段 / 355MB，来自上个小时的
// 跑动）。验收要的是"这一次跑出来的数字"，所以给个开关让每次从空目录开始。
const freshIdx = passThrough.indexOf('--fresh')
if (freshIdx >= 0) {
  env.SHADOW_RECORD_ROOT = path.join(tempRoot, 'records', '_fresh')
  console.log(`[launch] 本次录制根目录：${env.SHADOW_RECORD_ROOT}（启动前清空）`)
  passThrough.splice(freshIdx, 1)
}

/**
 * 某个 pid 现在还活着吗。
 *
 * 用 `process.kill(pid, 0)`：信号 0 不投递任何信号，只做权限/存在性检查。
 * 进程不存在抛 ESRCH；存在但没权限抛 EPERM（那也是活着）。
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

/**
 * 采集服务在不在跑。
 *
 * 两条判据，**必须都看**：
 *   1. `shadow-capture --status` 的退出码（互斥体）；
 *   2. 状态文件 status-v1.json 是不是新鲜，**而且它记的 pid 还活着**。
 *
 * 为什么不能只看第 1 条：互斥体是进程级的内核对象，进程刚退出、或者被杀掉
 * 之后还有一瞬间能拿到句柄，`--status` 会骗人说在跑；而"上一轮的服务已经
 * 死了、这一轮该起一个新的"这种场景，靠一个会抖的退出码判断太脆。
 *
 * 为什么第 2 条还要验 pid：状态文件每秒刷一次，**进程被杀之后文件仍然新鲜**
 * （mtime 还是刚才那次写入）。只看 mtime 的话，`Stop-Process` 之后 5 秒内
 * 再启动就会误判"服务还在跑"，于是跳过拉起 —— 紧接着 probe 报
 * 「打不开共享内存帧环」。这是真踩到过的间歇性失败，见 AUDIT.md。
 */
function serviceStatus() {
  if (backendMissing) return { running: false }
  try {
    // 只看退出码，避免依赖子进程管道（受限环境里管道创建可能被拒）
    const r = spawnSync(BACKEND, ['--status'], { stdio: 'ignore', windowsHide: true })
    if (r.status === 0) return { running: true, how: 'mutex' }
  } catch {
    // 落到下面看状态文件
  }
  const file = path.join(TEMP_ROOT, 'status', 'status-v1.json')
  try {
    const st = statSync(file)
    if (Date.now() - st.mtimeMs < 5000) {
      let pid = 0
      try {
        pid = Number(JSON.parse(readFileSync(file, 'utf8')).pid) || 0
      } catch {
        pid = 0
      }
      if (pidAlive(pid)) return { running: true, how: 'status-file' }
    }
  } catch {
    // 文件不存在 = 服务确实没跑
  }
  return { running: false }
}

/**
 * 把采集服务拉起来（普通权限的后台独立进程，**不申请任何权限**）。
 *
 * 以前这里会申请管理员权限，理由是"窗口捕获（WGC）抓别的进程窗口需要提权"。
 * 实测这个前提不成立：普通权限的服务抓资源管理器窗口完全正常（窗口那一路
 * live + 正在录），而提权带来一串副作用 —— 每次 `npm start` 弹 UAC（有一阵
 * 甚至弹两次：一次这里、一次服务抓不到窗口后自己重拉自己）、进程变成 High
 * 完整性级别后普通权限的 taskkill 停不掉、出问题时看不到它的窗口。
 *
 * 现在这条路干净了：服务用当前权限起，detached + unref 保证它不属于这个
 * 进程，关掉终端/关掉界面它都继续跑。起不来就直说，不再兜着提权重试。
 *
 * @returns {{ ok: boolean, how: string }}
 */
function launchService(svcArgs) {
  const status = serviceStatus()
  if (status.running) return { ok: true, how: 'already-running' }

  const svc = spawn(BACKEND, svcArgs, {
    cwd: projectRoot,
    detached: true,
    stdio: 'ignore',
    windowsHide: true,
    // 把当前 node 的绝对路径告诉服务：托盘「打开主界面」要从服务这边重新
    // 拉起界面，它得知道拿什么去跑 launch.mjs（nvm/便携版 Node 不在 PATH
    // 里时按名字找不到，只能靠这个）。
    env: { ...process.env, SHADOW_NODE_EXE: process.execPath },
  })
  svc.unref()
  if (waitForService(6000)) {
    writeServicePid(svc.pid)
    return { ok: true, how: 'plain' }
  }
  // 服务自己可能刚好在起（慢半拍），再看一眼
  if (serviceStatus().running) return { ok: true, how: 'already-running' }
  return { ok: false, how: 'failed' }
}

/**
 * 把服务 PID 记到 %TEMP%\shadow-play\app\service.pid。
 *
 * 为什么需要：机器上可能同时跑着别的 shadow-capture，按名字杀会误伤。记下
 * PID 之后 `npm run service:stop` 能精确地停掉这一个。
 *
 * pid 可能拿不到，那就退回读服务自己写进状态文件的 `pid` —— 那个一定准。
 */
function writeServicePid(pid) {
  let real = Number.isFinite(pid) && pid > 0 ? pid : null
  if (real === null) {
    try {
      const st = JSON.parse(
        readFileSync(path.join(TEMP_ROOT, 'status', 'status-v1.json'), 'utf8'),
      )
      const n = Number(st?.pid)
      if (Number.isFinite(n) && n > 0) real = n
    } catch {
      // 状态文件还没写出来，就这次先不记
    }
  }
  if (real === null) return
  try {
    writeFileSync(path.join(TEMP_ROOT, 'service.pid'), String(real), 'utf8')
  } catch {
    // 记不下来不影响服务本身，停服务时退回按名字杀
  }
}

/**
 * 等服务真的可用。
 *
 * 跟 serviceStatus() 一样两条判据一起看：服务启动后每秒写一次
 * `%TEMP%\shadow-play\app\status\status-v1.json`，文件新鲜 + 互斥体打通
 * 才算真的起来了。
 */
function waitForService(timeoutMs) {
  const deadline = Date.now() + timeoutMs
  while (Date.now() < deadline) {
    if (serviceStatus().running) return true
    msleep(200)
  }
  return serviceStatus().running
}

/** 同步小睡。launch.mjs 主体是顶层 await，但等待服务就绪这几处保持同步更简单。 */
function msleep(ms) {
  const end = Date.now() + ms
  while (Date.now() < end) {
    // 空转，最多 150ms
  }
}

// ---------------------------------------------------------------------------
// 采集服务
// ---------------------------------------------------------------------------
if (backendMissing) {
  console.warn(
    '[launch] 警告：采集服务还没编译（native\\build\\shadow-capture.exe）。\n' +
      '         界面能打开，但要先执行 npm run build:native 才能出画面。',
  )
}

// 界面产物也必须先构建。以前漏了这条检查：dist/ 缺了的时候界面窗口会打开
// 但白屏，主进程只打一句"加载开发服务器失败"（因为非开发模式也去连
// 127.0.0.1:5173 了），看上去像网络问题。现在启动前就说清楚。
const webEntry = path.join(TEMP_ROOT, 'dist', 'index.html')
if (!existsSync(webEntry) && !process.env.DEV_SERVER_URL) {
  console.warn(
    '[launch] 警告：界面产物还没构建（' +
      webEntry +
      '）。\n' +
      '         先执行 npm run build:web，或者 npm run build 一次编完原生 + 界面。',
  )
}

if (!backendMissing && !serviceStatus().running) {
  // 启动时清掉可能残留的停机哨兵（上次干净退出留下的），免得它挡住本次启动。
  try {
    rmSync(path.join(tempRoot, 'status', 'shutdown-requested'), { force: true })
  } catch {}
  console.log('[launch] 采集服务没在运行，先拉起它…')
  // 有 profiles.json 就让服务跑多会话模式；否则退回单路模式。
  //
  // 这一条以前漏了：服务不带 --profiles 时是单会话，界面里配了几路它都
  // 只建一个环 —— 表现是"多路预览永远只有一路"，而配置看起来完全正常。
  const profPath = process.env.SHADOW_PROFILES || path.join(projectRoot, 'profiles.json')
  const hasProfiles = existsSync(profPath)
  const svcArgs = hasProfiles
    ? ['--profiles', profPath]
    : producerFps === null
      ? []
      : ['--fps', producerFps]
  if (hasProfiles) console.log(`[launch] 多会话模式：--profiles ${profPath}`)
  const launched = launchService(svcArgs)
  if (launched.ok && launched.how !== 'already-running') {
    console.log('[launch] 采集服务已启动（独立进程，关掉界面后继续跑）。')
  } else if (!launched.ok) {
    console.warn(
      '[launch] 采集服务没能起来。界面照样会打开，但不会有画面。\n' +
        '         想看它到底报了什么错：npm run service -- --fg',
    )
  }

  // 把用户设置里存的预览帧率上限下发一遍。
  //
  // 服务进程读不到 Electron 那边的 settings.json —— 它只认命令行参数和
  // 配置文件，预览上限（previewMaxFps）只能走命令通道告诉它。而"服务是
  // 这个脚本起的"这条路径以前完全没下发：于是 `npm start` 起的服务永远
  // 按 0（不限）跑，设置页里调成 60 也没用（实测设置 60、界面画出 80.8fps，
  // 排查了半天才发现是这里漏了）。
  //
  // 这里只读、失败就静默跳过：预览上限是"锦上添花"的设置，不能因为它
  // 拖住界面启动。管道还没建好也要重试几次（服务起后约几百毫秒才建好）。
  try {
    const settingsFile = path.join(TEMP_ROOT, 'settings.json')
    if (existsSync(settingsFile)) {
      const s = JSON.parse(readFileSync(settingsFile, 'utf8'))
      const raw = Number(s?.previewMaxFps)
      if (Number.isFinite(raw)) {
        // 上限跟录制侧一致（120，见 MfEncoder::kMaxFps）
        const fps = Math.max(0, Math.min(120, Math.round(raw)))
        const pipeName = '\\\\.\\pipe\\ShadowPlay.Cmd.v1'
        const line = JSON.stringify({ cmd: 'set-preview-fps', fps }) + '\n'
        for (let i = 0; i < 25; i++) {
          try {
            const h = await fsOpen(pipeName, 'w')
            try {
              await h.write(line)
            } finally {
              await h.close()
            }
            console.log(`[launch] 预览帧率上限已下发：${fps === 0 ? '不限' : fps + ' fps'}`)
            break
          } catch {
            // 管道还没建好，或者刚好被别的命令占着 —— 等一下再来
            await new Promise((r) => setTimeout(r, 100))
          }
        }
      }
    }
  } catch (err) {
    console.log('[launch] 预览帧率上限没下发成功（不影响录制）：', err?.message ?? err)
  }
}

// ---------------------------------------------------------------------------
// 自检模式
// ---------------------------------------------------------------------------
if (isProbe) {
  try {
    rmSync(REPORT, { force: true })
  } catch {
    /* 忽略 */
  }

  // `--session <id>` 要透传给自检：多会话模式下每一路各有自己的环名（就是配置
  // id），不指定就只能靠状态文件猜。这里保留原样的两个参数，Electron 主进程
  // 的 `runProbe()` 自己解析。
  const probeArgs = ['.', '--probe', '--disable-gpu', '--no-sandbox', ...CHILD_FLAGS]
  const probeSessionIdx = passThrough.indexOf('--session')
  if (probeSessionIdx >= 0) {
    const sessionValue = passThrough[probeSessionIdx + 1]
    if (sessionValue && !sessionValue.startsWith('-')) {
      probeArgs.push('--session', sessionValue)
    }
  }

  const child = spawn(exe, probeArgs, {
    cwd: projectRoot,
    detached: true,
    stdio: 'ignore',
    env,
  })
  child.unref()
  console.log('[launch] 已启动 Electron 自检，等待报告…')

  const deadline = Date.now() + 60000
  let report = null
  while (Date.now() < deadline) {
    if (existsSync(REPORT)) {
      try {
        const text = readFileSync(REPORT, 'utf8')
        if (text.trim().endsWith('}')) {
          report = JSON.parse(text)
          break
        }
      } catch {
        /* 还在写 */
      }
    }
    await new Promise((r) => setTimeout(r, 300))
  }

  if (!report) {
    console.error('[launch] 等待自检报告超时；检查 native\\build\\artifacts\\probe-report.json')
    process.exit(1)
  }

  console.log('\n===== 自检结果 =====')
  for (const step of report.steps ?? []) {
    console.log(`- ${step.name}: ${JSON.stringify(step)}`)
  }
  if (report.error) console.error('错误：', report.error)
  console.log(report.ok ? '\n结果：通过' : '\n结果：失败')
  process.exit(report.ok ? 0 : 1)
}

// ---------------------------------------------------------------------------
// 界面自检模式：真的开界面跑几秒，然后看渲染进程画了多少帧
// ---------------------------------------------------------------------------
if (isSelftest) {
  try {
    rmSync(SELFTEST_REPORT, { force: true })
  } catch {
    /* 忽略 */
  }

  const child = spawn(exe, ['.', ...passThrough, '--no-sandbox', '--enable-logging', ...CHILD_FLAGS], {
    cwd: projectRoot,
    detached: true,
    // 同前台模式：走管道由本进程转写，避免 Electron 的原始 UTF-8 字节被按 GBK 解。
    stdio: ['ignore', 'pipe', 'pipe'],
    env,
  })
  child.stdout.setEncoding('utf8')
  child.stderr.setEncoding('utf8')
  child.stdout.on('data', (s) => process.stdout.write(s))
  child.stderr.on('data', (s) => process.stderr.write(s))
  child.unref()
  console.log('[launch] 已启动界面自检，等待报告…')

  const deadline = Date.now() + 60000
  let report = null
  while (Date.now() < deadline) {
    if (existsSync(SELFTEST_REPORT)) {
      try {
        const text = readFileSync(SELFTEST_REPORT, 'utf8')
        if (text.trim().endsWith('}')) {
          report = JSON.parse(text)
          break
        }
      } catch {
        /* 还在写 */
      }
    }
    await new Promise((r) => setTimeout(r, 300))
  }

  if (!report) {
    console.error('[launch] 等待界面自检报告超时；检查 native\\build\\artifacts\\selftest-report.json')
    process.exit(1)
  }

  console.log('\n===== 界面自检结果 =====')
  console.log(JSON.stringify(report, null, 2))
  console.log(report.ok ? '\n结果：通过' : '\n结果：失败')
  process.exit(report.ok ? 0 : 1)
}

// ---------------------------------------------------------------------------
// 正常启动界面
// ---------------------------------------------------------------------------
if (detach) {
  // 输出落到 native\build\artifacts\gui.log。
  //
  // 【为什么不是 stdio:'ignore'】这条路不只是在终端里跑 `npm run start:bg`：
  // 托盘「打开主界面」也是走它拉起来的（见 native/backend/TrayIcon.cpp），
  // 那时候界面上根本没有控制台，日志一丢就完全没法查"为什么窗口没起来"。
  const logFd = openLogFile()
  const child = spawn(exe, ['.', ...passThrough, ...CHILD_FLAGS], {
    cwd: projectRoot,
    detached: true,
    stdio: ['ignore', logFd, logFd],
    env,
  })
  child.unref()
  // 先把界面 pid 落给托盘：它靠这个文件区分"正在启动"和"已经被用户叉掉"
  // （界面自己启动后也会写一次同值，见 app/main.cjs 的 GUI_PID_FILE）。
  try {
    writeFileSync(path.join(TEMP_ROOT, 'gui.pid'), String(child.pid), 'utf8')
  } catch {
    /* 写不了只是托盘判断慢一点，不影响启动 */
  }
  console.log(`[launch] 界面已在后台启动（pid ${child.pid}）。`)
  process.exit(0)
}

// 前台模式：日志直接打到当前控制台，Ctrl+C 一起退出。
// 采集服务是独立进程，不受影响。
console.log('[launch] 启动界面…（Ctrl+C 退出；采集服务会继续在后台运行）')
console.log(`[launch] 命令：electron . ${passThrough.concat(CHILD_FLAGS).join(' ')}`)

// stdout/stderr 走管道、由本进程转写一遍（而不是直接 inherit）。
//
// 【为什么】Electron 主进程往控制台写的是原始 UTF-8 字节，控制台按自己的代码页
// （中文 Windows 默认 936/GBK）解码 → `[main]` 的中文全变成「宸叉竻鐞…」。
// 而 Node 写控制台用的是 WriteConsoleW（UTF-16），**跟代码页无关**，所以只要让
// 输出先经过 Node 一次（按 UTF-8 解码成字符串再写出），显示就一定正确。
// 这套不依赖 chcp，是真正兜底的那道保险。
const child = spawn(exe, ['.', ...passThrough, ...CHILD_FLAGS], {
  cwd: projectRoot,
  stdio: ['inherit', 'pipe', 'pipe'],
  env,
})

// setEncoding('utf8') 用的是 StringDecoder，跨 data 事件的多字节字符不会截断成乱码。
child.stdout.setEncoding('utf8')
child.stderr.setEncoding('utf8')
child.stdout.on('data', (s) => process.stdout.write(s))
child.stderr.on('data', (s) => process.stderr.write(s))

process.on('SIGINT', () => {
  try {
    child.kill('SIGINT')
  } catch {
    /* 忽略 */
  }
})
process.on('SIGTERM', () => {
  try {
    child.kill('SIGTERM')
  } catch {
    /* 忽略 */
  }
})

child.on('exit', (code, signal) => {
  if (signal) console.log(`\n[launch] 界面被信号 ${signal} 结束`)
  else console.log(`\n[launch] 界面已退出（code=${code}）`)
  process.exit(code ?? 0)
})

child.on('error', (err) => {
  console.error('[launch] 启动界面失败：', err.message)
  process.exit(1)
})
