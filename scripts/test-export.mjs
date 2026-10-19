// scripts/test-export.mjs — 需求 6 的端到端验收：真的点一次"导出"，检查 MP4 落地
//
// 为什么不能只测 C++ 侧的 ExportMerged：
//   导出命令走的是**服务 stdin**，而这条通道只有"启动服务的那个进程"拿得到。
//   外部 `npm run service` 起的服务，界面只能看到帧环、看不到录制/导出状态，
//   导出按钮是灰的 —— 所以这条链路必须让界面自己起服务才测得出来。
//
// 用法：node scripts/test-export.mjs [等秒数]
//
// 做的事：
//   1. 把仓库的 profiles.json 复制一份到 <临时根>/proofs/export/，只把每路配置的
//      replaySeconds 改小（默认 20 秒），免得为了攒够分片等一分钟；
//      录像和导出目录都指向 proofs 下面，不污染用户在用的 records/exports。
//   2. 拉起 Electron（`--gui-export`），主进程会自己起采集服务、打开界面。
//   3. Electron 侧：等分片涨起来 → 通过页面真的调一次导出 → 等 export_result
//      → 校验磁盘上那个文件确实是 MP4（头部 'ftyp'）且时长 > 0。
//   4. 读 native/build/artifacts/gui-export-report.json 输出结论。
//
// 临时根默认是 %TEMP%/shadow-play/app（见 app/temp-const.cjs）。
// 退出码 0 = 通过。

import { spawn, spawnSync } from 'node:child_process'
import { existsSync, mkdirSync, readFileSync, writeFileSync, rmSync } from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { projectRoot, tempRoot, electronDistPath, assertElectronDist } from './electron-path.mjs'

const __dirname = path.dirname(fileURLToPath(import.meta.url))

const replaySeconds = Number(process.env.SHADOW_TEST_REPLAY_SECONDS ?? 20)
// 【等待时长必须大于回放窗口】以前这里直接拿命令行那个数当等待上限，于是
// `test-export.mjs 18` 在 20 秒的回放窗口下**永远录不满**：导出时回放窗口里
// 有 16 秒"还没录到"，被按设计补成 NO SIGNAL，成片是黑的却一路判"通过"。
// 现在至少等到回放窗口 + 6 秒（留出切片/落盘的余量），调用方给的数只能把它
// 调得更长，不能调短。
const requested = Number(process.argv[2]) > 0 ? Number(process.argv[2]) : 0
const waitSeconds = Math.max(requested, replaySeconds + 6)

const PROOF_DIR = path.join(tempRoot, 'proofs', 'export')
const EXPORT_DIR = path.join(PROOF_DIR, 'exports')
const PROFILES_FILE = path.join(PROOF_DIR, 'profiles.json')
const RECORD_ROOT = path.join(PROOF_DIR, 'record')
const ARTIFACT_DIR = path.join(projectRoot, 'native', 'build', 'artifacts')
const REPORT = path.join(ARTIFACT_DIR, 'gui-export-report.json')

const exe = assertElectronDist()
const backend = path.join(projectRoot, 'native', 'build', 'shadow-capture.exe')

if (!existsSync(backend)) {
  console.error(`[test-export] 找不到采集服务：${backend}\n请先执行 npm run build:native`)
  process.exit(1)
}

// 界面产物缺了的话，Electron 打开的是一个白窗口，脚本只会"等报告超时"，
// 完全看不出是 dist 没构建。这里提前拦住并说清补什么命令。
if (!existsSync(path.join(tempRoot, 'dist', 'index.html'))) {
  console.error(
    `[test-export] 找不到界面产物：${path.join(tempRoot, 'dist', 'index.html')}\n` +
      '请先执行 npm run build:web',
  )
  process.exit(1)
}

// --- 0. 先清掉可能残留的采集服务 -------------------------------------------
//
// 这一步不能省，否则验收会"看着通过、实际测的是上一次"。
//
// 界面主进程的 launchService() 是这么判断的：用 `shadow-capture --status` 探测
// **系统里有没有**采集服务在跑，有就直接复用（alreadyRunning）。它不区分那个
// 进程用的是哪份配置、录的是哪个目录 —— 对它来说"有服务"就够了。
//
// 而本脚本每次都会把 test/_export-proof/ 整个删掉重建。两件事一撞：
//   残留服务还在录**旧目录**（比如用户 `npm run service` 起的那份），
//   界面看到的分片数是旧目录的（攒够 3 段了，等待条件立刻通过），
//   点导出时服务去新目录找分片 → "分片目录里还没有可导出的完整分片"。
// 实测就是这么失败的：界面报 7 段，磁盘上只有 1 段。
//
// 所以起界面之前必须保证"没有别的采集服务"。用 SHADOW_STATUS_FILE 把状态引到
// 自己的临时目录，顺便让主进程也读同一份（它实现了同样的覆盖）。
const statusFile = path.join(PROOF_DIR, 'status', 'status-v1.json')

function killStrayServices() {
  // 只杀仓库里这一份 shadow-capture.exe：按可执行文件路径匹配，不会误伤别的程序。
  const r = spawnSync(
    'powershell',
    [
      '-NoProfile',
      '-Command',
      `Get-Process shadow-capture -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq '${backend.replace(/'/g, "''")}' } | Stop-Process -Force; exit 0`,
    ],
    { stdio: 'ignore', windowsHide: true },
  )
  return r.status === 0
}

