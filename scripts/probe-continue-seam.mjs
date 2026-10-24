// scripts/probe-continue-seam.mjs —— 验「连续保存两支视频，首尾是不是严丝合缝」
//
// 用户报的问题（原话）：
//   「窗口捕获连续保存两个视频后，两个视频不连续，出现了丢帧。」
//
// 根因（见 app/main.cjs 的 saveExportCut 注释）：切点原来是主进程的 Date.now()，
// 而分片时间轴是**内容轴** —— 编码器跟不上时它落后墙钟（1440p@144 实测落后十几
// 秒），点保存那一刻早就越过成品的末帧了。于是第二支从"现在"开始导，中间那段
// 真实画面两支都没有，拼起来就是"丢帧"。
//
// 现在导出端回报 contentEndUnixMs（成片真正铺到的那一刻），主进程拿它当下一次
// 的切点。这个探针用**真采集 + 真导出**量这件事（不造假分片）：
//
//   1. 起服务，录满回放窗口 → flush
//   2. 导出 A（普通"最近 N 秒"），读回 contentEndUnixMs
//   3. 继续录一段 → flush
//   4. 导出 B：切点 = A 的 contentEndUnixMs        ← 新口径
//   5. 导出 C：切点 = flush 完成后的 Date.now()    ← 旧口径（对照）
//
// 判据：
//   dur(B) ≈ contentEnd(B) - contentEnd(A)   → 一支接一支，一帧不丢
//   dur(C) ≈ dur(B) - 内容轴落后量            → 旧口径丢的就是这段
//
// 用法：node scripts/probe-continue-seam.mjs
//       node scripts/probe-continue-seam.mjs --replay=12 --after=4.5

import { spawn, spawnSync } from 'node:child_process'
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs'
import { createRequire } from 'node:module'
import path from 'node:path'
import { projectRoot, tempRoot } from './electron-path.mjs'

const require = createRequire(import.meta.url)
const backend = path.join(projectRoot, 'native', 'build', 'shadow-capture.exe')
const addonPath = path.join(projectRoot, 'native', 'build', 'shadow_ring.nodejs.node')
const PROOF = path.join(tempRoot, 'proofs', 'continue-seam')
const RECORD_ROOT = path.join(PROOF, 'records')
const EXPORT_DIR = path.join(PROOF, 'exports')
const PROFILES = path.join(PROOF, 'profiles.json')
const STATUS = path.join(tempRoot, 'status', 'status-v1.json')

const arg = (name, dflt) => {
  const hit = process.argv.find((a) => a.startsWith(`--${name}=`))
  return hit ? hit.split('=')[1] : dflt
}
const REPLAY = Number(arg('replay', 12)) // 回放窗口（秒）：导出 A 的时长
const AFTER = Number(arg('after', 4.5)) // 两次保存之间再录多久（秒）= 导出 B 的时长

