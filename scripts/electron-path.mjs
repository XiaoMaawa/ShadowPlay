// scripts/electron-path.mjs — 定位 Electron 二进制
//
// 背景：electron 的 npm 包只带 JS 外壳，二进制要单独下载。
// 本机没有外网，install.js 取不到包，所以二进制是从 electron 的下载缓存里
// 解压出来的。为了让 `npx electron` / `require('electron')` 也能正常工作，
// 解压结果同时放在标准位置 node_modules/electron/dist/。
//
// 查找顺序：
//   1. node_modules/electron/dist/electron.exe   标准位置（npm 脚本、npx electron 都用它）
//   2. .electron/dist/electron.exe               备用位置（手动解压）
//
// 找到之后会把 ELECTRON_OVERRIDE_DIST_PATH 指过去，保证所有调用方一致。
import { existsSync } from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const here = path.dirname(fileURLToPath(import.meta.url))
export const projectRoot = path.resolve(here, '..')

// 临时产物根目录。**唯一来源是 app/temp-const.cjs**，这里是转发。
// 规则：%TEMP%/shadow-play/app，或环境变量 SHADOW_TEMP_ROOT 覆盖。
// 所有跑一次就有、删掉能重建的东西都收在里面（日志/状态/录像/导出/构建产物/证据）。
import { createRequire } from 'node:module'
const requireCjs = createRequire(import.meta.url)
const tempConst = requireCjs('../app/temp-const.cjs')
export const tempRoot = tempConst.TEMP_ROOT
export const TEMP = tempConst.TEMP

const CANDIDATES = [
  path.join(projectRoot, 'node_modules', 'electron', 'dist'),
  path.join(projectRoot, '.electron', 'dist'),
]

/** 找到第一个真的含 electron.exe 的目录 */
export function findElectronDist() {
  for (const dir of CANDIDATES) {
    if (existsSync(path.join(dir, 'electron.exe'))) return dir
  }
  return null
}

/** 实际使用的 dist 目录（找不到时回退到标准位置，便于报错信息可读） */
export const electronDistPath = findElectronDist() ?? CANDIDATES[0]

/** 返回给 child_process 用的环境变量：保证所有子进程用同一份二进制 */
export function electronEnv(extra = {}) {
  const env = { ...process.env, ...extra }
  // undefined 的键在 spawn 时等于删除；这个变量会让 electron.exe 退化成纯 Node
  delete env.ELECTRON_RUN_AS_NODE
  env.ELECTRON_OVERRIDE_DIST_PATH = electronDistPath
  return env
}

/** 返回 electron.exe 的绝对路径；找不到就抛出带修复指引的错误 */
export function assertElectronDist() {
  const dir = findElectronDist()
  if (!dir) {
    throw new Error(
      '没有找到 Electron 二进制（electron.exe）。\n' +
        `已查找：\n  ${CANDIDATES.join('\n  ')}\n\n` +
        '修复办法（任选其一）：\n' +
        '  1. 在有网络的机器上装好依赖，把 electron 的 dist 目录拷过来；\n' +
        '  2. 手动把 electron-v<版本>-win32-x64.zip 解压到 ' +
        path.join(projectRoot, 'node_modules', 'electron', 'dist') + '；\n' +
        '  3. 让 electron 包认得这个二进制：\n' +
        `     在 node_modules/electron/path.txt 里写  electron.exe\n` +
        `     并把 dist 目录放到 node_modules/electron/dist/`,
    )
  }
  return path.join(dir, 'electron.exe')
}
