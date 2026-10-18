// 验证「导出比录制短两秒」：直接调 addon 的 exportStart，比对成品时长与分片总时长。
//
// 用法：node scripts/probe-export-length.mjs <分片目录> [目标帧率] [分片时长]
//
// 前提：服务在录，分片目录里已经积累了若干个分片。
import fs from 'node:fs'
import path from 'node:path'
import { execFileSync } from 'node:child_process'
import { createRequire } from 'node:module'

const require = createRequire(import.meta.url)
const segmentsDir = process.argv[2]
const targetFps = Number(process.argv[3] ?? 0)
const segmentSeconds = Number(process.argv[4] ?? 2)

if (!segmentsDir) {
  console.error('用法：node scripts/probe-export-length.mjs <分片目录> [目标帧率] [分片时长]')
  process.exit(2)
}

const exportDir = path.resolve('export')
fs.mkdirSync(exportDir, { recursive: true })

const addon = require('../native/build/shadow_ring.nodejs.node')
const res = addon.exportStart(segmentsDir, exportDir, '', 0, targetFps, segmentSeconds)
console.log('exportStart →', JSON.stringify(res))
if (!res?.ok) process.exit(1)

// 等导出结束
const t0 = Date.now()
let st = null
for (;;) {
  st = addon.exportStatus()
  if (!st?.running) break
  if (Date.now() - t0 > 120000) {
    console.log('超时')
    process.exit(1)
  }
  await new Promise((r) => setTimeout(r, 200))
}
console.log('导出完成 →', JSON.stringify({ ok: st.ok, detail: st.detail, durationSeconds: st.durationSeconds, segmentCount: st.segmentCount, outPath: st.outPath }))

if (!st.ok || !st.outPath) process.exit(1)

// 用 ffprobe 看成品真实时长与帧数
const probe = execFileSync(
  'ffprobe',
  ['-v', 'error', '-select_streams', 'v:0',
   '-show_entries', 'stream=r_frame_rate,nb_frames,duration', '-of', 'default=nw=1', st.outPath],
  { encoding: 'utf8' },
)
console.log('--- 成品 ---')
console.log(probe.trim())
