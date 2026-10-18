// 验证两件事：
//   1. 把一路配置 enabled 改成 false 之后，这一路真的停止捕获（帧环消失/状态 Disabled）
//   2. 服务启动时目标窗口还不存在，之后把窗口打开，这一路能自己活过来
//
// 用法：node scripts/test-selfheal.mjs
//
// 全程通过命名管道给服务下命令，不重启服务 —— 这也顺带验证了热重载链路。
import { spawn } from 'node:child_process'
import { createRequire } from 'node:module'
import net from 'node:net'
import fs from 'node:fs'
import path from 'node:path'

const ROOT = process.cwd()
const EXE_SRC = path.join(ROOT, 'native', 'build', 'shadow-capture.exe')
// 【为什么要复制一份 exe 再跑】项目根目录被 DSH 沙箱打上了 Low 完整性标签
// （`Mandatory Label\Low Mandatory Level`），这个标签向下继承到 exe，而 WGC
// 拒绝低完整性进程做窗口采集（0x80070005）。%TEMP% 下没有这个标签，把 exe
// 复制过去跑就能验证 WGC 链路本身是好的。这只影响"验证脚本怎么跑"，
// 产品代码一行不改。
//
// 注意：不能用 fs.copyFileSync —— 它在 Windows 上走 CopyFileW，会把源文件的
// ACL 一并搬过去，结果在 %TEMP% 下跑出来的 exe **照样带 Low 标签**（实测
// 仍然报 0x80070005）。必须自己读字节再写字节，新文件才会用目录的默认 ACL。
const EXE = path.join(process.env.TEMP ?? 'C:\\Windows\\Temp', 'shadow-capture-selfheal.exe')
fs.writeFileSync(EXE, fs.readFileSync(EXE_SRC))
const { TEMP_ROOT } = createRequire(import.meta.url)('../app/temp-const.cjs')
const TEMP = TEMP_ROOT
const PIPE = '\\\\.\\pipe\\ShadowPlay.Cmd.v1'
const CFG = path.join(TEMP, 'proofs', 'selfheal-profiles.json')
const REC = path.join(TEMP, 'proofs', 'selfheal-records')
// 服务自己按 %TEMP%/shadow-play/app/status 定位状态文件，和界面同一套规则。
// 这里显式指到 proofs 下，跑完跟着临时目录一起消失。
const STATUS = path.join(TEMP, 'proofs', 'selfheal-status.json')

fs.mkdirSync(path.dirname(CFG), { recursive: true })
fs.mkdirSync(REC, { recursive: true })

// 目标窗口：记事本。先确保一个都没有。
function run(cmd, args) {
  return new Promise((resolve) => {
    const p = spawn(cmd, args, { stdio: ['ignore', 'pipe', 'pipe'] })
    let out = ''
    p.stdout.on('data', (d) => (out += d))
    p.stderr.on('data', (d) => (out += d))
    p.on('close', () => resolve(out))
  })
}

const PS = path.join(process.env.SystemRoot ?? 'C:\\Windows', 'System32', 'WindowsPowerShell', 'v1.0', 'powershell.exe')

// 数一下当前有几个记事本主窗口
async function notepadCount() {
  const out = await run(PS, [
    '-NoProfile',
    '-Command',
    "(Get-Process notepad -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 }).Count",
  ])
  const n = parseInt(out.trim(), 10)
  return Number.isFinite(n) ? n : 0
}

function sendPipeOnce(cmd) {
  return new Promise((resolve) => {
    const payload = JSON.stringify(cmd) + '\n'
    console.log(`    [pipe] connect ${PIPE}`)
    const s = net.connect(PIPE)
    let done = false
    const finish = (r) => {
      if (done) return
      done = true
      try {
        s.destroy()
      } catch {}
      resolve(r)
    }
    s.on('connect', () => {
      console.log(`    [pipe] connected, sending ${payload.trim()}`)
      // 跟 scripts/test-pipe.mjs 一样：end(payload) 一把写完就关。分两步
      // write + 延迟 end 在服务端 ReadFile 阻塞等数据的模型下更容易撞上
      // "写完但没读到"的时序，实测会丢命令。
      s.end(payload)
    })
    s.on('error', (e) => finish({ ok: false, error: e.message }))
    s.on('close', () => finish({ ok: true }))
    setTimeout(() => finish({ ok: false, error: 'timeout' }), 5000).unref()
  })
}

