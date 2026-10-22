// scripts/test-export-mixed.mjs —— 验证「录制中途改帧率/缩放」后的导出
//
// 测什么（对应用户的原话）：
//   「多分辨率下，比如窗口进行了缩放，或者帧率出现了切换，导致一堆切片的
//    参数不一致」——这不是 bug，是设计内场景（DESIGN.md 9.9.2）：导出要
//    把参数不一致的分片重编码到同一个画布、按真实时间戳重采样成恒定帧率。
//
// 构造：前 7 秒按 30fps 原尺寸录，然后把配置改成 60fps + 缩放到 960x540
// 并 reload（和界面保存配置走的是同一条路），再录 7 秒。这样分片目录里
// 就有 30fps 全分辨率 + 60fps 小分辨率两种片子。导出必须：
//   ① 成功（不能因为参数不一致就报错）；
//   ② 时长 ≈ 14 秒（两段都在，没有互相挤掉）；
//   ③ detail 里带「按窗口内最大尺寸」—— 证明走了重编码 + 混合参数路径。
//
// 用法：node scripts/test-export-mixed.mjs
// 退出码 0 = 通过。

import { spawn } from 'node:child_process'
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync, readdirSync, statSync } from 'node:fs'
import path from 'node:path'
import { projectRoot, tempRoot } from './electron-path.mjs'

const backend = path.join(projectRoot, 'native', 'build', 'shadow-capture.exe')
const PROOF = path.join(tempRoot, 'proofs', 'export-mixed')
const RECORD_ROOT = path.join(PROOF, 'records')
const EXPORT_DIR = path.join(PROOF, 'exports')
const PROFILES = path.join(PROOF, 'profiles.json')
const STATUS = path.join(tempRoot, 'status', 'status-v1.json')

if (!existsSync(backend)) {
  console.error('[test] 找不到采集服务，先 npm run build:native')
  process.exit(1)
}

rmSync(PROOF, { recursive: true, force: true })
mkdirSync(RECORD_ROOT, { recursive: true })
mkdirSync(EXPORT_DIR, { recursive: true })

const baseProfile = (fps, sw, sh) => ({
  id: 'p-desk',
  num: 1,
  name: 'mixed',
  enabled: true,
  source: { kind: 'monitor', monitorIndex: 0, clientAreaOnly: false, wgc: true },
  video: {
    fps,
    bitrateMbps: 6,
    replaySeconds: 20,
    segmentSeconds: 1,
    scaleWidth: sw,
    scaleHeight: sh,
  },
  createdAt: '2026-01-01T00:00:00.000Z',
})

const writeProfiles = (p) =>
  writeFileSync(
    PROFILES,
    JSON.stringify(
      { version: 1, recordRoot: RECORD_ROOT, exportDir: EXPORT_DIR, profiles: [p] },
      null,
      2,
    ),
  )

// 第一段：30fps、不缩放
writeProfiles(baseProfile(30, 0, 0))

const sleep = (ms) => new Promise((r) => setTimeout(r, ms))

function readStatus() {
  try {
    return JSON.parse(readFileSync(STATUS, 'utf8'))
  } catch {
    return null
  }
}
function desk() {
  return readStatus()?.items?.find((s) => s.id === 'p-desk') ?? null
}

/** 读 MP4 的 mvhd 时长（秒）。 */
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

async function waitFor(pred, timeoutMs, what) {
  const t0 = Date.now()
  while (Date.now() - t0 < timeoutMs) {
    await sleep(200)
    const v = pred()
    if (v) return v
  }
  throw new Error(`等待超时：${what}`)
}

console.log('[test] 启动采集服务（第一段 30fps 原尺寸）')
const bootAt = Date.now()
while (Date.now() - bootAt < 15000) {
  await sleep(200)
  const s = desk()
  if (s && s.state === 'live' && (s.recordSegments ?? 0) >= 6) break
}
console.log(`[test] 第一段录完：${desk()?.recordSegments ?? 0} 片`)

// 收尾当前片，保证 30fps 段的片子完整落盘
send({ cmd: 'flush', id: 'p-desk' })
await sleep(500)

// 第二段：60fps + 缩放到 960x540，走和界面保存配置同一条 reload 路
console.log('[test] 切换配置 → 60fps + 960x540，reload')
writeProfiles(baseProfile(60, 960, 540))
send({ cmd: 'reload', id: 'p-desk' })

const boot2 = Date.now()
while (Date.now() - boot2 < 15000) {
  await sleep(200)
  const s = desk()
  if (s && s.state === 'live' && (s.recordSegments ?? 0) >= 6) break
}
console.log(`[test] 第二段录完：${desk()?.recordSegments ?? 0} 片`)
send({ cmd: 'flush', id: 'p-desk' })
await sleep(500)

// 导出（20 秒窗口覆盖全部两段）
const beforeFiles = countMp4()
const beforeJob = desk()?.exportJobId ?? 0
const t0 = Date.now()
send({ cmd: 'export', id: 'p-desk' })
let result = null
while (Date.now() - t0 < 60000) {
  await sleep(150)
  const s = desk()
  if (!s) continue
  const files = countMp4()
  if (files > beforeFiles && (s.exportJobId ?? 0) > beforeJob && !s.exportRunning) {
    result = s
    break
  }
  if (files > beforeFiles && s.exportFailed) {
    result = s
    break
  }
}
if (!result) throw new Error('导出超时未结束')
console.log(`[test] 导出耗时：${((Date.now() - t0) / 1000).toFixed(2)}s`)
const detail = String(result.exportDetail ?? '')
console.log(
  `[test] 导出：ok=${result.exportOk} 分片=${result.exportSegments} ` +
    `时长=${Number(result.exportDurationSeconds ?? 0).toFixed(2)}s detail=${detail}`,
)

const files = readdirSync(EXPORT_DIR).filter((f) => f.endsWith('.mp4'))
const durations = files.map((f) => ({
  name: f,
  dur: mp4Duration(path.join(EXPORT_DIR, f)),
  size: statSync(path.join(EXPORT_DIR, f)).size,
}))
for (const d of durations) {
  console.log(`[test] 产物 ${d.name}：${d.dur.toFixed(2)}s / ${(d.size / 1048576).toFixed(2)} MB`)
}

child.stdin.end()
child.kill()

for (const l of logs) {
  if (l.includes('export_plan')) console.log('[plan] ' + l.trim())
}

const problems = []
if (!result.exportOk) problems.push(`导出失败：${detail}`)
const dur = durations[0]?.dur ?? 0
if (dur < 12) problems.push(`成片只有 ${dur.toFixed(2)}s，两段没有都被收进来（预期 ≈14s）`)
if (dur > 22) problems.push(`成片 ${dur.toFixed(2)}s 超出窗口太多（预期 ≈14s）`)
if (!detail.includes('最大尺寸') && !detail.includes('NO SIGNAL')) {
  problems.push(`detail 里没有"按窗口内最大尺寸"字样，可能没走混合参数重编码路径：${detail}`)
}

if (problems.length) {
  console.log('\n[test] 失败：')
  for (const p of problems) console.log('  - ' + p)
  console.log('\n[test] 服务日志尾部：')
  for (const l of logs.slice(-25)) console.log('  ' + l)
  process.exit(1)
}
console.log('\n[test] 通过：混合帧率/分辨率分片正常重编码，时长与两段都对得上')
process.exit(0)
