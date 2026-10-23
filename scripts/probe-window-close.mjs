// scripts/probe-window-close.mjs —— 「关掉被录的窗口不许改录同应用的其它窗口」
//
// 复现用户报的现象（2026-10-23）：
//   「窗口捕获了哔哩哔哩客户端的播放器界面，然后把播放器关了，他录制了主界面……
//     点开编辑，下拉选择框里那个窗口还是播放器的标题。」
//
// 根因：锁了 hwnd，但 hwnd 一失效就退回按规则重新匹配。播放器和主界面同 exe、
// 同窗口类，主界面天然命中，于是画面悄悄换了源。
//
// 这里用真 Edge 造一个"一个进程两个窗口"的场景（浏览器窗口都是
// Chrome_WidgetWin_1、同一个 msedge.exe），正是用户那个 case 的结构：
//   1. 独立 user-data-dir 起 Edge，开 player 窗口
//   2. 同实例再开一个 main 窗口（Chromium 会把新进程转发给已有实例）
//   3. 服务按标题只匹配 player 窗口 → 记录锁定到的 hwnd
//   4. 用 CDP 关掉 player 窗口
//      期望：state 变成 disabled + windowMissing=true，hwnd **还是 player 那个**，
//            绝不等于 main 窗口的 hwnd（修之前会漂到 main，并且顶栏写着主界面）
//   5. 重新打开 player 窗口
//      期望：3~5 秒内自己接回来，hwnd 换成新窗口，windowMissing 归 false
//
// 用法：node scripts/probe-window-close.mjs

import { spawn, spawnSync } from 'node:child_process'
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs'
import path from 'node:path'
import { projectRoot, tempRoot } from './electron-path.mjs'

const EDGE = 'C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe'
const backend = path.join(projectRoot, 'native', 'build', 'shadow-capture.exe')
const PROOF = path.join(tempRoot, 'proofs', 'window-close')
const RECORD_ROOT = path.join(PROOF, 'records')
const PROFILE_DIR = path.join(PROOF, 'edge-profile')
const PROFILES = path.join(PROOF, 'profiles.json')
const PAGE_PLAYER = path.join(PROOF, 'player.html')
const PAGE_MAIN = path.join(PROOF, 'main.html')
const STATUS_DIR = path.join(tempRoot, 'status')
const STATUS = path.join(STATUS_DIR, 'status-v1.json')
const CDP_PORT = 9333

const TITLE_PLAYER = 'shadow-player-win'
const TITLE_MAIN = 'shadow-main-win'

