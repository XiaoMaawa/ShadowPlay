// scripts/test-save-export.mjs —— 复现「点保存之后导出，音量丢失 / 时长超长」
//
// 用户报的现象：
//   1) 点保存后过一会，有概率捕捉不到声音；
//   2) 点保存后导出的成片时长会超过设定的回放窗口（设 20 秒，出来 22 秒）。
//
// 两个都指向同一条路径：reload 重建会话。所以这里按真实节奏走一遍：
//   启动 → 录满回放窗口 → 发 reload（等价于点保存）→ 再录一段 → 导出，
// 然后量成片时长、音轨样本数，并把服务打出来的 export_plan 原样贴出来。
//
// 用法：
//   node scripts/test-save-export.mjs                 # 默认 20s 窗口 / 2s 分片
//   node scripts/test-save-export.mjs --rounds=3      # 连跑 3 轮看概率
//   node scripts/test-save-export.mjs --tone          # 顺带用 ffplay 放测试音
// 退出码 0 = 通过。

import { spawn } from 'node:child_process'
import { existsSync, mkdirSync, readFileSync, readdirSync, rmSync, statSync, writeFileSync } from 'node:fs'
import path from 'node:path'
import { projectRoot, tempRoot } from './electron-path.mjs'

const backend = path.join(projectRoot, 'native', 'build', 'shadow-capture.exe')
const PROOF = path.join(tempRoot, 'proofs', 'save-export')
const RECORD_ROOT = path.join(PROOF, 'records')
const EXPORT_DIR = path.join(PROOF, 'exports')
const PROFILES = path.join(PROOF, 'profiles.json')
const STATUS = path.join(tempRoot, 'status', 'status-v1.json')
const TONE = path.join(PROOF, 'tone.wav')

const arg = (name, dflt) => {
  const hit = process.argv.find((a) => a.startsWith(`--${name}=`))
  return hit ? Number(hit.split('=')[1]) : dflt
}
const WINDOW_SEC = arg('window', 20)
const SEGMENT_SEC = arg('segment', 2)
const FPS = arg('fps', 60)
const ROUNDS = arg('rounds', 1)
const WITH_TONE = process.argv.includes('--tone')

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
        const ts = buf.readUInt32BE(i + 8 + 20)
        const d = Number(buf.readBigUInt64BE(i + 8 + 24))
        return ts ? d / ts : 0
      }
      const ts = buf.readUInt32BE(i + 8 + 12)
      const d = buf.readUInt32BE(i + 8 + 16)
      return ts ? d / ts : 0
    }
    i += size
  }
  return 0
}

/** 每条轨道的时长（tkhd 的 duration），用来区分"视频长"还是"音频长"。 */
function trackDurations(file) {
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
  const children = (start, end, want) => {
    const out = []
    boxes(start, end, (t, a, b) => {
      if (t === want) out.push([a, b])
    })
    return out
  }
  const findChild = (start, end, want) => children(start, end, want)[0] ?? null
  const moov = children(0, buf.length, 'moov')[0]
  if (!moov) return []
  let movieTs = 0
  for (const [a] of children(moov[0], moov[1], 'mvhd')) {
    const v = buf[a]
    movieTs = v === 1 ? buf.readUInt32BE(a + 20) : buf.readUInt32BE(a + 12)
  }
  const out = []
  for (const [ta, tb] of children(moov[0], moov[1], 'trak')) {
    const mdia = findChild(ta, tb, 'mdia')
    const tkhd = findChild(ta, tb, 'tkhd')
    if (!mdia || !tkhd) continue
    const hdlr = findChild(mdia[0], mdia[1], 'hdlr')
    const kind = hdlr ? buf.toString('ascii', hdlr[0] + 8, hdlr[0] + 12) : '?'
    const v = buf[tkhd[0]]
    const dur = v === 1 ? Number(buf.readBigUInt64BE(tkhd[0] + 28)) : buf.readUInt32BE(tkhd[0] + 20)
    out.push({ kind, sec: movieTs ? dur / movieTs : 0 })
  }
  return out
}

