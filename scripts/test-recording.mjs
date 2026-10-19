// scripts/test-recording.mjs —— 验证分片录制 + 滚动删除（需求 5）。
//
// 为什么不用界面自检来验：录制是**服务侧**行为，跟有没有人预览无关
// （而且它正是为了"界面没开也一直在录"才被设计成挂在采集循环上的）。
// 这里直接起 shadow-capture.exe，喂一份「回放 10 秒 / 分片 2 秒」的配置，
// 默认连录两轮（同一份配置、同一个目录），验四件事：
//
//   1. temp/proofs/recording/<子目录>/segments/<轮次> 下真的出现了 .mp4 分片，
//      而且能被 ffprobe 解出来（不是 0 字节空壳）；
//   2. 磁盘上的内容被**回放时长**限住，不会一直涨（滚动删除生效）；
//   3. 时间轴确实在往前走 —— 本轮 index.csv 里各槽位的区间终点一直推进，
//      而磁盘上始终只有回放窗口那么长的内容。
//   4. 第二轮**不**继承第一轮的分片字节数（复写槽位时文件必须先截断）。
//
// ⚠️ 分片**数量**不再是判据。录制端从「保留最后 N 片」改成了「保留最近 N 秒」
// （见 DESIGN.md 9.5 与 MfEncoder::PurgeExpired）：尺寸一变就切片，一次拖窗口
// 能切出七八片零点几秒的过渡片；若仍按片数保留，20 秒窗口会被这些碎片吃光，
// 导出几乎全程 NO SIGNAL（用户实际报过）。现在槽位给的是标称片数的 8 倍余量，
// 正常等长录制时磁盘上约 1.5 倍标称片数（每片在开下一片时才被判定过期），
// 拖窗口时片数会更多而**总时长**不变 —— 所以断言看「清单跨度 + 磁盘字节」，不看行数。
//
// 第 3 条是必须的，别只盯着文件名。踩过的坑：最早那版断言写的是
// 「第一个分片不应该是 000000.mp4」，跑 26 秒后判定失败。其实滚动**是**生效的，
// 是断言错了 —— 分片名是**循环复用的槽位**，不是单调递增的序号。
// 回放 10 秒 / 分片 2 秒 ⇒ 保留 5 个槽位，写满 5 个之后文件名就在
// 000000.mp4..000004.mp4 之间打转，任何时刻第一个文件都叫 000000.mp4。
// 要看"有没有删"，只能看 ①分片数有没有被限住 ②时间轴有没有推进。
//
// 第二个变化（MF 改造后）：index.csv 从**追加日志**变成了**当前窗口清单**。
// ffmpeg 的 `-segment_list` 只会往后追加，跑 26 秒就有 12 行覆盖 0~24s，
// 所以那时不能拿"首行起点 → 末行终点"当窗口跨度。现在 `MfEncoder` 每次
// 开关分片都整份重写清单，行数恒等于槽位数，末行终点就是时间轴末端。
// 真正代表"此刻磁盘上留着哪一段"的还是**磁盘上那几个分片**：
// 5 个文件、每个 2 秒 ⇒ 窗口 ≈ 10 秒。下面用「分片数 × 分片时长」来
// 卡窗口大小，用"末行终点一直在涨"来证明它在往前滚。
//
// 第三个坑（这轮才修）：环形复写槽位时**不会先截断**。
// 上一轮这个槽位有 6.4 MB、这轮只写 2 MB，文件就还是 6.4 MB —— 于是
// 界面上的"占多少磁盘 / 录了多少字节"跟着虚高（实测回放 120s 的配置显示
// 356 MB，而按窗口时长 × 码率算只有 120 MB 上下）。修法是起录前清掉
// 旧的分片（`MfEncoder::Start` 里清目录），第 4 条就是它的回归闸门。
//
// 第四个变化（分代改造）：分片现在按**轮次**分子目录存
// （`segments/0001-fps30/`），每改一次帧率就换一个轮次，旧轮次留着，
// 导出时一起拼进去按新帧率重采样。所以「本轮」= 数字最大的那个目录，
// 底下的清单文件也叫 `index.csv` 了。
//
// 另外一个坑：不要在验完之前把服务 kill 掉。分片是 ffmpeg 收尾时才写完的，
// 强杀会让最后一个分片停在中途，服务自报的 recordBytes 也会比磁盘上的小。
// 这里留一段时间给收尾。
import { spawn, spawnSync } from 'node:child_process'
import { existsSync, mkdirSync, readdirSync, readFileSync, rmSync, statSync, writeFileSync } from 'node:fs'
import path from 'node:path'
import { projectRoot, tempRoot } from './electron-path.mjs'
import { ensureServiceExe } from '../app/temp-const.cjs'

