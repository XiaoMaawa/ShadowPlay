// scripts/dev-server.mjs — 只起 Vite 开发服务器（前端热更新）
import { spawnSync } from 'node:child_process'
import { existsSync } from 'node:fs'
import path from 'node:path'
import { projectRoot } from './electron-path.mjs'

const viteBin = path.join(projectRoot, 'frontend', 'node_modules', 'vite', 'bin', 'vite.js')
if (!existsSync(viteBin)) {
  console.error(`找不到 Vite：${viteBin}`)
  process.exit(1)
}

const r = spawnSync(process.execPath, [viteBin], {
  cwd: path.join(projectRoot, 'frontend'),
  stdio: 'inherit',
})
process.exit(r.status ?? 1)
