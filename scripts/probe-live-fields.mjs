// 直接问主进程 service:live 到底返回了什么，定位"服务未启动"误报的字段。
//
// 用法：node scripts/probe-live-fields.mjs
//
// 走 CDP 在**页面**里 require 不到主进程，所以只能读 preload 暴露的 API。

const PORT = process.env.CDP_PORT || '9222'

const r = await fetch(`http://127.0.0.1:${PORT}/json/list`)
const list = await r.json()
const t = list.find((x) => x.type === 'page')
if (!t) {
  console.error('找不到页面目标')
  process.exit(1)
}

const ws = new WebSocket(t.webSocketDebuggerUrl)
let id = 0
const pending = new Map()
ws.addEventListener('message', (ev) => {
  const m = JSON.parse(ev.data)
  if (m.id && pending.has(m.id)) {
    const { resolve, reject } = pending.get(m.id)
    pending.delete(m.id)
    if (m.error) reject(new Error(JSON.stringify(m.error)))
    else resolve(m.result)
  }
})
await new Promise((res, rej) => {
  ws.addEventListener('open', res)
  ws.addEventListener('error', () => rej(new Error('ws error')))
})

function send(method, params = {}) {
  const n = ++id
  return new Promise((resolve, reject) => {
    pending.set(n, { resolve, reject })
    ws.send(JSON.stringify({ id: n, method, params }))
  })
}

async function evalJs(expression) {
  const res = await send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true })
  if (res.exceptionDetails) throw new Error(JSON.stringify(res.exceptionDetails))
  return res.result.value
}

await send('Runtime.enable')

// 1) 页面拿得到的 API 有哪些
const api = await evalJs(`Object.keys(window.shadowPreview || {})`)
console.log('shadowPreview keys:', api)

// 2) 逐条问主进程
const live = await evalJs(`window.shadowPreview.serviceLive ? window.shadowPreview.serviceLive() : 'no-api'`)
console.log('serviceLive:', JSON.stringify(live, null, 0)?.slice(0, 1200))

const info = await evalJs(`window.shadowPreview.serviceInfo ? window.shadowPreview.serviceInfo() : 'no-api'`)
console.log('serviceInfo:', JSON.stringify(info))

const mirror = await evalJs(`window.shadowPreview.serviceMirror ? window.shadowPreview.serviceMirror() : 'no-api'`)
console.log('serviceMirror:', JSON.stringify(mirror))

ws.close()
