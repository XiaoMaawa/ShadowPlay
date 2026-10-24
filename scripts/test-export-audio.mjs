// scripts/test-export-audio.mjs —— 验证「导出成片里真的带音轨」
//
// 音频现在**直接编进分片**（MfEncoder 的常驻 AAC 编码器），audio/ PCM 目录
// 已删除。导出端的音源只有分片一个：直拷通道搬 AAC 样本、重编码通道解成
// PCM 重压。这里录制 + 导出，然后检查产物里有没有 AAC 音轨（MP4 里的
// mp4a / soun box）以及音轨里有没有实际样本。
//
// 注意：整机静音时 WASAPI loopback 一个包都不发，分片里的音轨就是空的 ——
// 这种情况下"音轨存在但 0 样本"判为警告而不是失败（链路是通的，只是没声）。
//
// 用法：node scripts/test-export-audio.mjs
// 退出码 0 = 通过。

import { spawn } from 'node:child_process'
import { existsSync, mkdirSync, readFileSync, readdirSync, rmSync, statSync, writeFileSync } from 'node:fs'
import path from 'node:path'
import { projectRoot, tempRoot } from './electron-path.mjs'

const backend = path.join(projectRoot, 'native', 'build', 'shadow-capture.exe')
const PROOF = path.join(tempRoot, 'proofs', 'export-audio')
const RECORD_ROOT = path.join(PROOF, 'records')
const EXPORT_DIR = path.join(PROOF, 'exports')
const PROFILES = path.join(PROOF, 'profiles.json')
const STATUS = path.join(tempRoot, 'status', 'status-v1.json')

// 参数（默认是"窗口 20 秒、只录 4 秒" → 必然补 NO SIGNAL，走重编码通道）：
//   --window=6 --segments=8 --skip-phase2
// 会变成"录满 6 秒窗口" → 不补遮罩，走**直拷**通道，用来验另一条路径。
const arg = (name, dflt) => {
  const hit = process.argv.find((a) => a.startsWith(`--${name}=`))
  return hit ? Number(hit.split('=')[1]) : dflt
}
const WINDOW_SEC = arg('window', 20)
const RECORD_SEGMENTS = arg('segments', 4)
const FPS = arg('fps', 30)
const SKIP_PHASE2 = process.argv.includes('--skip-phase2')

if (!existsSync(backend)) {
  console.error('[test] 找不到采集服务，先 npm run build:native')
  process.exit(1)
}

rmSync(PROOF, { recursive: true, force: true })
mkdirSync(RECORD_ROOT, { recursive: true })
mkdirSync(EXPORT_DIR, { recursive: true })
// 状态文件是服务每秒写的，可能残留着上一次运行的内容。不清掉的话，
// "等服务起来"会立刻被旧状态骗过去（读到上一次的 live/分片数）。
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
            name: 'audiotest',
            enabled: true,
            source: { kind: 'monitor', monitorIndex: 0, clientAreaOnly: false, wgc: true },
            video: {
              fps: FPS,
              bitrateMbps: 6,
              replaySeconds,
              segmentSeconds: 1,
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

// 窗口 / 录制时长由参数决定（见文件头的说明）。
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
      const version = buf[i + 8]
      if (version === 1) {
        const timescale = buf.readUInt32BE(i + 8 + 20)
        const duration = Number(buf.readBigUInt64BE(i + 8 + 24))
        return timescale ? duration / timescale : 0
      }
      const timescale = buf.readUInt32BE(i + 8 + 12)
      const duration = buf.readUInt32BE(i + 8 + 16)
      return timescale ? duration / timescale : 0
    }
    i += size
  }
  return 0
}

/** 产物里有没有音轨：MP4 的 hdlr 里有 "soun"，样本描述是 "mp4a"。 */
function hasAudioTrack(file) {
  const buf = readFileSync(file)
  const latin = buf.toString('latin1')
  return { soun: latin.includes('soun'), mp4a: latin.includes('mp4a') }
}

/**
 * 音轨里到底有多少**采样数据**（不是"有个空音轨"）。
 *
 * 只看 mp4a 盒子是不够的：SinkWriter 建了音轨却一个样本都没写时，产物里
 * 照样有 mp4a 和 soun，但播出来是一片静音 —— 那等于音频链路没通。
 * 这里走 moov → trak*（挑 handler 是 soun 的那个）→ stbl/stsz，把
 * sample_count 和总字节数算出来。
 */
