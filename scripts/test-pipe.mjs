// 单独测命名管道命令通道：起一个服务（只有桌面这一路，保证稳定），
// 然后发命令、看服务日志里有没有对应的行。
import { spawn } from 'node:child_process'
import { createRequire } from 'node:module'
import net from 'node:net'
import fs from 'node:fs'
import path from 'node:path'

const ROOT = process.cwd()
const EXE = path.join(ROOT, 'native', 'build', 'shadow-capture.exe')
const { TEMP_ROOT } = createRequire(import.meta.url)('../app/temp-const.cjs')
const TEMP = TEMP_ROOT
const CFG = path.join(TEMP, 'proofs', 'pipe-test.json')
const LOG = path.join(TEMP, 'proofs', 'pipe-test.log')
const PIPE = '\\\\.\\pipe\\ShadowPlay.Cmd.v1'

fs.mkdirSync(path.dirname(CFG), { recursive: true })
fs.writeFileSync(
  CFG,
  JSON.stringify(
    {
      version: 1,
      recordRoot: path.join(TEMP, 'proofs', 'pipe-records'),
      exportDir: path.join(TEMP, 'proofs', 'pipe-exports'),
      profiles: [
        {
          id: 'p-desk',
          name: '整屏',
          enabled: true,
          source: { kind: 'desktop', monitorIndex: 0, wgc: false },
          video: { fps: 30, replaySeconds: 10 },
          match: {},
        },
      ],
    },
    null,
    2,
  ),
  'utf8',
)

const logFd = fs.openSync(LOG, 'w')
// stdin 保持打开（'pipe' 且不 end），这样 cmdThread 不会立刻退出
const svc = spawn(EXE, ['--profiles', CFG], { stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true })
svc.stdout.on('data', (d) => fs.writeSync(logFd, d))
svc.stderr.on('data', (d) => fs.writeSync(logFd, d))
console.log('服务 pid =', svc.pid)

const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
await sleep(2500)

// 先确认管道存在
console.log('管道存在？', fs.existsSync(PIPE))

// 发一条 reload —— 用 reload 但 id 不存在时也会打日志（profile_reload_failed
// 或 session_reload_failed），所以只要出现了任何 reload 相关行，就说明命令通了。
function send(payload) {
  return new Promise((resolve) => {
    const s = net.connect(PIPE)
    s.on('connect', () => s.end(payload))
    s.on('error', (e) => resolve({ ok: false, error: e.message }))
    s.on('close', () => resolve({ ok: true }))
    setTimeout(() => resolve({ ok: false, error: 'timeout' }), 3000).unref()
  })
}

console.log('发送 reload:', JSON.stringify(await send('{"cmd":"reload","id":"p-desk"}\n')))
await sleep(2000)

const log = fs.readFileSync(LOG, 'utf8')
const reloadLines = log.split(/\r?\n/).filter((l) => l.includes('reload'))
console.log(`日志里 reload 相关行数 = ${reloadLines.length}`)
for (const l of reloadLines) console.log('  ', l)

// 再试一次 export（这个一定会打日志）
console.log('发送 export:', JSON.stringify(await send('{"cmd":"export","id":"p-desk"}\n')))
await sleep(3000)
const log2 = fs.readFileSync(LOG, 'utf8')
const expLines = log2.split(/\r?\n/).filter((l) => l.includes('export'))
console.log(`日志里 export 相关行数 = ${expLines.length}`)
for (const l of expLines.slice(0, 3)) console.log('  ', l.slice(0, 160))

svc.kill()
console.log('完成')
process.exit(0)
