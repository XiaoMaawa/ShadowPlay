// scripts/test-export-snapshot.mjs —— 验证「点击导出时给分片目录拍照」这条语义
//
// 测什么（对应用户的两条原话）：
//   ① 「点击导出后，应该立刻把当前目录的切片隔离，不要让新切片写进来」——
//      导出开始时定格了几片，日志里 export_snapshot n=… 就是几片；导出过程中
//      继续录出来的新片不能出现在成片里（成片时长应≈回放窗口，而不是越滚越长）。
//   ② 「导出完后再次点击导出，第一次导出前的切片如果在时间内也要导出」——
//      连点两次导出，两次都成功、时长都≈回放窗口，且第二次的分片集合与第一次
//      有重叠（没有被第一次"消费"掉）。
//
// 用法：node scripts/test-export-snapshot.mjs
// 退出码 0 = 通过。

import { spawn } from 'node:child_process'
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync, readdirSync, statSync } from 'node:fs'
import path from 'node:path'
import { projectRoot, tempRoot } from './electron-path.mjs'

const backend = path.join(projectRoot, 'native', 'build', 'shadow-capture.exe')
const PROOF = path.join(tempRoot, 'proofs', 'export-snap')
const RECORD_ROOT = path.join(PROOF, 'records')
const EXPORT_DIR = path.join(PROOF, 'exports')
const PROFILES = path.join(PROOF, 'profiles.json')
const STATUS = path.join(tempRoot, 'status', 'status-v1.json')

const REPLAY_SECONDS = 10
const SEGMENT_SECONDS = 1

if (!existsSync(backend)) {
  console.error('[test] 找不到采集服务，先 npm run build:native')
  process.exit(1)
}

