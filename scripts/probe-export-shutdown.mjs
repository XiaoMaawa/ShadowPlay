// scripts/probe-export-shutdown.mjs —— 「导出中改配置 / 退出服务卡死」
//
// 复现用户报的现象（2026-10-23）：
//   「点保存后我去修改配置（改回放时间），会导致短暂 no signal，这个时候导出的
//     视频会出现 0kb 直接中断导出，然后点击退出服务也不能立刻退出，需要等好久
//     才能退出或者干脆卡死不退出。」
//
// 根因两条：
//   1. 保存后的 reload 会重建会话 → Exporter 被析构 → 导出被拦腰打断，
//      成品停在 0KB；
//   2. 析构里是"等 10 秒再 detach"，而退出流程要等重载线程 —— 于是点"退出
//      服务"要愣十几秒，detach 出去的线程还在写成品。
//
// 现在：reload 遇到正在导出的一路会先等它导完（最多 3 秒），Exporter 析构
// 时先置 cancel_ 让主循环尽快收尾。
//
// 这个脚本量三件事：
//   1. 导出期间发 reload，导出是否仍然成功（不是 0KB、不是失败）
//   2. 之后发 shutdown，进程多久真的退掉（期望 < 6 秒）
//
// 用法：node scripts/probe-export-shutdown.mjs

import { spawn } from 'node:child_process'
import { existsSync, mkdirSync, readdirSync, readFileSync, rmSync, statSync, writeFileSync } from 'node:fs'
import path from 'node:path'
import { projectRoot, tempRoot } from './electron-path.mjs'

const backend = path.join(projectRoot, 'native', 'build', 'shadow-capture.exe')
const PROOF = path.join(tempRoot, 'proofs', 'export-shutdown')
const RECORD_ROOT = path.join(PROOF, 'records')
const EXPORT_DIR = path.join(PROOF, 'exports')
const PROFILES = path.join(PROOF, 'profiles.json')
const STATUS = path.join(tempRoot, 'status', 'status-v1.json')

const PROFILE_ID = 'p-shut'
const WINDOW_SEC = 12
const RECORD_SEC = 14

