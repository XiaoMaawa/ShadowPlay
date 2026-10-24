// 复现「改回放时长后导出没声音」的探针。
//
// 流程：起服务（回放 120 秒）→ 播放测试音 → 录若干秒 → 改回放时长为 10 秒
// → reload（触发拆掉重建）→ 再录几秒 → 导出 → 检查成片有没有 AAC 音轨和
// 音频样本。全程依赖真实 WASAPI loopback 采到的音频，不合成 PCM。
//
// 用法：node scripts/probe-replay-change-audio.mjs [--before=8] [--after=4]
// 退出码 0 = 有声音；非 0 = 复现了 bug。

import { spawn } from 'node:child_process'
import { existsSync, mkdirSync, readFileSync, readdirSync, rmSync, writeFileSync, statSync } from 'node:fs'
import path from 'node:path'
import { tmpdir } from 'node:os'
import { projectRoot, tempRoot } from './electron-path.mjs'

const backend = path.join(projectRoot, 'native', 'build', 'shadow-capture.exe')
const PROOF = path.join(tempRoot, 'proofs', 'replay-change-audio')
const RECORD_ROOT = path.join(PROOF, 'records')
const EXPORT_DIR = path.join(PROOF, 'exports')
const PROFILES = path.join(PROOF, 'profiles.json')
const STATUS = path.join(tempRoot, 'status', 'status-v1.json')

const arg = (name, dflt) => {
  const hit = process.argv.find((a) => a.startsWith(`--${name}=`))
  return hit ? Number(hit.split('=')[1]) : dflt
}
const BEFORE_SEC = arg('before', 8)   // 改配置前录多久
const AFTER_SEC = arg('after', 4)     // 改配置后录多久

const sleep = (ms) => new Promise((r) => setTimeout(r, ms))

if (!existsSync(backend)) {
  console.error('[probe] 找不到 shadow-capture.exe，先 npm run build:native')
  process.exit(1)
}

rmSync(PROOF, { recursive: true, force: true })
mkdirSync(RECORD_ROOT, { recursive: true })
mkdirSync(EXPORT_DIR, { recursive: true })
rmSync(STATUS, { force: true })

const writeProfile = (replaySeconds) =>
  writeFileSync(
    PROFILES,
    JSON.stringify({
      version: 1,
      recordRoot: RECORD_ROOT,
      exportDir: EXPORT_DIR,
      profiles: [{
        id: 'p-desk', num: 1, name: 'replayaud', enabled: true,
        source: { kind: 'monitor', monitorIndex: 0, clientAreaOnly: false, wgc: true },
        video: { fps: 30, bitrateMbps: 6, replaySeconds, segmentSeconds: 1, scaleWidth: 0, scaleHeight: 0 },
        createdAt: '2026-01-01T00:00:00.000Z',
      }],
    }, null, 2),
  )

writeProfile(120)

const readStatus = () => { try { return JSON.parse(readFileSync(STATUS, 'utf8')) } catch { return null } }
const desk = () => readStatus()?.items?.find((s) => s.id === 'p-desk') ?? null

// ---- 播放测试音：让 loopback 真的产出非静音包 ------------------------------
const RATE = 48000
function writeToneWav(file, seconds = 240, freq = 440, amp = 0.25) {
  const frames = seconds * RATE
  const dataBytes = frames * 2 * 2
  const buf = Buffer.alloc(44 + dataBytes)
  buf.write('RIFF', 0, 'latin1'); buf.writeUInt32LE(36 + dataBytes, 4); buf.write('WAVE', 8, 'latin1')
  buf.write('fmt ', 12, 'latin1'); buf.writeUInt32LE(16, 16); buf.writeUInt16LE(1, 20)
  buf.writeUInt16LE(2, 22); buf.writeUInt32LE(RATE, 24); buf.writeUInt32LE(RATE * 4, 28)
  buf.writeUInt16LE(4, 32); buf.writeUInt16LE(16, 34); buf.write('data', 36, 'latin1')
  buf.writeUInt32LE(dataBytes, 40)
  let off = 44
  for (let i = 0; i < frames; i++) {
    const v = Math.round(Math.sin((2 * Math.PI * freq * i) / RATE) * amp * 32767)
    buf.writeInt16LE(v, off); buf.writeInt16LE(v, off + 2); off += 4
  }
  writeFileSync(file, buf)
  return file
}
const tone = path.join(PROOF, 'tone.wav')
writeToneWav(tone)
const player = spawn('ffplay', ['-hide_banner', '-loglevel', 'quiet', '-nodisp', '-autoexit', '-loop', '0', tone], { stdio: 'ignore', windowsHide: true })
await sleep(2500)
console.log('[probe] 已开始循环播放 440Hz 测试音')

const child = spawn(backend, ['--profiles', PROFILES], { cwd: projectRoot, stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true })
const logs = []
child.stdout.setEncoding('utf8')
child.stdout.on('data', (d) => logs.push(...d.split(/\r?\n/).filter((l) => l.trim())))
child.stderr.setEncoding('utf8')
child.stderr.on('data', (d) => logs.push(`[stderr] ${d.trim()}`))
const send = (cmd) => child.stdin.write(JSON.stringify(cmd) + '\n')

// ---- 等录制起来 -----------------------------------------------------------
const t0 = Date.now()
let liveAt = 0
while (Date.now() - t0 < 20000) {
  await sleep(200)
  const s = desk()
  if (s && s.state === 'live' && s.audioActive) { liveAt = Date.now(); break }
}
if (!liveAt) { console.log('[probe] 服务没起来 / 没声音'); child.kill(); process.exit(2) }
console.log(`[probe] 服务 live，声音 active，录 ${BEFORE_SEC}s（回放 120s）`)
await sleep(BEFORE_SEC * 1000)

