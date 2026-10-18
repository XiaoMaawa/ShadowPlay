// app/temp-clean.cjs — 临时目录的清理策略
//
// 规矩只有一条：**临时产物没有在两次运行之间活下来的权利。**
//
// 具体做两件事：
//   1. purgeRunArtifacts()  启动时清掉上一次运行留下的产物（运行产物，删）
//   2. ensureLayout()  把骨架目录补回来
//
// 但这里有一条重要的分界：**运行产物**和**构建产物**不是一回事。
//
//   records/ exports/ status/ logs/ userdata/   ← 运行产物，每次启动整个删
//   proofs/ 下各脚本的子目录                    ← 运行产物，每次启动删
//   proofs/ 下的配置文件                        ← 脚本预先写的，留着
//   dist/                                       ← **构建产物**，绝不能删
//
// dist 被删过一次，后果很直接：界面 loadFile 找不到 index.html 就退回连
// Vite 开发服务器，开发服务器没开，于是整块白屏 + ERR_CONNECTION_REFUSED
// —— 看起来像"改坏了一堆东西"，实际只是清理逻辑多删了一个目录。
//
// ---- records/ 为什么不能无条件删（踩过的大坑）----
//
// 采集服务现在是**独立进程**，关掉界面它也不停（用户明确要求）。
// 于是"启动界面"和"服务正在录制"完全可能同时成立 —— 这时把 records/ 整棵
// 删掉，ffmpeg 手里那个句柄指向的文件就没了：它要么写失败、要么 muxer 找
// 不回分片，最后在界面上表现为「ffmpeg 提前退出（分片写入中断）」。查了很
// 久才发现根因是清理逻辑自己把正在录的东西删了。
//
// 所以 records/ 改成**看服务活没活着**再决定：服务在跑就一个字节都不动。
// 真正的孤儿录像由临时根里的 .owner 标记兜底（sweepStaleRoots 会整棵删掉
// 拥有者已消失的旧根）。
//
// 为什么其余目录这么激进：它们里面全是"这一次运行产生的垃圾"。留着它们的
// 唯一后果是误判 —— 上次的录像被当成这次的、旧状态文件让界面显示"正在
// 录制"、半截 mp4 让导出报错。宁可每次多花几毫秒删一遍。
const { rmSync, mkdirSync, readdirSync, statSync } = require('node:fs')
const path = require('node:path')

/** proofs 里这些子目录是验证脚本的产物，启动时可以安全清掉 */
const EPHEMERAL_IN_PROOFS = ['recording', 'export', 'selfheal', 'reload', 'hang', 'pipe', 'debug']

/** 临时根下这些是**构建产物**，启动清理时必须原样留着 */
const KEEP_IN_ROOT = ['dist']

/**
 * 这些目录里有**活着的进程正在写**，启动清理默认不动。
 *
 * records/ 由采集服务的 ffmpeg 写；服务独立运行，界面重启时它常常还在录。
 * 调用方可以通过 opts.keepLive 显式覆盖（服务确实没跑时才清）。
 */
const LIVE_WRITTEN = ['records']

/**
 * 这些子目录里，**顶层文件**要留着，只删子目录。
 *
 * 为什么：验证脚本（scripts/test-export.mjs 之类）会先把 profiles.json、
 * 报告文件写进 proofs/<名字>/，再拉起界面。如果启动清理把这个目录整个删掉，
 * 脚本刚写的配置就凭空消失，界面读到的是仓库里那份 profiles.json，表现得
 * 像"配置没生效 / 一路都没录"。所以这些目录本身保留，只清它们下面的产物目录。
 *
 * 反过来，EPHEMERAL_IN_PROOFS 里不在这个清单中的（比如 recording），
 * 里面没有脚本预先要写的文件，整棵删掉最干净。
 */
const KEEP_TOP_FILES_IN_PROOFS = ['export', 'selfheal', 'reload', 'pipe']

function rmQuiet(target) {
  try {
    rmSync(target, { recursive: true, force: true, maxRetries: 5, retryDelay: 60 })
    return true
  } catch {
    // 文件被占着（服务还在跑、ffmpeg 还没退）就跳过，下次再说。
    return false
  }
}

