// 复现「点保存报 Error: An object could not be cloned.」
//
// 通过 CDP 直接调用页面里的 window.shadowPreview.saveProfiles(...)，
// 一步步缩小到底是哪个字段不可结构化克隆。
//
// 用法：node scripts/probe-save.mjs 9222
import { setTimeout as sleep } from 'node:timers/promises'

const port = process.argv[2] ?? '9222'
const base = `http://127.0.0.1:${port}`

async function findPage() {
  for (let i = 0; i < 60; i++) {
    try {
      const list = await (await fetch(`${base}/json/list`)).json()
      const page = list.find((t) => t.type === 'page' && t.webSocketDebuggerUrl)
      if (page) return page
    } catch {
      // 端口还没起来
    }
    await sleep(250)
  }
  throw new Error('找不到可调试的页面')
}

const page = await findPage()
const ws = new WebSocket(page.webSocketDebuggerUrl)
await new Promise((res, rej) => {
  ws.onopen = res
  ws.onerror = () => rej(new Error('WebSocket 连接失败'))
})

let seq = 0
const pending = new Map()
ws.onmessage = (ev) => {
  const msg = JSON.parse(ev.data)
  if (msg.id && pending.has(msg.id)) {
    const { resolve, reject } = pending.get(msg.id)
    pending.delete(msg.id)
    if (msg.error) reject(new Error(JSON.stringify(msg.error)))
    else resolve(msg.result)
  }
}

function send(method, params = {}) {
  const id = ++seq
  ws.send(JSON.stringify({ id, method, params }))
  return new Promise((resolve, reject) => {
    pending.set(id, { resolve, reject })
    setTimeout(() => {
      if (pending.has(id)) {
        pending.delete(id)
        reject(new Error(`${method} 超时`))
      }
    }, 20000)
  })
}

async function evaluate(expression) {
  const r = await send('Runtime.evaluate', {
    expression,
    awaitPromise: true,
    returnByValue: true,
  })
  if (r.exceptionDetails) {
    return { threw: true, text: r.exceptionDetails.exception?.description ?? r.exceptionDetails.text }
  }
  return { threw: false, value: r.result?.value }
}

await send('Runtime.enable')

const cases = [
  // 1. 最小：只塞一个普通对象
  ['最小对象 {a:1}', `window.shadowPreview.saveProfiles({a:1})`],
  // 2. 界面真正发出去的那个形状（recordRoot/exportDir 都可能为空）
  ['真实形状（取页面当前 profiles）', `(async () => {
     const doc = await window.shadowPreview.serviceProfiles()
     const payload = { recordRoot: doc.recordRoot || undefined, exportDir: doc.exportDir || undefined, profiles: doc.items }
     const bad = []
     JSON.stringify(payload) // 只验能否序列化
     for (const p of payload.profiles) {
       for (const [k, v] of Object.entries(p)) {
         if (typeof v === 'function') bad.push(p.id + '.' + k + ' = function')
         if (typeof v === 'symbol') bad.push(p.id + '.' + k + ' = symbol')
       }
       for (const [k, v] of Object.entries(p.video || {})) {
         if (v === undefined) bad.push(p.id + '.video.' + k + ' = undefined')
         if (typeof v === 'function') bad.push(p.id + '.video.' + k + ' = function')
         if (typeof v === 'symbol') bad.push(p.id + '.video.' + k + ' = symbol')
         if (typeof v === 'object' && v !== null) bad.push(p.id + '.video.' + k + ' = object')
       }
     }
     return { payloadJson: JSON.stringify(payload).slice(0, 200), suspicious: bad }
   })()`],
  // 3. 真的调一次（不写盘版本无法测，这里就真调，反正内容一样）
  ['真实 payload 直接 invoke', `(async () => {
     const doc = await window.shadowPreview.serviceProfiles()
     return window.shadowPreview.saveProfiles({
       recordRoot: doc.recordRoot || undefined,
       exportDir: doc.exportDir || undefined,
       profiles: doc.items,
     })
   })()`],
]

for (const [name, expr] of cases) {
  const r = await evaluate(expr)
  if (r.threw) console.log(`✗ ${name}\n   ${r.text}`)
  else console.log(`✓ ${name}\n   ${JSON.stringify(r.value)}`)
}

ws.close()
