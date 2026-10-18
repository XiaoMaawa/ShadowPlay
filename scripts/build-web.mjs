// scripts/build-web.mjs — 用 Vite 打包前端到临时目录下的 dist/
//
// 直接调用 Vite 的 JS 入口，不经过 npm，避免 npm cache 的权限问题。
//
// 产物落在 <临时根>/dist（默认 %TEMP%/shadow-play/app/dist）而不是仓库里：
// dist 属于构建产物，和 records/exports/proofs/status/logs 一样都是"跑一次
// 就有、删掉能重建"的东西，统一收在临时目录，仓库根只留源码和配置。
// electron 侧读这个路径见 app/main.cjs（同一个 app/temp-const.cjs 算出来的）。
import { spawnSync } from 'node:child_process'
import { existsSync, mkdirSync } from 'node:fs'
import path from 'node:path'
import { projectRoot, tempRoot } from './electron-path.mjs'

const viteBin = path.join(projectRoot, 'frontend', 'node_modules', 'vite', 'bin', 'vite.js')
if (!existsSync(viteBin)) {
  console.error(`找不到 Vite：${viteBin}\n请先在 frontend/ 下执行一次 npm install`)
  process.exit(1)
}

const outDir = path.join(tempRoot, 'dist')
mkdirSync(outDir, { recursive: true })

const r = spawnSync(process.execPath, [viteBin, 'build'], {
  cwd: path.join(projectRoot, 'frontend'),
  stdio: 'inherit',
})

if (r.error) {
  console.error('[build-web] 启动 Vite 失败：', r.error.message)
  process.exit(1)
}
if (r.status !== 0) {
  console.error(`[build-web] Vite 退出码 ${r.status}`)
  process.exit(r.status ?? 1)
}

const index = path.join(outDir, 'index.html')
console.log(`[build-web] 完成：${index}`)
