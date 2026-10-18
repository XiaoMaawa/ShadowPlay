// scripts/build-native.mjs — 编译原生部分
//
//   node scripts/build-native.mjs            编译采集服务 + Electron 版 addon
//   node scripts/build-native.mjs --node     额外再链接一个纯 Node 版 addon（自检用）
//
// 为什么 addon 要链接两次：
//   N-API 的入口点（napi_*）由宿主进程导出。node.exe 和 electron.exe 都导出
//   同一组 N-API 符号，但它们各自有一个 import library，导入描述符里写着
//   宿主可执行文件名（node.exe / electron.exe）。
//   所以同一份 .obj 要分别链一次：
//     shadow_ring.node        <- 指向 electron.exe，给 Electron 用
//     shadow_ring.nodejs.node <- 指向 node.exe，给纯 Node 自检用
//   两者实现完全相同（同一份 .obj），只是导入表不同。
//
// 实现细节：所有 cl/lib/link 调用都写进一个临时 .bat 再执行。
// 直接在 cmd /c 的一行里拼 "C:\Program Files\..." 会因为引号被剥离而失败。

import { spawnSync } from 'node:child_process'
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs'
import path from 'node:path'
import { projectRoot } from './electron-path.mjs'
import { msvcScript } from './msvc-env.mjs'

const BUILD_DIR = path.join(projectRoot, 'native', 'build')
const WANT_NODE_VARIANT = process.argv.includes('--node')

/** 把一段批处理写到 native/build 下并执行 */
function runBatch(label, body) {
  const batPath = path.join(BUILD_DIR, '_tmp-build.bat')
  // %~dp0 指向 native/build，所以先切到仓库根
  const script = msvcScript(`cd /d "${projectRoot}"\r\n${body}`)
  writeFileSync(batPath, script, 'ascii')

  process.stdout.write(`\n=== ${label} ===\n`)
  const r = spawnSync('cmd.exe', ['/c', batPath], { stdio: 'inherit', cwd: projectRoot })
  try {
    rmSync(batPath, { force: true })
  } catch {
    /* 忽略 */
  }
  if (r.error) throw r.error
  if (r.status !== 0) {
    process.stderr.write(`\n[build-native] ${label} 失败（退出码 ${r.status}）\n`)
    process.exit(r.status ?? 1)
  }
}

/** 用 dumpbin 取出某个 PE 导出的 napi_* 符号，并生成对应的 .def */
function generateImportLib(exePath, name) {
  const dump = path.join(BUILD_DIR, `${name}-exports.txt`)
  runBatch(`dumpbin ${path.basename(exePath)}`, `dumpbin /nologo /exports "${exePath}" > "${dump}"`)

  const text = readFileSync(dump, 'utf8')
  const symbols = []
  const re = /^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]{8}\s+(napi_\w+)\s*$/gm
  let m
  while ((m = re.exec(text)) !== null) symbols.push(m[1])
  if (symbols.length === 0) throw new Error(`没有从 ${exePath} 解析出任何 napi_* 导出`)

  const defPath = path.join(BUILD_DIR, `${name}.def`)
  writeFileSync(defPath, `LIBRARY ${path.basename(exePath)}\r\nEXPORTS\r\n${symbols.join('\r\n')}\r\n`, 'ascii')
  console.log(`[build-native] ${path.basename(exePath)}: ${symbols.length} 个 napi 导出`)
  return defPath
}

mkdirSync(BUILD_DIR, { recursive: true })

// ---------------------------------------------------------------------------
// 1) 采集服务
// ---------------------------------------------------------------------------
runBatch(
  '采集服务 shadow-capture.exe',
  [
    'cl /nologo /utf-8 /std:c++17 /permissive- /EHsc /O2 /MD /W3 /DNDEBUG ^',
    '   /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DUNICODE /D_UNICODE ^',
    '   /I"native" /Fo"native\\build\\\\" /Fd"native\\build\\shadow-capture.pdb" ^',
    '   native\\backend\\main.cpp native\\backend\\ScreenCapture.cpp native\\backend\\FrameRing.cpp ^',
    '   native\\backend\\WindowCapture.cpp ^',
    '   native\\backend\\Profiles.cpp native\\backend\\WindowEnum.cpp ^',
    '   native\\backend\\Session.cpp native\\backend\\SegmentRecorder.cpp ^',
    '   native\\backend\\Exporter.cpp native\\backend\\FfmpegUtil.cpp ^',
    '   native\\backend\\EnvPath.cpp ^',
    '   native\\backend\\StatusFile.cpp ^',
    '   /Fe"native\\build\\shadow-capture.exe" ^',
    '   /link /SUBSYSTEM:CONSOLE /OPT:REF /OPT:ICF ^',
    '   d3d11.lib dxgi.lib user32.lib gdi32.lib ole32.lib kernel32.lib advapi32.lib ^',
    // windowsapp.lib 提供 WinRT 的激活底层：RoGetActivationFactory / WindowsCreateString /
    // WindowsDeleteString。WindowCapture.cpp 用 Windows.Graphics.Capture 抓窗口
    // （被挡住也能录）时离不开这三个符号，漏了就是 LNK2019。
    '   winmm.lib windowsapp.lib',
  ].join('\r\n'),
)

