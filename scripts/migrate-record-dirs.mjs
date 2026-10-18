// 旧录像目录迁移脚本（一次性）。
//
// 把老的 `<源>-<名字>-<尾号>` 目录并入新式数字目录，并清掉老的分配记录。
// 用法：node scripts/migrate-record-dirs.mjs [--dry]
import fs from 'node:fs'
import path from 'node:path'

const dry = process.argv.includes('--dry')
const root = path.join(process.env.TEMP, 'shadow-play', 'app', 'records')

// 配置 id -> 数字编号（读仓库根的 profiles.json）
const doc = JSON.parse(fs.readFileSync('profiles.json', 'utf8'))
const byNum = new Map(doc.profiles.map((p) => [p.num, p]))

// 老的 .alloc/<id>.json 记着"这条配置原来落在哪个目录"，以它为准。
const allocDir = path.join(root, '.alloc')
const claimed = new Map() // 老目录名 -> num
if (fs.existsSync(allocDir)) {
  for (const f of fs.readdirSync(allocDir)) {
    const id = f.replace(/\.json$/, '')
    const text = fs.readFileSync(path.join(allocDir, f), 'utf8')
    const m = text.match(/"dir"\s*:\s*"([^"]+)"/)
    if (!m) continue
    const p = doc.profiles.find((x) => x.id === id)
    if (!p) {
      console.log(`跳过 ${id}：profiles.json 里已经没有这条配置`)
      continue
    }
    claimed.set(m[1], p.num)
  }
}

// 没有分配记录的旧目录（monitor-src-desk、-2、-3…）也按"前缀 -> 配置"猜一个。
// 这些目录里的录像本来就已经滚出回放窗口了，只是不想留在盘上碍眼。
function guessNum(name) {
  if (name === 'win-src-win') return doc.profiles.find((p) => p.source?.kind === 'window')?.num ?? 0
  if (name.startsWith('monitor-src') || name.startsWith('desktop-')) {
    return doc.profiles.find((p) => p.source?.kind === 'monitor' || p.source?.kind === 'desktop')?.num ?? 0
  }
  return 0
}

const entries = fs.readdirSync(root).filter((n) => {
  const full = path.join(root, n)
  return fs.statSync(full).isDirectory() && !n.startsWith('.')
})

for (const name of entries) {
  if (/^\d+$/.test(name)) continue // 已经是新式数字目录
  const num = claimed.get(name) ?? guessNum(name)
  if (!num) {
    console.log(`${name}：认不出属于哪条配置，保留不动`)
    continue
  }
  const target = String(num)
  const from = path.join(root, name)
  const to = path.join(root, target)
  console.log(`${dry ? '[试运行] ' : ''}${name} → ${target}/`)

  if (dry) continue

  fs.mkdirSync(path.join(to, 'segments'), { recursive: true })
  // 旧目录下可能直接是 segments/，也可能什么都没有
  const segFrom = path.join(from, 'segments')
  if (fs.existsSync(segFrom)) {
    // 旧的分代目录（0001-fps60 之类）搬进目标下，序号顺延
    let next = 1
    for (const e of fs.readdirSync(path.join(to, 'segments'))) {
      if (/^\d+$/.test(e)) next = Math.max(next, Number(e) + 1)
    }
    for (const gen of fs.readdirSync(segFrom)) {
      const src = path.join(segFrom, gen)
      if (!fs.statSync(src).isDirectory()) continue
      const dst = path.join(to, 'segments', String(next))
      fs.mkdirSync(dst, { recursive: true })
      for (const f of fs.readdirSync(src)) {
        fs.renameSync(path.join(src, f), path.join(dst, f))
      }
      next += 1
    }
  }
  // 搬完就删掉空壳
  fs.rmSync(from, { recursive: true, force: true })
  console.log(`  ${name} 已并入 ${target}/ 并删除空壳`)
}

if (!dry && fs.existsSync(allocDir)) {
  fs.rmSync(allocDir, { recursive: true, force: true })
  console.log('已清掉老的 .alloc 分配记录（新目录名由 num 直接决定，不再需要它）')
}
