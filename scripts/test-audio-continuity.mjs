// 音频连续性探针：播放一段持续的 440Hz 正弦音，让 WASAPI loopback 真的产包，
// 采集若干秒后检查落盘 PCM 里有没有"空洞"（不该出现的静音段）。
//
// 用法：
//   node scripts/test-audio-continuity.mjs [--seconds=20] [--num=1] [--no-play]
//
// 为什么要放声音：很多驱动的 loopback 在系统没有音频渲染时根本不发包，
// 那样测出来"没有音频"是假的。
import { spawn } from 'node:child_process'
import { existsSync, mkdirSync, readdirSync, readFileSync, rmSync, statSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import path from 'node:path'

const ROOT = path.resolve(import.meta.dirname, '..')
const arg = (name, dflt) => {
  const hit = process.argv.find((a) => a.startsWith(`--${name}=`))
  return hit ? Number(hit.split('=')[1]) : dflt
}
const SECONDS = arg('seconds', 20)
const PROFILE_NUM = arg('num', 1)
const NO_PLAY = process.argv.includes('--no-play')

const RECORD_ROOT = path.join(tmpdir(), 'shadow-play', 'app', 'records', String(PROFILE_NUM))
const AUDIO_DIR = path.join(RECORD_ROOT, 'audio')

const RATE = 48000
const CH = 2

/** 生成一段可循环播放的 48k/16bit/立体声正弦 WAV。 */
function writeToneWav(file, seconds = 120, freq = 440, amp = 0.25) {
  const frames = seconds * RATE
  const dataBytes = frames * CH * 2
  const buf = Buffer.alloc(44 + dataBytes)
  buf.write('RIFF', 0, 'latin1')
  buf.writeUInt32LE(36 + dataBytes, 4)
  buf.write('WAVE', 8, 'latin1')
  buf.write('fmt ', 12, 'latin1')
  buf.writeUInt32LE(16, 16)
  buf.writeUInt16LE(1, 20) // PCM
  buf.writeUInt16LE(CH, 22)
  buf.writeUInt32LE(RATE, 24)
  buf.writeUInt32LE(RATE * CH * 2, 28)
  buf.writeUInt16LE(CH * 2, 32)
  buf.writeUInt16LE(16, 34)
  buf.write('data', 36, 'latin1')
  buf.writeUInt32LE(dataBytes, 40)
  let off = 44
  for (let i = 0; i < frames; i++) {
    const v = Math.round(Math.sin((2 * Math.PI * freq * i) / RATE) * amp * 32767)
    buf.writeInt16LE(v, off)
    buf.writeInt16LE(v, off + 2)
    off += 4
  }
  writeFileSync(file, buf)
  return file
}

/** 读 audio 目录里所有块，按文件名（起始墙钟）排序拼成一条时间线。 */
function loadTimeline(dir) {
  if (!existsSync(dir)) return null
  const files = readdirSync(dir)
    .filter((f) => /^a_\d+\.pcm$/.test(f))
    .map((f) => ({ f, startMs: Number(f.slice(2, -4)), size: statSync(path.join(dir, f)).size }))
    .sort((a, b) => a.startMs - b.startMs)
  if (!files.length) return null
  const first = files[0].startMs
  const last = files[files.length - 1]
  const spanMs = last.startMs + Math.floor((last.size / 4) * 1000 / RATE) - first
  const total = Math.max(1, spanMs) * (RATE / 1000)
  const acc = new Float32Array(Math.ceil(total) * CH)
  const cover = new Uint8Array(Math.ceil(total))

  for (const it of files) {
    const buf = readFileSync(path.join(dir, it.f))
    const frames = Math.floor(buf.length / 4)
    const base = Math.round(((it.startMs - first) * RATE) / 1000)
    for (let i = 0; i < frames; i++) {
      const idx = base + i
      if (idx < 0 || idx >= total) continue
      acc[idx * 2] = buf.readInt16LE(i * 4) / 32768
      acc[idx * 2 + 1] = buf.readInt16LE(i * 4 + 2) / 32768
      cover[idx] = 1
    }
  }
  return { files, first, spanMs, frames: total, acc, cover }
}

function analyze(tl, winMs = 5) {
  const win = Math.round((winMs * RATE) / 1000)
  const nWin = Math.floor(tl.frames / win)
  const silent = []
  for (let w = 0; w < nWin; w++) {
    let peak = 0
    for (let i = w * win; i < (w + 1) * win; i++) {
      const a = Math.abs(tl.acc[i * 2])
      if (a > peak) peak = a
    }
    silent.push(peak < 0.01)
  }
  // 找出"被非静音包围的静音段"—— 那才是听到的断点（首尾不算）
  const holes = []
  let i = 0
  while (i < nWin) {
    if (!silent[i]) { i++; continue }
    let j = i
    while (j < nWin && silent[j]) j++
    const beforeOk = i > 0 && !silent[i - 1]
    const afterOk = j < nWin && !silent[j]
    if (beforeOk && afterOk) holes.push({ atMs: i * winMs, lenMs: (j - i) * winMs })
    i = j
  }
  // 首尾的静音（开始还没出声 / 结束已经停了）单独统计
  let headSilent = 0
  while (headSilent < nWin && silent[headSilent]) headSilent++
  let tailSilent = 0
  while (tailSilent < nWin && silent[nWin - 1 - tailSilent]) tailSilent++
  return { holes, headMs: headSilent * winMs, tailMs: tailSilent * winMs, nWin }
}

const tone = path.join(tmpdir(), 'shadow-tone.wav')
writeToneWav(tone, Math.max(120, SECONDS + 30))
console.log(`[probe] 音源 ${tone}`)

let player = null
if (!NO_PLAY) {
  // 用 ffplay 而不是 SoundPlayer：ffplay 走默认渲染端点的共享混音，
  // loopback 一定能抓到（SoundPlayer 在这台机器上抓出来是静音）。
  player = spawn('ffplay', ['-hide_banner', '-loglevel', 'quiet', '-nodisp', '-autoexit', '-loop', '0', tone], {
    stdio: 'ignore',
    windowsHide: true,
  })
  await new Promise((r) => setTimeout(r, 2500))
  console.log('[probe] 已开始循环播放测试音')
}

rmSync(AUDIO_DIR, { recursive: true, force: true })
mkdirSync(AUDIO_DIR, { recursive: true })

const svc = spawn(path.join(ROOT, 'native', 'build', 'shadow-capture.exe'), ['--profiles', path.join(ROOT, 'profiles.json')], {
  cwd: ROOT,
  stdio: 'ignore',
  windowsHide: true,
})
console.log(`[probe] 采集服务 pid=${svc.pid}，采 ${SECONDS}s …`)
await new Promise((r) => setTimeout(r, SECONDS * 1000))

const statusFile = path.join(tmpdir(), 'shadow-play', 'app', 'status', 'status-v1.json')
try {
  const s = JSON.parse(readFileSync(statusFile, 'utf8'))
  for (const it of s.items) {
    console.log(`[status] ${it.name} audio=${it.audioActive} bytes=${it.audioBytes} detail=${it.audioDetail}`)
  }
} catch {
  console.log('[status] 读不到状态文件')
}

spawn('taskkill', ['/F', '/IM', 'shadow-capture.exe'], { stdio: 'ignore', windowsHide: true })
if (player) {
  try { player.kill() } catch {}
}
await new Promise((r) => setTimeout(r, 1200))

const tl = loadTimeline(AUDIO_DIR)
if (!tl) {
  console.log('[probe] 没有采到任何 PCM —— 播放没走默认输出设备？')
  process.exit(2)
}
const r = analyze(tl)
console.log(
  `[probe] 块数=${tl.files.length} 跨度=${(tl.spanMs / 1000).toFixed(2)}s 帧数=${tl.frames} ` +
    `(${(tl.frames / RATE).toFixed(2)}s) 开头静音=${r.headMs}ms 尾部静音=${r.tailMs}ms`,
)
const totalHoleMs = r.holes.reduce((a, h) => a + h.lenMs, 0)
console.log(`[probe] 中断段数=${r.holes.length} 合计=${totalHoleMs}ms`)
for (const h of r.holes.slice(0, 40)) {
  console.log(`        断点 @${(h.atMs / 1000).toFixed(3)}s 长 ${h.lenMs}ms`)
}
// 每块都要能无缝接上：相邻两块的重叠/空洞都算问题
for (let i = 1; i < tl.files.length; i++) {
  const prev = tl.files[i - 1]
  const prevEnd = prev.startMs + Math.floor((prev.size / 4) * 1000 / RATE)
  const gap = tl.files[i].startMs - prevEnd
  if (Math.abs(gap) > 2) {
    console.log(`        块缝 @${((tl.files[i].startMs - tl.first) / 1000).toFixed(3)}s 间隙 ${gap}ms`)
  }
}
process.exit(r.holes.length === 0 ? 0 : 1)