// 必须走 ensureServiceExe 拿「临时目录里那份副本」，不能直接用 native/build 下的原件。
// 仓库目录可能带 Low 完整性标签（DSH 沙箱会打），原件的进程也跟着是 Low，
// 于是往 %TEMP% 里建录制目录会直接 `Access is denied.`。详见 app/temp-const.cjs。
const SERVICE = ensureServiceExe({
  srcExe: path.join(projectRoot, 'native', 'build', 'shadow-capture.exe'),
  tempRoot,
  log: (m) => console.log('[recording]', m),
})
const TEST_ROOT = path.join(tempRoot, 'proofs', 'recording')
const PROFILE_FILE = path.join(projectRoot, 'native', 'build', 'artifacts', 'record-profiles.json')

// 参数是**秒**数，别直接当毫秒用（第一版就是这么把 25 秒跑成 0.025 秒的）。
const WATCH_MS = Number(process.argv[2] ?? 25) * 1000
const REPLAY_S = 10
const SEGMENT_S = 2
const EXPECT_SEGMENTS = Math.ceil(REPLAY_S / SEGMENT_S)
// 第 0 轮：验证"滚动删除有没有把分片数限住"。
// 第 1 轮：**同一份配置、同一个目录再录一遍** —— 验证复写槽位时文件被截断。
const ROUNDS = Number(process.env.SHADOW_RECORD_ROUNDS ?? 2)
// 强杀之后留给 ffmpeg 写完当前分片的时间。
const DRAIN_MS = 4000

const VERBOSE = process.argv.includes('--verbose') || process.env.SHADOW_TEST_VERBOSE === '1'

if (!existsSync(SERVICE)) {
  console.error(`找不到采集服务：${SERVICE}`)
  process.exit(2)
}

// 干净起点：残留的旧分片会让"滚动删除有没有生效"看不出来。
rmSync(TEST_ROOT, { recursive: true, force: true })
mkdirSync(TEST_ROOT, { recursive: true })
mkdirSync(path.dirname(PROFILE_FILE), { recursive: true })

const doc = {
  version: 1,
  recordRoot: TEST_ROOT,
  exportDir: path.join(tempRoot, 'exports'),
  profiles: [
    {
      id: 'p-rec',
      name: '滚动录制验证',
      enabled: true,
      source: { kind: 'desktop', monitorIndex: 0, clientAreaOnly: false },
      video: { fps: 30, bitrateMbps: 6, replaySeconds: REPLAY_S, segmentSeconds: SEGMENT_S },
      createdAt: '2026-10-16T18:40:00+08:00',
    },
  ],
}
writeFileSync(PROFILE_FILE, JSON.stringify(doc, null, 2), 'utf8')

/**
 * 找出**正在录的那一轮**目录。
 *
 * 分片是按代次分目录存的（`segments/1/`、`segments/2/`…）：每改一次帧率就换
 * 一代，旧一代的录像留着，导出时一起拼进去按新帧率重采样。所以测试要盯的是
 * 数字最大的那一代，否则会把上一代留下的分片算进这一轮。
 *
 * 兼容两种命名：老目录曾叫 `0001-fps30`（序号-fps），现在只叫 `1`（纯代次）。
 * 平铺在 `segments/` 根下的分片是分代改造之前的老数据，不算本轮。
 */