killStrayServices()
// 进程退干净、句柄释放需要一点时间（服务还要 Finalize 当前分片）。
await new Promise((r) => setTimeout(r, 1500))
console.log('[test-export] 已清掉残留的采集服务')

// --- 1. 造一份"短回放窗口"的配置副本 -----------------------------------------
const srcFile = path.join(projectRoot, 'profiles.json')
const doc = JSON.parse(readFileSync(srcFile, 'utf8'))
doc.recordRoot = RECORD_ROOT
doc.exportDir = EXPORT_DIR
for (const p of doc.profiles ?? []) {
  if (!p.video) p.video = {}
  p.video.replaySeconds = replaySeconds
  // 分片切短一点，短测试里也能攒出好几段
  if (!(p.video.segmentSeconds > 0)) p.video.segmentSeconds = 2
  // 测试只跑十几秒，窗口源那路匹配的是"标题含 shadow-play-app"，Electron 一开
  // 就能匹配到。不强制关掉任何一路，正好顺带验证多路都在录。
}
rmSync(PROOF_DIR, { recursive: true, force: true })
mkdirSync(PROFILES_FILE ? path.dirname(PROFILES_FILE) : PROOF_DIR, { recursive: true })
mkdirSync(EXPORT_DIR, { recursive: true })
writeFileSync(PROFILES_FILE, JSON.stringify(doc, null, 2), 'utf8')
console.log(`[test-export] 配置副本：${PROFILES_FILE}（回放窗口 ${replaySeconds}s）`)
console.log(`[test-export] 导出目录：${EXPORT_DIR}`)

// --- 2. 拉起来 ---------------------------------------------------------------
try {
  rmSync(REPORT, { force: true })
} catch {
  /* 忽略 */
}
mkdirSync(ARTIFACT_DIR, { recursive: true })

const env = { ...process.env }
delete env.ELECTRON_RUN_AS_NODE
env.ELECTRON_OVERRIDE_DIST_PATH = electronDistPath
// 主进程的 profilesPath() 优先看 SHADOW_PROFILES：界面读到的配置必须和服务
// 真正加载的是同一份，否则验收测的是另一套东西。
env.SHADOW_PROFILES = PROFILES_FILE
// 状态文件也引到本脚本自己的临时目录：不然残留服务写的那份（录的是别的目录、
// 分片数对不上）会被界面读走，等待条件立刻通过而磁盘上一段都还没切出来。
env.SHADOW_STATUS_FILE = statusFile
if (!env.SHADOW_RENDER_FPS) env.SHADOW_RENDER_FPS = '60'

const child = spawn(
  exe,
  ['.', '--gui-export', String(waitSeconds), '--no-sandbox', '--disable-breakpad', '--disable-crash-reporter'],
  { cwd: projectRoot, detached: true, stdio: ['ignore', 1, 2], env },
)
child.unref()
console.log(`[test-export] 已拉起界面（pid ${child.pid}），等它录完并导出…`)

// --- 3. 等报告 ---------------------------------------------------------------
const deadline = Date.now() + (waitSeconds + 90) * 1000
let report = null
while (Date.now() < deadline) {
  if (existsSync(REPORT)) {
    try {
      const text = readFileSync(REPORT, 'utf8')
      if (text.trim().endsWith('}')) {
        report = JSON.parse(text)
        break
      }
    } catch {
      /* 还在写 */
    }
  }
  await new Promise((r) => setTimeout(r, 400))
}

if (!report) {
  console.error(`[test-export] 等报告超时：${REPORT}`)
  process.exit(1)
}

// --- 4. 结论 -----------------------------------------------------------------
console.log('\n===== 导出端到端验收 =====')
console.log(JSON.stringify(report, null, 2))

// 再独立核一次磁盘：报告是自报的，文件才是证据。
if (report.ok && report.outPath) {
  if (!existsSync(report.outPath)) {
    console.error(`[test-export] 报告说成功，但文件不在：${report.outPath}`)
    process.exit(1)
  }
  const head = readFileSync(report.outPath).subarray(0, 12).toString('latin1')
  const size = readFileSync(report.outPath).length
  console.log(`[test-export] 文件：${report.outPath}`)
  console.log(`[test-export] 大小：${size} 字节，头部：${JSON.stringify(head)}`)
  if (head.slice(4, 8) !== 'ftyp') {
    console.error('[test-export] 头部不是 MP4 的 ftyp box')
    process.exit(1)
  }
  // 文件名里的中文必须原样留下。仓库的出厂配置就带中文名（"整屏-主显示器"），
  // 而 C++ 侧一旦用 fs::path::string() 把路径转成窄字符串，中文会按 ANSI 代码页
  // 被压成一串 U+FFFD，磁盘上就会冒出 "????-????ʾ??_Replay_...mp4" 这种东西。
  // 这个断言就是那个 bug 的回归闸门 —— 别删。
  const baseName = path.basename(report.outPath)
  if (baseName.includes('\uFFFD')) {
    console.error(`[test-export] 导出文件名被破坏（出现了替换字符 U+FFFD）：${baseName}`)
    process.exit(1)
  }
  if (!/[\u4e00-\u9fa5]/.test(baseName)) {
    console.warn(`[test-export] 提示：本次配置名不含中文（${baseName}），中文路径那条回归断言没被覆盖到`)
  }
}

console.log(report.ok ? '\n结果：通过' : '\n结果：失败')
process.exit(report.ok ? 0 : 1)