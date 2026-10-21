// app/guardian.cjs — 读采集服务守护进程的心跳（**只读，不拉起**）
//
// 守护进程是采集服务自己拉起来的（shadow-capture --guard，同一个 exe 的
// 另一个模式，见 native/backend/GuardianMode.h）：
//
//   服务启动 ──拉起──▶ 守护
//   守护     ──盯着──▶ 服务（不在了就重新拉起）
//   服务     ──盯着──▶ 守护（心跳没了就重新拉起）
//
// 两条边都在服务端，所以关掉 Electron 之后看门照样成立 —— 这正是"守护不要
// 写在 Electron 里"的原因：界面关掉恰恰是最需要有人看着服务的时刻。
//
// 这里只做一件事：把守护的心跳读出来给界面显示（顶栏"守护在岗/未运行"）。
// 界面不参与守护的生死，也拿不到它 —— 它归服务管。
const path = require('node:path')
const { existsSync, readFileSync, statSync } = require('node:fs')

const { TEMP_ROOT } = require('./temp-const.cjs')

/** 守护心跳（C++ 侧 GuardianMode.cpp 写，和状态文件同目录） */
const HEARTBEAT = path.join(TEMP_ROOT, 'status', 'guardian-v1.json')

/** 心跳多久算过期：守护 2 秒写一次，给 6 倍余量，卡一下不至于误判 */
const HB_STALE_MS = 12000

/** 某个 pid 现在还活着吗（信号 0 只做存在性检查） */
function pidAlive(pid) {
  if (!Number.isFinite(pid) || pid <= 0) return false
  try {
    process.kill(pid, 0)
    return true
  } catch (e) {
    return Boolean(e) && e.code === 'EPERM'
  }
}

/**
 * 读心跳。
 *
 * 判据是「文件新鲜 **且** 里面记的 pid 还活着」：只看文件时间的话，守护被
 * 强杀之后它的心跳仍然"新鲜"；只看 pid 的话，pid 被系统回收复用了会误判。
 */
function readHeartbeat() {
  try {
    const st = statSync(HEARTBEAT)
    const doc = JSON.parse(readFileSync(HEARTBEAT, 'utf8'))
    const age = Date.now() - st.mtimeMs
    return { ok: true, alive: age < HB_STALE_MS && pidAlive(Number(doc?.pid)), age, doc }
  } catch {
    return { ok: false, alive: false, age: Infinity, doc: null }
  }
}

/** 快照形式的守护状态，直接塞进 service:live / service:info 的返回里。 */
function guardianSnapshot() {
  const hb = readHeartbeat()
  return {
    alive: hb.alive,
    pid: Number(hb.doc?.pid) || null,
    restarts: Number(hb.doc?.restarts) || 0,
    lastError: hb.doc?.lastError ?? '',
    lastRestartAt: hb.doc?.lastRestartAt ?? null,
  }
}

module.exports = { HEARTBEAT, readHeartbeat, guardianSnapshot }