function currentRunDir(segRoot) {
  if (!existsSync(segRoot)) return null
  let best = null
  for (const e of readdirSync(segRoot, { withFileTypes: true })) {
    if (!e.isDirectory()) continue
    const m = e.name.match(/^(\d+)(?:-|$)/)
    if (!m) continue
    const seq = Number(m[1])
    if (!best || seq > best.seq) best = { seq, name: e.name, dir: path.join(segRoot, e.name) }
  }
  return best
}

function segmentsOf(dir) {
  if (!dir || !existsSync(dir)) return []
  return readdirSync(dir)
    .filter((f) => f.endsWith('.mp4'))
    .sort()
    .map((f) => {
      const st = statSync(path.join(dir, f))
      return { name: f, bytes: st.size, mtimeMs: st.mtimeMs }
    })
}

/**
 * 去掉"正在写"的那一片。
 *
 * 分段器是边写边切：任何时刻总有**一片还在写**，它的体积和时长都只有完整片的
 * 一小截（实测 0.2s / 262144 字节，完整片 2.0s / 1.5MB）。而且槽位循环复写
 * 会让文件名循环覆写 —— 最新的一片未必叫 000004.mp4，可能是 000002.mp4。
 *
 * 所以不能按文件名猜，只能按 mtime：mtime 最新的那一片就是正在写的，排除掉。
 * 只留 mtime 明显更早的完整片。
 */
function completeSegments(segs) {
  if (segs.length <= 1) return []
  const newest = Math.max(...segs.map((s) => s.mtimeMs))
  return segs.filter((s) => s.mtimeMs < newest)
}

function findSegmentsDir() {
  if (!existsSync(TEST_ROOT)) return null
  for (const entry of readdirSync(TEST_ROOT, { withFileTypes: true })) {
    if (!entry.isDirectory()) continue
    const seg = path.join(TEST_ROOT, entry.name, 'segments')
    if (existsSync(seg)) return { profileDir: path.join(TEST_ROOT, entry.name), seg }
  }
  return null
}

/** 读本轮的分片清单 `index.csv` —— 它是**时间轴**的真凭据（每行：文件,起点秒,终点秒）。 */
function readTimeline(runDir) {
  if (!runDir) return null
  const p = path.join(runDir, 'index.csv')
  if (!existsSync(p)) return null
  const rows = []
  for (const line of readFileSync(p, 'utf8').split(/\r?\n/)) {
    // 第四列是每片的墙钟 Unix **毫秒**（本项目加在 ffmpeg 的 csv 格式之后，用来
    // 判断"这片属不属于回放窗口"）；第五、六列是这片编码时的画布宽高（过渡期
    // 画布可能比真实内容大，导出端据此裁黑边）。老数据只有三列，所以后面都可选。
    const m = line.match(/^([0-9]+\.mp4),([0-9.]+),([0-9.]+)(?:,([0-9]+))?(?:,([0-9]+),([0-9]+))?$/)
    if (m) {
      rows.push({
        name: m[1],
        start: Number(m[2]),
        end: Number(m[3]),
        startUnix: m[4] ? Number(m[4]) : 0,
      })
    }
  }
  if (!rows.length) return null
  return { rows, first: rows[0].start, last: rows[rows.length - 1].end }
}

/**
 * 录一轮，返回这一轮观测到的数据。**不**清目录、**不**判成败 ——
 * 判定统一放到所有轮次跑完之后，这样跨轮的结论（第 4 条）才有地方落。
 */
