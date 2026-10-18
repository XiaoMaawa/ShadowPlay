// scripts/test-reload.mjs — 验证"保存只重载改动的那一路，且绝不重启服务"
//
// 判据（全部是外部可观测的事实，不看日志说了什么）：
//   1. 保存后服务的 pid 不变 —— 这是最硬的一条：用户明确要求任何功能都不许重启服务。
//   2. 目标那一路的帧率当场就换成新值（gapMs 跟着变，比如 120fps→8.3ms、60fps→16.7ms）。
//   3. 其它路的帧率 / 计时一拍都没变（没被牵连）。
//
// 为什么不再用 producedFrames 判"有没有重载"：那是环形计数器，会话重建会建一个新环，
// 读到的值既可能归零也可能接着涨，据此判断会得出"未重载"的错误结论（本鱼踩过）。
// 帧率和 gapMs 是订阅了配置并且每秒重算的，改没改一眼就能看出来。
//
// 【两个踩过的坑，别再犯】
//
// ① 被测对象必须**写死**是桌面那一路。
//    早先的版本写 `Object.keys(before).find(k => before[k].target > 0)` —— 状态文件里
//    items 的顺序不固定，碰巧排第一的是 30fps 的窗口那一路时，脚本就把它 30 改成 60，
//    用户看到的是"我只改了一路，隔壁 30fps 怎么变 60 了"。被测对象不能靠运气选。
//
// ② 采样前必须等每一路都进入**稳定状态**。
//    服务刚起来时 window 那一路先是 waiting（WGC 正在找窗口），一两秒后才变 live。
//    如果保存前采到 waiting、保存后采到 live，判据会得出"被牵连了"——实际上那一路
//    只是刚起来。所以下面先用 waitStable() 等齐，再开始测量。
//
// 跑完会把目标路的帧率改回原值，不给用户留烂摊子。
const DEBUG_PORT = Number(process.argv[2] || 9222)
const TARGET_ID = 'p-desk'

const { execFileSync } = await import('node:child_process')
const { readFileSync } = await import('node:fs')
const path = await import('node:path')
const os = await import('node:os')

const STATUS = path.join(os.tmpdir(), 'shadow-play', 'app', 'status', 'status-v1.json')

function servicePid() {
  try {
    const out = execFileSync('powershell', ['-NoProfile', '-Command',
      "(Get-Process shadow-capture -ErrorAction SilentlyContinue | Select-Object -First 1).Id"], {
      encoding: 'utf8', windowsHide: true,
    })
    return out.trim()
  } catch { return '' }
}

/** 直接读服务写的状态文件：里面每路都有 targetFps / actualFps / gapMs。 */
function statusSnap() {
  const j = JSON.parse(readFileSync(STATUS, 'utf8'))
  const out = {}
  for (const it of j.items ?? []) {
    out[it.id] = { target: it.targetFps, actual: it.actualFps, gap: it.gapMs, state: it.state }
  }
  return out
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms))

/**
 * 等到每一路都不再是"正在起"的状态。
 *
 * 认作"正在起"的：state === 'waiting'，或者 state === 'live' 但 actual 还是 0
 * （会话建好了、还没采到第一帧）。这两件事都会在一两秒内自己过去，属于噪声。
 * disabled / unavailable 是稳定态，等不到也没关系 —— 它们不会因为保存而动。
 */
async function waitStable(label) {
  const deadline = Date.now() + 12000
  let last = null
  while (Date.now() < deadline) {
    const snap = statusSnap()
    last = snap
    const unstable = Object.entries(snap).filter(([, v]) => v.state === 'waiting' || (v.state === 'live' && !(v.actual > 0)))
    if (unstable.length === 0) return snap
    await sleep(400)
  }
  console.log(`（等 ${label} 稳定超时，用最后一份快照继续）`)
  return last ?? statusSnap()
}

/**
 * 等目标那一路真的换上新的目标帧率。
 *
 * 【为什么不能只等"稳定"就采样】
 * 状态文件是服务**每秒写一次**的。保存返回的那一刻，服务可能连新会话都刚建好，
 * 状态文件里还是上一拍的旧数字。早先的版本保存后只等"没有 waiting"，而实测
 * 那时每一路本来就已经是 live，于是立刻采样、读到的全是旧值，判出"帧率没生效"
 * 的假失败。这里改成盯住要变的那个字段本身。
 */
async function waitTarget(id, want, label) {
  const deadline = Date.now() + 12000
  let last = null
  while (Date.now() < deadline) {
    const snap = statusSnap()
    last = snap
    if (Math.round(snap[id]?.target ?? -1) === want) return snap
    await sleep(300)
  }
  console.log(`（等 ${label} 变成 ${want} 超时，用最后一份快照继续）`)
  return last ?? statusSnap()
}

const pidBefore = servicePid()
const before = await waitStable('保存前')
console.log(`保存前服务 pid = ${pidBefore}`)
console.log('保存前:', JSON.stringify(before))

