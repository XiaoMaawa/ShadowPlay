// scripts/probe-ring.mjs — 逐拍读 syncVersion，看新帧到底什么时候到
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

// 每轮 rAF 读一次 syncVersion，统计"这一轮有没有新帧"
const res = await evaluate(`
new Promise((resolve) => {
  const rb = window.shadowPreview.getRingBuffer();
  if (!rb) return resolve({ error: '拿不到 ring buffer' });
  const view = new DataView(rb.buffer);
  // syncVersion 的偏移：从 ringLayout 里查，这里直接扫一圈找不现实，
  // 改用 stats() 的 counter（它读的是同一个位置）
  const samples = [];
  let lastCounter = null;
  let newFrameTicks = 0;
  let sameTicks = 0;
  let ticks = 0;
  const gaps = [];
  let lastTs = 0;
  const t0 = performance.now();
  function step(ts) {
    ticks++;
    const s = window.shadowPreview.stats();
    const c = s.producedFrames;
    if (lastCounter !== null) {
      if (c !== lastCounter) newFrameTicks++; else sameTicks++;
    }
    lastCounter = c;
    if (lastTs) gaps.push(+(ts - lastTs).toFixed(2));
    lastTs = ts;
    if (ts - t0 < ${SECONDS * 1000}) requestAnimationFrame(step);
    else resolve({ ticks, newFrameTicks, sameTicks, span: ts - t0, gaps: gaps.slice(0, 40) });
  }
  requestAnimationFrame(step);
})`)

if (res.error) { console.log(res.error); process.exit(0) }
const secs = res.span / 1000
console.log(`rAF ${res.ticks} 轮 / ${secs.toFixed(1)}s = ${(res.ticks / secs).toFixed(0)} Hz`)
console.log(`有新帧的轮次 ${res.newFrameTicks}（${(res.newFrameTicks / secs).toFixed(1)}/s）`)
console.log(`同帧的轮次     ${res.sameTicks}（${(res.sameTicks / secs).toFixed(1)}/s）`)
console.log(`\n前 40 个 rAF 间隔:`, res.gaps.join(' '))
ws.close()
process.exit(0)
