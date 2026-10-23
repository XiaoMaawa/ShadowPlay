// 临时探针：A 独奏 vs A+B 双播放，看进程环回的采集字节/电平有没有掉。
// 必须在同一个进程里跑完（后台子进程会随 shell 退出被回收）。
import { spawn, execSync } from 'node:child_process'
import { readFileSync } from 'node:fs'
import path from 'node:path'

const STATUS = path.join(process.env.TEMP, 'shadow-play', 'app', 'status', 'status-v1.json')
const PROFILES = path.join(process.env.TEMP, 'p-proc.json')
const BACKEND = 'native/build/shadow-capture.exe'

const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
const rd = () => {
  try {
    return JSON.parse(readFileSync(STATUS, 'utf8')).items?.[0] ?? null
  } catch {
    return null
  }
}
const alive = () => {
  try {
    const out = execSync('tasklist /FI "IMAGENAME eq ffplay.exe"').toString()
    return (out.match(/ffplay\.exe/g) ?? []).length
  } catch {
    return -1
  }
}

const kill = (name) => {
  try {
    execSync(`taskkill /F /IM ${name}`, { stdio: 'ignore' })
  } catch {
    /* 没有这个进程时 taskkill 返回 128，正常 */
  }
}
kill('ffplay.exe')
kill('shadow-capture.exe')
await sleep(1000)

// A：带窗口，能被窗口源匹配到
spawn('ffplay', ['-hide_banner', '-loglevel', 'quiet', '-x', '200', '-y', '150', '-f', 'lavfi', '-i',
  'sine=frequency=440:sample_rate=48000,volume=0.2'], { stdio: 'ignore', detached: true }).unref()
await sleep(3000)

spawn(BACKEND, ['--profiles', PROFILES], { cwd: process.cwd(), stdio: 'ignore', detached: true }).unref()
await sleep(6000)

for (let i = 0; i < 3; i += 1) {
  const it = rd()
  console.log(`[A独奏] peak=${(it?.audioPeak ?? 0).toFixed(4)} rms=${(it?.audioRms ?? 0).toFixed(4)} bytes=${it?.audioBytes} ffplay=${alive()} ${it?.audioDetail ?? ''}`)
  await sleep(1000)
}

console.log('--- 起第二路播放器（无窗口） ---')
spawn('ffplay', ['-hide_banner', '-loglevel', 'quiet', '-nodisp', '-f', 'lavfi', '-i',
  'sine=frequency=660:sample_rate=48000,volume=0.2'], { stdio: 'ignore', detached: true }).unref()
await sleep(2000)

let last = -1
for (let i = 0; i < 10; i += 1) {
  const it = rd()
  const b = it?.audioBytes ?? 0
  console.log(`[双播放 t+${(i * 1.5).toFixed(1)}s] peak=${(it?.audioPeak ?? 0).toFixed(4)} rms=${(it?.audioRms ?? 0).toFixed(4)} bytes=${b}${b === last ? '  <-- 冻结' : ''} ffplay=${alive()}`)
  last = b
  await sleep(1500)
}
kill('ffplay.exe')
kill('shadow-capture.exe')
process.exit(0)
