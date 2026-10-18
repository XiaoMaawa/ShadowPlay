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
//     bin/         采集服务的运行副本（见下方 ensureServiceExe 的说明）
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
  /** 采集服务的运行副本（见 ensureServiceExe） */
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
 * 返回「该运行的那一份采集服务 exe」。
 *
 * 为什么不直接跑 native/build/shadow-capture.exe：**Windows 的完整性级别是
 * 从文件继承的**。仓库目录如果带了 `Mandatory Label\Low Mandatory Level`
 * （DSH 沙箱就会打这个标签，同步盘/某些杀毒软件也会），构建出来的 exe 会
 * 继承那个标签，于是服务进程以 Low 完整性级别运行，两个后果都很难看：
 *
 *   1. WGC 窗口采集被系统拒绝：`0x80070005`「进程完整性级别低于 Medium」；
 *   2. 往 Medium 目录写文件被拒：`建不了分配记录目录：Access is denied.`
 *      —— 临时目录（%TEMP%）正是 Medium 的。
 *
 * 而继承来的标签**改不掉**：`icacls /setintegritylevel Medium` 会报
 * `Access is denied`（继承 ACE 不是自己的，当前用户又没有 WRITE_OWNER）。
 *
 * 所以换个思路：把 exe **复制到临时根**再跑。副本是在 %TEMP% 下新建的文件，
 * 继承的是临时目录的标签（Medium），一复制问题就没了。成本是一次几十 MB
 * 的拷贝，所以带缓存：源文件大小 + 修改时间都没变就直接复用上次的副本。
 *
 * 在没打标签的机器上这等于多一次无害的复制（首次），之后完全走缓存。
 *
 * @param {{srcExe: string, tempRoot?: string, log?: (msg: string) => void}} opts
 * @returns {string} 该 spawn 的 exe 路径（源文件不存在时原样返回，让调用方报错）
 */
function ensureServiceExe(opts) {
  const fs = require('node:fs')
  const srcExe = opts.srcExe
  const tempRoot = opts.tempRoot || TEMP_ROOT
  const log = opts.log || (() => {})

  if (!fs.existsSync(srcExe)) return srcExe

  const binDir = path.join(tempRoot, 'bin')
  const dstExe = path.join(binDir, path.basename(srcExe))
  const stampPath = path.join(binDir, '.stamp.json')

  let srcStat
  try {
    srcStat = fs.statSync(srcExe)
  } catch {
    return srcExe
  }
  const want = `${srcStat.size}:${srcStat.mtimeMs}`

  // 缓存命中：副本在，且记录的大小/时间与源文件一致
  try {
    const got = JSON.parse(fs.readFileSync(stampPath, 'utf8'))
    if (got && got.stamp === want && fs.existsSync(dstExe) && fs.statSync(dstExe).size === srcStat.size) {
      return dstExe
    }
  } catch {
    /* 没有缓存，下面复制 */
  }

  try {
    fs.mkdirSync(binDir, { recursive: true })
    // 目标可能正被上一次的服务进程占着（Windows 上运行中的 exe 不能覆盖），
    // 那就先删；删不掉说明真的在用，直接用现有的那份。
    try {
      if (fs.existsSync(dstExe)) fs.unlinkSync(dstExe)
    } catch {
      return dstExe
    }
    fs.copyFileSync(srcExe, dstExe)
    fs.writeFileSync(stampPath, JSON.stringify({ stamp: want, src: srcExe }), 'utf8')
    log(`已把采集服务复制到临时目录：${dstExe}`)
    return dstExe
  } catch (err) {
    // 复制失败（权限、占用）就退回原件：打不了标签的机器上原件本来就能跑
    log(`复制采集服务失败（${err.message}），直接使用 ${srcExe}`)
    return srcExe
  }
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