async function recordOnce(round) {
  console.log(`\n[recording] 第 ${round + 1}/${ROUNDS} 轮：起服务，观察 ${WATCH_MS / 1000}s（回放 ${REPLAY_S}s / 分片 ${SEGMENT_S}s）`)
  const child = spawn(SERVICE, ['--profiles', PROFILE_FILE], {
    cwd: projectRoot,
    stdio: ['ignore', 'pipe', 'pipe'],
  })

  let lastRecordLine = null
  const allLines = []
  let stdoutBuf = ''
  child.stdout.on('data', (chunk) => {
    stdoutBuf += chunk.toString('utf8')
    let idx
    while ((idx = stdoutBuf.indexOf('\n')) >= 0) {
      const line = stdoutBuf.slice(0, idx)
      stdoutBuf = stdoutBuf.slice(idx + 1)
      if (VERBOSE) console.log(`[service] ${line}`)
      else allLines.push(line)
      if (!line.includes('"type":"session"')) continue
      // 只记服务自己报的录制数字，脚本不自己算，避免"自己验证自己"。
      const m = line.match(/"recordSegments":(\d+).*?"recordBytes":(\d+).*?"recordFrames":(\d+)/)
      if (m) lastRecordLine = { segments: +m[1], bytes: +m[2], frames: +m[3] }
    }
  })
  child.stderr.on('data', (c) => process.stderr.write(`[service] ${c}`))

  const samples = []
  const sampler = setInterval(() => {
    const found = findSegmentsDir()
    if (!found) return
    const run = currentRunDir(found.seg)
    const segs = segmentsOf(run?.dir)
    const tl = readTimeline(run?.dir)
    samples.push({
      count: segs.length,
      // 时间轴终点：证明"内容在往前走"。末行终点就是当前时间轴末端。
      timelineEnd: tl ? tl.last : 0,
      bytes: segs.reduce((a, s) => a + s.bytes, 0),
      names: segs.map((s) => s.name).join(','),
    })
  }, 1000)

  await new Promise((r) => setTimeout(r, WATCH_MS))
  clearInterval(sampler)

  // 先让服务有机会自己收尾（它会 Finalize 当前分片再退出）。强杀只作兜底：
  // 分片是收尾时写完的，太早杀会让最后一个分片停在中途。
  child.kill('SIGINT')
  await new Promise((r) => setTimeout(r, DRAIN_MS))
  if (child.exitCode === null && child.signalCode === null) child.kill()
  await new Promise((r) => setTimeout(r, 1200))

  const found = findSegmentsDir()
  if (!found) {
    console.error(`[recording] ✗ 第 ${round + 1} 轮没找到任何分片目录 —— 录制根本没起来`)
    if (!VERBOSE) {
      const tail = allLines.slice(-25)
      if (tail.length) {
        console.error(`\n=== 服务输出尾部 ===`)
        for (const l of tail) console.error(l)
      }
    }
    process.exit(1)
  }

  const run = currentRunDir(found.seg)
  const segs = segmentsOf(run?.dir)
  return {
    found,
    runDir: run?.dir ?? null,
    runName: run?.name ?? '(无轮次目录)',
    segs,
    timeline: readTimeline(run?.dir),
    lastRecordLine,
    samples,
    allLines,
    totalBytes: segs.reduce((a, s) => a + s.bytes, 0),
    peak: samples.reduce((a, s) => Math.max(a, s.count), 0),
    peakBytes: samples.reduce((a, s) => Math.max(a, s.bytes), 0),
  }
}

const runs = []
for (let r = 0; r < ROUNDS; r++) runs.push(await recordOnce(r))

// ---- 判定 --------------------------------------------------------------
const problems = []
// 窗口预算：配置请求 6 Mbps，但 MF 的 CBR 输出会**超额**。
//
// 实测 2560x1440@30 / 6Mbps 配置下，每轮 18 秒录到 8.4 MB ⇒ 实际码率约
// 3.9 Mbps（低于请求，正常）；但**瞬时**峰值会冲到 12.3 MB。原因是
// SinkWriter 每个分片都有独立的 VBV 缓冲，切片的头一帧是关键帧（大），
// 5 个槽位里新写的那个还没被后面的平缓帧摊薄。所以预算给 2.5 倍余量，
// 只用来抓"老分片在堆积"这种量级错误（那样会到 20MB 以上）。
const windowBudget = (REPLAY_S * 6e6 / 8) * 2.5
const probeTargets = []

