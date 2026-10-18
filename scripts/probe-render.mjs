// scripts/probe-render.mjs — 找出界面那个 fps 数字背后的真实计数来源
//
// 目标：区分三种可能
//   A. 采集端产出不够（共享环 produced 涨得慢）→ 服务问题
//   B. 泵没把新帧拷上来（counter 不涨）→ preload 问题
//   C. 渲染器被限流/跳帧（skipBudget 涨）→ usePreview 问题
const DEBUG_PORT = Number(process.argv[2] || 9222)
const SECONDS = Number(process.argv[3] || 10)

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

console.log('顶栏文本:', await evaluate(`document.querySelector('header')?.innerText.replace(/\\n+/g,' | ')`))

// 每 500ms 采一次：共享环 counter + 顶栏 fps + 泵 tick
const data = await evaluate(`
new Promise((resolve) => {
  const out = [];
  const iv = setInterval(() => {
    const s = window.shadowPreview.stats();
    const b = document.querySelector('header b');
    const p = window.shadowPreview.pumpStats ? window.shadowPreview.pumpStats() : null;
    out.push({
      t: Math.round(performance.now()),
      counter: Number(s.produced),
      gapDropped: s.dropped,
      uiFps: b ? b.innerText : null,
      pumpTicks: p ? p.ticks : null,
      pumpPollMs: p ? p.pollMs : null,
    });
  }, 500);
  setTimeout(() => { clearInterval(iv); resolve(out); }, ${SECONDS * 1000});
})`)

console.log('\n时刻    环counter  帧增量  界面fps  泵ticks  泵poll累计ms')
let prev = null
for (const d of data) {
  const dc = prev ? d.counter - prev.counter : 0
  const dt = prev ? (d.t - prev.t) / 1000 : 0
  const rate = prev && dt > 0 ? (dc / dt).toFixed(1) : '-'
  console.log(
    `${String(d.t).padStart(6)}  ${String(d.counter).padStart(8)}  ${String(dc).padStart(5)} (${String(rate).padStart(5)}/s)  ${String(d.uiFps).padStart(6)}  ${String(d.pumpTicks).padStart(7)}  ${d.pumpPollMs?.toFixed(1)}`,
  )
  prev = d
}

const first = data[0], last = data[data.length - 1]
const span = (last.t - first.t) / 1000
console.log(`\n共享环产出速率：${((last.counter - first.counter) / span).toFixed(2)} fps`)
console.log(`泵速率：${((last.pumpTicks - first.pumpTicks) / span).toFixed(1)} /s`)
ws.close()
process.exit(0)
