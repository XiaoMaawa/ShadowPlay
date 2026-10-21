// app/temp-const.cjs — 临时目录约定（主进程 / 渲染进程脚本 / 构建脚本共用一个来源）
//
// 为什么要有这个文件：录制分片、导出的 mp4、验证产物以前散在仓库根目录的
// test/、test-multi/、exports/、dist/ 里，跑几次就多出几百 MB，跟源码混在
// 一起，`git status` 和清理都很难受。
//
// 现在**所有临时产物一律进系统临时目录**，仓库里一个字节都不留：
//
//   %TEMP%/shadow-play/app/           ← 根（每次启动先整个清掉）
//     dist/        前端构建产物（Electron 从这里 loadFile index.html）
//     bin/         （已废弃：以前放采集服务的运行副本，现在直接跑编译产物）
//     records/     采集服务的录制分片（生产默认落点）
//     proofs/      验证脚本的产物（录制/导出/自检/探针的证据目录）
//     status/      服务状态镜像
//     logs/        服务日志、诊断日志
//     userdata/    Electron 的 userData（缓存、DevTools 数据）
//
// **导出的 mp4 不在这里**：分片是滚动覆盖的中间产物，跑完就该扔；导出的
// mp4 是用户要拿走的成品，放临时目录等于随时可能被清理、也不好找。它落在
// 仓库根的 export/（见 app/main.cjs 的 EXPORT_DIR）。
//
// 为什么不放仓库里：仓库可能被同步盘、杀毒、DSH 沙箱盯着，写文件既慢又会
// 给目录打上奇怪的安全标签（本鱼就被 Low 完整性标签坑过一次，WGC 直接
// 0x80070005）。系统 temp 是专门给这种东西用的地方。
//
// 清理策略（见 app/temp-clean.cjs）：**每次启动整个删掉重建**。临时产物
// 的寿命本来就该是"一次运行"，跨运行复用只会带来"看到上一次的旧录像"这种
// 误判。唯一例外是 proofs/ —— 验证脚本要在启动之前往里面写配置，删了会
// 让脚本刚写的配置凭空消失，所以它只在构建开始前清。
const os = require('node:os')
const path = require('node:path')

const PROJECT_ROOT = path.join(__dirname, '..')

// 允许用环境变量指到别处（测试/绿色版/WSL 挂载盘之类的场景）。
const TEMP_ROOT =
  process.env.SHADOW_TEMP_ROOT || path.join(os.tmpdir(), 'shadow-play', 'app')

const TEMP = {
  root: TEMP_ROOT,
  /** 前端构建产物（app/main.cjs 按这个路径 loadFile） */
  dist: path.join(TEMP_ROOT, 'dist'),
  /** （已废弃，保留只是为了老脚本不报错：采集服务现在直接跑编译产物） */
  bin: path.join(TEMP_ROOT, 'bin'),
  /** 采集服务的录制分片默认落点 */
  records: path.join(TEMP_ROOT, 'records'),
  /** 验证脚本的产物（录制/导出/自检/探针） */
  proofs: path.join(TEMP_ROOT, 'proofs'),
  /** 服务状态镜像 */
  status: path.join(TEMP_ROOT, 'status'),
  /** 日志 */
  logs: path.join(TEMP_ROOT, 'logs'),
  /** Electron 的 userData（缓存、DevTools 数据，删掉能重建） */
  userdata: path.join(TEMP_ROOT, 'userdata'),
}

/** 建出某个子目录（配套递归创建，已存在不报错） */
function ensureDir(dir, fs = require('node:fs')) {
  fs.mkdirSync(dir, { recursive: true })
  return dir
}

/**
 * 返回「该运行的那一份采集服务 exe」：**就是编译产物本身**，原样返回。
 *
 * 【这里以前会把 exe 复制到临时根再跑 —— 那条路已经废掉，别加回来】
 * 旧实现的理由是"仓库目录可能带低完整性标签（DSH 沙箱、同步盘会打），
 * 从那里起的进程会被 WGC 拒绝，复制到 %TEMP% 就没有那个标签"。收益没兑现，
 * 代价倒是实打实的两个：
 *
 *   1. 副本成了第二个真实存在的可执行文件，"服务在用哪一份"变成新的状态。
 *      启动清理把 bin/ 删掉后，SERVICE_EXE 永久指向一个不存在的路径：探测
 *      不到 → 自动启动 → 还是那个不存在的路径 → 起不来。界面就一直显示
 *      「正在连接…」，任务管理器里也没有采集服务。
 *   2. 服务已经在跑时副本不能刷新（换文件等于弄死正在跑的进程），于是重新
 *      编译出来的 exe 在老服务活着时永远用不上，排查"改了没生效"全卡在这。
 *
 * 只跑一份编译产物，两类问题一起消失：路径恒定、重编译后重启服务必生效。
 *
 * 函数保留（签名不变）只是为了让既有调用方不必改：它现在就是个恒等函数。
 *
 * @param {{srcExe: string, tempRoot?: string, log?: (msg: string) => void}} opts
 * @returns {string} 该 spawn 的 exe 路径
 */
