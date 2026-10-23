// scripts/test-audio-fidelity.mjs —— 验证「成片里的声音是连续的」
//
// 为什么需要它：test-export-audio.mjs 只验"有音轨、有样本"，全静音的音轨
// 也能过。听感上的"卡顿"是音轨里出现了不该有的静音段，或者拼接处相位跳变，
// 必须把成片解回 PCM 逐点看。
//
// 做法：
//   1. 录一段视频（拿到真实分片时间轴）；
//   2. 往 audio/ 里写一条**跨块连续**的 440Hz 正弦（模拟真实采集的 2 秒一块，
//      相位跨块连续），覆盖整个回放窗口；
//   3. 导出；
//   4. 用 ffmpeg 把成片的音轨解回 s16le，检查：
//        * 有没有被声音包围的静音段（= 断点，"卡"的来源）
//        * 有没有采样跳变（= 拼接没有对齐，听起来是"咔哒"）
//        * 总时长对不对
//
// 用法：node scripts/test-audio-fidelity.mjs [--window=20] [--segments=4] [--amp=8000]
import { spawn } from 'node:child_process'
import { existsSync, mkdirSync, readFileSync, readdirSync, rmSync, writeFileSync } from 'node:fs'
import path from 'node:path'
import { projectRoot, tempRoot } from './electron-path.mjs'

const backend = path.join(projectRoot, 'native', 'build', 'shadow-capture.exe')
const PROOF = path.join(tempRoot, 'proofs', 'audio-fidelity')
const RECORD_ROOT = path.join(PROOF, 'records')
const EXPORT_DIR = path.join(PROOF, 'exports')
const PROFILES = path.join(PROOF, 'profiles.json')
const STATUS = path.join(tempRoot, 'status', 'status-v1.json')
const RATE = 48000

const arg = (name, dflt) => {
  const hit = process.argv.find((a) => a.startsWith(`--${name}=`))
  return hit ? Number(hit.split('=')[1]) : dflt
}
const WINDOW_SEC = arg('window', 20)
const SEGMENTS = arg('segments', 4)
const AMP = arg('amp', 8000)
// --live：不手写 PCM，改成真放一段测试音、让 WASAPI 真的去采（走完整链路）。
const LIVE = process.argv.includes('--live')

if (!existsSync(backend)) {
  console.error('[fidelity] 找不到采集服务，先 npm run build:native')
  process.exit(1)
}

rmSync(PROOF, { recursive: true, force: true })
mkdirSync(RECORD_ROOT, { recursive: true })
mkdirSync(EXPORT_DIR, { recursive: true })
rmSync(STATUS, { force: true })

writeFileSync(
  PROFILES,
  JSON.stringify(
    {
      version: 1,
      recordRoot: RECORD_ROOT,
      exportDir: EXPORT_DIR,
      profiles: [
        {
          id: 'p-desk',
          num: 1,
          name: 'fidelity',
          enabled: true,
          source: { kind: 'monitor', monitorIndex: 0, clientAreaOnly: false, wgc: true },
          video: { fps: 30, bitrateMbps: 6, replaySeconds: WINDOW_SEC, segmentSeconds: 1, scaleWidth: 0, scaleHeight: 0 },
          createdAt: '2026-01-01T00:00:00.000Z',
        },
      ],
    },
    null,
    2,
  ),
)

const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
const readStatus = () => {
  try {
    return JSON.parse(readFileSync(STATUS, 'utf8'))
  } catch {
    return null
  }
}
const desk = () => readStatus()?.items?.find((s) => s.id === 'p-desk') ?? null

/**
 * 按 2 秒一块写连续正弦（相位跨块连续，块与块之间接着数），
 * 这样"块缝处有没有断"跟真实采集一致。
 */
function writeContinuousTone(dir, startMs, endMs) {
  mkdirSync(dir, { recursive: true })
  const chunkFrames = 2 * RATE
  const totalFrames = Math.floor(((endMs - startMs) / 1000) * RATE)
  const files = []
  let written = 0
  let phase = 0
  while (written < totalFrames) {
    const frames = Math.min(chunkFrames, totalFrames - written)
    const buf = Buffer.alloc(frames * 4)
    for (let i = 0; i < frames; i++) {
      const v = Math.round(Math.sin(phase) * AMP)
      phase += (2 * Math.PI * 440) / RATE
      if (phase > 2 * Math.PI) phase -= 2 * Math.PI
      buf.writeInt16LE(v, i * 4)
      buf.writeInt16LE(v, i * 4 + 2)
    }
    const at = startMs + Math.round((written * 1000) / RATE)
    const f = path.join(dir, `a_${at}.pcm`)
    writeFileSync(f, buf)
    files.push(f)
    written += frames
  }
  return files
}

