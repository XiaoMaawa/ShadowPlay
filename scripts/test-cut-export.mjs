// scripts/test-cut-export.mjs —— 「从上次保存处继续」这条导出路径的时长/音轨检查
//
// 为什么单独一个：界面点导出走的是**插件**（shadow_ring.node 的 exportStart），
// 且设置里开着 continueAfterSave 时会多传一个 startUnixMs（上次保存的点击时刻）。
// 服务命令通道的 export 没有这个参数，所以以前的测试根本覆盖不到它。
//
// 这个脚本自己起服务（窗口源，真采集真声音），录够一段之后直接调插件导出，
// 然后量：
//   * 成片时长有没有超过回放窗口（用户报的 22/23 秒）
//   * 音轨在不在、有没有数据（用户报的"捕捉不到声音"）
//
// 用法：
//   node scripts/test-cut-export.mjs                 # 切点 = 22 秒前（应该 ≤20 秒）
//   node scripts/test-cut-export.mjs --cut=2         # 切点 = 2 秒前
//   node scripts/test-cut-export.mjs --profiles=xxx.json

import { spawn, spawnSync } from 'node:child_process'
import { existsSync, mkdirSync, readFileSync, readdirSync, rmSync, statSync, writeFileSync } from 'node:fs'
import { createRequire } from 'node:module'
import path from 'node:path'
import { projectRoot, tempRoot } from './electron-path.mjs'

const require = createRequire(import.meta.url)
const backend = path.join(projectRoot, 'native', 'build', 'shadow-capture.exe')
const addonPath = path.join(projectRoot, 'native', 'build', 'shadow_ring.nodejs.node')
const PROOF = path.join(tempRoot, 'proofs', 'cut-export')
const RECORD_ROOT = path.join(PROOF, 'records')
const EXPORT_DIR = path.join(PROOF, 'exports')
const PROFILES = path.join(PROOF, 'profiles.json')
const STATUS = path.join(tempRoot, 'status', 'status-v1.json')

const arg = (name, dflt) => {
  const hit = process.argv.find((a) => a.startsWith(`--${name}=`))
  return hit ? hit.split('=')[1] : dflt
}
const WINDOW_SEC = Number(arg('window', 20))
const CUT_AGO = Number(arg('cut', 22)) // 切点距现在多少秒
const TARGET = arg('profiles', '') // 可选：从某份真实配置里抄「窗口源」那一路