function ensureServiceExe(opts) {
  return opts.srcExe
}

/**
 * 扫一遍 %TEMP%/shadow-play/ 下的各个实例根目录，把「拥有者进程已经不在」的删掉。
 *
 * 每个实例根目录里有一份 `.owner`（JSON：{ owner, pid, startedAt }），由主进程
 * 在启动时写下。读到 pid 之后用 `process.kill(pid, 0)` 判断它还在不在：
 * 抛 ESRCH 就是没了，这一整个根目录可以整棵删。
 *
 * 为什么要这一步：临时产物本是「一次运行一份」，但被强杀 / 断电 / 崩溃的运行
 * 不会自己收尾，它们的根目录会一直躺在 %TEMP% 里，时间一久就是几十个 GB。
 *
 * 保守规则（宁可不删也不误删）：
 *   · 没有 `.owner` 的目录不删 —— 可能是别的工具碰巧用了同一个名字；
 *   · pid 正活着就不删 —— 那是另一份正在跑的实例；
 *   · 判断进程时排除 pid 0/1/-1 这种「发送信号有特殊含义」的值；
 *   · 删除失败（文件被占）就跳过，下次再说。
 *
 * @param {{log?: (msg: string) => void, keep?: string}} [opts]
 * @returns {{removed: string[], skipped: string[]}}
 */
function sweepStaleRoots(opts = {}) {
  const fs = require('node:fs')
  const log = opts.log || (() => {})
  const base = path.dirname(TEMP_ROOT)
  const keep = path.resolve(opts.keep || TEMP_ROOT)
  const removed = []
  const skipped = []

  let entries = []
  try {
    entries = fs.readdirSync(base, { withFileTypes: true })
  } catch {
    return { removed, skipped }
  }

  for (const ent of entries) {
    if (!ent.isDirectory()) continue
    const full = path.join(base, ent.name)
    if (path.resolve(full) === keep) continue

    let owner = null
    try {
      owner = JSON.parse(fs.readFileSync(path.join(full, '.owner'), 'utf8'))
    } catch {
      // 没有标记 —— 不是我们体系里的目录，不动
      skipped.push(full)
      continue
    }

    const pid = Number(owner?.pid)
    if (!Number.isInteger(pid) || pid <= 1) {
      skipped.push(full)
      continue
    }
    let alive = true
    try {
      process.kill(pid, 0)
    } catch (err) {
      // ESRCH = 没有这个进程；EPERM = 进程在但没权限（当成活着）
      alive = err && err.code === 'EPERM'
    }
    if (alive) {
      skipped.push(full)
      continue
    }
    try {
      fs.rmSync(full, { recursive: true, force: true, maxRetries: 3, retryDelay: 80 })
      removed.push(full)
    } catch {
      skipped.push(full)
    }
  }

  if (removed.length) log(`已清掉 ${removed.length} 个上次运行残留的临时目录`)
  return { removed, skipped }
}

/** 在本次运行的根目录写下拥有者标记（供下一次启动时判断死活） */
function markOwner(dir = TEMP_ROOT, owner = 'app') {
  const fs = require('node:fs')
  try {
    fs.mkdirSync(dir, { recursive: true })
    fs.writeFileSync(
      path.join(dir, '.owner'),
      JSON.stringify({ owner, pid: process.pid, startedAt: new Date().toISOString() }),
      'utf8',
    )
  } catch {
    /* 写不了就算了，最多是这次运行结束时没人帮它收尾 */
  }
}

module.exports = {
  PROJECT_ROOT,
  TEMP_ROOT,
  TEMP,
  ensureDir,
  ensureServiceExe,
  sweepStaleRoots,
  markOwner,
}