// ---------------------------------------------------------------------------
// 2) addon：编译成 .obj（只编一次）
// ---------------------------------------------------------------------------
const objPath = path.join(BUILD_DIR, 'shadow_ring.obj')
runBatch(
  '编译 shadow_ring.obj',
  [
    'cl /nologo /utf-8 /std:c++17 /permissive- /EHsc /O2 /MD /W3 /DNDEBUG ^',
    '   /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DUNICODE /D_UNICODE /DNAPI_VERSION=8 ^',
    '   /I"bridge\\shim" /I"native" ^',
    `   /Fo"${objPath}" /Fd"native\\build\\shadow_ring.pdb" ^`,
    '   /c bridge\\shadow_ring.cc',
  ].join('\r\n'),
)

// ---------------------------------------------------------------------------
// 2b) 导出那部分也编进 addon
//
// Exporter 编进插件，界面才能直接调 C++ 导出（原因见 bridge/shadow_ring.cc
// 里 ExportStart 上方的长注释）。这带来一个必须留意的约束：
// Exporter.cpp / FfmpegUtil.cpp 会被**编译两遍**（一遍进服务，一遍进插件），
// 两边是各自独立的副本。它们目前只依赖 Windows API 和 ffmpeg 命令行，
// 没有需要跨副本共享的全局状态，所以是安全的 —— 但如果以后往里加
// "记录当前导出状态"这类全局变量，要记住服务和插件的副本互不可见。
//
// 单独编成 .obj 而不是把 .cpp include 进来：一个是编译单元更清楚，
// 另一个是 /Fo 的路径不会互相覆盖（服务那批 .obj 在 native\build\ 根下）。
//
// EnvPath.cpp 也在里面：Exporter 用它展开导出目录里的 `%TEMP%`，而
// Profiles.cpp（服务和 addon 共用的那个 ExpandEnv 定义）并不参与 addon
// 的编译，漏掉就是 LNK2019。
// ---------------------------------------------------------------------------
const addonExtraObjs = []
for (const src of ['Exporter', 'FfmpegUtil', 'EnvPath']) {
  const o = path.join(BUILD_DIR, `addon_${src}.obj`)
  runBatch(
    `编译 addon_${src}.obj`,
    [
      'cl /nologo /utf-8 /std:c++17 /permissive- /EHsc /O2 /MD /W3 /DNDEBUG ^',
      '   /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DUNICODE /D_UNICODE ^',
      '   /I"bridge\\shim" /I"native" ^',
      `   /Fo"${o}" /Fd"native\\build\\addon-extra.pdb" ^`,
      `   /c native\\backend\\${src}.cpp`,
    ].join('\r\n'),
  )
  addonExtraObjs.push(o)
}

// ---------------------------------------------------------------------------
// 3) 分别链接 Electron 版 / Node 版
// ---------------------------------------------------------------------------
const electronExe = path.join(projectRoot, '.electron', 'dist', 'electron.exe')
if (!existsSync(electronExe)) {
  throw new Error(`找不到 electron.exe：${electronExe}（先完成一次 Electron 下载）`)
}

const hosts = [{ exe: electronExe, name: 'electron', out: 'shadow_ring.node' }]
if (WANT_NODE_VARIANT) {
  hosts.push({ exe: process.execPath, name: 'node', out: 'shadow_ring.nodejs.node' })
}

for (const host of hosts) {
  const defPath = generateImportLib(host.exe, host.name)
  const libPath = path.join(BUILD_DIR, `${host.name}.lib`)
  runBatch(`生成 ${host.name}.lib`, `lib /nologo /def:"${defPath}" /machine:x64 /out:"${libPath}"`)

  const outPath = path.join(BUILD_DIR, host.out)
  runBatch(
    `链接 ${host.out}`,
    [
      'link /nologo /DLL /OPT:REF /OPT:ICF /INCREMENTAL:NO ^',
      `   /OUT:"${outPath}" "${objPath}" ` +
        addonExtraObjs.map((o) => `"${o}"`).join(' ') +
        ` "${libPath}" ^`,
      '   kernel32.lib user32.lib advapi32.lib shell32.lib',
    ].join('\r\n'),
  )

  for (const ext of ['.lib', '.exp']) {
    const junk = outPath.replace(/\.node$/, ext)
    if (existsSync(junk)) rmSync(junk, { force: true })
  }
}

process.stdout.write('\n[build-native] 完成\n')