for (let i = 0; i < runs.length; i++) {
  const r = runs[i]
  const tag = `第 ${i + 1} 轮`

  console.log(`\n=== ${tag} 结果 ===`)
  console.log(`分片根目录   : ${r.found.seg}`)
  console.log(`本轮目录     : ${r.runName}`)
  console.log(`最终分片数   : ${r.segs.length}（期望 ≈ ${EXPECT_SEGMENTS}）`)
  console.log(`观测到的峰值 : ${r.peak}`)
  console.log(`分片清单     : ${r.segs.map((s) => s.name).join(', ')}`)
  console.log(`总字节       : ${r.totalBytes}（峰值 ${r.peakBytes}，窗口预算 ${Math.round(windowBudget)}）`)
  console.log(`服务自报     : ${JSON.stringify(r.lastRecordLine)}`)
  if (r.timeline) {
    console.log(
      `时间轴       : 日志共 ${r.timeline.rows.length} 个分片区间，` +
        `末端已到 ${r.timeline.last.toFixed(1)}s（磁盘窗口 = ${r.segs.length} × ${SEGMENT_S}s）`,
    )
  }
  console.log(`采样序列     : ${r.samples.map((s) => `${s.count}@${s.timelineEnd.toFixed(0)}s`).join(' -> ')}`)

  if (r.segs.length === 0) problems.push(`${tag}：一个分片都没有`)
  if (r.totalBytes === 0) problems.push(`${tag}：分片都是空的`)

  if (WATCH_MS / 1000 > REPLAY_S + SEGMENT_S * 2) {
    // 分片数上限：正常等长录制时槽位不会被占满，PurgeExpired 在开新片时清，
    // 所以稳态约 1.5 倍标称片数（实测 10s 窗口 / 2s 片 → 7~8 片）。
    // 这里只拦"完全不删"的病态情况（涨到标称片数好几倍）。
    const segCap = Math.ceil((REPLAY_S * 2) / SEGMENT_S) + 2
    if (r.segs.length > segCap) {
      problems.push(`${tag}：分片数 ${r.segs.length} 超过上限 ${segCap} —— 滚动删除没把长度限住`)
    }
    if (r.peak > segCap + 2) {
      problems.push(`${tag}：分片数涨到 ${r.peak} 超过上限 ${segCap + 2} —— 滚动删除没生效`)
    }
    // 磁盘占用必须跟"窗口时长 × 码率"同量级，而不是跟总录制时长同量级。
    if (r.peakBytes > windowBudget) {
      problems.push(
        `${tag}：磁盘占用峰值 ${r.peakBytes} 字节，超过窗口预算 ${Math.round(windowBudget)} —— ` +
          (i === 0 ? '老分片在堆积' : '上一轮的分片没被清掉（复写槽位前没截断）'),
      )
    }
  }

  if (!r.timeline) {
    problems.push(`${tag}：本轮没有 index.csv —— 看不出时间轴，无法证明录制在推进`)
  } else {
    // index.csv 是**当前窗口清单**（每次开/关分片整份重写），不是追加日志。
    // 行数 = 此刻盘上的活分片数，不做等值断言（见文件头：保留策略已改成按秒）。
    if (r.timeline.rows.length === 0) {
      problems.push(`${tag}：index.csv 是空的 —— 分片没被登记进清单`)
    }
    if (r.timeline.last < REPLAY_S) {
      problems.push(`${tag}：时间轴末端只有 ${r.timeline.last.toFixed(1)}s，没超过回放窗口 ${REPLAY_S}s`)
    }
    if (r.timeline.last <= r.segs.length * SEGMENT_S) {
      problems.push(`${tag}：时间轴末端 ${r.timeline.last.toFixed(1)}s 还没超出磁盘容量 ${r.segs.length * SEGMENT_S}s —— 没滚过`)
    }
    // 环形窗口的正确形态：清单里各槽位按起点排好后应当**首尾相接、无缝**，
    // 且总跨度等于回放窗口（不留过期余量时最多 window + 一片，因为过期判定
    // 发生在开新片的那一刻）。起点不是 0 是正常的 —— 录到 18 秒时，
    // 留在盘上的是最后 10 秒（8s..18s），最早那几片已经被清掉了。
    // 曾经在这里断言"第二轮必须从 0 开始"，那是对环形缓冲的误解。
    const sorted = [...r.timeline.rows].sort((a, b) => a.start - b.start)
    let gaps = 0
    for (let k = 1; k < sorted.length; k++) {
      if (Math.abs(sorted[k].start - sorted[k - 1].end) > 0.05) gaps++
    }
    if (gaps > 0) {
      problems.push(`${tag}：清单里有 ${gaps} 处时间断档 —— 槽位复写后时间轴串不起来`)
    }
    const span = sorted[sorted.length - 1].end - sorted[0].start
    const spanCap = REPLAY_S + SEGMENT_S * 2
    if (span > spanCap) {
      problems.push(`${tag}：清单跨度 ${span.toFixed(1)}s，超过窗口上限 ${spanCap}s —— 老分片没被清掉`)
    }
    if (span < REPLAY_S - SEGMENT_S) {
      problems.push(`${tag}：清单跨度只有 ${span.toFixed(1)}s，短于回放窗口 ${REPLAY_S}s —— 内容被切丢太多`)
    }
  }

  // 抽一片已经写完的分片做解码验证 —— 正在写的那一片必然是不完整的。
  const done = completeSegments(r.segs)
  const probeTarget = done[Math.floor(done.length / 2)]
  if (probeTarget) probeTargets.push({ round: i + 1, dir: r.runDir, name: probeTarget.name })
  else problems.push(`${tag}：只有 ${r.segs.length} 个 .mp4，挑不出已经写完的分片来做解码验证`)
}

