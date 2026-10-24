// scripts/probe-export-segment-audio.mjs —— 端到端验证「音频编码进切片 + 导出直拷音轨」。
//
// 链路：录制（带系统提示音）→ 分片内嵌 AAC 音轨 → 导出直拷分片音轨 → 成片带 AAC。
// 与 test-export-audio.mjs 的区别：那个是手动合成 PCM 喂导出端旧链路；
// 这个是验证「录制端常驻 AAC 编码器 → 分片音轨 → 导出直拷」的完整新链路。
//
// 用法：node scripts/probe-export-segment-audio.mjs [窗口秒数=20] [录片数=4]
// 退出码 0 = 成片带 AAC 音轨（链路通）；2 = 环境问题；1 = 链路断。

import { spawn } from 'node:child_process'
import { existsSync, mkdirSync, readdirSync, readFileSync, rmSync, statSync, writeFileSync } from 'node:fs'
import path from 'node:path'
import { projectRoot, tempRoot } from './electron-path.mjs'

const SERVICE = path.join(projectRoot, 'native', 'build', 'shadow-capture.exe')
const PROOF = path.join(tempRoot, 'proofs', 'export-seg-audio')
const RECORD_ROOT = path.join(PROOF, 'records')
const EXPORT_DIR = path.join(PROOF, 'exports')
const PROFILES = path.join(PROOF, 'profiles.json')
const STATUS = path.join(tempRoot, 'status', 'status-v1.json')

const WINDOW_SEC = Number(process.argv[2] ?? 20)
const RECORD_SEGMENTS = Number(process.argv[3] ?? 4)

if (!existsSync(SERVICE)) {
  console.error('[probe] 找不到采集服务，先 npm run build:native')
  process.exit(1)
}

rmSync(PROOF, { recursive: true, force: true })
mkdirSync(RECORD_ROOT, { recursive: true })
mkdirSync(EXPORT_DIR, { recursive: true })
rmSync(STATUS, { force: true })

const writeProfile = (replaySeconds) =>
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
            name: 'segau',
            enabled: true,
            source: { kind: 'monitor', monitorIndex: 0, clientAreaOnly: false, wgc: true },
            video: { fps: 30, bitrateMbps: 6, replaySeconds, segmentSeconds: 1 },
            createdAt: '2026-01-01T00:00:00.000Z',
          },
        ],
      },
      null,
      2,
    ),
  )

// 录满窗口走直拷通道（不补 NO SIGNAL），这正是「直拷分片音轨」的目标路径。
writeProfile(WINDOW_SEC)

const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
const readStatus = () => {
  try {
    return JSON.parse(readFileSync(STATUS, 'utf8'))
  } catch {
    return null
  }
}
const desk = () => readStatus()?.items?.find((s) => s.id === 'p-desk') ?? null

// 成片是否带 AAC 音轨（走 moov → trak(soun) → stbl/stsz 拿样本数，避开 fMP4 的 moof 布局差异）。
function audioSamples(file) {
  const buf = readFileSync(file)
  const boxes = (start, end, cb) => {
    for (let i = start; i + 8 <= end; ) {
      const size = buf.readUInt32BE(i)
      const type = buf.toString('ascii', i + 4, i + 8)
      if (size < 8) break
      cb(type, i + 8, Math.min(i + size, end))
      i += size
    }
  }
  const find = (s, e, w) => {
    let h = null
    boxes(s, e, (t, a, b) => {
      if (t === w && !h) h = [a, b]
    })
    return h
  }
  const children = (s, e, w) => {
    const o = []
    boxes(s, e, (t, a, b) => {
      if (t === w) o.push([a, b])
    })
    return o
  }
  let moov = null
  boxes(0, buf.length, (t, a, b) => {
    if (t === 'moov' && !moov) moov = [a, b]
  })
  if (!moov) return { soun: buf.toString('latin1').includes('soun'), mp4a: buf.toString('latin1').includes('mp4a'), samples: 0, bytes: 0 }
  for (const [ta, tb] of children(moov[0], moov[1], 'trak')) {
    const mdia = find(ta, tb, 'mdia')
    if (!mdia) continue
    const hdlr = find(mdia[0], mdia[1], 'hdlr')
    if (!hdlr) continue
    if (buf.toString('ascii', hdlr[0] + 8, hdlr[0] + 12) !== 'soun') continue
    const minf = find(mdia[0], mdia[1], 'minf')
    if (!minf) continue
    const stbl = find(minf[0], minf[1], 'stbl')
    if (!stbl) continue
    const stsz = find(stbl[0], stbl[1], 'stsz')
    if (!stsz) continue
    const ss = buf.readUInt32BE(stsz[0] + 4)
    const cnt = buf.readUInt32BE(stsz[0] + 8)
    if (ss > 0) return { soun: true, mp4a: true, samples: cnt, bytes: ss * cnt }
    let total = 0
    for (let k = 0; k < cnt; k += 1) {
      const at = stsz[0] + 12 + k * 4
      if (at + 4 > stsz[1]) break
      total += buf.readUInt32BE(at)
    }
    return { soun: true, mp4a: true, samples: cnt, bytes: total }
  }
  return { soun: false, mp4a: false, samples: 0, bytes: 0 }
}