if (!before[TARGET_ID]) {
  console.log(`状态文件里没有 ${TARGET_ID} 这一路，无法测试`)
  process.exit(1)
}
const cur = Math.round(before[TARGET_ID]?.target ?? 60)
// 在 60 和 120 之间来回切，保证每次运行都真的改了东西
const toggle = cur === 60 ? 120 : 60

// 直接走界面那条单路保存通道（preload.saveProfile），和用户点保存完全同一条路。
const list = await (await fetch(`http://127.0.0.1:${DEBUG_PORT}/json/list`)).json()
const page = list.find((t) => t.type === 'page')
if (!page) {
  console.log('找不到 Electron 页面，无法通过界面通道保存')
  process.exit(1)
}
const ws = new WebSocket(page.webSocketDebuggerUrl)
let nextId = 1
const pending = new Map()
ws.addEventListener('message', (ev) => {
  const msg = JSON.parse(ev.data)
  if (msg.id && pending.has(msg.id)) { pending.get(msg.id)(msg); pending.delete(msg.id) }
})
await new Promise((r) => ws.addEventListener('open', r))
function send(method, params = {}) {
  const id = nextId++
  return new Promise((resolve) => { pending.set(id, resolve); ws.send(JSON.stringify({ id, method, params })) })
}
async function evaluate(expression) {
  const r = await send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true })
  if (r.result?.exceptionDetails) {
    throw new Error(r.result.exceptionDetails.exception?.description || r.result.exceptionDetails.text)
  }
  return r.result?.result?.value
}

async function saveFps(fps) {
  return await evaluate(`
(async () => {
  const doc = await window.shadowPreview.serviceProfiles();
  const items = doc.items ?? [];
  const target = items.find((p) => p.id === ${JSON.stringify(TARGET_ID)});
  if (!target) return { error: '找不到目标配置' };
  const patch = { ...target, video: { ...target.video, fps: ${fps} } };
  return await window.shadowPreview.saveProfile(${JSON.stringify(TARGET_ID)}, patch);
})()`)
}

const res = await saveFps(toggle)
console.log(`\n把 ${TARGET_ID} 的帧率改成 ${toggle}`)
console.log('保存返回:', JSON.stringify(res))

const after = await waitTarget(TARGET_ID, toggle, '目标帧率')
const pidAfter = servicePid()
console.log(`保存后服务 pid = ${pidAfter}`)
console.log('保存后:', JSON.stringify(after))

console.log('\n=== 判定 ===')
let ok = true
const pidSame = pidBefore === pidAfter && pidBefore !== ''
console.log(`${pidSame ? '通过' : '失败'}：服务 pid ${pidSame ? '未变（没有重启）' : '变了（偷偷重启了服务！）'}`)
ok &&= pidSame

const t = after[TARGET_ID]
const gotFps = Math.round(t?.target ?? 0)
const fpsOk = gotFps === toggle
console.log(`${fpsOk ? '通过' : '失败'}：${TARGET_ID} 的目标帧率 ${cur} → ${gotFps}（期望 ${toggle}），实测 ${t?.actual}，每拍 ${t?.gap}ms`)
ok &&= fpsOk

for (const id of Object.keys(before)) {
  if (id === TARGET_ID) continue
  const b = before[id], a = after[id]
  if (!a) { console.log(`  ${id}: 保存后消失了`); ok = false; continue }
  // 只比"配置有没有被别人动过"：目标帧率 + 是否启用。
  // 实测 fps 会呼吸（这一拍没采到新帧就掉一点），拿它当判据只会误报。
  const sameTarget = Math.round(a.target) === Math.round(b.target)
  const sameState = a.state === b.state
  const affected = !sameTarget || !sameState
  const why = !sameTarget
    ? `目标帧率被改了（${b.target} → ${a.target}）`
    : !sameState
      ? `状态被改了（${b.state} → ${a.state}）`
      : '未受影响'
  console.log(`  ${id}: 目标 ${b.target} → ${a.target}，状态 ${b.state} → ${a.state}，实测 ${b.actual} → ${a.actual}  ${affected ? `被牵连了！${why}` : why}`)
  ok &&= !affected
}

// 把目标路改回原值，别给用户留烂摊子。
console.log(`\n恢复 ${TARGET_ID} 的帧率到 ${cur}`)
const back = await saveFps(cur)
console.log('恢复返回:', JSON.stringify(back))
const finalSnap = await waitTarget(TARGET_ID, cur, '恢复后的目标帧率')
const restored = Math.round(finalSnap[TARGET_ID]?.target ?? 0) === cur
console.log(`${restored ? '通过' : '失败'}：${TARGET_ID} 的帧率已恢复成 ${finalSnap[TARGET_ID]?.target}`)
ok &&= restored

console.log(`\n结果：${ok ? '通过' : '失败'}`)
ws.close()
process.exit(ok ? 0 : 1)