/** 用 ffmpeg 把成片音轨解成 s16le/48k/立体声裸 PCM。 */
function decodeAudio(mp4, outPcm) {
  return new Promise((resolve) => {
    const p = spawn('ffmpeg', ['-hide_banner', '-loglevel', 'error', '-y', '-i', mp4, '-vn', '-f', 's16le', '-acodec', 'pcm_s16le', '-ar', String(RATE), '-ac', '2', outPcm], {
      stdio: ['ignore', 'ignore', 'pipe'],
    })
    let err = ''
    p.stderr.setEncoding('utf8')
    p.stderr.on('data', (d) => (err += d))
    p.on('close', (code) => resolve({ code, err }))
  })
}

function analyze(pcmFile) {
  const buf = readFileSync(pcmFile)
  const frames = Math.floor(buf.length / 4)
  const win = Math.round(0.005 * RATE) // 5ms
  const nWin = Math.floor(frames / win)
  const silent = []
  for (let w = 0; w < nWin; w++) {
    let peak = 0
    for (let i = w * win; i < (w + 1) * win; i++) {
      const a = Math.abs(buf.readInt16LE(i * 4))
      if (a > peak) peak = a
    }
    silent.push(peak < AMP * 0.06) // 低于源信号的 6% 就算静音
  }
  const holes = []
  let i = 0
  while (i < nWin) {
    if (!silent[i]) { i++; continue }
    let j = i
    while (j < nWin && silent[j]) j++
    if (i > 0 && j < nWin && !silent[i - 1] && !silent[j]) holes.push({ atMs: i * 5, lenMs: (j - i) * 5 })
    i = j
  }
  // 首尾静音（AAC 编码器 priming / 结尾补齐）单独算，不算问题
  let head = 0
  while (head < nWin && silent[head]) head++
  let tail = 0
  while (tail < nWin && silent[nWin - 1 - tail]) tail++

  // 采样跳变：440Hz 正弦相邻样点最大差约 2π*440/48000*amp ≈ 0.058*amp，
  // 拼接处相位不连续会远超这个量级，听起来是"咔哒"。
  const jumpLimit = AMP * 0.25
  const jumps = []
  for (let k = 1; k < frames; k++) {
    const d = Math.abs(buf.readInt16LE(k * 4) - buf.readInt16LE((k - 1) * 4))
    if (d > jumpLimit) {
      const atMs = (k * 1000) / RATE
      if (!jumps.length || atMs - jumps[jumps.length - 1] > 50) jumps.push(atMs)
    }
  }
  return { frames, durationSec: frames / RATE, holes, headMs: head * 5, tailMs: tail * 5, jumps }
}

/**
 * --live 用：生成一段 440Hz 正弦 WAV，交给 ffplay 走默认声卡放出来。
 * ffplay 播放走的是共享混音，loopback 一定能抓到（用 SoundPlayer 在这台
 * 机器上抓出来是静音，别用它）。
 */
function writeToneWav(file, seconds = 180, amp = 0.25) {
  const frames = Math.round(seconds * RATE)
  const dataBytes = frames * 4
  const buf = Buffer.alloc(44 + dataBytes)
  buf.write('RIFF', 0, 'latin1')
  buf.writeUInt32LE(36 + dataBytes, 4)
  buf.write('WAVE', 8, 'latin1')
  buf.write('fmt ', 12, 'latin1')
  buf.writeUInt32LE(16, 16)
  buf.writeUInt16LE(1, 20)
  buf.writeUInt16LE(2, 22)
  buf.writeUInt32LE(RATE, 24)
  buf.writeUInt32LE(RATE * 4, 28)
  buf.writeUInt16LE(4, 32)
  buf.writeUInt16LE(16, 34)
  buf.write('data', 36, 'latin1')
  buf.writeUInt32LE(dataBytes, 40)
  let off = 44
  for (let i = 0; i < frames; i++) {
    const v = Math.round(Math.sin((2 * Math.PI * 440 * i) / RATE) * amp * 32767)
    buf.writeInt16LE(v, off)
    buf.writeInt16LE(v, off + 2)
    off += 4
  }
  writeFileSync(file, buf)
  return file
}

let player = null
if (LIVE) {
  const tone = writeToneWav(path.join(PROOF, 'tone.wav'), Math.max(120, WINDOW_SEC + 40))
  player = spawn('ffplay', ['-hide_banner', '-loglevel', 'quiet', '-nodisp', '-autoexit', '-loop', '0', tone], {
    stdio: 'ignore',
    windowsHide: true,
  })
  await sleep(2500)
  console.log('[fidelity] --live：已开始循环播放测试音（真实 loopback 采集）')
}