/**
 * 清掉一次运行留下的全部临时产物，但保留 proofs/ 下脚本预先写好的配置。
 *
 * @param {string} tempRoot 临时根目录
 * @param {{keepLive?: boolean}} [opts] keepLive=true 表示"采集服务还在跑"，
 *        此时 records/ 一个字节都不动（见文件头那段说明）。
 * @returns {{removed: string[], kept: string[]}}
 */
function purgeRunArtifacts(tempRoot, opts = {}) {
  const removed = []
  const kept = []
  const liveDirs = opts.keepLive === false ? [] : LIVE_WRITTEN
  let entries = []
  try {
    entries = readdirSync(tempRoot)
  } catch {
    // 根目录还不存在，没什么可清的
    return { removed, kept }
  }
  for (const name of entries) {
    const full = path.join(tempRoot, name)
    if (KEEP_IN_ROOT.includes(name)) {
      // 构建产物：不是"这一次运行留下的"，删了界面就没了
      kept.push(name)
      continue
    }
    if (liveDirs.includes(name)) {
      // 有活着的进程在写（采集服务的 ffmpeg），删了就是「分片写入中断」
      kept.push(name)
      continue
    }
    if (name === 'proofs') {
      // proofs 逐个子目录清，配置类文件留着
      let proofsEntries = []
      try {
        proofsEntries = readdirSync(full)
      } catch {
        continue
      }
      for (const sub of proofsEntries) {
        if (!EPHEMERAL_IN_PROOFS.includes(sub)) {
          kept.push(path.join('proofs', sub))
          continue
        }
        const subFull = path.join(full, sub)
        // 只删目录型的产物，顺手的临时文件不管
        try {
          if (!statSync(subFull).isDirectory()) continue
        } catch {
          continue
        }
        // 这个子目录里可能有脚本预先写好的配置/报告，逐项清，别整棵删
        if (KEEP_TOP_FILES_IN_PROOFS.includes(sub)) {
          let inner = []
          try {
            inner = readdirSync(subFull)
          } catch {
            continue
          }
          for (const name of inner) {
            const target = path.join(subFull, name)
            try {
              if (!statSync(target).isDirectory()) {
                kept.push(path.join('proofs', sub, name))
                continue
              }
            } catch {
              continue
            }
            if (rmQuiet(target)) removed.push(path.join('proofs', sub, name))
          }
          continue
        }
        if (rmQuiet(subFull)) removed.push(path.join('proofs', sub))
      }
      continue
    }
    if (rmQuiet(full)) removed.push(name)
    else kept.push(name)
  }
  return { removed, kept }
}

/** 把临时根和它的标准子目录建出来 */
function ensureLayout(tempRoot) {
  for (const sub of ['dist', 'records', 'exports', 'proofs', 'status', 'logs']) {
    mkdirSync(path.join(tempRoot, sub), { recursive: true })
  }
}

/**
 * 启动前的清理：删掉上一次运行的运行产物，然后补回目录骨架。
 *
 * dist/ 不动（构建产物），proofs/ 下的配置不动（脚本预先写的），
 * records/ 在采集服务活着时也不动（它正在写）。
 *
 * @param {string} tempRoot 临时根目录
 * @param {{log?: (msg: string) => void, keepLive?: boolean}} [opts]
 *        keepLive 默认 true：保守起见把 records/ 当作"可能正在被写"。
 */
function cleanOnStartup(tempRoot, opts = {}) {
  const log = opts.log || (() => {})
  const { removed, kept } = purgeRunArtifacts(tempRoot, { keepLive: opts.keepLive })
  ensureLayout(tempRoot)
  if (removed.length) log(`已清理上次运行的临时产物：${removed.length} 项`)
  const noteable = kept.filter((k) => !k.startsWith('dist'))
  if (noteable.length) log(`保留：${noteable.join(', ')}`)
  return { removed, kept }
}

module.exports = { cleanOnStartup, purgeRunArtifacts, ensureLayout }

// 允许直接 `node app/temp-clean.cjs` 手动清一次（排查时用得上）。
if (require.main === module) {
  const { TEMP_ROOT } = require('./temp-const.cjs')
  // 手动跑时也保守：不动 records/，除非显式 `--purge-records`
  const keepLive = !process.argv.includes('--purge-records')
  const r = cleanOnStartup(TEMP_ROOT, { log: (m) => console.log('[temp-clean]', m), keepLive })
  console.log(`[temp-clean] ${TEMP_ROOT}  删除 ${r.removed.length} 项`)
}