// ---- 抽一片真解码，确认不是空壳 -----------------------------------------
for (const t of probeTargets) {
  const p = path.join(t.dir, t.name)
  const r = spawnSync(
    'ffprobe',
    ['-v', 'error', '-show_entries', 'format=duration', '-show_entries',
     'stream=codec_name,width,height,nb_frames', '-of', 'json', p],
    { encoding: 'utf8' },
  )
  console.log(`\nffprobe（第 ${t.round} 轮）${t.name}：`)
  try {
    const info = JSON.parse(r.stdout)
    const st = info.streams?.[0] ?? {}
    const dur = Number(info.format?.duration ?? 0)
    console.log(`  -> codec=${st.codec_name} ${st.width}x${st.height} 时长=${dur.toFixed(2)}s`)
    if (st.codec_name !== 'h264') problems.push(`第 ${t.round} 轮：分片编码不是 h264：${st.codec_name}`)
    if (!(dur > 0.5)) problems.push(`第 ${t.round} 轮：抽检分片时长只有 ${dur}s，解码内容不完整`)
  } catch {
    console.log(r.stdout?.trim() || r.stderr?.trim() || '(无输出)')
    problems.push(`第 ${t.round} 轮：ffprobe 输出解析不了，分片可能不可播放`)
  }
}

console.log('')
if (problems.length) {
  console.log('结果：失败')
  for (const p of problems) console.log(`  ✗ ${p}`)
  const last = runs[runs.length - 1]
  const tail = last.allLines.slice(-25)
  if (tail.length) {
    console.error(`\n=== 服务输出尾部 ===`)
    for (const l of tail) console.error(l)
  }
  process.exit(1)
}
console.log(
  `结果：通过 —— 分片落地、能解码、数量被回放时长限住、时间轴持续推进` +
    (ROUNDS > 1 ? `，且复录 ${ROUNDS} 轮后磁盘占用仍只有回放窗口那么大（槽位复写前有截断）` : ''),
)
