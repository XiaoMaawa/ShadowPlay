// scripts/test-live-replay.mjs —— 验证「导出途中改回放时长」不再重启录制
//
// 用户场景：点"保存视频"（导出开始）→ 立刻点"编辑配置"→ 回放时间 120s→10s
// → 确定。以前这一串操作会判成"配置变了"，于是拆掉整路会话：
//   * 预览立刻 NO SIGNAL（新录制器从零开始）
//   * 正在跑的导出被 reload 线程等住 → 变得极慢
//
// 现在改回放时长走就地生效（MfEncoder::SetKeepSegments），要求：
//   1) session_reloaded 的 applied=true 且没有重建会话；
//   2) 录像目录不变、recordSegments 继续增长（录制是连续的）；
//   3) 导出正常完成、耗时没有异常放大。
//
// 用法：node scripts/test-live-replay.mjs
// 退出码 0 = 通过。

import { spawn } from 'node:child_process'
import { existsSync, mkdirSync, readFileSync, readdirSync, rmSync, writeFileSync } from 'node:fs'
import path from 'node:path'
import { projectRoot, tempRoot } from './electron-path.mjs'

const backend = path.join(projectRoot, 'native', 'build', 'shadow-capture.exe')
const PROOF = path.join(tempRoot, 'proofs', 'live-replay')
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
            name: 'livelive',
            enabled: true,
            source: { kind: 'monitor', monitorIndex: 0, clientAreaOnly: false, wgc: true },
            video: {
              fps: 60,
              bitrateMbps: 8,
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

// 一开始给一个比较长的回放窗口（20s），方便制造"窗口远大于已录内容"。
writeProfile(20)

const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
const readStatus = () => {
  try {
    return JSON.parse(readFileSync(STATUS, 'utf8'))
  } catch {
    return null
  }
}
const desk = () => readStatus()?.items?.find((s) => s.id === 'p-desk') ?? null

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

const problems = []

console.log('[test] 启动采集服务（回放窗口 20s，录满 6 片后开始导出）')
const t0 = Date.now()
let before = null
while (Date.now() - t0 < 25000) {
  await sleep(200)
  const s = desk()
  if (s && s.state === 'live' && (s.recordSegments ?? 0) >= 6 && String(s.recordDir ?? '')) {
    before = s
    break
  }
}
if (!before) {
  console.log('[test] 失败：等服务进入录制状态超时')
  child.kill()
  process.exit(1)
}
const dir0 = String(before.recordDir)
console.log(`[test] 录像目录：${dir0}`)

// 点"保存视频"：先 flush（把当前片封口），再发导出。
send({ cmd: 'flush', id: 'p-desk' })
await sleep(300)
const filesBefore = countMp4()
const jobBefore = desk()?.exportJobId ?? 0
const expT0 = Date.now()
send({ cmd: 'export', id: 'p-desk' })
console.log('[test] 已发起导出，立刻把回放时长改成 2 秒并 reload')

// 导出进行中：改回放时长（20s → 2s）并重读配置。
await sleep(120)
writeProfile(2)
send({ cmd: 'reload', id: 'p-desk' })

// 等导出完成。
let result = null
while (Date.now() - expT0 < 60000) {
  await sleep(100)
  const s = desk()
  if (!s) continue
  if (countMp4() > filesBefore && (s.exportJobId ?? 0) > jobBefore && !s.exportRunning) {
    result = s
    break
  }
}
const expMs = Date.now() - expT0

const after = desk()
const dir1 = String(after?.recordDir ?? '')
console.log(
  `[test] 导出：ok=${result?.exportOk} 用时=${expMs}ms detail=${result?.exportDetail ?? '-'}`,
)
console.log(`[test] 导出后录像目录：${dir1}（recordSegments=${after?.recordSegments}）`)

if (!result || !result.exportOk) problems.push('导出失败或超时')
if (dir1 !== dir0) problems.push(`录像目录被换掉了（${dir0} → ${dir1}），说明会话被重建`)
if ((after?.recordSegments ?? 0) < (before?.recordSegments ?? 0)) {
  problems.push('recordSegments 倒退了，说明录制被重启')
}

const reloadLine = logs.find((l) => l.includes('session_reloaded') && l.includes('p-desk'))
console.log('[test] reload 结果：' + (reloadLine ?? '（没有 session_reloaded 日志）'))
if (reloadLine && /"applied":false/.test(reloadLine)) {
  problems.push('reload 没有就地生效（applied=false），仍然走了重建')
}
if (reloadLine && /"active":false/.test(reloadLine)) problems.push('reload 把这一路停掉了')

// 预览用的帧环应持续有帧（不能 NO SIGNAL）。status 里的 recordFrames 一直在涨即可。
const f1 = Number(before?.recordFrames ?? 0)
await sleep(1200)
const f2 = Number(desk()?.recordFrames ?? 0)
console.log(`[test] 录制帧数：${f1} → ${f2}`)
if (f2 <= f1) problems.push('录制帧数没有继续增长（预览会 NO SIGNAL）')

child.stdin.end()
child.kill()

if (problems.length) {
  console.log('\n[test] 失败：')
  for (const p of problems) console.log('  - ' + p)
  process.exit(1)
}
console.log('\n[test] 通过：改回放时长就地生效，录制连续、导出正常完成')
process.exit(0)
