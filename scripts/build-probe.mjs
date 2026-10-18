// scripts/build-probe.mjs — 单独编译 native/build/artifacts/wgc-probe.cpp
//
// 用途：把 WGC 的每一步拆开验证。走和 build-native.mjs 完全一样的
// msvcScript()（先 call vcvars64.bat），所以 INCLUDE 里的 SDK 路径
// 由 vcvars 自己配好，不需要手工指定 winrt / um 目录。
//
//   node scripts/build-probe.mjs

import { spawnSync } from 'node:child_process'
import { mkdirSync, rmSync, writeFileSync } from 'node:fs'
import path from 'node:path'
import { projectRoot } from './electron-path.mjs'
import { msvcScript } from './msvc-env.mjs'

const BUILD_DIR = path.join(projectRoot, 'native', 'build')
const ART = path.join(BUILD_DIR, 'artifacts')
mkdirSync(ART, { recursive: true })

const batPath = path.join(BUILD_DIR, '_tmp-probe.bat')
// 写成**一行**：用 ^ 续行在 msvcScript 生成的 bat 里不好使
// （cl 会报 D8003 缺少源文件名），单行最稳。
//
// /Fo 的路径**不加引号**：cmd 会把 "...\" 里的 \" 当成转义引号，
// 导致引号不闭合、后面的源文件名被一起吞掉，cl 就报 D8003。
// 这里路径没有空格，去掉引号最干净（项目原代码用 /Fo"native\build\\"
// 的双反斜杠也是躲同一个坑）。
const body =
  'cl /nologo /utf-8 /std:c++17 /permissive- /EHsc /O2 /MD /W3 /DNDEBUG ' +
  '/DWIN32_LEAN_AND_MEAN /DNOMINMAX /DUNICODE /D_UNICODE ' +
  '/Inative /Fonative\\build\\artifacts\\ ' +
  'native\\build\\artifacts\\wgc-probe.cpp ' +
  '/Fenative\\build\\artifacts\\wgc-probe.exe ' +
  '/link /SUBSYSTEM:CONSOLE ' +
  'd3d11.lib dxgi.lib user32.lib gdi32.lib ole32.lib kernel32.lib advapi32.lib ' +
  'windowsapp.lib'

writeFileSync(batPath, msvcScript(`cd /d "${projectRoot}"\r\n${body}`), 'ascii')
if (process.env.KEEP_PROBE_BAT) {
  console.log(`[build-probe] bat 留在 ${batPath}`)
  console.log('--- bat 内容 ---')
  console.log(msvcScript(`cd /d "${projectRoot}"\r\n${body}`))
  console.log('--- 结束 ---')
}

console.log('=== 编译 wgc-probe.exe ===')
const r = spawnSync('cmd.exe', ['/c', batPath], { stdio: 'inherit', cwd: projectRoot })
try {
  rmSync(batPath, { force: true })
} catch {
  /* 忽略 */
}
if (r.error) throw r.error
if (r.status !== 0) {
  console.error(`[build-probe] 失败（退出码 ${r.status}）`)
  process.exit(r.status ?? 1)
}
console.log('[build-probe] 完成')
