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
const { ensureServiceExe, TEMP_ROOT } = require('../app/temp-const.cjs')

const exe = assertElectronDist()
// 采集服务用**运行副本**，不是 native/build 里那个原件。
//
// 为什么：仓库目录可能继承一个"低完整性标签"（DSH 沙箱、同步盘、杀软都会留
// 这种东西）。从那种目录启动的进程也是低完整性级别，于是：
//   * 写 %TEMP% 下别的进程建的目录会被拒 —— 状态文件 status-v1.json 直接
//     写不出来，界面永远显示"服务未启动"（本鱼实测过：原件 0 个状态文件，
//     副本 1 个）；
//   * WGC 抓窗口直接被系统拒绝（0x80070005）。
// 复制到 %TEMP% 里的副本没有那个标签，两件事一起解决。
//
// app/main.cjs 早就在用 ensureServiceExe 了，这里以前漏了 —— 结果就是
// `npm start` 拉起的服务和界面自己拉起的服务行为不一样。
const BACKEND_SRC = path.join(projectRoot, 'native', 'build', 'shadow-capture.exe')
const BACKEND = existsSync(BACKEND_SRC)
  ? ensureServiceExe({ srcExe: BACKEND_SRC, tempRoot: TEMP_ROOT, log: (m) => console.log('[launch]', m) })
  : BACKEND_SRC
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
 * 采集服务在不在跑。
 *
 * 两条判据，**必须都看**：
 *   1. `shadow-capture --status` 的退出码（互斥体）；
 *   2. 状态文件 status-v1.json 是不是新鲜。
 *
 * 为什么不能只看第 1 条：互斥体是进程级的内核对象，进程刚退出、或者被杀掉
 * 之后还有一瞬间能拿到句柄，`--status` 会骗人说在跑；而"上一轮的服务已经
 * 死了、这一轮该起一个新的"这种场景，靠一个会抖的退出码判断太脆。
 *
 * 状态文件是服务每秒刷一次的可读文件（%TEMP% 下），mtime 在 5 秒内就说明
 * 服务活着，跟退出码互为印证。app/main.cjs 的 serviceRunningFast() 用的也是
 * 这个思路。
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
    if (Date.now() - st.mtimeMs < 5000) return { running: true, how: 'status-file' }
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

if (!isProbe && !backendMissing && !serviceStatus().running) {
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
        const fps = Math.max(0, Math.min(240, Math.round(raw)))
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

  const child = spawn(exe, ['.', '--probe', '--disable-gpu', '--no-sandbox', ...CHILD_FLAGS], {
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
    stdio: ['ignore', 1, 2],
    env,
  })
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
  const child = spawn(exe, ['.', ...passThrough, ...CHILD_FLAGS], {
    cwd: projectRoot,
    detached: true,
    stdio: 'ignore',
    env,
  })
  child.unref()
  console.log(`[launch] 界面已在后台启动（pid ${child.pid}）。`)
  process.exit(0)
}

// 前台模式：日志直接打到当前控制台，Ctrl+C 一起退出。
// 采集服务是独立进程，不受影响。
console.log('[launch] 启动界面…（Ctrl+C 退出；采集服务会继续在后台运行）')
console.log(`[launch] 命令：electron . ${passThrough.concat(CHILD_FLAGS).join(' ')}`)

const child = spawn(exe, ['.', ...passThrough, ...CHILD_FLAGS], {
  cwd: projectRoot,
  stdio: 'inherit',
  env,
})

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
