// scripts/probe-segment-audio.mjs —— 验证「音频编码进切片」（需求 1）的录制端产出。
//
// 起采集服务录几秒，然后直接检查每个分片 mp4 里有没有 AAC 音轨（mp4a/soun）。
// 这是验证录制端常驻 AAC 编码器 + 分片音轨直拷是否真的工作的唯一直接证据。
//
// 【依赖声卡出声】分片的音轨来自 WASAPI loopback 采集的 PCM 喂进常驻 AAC 编码器。
// 这台机器如果整机静音，loopback 不产 PCM，分片就没音轨（这是正确行为，不是 bug）。
// 所以脚本会同时报告「服务有没有报告音频活跃」，帮助区分「机器没声音」和「链路断了」。
//
// 用法：node scripts/probe-segment-audio.mjs [观察秒数，默认 8]
// 退出码 0 = 分片带 AAC 音轨（链路通）；2 = 环境问题（没声音/没分片）；1 = 链路断。

import { spawn } from 'node:child_process'
import { existsSync, mkdirSync, readdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs'
import path from 'node:path'
import { projectRoot, tempRoot } from './electron-path.mjs'

const SERVICE = path.join(projectRoot, 'native', 'build', 'shadow-capture.exe')
const PROOF = path.join(tempRoot, 'proofs', 'seg-audio')
const RECORD_ROOT = path.join(PROOF, 'records')
const PROFILES = path.join(PROOF, 'profiles.json')
const STATUS = path.join(tempRoot, 'status', 'status-v1.json')

const WATCH_S = Number(process.argv[2] ?? 8)

if (!existsSync(SERVICE)) {
  console.error('[probe] 找不到采集服务，先 npm run build:native')
  process.exit(1)
}

rmSync(PROOF, { recursive: true, force: true })
mkdirSync(RECORD_ROOT, { recursive: true })
rmSync(STATUS, { force: true })

writeFileSync(
  PROFILES,
  JSON.stringify(
    {
      version: 1,
      recordRoot: RECORD_ROOT,
      exportDir: path.join(PROOF, 'exports'),
      profiles: [
        {
          id: 'p-desk',
          num: 1,
          name: 'segaudio',
          enabled: true,
          source: { kind: 'monitor', monitorIndex: 0, clientAreaOnly: false, wgc: true },
          video: { fps: 30, bitrateMbps: 6, replaySeconds: 20, segmentSeconds: 1 },
          createdAt: '2026-01-01T00:00:00.000Z',
        },
      ],
    },
    null,
    2,
  ),
)

const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
const readStatus = () => {
  try {
    return JSON.parse(readFileSync(STATUS, 'utf8'))
  } catch {
    return null
  }
}
const desk = () => readStatus()?.items?.find((s) => s.id === 'p-desk') ?? null

function hasAudioTrack(file) {
  const buf = readFileSync(file)
  const latin = buf.toString('latin1')
  return { soun: latin.includes('soun'), mp4a: latin.includes('mp4a') }
}

const child = spawn(SERVICE, ['--profiles', PROFILES], { cwd: projectRoot, stdio: ['pipe', 'ignore', 'pipe'] })
child.stderr.on('data', (d) => process.stderr.write(d))
const send = (cmd) => child.stdin.write(JSON.stringify(cmd) + '\n')

// 录制期间播放系统提示音，让 WASAPI loopback 有 PCM 可采（这台机器可能静音）。
const { spawn: spawn2 } = await import('node:child_process')
const beeper = spawn2('powershell', ['-NoProfile', '-Command', `
  for ($i=0; $i -lt 20; $i++) {
    [System.Media.SystemSounds]::Beep.Play()
    Start-Sleep -Milliseconds 400
  }
`], { windowsHide: true, stdio: 'ignore' })
console.log('[probe] 开始播放系统提示音（约 8 秒）')

console.log(`[probe] 起服务，录 ${WATCH_S}s（显示器捕获）`)
const t0 = Date.now()
while (Date.now() - t0 < 20000) {
  await sleep(200)
  const s = desk()
  if (s && s.state === 'live' && (s.recordSegments ?? 0) >= 2) break
}

// 报告音频状态，帮助区分「机器没声音」和「链路断」。
const st = desk()
console.log(`[probe] 音频通道：${st?.audioDetail ?? '（未知）'} / 活跃=${st?.audioActive ?? '?'}`)
console.log(`[probe] 已录分片数：${st?.recordSegments ?? 0}`)

send({ cmd: 'flush', id: 'p-desk' })
await sleep(800)

const recordDir = String(desk()?.recordDir ?? '')
if (!recordDir) {
  console.error('[probe] 状态里没有 recordDir')
  child.kill()
  process.exit(2)
}

// 找分片文件（轮次目录下）。
let segs = []
const segRoot = path.join(recordDir, 'segments')
if (existsSync(segRoot)) {
  for (const entry of readdirSync(segRoot, { withFileTypes: true })) {
    if (!entry.isDirectory()) continue
    const dir = path.join(segRoot, entry.name)
    for (const f of readdirSync(dir)) {
      if (f.endsWith('.mp4')) segs.push(path.join(dir, f))
    }
  }
}
segs.sort()

child.stdin.end()
child.kill()

if (segs.length === 0) {
  console.error('[probe] 没有分片文件')
  process.exit(2)
}

console.log(`\n[probe] 检查 ${segs.length} 个分片的音轨：`)
let withAudio = 0
for (const f of segs) {
  const t = hasAudioTrack(f)
  if (t.soun && t.mp4a) withAudio++
  console.log(`  ${path.basename(path.dirname(f))}/${path.basename(f)}: soun=${t.soun} mp4a=${t.mp4a}`)
}

console.log(`\n[probe] ${withAudio}/${segs.length} 个分片带 AAC 音轨`)
if (withAudio > 0) {
  console.log('[probe] 通过：音频编码进切片生效')
  process.exit(0)
}
console.log('[probe] 分片都没有音轨。可能原因：')
console.log('        1. 这台机器当前静音，WASAPI loopback 没产出 PCM（正常，不是 bug）；')
console.log('        2. 常驻 AAC 编码器链路断了（需要排查 InitAudioEncoder/PumpAudio）。')
process.exit(1)
