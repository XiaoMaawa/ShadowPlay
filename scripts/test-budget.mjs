// scripts/test-budget.mjs — 打印每一路每一拍的时间花在哪
//
// 用途：回答"为什么 fps 只到这个数"。光看 actualFps 是个结果，要回答"什么
// 导致的"，得把一拍拆成三笔：采集、发布到共享环、喂录制器。这三笔加起来离
// 帧周期（1000/fps）还有多少余量，一眼就能看出来：
//
//   余量为正 → 这一档跑得动
//   余量为负 → 这一拍干不完，帧率必然掉，掉到哪个数就是 1000/每拍总耗时
//
// 用法：node scripts/test-budget.mjs [秒数]
import { readFileSync } from 'node:fs'
import path from 'node:path'
import os from 'node:os'

const STATUS = path.join(os.tmpdir(), 'shadow-play', 'app', 'status', 'status-v1.json')
const seconds = Number(process.argv[2] || 1)

function snap() {
  return JSON.parse(readFileSync(STATUS, 'utf8'))
}

const a = snap()
await new Promise((r) => setTimeout(r, seconds * 1000))
const b = snap()

const byId = (doc) => {
  const m = new Map()
  for (const it of doc.items ?? []) m.set(it.id, it)
  return m
}
const A = byId(a)
const B = byId(b)
const dt = (Number(b.ts) - Number(a.ts)) / 1000

console.log(`采样 ${dt.toFixed(2)} 秒\n`)
const problems = []
for (const [id, nb] of B) {
  const na = A.get(id)
  if (!na) continue
  const tgt = Number(nb.targetFps || 0)
  const act = Number(nb.actualFps || 0)
  const cap = Number(nb.captureMs || 0)
  const pub = Number(nb.publishMs || 0)
  const rec = Number(nb.feedMs || 0)
  const want = tgt > 0 ? 1000 / tgt : 0
  const used = cap + pub + rec
  const frames = Number(nb.recordFrames || 0) - Number(na.recordFrames || 0)
  const segs = Number(nb.recordSegments || 0) - Number(na.recordSegments || 0)

  console.log(`${id}  ${nb.state}  目标 ${tgt}fps  实测 ${act.toFixed(1)}fps`)
  if (tgt <= 0) { console.log('  （这一路没在抓）\n'); continue }
  console.log(`  帧周期 ${want.toFixed(2)}ms，每拍用掉 ${used.toFixed(2)}ms（采集 ${cap.toFixed(2)} + 发布 ${pub.toFixed(2)} + 喂录制 ${rec.toFixed(2)}）`)
  const head = want - used
  if (head < 0) {
    const ceiling = used > 0 ? 1000 / used : 0
    console.log(`  余量 ${head.toFixed(2)}ms ← 超支了：这一拍干不完，帧率被压在 ~${ceiling.toFixed(0)}fps`)
    problems.push({ id, tgt, cap, pub, rec, used, ceiling })
  } else {
    console.log(`  余量 ${head.toFixed(2)}ms ← 够用`)
  }
  if (act > 0 && tgt > 0 && act < tgt * 0.95) {
    console.log(`  实际节奏 ${(1000 / act).toFixed(2)}ms 一拍，比目标慢 ${(1000 / act - want).toFixed(2)}ms`)
  }
  console.log(`  录制：${dt.toFixed(1)} 秒里落盘 ${frames} 帧 / 切出 ${segs} 片`)
  console.log('')
}

if (problems.length > 0) {
  console.log('要提速只能压缩这几笔里最大的一笔：')
  for (const p of problems) {
    const worst = [
      { n: `采集 ${p.cap.toFixed(2)}ms`, v: p.cap },
      { n: `发布 ${p.pub.toFixed(2)}ms`, v: p.pub },
      { n: `喂录制 ${p.rec.toFixed(2)}ms`, v: p.rec },
    ].sort((x, y) => y.v - x.v)[0]
    console.log(`  ${p.id}（目标 ${p.tgt}fps，每拍 ${p.used.toFixed(2)}ms）：最大一笔是 ${worst.n}`)
  }
  console.log('')
  console.log('采集这一笔的大头是整屏搬一次（1440p 一帧 14.7MB，GPU→CPU 再拷进本路缓冲）。')
  console.log('想在这一档达标，要么把配置里的 scaleWidth/scaleHeight 降下来（例如 1920x1080 只有 1/1.8 的字节量），')
  console.log('要么就把帧率设在"余量为正"的那一档 —— 时间轴照样是准的，只是画面按实际帧率走。')
}

