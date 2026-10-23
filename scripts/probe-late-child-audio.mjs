// scripts/probe-late-child-audio.mjs —— "迟到子进程"导致窗口没声音的回归测试
//
// 复现用户报的现象：「一个浏览器播放，一个 bilibili 客户端播放，捕捉 bilibili
// 客户端，会出现没有声音的情况」——这次没声、什么都不动下次可能就有。
//
// 根因（见 AudioCapture.h 的 NewChildCount 注释）：进程环回的
// PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE 在**创建那一刻**就把进程树
// 定死了。Chromium 系应用只在真正出声时才懒创建 audio.mojom.AudioService
// 子进程；它要是晚于环回出现，就永远进不来，而且不报任何错。
//
// 这里用真 Edge 复现，不搞模拟：
//   1. 生成一段 440Hz 的 wav，写一个**延迟 8 秒才 play()** 的页面
//   2. 用独立的 user-data-dir 起 Edge，窗口标题是 shadow-late-child
//      （不复用用户已有的 Edge 进程树，否则窗口属于另一个进程）
//   3. 服务用窗口源匹配这个标题 —— 此刻页面还没出声，环回抓到的树里
//      大概率还没有音频子进程
//   4. 8 秒后页面开始出声：期望 3 秒内 bytes 开始涨（说明 Session 发现
//      进程树变了、重建了环回）
//
// 修之前：bytes 一直是 0（配置显示"窗口进程环回"但永远没声音）。

import { spawn, spawnSync } from 'node:child_process'
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs'
import path from 'node:path'
import { projectRoot, tempRoot } from './electron-path.mjs'

const EDGE = 'C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe'
const backend = path.join(projectRoot, 'native', 'build', 'shadow-capture.exe')
const PROOF = path.join(tempRoot, 'proofs', 'late-child')
const RECORD_ROOT = path.join(PROOF, 'records')
const PROFILE_DIR = path.join(PROOF, 'edge-profile')
const PROFILES = path.join(PROOF, 'profiles.json')
const TONE = path.join(PROOF, 'tone.wav')
const PAGE = path.join(PROOF, 'page.html')
const STATUS_DIR = path.join(tempRoot, 'status')
const STATUS = path.join(STATUS_DIR, 'status-v1.json')

const TITLE = 'shadow-late-child'
const PLAY_DELAY_SEC = 8

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
    spawnSync('taskkill', ['/pid', String(p.pid), '/T', '/F'], { stdio: 'ignore', windowsHide: true })
  } catch {
    /* 已经退了 */
  }
}

rmSync(PROOF, { recursive: true, force: true })
mkdirSync(RECORD_ROOT, { recursive: true })
mkdirSync(PROFILE_DIR, { recursive: true })
mkdirSync(STATUS_DIR, { recursive: true })
rmSync(STATUS, { force: true })

// ---- 0. 音源 + 延迟播放的页面 ---------------------------------------------
const gen = spawnSync(
  'ffmpeg',
  ['-y', '-hide_banner', '-loglevel', 'error', '-f', 'lavfi', '-i',
   'sine=frequency=440:sample_rate=48000:duration=60', '-af', 'volume=0.25', TONE],
  { stdio: 'ignore' },
)
if (gen.status !== 0) {
  console.log('[probe] 失败：生成 wav 失败')
  process.exit(1)
}
writeFileSync(
  PAGE,
  `<!doctype html><html><head><meta charset="utf-8"><title>${TITLE}</title>
<style>body{background:#111;color:#ccc;font:16px sans-serif;padding:24px}</style></head>
<body><h1>${TITLE}</h1><p id="s">等 ${PLAY_DELAY_SEC} 秒后开始播放</p>
<script>
setTimeout(async () => {
  const a = new Audio('tone.wav');
  a.loop = true;
  a.volume = 0.25;
  try { await a.play(); document.getElementById('s').textContent = '正在播放'; }
  catch (e) { document.getElementById('s').textContent = 'play 失败：' + e.message; }
}, ${PLAY_DELAY_SEC * 1000});
</script></body></html>`,
)

