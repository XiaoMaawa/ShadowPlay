// scripts/check-frame-dups.mjs —— 逐帧哈希，找「连续重复帧」（画面定格）与时间轴异常。
//
// 为什么需要它：用户报「导出的视频每两秒顿一下」，但看统计只能看到
// repeatFrames（补帧数），看不到「画面到底有没有真的定住」。这个脚本把每帧
// 解码后的像素哈希取出来，连续相同的串就是定格段，一眼能看出「每 2 秒定格
// 283ms」这种周期性毛病。
//
// 用法：
//   node scripts/check-frame-dups.mjs <目录或文件> [更多...]
//
// 判读：
//   * 连续重复串里「起始帧」在每片都一样 → 是录制/采集端的周期性问题；
//   * 串的位置随内容乱跳 → 是源画面本身静止（比如播放器暂停、2 秒循环）；
//   * 交叉比对不同分片的同一帧哈希完全相等 → 源内容就是循环的，不是录制丢帧。
import { spawnSync } from 'node:child_process'
import { readdirSync, statSync } from 'node:fs'
import path from 'node:path'

const args = process.argv.slice(2)
const files = []
for (const a of args) {
  if (statSync(a).isDirectory()) {
    for (const n of readdirSync(a).filter((x) => x.endsWith('.mp4')).sort()) {
      files.push(path.join(a, n))
    }
  } else files.push(a)
}

let prev = null
let prevFile = ''
let dupPairs = 0
let total = 0
const runs = []          // 连续重复串：[{file,startIdx,len}]
let curRun = 0
let curStart = 0
const deltas = new Map()

for (const f of files) {
  const r = spawnSync('ffmpeg', ['-v', 'error', '-i', f, '-f', 'framemd5', '-'], {
    encoding: 'utf8', maxBuffer: 256 * 1024 * 1024,
  })
  if (r.status !== 0) {
    console.log(`!! ${f} 读不了：${(r.stderr || '').trim().slice(0, 200)}`)
    continue
  }
  const rows = r.stdout.split(/\r?\n/).filter((l) => l && !l.startsWith('#'))
  let idx = 0
  for (const row of rows) {
    const c = row.split(',').map((s) => s.trim())
    const pts = Number(c[2])
    const hash = c[5]
    total++
    if (prev !== null && hash === prev) {
      dupPairs++
      if (curRun === 0) curStart = idx - 1
      curRun++
    } else if (curRun > 0) {
      runs.push({ file: path.basename(prevFile), start: curStart, len: curRun + 1 })
      curRun = 0
    }
    prev = hash
    prevFile = f
    idx++
    if (c.length >= 2) deltas.set(c[1], (deltas.get(c[1]) ?? 0) + 1)
  }
  if (curRun > 0) {
    runs.push({ file: path.basename(prevFile), start: curStart, len: curRun + 1 })
    curRun = 0
  }
}
if (curRun > 0) runs.push({ file: path.basename(prevFile), start: curStart, len: curRun + 1 })

console.log(`文件 ${files.length} 个，帧 ${total}，连续重复对 ${dupPairs}（${((dupPairs / Math.max(1, total)) * 100).toFixed(2)}%）`)
if (runs.length) {
  console.log('连续重复串：')
  for (const r of runs) console.log(`  ${r.file} 起始帧=${r.start} 长度=${r.len}`)
} else console.log('没有连续重复帧')