const child = spawn(SERVICE, ['--profiles', PROFILES], { cwd: projectRoot, stdio: ['pipe', 'ignore', 'pipe'] })
child.stderr.on('data', (d) => process.stderr.write(d))
const send = (cmd) => child.stdin.write(JSON.stringify(cmd) + '\n')

// 录制期间播放系统提示音，让 WASAPI loopback 有 PCM 可采。
const beeper = spawn(
  'powershell',
  ['-NoProfile', '-Command', `
  for ($i=0; $i -lt ${WINDOW_SEC * 2}; $i++) {
    [System.Media.SystemSounds]::Beep.Play()
    Start-Sleep -Milliseconds 400
  }
`],
  { windowsHide: true, stdio: 'ignore' },
)
console.log(`[probe] 起服务，录 ${RECORD_SEGMENTS} 片（窗口 ${WINDOW_SEC}s，期间播放系统提示音）`)

const t0 = Date.now()
while (Date.now() - t0 < 20000) {
  await sleep(200)
  const s = desk()
  if (s && s.state === 'live' && (s.recordSegments ?? 0) >= RECORD_SEGMENTS) break
}
const st = desk()
console.log(`[probe] 音频通道：${st?.audioDetail ?? '（未知）'} / 活跃=${st?.audioActive ?? '?'} / 已录 ${st?.recordSegments ?? 0} 片`)

send({ cmd: 'flush', id: 'p-desk' })
await sleep(800)

const recordDir = String(desk()?.recordDir ?? '')
if (!recordDir) {
  console.error('[probe] 状态里没有 recordDir')
  child.kill()
  process.exit(2)
}

// 导出。
const beforeFiles = (() => {
  try {
    return readdirSync(EXPORT_DIR).filter((f) => f.endsWith('.mp4')).length
  } catch {
    return 0
  }
})()
const beforeJob = desk()?.exportJobId ?? 0
send({ cmd: 'export', id: 'p-desk' })
let result = null
const tExp = Date.now()
while (Date.now() - tExp < 60000) {
  await sleep(150)
  const s = desk()
  if (!s) continue
  const nowFiles = (() => {
    try {
      return readdirSync(EXPORT_DIR).filter((f) => f.endsWith('.mp4')).length
    } catch {
      return 0
    }
  })()
  if (nowFiles > beforeFiles && (s.exportJobId ?? 0) > beforeJob && !s.exportRunning) {
    result = s
    break
  }
}

child.stdin.end()
child.kill()

if (!result) {
  console.error('[probe] 导出超时')
  process.exit(1)
}
const outPath = String(result.exportOutPath ?? '')
console.log(`\n[probe] 导出：ok=${result.exportOk} 时长=${Number(result.exportDurationSeconds ?? 0).toFixed(2)}s detail=${result.exportDetail}`)
if (!outPath || !existsSync(outPath)) {
  console.error('[probe] 找不到产物文件')
  process.exit(1)
}

const a = audioSamples(outPath)
const mb = (statSync(outPath).size / 1048576).toFixed(2)
console.log(`[probe] 产物：${path.basename(outPath)} ${mb} MB / soun=${a.soun} mp4a=${a.mp4a} / 音频样本 ${a.samples} 个 ${(a.bytes / 1024).toFixed(1)} KB`)

if (!a.soun || !a.mp4a) {
  console.error('[probe] 失败：成片没有 AAC 音轨')
  process.exit(1)
}
if (a.samples <= 0 || a.bytes < 8000) {
  console.error(`[probe] 失败：音轨是空的（样本 ${a.samples} / ${a.bytes} 字节）`)
  process.exit(1)
}
console.log('[probe] 通过：音频编码进切片 + 导出直拷音轨，成片带 AAC 音轨')
process.exit(0)