// 服务端处理完一个连接就 DisconnectNamedPipe + 重建监听，中间有个毫秒级的
// 空窗。撞上那一下会 ENOENT，重试两次就好。
async function sendPipe(cmd) {
  for (let i = 0; i < 5; i++) {
    const r = await sendPipeOnce(cmd)
    if (r.ok) return r
    await sleep(400)
  }
  return { ok: false, error: 'retries exhausted' }
}

function readStatus() {
  try {
    return JSON.parse(readFileRetry(STATUS))
  } catch {
    return null
  }
}

// Windows 上文件可能正被写，重试几次
function readFileRetry(p) {
  for (let i = 0; i < 20; i++) {
    try {
      return fs.readFileSync(p, 'utf8')
    } catch {
      // 同步忙等几十毫秒，脚本里无所谓
      const until = Date.now() + 30
      while (Date.now() < until) {}
    }
  }
  throw new Error(`读不到 ${p}`)
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms))

// ---------------------------------------------------------------------------
// 配置：一路桌面（当对照），一路记事本窗口
// ---------------------------------------------------------------------------
const cfg = {
  version: 1,
  recordRoot: REC,
  exportDir: path.join(TEMP, 'proofs', 'selfheal-exports'),
  profiles: [
    {
      id: 'p-desk',
      name: '整屏',
      enabled: true,
      source: { kind: 'desktop', monitorIndex: 0, wgc: false },
      video: { fps: 30, replaySeconds: 10 },
      match: {},
    },
    {
      id: 'p-note',
      name: '记事本',
      enabled: true,
      source: { kind: 'window', monitorIndex: 0, wgc: true },
      video: { fps: 30, replaySeconds: 10 },
      match: { by: 'process', mode: 'contains', pick: 'largest', pattern: 'notepad', pollMs: 1000 },
    },
  ],
}
// 每次跑都从「两路都开着」起步。脚本自己在阶段 3 会把 p-note 关掉，
// 文件留在磁盘上；下一次跑如果不重置，服务启动时 p-note 就是 disabled，
// 第一阶段的判定全错（踩过一次：看到 items 里根本没有 p-note）。
cfg.profiles[0].enabled = true
cfg.profiles[1].enabled = true
fs.writeFileSync(CFG, JSON.stringify(cfg, null, 2), 'utf8')

// 清场：先关掉所有记事本，保证服务启动时目标窗口不存在
await run(PS, ['-NoProfile', '-Command', 'Get-Process notepad -ErrorAction SilentlyContinue | Stop-Process -Force'])
await sleep(800)
console.log(`启动前记事本窗口数 = ${await notepadCount()}`)

// 起服务
const logFile = fs.openSync(path.join(TEMP, 'proofs', 'selfheal-service.log'), 'w')
const svc = spawn(EXE, ['--profiles', CFG], {
  stdio: ['pipe', 'pipe', 'pipe'],
  windowsHide: true,
  env: { ...process.env, SHADOW_STATUS_FILE: STATUS },
})
svc.stdout.on('data', (d) => fs.writeSync(logFile, d))
svc.stderr.on('data', (d) => fs.writeSync(logFile, d))
const pid = svc.pid
console.log(`服务 pid = ${pid}`)

function show(tag) {
  const st = readStatus()
  if (!st) {
    console.log(`  ${tag}: 还没有状态文件`)
    return null
  }
  for (const s of st.items) {
    console.log(
      `  ${tag} ${s.id}: state=${s.state} kind=${s.captureKind} ${s.width}x${s.height} fps=${s.actualFps} rec=${s.recording}`,
    )
    if (s.detail) console.log(`        detail: ${s.detail}`)
  }
  return st
}

// ---- 阶段 1：窗口不存在 ------------------------------------------------
await sleep(3000)
console.log('\n=== 阶段1：服务已起，记事本不存在 ===')
const st1 = show('t=3s')
const note1 = st1?.items.find((s) => s.id === 'p-note')
console.log(`判定：p-note 应为 disabled（匹配不到窗口）→ ${note1?.state}`)

