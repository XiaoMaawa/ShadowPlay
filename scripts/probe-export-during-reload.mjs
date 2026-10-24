// 复现「导出进行中改回放时长 → 成片没声音」的探针。
//
// 用户精确描述（2026-10-24）：
//   「显示器捕获，设定回放时长为120秒，点击保存视频（=开始导出），然后因为
//     导出不是秒导出的，我点击配置，修改回放时长为10秒，点击保存配置，后面
//     什么都不用操作，导出的视频是没有声音的」
//
// 关键差异（与 probe-replay-change-audio.mjs 不同）：改动发生在**导出进行中**，
// 不是导出前。reload 会摘出正在导出的会话，等 3 秒（等不到就放回、不应用）。
//
// 这里用「录制时长 < 回放窗口」强制走重编码（补 NO SIGNAL），让导出足够慢，
// 从而能在导出进行中改配置。回放窗口给 30 秒、只录 15 秒，导出约需数秒。
//
// 用法：node scripts/probe-export-during-reload.mjs
// 退出码 0 = 有声音；非 0 = 复现了 bug。

import { spawn } from 'node:child_process'
import { existsSync, mkdirSync, readFileSync, readdirSync, rmSync, writeFileSync, statSync } from 'node:fs'
import path from 'node:path'
import { projectRoot, tempRoot } from './electron-path.mjs'

const backend = path.join(projectRoot, 'native', 'build', 'shadow-capture.exe')
const PROOF = path.join(tempRoot, 'proofs', 'export-during-reload')
const RECORD_ROOT = path.join(PROOF, 'records')
const EXPORT_DIR = path.join(PROOF, 'exports')
const PROFILES = path.join(PROOF, 'profiles.json')
const STATUS = path.join(tempRoot, 'status', 'status-v1.json')

const REPLAY_SEC = 30   // 回放窗口
const RECORD_SEC = 15   // 只录 15 秒 < 30 秒 → 导出要补 NO SIGNAL → 重编码 → 慢
const NEW_REPLAY = 5    // 导出中改成 5 秒

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
  writeFileSync(PROFILES, JSON.stringify({
    version: 1, recordRoot: RECORD_ROOT, exportDir: EXPORT_DIR,
    profiles: [{
      id: 'p-desk', num: 1, name: 'replayaud', enabled: true,
      source: { kind: 'monitor', monitorIndex: 0, clientAreaOnly: false, wgc: true },
      video: { fps: 30, bitrateMbps: 6, replaySeconds, segmentSeconds: 2, scaleWidth: 0, scaleHeight: 0 },
      createdAt: '2026-01-01T00:00:00.000Z',
    }],
  }, null, 2))

writeProfile(REPLAY_SEC)

const readStatus = () => { try { return JSON.parse(readFileSync(STATUS, 'utf8')) } catch { return null } }
const desk = () => readStatus()?.items?.find((s) => s.id === 'p-desk') ?? null

// ---- 播放测试音（loopback 需要真实出声才有非静音包）------------------------
const RATE = 48000
function writeToneWav(file, seconds = 240, freq = 440, amp = 0.25) {
  const frames = seconds * RATE
  const dataBytes = frames * 4
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
console.log(`[probe] 服务 live，声音 active，录 ${RECORD_SEC}s（回放 ${REPLAY_SEC}s）`)
await sleep(RECORD_SEC * 1000)

// ---- flush 尾片（导出前必要）---------------------------------------------
send({ cmd: 'flush', id: 'p-desk' })
await sleep(600)

// ---- 开始导出（会补 NO SIGNAL → 重编码 → 慢）----------------------------
const beforeFiles = (() => { try { return readdirSync(EXPORT_DIR).filter((f) => f.endsWith('.mp4')).length } catch { return 0 } })()
const beforeJob = desk()?.exportJobId ?? 0
console.log('[probe] 开始导出（重编码，会较慢）...')
send({ cmd: 'export', id: 'p-desk' })

// ---- 等导出真的开跑 -------------------------------------------------------
let runningSeen = false
{
  const t = Date.now()
  while (Date.now() - t < 8000) {
    await sleep(50)
    const s = desk()
    if (s && s.exportRunning) { runningSeen = true; break }
  }
}
console.log(`[probe] 导出开跑：${runningSeen}`)

// ---- 导出进行中，改回放时长 30 -> 5，reload ------------------------------
// 关键：在导出还在跑的时候发 reload，模拟用户的「点配置改回放时长点保存」
writeProfile(NEW_REPLAY)
console.log(`[probe] 导出进行中，改回放时长 ${REPLAY_SEC} -> ${NEW_REPLAY}，发 reload`)
send({ cmd: 'reload', id: 'p-desk' })

// ---- 等导出结束 -----------------------------------------------------------
let result = null
const tExp = Date.now()
while (Date.now() - tExp < 90000) {
  await sleep(150)
  const s = desk()
  if (!s) continue
  const files = (() => { try { return readdirSync(EXPORT_DIR).filter((f) => f.endsWith('.mp4')).length } catch { return 0 } })()
  if (files > beforeFiles && (s.exportJobId ?? 0) > beforeJob && !s.exportRunning) { result = s; break }
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

console.log('\n[probe] ==== 结果 ====')
const st = result
if (!st) {
  console.log('[probe] 导出没完成（超时）')
  for (const l of logs) if (l.includes('audio:') || l.includes('export_plan') || l.includes('session_reloaded')) console.log('[log] ' + l)
  process.exit(2)
}
const outPath = String(st.exportOutPath ?? '')
console.log(`[probe] 导出 ok=${st.exportOk} 时长=${Number(st.exportDurationSeconds ?? 0).toFixed(2)}s detail=${st.exportDetail}`)
console.log(`[probe] 产物：${outPath}`)
for (const l of logs) if (l.includes('audio:') || l.includes('export_plan') || l.includes('session_reloaded')) console.log('[log] ' + l)

if (outPath && existsSync(outPath)) {
  const p = audioPayload(outPath)
  console.log(`[probe] 音频：soun=${p.soun} 样本=${p.samples} 个 ${(p.bytes / 1024).toFixed(1)} KB`)
  console.log(`[probe] 文件大小 ${(statSync(outPath).size / 1048576).toFixed(2)} MB`)
  if (p.samples > 0 && p.bytes >= 8000) { console.log('\n[probe] 通过：成片带音频'); process.exit(0) }
  console.log('\n[probe] 复现 bug：成片没有音频（样本 ' + p.samples + ' / ' + p.bytes + ' 字节）')
  process.exit(1)
}
process.exit(2)