function audioPayload(file) {
  const buf = readFileSync(file)
  const boxes = (start, end, cb) => {
    for (let i = start; i + 8 <= end; ) {
      const size = buf.readUInt32BE(i)
      const type = buf.toString('ascii', i + 4, i + 8)
      if (size < 8) break
      const stop = Math.min(i + size, end)
      cb(type, i + 8, stop)
      i += size
    }
  }
  const findChild = (start, end, want) => {
    let hit = null
    boxes(start, end, (type, a, b) => {
      if (type === want && !hit) hit = [a, b]
    })
    return hit
  }
  const children = (start, end, want) => {
    const out = []
    boxes(start, end, (type, a, b) => {
      if (type === want) out.push([a, b])
    })
    return out
  }

  let moov = null
  boxes(0, buf.length, (type, a, b) => {
    if (type === 'moov' && !moov) moov = [a, b]
  })
  if (!moov) return { bytes: 0, samples: 0 }

  for (const [trakA, trakB] of children(moov[0], moov[1], 'trak')) {
    const mdia = findChild(trakA, trakB, 'mdia')
    if (!mdia) continue
    const hdlr = findChild(mdia[0], mdia[1], 'hdlr')
    if (!hdlr) continue
    // hdlr 载荷：version+flags(4) + pre_defined(4) + handler_type(4)
    if (buf.toString('ascii', hdlr[0] + 8, hdlr[0] + 12) !== 'soun') continue
    const minf = findChild(mdia[0], mdia[1], 'minf')
    if (!minf) continue
    const stbl = findChild(minf[0], minf[1], 'stbl')
    if (!stbl) continue
    const stsz = findChild(stbl[0], stbl[1], 'stsz')
    if (!stsz) continue
    // stsz 载荷：version+flags(4) + sample_size(4) + sample_count(4) [+ 每样本 4 字节]
    const sampleSize = buf.readUInt32BE(stsz[0] + 4)
    const count = buf.readUInt32BE(stsz[0] + 8)
    if (sampleSize > 0) return { bytes: sampleSize * count, samples: count }
    let total = 0
    for (let k = 0; k < count; k += 1) {
      const at = stsz[0] + 12 + k * 4
      if (at + 4 > stsz[1]) break
      total += buf.readUInt32BE(at)
    }
    return { bytes: total, samples: count }
  }
  return { bytes: 0, samples: 0 }
}

/** 合成 PCM 的旧机制已删除：音频现在由采集端直接编进分片音轨。 */

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

const send = (cmd) => child.stdin.write(JSON.stringify(cmd) + '\n')
const countMp4 = () => {
  try {
    return readdirSync(EXPORT_DIR).filter((f) => f.endsWith('.mp4')).length
  } catch {
    return 0
  }
}

console.log(`[test] 启动采集服务：窗口 ${WINDOW_SEC}s，录 ${RECORD_SEGMENTS} 片`)
const t0 = Date.now()
while (Date.now() - t0 < 20000) {
  await sleep(200)
  const s = desk()
  // recordDir 也一起等：状态文件刚写出来时，录制的目录可能还没分好。
  if (s && s.state === 'live' && (s.recordSegments ?? 0) >= RECORD_SEGMENTS && String(s.recordDir ?? '')) break
}
send({ cmd: 'flush', id: 'p-desk' })
await sleep(600)

const st = desk()
const recordDir = String(st?.recordDir ?? '')
if (!recordDir) {
  console.log('[test] 失败：状态里没有 recordDir')
  child.kill()
  process.exit(1)
}// 音频由采集端编进分片：整机静音时分片音轨为空，属正常（链路通、无声音）。
console.log(`[test] 录像目录：${recordDir}`)
console.log(`[test] 声音通道：${st?.audioDetail ?? '（服务没报）'}`)
const machineSilent = !st?.audioActive

const beforeFiles = countMp4()
const beforeJob = desk()?.exportJobId ?? 0
send({ cmd: 'export', id: 'p-desk' })
let result = null
const tExp = Date.now()
while (Date.now() - tExp < 60000) {
  await sleep(150)
  const s = desk()
  if (!s) continue
  if (countMp4() > beforeFiles && (s.exportJobId ?? 0) > beforeJob && !s.exportRunning) {
    result = s
    break
  }
}
const problems = []