function audioPayload(file) {
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
  const children = (start, end, want) => {
    const out = []
    boxes(start, end, (t, a, b) => {
      if (t === want) out.push([a, b])
    })
    return out
  }
  const findChild = (start, end, want) => children(start, end, want)[0] ?? null
  const moov = children(0, buf.length, 'moov')[0]
  if (!moov) return { bytes: 0, samples: 0 }
  for (const [ta, tb] of children(moov[0], moov[1], 'trak')) {
    const mdia = findChild(ta, tb, 'mdia')
    if (!mdia) continue
    const hdlr = findChild(mdia[0], mdia[1], 'hdlr')
    if (!hdlr || buf.toString('ascii', hdlr[0] + 8, hdlr[0] + 12) !== 'soun') continue
    const minf = findChild(mdia[0], mdia[1], 'minf')
    const stbl = minf ? findChild(minf[0], minf[1], 'stbl') : null
    const stsz = stbl ? findChild(stbl[0], stbl[1], 'stsz') : null
    if (!stsz) continue
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

/** 440Hz 正弦，铺满整个回放窗口（这台机器静音时采集端不产 PCM，只能自己造）。 */
function writeTone(dir, startMs, seconds) {
  mkdirSync(dir, { recursive: true })
  const rate = 48000
  const frames = Math.round(seconds * rate)
  const buf = Buffer.alloc(frames * 4)
  for (let i = 0; i < frames; i += 1) {
    const v = Math.round(Math.sin((2 * Math.PI * 440 * i) / rate) * 8000)
    buf.writeInt16LE(v, i * 4)
    buf.writeInt16LE(v, i * 4 + 2)
  }
  writeFileSync(path.join(dir, `a_${startMs}.pcm`), buf)
  return frames
}

const writeProfile = () =>
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
            name: 'save-export',
            enabled: true,
            source: { kind: 'monitor', monitorIndex: 0, clientAreaOnly: false, wgc: true },
            video: {
              fps: FPS,
              bitrateMbps: 6,
              replaySeconds: WINDOW_SEC,
              segmentSeconds: SEGMENT_SEC,
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

rmSync(PROOF, { recursive: true, force: true })
mkdirSync(RECORD_ROOT, { recursive: true })
mkdirSync(EXPORT_DIR, { recursive: true })
rmSync(STATUS, { force: true })
writeProfile()

// 真音源（可选）：没有它的时候系统混音一个包都不发，只能靠合成 PCM。
let tone = null
if (WITH_TONE) {
  const rate = 48000
  const frames = rate * 3
  const buf = Buffer.alloc(44 + frames * 4)
  buf.write('RIFF', 0)
  buf.writeUInt32LE(36 + frames * 4, 4)
  buf.write('WAVE', 8)
  buf.write('fmt ', 12)
  buf.writeUInt32LE(16, 16)
  buf.writeUInt16LE(1, 20)
  buf.writeUInt16LE(2, 22)
  buf.writeUInt32LE(rate, 24)
  buf.writeUInt32LE(rate * 4, 28)
  buf.writeUInt16LE(4, 32)
  buf.writeUInt16LE(16, 34)
  buf.write('data', 36)
  buf.writeUInt32LE(frames * 4, 40)
  for (let i = 0; i < frames; i += 1) {
    const v = Math.round(Math.sin((2 * Math.PI * 440 * i) / rate) * 7000)
    buf.writeInt16LE(v, 44 + i * 4)
    buf.writeInt16LE(v, 44 + i * 4 + 2)
  }
  writeFileSync(TONE, buf)
  tone = spawn('ffplay', ['-hide_banner', '-loglevel', 'quiet', '-nodisp', '-autoexit', '-loop', '0', TONE], {
    stdio: 'ignore',
    windowsHide: true,
  })
  console.log('[test] 已循环播放测试音（ffplay）')
}

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

async function waitLive(minSegments, maxMs) {
  const t0 = Date.now()
  while (Date.now() - t0 < maxMs) {
    await sleep(200)
    const s = desk()
    if (s && s.state === 'live' && (s.recordSegments ?? 0) >= minSegments && String(s.recordDir ?? '')) return s
  }
  return desk()
}

const problems = []
const needSegsBefore = Math.ceil(WINDOW_SEC / SEGMENT_SEC) + 2
const needSegsAfter = 6

console.log(
  `[test] 窗口 ${WINDOW_SEC}s / 分片 ${SEGMENT_SEC}s / ${FPS}fps / 共 ${ROUNDS} 轮` +
    `（保存前录 ${needSegsBefore} 片，保存后再录 ${needSegsAfter} 片）`,
)

for (let round = 1; round <= ROUNDS; round += 1) {
  console.log(`\n[test] ===== 第 ${round} 轮 =====`)
  rmSync(STATUS, { force: true })
  writeProfile()
  const before = countMp4()

  // ---- 1. 先录满回放窗口（这样导出的窗口里全是真实画面） ----
  let s = await waitLive(needSegsBefore, 60000)
  if (!s || s.state !== 'live') {
    problems.push(`第 ${round} 轮：服务没进入 live（${s?.state}：${s?.detail ?? ''}）`)
    break
  }
  const dir1 = String(s.recordDir)
  console.log(
    `[test] 保存前：segments=${s.recordSegments} fps=${s.actualFps} audio=${s.audioActive}` +
      ` bytes=${s.audioBytes} dir=${path.basename(dir1)}`,
  )

  // ---- 2. 点保存 = 重写配置 + reload ----
  writeProfile()
  send({ cmd: 'reload', id: 'p-desk' })
  const tReload = Date.now()
  await sleep(1500)
  const afterReload = desk()
  console.log(
    `[test] reload 后：state=${afterReload?.state} dir=${path.basename(String(afterReload?.recordDir ?? ''))}` +
      ` audio=${afterReload?.audioActive} detail=${afterReload?.audioDetail ?? ''}` +
      ` err=${afterReload?.audioError ?? ''}`,
  )
  if (String(afterReload?.recordDir ?? '') && String(afterReload.recordDir) !== dir1) {
    problems.push(`第 ${round} 轮：保存后录像目录变了（${dir1} -> ${afterReload.recordDir}），旧画面会丢`)
  }

  // ---- 3. 保存后再录一段（用户点保存后就是接着录） ----
  //
  // 不能等"分片数达到 N"：状态里的 recordSegments 是**目录里现存的片数**
  // （滚动删除，窗口 20s / 片 2s 时就稳定在 10 上下），不会一直增长。
  // 按时间等：reload 之后再录 needSegsAfter*分片 秒。
  await sleep(needSegsAfter * SEGMENT_SEC * 1000 + 2000)
  s = desk()
  console.log(
    `[test] 保存后 ${((Date.now() - tReload) / 1000).toFixed(1)}s：segments=${s?.recordSegments}` +
      ` audio=${s?.audioActive} bytes=${s?.audioBytes} detail=${s?.audioDetail ?? ''}` +
      ` err=${s?.audioError ?? ''}`,
  )
  if (WITH_TONE && s && !s.audioActive) {
    problems.push(`第 ${round} 轮：保存后音频采集没起来（${s.audioDetail ?? ''}）`)
  }

  send({ cmd: 'flush', id: 'p-desk' })
  await sleep(800)

  // 静音机器上采集端不产 PCM：铺一条全覆盖的音轨，保证"导出有没有把声音编进去"
  // 这件事本身是可测的（否则测不出来到底是导出丢了还是压根没录到）。
  const recordDir = String(desk()?.recordDir ?? '')
  if (recordDir) {
    const frames = writeTone(path.join(recordDir, 'audio'), Date.now() - (WINDOW_SEC + 3) * 1000, WINDOW_SEC + 4)
    console.log(`[test] 已铺测试音：${((frames / 48000) | 0)}s 覆盖整个窗口`)
  }

  // ---- 4. 导出 ----
  const job = desk()?.exportJobId ?? 0
  send({ cmd: 'export', id: 'p-desk' })
  let result = null
  const tExp = Date.now()
  while (Date.now() - tExp < 120000) {
    await sleep(150)
    const st = desk()
    if (!st) continue
    // 导出失败时不会产生 mp4，所以不能把"文件数变多"当必要条件。
    if ((st.exportJobId ?? 0) > job && !st.exportRunning) {
      result = st
      break
    }
  }
  if (!result) {
    const st = desk()
    problems.push(
      `第 ${round} 轮：导出超时（jobId ${job} -> ${st?.exportJobId} running=${st?.exportRunning}` +
        ` ok=${st?.exportOk} detail=${st?.exportDetail ?? ''}）`,
    )
    continue
  }
  const outPath = String(result.exportOutPath ?? '')
  if (!result.exportOk) problems.push(`第 ${round} 轮：导出失败（${result.exportDetail}）`)
  if (!outPath || !existsSync(outPath)) {
    problems.push(`第 ${round} 轮：找不到产物`)
    continue
  }
  const dur = mp4Duration(outPath)
  const tracks = trackDurations(outPath)
  const payload = audioPayload(outPath)
  const vtr = tracks.find((t) => t.kind === 'vide')
  const atr = tracks.find((t) => t.kind === 'soun')
  console.log(
    `[test] 产物：${path.basename(outPath)} ${dur.toFixed(2)}s（${(statSync(outPath).size / 1048576).toFixed(2)} MB）` +
      ` 视频轨=${vtr ? vtr.sec.toFixed(2) : '-'}s 音频轨=${atr ? atr.sec.toFixed(2) : '-'}s` +
      ` 服务报=${Number(result.exportDurationSeconds ?? 0).toFixed(2)}s`,
  )
  console.log(`[test] 音轨样本 ${payload.samples} 个 / ${(payload.bytes / 1024).toFixed(1)} KB`)
  if (!atr) problems.push(`第 ${round} 轮：产物没有音轨`)
  if (payload.samples <= 0) problems.push(`第 ${round} 轮：音轨是空的`)
  const over = dur - WINDOW_SEC
  if (over > 1.0) problems.push(`第 ${round} 轮：成片 ${dur.toFixed(2)}s，超出窗口 ${over.toFixed(2)}s`)
  if (dur < WINDOW_SEC - 1.0) problems.push(`第 ${round} 轮：成片只有 ${dur.toFixed(2)}s，短了`)
}

child.stdin.end()
child.kill()
if (tone) tone.kill()

for (const l of logs) {
  if (l.includes('audio:') || l.includes('export_plan') || l.includes('reload') || l.includes('export')) {
    console.log('[log] ' + l.trim().slice(0, 2000))
  }
}
if (!logs.some((l) => l.includes('export_plan'))) {
  console.log('[log] （服务没打出 export_plan，原始输出最后 15 行）')
  for (const l of logs.slice(-15)) console.log('[raw] ' + l.trim().slice(0, 500))
}

if (problems.length) {
  console.log('\n[test] 失败：')
  for (const item of problems) console.log('  - ' + item)
  process.exit(1)
}
console.log('\n[test] 通过：保存后导出时长对得上窗口，音轨有数据')
process.exit(0)