// ---- 改回放时长 120 -> 10，reload ------------------------------------------
writeProfile(10)
console.log('[probe] 改回放时长 120 -> 10，发 reload')
send({ cmd: 'reload', id: 'p-desk' })
// 等重建完成 + 再录 AFTER_SEC
const t1 = Date.now()
while (Date.now() - t1 < 15000) {
  await sleep(200)
  const s = desk()
  if (s && s.state === 'live' && s.audioActive && (s.recordSegments ?? 0) >= 1) break
}
console.log(`[probe] 重建后再录 ${AFTER_SEC}s（回放 10s）`)
await sleep(AFTER_SEC * 1000)

// ---- flush + 导出 ----------------------------------------------------------
send({ cmd: 'flush', id: 'p-desk' })
await sleep(600)
const beforeFiles = (() => { try { return readdirSync(EXPORT_DIR).filter((f) => f.endsWith('.mp4')).length } catch { return 0 } })()
const beforeJob = desk()?.exportJobId ?? 0
send({ cmd: 'export', id: 'p-desk' })
let result = null
const tExp = Date.now()
while (Date.now() - tExp < 60000) {
  await sleep(150)
  const s = desk()
  if (!s) continue
  if (((() => { try { return readdirSync(EXPORT_DIR).filter((f) => f.endsWith('.mp4')).length } catch { return 0 } })() > beforeFiles) &&
      (s.exportJobId ?? 0) > beforeJob && !s.exportRunning) { result = s; break }
}

// ---- 检查成片音频 ----------------------------------------------------------
function audioPayload(file) {
  const buf = readFileSync(file)
  const boxes = (start, end, cb) => {
    for (let i = start; i + 8 <= end;) {
      const size = buf.readUInt32BE(i); const type = buf.toString('ascii', i + 4, i + 8)
      if (size < 8) break; cb(type, i + 8, Math.min(i + size, end)); i += size
    }
  }
  const findChild = (s, e, w) => { let h = null; boxes(s, e, (t, a, b) => { if (t === w && !h) h = [a, b] }); return h }
  const children = (s, e, w) => { const o = []; boxes(s, e, (t, a, b) => { if (t === w) o.push([a, b]) }); return o }
  let moov = null; boxes(0, buf.length, (t, a, b) => { if (t === 'moov' && !moov) moov = [a, b] })
  if (!moov) return { bytes: 0, samples: 0, soun: false }
  const latin = buf.toString('latin1')
  for (const [trakA, trakB] of children(moov[0], moov[1], 'trak')) {
    const mdia = findChild(trakA, trakB, 'mdia'); if (!mdia) continue
    const hdlr = findChild(mdia[0], mdia[1], 'hdlr'); if (!hdlr) continue
    if (buf.toString('ascii', hdlr[0] + 8, hdlr[0] + 12) !== 'soun') continue
    const minf = findChild(mdia[0], mdia[1], 'minf'); if (!minf) continue
    const stbl = findChild(minf[0], minf[1], 'stbl'); if (!stbl) continue
    const stsz = findChild(stbl[0], stbl[1], 'stsz'); if (!stsz) continue
    const sampleSize = buf.readUInt32BE(stsz[0] + 4); const count = buf.readUInt32BE(stsz[0] + 8)
    if (sampleSize > 0) return { bytes: sampleSize * count, samples: count, soun: latin.includes('soun') }
    let total = 0
    for (let k = 0; k < count; k++) { const at = stsz[0] + 12 + k * 4; if (at + 4 > stsz[1]) break; total += buf.readUInt32BE(at) }
    return { bytes: total, samples: count, soun: latin.includes('soun') }
  }
  return { bytes: 0, samples: 0, soun: latin.includes('soun') }
}

child.stdin.end()
child.kill()
try { player.kill() } catch {}

const st = result
console.log('\n[probe] ==== 结果 ====')
if (!st) {
  console.log('[probe] 导出没完成（超时）')
  for (const l of logs) if (l.includes('audio:') || l.includes('export_plan') || l.includes('session_reloaded')) console.log('[log] ' + l)
  process.exit(2)
}
const outPath = String(st.exportOutPath ?? '')
console.log(`[probe] 导出 ok=${st.exportOk} 时长=${Number(st.exportDurationSeconds ?? 0).toFixed(2)}s detail=${st.exportDetail}`)
console.log(`[probe] 产物：${outPath}`)
if (outPath && existsSync(outPath)) {
  const p = audioPayload(outPath)
  console.log(`[probe] 音频：soun=${p.soun} 样本=${p.samples} 个 ${(p.bytes / 1024).toFixed(1)} KB`)
  console.log(`[probe] 文件大小 ${(statSync(outPath).size / 1048576).toFixed(2)} MB`)
}
for (const l of logs) if (l.includes('audio:') || l.includes('export_plan')) console.log('[log] ' + l)

if (outPath && existsSync(outPath)) {
  const p = audioPayload(outPath)
  if (p.samples > 0 && p.bytes >= 8000) { console.log('\n[probe] 通过：成片带音频'); process.exit(0) }
  console.log('\n[probe] 复现 bug：成片没有音频（样本 ' + p.samples + ' / ' + p.bytes + ' 字节）')
  process.exit(1)
}
process.exit(2)
