// scripts/build-native.mjs — 编译原生部分
//
//   node scripts/build-native.mjs            编译全部产物
//   node scripts/build-native.mjs --node     额外再链接一个纯 Node 版插件（自检用）
//
// 产物有三个：
//
//   shadow-capture.exe        采集服务。**独立进程**，界面退出它也继续跑
//                             （detached 常驻）。内部零子进程：编码走进程内的
//                             Media Foundation，导出走 IMFSourceReader +
//                             IMFSinkWriter，不再 spawn ffmpeg.exe。
//
//   shadow_ring.node          渲染端消费共享内存帧环的插件（Electron 版）
//   shadow_ring.nodejs.node   同上，指向 node.exe（加 --node 时才编，自检用）
//
// 为什么 addon 要链两次：
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

// 采集服务（shadow-capture.exe）的源文件。入口是 exe_main.cpp，它调
// main.cpp 里的 shadow::ServiceMain —— 拆成两个文件是为了 argv 走
// CommandLineToArgvW（UTF-16 → UTF-8），中文路径不会变成乱码。
const SERVICE_SOURCES = [
  'native\\backend\\exe_main.cpp',
  'native\\backend\\main.cpp',
  'native\\backend\\ScreenCapture.cpp',
  'native\\backend\\FrameRing.cpp',
  'native\\backend\\WindowCapture.cpp',
  'native\\backend\\Profiles.cpp',
  'native\\backend\\WindowEnum.cpp',
  'native\\backend\\Session.cpp',
  'native\\backend\\SegmentRecorder.cpp',
  'native\\backend\\Exporter.cpp',
  'native\\backend\\MfEncoder.cpp',
  'native\\backend\\Utf8Path.cpp',
  'native\\backend\\EnvPath.cpp',
  'native\\backend\\StatusFile.cpp',
]

const SERVICE_LIBS = [
  // Media Foundation：MfEncoder（IMFSinkWriter）和 Exporter（IMFSourceReader）
  // 都在进程内做编解码/换封装，不再起 ffmpeg.exe 子进程。
  //   mfplat     -- MFStartup / MFCreateSinkWriterFromURL / MFCreateSourceReaderFromURL
  //   mfreadwrite-- SinkWriter / SourceReader 的实现体
  //   mfuuid     -- MFVideoFormat_H264 这类 GUID
  //   mf         -- 少数老接口（ICodecAPI 的 CODECAPI_* 属性键在 wmcodecdspuuid 里）
  //   wmcodecdspuuid -- CODECAPI_AVEncMPVDefaultBPictureCount / AVEncCommonLowLatency
  'd3d11.lib', 'dxgi.lib', 'user32.lib', 'gdi32.lib', 'ole32.lib', 'oleaut32.lib',
  'kernel32.lib', 'advapi32.lib', 'mfplat.lib', 'mfreadwrite.lib', 'mfuuid.lib',
  'mf.lib', 'wmcodecdspuuid.lib',
  // windowsapp.lib 提供 WinRT 的激活底层：RoGetActivationFactory / WindowsCreateString /
  // WindowsDeleteString。WindowCapture.cpp 用 Windows.Graphics.Capture 抓窗口
  // （被挡住也能录）时离不开这三个符号，漏了就是 LNK2019。
  'winmm.lib', 'windowsapp.lib',
  // exe_main.cpp 用 CommandLineToArgvW
  'shell32.lib',
]

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

const electronExe = path.join(projectRoot, '.electron', 'dist', 'electron.exe')
if (!existsSync(electronExe)) {
  throw new Error(`找不到 electron.exe：${electronExe}（先完成一次 Electron 下载）`)
}

const hosts = [{ exe: electronExe, name: 'electron', suffix: '' }]
if (WANT_NODE_VARIANT) hosts.push({ exe: process.execPath, name: 'node', suffix: '.nodejs' })