if (!existsSync(backend)) {
  console.error('[probe] 找不到采集服务，先 npm run build:native')
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
const fail = (msg) => {
  console.log(`[probe] × ${msg}`)
  process.exitCode = 1
}
const ok = (msg) => console.log(`[probe] ✓ ${msg}`)

const profileJson = (replaySeconds) => ({
  version: 1,
  recordRoot: RECORD_ROOT,
  exportDir: EXPORT_DIR,
  profiles: [
    {
      id: PROFILE_ID,
      num: 1,
      name: 'shut',
      enabled: true,
      source: { kind: 'monitor', monitorIndex: 0, clientAreaOnly: false, wgc: true },
      video: {
        fps: 30,
        bitrateMbps: 6,
        replaySeconds,
        segmentSeconds: 2,
        scaleWidth: 0,
        scaleHeight: 0,
      },
      createdAt: '2026-01-01T00:00:00.000Z',
    },
  ],
})

rmSync(PROOF, { recursive: true, force: true })
mkdirSync(RECORD_ROOT, { recursive: true })
mkdirSync(EXPORT_DIR, { recursive: true })
mkdirSync(path.dirname(STATUS), { recursive: true })
rmSync(STATUS, { force: true })
writeFileSync(PROFILES, JSON.stringify(profileJson(WINDOW_SEC), null, 2))

const child = spawn(backend, ['--profiles', PROFILES], {
  cwd: projectRoot,
  stdio: ['pipe', 'pipe', 'pipe'],
  windowsHide: true,
})
child.stdout.setEncoding('utf8')
child.stdout.on('data', (chunk) => {
  // export_started / export_result 是判断"到底有没有开始导"的唯一直接证据。
  for (const line of String(chunk).split('\n')) {
    if (/export_started|export_result|session_reloaded|profile_reload_failed/.test(line)) {
      console.log(`[svc] ${line.trim()}`)
    }
  }
})
child.stderr.setEncoding('utf8')
child.stderr.on('data', () => {})

let exited = false
let exitedAt = 0
child.on('exit', () => {
  exited = true
  exitedAt = Date.now()
})

// ---- 1. 录够一段 ----------------------------------------------------------
let it = null
const t0 = Date.now()
while (Date.now() - t0 < 20000) {
  await sleep(300)
  it = item()
  if (it?.state === 'live' && (it.recordFrames ?? 0) > 60) break
}
if (!it || it.state !== 'live') {
  fail(`服务没进 live（${it?.state}：${it?.detail ?? ''}）`)
  child.kill()
  process.exit(1)
}
console.log(`[probe] live，录 ${RECORD_SEC}s 填窗口…`)
await sleep(RECORD_SEC * 1000)

// ---- 2. 发导出，紧接着改配置 + reload -------------------------------------
child.stdin.write(`{"cmd":"export","id":"${PROFILE_ID}","seconds":${WINDOW_SEC}}\n`)
await sleep(150)

// 用户"改回放时间"：先把新值写盘，再让服务重读。
writeFileSync(PROFILES, JSON.stringify(profileJson(8), null, 2))
const tReload = Date.now()
child.stdin.write(`{"cmd":"reload","id":"${PROFILE_ID}"}\n`)
console.log('[probe] 导出进行中，已改配置并发出 reload')

// ---- 3. 等导出结束 --------------------------------------------------------
//
// 【为什么不再盯 exportJobId】reload 会重建会话 → Exporter 跟着重建 →
// 状态里的 jobId 归零。所以"jobId 有没有前进"这件事在重建之后根本读不到。
// 真正该验的是**磁盘上的成品**：有文件、不是 0 字节。
await sleep(6000)

let files = []
try {
  files = readdirSync(EXPORT_DIR)
    .filter((f) => f.toLowerCase().endsWith('.mp4'))
    .map((f) => {
      const p = path.join(EXPORT_DIR, f)
      return { name: f, bytes: statSync(p).size }
    })
} catch {
  /* 目录还没有 */
}
const biggest = files.slice().sort((a, b) => b.bytes - a.bytes)[0]
if (!files.length) {
  fail('导出目录里没有产物（导出被 reload 打断，而且半成品也没了）')
} else if (!biggest || biggest.bytes < 50 * 1024) {
  fail(`产物太小/空文件：${files.map((f) => `${f.name}=${f.bytes}B`).join(', ')}`)
} else {
  ok(`产物完整：${biggest.name}（${(biggest.bytes / 1048576).toFixed(2)}MB，共 ${files.length} 个）`)
}

// 顺便看一眼 reload 有没有把会话搞坏
const afterReload = item()
console.log(
  `[probe] reload 之后：state=${afterReload?.state} recording=${afterReload?.recording} ` +
    `frames=${afterReload?.recordFrames}`,
)

// ---- 4. 退出服务，量耗时 --------------------------------------------------
const tQuit = Date.now()
child.stdin.write('{"cmd":"shutdown"}\n')
while (!exited && Date.now() - tQuit < 30000) await sleep(50)
const quitMs = exited ? exitedAt - tQuit : -1
if (!exited) {
  fail('30 秒内没有退出（卡死）')
  try {
    child.kill('SIGKILL')
  } catch {
    /* ignore */
  }
} else if (quitMs > 6000) {
  fail(`退出用了 ${(quitMs / 1000).toFixed(1)}s（太慢，用户会以为卡死）`)
} else {
  ok(`退出用了 ${(quitMs / 1000).toFixed(2)}s`)
}

console.log(process.exitCode ? '[probe] 结论：不通过' : '[probe] 结论：通过')
process.exit(process.exitCode ?? 0)