function checkExport(label, r) {
  if (!r) {
    problems.push(`${label}：导出超时`)
    return
  }
  const outPath = String(r.exportOutPath ?? '')
  console.log(
    `[test] ${label}导出：ok=${r.exportOk} 时长=${Number(r.exportDurationSeconds ?? 0).toFixed(2)}s detail=${r.exportDetail}`,
  )
  if (!r.exportOk) problems.push(`${label}导出失败：${r.exportDetail}`)
  if (!outPath || !existsSync(outPath)) {
    problems.push(`${label}找不到产物文件`)
    return
  }
  const dur = mp4Duration(outPath)
  const track = hasAudioTrack(outPath)
  const payload = audioPayload(outPath)
  const mb = (statSync(outPath).size / 1048576).toFixed(2)
  console.log(
    `[test] ${label}产物：${path.basename(outPath)} ${dur.toFixed(2)}s / ${mb} MB / 音轨 soun=${track.soun} mp4a=${track.mp4a}` +
      ` / 音频样本 ${payload.samples} 个 ${(payload.bytes / 1024).toFixed(1)} KB`,
  )
  if (!track.soun || !track.mp4a) problems.push(`${label}产物里没有 AAC 音轨（mp4a 缺失）`)
  // 光有音轨盒子不够：一个样本都没写时也有 mp4a。整机静音时这是正常现象
  // （WASAPI loopback 无包 → 分片音轨 0 样本），只警告不判失败。
  if (payload.samples <= 0 || payload.bytes < 8000) {
    const msg = `${label}音轨是空的（样本 ${payload.samples} 个 / ${payload.bytes} 字节）`
    if (machineSilent) console.log(`[test] 警告（整机静音，属预期）：${msg}`)
    else problems.push(msg)
  }
  if (dur < 3) problems.push(`${label}成片只有 ${dur.toFixed(2)}s，太短`)
}

// 录满窗口 → 没有补遮罩 → 走直拷；录不够 → 补 NO SIGNAL → 走重编码。
const phase1Mode = RECORD_SEGMENTS >= WINDOW_SEC ? '直拷' : '重编码'
checkExport(`第一段（${phase1Mode}）`, result)

// ---------------------------------------------------------------- 第二段 ----
// 回放窗口缩到 6 秒并录满 8 秒 → 窗口里全是真实画面（不用补遮罩），导出走
// **直拷**通道。音频走的是同一条 AudioMux，但调用点在另一个循环里，必须验。
if (SKIP_PHASE2) {
  console.log('\n[test] 已跳过第二段（--skip-phase2）')
} else {
console.log('\n[test] 第二段：录满窗口走直拷通道')
writeProfile(6)
send({ cmd: 'reload', id: 'p-desk' })
const t2 = Date.now()
while (Date.now() - t2 < 25000) {
  await sleep(200)
  const s = desk()
  if (s && s.state === 'live' && (s.recordSegments ?? 0) >= 8) break
}
send({ cmd: 'flush', id: 'p-desk' })
await sleep(600)

const recDir2 = String(desk()?.recordDir ?? '')
if (!recDir2) console.log('[test] 注意：第二轮没拿到 recordDir')

const before2 = countMp4()
const job2 = desk()?.exportJobId ?? 0
send({ cmd: 'export', id: 'p-desk' })
let result2 = null
const tExp2 = Date.now()
while (Date.now() - tExp2 < 60000) {
  await sleep(150)
  const s = desk()
  if (!s) continue
  if (countMp4() > before2 && (s.exportJobId ?? 0) > job2 && !s.exportRunning) {
    result2 = s
    break
  }
}
checkExport('第二段（直拷）', result2)
}

child.stdin.end()
child.kill()

for (const l of logs) {
  if (l.includes('audio:') || l.includes('export_plan')) console.log('[log] ' + l.trim())
}

if (problems.length) {
  console.log('\n[test] 失败：')
  for (const item of problems) console.log('  - ' + item)
  process.exit(1)
}
console.log('\n[test] 通过：重编码与直拷两条通道的成片都带 AAC 音轨')
process.exit(0)
