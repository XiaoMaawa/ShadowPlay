// scripts/dev.mjs — 开发模式：Vite 开发服务器 + Electron
//
//   npm run dev
//
// 前端有热更新；采集服务需要单独启动（npm run service），
// 这样改前端不会打断采集，改 C++ 也只需重启服务。
import { spawn } from 'node:child_process'
import path from 'node:path'
import { projectRoot, electronDistPath, assertElectronDist } from './electron-path.mjs'

const DEV_URL = process.env.DEV_SERVER_URL || 'http://127.0.0.1:5173'
const exe = assertElectronDist()

const env = { ...process.env }
delete env.ELECTRON_RUN_AS_NODE
env.ELECTRON_OVERRIDE_DIST_PATH = electronDistPath
env.DEV_SERVER_URL = DEV_URL

const viteBin = path.join(projectRoot, 'frontend', 'node_modules', 'vite', 'bin', 'vite.js')
const vite = spawn(process.execPath, [viteBin], {
  cwd: path.join(projectRoot, 'frontend'),
  stdio: 'inherit',
})

const children = [vite]
let shuttingDown = false

function shutdown(code = 0) {
  if (shuttingDown) return
  shuttingDown = true
  for (const c of children) {
    if (c && !c.killed) {
      try {
        c.kill()
      } catch {
        /* 忽略 */
      }
    }
  }
  process.exit(code)
}

vite.on('exit', (code) => {
  if (!shuttingDown) {
    console.error(`[dev] Vite 退出（${code}）`)
    shutdown(code ?? 1)
  }
})

async function waitForServer(url, timeoutMs = 30000) {
  const deadline = Date.now() + timeoutMs
  while (Date.now() < deadline) {
    try {
      const res = await fetch(url)
      if (res.ok || res.status === 404) return true
    } catch {
      /* 还没起来 */
    }
    await new Promise((r) => setTimeout(r, 300))
  }
  return false
}

if (!(await waitForServer(DEV_URL))) {
  console.error(`[dev] 等待 ${DEV_URL} 超时`)
  shutdown(1)
}

console.log(`[dev] Vite 就绪：${DEV_URL}`)
console.log('[dev] 提示：采集服务要单独跑 ——  另开终端执行  npm run service')
console.log('[dev] 启动 Electron …')

const electron = spawn(exe, ['.', '--dev'], { cwd: projectRoot, stdio: 'inherit', env })
children.push(electron)
electron.on('exit', (code) => shutdown(code ?? 0))
process.on('SIGINT', () => shutdown(0))
process.on('SIGTERM', () => shutdown(0))