if (!existsSync(backend)) {
  console.error('[probe] 找不到采集服务，先 npm run build:native')
  process.exit(1)
}
if (!existsSync(EDGE)) {
  console.error('[probe] 没装 Edge，这个探针用不了')
  process.exit(1)
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
const item = () => {
  try {
    return JSON.parse(readFileSync(STATUS, 'utf8')).items?.[0] ?? null
  } catch {
    return null
  }
}
const killTree = (p) => {
  if (!p?.pid) return
  try {
    spawnSync('taskkill', ['/pid', String(p.pid), '/T', '/F'], {
      stdio: 'ignore',
      windowsHide: true,
    })
  } catch {
    /* 已经退了 */
  }
}
const fail = (msg) => {
  console.log(`[probe] 失败：${msg}`)
  process.exitCode = 1
}

/** 当前所有 Edge 页面 target（CDP over HTTP，不用 WebSocket）。 */
async function cdpTargets() {
  try {
    const res = await fetch(`http://127.0.0.1:${CDP_PORT}/json/list`)
    if (!res.ok) return []
    return await res.json()
  } catch {
    return []
  }
}

/** 关掉标题含 needle 的那个窗口（等价于用户点右上角叉）。 */
async function closeTargetByTitle(needle) {
  for (const t of await cdpTargets()) {
    if (t.type === 'page' && String(t.title ?? '').includes(needle)) {
      await fetch(`http://127.0.0.1:${CDP_PORT}/json/close/${t.id}`).catch(() => {})
      return true
    }
  }
  return false
}

rmSync(PROOF, { recursive: true, force: true })
mkdirSync(RECORD_ROOT, { recursive: true })
mkdirSync(PROFILE_DIR, { recursive: true })
mkdirSync(STATUS_DIR, { recursive: true })
rmSync(STATUS, { force: true })

const pageHtml = (title, bg) =>
  `<!doctype html><html><head><meta charset="utf-8"><title>${title}</title>
<style>html,body{margin:0;height:100%;background:${bg};color:#ddd;font:20px sans-serif;
display:flex;align-items:center;justify-content:center}</style></head>
<body><h1>${title}</h1></body></html>`

writeFileSync(PAGE_PLAYER, pageHtml(TITLE_PLAYER, '#123'))
writeFileSync(PAGE_MAIN, pageHtml(TITLE_MAIN, '#321'))

writeFileSync(
  PROFILES,
  JSON.stringify(
    {
      version: 1,
      recordRoot: RECORD_ROOT,
      exportDir: path.join(PROOF, 'exports'),
      profiles: [
        {
          id: 'p-close',
          num: 1,
          name: 'closewin',
          enabled: true,
          source: { kind: 'window', monitorIndex: 0, clientAreaOnly: false, wgc: true },
          // 【关键】用「类名 + 应用程序」这一档，和用户哔哩哔哩那条配置完全同构：
          // 两个 Edge 窗口都是 Chrome_WidgetWin_1、都是 msedge.exe，**都命中规则**。
          // 修之前 pick:largest 会在播放器窗口关掉后选中主界面 —— 漂移就是这么发生的。
          match: {
            rule: 'class+process',
            by: 'class',
            mode: 'contains',
            pattern: 'Chrome_WidgetWin_1',
            // 用 foreground：两个窗口尺寸一样大（Edge 不认 --window-size 的第二次），
            // largest 是抛硬币。**先开 main 再开 player**，player 就在前台，
            // pick 才是确定的；player 一关，前台立刻变成 main —— 正好是修之前
            // "漂到主界面"的那条路径。
            pick: 'foreground',
            pollMs: 500,
            requireVisible: true,
            className: 'Chrome_WidgetWin_1',
            process: 'msedge.exe',
          },
          video: {
            fps: 30,
            bitrateMbps: 4,
            replaySeconds: 10,
            segmentSeconds: 2,
            scaleWidth: 0,
            scaleHeight: 0,
          },
          createdAt: '2026-01-01T00:00:00.000Z',
        },
      ],
    },
    null,
    2,
  ),
)

// 窗口尺寸固定下来，pick:largest 才有确定的答案（player 永远比 main 大）。
const edgeArgs = (file, size) => [
  `--user-data-dir=${PROFILE_DIR}`,
  '--no-first-run',
  '--no-default-browser-check',
  '--disable-features=Translate',
  `--window-size=${size}`,
  `--remote-debugging-port=${CDP_PORT}`,
  '--new-window',
  `file:///${file.replace(/\\/g, '/')}`,
]

const SIZE_PLAYER = '1100,800'
const SIZE_MAIN = '420,320'

// ---- 1. 一个 Edge 实例、两个窗口 ------------------------------------------
// 第一条命令真正启动浏览器；第二条会被转发给已有实例（新进程立刻退出），
// 所以两个窗口属于**同一个** msedge.exe 进程树 —— 和哔哩哔哩一样的结构。
// 顺序不能反：后开的那个在前台，而配置是 pick:foreground。
// 第一次 spawn 拿到的是**浏览器进程本身**，后面要 kill 它就靠这个句柄。
const edge = spawn(EDGE, edgeArgs(PAGE_MAIN, SIZE_MAIN), {
  stdio: 'ignore',
  windowsHide: false,
})
await sleep(4000)
// 第二次会被转发给已有实例（新进程立刻退出），所以这个返回值的 pid 没用。
spawn(EDGE, edgeArgs(PAGE_PLAYER, SIZE_PLAYER), { stdio: 'ignore', windowsHide: false })
await sleep(2500)

const titles = (await cdpTargets()).map((t) => t.title).filter(Boolean)
console.log(`[probe] Edge 就绪，页面：${titles.join(' | ') || '(没读到)'}`)

// ---- 2. 起服务，等它锁到 player 窗口 --------------------------------------
const child = spawn(backend, ['--profiles', PROFILES], {
  cwd: projectRoot,
  stdio: ['pipe', 'pipe', 'pipe'],
  windowsHide: true,
})
child.stdout.setEncoding('utf8')
child.stdout.on('data', () => {})
child.stdin.write('')

let it = null
const t0 = Date.now()
while (Date.now() - t0 < 20000) {
  await sleep(300)
  it = item()
  if (it?.state === 'live') break
}
if (!it || it.state !== 'live') {
  fail(`服务没进 live（${it?.state}：${it?.detail ?? ''}）`)
  killTree(child)
  killTree(edge)
  process.exit(1)
}
const playerHwnd = Number(it.hwnd ?? 0)
console.log(
  `[probe] 已锁定：hwnd=${playerHwnd} 标题=${it.matchedTitle || '(空)'} ` +
    `缺窗口标记=${it.windowMissing ?? false}`,
)
if (!playerHwnd) fail('服务 live 但没报 hwnd')
if (!String(it.matchedTitle ?? '').includes(TITLE_PLAYER)) {
  fail(`前提不成立：锁到了「${it.matchedTitle}」而不是 player 窗口`)
}

// ---- 3. 关掉 player 窗口：不许漂到 main ------------------------------------
const closed = await closeTargetByTitle(TITLE_PLAYER)
if (!closed) {
  fail('没能通过 CDP 关掉 player 窗口')
} else {
  console.log('[probe] 已关掉 player 窗口，观察 6 秒…')
}
let sawMissing = false
let drifted = false
let missingHwnd = 0
const tClose = Date.now()
while (Date.now() - tClose < 6000) {
  await sleep(300)
  const cur = item()
  if (!cur) continue
  const h = Number(cur.hwnd ?? 0)
  if (cur.windowMissing) {
    if (!sawMissing) {
      sawMissing = true
      missingHwnd = h
      console.log(
        `[probe] t+${((Date.now() - tClose) / 1000).toFixed(1)}s → state=${cur.state} ` +
          `windowMissing=true hwnd=${h} 明细=${cur.detail ?? ''}`,
      )
    }
  } else if (cur.state === 'live' && h && h !== playerHwnd) {
    drifted = true
    console.log(
      `[probe] t+${((Date.now() - tClose) / 1000).toFixed(1)}s **漂到别的窗口**：` +
        `hwnd=${h} 标题=${cur.matchedTitle ?? ''}`,
    )
    break
  }
}
if (!sawMissing) fail('关掉窗口后没进"等它回来"的状态（windowMissing 一直是 false）')
// 前提校验：关掉播放器的时候，主界面必须还在 —— 否则"不漂移"这件事无从谈起。
const stillOpen = (await cdpTargets()).map((t) => String(t.title ?? ''))
if (!stillOpen.some((t) => t.includes(TITLE_MAIN))) {
  console.log('[probe] 警告：主界面也不在了，这个用例没真正覆盖"同应用其它窗口"')
}
if (missingHwnd && missingHwnd !== playerHwnd) {
  fail(`等待状态下 hwnd 变成了 ${missingHwnd}，应该是原来那个 ${playerHwnd}`)
}
if (drifted) fail('画面漂到了同应用的另一个窗口 —— 正是要修的那个 bug')

// ---- 4. 重新打开 player 窗口：应该自己接回来 ------------------------------
spawn(EDGE, edgeArgs(PAGE_PLAYER, SIZE_PLAYER), { stdio: 'ignore', windowsHide: false, detached: true })
const tRe = Date.now()
let reattached = null
while (Date.now() - tRe < 15000) {
  await sleep(300)
  const cur = item()
  if (cur && cur.state === 'live' && !cur.windowMissing) {
    reattached = cur
    break
  }
}
if (!reattached) {
  fail('重新打开窗口后没有自动接回来')
} else {
  const newHwnd = Number(reattached.hwnd ?? 0)
  console.log(
    `[probe] t+${((Date.now() - tRe) / 1000).toFixed(1)}s 接回来了：hwnd=${newHwnd} ` +
      `标题=${reattached.matchedTitle || '(空)'}`,
  )
  if (newHwnd === playerHwnd) {
    console.log('[probe] 注意：hwnd 没变（系统复用了句柄），也算正确')
  }
}

// ---- 5. 应用整个退出：黑名单必须作废，新实例能重新锁上 ----------------
//
// 用户的要求：「除非这个应用程序退出……才能刷新这个黑名单列表」。
// 这里把整个 Edge 杀掉（等同于用户退出应用），再用**另一个** profile 只开
// 主界面那种窗口 —— 它和之前被拉黑的"主界面"同 exe 同 class，只有黑名单
// 真的清掉了才可能被选中。
killTree(edge)
await sleep(3500)
const PROFILE_DIR2 = `${PROFILE_DIR}-2`
mkdirSync(PROFILE_DIR2, { recursive: true })
const edge2 = spawn(EDGE, [`--user-data-dir=${PROFILE_DIR2}`, '--no-first-run', '--no-default-browser-check',
  '--new-window', `file:///${PAGE_MAIN.replace(/\\/g, '/')}`], {
  stdio: 'ignore',
  windowsHide: false,
})
const tRestart = Date.now()
let relocked = null
while (Date.now() - tRestart < 15000) {
  await sleep(300)
  const cur = item()
  if (cur && cur.state === 'live' && !cur.windowMissing) {
    relocked = cur
    break
  }
}
if (!relocked) {
  fail('应用退出后重新打开，服务没能重新锁上（黑名单没清）')
} else {
  console.log(
    `[probe] t+${((Date.now() - tRestart) / 1000).toFixed(1)}s 应用重启后重新锁上：` +
      `hwnd=${relocked.hwnd} 标题=${relocked.matchedTitle || '(空)'}`,
  )
}

killTree(child)
// 只杀自己起的那个实例。**不要**用 taskkill /im msedge.exe —— 那会把用户
// 正在用的浏览器一起关掉。
killTree(edge2)
console.log(process.exitCode ? '[probe] 结论：不通过' : '[probe] 结论：通过')
process.exit(process.exitCode ?? 0)