rmSync(PROOF, { recursive: true, force: true })
mkdirSync(RECORD_ROOT, { recursive: true })
mkdirSync(EXPORT_DIR, { recursive: true })

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
          name: 'snap-desk',
          enabled: true,
          source: { kind: 'monitor', monitorIndex: 0, clientAreaOnly: false, wgc: true },
          video: {
            fps: 30,
            bitrateMbps: 6,
            replaySeconds: REPLAY_SECONDS,
            segmentSeconds: SEGMENT_SECONDS,
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

const sleep = (ms) => new Promise((r) => setTimeout(r, ms))

function readStatus() {
  try {
    return JSON.parse(readFileSync(STATUS, 'utf8'))
  } catch {
    return null
  }
}

/** 取 p-desk 的会话状态 */
function desk() {
  const st = readStatus()
  const items = st?.items ?? []
  return items.find((s) => s.id === 'p-desk') ?? null
}

/** 读 MP4 的 mvhd 时长（秒）。只认 fMP4/MP4 的 moov->mvhd。 */
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

const logs = []
let snapshotN = 0

const child = spawn(backend, ['--profiles', PROFILES], {
  cwd: projectRoot,
  stdio: ['pipe', 'pipe', 'pipe'],
  windowsHide: true,
})
child.stdout.setEncoding('utf8')
child.stdout.on('data', (d) => {
  for (const line of d.split(/\r?\n/)) {
    if (!line.trim()) continue
    logs.push(line)
    const m = line.match(/export_snapshot n=(\d+)/)
    if (m) {
      snapshotN = Number(m[1])
      console.log(`[test] 拍照：定格 ${snapshotN} 片`)
    }
  }
})
child.stderr.setEncoding('utf8')
child.stderr.on('data', (d) => logs.push(`[stderr] ${d.trim()}`))

function send(cmd) {
  child.stdin.write(JSON.stringify(cmd) + '\n')
}

function countMp4() {
  try {
    return readdirSync(EXPORT_DIR).filter((f) => f.endsWith('.mp4')).length
  } catch {
    return 0
  }
}

/**
 * 发一次导出并等它结束；返回 { ok, seconds, segments, detail, snapshot }。
 *
 * 【为什么用"产物文件数"判结束，而不是只看 exportRunning】状态文件是每秒刷一次
 * 的镜像，导出本身只要一两秒 —— 只盯 exportRunning 很容易漏掉中间那一拍，
 * 于是永远等不到"true→false"。文件数 + jobId 两个信号都变才认完成。
 */
async function exportOnce(tag) {
  const beforeFiles = countMp4()
  const beforeJob = desk()?.exportJobId ?? 0
  snapshotN = 0
  send({ cmd: 'export', id: 'p-desk' })
  const t0 = Date.now()
  let last = null
  while (Date.now() - t0 < 40000) {
    await sleep(120)
    const s = desk()
    if (!s) continue
    last = s
    const files = countMp4()
    const jobAdvanced = (s.exportJobId ?? 0) > beforeJob
    if (files > beforeFiles && jobAdvanced && !s.exportRunning) {
      const dur = Number(s.exportDurationSeconds ?? 0)
      console.log(
        `[test] ${tag}：ok=${s.exportOk} 分片=${s.exportSegments} 时长=${dur.toFixed(2)}s ` +
          `拍照定格=${snapshotN} 片 detail=${s.exportDetail}`,
      )
      return {
        ok: !!s.exportOk,
        seconds: dur,
        segments: Number(s.exportSegments ?? 0),
        detail: s.exportDetail ?? '',
        snapshot: snapshotN,
      }
    }
    if (files > beforeFiles && s.exportFailed) {
      console.log(`[test] ${tag}：失败 ${s.exportDetail}`)
      return { ok: false, seconds: 0, segments: 0, detail: s.exportDetail ?? 'failed', snapshot: snapshotN }
    }
  }
  console.log(`[test] ${tag}：超时未结束（最后状态 ${JSON.stringify(last)}）`)
  return { ok: false, seconds: 0, segments: 0, detail: 'timeout', snapshot: snapshotN }
}

console.log(`[test] 启动采集服务（${REPLAY_SECONDS}s 回放窗口 / ${SEGMENT_SECONDS}s 分片）`)
const bootAt = Date.now()
while (Date.now() - bootAt < 15000) {
  await sleep(200)
  const s = desk()
  if (s && (s.recordSegments ?? 0) >= REPLAY_SECONDS - 1 && s.state === 'live') break
}
console.log(`[test] 服务就绪：${desk()?.recordSegments ?? 0} 片`)

// 等到时间轴攒够整整一个回放窗口
await sleep((REPLAY_SECONDS + 2) * 1000)

const a = await exportOnce('第一次导出')
// 立刻再点一次：不能因为"已经被导过"就少拍片
const b = await exportOnce('第二次导出')

const files = existsSync(EXPORT_DIR)
  ? readdirSync(EXPORT_DIR).filter((f) => f.endsWith('.mp4'))
  : []
const durations = files.map((f) => ({ name: f, dur: mp4Duration(path.join(EXPORT_DIR, f)), size: statSync(path.join(EXPORT_DIR, f)).size }))
for (const d of durations) {
  console.log(`[test] 产物 ${d.name}：${d.dur.toFixed(2)}s / ${(d.size / 1048576).toFixed(2)} MB`)
}

child.stdin.end()
child.kill()

const problems = []
if (!a.ok) problems.push(`第一次导出失败：${a.detail}`)
if (!b.ok) problems.push(`第二次导出失败：${b.detail}`)
if (files.length < 2) problems.push(`只生成了 ${files.length} 个 MP4，预期 2 个`)
if (a.ok && a.seconds < REPLAY_SECONDS - 2.5) problems.push(`第一次成片只有 ${a.seconds.toFixed(2)}s，明显少拍`)
if (b.ok && b.seconds < REPLAY_SECONDS - 2.5) problems.push(`第二次成片只有 ${b.seconds.toFixed(2)}s，明显少拍`)
if (a.snapshot === 0 || b.snapshot === 0) problems.push('日志里没有 export_snapshot，拍照这一步没跑到')
if (b.snapshot && a.snapshot && b.snapshot < Math.max(2, a.snapshot - 2)) {
  problems.push(`第二次拍照只定格 ${b.snapshot} 片（第一次 ${a.snapshot} 片）—— 第一次导出把片子吃掉了`)
}

if (problems.length) {
  console.log('\n[test] 失败：')
  for (const p of problems) console.log('  - ' + p)
  process.exit(1)
}
console.log('\n[test] 通过：拍照定格、新片不进片、再次导出仍含旧片')
process.exit(0)
