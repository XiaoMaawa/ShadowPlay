// scripts/service.mjs — 管理独立运行的 C++ 采集服务
//
//   node scripts/service.mjs start    启动（后台独立进程，不是这个脚本的子进程）
//   node scripts/service.mjs status   查看是否在运行
//   node scripts/service.mjs stop     停止
//
// start 后面的多余参数会原样转给采集服务，例如：
//   npm run service -- --fps 120    发布上限提到 120fps
//   npm run service -- --fps 0      不限制（桌面有变化就发）
//
// start 默认会带上 `--profiles <仓库里的 profiles.json>`，跟界面（app/main.cjs
// 的 launchService）起服务时用的是同一套参数。**这个默认值不能省**：
// 不带 --profiles 时服务走的是"单路旧模式"，那套模式里
//   * 没有录制、没有导出（需求(5)(6) 的能力全都不在）；
//   * 更要紧的是它压根不写状态文件（见 native/backend/main.cpp 的
//     RunProfilesMode —— StartStatusMirror 只在那个分支里调）。
// 于是界面读不到任何状态，顶栏就一直显示"服务未启动"，哪怕服务明明在跑。
//
// 环境变量 SHADOW_PROFILES 可以换成别的配置文件（验收脚本用它指到临时副本，
// 跟界面侧 app/main.cjs 的 profilesPath() 取值规则一致）。
// 想显式跑单路旧模式，用 `npm run service -- --legacy`。
//
// 采集服务是常驻后台进程：关掉界面它继续跑，所以启动/停止是独立操作。
// Electron 界面会自动连上它（见 app/preload.cjs）。
//
// ---------------------------------------------------------------------------
// 关于管理员权限：**不需要，这里也没有**
//
// 以前这套脚本会在 start / stop 时申请提权（PowerShell `Start-Process -Verb
// RunAs`，也就是弹 UAC）。理由是"窗口捕获（WGC）抓别的进程窗口需要管理员"。
// 实测下来这个前提不成立：普通权限的采集服务抓资源管理器窗口完全正常，反而
// 提权带来一串副作用 —— 每次一键启动都弹 UAC（`npm start` 甚至弹两次：一次
// launch.mjs 替服务申请的、一次服务抓不到窗口后自己重拉自己的）、进程变成
// High 完整性级别之后普通权限的 taskkill 停不掉、调试时看不到它。
//
// 所以现在 start / stop 都是纯粹的普通权限操作：起一个 detached 的后台进程，
// 停的时候 taskkill /PID。整条链路一次 UAC 都没有。
// ---------------------------------------------------------------------------

import { spawn, spawnSync } from 'node:child_process'
import { existsSync, readFileSync, rmSync, statSync, writeFileSync } from 'node:fs'
import path from 'node:path'
import { projectRoot, tempRoot } from './electron-path.mjs'

const BACKEND = path.join(projectRoot, 'native', 'build', 'shadow-capture.exe')
const PID_FILE = path.join(tempRoot, 'service.pid')
// 停机哨兵：状态目录下的这个文件一旦存在，服务和守护都该退出且不再互拉。
// 见 native/backend/GuardianMode.h 的 ShutdownRequested 说明。
const SHUTDOWN_SENTINEL = path.join(tempRoot, 'status', 'shutdown-requested')
const cmd = (process.argv[2] ?? 'status').toLowerCase()
// `npm run service -- --fps 120` 里的 --fps 120 会落到 argv[3..]
let extraArgs = process.argv.slice(3)

// `--legacy` 是本地开关，不给服务看（服务不认这个参数，传过去会被当成未知参数）。
const legacy = extraArgs.includes('--legacy')
if (legacy) extraArgs = extraArgs.filter((a) => a !== '--legacy')

// ---------------------------------------------------------------------------
// 配置文件 / 服务状态
// ---------------------------------------------------------------------------

/** 默认配置文件的算法必须和界面一致：app/main.cjs 的 profilesPath() 就是
 *  `process.env.SHADOW_PROFILES || <仓库根>/profiles.json`。两边算不到一块儿去
 *  的话，会出现"界面显示一套配置、服务录的是另一套"，所以这里照抄同一套规则。 */
function profilesPath() {
  return process.env.SHADOW_PROFILES || path.join(projectRoot, 'profiles.json')
}

/** 拼最终传给服务的参数：默认走多配置模式，除非用户自己已经给了 --profiles
 *  或者显式要 --legacy。 */
function backendArgs() {
  if (legacy || extraArgs.includes('--profiles')) return extraArgs
  const file = profilesPath()
  if (!existsSync(file)) {
    console.error(
      `找不到配置文件：${file}\n` +
        `请先在界面上保存一份配置，或用 SHADOW_PROFILES 指定配置文件；\n` +
        `也可以用 npm run service -- --legacy 跑单路旧模式。`,
    )
    process.exit(1)
  }
  return ['--profiles', file, ...extraArgs]
}

