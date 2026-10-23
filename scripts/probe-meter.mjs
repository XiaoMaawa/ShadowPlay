// scripts/probe-meter.mjs —— 实时电平表（meter-v1.json）的写入频率与衰减检查
//
// 背景：顶栏那条音量条以前读的是状态文件里的"每秒峰值保持"，一秒才跳一格。
// 现在服务另写一份 meter-v1.json（30Hz），界面按 20Hz 读。这个脚本量三件事：
//
//   1. 写入频率 —— 有声音时每秒应该有二三十次 ts 前进（不是 1 次）；
//   2. 电平是否真的跟着声音走 —— 放 440Hz 正弦，lv 应该明显 > 0；
//   3. 声音停了会不会归零 —— 停掉音源后 lv 必须自己掉下去（靠 Session 的
//      时间戳衰减），不能挂在最后一个值上。
//
// 用法：node scripts/probe-meter.mjs
//       node scripts/probe-meter.mjs --profiles=profiles.json   # 抄真实配置里的桌面/显示器那一路

import { spawn } from 'node:child_process'
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs'
import path from 'node:path'
import { projectRoot, tempRoot } from './electron-path.mjs'

const backend = path.join(projectRoot, 'native', 'build', 'shadow-capture.exe')
const PROOF = path.join(tempRoot, 'proofs', 'meter')
const RECORD_ROOT = path.join(PROOF, 'records')
const PROFILES = path.join(PROOF, 'profiles.json')
const STATUS_DIR = path.join(tempRoot, 'status')
const STATUS = path.join(STATUS_DIR, 'status-v1.json')
const METER = path.join(STATUS_DIR, 'meter-v1.json')

const arg = (name, dflt) => {
  const hit = process.argv.find((a) => a.startsWith(`--${name}=`))
  return hit ? hit.split('=')[1] : dflt
}
const TARGET = arg('profiles', '')

