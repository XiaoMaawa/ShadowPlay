// scripts/probe-nosignal-export.mjs —— 「没画面的时候也要能导出，且补 NO SIGNAL」
//
// 复现用户报的现象（2026-10-23）：
//   「窗口捕获，窗口都已经不存在了，预览出现 no signal 了，为什么导出的视频还有
//     画面，导出的视频不应该 no signal 吗」
//   「窗口捕获和显示器捕获无论是不是 no signal，都要允许导出，没有画面的部分用
//     no signal 代替」
//
// 根因：内层采集循环一退出就把录制器 Stop 掉了，时间轴从此冻住。于是
//   * 导出窗口 = "最后内容往前 N 秒" —— 整段都是关窗前的画面，一个 NO SIGNAL 都没有；
//   * recorder_ 没了，ExportNow 直接拒绝导出。
//
// 修法：没画面时继续按录制帧率喂一张 NO SIGNAL 画布（Session::FeedNoSignal），
// 时间轴一直走到"现在"。
//
// 这个脚本验三件事：
//   1. 关掉窗口后 recordFrames 还在涨（时间轴没停）
//   2. 关着窗口也能导出
//   3. 成片末尾那截是 NO SIGNAL（黑），开头那截是真画面（亮）
//
// 用法：node scripts/probe-nosignal-export.mjs

import { spawn, spawnSync } from 'node:child_process'
import { createRequire } from 'node:module'
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs'
import path from 'node:path'
import { projectRoot, tempRoot } from './electron-path.mjs'

const require = createRequire(import.meta.url)
const EDGE = 'C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe'
const backend = path.join(projectRoot, 'native', 'build', 'shadow-capture.exe')
const addonPath = path.join(projectRoot, 'native', 'build', 'shadow_ring.nodejs.node')
const PROOF = path.join(tempRoot, 'proofs', 'nosignal-export')
const RECORD_ROOT = path.join(PROOF, 'records')
const EXPORT_DIR = path.join(PROOF, 'exports')
const PROFILE_DIR = path.join(PROOF, 'edge-profile')
const PROFILES = path.join(PROOF, 'profiles.json')
const PAGE = path.join(PROOF, 'player.html')
const STATUS = path.join(tempRoot, 'status', 'status-v1.json')
const CDP_PORT = 9334
const TITLE = 'shadow-nosignal-win'

// 回放窗口给短一点，跑得快。
const WINDOW_SEC = 8
// 关窗之后要等多久再导出：这段时间应该全是 NO SIGNAL。
//
// 给到 9 秒是有意的：关窗那一刻要先走完 WGC 的重建重试（6 次，最长约 5 秒），
// 之后才进"等待窗口回来 + 喂 NO SIGNAL"那条路。等太短会量到重试期间那段
// 空档（那本来就是没帧的）。
const AFTER_CLOSE_SEC = 9

// 导出多长。
//
// 【为什么不是直接用 WINDOW_SEC】回放窗口只有 8 秒，而关窗后又等了 9 秒 ——
// 按 8 秒导的话整支片子都是关窗之后的，本来就该全是 NO SIGNAL，探针就没法
// 同时验"开头是真画面、末尾是遮罩"这半条。多导 6 秒，让成片左半截是关窗前的
// 真画面、右半截是关窗后的遮罩，一条片子把两件事都量到。
const EXPORT_SEC = WINDOW_SEC + 6

if (!existsSync(backend) || !existsSync(addonPath)) {
  console.error('[probe] 找不到原生产物，先 npm run build:native')
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
  console.log(`[probe] × ${msg}`)
  process.exitCode = 1
}
const ok = (msg) => console.log(`[probe] ✓ ${msg}`)

async function cdpTargets() {
  try {
    const res = await fetch(`http://127.0.0.1:${CDP_PORT}/json/list`)
    if (!res.ok) return []
    return await res.json()
  } catch {
    return []
  }
}
async function closeTargetByTitle(needle) {
  for (const t of await cdpTargets()) {
    if (t.type === 'page' && String(t.title ?? '').includes(needle)) {
      await fetch(`http://127.0.0.1:${CDP_PORT}/json/close/${t.id}`).catch(() => {})
      return true
    }
  }
  return false
}