if (!existsSync(BACKEND)) {
  console.error(`找不到采集服务：${BACKEND}\n请先执行  npm run build:native`)
  process.exit(1)
}

/**
 * 服务在不在跑。
 *
 * 只依赖退出码，不让 stdout 走管道：某些受限环境无法为子进程创建管道，
 * spawnSync 会直接抛 EPERM / 返回 status=null，那样会被误判成"没在运行"。
 * shadow-capture --status 的约定是：0 = 在运行，1 = 没运行。
 *
 * 退出码不是 0 时再看状态文件（服务每秒写一次 status-v1.json）—— 万一
 * 互斥体那边出岔子，只要状态还在刷新就说明它活着。
 */
function status() {
  try {
    const r = spawnSync(BACKEND, ['--status'], { stdio: 'ignore', windowsHide: true })
    if (r.status === 0) return { running: true, how: 'mutex', exitCode: 0 }
  } catch {
    // 落到下面看状态文件
  }
  try {
    const f = path.join(tempRoot, 'status', 'status-v1.json')
    const age = Date.now() - statSync(f).mtimeMs
    if (age >= 0 && age < 5000) return { running: true, how: 'status-file', exitCode: 0 }
  } catch {
    // 没有状态文件就是没在跑
  }
  return { running: false, how: 'none', exitCode: 1 }
}

/**
 * 读启动服务时记下的 PID（%TEMP%\shadow-play\app\service.pid）。
 *
 * 有 PID 就能精确停掉**本项目的**那一个服务进程。没 PID 时只能按名字
 * `shadow-capture.exe` 杀，机器上要是同时跑着别的采集器就会被误伤。
 */
function readServicePid() {
  try {
    const n = Number(readFileSync(PID_FILE, 'utf8').trim())
    return Number.isFinite(n) && n > 0 ? n : null
  } catch {
    return null
  }
}

/** 记下服务 PID，供 service:stop 精确停止。 */
function writeServicePid(pid) {
  if (!Number.isFinite(pid) || pid <= 0) return
  try {
    writeFileSync(PID_FILE, String(pid), 'utf8')
  } catch {
    // 记不下来不影响服务本身
  }
}

/** 按 PID（首选）或名字结束采集服务。ok=成功；notFound=本来就没跑。 */
function killService() {
  const pid = readServicePid()
  if (pid) {
    const r = spawnSync('taskkill', ['/PID', String(pid), '/F'], { encoding: 'utf8' })
    if (r.status === 0) {
      rmSync(PID_FILE, { force: true })
      return { ok: true, text: `${r.stdout ?? ''}`.trim() }
    }
  }
  const r = spawnSync('taskkill', ['/IM', 'shadow-capture.exe', '/F'], { encoding: 'utf8' })
  const text = `${r.stdout ?? ''}${r.stderr ?? ''}`.trim()
  if (r.status === 0) {
    rmSync(PID_FILE, { force: true })
    return { ok: true, text }
  }
  // taskkill 的"没找到"在不同语言下措辞不同，靠进程是否还在更可靠
  return { ok: false, notFound: !status().running, text }
}

/** 读守护进程 PID（心跳文件里记的）。 */
function readGuardianPid() {
  try {
    const f = path.join(tempRoot, 'status', 'guardian-v1.json')
    const doc = JSON.parse(readFileSync(f, 'utf8'))
    const p = Number(doc?.pid)
    return Number.isFinite(p) && p > 0 ? p : null
  } catch {
    return null
  }
}

/** 立停机哨兵：让守护别再把服务拉起来。 */
function writeShutdownSentinel() {
  try {
    writeFileSync(SHUTDOWN_SENTINEL, '', 'utf8')
  } catch {
    // 写不下也不致命，下面按 PID 强杀兜底
  }
}

/** 清掉停机哨兵（启动前调用，免得残留的哨兵挡住本次启动）。 */
function clearShutdownSentinel() {
  try {
    rmSync(SHUTDOWN_SENTINEL, { force: true })
  } catch {
    /* 没有就忽略 */
  }
}

/**
 * 干净地停掉**整套**（服务 + 守护）。
 *
 * 关键：先立哨兵，再循环杀。守护每 2 秒巡检一次，若不立哨兵，杀服务的瞬间
 * 守护就会把它拉起来，于是"停止"永远停不干净。立了哨兵后，守护下一轮要么直接
 * 退出、要么看到哨兵不再拉起；循环里反复按 PID + 按名字补刀，直到都没了。
 *
 * @returns {boolean} 是否成功停掉
 */