writeFileSync(
  PROFILES,
  JSON.stringify(
    {
      version: 1,
      recordRoot: RECORD_ROOT,
      exportDir: path.join(PROOF, 'exports'),
      profiles: [
        {
          id: 'p-late',
          num: 1,
          name: 'latechild',
          enabled: true,
          source: { kind: 'window', monitorIndex: 0, clientAreaOnly: false, wgc: true },
          match: {
            rule: 'title+process',
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

// ---- 1. 起 Edge（独立 profile，保证是我们自己的进程树）--------------------
const edge = spawn(
  EDGE,
  [
    `--user-data-dir=${PROFILE_DIR}`,
    '--no-first-run',
    '--no-default-browser-check',
    '--autoplay-policy=no-user-gesture-required',
    '--disable-features=Translate',
    `file:///${PAGE.replace(/\\/g, '/')}`,
  ],
  { stdio: 'ignore', windowsHide: false, detached: false },
)
const tEdge = Date.now()
console.log(`[probe] 已启动 Edge（页面第 ${PLAY_DELAY_SEC} 秒才出声）`)

await sleep(3000)

// ---- 2. 起服务：此刻页面还没出声 -----------------------------------------
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
  if (it?.state === 'live' && it.recording) break
}
if (!it || it.state !== 'live') {
  console.log(`[probe] 失败：服务没进 live（${it?.state}：${it?.detail ?? ''}）`)
  killTree(child)
  killTree(edge)
  process.exit(1)
}
const bytesBefore = it.audioBytes ?? 0
const liveAt = (Date.now() - tEdge) / 1000
console.log(
  `[probe] ${liveAt.toFixed(1)}s 服务 live：${it.matchedTitle || '(没匹到窗口)'}` +
    ` | ${it.audioDetail || '(无音频)'} | bytes=${bytesBefore}`,
)

// ---- 3. 等页面出声，看多久能听到 -----------------------------------------
//
// 【判据必须是 peak，不能是 bytes】实测：Chromium 目标的环回**一直有包**
// （每秒几十 KB），只是全是静音 —— 用 bytes 涨没涨当判据，页面还没出声就
// 已经"通过"了，等于什么都没测。
const AUDIBLE = 0.002
let firstSoundAt = null
let peakAtSound = 0
let restartsAtSound = 0
const tWait = Date.now()
while (Date.now() - tWait < 25000) {
  await sleep(250)
  const cur = item()
  if (!cur) continue
  const peak = Number(cur.audioPeak ?? 0)
  if (peak > AUDIBLE) {
    firstSoundAt = Date.now()
    peakAtSound = peak
    restartsAtSound = cur.audioRestarts ?? 0
    console.log(
      `\n[probe] t+${((Date.now() - tEdge) / 1000).toFixed(1)}s 开始有电平：` +
        `peak=${peak.toFixed(4)} bytes=${cur.audioBytes} 采集重建次数=${restartsAtSound}`,
    )
    break
  }
  process.stdout.write(
    `\r[probe] t+${((Date.now() - tEdge) / 1000).toFixed(1)}s peak=${peak.toFixed(4)}` +
      ` bytes=${cur.audioBytes} 重建=${cur.audioRestarts ?? 0}      `,
  )
}
process.stdout.write('\n')

if (firstSoundAt === null) {
  console.log('[probe] 结论：不通过 —— 页面出声后一直没采到（进程树变了但没重建环回）')
  killTree(child)
  killTree(edge)
  process.exit(1)
}
const afterPlay = (firstSoundAt - tEdge) / 1000 - PLAY_DELAY_SEC
console.log(
  `[probe] 结论：通过 —— 页面开始出声后约 ${afterPlay.toFixed(1)}s 采到声音` +
    `（peak=${peakAtSound.toFixed(4)}，重建 ${restartsAtSound} 次）`,
)
if (peakAtSound <= 0.002) {
  console.log('[probe] 但峰值接近 0，音源可能没真的响')
  killTree(child)
  killTree(edge)
  process.exit(1)
}
if (restartsAtSound === 0) {
  console.log(
    '[probe] 注意：没有发生重建 —— 说明环回创建时音频子进程就已经在了（这次没走到那条路径）',
  )
}

// ---- 4. 单独验证"进程树变化"这条判据本身 ----------------------------------
//
// 上面那种情况（新开一个 Edge，音频进程一开始就在树里）测不到看门狗。这里
// 换个角度直接验判据：在**同一个 Edge 实例**里再开一个窗口 —— 复用
// user-data-dir 时新进程只是把 URL 交给已有实例，由那个实例新建一个 renderer
// 子进程，正好就是"建环回之后树长大了"。
// 期望：audioTreeGrown > 0（有声音在放，所以不会触发重建，只看判据准不准）。
console.log('[probe] 再开一个同实例窗口，检查进程树变化能不能被发现…')
const target2 = spawn(
  EDGE,
  [`--user-data-dir=${PROFILE_DIR}`, '--no-first-run', 'about:blank'],
  { stdio: 'ignore', windowsHide: false, detached: false },
)
let grew = null
const tGrow = Date.now()
while (Date.now() - tGrow < 12000) {
  await sleep(400)
  const cur = item()
  if ((cur?.audioTreeGrown ?? 0) > 0) {
    grew = cur.audioTreeGrown
    break
  }
}
killTree(target2)
killTree(child)
killTree(edge)
console.log(
  grew === null
    ? '[probe] 判据：没测到进程树变化（audioTreeGrown 一直是 0）—— 需要复查 NewChildCount'
    : `[probe] 判据：OK —— 发现 ${grew} 个新子进程（audioTreeGrown=${grew}）`,
)
process.exit(grew === null ? 1 : 0)