// ---------------------------------------------------------------------------
// 1) 采集服务 shadow-capture.exe
// ---------------------------------------------------------------------------
runBatch(
  '采集服务 shadow-capture.exe',
  [
    'cl /nologo /utf-8 /std:c++17 /permissive- /EHsc /O2 /MD /W3 /DNDEBUG ^',
    '   /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DUNICODE /D_UNICODE ^',
    // /Fo 的路径末尾必须是**两个反斜杠**（"native\build\\"）：
    // Windows 的命令行解析里 `\"` 是「转义的双引号」，只写一个反斜杠会让
    // 引号被吃掉，cl 收到的就是一串以 `"` 结尾的乱参数，报 D8003
    // 「缺少源文件名」。原来那版 `/Fo"native\build\\"` 看着奇怪，正是这个原因。
    '   /I"native" /Fo"native\\build\\\\" /Fd"native\\build\\shadow-capture.pdb" ^',
    ...SERVICE_SOURCES.map((s) => `   ${s} ^`),
    '   /Fe"native\\build\\shadow-capture.exe" ^',
    '   /link /SUBSYSTEM:CONSOLE /OPT:REF /OPT:ICF ^',
    `   ${SERVICE_LIBS.join(' ')}`,
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
// 2b) 导出/编码那部分也编进 addon
//
// Exporter 编进插件，界面才能直接调 C++ 导出（原因见 bridge/shadow_ring.cc
// 里 ExportStart 上方的长注释）。这带来一个必须留意的约束：
// Exporter.cpp / MfEncoder.cpp / Utf8Path.cpp 会被**编译两遍**（服务、插件），
// 各份是彼此独立的副本。它们目前只依赖 Windows API 和 Media Foundation，
// 没有需要跨副本共享的全局状态，所以是安全的 —— 但 MfAcquire() 的引用计数
// 就是典型的"每个副本各数各的"：各份和自己的 MFStartup 配对，互不影响。
// 如果以后往里加"记录当前导出状态"这类全局变量，要记住这几份副本互不可见。
//
// EnvPath.cpp 也在里面：Exporter 用它展开导出目录里的 `%TEMP%`，而
// Profiles.cpp（服务和 addon 共用的那个 ExpandEnv 定义）并不参与 addon
// 的编译，漏掉就是 LNK2019。
// ---------------------------------------------------------------------------
const addonExtraObjs = []
for (const src of ['Exporter', 'MfEncoder', 'Utf8Path', 'EnvPath']) {
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
// 3) 分别链接 Electron 版 / Node 版 shadow_ring
// ---------------------------------------------------------------------------
for (const host of hosts) {
  const defPath = generateImportLib(host.exe, host.name)
  const libPath = path.join(BUILD_DIR, `${host.name}.lib`)
  runBatch(`生成 ${host.name}.lib`, `lib /nologo /def:"${defPath}" /machine:x64 /out:"${libPath}"`)

  const outPath = path.join(BUILD_DIR, `shadow_ring${host.suffix}.node`)
  runBatch(
    `链接 shadow_ring${host.suffix}.node`,
    [
      'link /nologo /DLL /OPT:REF /OPT:ICF /INCREMENTAL:NO ^',
      `   /OUT:"${outPath}" "${objPath}" ` +
        addonExtraObjs.map((o) => `"${o}"`).join(' ') +
        ` "${libPath}" ^`,
      '   kernel32.lib user32.lib advapi32.lib shell32.lib ole32.lib ^',
      // 插件里也有 MfEncoder/Exporter 的那份副本，MF 的库一个都不能少。
      //   ole32    -- CoInitializeEx / CoUninitialize / PropVariantClear
      //   oleaut32 -- VariantInit（ICodecAPI 的 PROPVARIANT 属性）
      //   d3d11/dxgi -- MfSharedDeviceReady 里的 D3D11CreateDevice /
      //                 MFCreateDXGIDeviceManager（MfEncoder 常驻共用设备）
      '   mfplat.lib mfreadwrite.lib mfuuid.lib mf.lib wmcodecdspuuid.lib oleaut32.lib ^',
      '   d3d11.lib dxgi.lib',
    ].join('\r\n'),
  )

  for (const ext of ['.lib', '.exp']) {
    const junk = outPath.replace(/\.node$/, ext)
    if (existsSync(junk)) rmSync(junk, { force: true })
  }
}

process.stdout.write('\n[build-native] 完成\n')