function stopAll() {
  writeShutdownSentinel()

  const deadline = Date.now() + 15000
  let everKilled = false
  while (Date.now() < deadline) {
    const pids = new Set([readServicePid(), readGuardianPid()].filter((p) => p))
    for (const p of pids) {
      spawnSync('taskkill', ['/PID', String(p), '/F'], { stdio: 'ignore', windowsHide: true })
      everKilled = true
    }
    // 名字兜底：万一心跳/pid 文件滞后，把同名的都清掉（单实例互斥体保证只有一套）。
    const byName = spawnSync('taskkill', ['/IM', 'shadow-capture.exe', '/F'], {
      stdio: 'ignore',
      windowsHide: true,
    })
    if (byName.status === 0) everKilled = true

    // 都清干净了就收工
    const r = spawnSync('tasklist', ['/FI', 'IMAGENAME eq shadow-capture.exe'], {
      encoding: 'utf8',
      windowsHide: true,
    })
    if (!/shadow-capture\.exe/i.test(r.stdout || '')) break
    spawnSync('ping', ['-n', '2', '127.0.0.1'], { stdio: 'ignore', windowsHide: true })
  }

  // 停机完成，清掉哨兵，方便下次启动。
  clearShutdownSentinel()
  return everKilled || !status().running
}

/** 等服务真的起来（最多 timeoutMs）。 */
function waitForService(timeoutMs) {
  const deadline = Date.now() + timeoutMs
  while (Date.now() < deadline) {
    if (status().running) return true
    spawnSync('ping', ['-n', '2', '127.0.0.1'], { stdio: 'ignore', windowsHide: true })
  }
  return status().running
}

/**
 * 后台启动服务本体（普通权限，独立进程）。
 *
 * 三点和以前一样重要：
 * 1. `detached: true` + `unref()`：关掉终端 / 关掉 Electron 都不会带走它；
 * 2. `stdio: 'ignore'` + `windowsHide: true`：不留任何窗口；
 * 3. 把 PID 记下来，`service:stop` 用它精确停。
 *
 * 不做的事：不提权。见文件头那段说明。
 *
 * @returns {{ok: boolean, pid: number|null}}
 */
function startDetached() {
  const args = backendArgs()
  const child = spawn(BACKEND, args, {
    cwd: projectRoot,
    detached: true,
    stdio: 'ignore',
    windowsHide: true,
    // 托盘「打开主界面」要靠它找到 node.exe（见 native/backend/TrayIcon.cpp）。
    env: { ...process.env, SHADOW_NODE_EXE: process.execPath },
  })
  child.unref()
  const pid = Number(child.pid) || null
  if (pid) writeServicePid(pid)
  return { ok: waitForService(8000), pid }
}

switch (cmd) {
  case 'status': {
    const s = status()
    console.log(
      s.running
        ? `采集服务正在运行（${s.how === 'mutex' ? '互斥体' : '状态文件'}）`
        : '采集服务没有运行',
    )
    process.exit(s.running ? 0 : 1)
    break
  }

  case 'start': {
    const s = status()
    if (s.running) {
      console.log('采集服务已经在运行了，无需重复启动。')
      console.log('（若要让它按新配置重来：npm run service:stop 后再启动）')
      process.exit(0)
    }

    // 启动时清掉可能残留的停机哨兵，免得它挡住本次启动。
    clearShutdownSentinel()

    if (extraArgs.includes('--fg')) {
      // 前台跑，日志直接打在终端里，方便排查（--fg 是本地开关，不传给服务）。
      const args = backendArgs()
      console.log(`启动采集服务（前台）：${BACKEND}`)
      console.log(`  ${args.join(' ')}`)
      console.log('（Ctrl+C 停止；也可以另开终端执行 npm run service:stop）\n')
      const child = spawn(BACKEND, args, { stdio: 'inherit', cwd: projectRoot })
      writeServicePid(child.pid)
      child.on('exit', (code) => {
        rmSync(PID_FILE, { force: true })
        process.exit(code ?? 0)
      })
      process.on('SIGINT', () => child.kill())
      break
    }

    const r = startDetached()
    if (!r.ok) {
      console.error(
        '采集服务没能起来。\n' +
          '想看它到底报了什么错，用：npm run service -- --fg',
      )
      process.exit(1)
    }
    console.log(`已启动采集服务（后台独立进程${r.pid ? `，pid ${r.pid}` : ''}，关掉 Electron 也不会停）。`)
    console.log('要停止：npm run service:stop')
    process.exit(0)
    break
  }

  case 'stop': {
    // 干净停掉整套（服务 + 守护）。见 stopAll 的说明：先立哨兵再循环杀，
    // 否则守护会把刚被杀的服务又拉起来，永远停不干净。
    const ok = stopAll()
    if (ok) {
      console.log('已停止采集服务（含守护进程）。')
      process.exit(0)
    }
    console.error(
      '停止失败：仍有 shadow-capture.exe 在运行。\n' +
        '可以在任务管理器里结束它，或稍后重试。',
    )
    process.exit(1)
    break
  }

  default:
    console.error(`未知命令：${cmd}\n用法：node scripts/service.mjs [start|status|stop]`)
    process.exit(2)
}
