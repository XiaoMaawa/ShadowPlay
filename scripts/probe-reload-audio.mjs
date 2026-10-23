// 压测：连续 reload（等价于连点保存）之后，音频采集是否每次都恢复。
//
// 用户报「点击保存后过一会保存有概率出现捕捉不到声音」。reload 会重建会话，
// 音频采集要跟着重开一次 —— 重开失败的窗口期就会没声音。这里连发 N 次
// reload，每次等几秒再读状态，把 audioActive / detail / error 全打出来。
//
// 用法：node scripts/probe-reload-audio.mjs [--rounds=8] [--profiles=xxx.json]

import { spawn } from 'node:child_process'
import { readFileSync } from 'node:fs'
import path from 'node:path'
import { projectRoot, tempRoot } from './electron-path.mjs'

const backend = path.join(projectRoot, 'native', 'build', 'shadow-capture.exe')
const STATUS = path.join(tempRoot, 'status', 'status-v1.json')
const arg = (name, dflt) => {
  const hit = process.argv.find((a) => a.startsWith(`--${name}=`))
  return hit ? hit.split('=')[1] : dflt
}
const ROUNDS = Number(arg('rounds', 8))
const PROFILES = arg('profiles', path.join(process.env.TEMP, 'pp-bili.json'))

const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
const desk = () => {
  try {
    return JSON.parse(readFileSync(STATUS, 'utf8')).items?.[0] ?? null
  } catch {
    return null
  }
}

const child = spawn(backend, ['--profiles', PROFILES], {
  cwd: projectRoot,
  stdio: ['pipe', 'pipe', 'inherit'],
  windowsHide: true,
})
child.stdout.resume()

console.log(`[probe] ${path.basename(PROFILES)}，${ROUNDS} 轮 reload`)
let bad = 0
for (let round = 1; round <= ROUNDS; round += 1) {
  await sleep(4000)
  const before = desk()
  if (!before || before.state !== 'live') {
    console.log(`[probe] 第 ${round} 轮：还没 live（${before?.state}）`)
    continue
  }
  child.stdin.write(JSON.stringify({ cmd: 'reload', id: before.id }) + '\n')
  // 盯 6 秒：每 500ms 看一次，记录"第一次恢复声音"的时刻
  const t0 = Date.now()
  let recoveredAt = -1
  let worst = ''
  while (Date.now() - t0 < 6000) {
    await sleep(500)
    const s = desk()
    if (!s) continue
    if (s.audioActive && recoveredAt < 0) recoveredAt = Date.now() - t0
    if (!s.audioActive || s.audioError) {
      worst = `audio=${s.audioActive} detail=${s.audioDetail ?? ''} err=${s.audioError ?? ''}`
    }
  }
  const now = desk()
  const ok = now?.audioActive === true && !(now?.audioError ?? '')
  if (!ok) bad += 1
  console.log(
    `[probe] 第 ${round} 轮：${ok ? 'ok' : '没声音'} 恢复耗时=${recoveredAt < 0 ? '未恢复' : recoveredAt + 'ms'}` +
      ` peak=${(now?.audioPeak ?? 0).toFixed(3)} bytes=${now?.audioBytes} | ${now?.audioDetail ?? ''}` +
      (ok ? '' : `  ← ${worst}`),
  )
}
child.stdin.end()
child.kill()
console.log(bad === 0 ? '\n[probe] 通过：每轮 reload 后声音都回来了' : `\n[probe] 失败：${bad} 轮没声音`)
process.exit(bad === 0 ? 0 : 1)