/**
 * 一段视频里某时刻那一帧的**最亮像素**（0..255）和"亮像素占多少"。
 *
 * 【为什么要单独量它】`frameLuma` 把整帧缩成 48x27 求平均，NO SIGNAL 的字只
 * 占几个像素，平均下来和纯黑没区别 —— 那正好**看不出字是白的还是灰的**。
 * 用户报的就是这个：「no signal 怎么是灰色的，不应该是白色的 no signal 吗」。
 * 字色靠最亮像素判：白字 240±、灰字（原来的 0x3A）只有 60 上下。
 */
function frameMaxLuma(file, seekArg, tmpName) {
  const out = path.join(PROOF, tmpName)
  const args = [
    '-hide_banner',
    '-loglevel',
    'error',
    ...seekArg,
    '-i',
    file,
    '-frames:v',
    '1',
    '-vf',
    'format=gray',
    '-f',
    'rawvideo',
    '-y',
    out,
  ]
  const r = spawnSync('ffmpeg', args, { stdio: 'ignore', windowsHide: true })
  if (r.status !== 0) return null
  let buf
  try {
    buf = readFileSync(out)
  } catch {
    return null
  }
  if (!buf.length) return null
  let max = 0
  let bright = 0
  for (const b of buf) {
    if (b > max) max = b
    if (b >= 200) bright += 1
  }
  return { max, brightRatio: bright / buf.length }
}

function frameLuma(file, seekArg, tmpName) {
  const out = path.join(PROOF, tmpName)
  const args = [
    '-hide_banner',
    '-loglevel',
    'error',
    ...seekArg,
    '-i',
    file,
    '-frames:v',
    '1',
    '-vf',
    'scale=48:27,format=gray',
    '-f',
    'rawvideo',
    '-y',
    out,
  ]
  const r = spawnSync('ffmpeg', args, { encoding: 'buffer', windowsHide: true })
  if (r.status !== 0) return null
  let buf
  try {
    buf = readFileSync(out)
  } catch {
    return null
  }
  if (!buf.length) return null
  let sum = 0
  for (const b of buf) sum += b
  return sum / buf.length
}

rmSync(PROOF, { recursive: true, force: true })
mkdirSync(RECORD_ROOT, { recursive: true })
mkdirSync(PROFILE_DIR, { recursive: true })
mkdirSync(EXPORT_DIR, { recursive: true })
mkdirSync(path.dirname(STATUS), { recursive: true })
rmSync(STATUS, { force: true })

// 亮底页面：NO SIGNAL 是黑底，两者亮度差得远，判据才稳。
writeFileSync(
  PAGE,
  `<!doctype html><html><head><meta charset="utf-8"><title>${TITLE}</title>
<style>html,body{margin:0;height:100%;background:#f2f2f2;color:#222;font:28px sans-serif;
display:flex;align-items:center;justify-content:center}</style></head>
<body><h1>${TITLE}</h1></body></html>`,
)

