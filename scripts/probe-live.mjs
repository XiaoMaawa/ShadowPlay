// 采样界面顶栏的服务状态与分辨率显示，用来验证"服务未启动"误报是否还在。
//
// 用法：node scripts/probe-live.mjs [seconds]
//
// 走 CDP 读 DOM（不是读内部状态），看到的就是用户眼睛看到的那一行字。

const PORT = process.env.CDP_PORT || '9222'
const SECONDS = Number(process.argv[2] || 12)

async function targets() {
  const r = await fetch(`http://127.0.0.1:${PORT}/json/list`)
  return r.json()
}

function wsUrl(t) {
  return t.webSocketDebuggerUrl
}

class Cdp {
  constructor(url) {
    this.ws = new WebSocket(url)
    this.id = 0
    this.pending = new Map()
    this.ws.addEventListener('message', (ev) => {
      const msg = JSON.parse(ev.data)
      if (msg.id && this.pending.has(msg.id)) {
        const { resolve, reject } = this.pending.get(msg.id)
        this.pending.delete(msg.id)
        if (msg.error) reject(new Error(JSON.stringify(msg.error)))
        else resolve(msg.result)
      }
    })
  }
  ready() {
    return new Promise((res, rej) => {
      this.ws.addEventListener('open', () => res())
      this.ws.addEventListener('error', (e) => rej(new Error('ws error')))
    })
  }
  send(method, params = {}) {
    const id = ++this.id
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject })
      this.ws.send(JSON.stringify({ id, method, params }))
    })
  }
  async eval(expr) {
    const r = await this.send('Runtime.evaluate', {
      expression: expr,
      returnByValue: true,
      awaitPromise: true,
    })
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.text)
    return r.result.value
  }
  close() {
    this.ws.close()
  }
}

const SNAPSHOT = `(() => {
  const bar = document.querySelector('header.bar')
  if (!bar) return { error: 'no header' }
  const txt = (el) => el ? el.textContent.replace(/\\s+/g, ' ').trim() : null
  const metas = [...bar.querySelectorAll('span.meta')].map(txt)
  return {
    bar: txt(bar),
    metas,
    running: !!bar.querySelector('.svcdot'),
    warn: !!bar.querySelector('.meta.warn'),
  }
})()`

const t = (await targets()).find((x) => x.type === 'page')
if (!t) {
  console.error('找不到页面目标')
  process.exit(1)
}
const cdp = new Cdp(wsUrl(t))
await cdp.ready()
await cdp.send('Runtime.enable')

const started = Date.now()
let lastBar = null
while (Date.now() - started < SECONDS * 1000) {
  const snap = await cdp.eval(SNAPSHOT)
  const line = JSON.stringify(snap)
  if (line !== lastBar) {
    console.log(`${((Date.now() - started) / 1000).toFixed(1)}s ${line}`)
    lastBar = line
  }
  await new Promise((r) => setTimeout(r, 500))
}
cdp.close()