// ---- 阶段 2：把记事本打开 ----------------------------------------------
console.log('\n=== 阶段2：打开记事本，看它能不能自己活过来 ===')
await run(PS, ['-NoProfile', '-Command', 'Start-Process notepad'])
await sleep(1500)
console.log(`  启动后记事本主窗口数 = ${await notepadCount()}`)
// 判定"活着"看的是有没有在真抓帧，不是 state：
// state=live 的意思是"有消费者连着"（界面没开的时候当然是 waiting），
// 跟这一路有没有在捕获毫无关系。
function alive(s) {
  return !!s && s.captureKind !== 'none' && s.actualFps > 1
}

let healed = false
for (let i = 0; i < 20; i++) {
  await sleep(1000)
  const st = readStatus()
  const note = st?.items.find((s) => s.id === 'p-note')
  if (alive(note)) {
    console.log(`  第 ${i + 1} 秒：p-note 已活过来，kind=${note.captureKind} ${note.width}x${note.height} fps=${note.actualFps}`)
    healed = true
    break
  }
  console.log(`  第 ${i + 1} 秒：state=${note?.state ?? '?'} kind=${note?.captureKind ?? '?'} fps=${note?.actualFps ?? '?'}`)
}
console.log(`判定：窗口后开也能自动捕获 → ${healed ? '通过' : '失败'}`)

// ---- 阶段 3：关掉这一路的开关 ------------------------------------------
console.log('\n=== 阶段3：把 p-note 的 enabled 改成 false（走命名管道热重载）===')
cfg.profiles[1].enabled = false
fs.writeFileSync(CFG, JSON.stringify(cfg, null, 2), 'utf8')
console.log('  sendPipe:', JSON.stringify(await sendPipe({ cmd: 'reload', id: 'p-note' })))
// 给足时间：reload 会先 Stop 旧会话（WGC 拆泵最多等 3 秒）再决定建不建。
await sleep(8000)
const st3 = show('after-off')
const note3 = st3?.items.find((s) => s.id === 'p-note')
const desk3 = st3?.items.find((s) => s.id === 'p-desk')
const offOk = !note3 || (!alive(note3) && note3.captureKind === 'none')
console.log(`判定：p-note 应已停止捕获 → ${note3 ? `kind=${note3.captureKind} fps=${note3.actualFps}` : '已移除'} ${offOk ? '（正确）' : '（错误）'}`)
console.log(`判定：p-desk 不受影响 → kind=${desk3?.captureKind} fps=${desk3?.actualFps} ${alive(desk3) ? '（正确）' : '（错误）'}`)

// ---- 阶段 4：再打开 ----------------------------------------------------
console.log('\n=== 阶段4：把 p-note 重新 enabled，看能不能回来 ===')
cfg.profiles[1].enabled = true
fs.writeFileSync(CFG, JSON.stringify(cfg, null, 2), 'utf8')
console.log('  sendPipe:', JSON.stringify(await sendPipe({ cmd: 'reload', id: 'p-note' })))
let back = false
for (let i = 0; i < 15; i++) {
  await sleep(1000)
  const st = readStatus()
  const note = st?.items.find((s) => s.id === 'p-note')
  if (alive(note)) {
    console.log(`  第 ${i + 1} 秒：p-note 回来了，kind=${note.captureKind} fps=${note.actualFps}`)
    back = true
    break
  }
  console.log(`  第 ${i + 1} 秒：state=${note?.state ?? '?'} kind=${note?.captureKind ?? '?'} fps=${note?.actualFps ?? '?'}`)
}
console.log(`判定：重新开了开关能恢复 → ${back ? '通过' : '失败'}`)

// ---- 收尾 --------------------------------------------------------------
console.log(`\n服务 pid 仍是 ${pid}（全程未重启）`)
svc.kill()
await run(PS, ['-NoProfile', '-Command', 'Get-Process notepad -ErrorAction SilentlyContinue | Stop-Process -Force'])
console.log('\n完成。')
process.exit(0)