writeFileSync(
  PROFILES,
  JSON.stringify(
    {
      version: 1,
      recordRoot: RECORD_ROOT,
      exportDir: EXPORT_DIR,
      profiles: [
        {
          id: 'p-nosig',
          num: 1,
          name: 'nosig',
          enabled: true,
          source: { kind: 'window', monitorIndex: 0, clientAreaOnly: false, wgc: true },
          match: {
            rule: 'title',
            by: 'title',
            mode: 'contains',
            pattern: TITLE,
            pick: 'largest',
            pollMs: 500,
            requireVisible: true,
            title: TITLE,
          },
          video: {
            fps: 30,
            bitrateMbps: 6,
            replaySeconds: WINDOW_SEC,
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

// ---- 1. 起 Edge（一个窗口，亮底页面） --------------------------------------
const edge = spawn(
  EDGE,
  [
    `--user-data-dir=${PROFILE_DIR}`,
    '--no-first-run',
    '--no-default-browser-check',
    '--disable-features=Translate',
    '--window-size=900,600',
    `--remote-debugging-port=${CDP_PORT}`,
    '--new-window',
    `file:///${PAGE.replace(/\\/g, '/')}`,
  ],
  { stdio: 'ignore', windowsHide: false },
)
await sleep(4000)

const titles = (await cdpTargets()).map((t) => t.title).filter(Boolean)
console.log(`[probe] Edge 就绪：${titles.join(' | ') || '(没读到)'}`)

// ---- 2. 起服务，等它进 live 并录够一段 -------------------------------------
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
  if (it?.state === 'live' && (it.recordFrames ?? 0) > 30) break
}
if (!it || it.state !== 'live') {
  fail(`服务没进 live（${it?.state}：${it?.detail ?? ''}）`)
  killTree(child)
  killTree(edge)
  process.exit(1)
}
console.log(`[probe] live：${it.detail}；先录 ${WINDOW_SEC}s 让它填满回放窗口…`)
await sleep(WINDOW_SEC * 1000 + 1500)

const beforeClose = item()
// 状态里的 recordDir 是**配置目录**（<root>/<序号>），分片在它下面的
// segments/ 里。导出接口要的是后者。
const segDir = beforeClose?.recordDir
  ? path.join(beforeClose.recordDir, 'segments')
  : null
if (!segDir) {
  fail('状态里没有 recordDir')
  killTree(child)
  killTree(edge)
  process.exit(1)
}

// ---- 3. 关掉窗口 -----------------------------------------------------------
console.log(`[probe] 关掉被录的窗口（帧数 ${beforeClose.recordFrames}）…`)
if (!(await closeTargetByTitle(TITLE))) {
  fail('没能通过 CDP 关掉窗口')
}
await sleep(1200)
const justClosed = item()
console.log(
  `[probe] 关窗后 1.2s：state=${justClosed?.state} windowMissing=${justClosed?.windowMissing} ` +
    `recording=${justClosed?.recording} frames=${justClosed?.recordFrames}`,
)

// ---- 4. 证明"时间轴还在走"：帧数必须继续涨 --------------------------------
const f1 = justClosed?.recordFrames ?? 0
await sleep(AFTER_CLOSE_SEC * 1000)
const after = item()
const f2 = after?.recordFrames ?? 0
const grew = f2 - f1
if (grew > (AFTER_CLOSE_SEC * 30) / 2) {
  ok(`关窗 ${AFTER_CLOSE_SEC}s 内仍在出帧：${f1} → ${f2}（+${grew}），说明时间轴没停`)
} else {
  fail(
    `关窗后几乎不再出帧（${f1} → ${f2}）：NO SIGNAL 没有被喂进录制器，` +
      `导出窗口右端会停在关窗那一刻`,
  )
}

// ---- 5. 关着窗口直接导出（用户要求"无论是不是 NO SIGNAL 都要能导"）-------
const addon = require(addonPath)

// 【先 flush】真实导出（Session::ExportNow）在开跑前一定会先 FlushSegment，
// 把正在写的那一片收尾。探针是直接调 addon，少了这一步 —— 尾片是个写了一半
// 的 fMP4，解码端等不到片尾标记会**永远挂着**（实测卡在 phase="合并" 120 秒
// 超时）。这里补上，和真实路径对齐。
async function flushNow() {
  const it0 = item()
  if (!it0) return
  const before = it0.flushSeq ?? 0
  child.stdin.write(JSON.stringify({ cmd: 'flush', id: it0.id }) + '\n')
  const t = Date.now()
  while (Date.now() - t < 6000) {
    await sleep(60)
    const now = item()?.flushSeq ?? 0
    if (now !== before) return
  }
  console.log('[probe] ! flush 没等到变化，尾片可能还是写了一半的状态')
}
await flushNow()

// maxSeconds 传 0 = 用配置里的回放窗口；startUnixMs 传 0 = 普通"最近 N 秒"路径。
const res = addon.exportStart(segDir, EXPORT_DIR, 'nosig', EXPORT_SEC, 30, 2, 0)
if (!res?.ok) {
  fail(`导不起来：${res?.error}`)
} else {
  ok('关着窗口也允许导出')
}

let st = null
// 【先等它真的开跑】exportStart 只是把任务排上，worker 线程要过一会儿才把
// running 翻成 true。立刻轮询会读到**上一趟**的"已结束"状态（ok=false、
// detail 空），于是干干净净地误判成"导出失败"。等 jobId 对上且 running 为真
// 再进收尾轮询。
const jobId = res?.jobId ?? 0
const tStart = Date.now()
while (Date.now() - tStart < 8000) {
  st = addon.exportStatus()
  if (st && st.running === true && (jobId ? st.jobId === jobId : true)) break
  await sleep(60)
}
if (!st?.running) {
  fail(`导出没开跑（jobId=${st?.jobId} 期望 ${jobId}，running=${st?.running}）`)
}
const tExp = Date.now()
while (Date.now() - tExp < 120000) {
  await sleep(150)
  st = addon.exportStatus()
  if (st && !st.running) break
}
if (!st) {
  fail('拿不到导出状态')
} else if (!st.ok) {
  fail(`导出失败：${st.detail || '(空)'} | ${JSON.stringify(st).slice(0, 500)}`)
} else {
  ok(`导出成功：${st.outPath}（${st.durationSeconds?.toFixed(2)}s）`)

  // ---- 6. 成片内容：开头该有画面，末尾该是 NO SIGNAL ---------------------
  //
  // 【为什么"开头"要扫一段而不是取固定一帧】片头那段是"回放窗口比录制时间
  // 长"补出来的 NO SIGNAL（探针只录了十几秒，却要导 14 秒），所以第 0.2 秒
  // 本来就是遮罩 —— 拿它当"真画面"的样本必然误判。真画面在遮罩之后的那几
  // 秒里，所以在前 60% 里逐点取样，取最亮的那一处代表"有没有真画面"。
  const dur = Number(st.durationSeconds) || 0
  const marks = []
  for (let t = 0.5; t < Math.min(8, dur * 0.6); t += 0.5) marks.push(t)
  const lums = marks.map((t) => frameLuma(st.outPath, ['-ss', String(t)], `head-${t}.gray`))
  const valid = lums.filter((v) => v !== null)
  const head = valid.length ? Math.max(...valid) : null
  const tail = frameLuma(st.outPath, ['-sseof', '-0.4'], 'tail.gray')
  console.log(
    `[probe] 亮度：开头段最高=${head?.toFixed(1) ?? '?'}（取样 ${valid.length} 帧）` +
      ` 末尾=${tail?.toFixed(1) ?? '?'}`,
  )
  // 判据用"两者差得开"而不是绝对阈值：页面是浅色底（灰度 240 上下），
  // 但 WGC 抓的是整窗（含标题栏/边框），缩放后均值会掉到 80 左右。
  // NO SIGNAL 是纯黑底 + 白字，均值个位数。两者差 40 以上就足够区分。
  if (head === null) {
    fail('取不到开头那一帧')
  } else if (head < 50) {
    fail(`开头那段看着就不像真画面（亮度 ${head.toFixed(1)}）`)
  } else {
    ok(`开头是真画面（亮度 ${head.toFixed(1)}）`)
  }
  if (tail === null) {
    fail('取不到末尾那一帧')
  } else if (tail < 40) {
    ok(`末尾是 NO SIGNAL（亮度 ${tail.toFixed(1)}）`)
  } else {
    fail(`末尾仍是旧画面（亮度 ${tail.toFixed(1)}）：关窗后的那段没有补 NO SIGNAL`)
  }
  if (head !== null && tail !== null && head - tail < 40) {
    fail(`首尾亮度太接近（${head.toFixed(1)} vs ${tail.toFixed(1)}），无法确认末尾被换成了遮罩`)
  }

  // ---- 6b. NO SIGNAL 的字必须是**白**的（用户：「不应该是灰色的」）------
  const tailText = frameMaxLuma(st.outPath, ['-sseof', '-0.4'], 'tail-full.gray')
  if (!tailText) {
    fail('取不到末尾那一帧的原尺寸灰图，量不了字色')
  } else {
    const pct = (tailText.brightRatio * 100).toFixed(3)
    console.log(`[probe] 末尾帧：最亮 ${tailText.max}，亮度≥200 占 ${pct}%`)
    if (tailText.max < 200) {
      fail(`NO SIGNAL 的字偏灰（最亮只有 ${tailText.max}，白字应 ≥200）`)
    } else if (tailText.brightRatio <= 0.0002) {
      fail(`最亮像素有了但几乎没有字（亮度≥200 只占 ${pct}%）`)
    } else {
      ok(`NO SIGNAL 的字是白的（最亮 ${tailText.max}，占 ${pct}%）`)
    }
  }
}

killTree(child)
killTree(edge)
console.log(process.exitCode ? '[probe] 结论：不通过' : '[probe] 结论：通过')
process.exit(process.exitCode ?? 0)