const child = spawn(backend, ['--profiles', PROFILES], { cwd: projectRoot, stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true })
const logs = []
child.stdout.setEncoding('utf8')
child.stdout.on('data', (d) => logs.push(...d.split(/\r?\n/).filter((l) => l.trim())))
const send = (cmd) => child.stdin.write(JSON.stringify(cmd) + '\n')
const countMp4 = () => {
  try {
    return readdirSync(EXPORT_DIR).filter((f) => f.endsWith('.mp4')).length
  } catch {
    return 0
  }
}

console.log(`[fidelity] 窗口 ${WINDOW_SEC}s / 录 ${SEGMENTS} 片`)
const t0 = Date.now()
const needSegments = LIVE ? WINDOW_SEC + 4 : SEGMENTS
while (Date.now() - t0 < (LIVE ? 60000 : 25000)) {
  await sleep(200)
  const s = desk()
  if (s && s.state === 'live' && (s.recordSegments ?? 0) >= needSegments && String(s.recordDir ?? '')) break
}
send({ cmd: 'flush', id: 'p-desk' })
await sleep(600)

const recordDir = String(desk()?.recordDir ?? '')
if (!recordDir) {
  console.log('[fidelity] 失败：状态里没有 recordDir')
  child.kill()
  process.exit(1)
}
if (LIVE) {
  const st = desk()
  console.log(`[fidelity] 真实采集：peak=${Number(st?.audioPeak ?? 0).toFixed(3)} rms=${Number(st?.audioRms ?? 0).toFixed(3)} ${st?.audioDetail ?? ''}`)
} else {
  // 覆盖整个回放窗口：从 (现在 - 窗口 - 5s) 一直写到 (现在 + 1s)
  const now = Date.now()
  const files = writeContinuousTone(path.join(recordDir, 'audio'), now - (WINDOW_SEC + 5) * 1000, now + 1000)
  console.log(`[fidelity] 已铺 ${files.length} 个音块（覆盖 ${WINDOW_SEC + 6}s，440Hz 连续相位）`)
}

const beforeFiles = countMp4()
const beforeJob = desk()?.exportJobId ?? 0
send({ cmd: 'export', id: 'p-desk' })
let result = null
const tExp = Date.now()
while (Date.now() - tExp < 90000) {
  await sleep(150)
  const s = desk()
  if (!s) continue
  if (countMp4() > beforeFiles && (s.exportJobId ?? 0) > beforeJob && !s.exportRunning) {
    result = s
    break
  }
}
child.stdin.end()
child.kill()
if (player) {
  try {
    player.kill()
  } catch {
    /* 已经退了 */
  }
}

const problems = []
if (!result) {
  console.log('[fidelity] 失败：导出超时')
  process.exit(1)
}
const outPath = String(result.exportOutPath ?? '')
const videoSec = Number(result.exportDurationSeconds ?? 0)
console.log(`[fidelity] 导出完成：${path.basename(outPath)} 视频 ${videoSec.toFixed(2)}s detail=${result.exportDetail}`)

const decoded = path.join(PROOF, 'decoded.pcm')
const dec = await decodeAudio(outPath, decoded)
if (dec.code !== 0) {
  console.log('[fidelity] ffmpeg 解码失败：' + dec.err.slice(0, 300))
  process.exit(1)
}
const r = analyze(decoded)
console.log(
  `[fidelity] 解出音轨 ${r.durationSec.toFixed(3)}s（视频 ${videoSec.toFixed(2)}s）` +
    ` 开头静音 ${r.headMs}ms 尾部静音 ${r.tailMs}ms`,
)
console.log(`[fidelity] 断点 ${r.holes.length} 段 / 采样跳变 ${r.jumps.length} 处`)
for (const h of r.holes.slice(0, 30)) console.log(`        断点 @${(h.atMs / 1000).toFixed(3)}s 长 ${h.lenMs}ms`)
for (const j of r.jumps.slice(0, 30)) console.log(`        跳变 @${(j / 1000).toFixed(3)}s`)

if (r.holes.length > 0) problems.push(`音轨里有 ${r.holes.length} 段断点（合计 ${r.holes.reduce((a, h) => a + h.lenMs, 0)}ms）`)
if (r.jumps.length > 2) problems.push(`音轨里有 ${r.jumps.length} 处采样跳变（拼接不对齐）`)
if (Math.abs(r.durationSec - videoSec) > 0.25) problems.push(`音轨 ${r.durationSec.toFixed(2)}s 与视频 ${videoSec.toFixed(2)}s 差太多`)

for (const l of logs) if (l.includes('audio:')) console.log('[log] ' + l.trim())

if (problems.length) {
  console.log('\n[fidelity] 失败：')
  for (const p of problems) console.log('  - ' + p)
  process.exit(1)
}
console.log('\n[fidelity] 通过：音轨连续（无断点、无跳变）')
process.exit(0)
