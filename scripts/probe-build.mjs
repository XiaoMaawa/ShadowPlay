// scripts/probe-build.mjs — 编译一次性探针程序（不参与产品构建）
//
//   node scripts/probe-build.mjs <源.cpp> <输出.exe> [--with-backend] [--with-mf] [--with-wgc]
//
// 之所以单独写一个：build-native.mjs 只编产品产物（服务 exe + 两个插件），
// 探针要临时加源文件/库，混进去会把产品构建搞脏。

import { spawnSync } from 'node:child_process'
import { existsSync, mkdirSync, writeFileSync } from 'node:fs'
import path from 'node:path'
import { projectRoot } from './electron-path.mjs'
import { msvcScript } from './msvc-env.mjs'

const BUILD_DIR = path.join(projectRoot, 'native', 'build')
const args = process.argv.slice(2)
const src = args[0]
const out = args[1]
const withBackend = args.includes('--with-backend')
const withMf = args.includes('--with-mf') || withBackend
const withWgc = args.includes('--with-wgc') || withBackend

if (!src || !out) {
  console.error('用法：node scripts/probe-build.mjs <源.cpp> <输出.exe> [--with-backend] [--with-mf] [--with-wgc]')
  process.exit(1)
}
if (!existsSync(path.join(projectRoot, src))) {
  console.error(`源文件不存在：${src}`)
  process.exit(1)
}

mkdirSync(BUILD_DIR, { recursive: true })

const BACKEND_SOURCES = [
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

const MF_LIBS = [
  'mfplat.lib', 'mfreadwrite.lib', 'mfuuid.lib', 'mf.lib',
  'wmcodecdspuuid.lib', 'ole32.lib', 'oleaut32.lib',
  'winmm.lib', 'windowsapp.lib', 'CoreMessaging.lib',
]
const BASE_LIBS = [
  'd3d11.lib', 'dxgi.lib', 'user32.lib', 'gdi32.lib', 'shell32.lib',
  'kernel32.lib', 'advapi32.lib',
]

const sources = [src.replace(/\//g, '\\')]
if (withBackend) sources.push(...BACKEND_SOURCES)

const libs = [...BASE_LIBS]
if (withMf) libs.push(...MF_LIBS)
if (withWgc) libs.push('windowsapp.lib', 'CoreMessaging.lib', 'dwmapi.lib')

const objDir = path.join(BUILD_DIR, 'probe-obj')
mkdirSync(objDir, { recursive: true })

const lines = []
for (const s of sources) {
  const obj = path.join(objDir, path.basename(s).replace(/\.cpp$/, '.obj'))
  // /utf-8 必须给：源文件里有中文注释，按代码页 936 解会把多字节字符
  // 拆错，报出一堆莫名其妙的 C2065（实测过）。
  lines.push(`cl /nologo /utf-8 /c /EHsc /std:c++17 /permissive- /O2 /MD /W3 /DNOMINMAX /DUNICODE /D_UNICODE /I"${projectRoot}\\native" /Fo"${obj}" "${s}"`)
  lines.push('if errorlevel 1 exit /b 1')
}
const objs = sources
  .map(s => `"${path.join(objDir, path.basename(s).replace(/\.cpp$/, '.obj'))}"`)
  .join(' ')
lines.push(`link /nologo /OUT:"${path.join(projectRoot, out)}" ${objs} ${libs.join(' ')}`)
lines.push('if errorlevel 1 exit /b 1')

const bat = path.join(BUILD_DIR, '_probe-build.bat')
// msvcScript 里已经 `cd /d "%~dp0"`（= native/build），必须再切回仓库根，
// 否则下面这些相对路径的源文件全找不到（C1083）。
writeFileSync(bat, msvcScript(`cd /d "${projectRoot}"\r\n` + lines.join('\r\n')), 'utf8')
const r = spawnSync('cmd.exe', ['/c', bat], { stdio: 'inherit', windowsHide: true })
process.exit(r.status ?? 1)
