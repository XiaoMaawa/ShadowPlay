// scripts/probe-draw.mjs — 逐轮 rAF 记录"是否新帧 / 是否画了"，算出每一帧被谁吃掉的
//
// 这是排查 46fps 的关键探针：用 monkey-patch 包住 render 流程不可行（打包后
// 是闭包），所以改成从外部复刻 usePreview 的判据，读同一份统计数据。
//
// 做法：每轮 rAF 读 syncVersion（通过 stats().counter 和 producedFrames，
// 它们随泵推进而变化），统计"新帧轮次"，同时记录页面自报 renderFps。
const DEBUG_PORT = Number(process.argv[2] || 9222)
const SECONDS = Number(process.argv[3] || 6)

const list = await (await fetch(`http://127.0.0.1:${DEBUG_PORT}/json/list`)).json()
const page = list.find((t) => t.type === 'page')
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
  if (r.result?.exceptionDetails) throw new Error(r.result.exceptionDetails.exception?.description || r.result.exceptionDetails.text)
  return r.result?.result?.value
}

const res = await evaluate(`
new Promise((resolve) => {
  const rb = window.shadowPreview.getRingBuffer();
  const view = new DataView(rb.buffer);
  // 头部布局：从 ringLayout.ts 的 RING_U64 里抄 syncVersion 偏移不现实，
  // 但 stats() 的 counter 读的就是它，用 counter 当"新帧信号"。
  let lastSync = null;
  let newTicks = 0, sameTicks = 0, ticks = 0;
  // 复刻当前限流判据：间隔 < 16.667*0.8 就跳过
  const BUDGET = 1000 / 60;
  let lastDrawAt = 0;
  let wouldDraw = 0, budgetBlocked = 0;
  let lastTs = 0;
  const t0 = performance.now();
  function step(ts) {
    ticks++;
    const s = window.shadowPreview.stats();
    const sync = s.producedFrames;
    const isNew = lastSync !== null && sync !== lastSync;
    if (isNew) newTicks++; else if (lastSync !== null) sameTicks++;
    lastSync = sync;

    if (isNew) {
      if (lastDrawAt === 0 || ts - lastDrawAt >= BUDGET * 0.8) {
        wouldDraw++;
        lastDrawAt = ts;
      } else {
        budgetBlocked++;
        // 被限流挡住的那一帧，下一轮会补画（如果那时还没新帧判据会挡）
      }
    }
    lastTs = ts;
    if (ts - t0 < ${SECONDS * 1000}) requestAnimationFrame(step);
    else resolve({ ticks, newTicks, sameTicks, wouldDraw, budgetBlocked, span: ts - t0 });
  }
  requestAnimationFrame(step);
})`)

const secs = res.span / 1000
console.log(`rAF         ${res.ticks} 轮 / ${secs.toFixed(1)}s = ${(res.ticks / secs).toFixed(0)} Hz`)
console.log(`新帧轮次    ${res.newTicks}（${(res.newTicks / secs).toFixed(1)}/s）`)
console.log(`同帧轮次    ${res.sameTicks}（${(res.sameTicks / secs).toFixed(1)}/s）`)
console.log(`\n按当前限流判据（80% 预算）：`)
console.log(`  会画     ${res.wouldDraw}（${(res.wouldDraw / secs).toFixed(1)}/s）`)
console.log(`  被预算挡 ${res.budgetBlocked}（${(res.budgetBlocked / secs).toFixed(1)}/s）`)
console.log(`\n页面自报 renderFps:`, await evaluate(`document.querySelector('header b')?.innerText`))
ws.close()
process.exit(0)