if (!existsSync(backend)) {
  console.error('[probe] 找不到采集服务，先 npm run build:native')
  process.exit(1)
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
const readMeter = () => {
  try {
    return JSON.parse(readFileSync(METER, 'utf8'))
  } catch {
    return null
  }
}

// 桌面循环（系统混音）最省事：只要机器有声音就能读到，不用去匹窗口。
let source = { kind: 'monitor', monitorIndex: 0, clientAreaOnly: false, wgc: true }
let match
if (TARGET && existsSync(TARGET)) {
  const real = JSON.parse(readFileSync(TARGET, 'utf8'))
  const m = (real.profiles ?? []).find((p) => p.source?.kind === 'monitor')
  if (m) {
    source = m.source
    match = m.match
  }
}

rmSync(PROOF, { recursive: true, force: true })
mkdirSync(RECORD_ROOT, { recursive: true })
mkdirSync(STATUS_DIR, { recursive: true })
rmSync(METER, { force: true })
writeFileSync(
  PROFILES,
  JSON.stringify(
    {
      version: 1,
      recordRoot: RECORD_ROOT,
      exportDir: path.join(PROOF, 'exports'),
      profiles: [
        {
          id: 'p-meter',
          num: 1,
          name: 'metertest',
          enabled: true,
          source,
          match,
          video: {
            fps: 30,
            bitrateMbps: 4,
            replaySeconds: 10,
            segmentSeconds: 2,
            scaleWidth: 0,
            scaleHeight: 0,
          },
          createdAt: '2026-01-01T00:00:00.000Z',
        },
      ],
    },
    null,
    2,
  ),
)

const child = spawn(backend, ['--profiles', PROFILES], {
  cwd: projectRoot,
  stdio: ['pipe', 'pipe', 'pipe'],
  windowsHide: true,
})
const logs = []
child.stdout.setEncoding('utf8')
child.stdout.on('data', (d) => logs.push(...d.split(/\r?\n/).filter((l) => l.trim())))
child.stderr.setEncoding('utf8')
child.stderr.on('data', (d) => logs.push(`[stderr] ${d.trim()}`))
child.stdin.write('')

let tone = null
const startTone = () => {
  // 走默认输出设备的 440Hz 正弦：系统混音环回一定看得见。
  tone = spawn(
    'ffplay',
    ['-nodisp', '-autoexit', '-loglevel', 'error', '-f', 'lavfi', '-i', 'sine=frequency=440:duration=12'],
    { stdio: 'ignore', windowsHide: true },
  )
}
const kill = (p) => {
  if (!p) return
  try {
    p.kill('SIGKILL')
  } catch {
    /* 已经退了 */
  }
  try {
    spawn('taskkill', ['/pid', String(p.pid), '/T', '/F'], { stdio: 'ignore', windowsHide: true })
  } catch {
    /* 忽略 */
  }
}

try {
  // ---- 等 service 起来并开始录音 -------------------------------------------
  const t0 = Date.now()
  let st = null
  while (Date.now() - t0 < 15000) {
    await sleep(300)
    try {
      st = JSON.parse(readFileSync(STATUS, 'utf8'))?.items?.[0] ?? null
    } catch {
      st = null
    }
    if (st?.state === 'live' && st.recording) break
  }
  if (!st || st.state !== 'live') {
    console.log(`[probe] 失败：服务没进 live（${st?.state}：${st?.detail ?? ''}）`)
    process.exitCode = 1
  } else {
    console.log(`[probe] 服务 live，音频：${st.audioDetail || '(无)'}`)
    await sleep(1500)

    // ---- 1/2. 放音期间采样 ------------------------------------------------
    startTone()
    await sleep(1200) // 让音源真正出声

    const samples = []
    const tStart = Date.now()
    let lastTs = 0
    let writes = 0
    let maxLv = 0
    let nonzero = 0
    while (Date.now() - tStart < 5000) {
      const m = readMeter()
      if (m && typeof m.ts === 'number') {
        if (m.ts !== lastTs) {
          if (lastTs !== 0) writes++
          lastTs = m.ts
        }
        const lv = Number(m.items?.[0]?.lv ?? 0)
        if (Number.isFinite(lv)) {
          samples.push(lv)
          if (lv > maxLv) maxLv = lv
          if (lv > 0.002) nonzero++
        }
      }
      await sleep(20)
    }
    const elapsed = (Date.now() - tStart) / 1000
    const hz = writes / elapsed

    // ---- 3. 停掉音源，看衰减 ---------------------------------------------
    kill(tone)
    tone = null
    const tStop = Date.now()
    let silentAt = null
    while (Date.now() - tStop < 3000) {
      const m = readMeter()
      const lv = Number(m?.items?.[0]?.lv ?? 0)
      if (lv <= 0.002) {
        silentAt = Date.now() - tStop
        break
      }
      await sleep(40)
    }

    console.log(`[probe] 电平表更新频率：${hz.toFixed(1)} 次/秒（采样 ${samples.length} 次）`)
    console.log(`[probe] 放音期间峰值 lv = ${maxLv.toFixed(4)}，非零样本 ${nonzero}/${samples.length}`)
    console.log(
      `[probe] 停音后归零耗时：${silentAt === null ? '3 秒内没归零（异常）' : `${silentAt} ms`}`,
    )

    const ok = hz >= 15 && maxLv > 0.005 && silentAt !== null && silentAt < 1500
    console.log(`[probe] 结论：${ok ? '通过' : '不通过'}`)
    if (!ok) {
      process.exitCode = 1
      console.log('[probe] 服务日志尾部：')
      for (const l of logs.slice(-15)) console.log('   ', l)
    }
  }
} finally {
  kill(tone)
  try {
    child.stdin.write('{"cmd":"quit"}\n')
  } catch {
    /* 管道已断 */
  }
  await sleep(400)
  kill(child)
  // 服务的 stdout 管道还在，node 会因为句柄没关而挂着不退 —— 这个探针是一次性的，
  // 该量的都量完了，直接退（退出码由前面设的 process.exitCode 决定）。
  await sleep(200)
  process.exit(process.exitCode ?? 0)
}