if (!existsSync(backend) || !existsSync(addonPath)) {
  console.error('[probe] 找不到原生产物，先 npm run build:native')
  process.exit(1)
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
const item = () => {
  try {
    return JSON.parse(readFileSync(STATUS, 'utf8')).items?.[0] ?? null
  } catch {
    return null
  }
}
const killTree = (p) => {
  if (!p?.pid) return
  try {
    spawnSync('taskkill', ['/pid', String(p.pid), '/T', '/F'], {
      stdio: 'ignore',
      windowsHide: true,
    })
  } catch {
    /* 已经退了 */
  }
}

const problems = []
const ok = (m) => console.log(`[probe] ✓ ${m}`)
const fail = (m) => {
  problems.push(m)
  console.log(`[probe] × ${m}`)
}

rmSync(PROOF, { recursive: true, force: true })
mkdirSync(RECORD_ROOT, { recursive: true })
mkdirSync(EXPORT_DIR, { recursive: true })
mkdirSync(path.dirname(STATUS), { recursive: true })
rmSync(STATUS, { force: true })

// 显示器捕获：不依赖任何外部窗口，跑起来就能录。
writeFileSync(
  PROFILES,
  JSON.stringify(
    {
      version: 1,
      recordRoot: RECORD_ROOT,
      exportDir: EXPORT_DIR,
      profiles: [
        {
          id: 'p-seam',
          num: 1,
          name: 'seam',
          enabled: true,
          source: { kind: 'monitor', monitorIndex: 0, clientAreaOnly: false, wgc: true },
          video: {
            fps: 60,
            bitrateMbps: 8,
            replaySeconds: REPLAY,
            segmentSeconds: 2,
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

const child = spawn(backend, ['--profiles', PROFILES], {
  cwd: projectRoot,
  stdio: ['pipe', 'pipe', 'pipe'],
  windowsHide: true,
})
child.stdout.setEncoding('utf8')
child.stdout.on('data', () => {})
child.stderr.setEncoding('utf8')
child.stderr.on('data', () => {})
child.stdin.write('') // 保持管道

const flushNow = async () => {
  const it = item()
  if (!it) return 0
  const before = it.flushSeq ?? 0
  child.stdin.write(JSON.stringify({ cmd: 'flush', id: it.id }) + '\n')
  // flush 会等编码器 Finalize（几十~几百 ms），状态文件 40ms 一拍。
  const t0 = Date.now()
  while (Date.now() - t0 < 6000) {
    await sleep(60)
    const now = item()?.flushSeq ?? 0
    if (now !== before) return now
  }
  console.log('[probe] ! flush 没等到变化（可能当前片一帧都没有）')
  return before
}

// mvhd 里的时长（不依赖 ffprobe，秒）
function mp4Duration(file) {
  const buf = readFileSync(file)
  let i = 0
  let moov = -1
  while (i + 8 <= buf.length) {
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
  for (let p = moov + 8; p + 8 <= end; ) {
    const size = buf.readUInt32BE(p)
    const type = buf.toString('ascii', p + 4, p + 8)
    if (size < 8) break
    if (type === 'mvhd') {
      const v = buf[p + 8]
      if (v === 1) {
        const ts = buf.readUInt32BE(p + 8 + 20)
        const d = Number(buf.readBigUInt64BE(p + 8 + 24))
        return ts ? d / ts : 0
      }
      const ts = buf.readUInt32BE(p + 8 + 12)
      return ts ? buf.readUInt32BE(p + 8 + 16) / ts : 0
    }
    p += size
  }
  return 0
}

const addon = require(addonPath)
/**
 * 导一次。
 *
 * allowEmpty=true 用于「旧口径」的对照组：切点取的是 flush 完成后的 Date.now()，
 * 而内容轴通常还落在它后面几百毫秒到几秒 —— 这时窗口里**一帧都没有**，导出端会
 * 直接拒绝（"上次保存之后还没有录到新的画面"）。那不是探针出错，那正是要证明的
 * 现象：旧口径下这支视频要么是空的，要么开头缺一截。所以它按 dur=0 记。
 */
async function exportOnce(segDir, cutMs, tag, allowEmpty = false) {
  const res = addon.exportStart(
    segDir,
    EXPORT_DIR,
    `seam-${tag}`,
    REPLAY,
    60,
    2,
    Math.max(0, Math.round(cutMs)),
  )
  if (!res?.ok && allowEmpty) {
    console.log(`[probe] ${tag}：切点 ${Math.round(cutMs)} → 导出端拒绝（${res?.error ?? ''}）`)
    return { ok: false, dur: 0, contentEndUnixMs: 0, empty: true }
  }
  if (!res?.ok) throw new Error(`${tag} 导出没能开始：${res?.error ?? '原因不明'}`)
  // 先等它真的开跑：exportStart 只是排队，worker 线程晚一点才把 running 翻真。
  // 不等的话第一次轮询会读到上一趟的"已结束"状态，直接误判成失败。
  const jobId = res.jobId ?? 0
  const tStart = Date.now()
  while (Date.now() - tStart < 8000) {
    const s = addon.exportStatus()
    if (s && s.running === true && (jobId ? s.jobId === jobId : true)) break
    await sleep(60)
  }
  const t0 = Date.now()
  let st = null
  while (Date.now() - t0 < 180000) {
    await sleep(120)
    st = addon.exportStatus()
    if (st && st.running !== true) break
  }
  if (!st || st.running === true) throw new Error(`${tag} 导出超时`)
  if (!st.ok) {
    if (allowEmpty) {
      console.log(`[probe] ${tag}：切点 ${Math.round(cutMs)} → ${st.detail ?? '导出失败'}`)
      return { ok: false, dur: 0, contentEndUnixMs: 0, empty: true }
    }
    throw new Error(`${tag} 导出失败：${st.detail ?? '原因不明'}`)
  }
  const dur = mp4Duration(st.outPath)
  console.log(
    `[probe] ${tag}：切点 ${Math.round(cutMs) || '（最近 N 秒）'} → ` +
      `${dur.toFixed(3)}s / 内容结尾 ${st.contentEndUnixMs} / ${st.segmentCount} 片`,
  )
  return { ...st, dur }
}

try {
  // ---- 1. 录满回放窗口（再多 3 秒，保证窗口内全是真实画面）----------------
  const needMs = (REPLAY + 3) * 1000
  const t0 = Date.now()
  let it = null
  while (Date.now() - t0 < needMs + 25000) {
    await sleep(300)
    it = item()
    if (it?.state === 'live' && Date.now() - t0 > needMs) break
  }
  if (!it || it.state !== 'live') {
    throw new Error(`服务没进 live（${it?.state}：${it?.detail ?? ''}）`)
  }
  const segDir = it.recordDir ? path.join(it.recordDir, 'segments') : ''
  if (!segDir) throw new Error('状态里没有 recordDir')
  console.log(
    `[probe] live ${((Date.now() - t0) / 1000).toFixed(1)}s：` +
      `${it.detail}；分片 ${it.recordSegments} 个`,
  )

  // ---- 2. 导出 A ----------------------------------------------------------
  await flushNow()
  const a = await exportOnce(segDir, 0, 'a')
  if (!(a.contentEndUnixMs > 0)) {
    throw new Error('导出端没回报 contentEndUnixMs（模块没重编？）')
  }

  // ---- 3. 继续录一段，再 flush（相当于用户又录了一会儿再点保存）----------
  await sleep(AFTER * 1000)
  await flushNow()
  // 旧口径的切点：flush 完成后的"现在"。主进程原来就是在这一刻取 Date.now()。
  const clickedAt = Date.now()

  // ---- 4. 导出 B：切点 = A 的内容结尾（新口径）---------------------------
  const b = await exportOnce(segDir, a.contentEndUnixMs, 'b-seam')

  // ---- 5. 导出 C：切点 = clickedAt（旧口径，对照）-------------------------
  const c = await exportOnce(segDir, clickedAt, 'c-old-cut', true)

  // ---- 6. 判据 -----------------------------------------------------------
  const spanB = (b.contentEndUnixMs - a.contentEndUnixMs) / 1000
  const lag = (clickedAt - a.contentEndUnixMs) / 1000 // 内容轴落后墙钟多少
  console.log('')
  console.log(`  第一支   ${a.dur.toFixed(3)}s  内容结尾 ${a.contentEndUnixMs}`)
  console.log(
    `  第二支   ${b.dur.toFixed(3)}s  [${a.contentEndUnixMs} → ${b.contentEndUnixMs}]` +
      `  应有 ${spanB.toFixed(3)}s`,
  )
  if (c.empty) {
    console.log(
      `  旧口径   导出端拒绝（切点比内容结尾晚 ${(lag * 1000).toFixed(0)}ms，窗口里一帧没有）`,
    )
  } else {
    console.log(
      `  旧口径   ${c.dur.toFixed(3)}s  切点比内容结尾晚 ${(lag * 1000).toFixed(0)}ms` +
        ` → 少 ${((b.dur - c.dur) * 1000).toFixed(0)}ms`,
    )
  }

  if (Math.abs(b.dur - spanB) > 0.15) {
    fail(
      `第二支时长 ${b.dur.toFixed(3)}s ≠ 接缝到内容结尾 ${spanB.toFixed(3)}s` +
        `（差 ${((spanB - b.dur) * 1000).toFixed(0)}ms，正数就是丢掉的画面）`,
    )
  } else {
    ok(`第二支正好铺满接缝到内容结尾（${spanB.toFixed(3)}s），一帧不丢`)
  }
  if (b.dur <= 0.5) fail(`第二支几乎没有内容（${b.dur.toFixed(3)}s）`)
  if (lag < 0.1) {
    console.log(
      `[probe] 注：这次内容轴只落后墙钟 ${(lag * 1000).toFixed(0)}ms（机器够快），` +
        `旧口径的差别看不出来；探针的核心判据仍然是上面那条`,
    )
  } else if (c.empty) {
    ok(
      `旧口径连一支都导不出来（切点已经越过内容结尾 ${(lag * 1000).toFixed(0)}ms）` +
        ` —— 这就是用户看到的"两支接不上"`,
    )
  } else if (b.dur - c.dur < lag - 0.15) {
    fail(
      `旧口径没短在应该短的地方（晚 ${(lag * 1000).toFixed(0)}ms，只少了 ` +
        `${((b.dur - c.dur) * 1000).toFixed(0)}ms）`,
    )
  } else {
    ok(`旧口径确实少了 ${((b.dur - c.dur) * 1000).toFixed(0)}ms —— 这就是用户看到的"丢帧"`)
  }
} catch (err) {
  fail(err?.message ?? String(err))
} finally {
  killTree(child)
}

console.log(problems.length ? '\n[probe] 结论：不通过' : '\n[probe] 结论：通过')
process.exit(problems.length ? 1 : 0)