if (!existsSync(backend)) {
  console.error('[test] 找不到采集服务，先 npm run build:native')
  process.exit(1)
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
const readStatus = () => {
  try {
    return JSON.parse(readFileSync(STATUS, 'utf8'))
  } catch {
    return null
  }
}
const item = () => readStatus()?.items?.[0] ?? null

// ---- 配置：优先抄真实配置里的窗口源那一路（这样测的是用户实际在用的匹配规则）
let source = { kind: 'monitor', monitorIndex: 0, clientAreaOnly: false, wgc: true }
// 匹配规则在 profile 上（不在 source 里），要一起抄过来，否则窗口源会报
// "窗口源但没有配匹配规则"。
let match = undefined
if (TARGET && existsSync(TARGET)) {
  const real = JSON.parse(readFileSync(TARGET, 'utf8'))
  const win = (real.profiles ?? []).find((p) => p.source?.kind === 'window')
  if (win) {
    source = win.source
    match = win.match
  }
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
          name: 'cuttest',
          enabled: true,
          source,
          match,
          video: {
            fps: 60,
            bitrateMbps: 8,
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

const child = spawn(backend, ['--profiles', PROFILES], {
  cwd: projectRoot,
  stdio: ['pipe', 'pipe', 'pipe'],
  windowsHide: true,
})
const logs = []
child.stdout.setEncoding('utf8')
child.stdout.on('data', (d) => logs.push(...d.split(/\r?\n/).filter((l) => l.trim())))
child.stderr.setEncoding('utf8')
child.stderr.on('data', (d) => logs.push(`[stderr] ${d.trim()}`))
child.stdin.write('') // 保持管道

console.log(`[test] 采集源：${source.kind}，窗口 ${WINDOW_SEC}s，切点 = ${CUT_AGO}s 前`)

// ---- 音源 ----------------------------------------------------------------
//
// 【为什么必须自己造声音】系统混音在"完全没有任何进程在渲染音频"时是**一个包
// 都不送**的（不是送静音包）。于是这个测试的结果取决于跑的时候机器上是不是
// 恰好在放歌：安静环境下会量到 bytes=0、没有音轨，看着像"采音坏了"。
// 默认放一段 440Hz 正弦，--tone=0 可以关掉。
const toneOn = arg('tone', '1') !== '0'
let tone = null
function stopTone() {
  if (!tone) return
  const pid = tone.pid
  try {
    tone.kill('SIGKILL')
  } catch {
    /* 已经退了 */
  }
  try {
    spawnSync('taskkill', ['/pid', String(pid), '/T', '/F'], { stdio: 'ignore', windowsHide: true })
  } catch {
    /* 忽略 */
  }
  tone = null
}
process.on('exit', stopTone)
if (toneOn) {
  try {
    tone = spawn(
      'ffplay',
      [
        '-nodisp',
        '-autoexit',
        '-loglevel',
        'error',
        '-f',
        'lavfi',
        '-i',
        `sine=frequency=440:duration=${WINDOW_SEC + 40}`,
      ],
      { stdio: 'ignore', windowsHide: true },
    )
    console.log('[test] 已启动 440Hz 音源（ffplay）')
  } catch (err) {
    console.log(`[test] 启动音源失败，这次不检查音轨内容：${err?.message ?? err}`)
  }
}

// ---- 1. 录到窗口塞满（还要额外几秒，好让切点落在窗口内部）
const needMs = (WINDOW_SEC + 8) * 1000
const t0 = Date.now()
let s = null
while (Date.now() - t0 < needMs + 20000) {
  await sleep(300)
  s = item()
  if (s && s.state === 'live' && Date.now() - t0 > needMs) break
}
if (!s || s.state !== 'live') {
  console.log(`[test] 失败：服务没进入 live（${s?.state}：${s?.detail ?? ''}）`)
  child.kill()
  process.exit(1)
}
console.log(
  `[test] 已录 ${((Date.now() - t0) / 1000).toFixed(1)}s：segments=${s.recordSegments}` +
    ` audio=${s.audioActive} peak=${(s.audioPeak ?? 0).toFixed(3)} bytes=${s.audioBytes}` +
    ` | ${s.audioDetail ?? ''}`,
)
child.stdin.write(JSON.stringify({ cmd: 'flush', id: s.id }) + '\n')
await sleep(1200)

const recordDir = String(item()?.recordDir ?? '')
const segmentsDir = path.join(recordDir, 'segments')
console.log(`[test] 分片目录：${segmentsDir}`)

// ---- 2. 直接调插件导出（带 startUnixMs = 界面"从上次保存处继续"传的那个）
const addon = require(addonPath)
const startUnixMs = Date.now() - CUT_AGO * 1000
const res = addon.exportStart(segmentsDir, EXPORT_DIR, 'cuttest', WINDOW_SEC, 60, 2, startUnixMs)
if (!res?.ok) {
  console.log(`[test] 失败：导出没能开始（${res?.error}）`)
  child.kill()
  process.exit(1)
}
let st = null
const tExp = Date.now()
while (Date.now() - tExp < 180000) {
  await sleep(120)
  st = addon.exportStatus()
  if (st && st.running !== true) break
}
child.stdin.end()
child.kill()
stopTone()

const problems = []
if (!st || st.running === true) {
  console.log('[test] 失败：导出超时')
  process.exit(1)
}
const outPath = String(st.outPath ?? '')
console.log(
  `[test] 导出：ok=${st.ok} 服务报时长=${Number(st.durationSeconds ?? 0).toFixed(2)}s` +
    ` 分片=${st.segmentCount} 详情=${st.detail ?? ''}`,
)
if (!st.ok) {
  console.log(`[test] 失败：${st.detail}`)
  process.exit(1)
}

// ---- 3. 量产物
function mp4Duration(file) {
  const buf = readFileSync(file)
  let moov = -1
  for (let i = 0; i + 8 <= buf.length; ) {
    const size = buf.readUInt32BE(i)
    const type = buf.toString('ascii', i + 4, i + 8)
    if (size < 8) break
    if (type === 'moov') {
      moov = i
      break
    }
    i += size
  }
  if (moov < 0) return 0
  const end = moov + buf.readUInt32BE(moov)
  for (let i = moov + 8; i + 8 <= end; ) {
    const size = buf.readUInt32BE(i)
    const type = buf.toString('ascii', i + 4, i + 8)
    if (size < 8) break
    if (type === 'mvhd') {
      const v = buf[i + 8]
      if (v === 1) {
        const ts = buf.readUInt32BE(i + 8 + 20)
        const d = Number(buf.readBigUInt64BE(i + 8 + 24))
        return ts ? d / ts : 0
      }
      const ts = buf.readUInt32BE(i + 8 + 12)
      return ts ? buf.readUInt32BE(i + 8 + 16) / ts : 0
    }
    i += size
  }
  return 0
}

const dur = mp4Duration(outPath)
const mb = (statSync(outPath).size / 1048576).toFixed(2)
const latin = readFileSync(outPath).toString('latin1')
const hasAudio = latin.includes('soun') && latin.includes('mp4a')
console.log(
  `[test] 产物：${path.basename(outPath)} ${dur.toFixed(2)}s / ${mb} MB / 音轨=${hasAudio ? '有' : '没有'}`,
)

// 音轨里到底有没有声音：解回 PCM 看最大幅度（全静音 = 没采到）
let decoded = ''
if (hasAudio) {
  const pcmPath = path.join(PROOF, 'decoded.pcm')
  const r = spawnSync('ffmpeg', ['-y', '-hide_banner', '-loglevel', 'error', '-i', outPath, '-vn', '-f', 's16le', '-acodec', 'pcm_s16le', '-ar', '48000', '-ac', '2', pcmPath], {
    stdio: 'ignore',
  })
  if (r.status === 0 && existsSync(pcmPath)) {
    const buf = readFileSync(pcmPath)
    let peak = 0
    let sum = 0
    const n = Math.floor(buf.length / 2)
    for (let i = 0; i < n; i += 1) {
      const v = buf.readInt16LE(i * 2) / 32768
      const a = Math.abs(v)
      if (a > peak) peak = a
      sum += v * v
    }
    const rms = n ? Math.sqrt(sum / n) : 0
    // 解码出来的是**立体声** 16bit：一帧 4 字节，所以秒数是 n / (48000 * 2)。
    // （早先只除 48000，把 20 秒的成片显示成 40 秒，看着像音轨超长。）
    decoded = `解码后 峰值=${peak.toFixed(4)} 有效值=${rms.toFixed(4)}（${(n / 96000).toFixed(2)}s）`
    console.log('[test] ' + decoded)
    if (peak < 0.002) problems.push(`音轨几乎是静音（峰值 ${peak.toFixed(4)}），等于没采到声音`)
  }
}

const over = dur - WINDOW_SEC
if (over > 1.0) problems.push(`时长 ${dur.toFixed(2)}s，超出回放窗口 ${over.toFixed(2)}s`)
if (dur < 1.0) problems.push(`时长只有 ${dur.toFixed(2)}s`)
if (!hasAudio) problems.push('产物里没有音轨')

for (const l of logs) {
  if (l.includes('audio:') || l.includes('export_plan')) console.log('[log] ' + l.trim().slice(0, 1600))
}

if (problems.length) {
  console.log('\n[test] 失败：')
  for (const p of problems) console.log('  - ' + p)
  process.exit(1)
}
console.log('\n[test] 通过：切点导出时长不超过窗口，且音轨有声音')
process.exit(0)
