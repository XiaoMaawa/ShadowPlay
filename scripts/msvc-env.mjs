// scripts/msvc-env.mjs — 找到 vcvars64.bat 并给出一条能在 cmd 里调用的前缀
import { existsSync, readdirSync } from 'node:fs'
import path from 'node:path'

const CANDIDATES = [
  'C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat',
  'C:\\Program Files\\Microsoft Visual Studio\\2022\\Professional\\VC\\Auxiliary\\Build\\vcvars64.bat',
  'C:\\Program Files\\Microsoft Visual Studio\\2022\\Enterprise\\VC\\Auxiliary\\Build\\vcvars64.bat',
  'C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Auxiliary\\Build\\vcvars64.bat',
]

export function findVcvars() {
  for (const c of CANDIDATES) {
    if (existsSync(c)) return c
  }
  // 退一步：扫一遍 VS 安装目录
  const roots = [
    'C:\\Program Files\\Microsoft Visual Studio',
    'C:\\Program Files (x86)\\Microsoft Visual Studio',
  ]
  for (const root of roots) {
    if (!existsSync(root)) continue
    for (const year of safeReaddir(root)) {
      for (const edition of safeReaddir(path.join(root, year))) {
        const p = path.join(root, year, edition, 'VC', 'Auxiliary', 'Build', 'vcvars64.bat')
        if (existsSync(p)) return p
      }
    }
  }
  throw new Error('找不到 vcvars64.bat，请安装 Visual Studio 2022（含 C++ 生成工具）')
}

function safeReaddir(dir) {
  try {
    return readdirSync(dir)
  } catch {
    return []
  }
}

/** 拼一条 cmd /c 用的命令：先加载 MSVC 环境，再执行 body */
export function withMsvc(body) {
  const vcvars = findVcvars()
  // 注意：不要把 "call \"C:\\Program Files\\...\"" 直接塞进 cmd /c 的一行里，
  // 内层引号会被剥掉，导致 "'C:\Program' 不是内部或外部命令"。
  // 统一走 msvcScript() 生成临时 .bat。
  return `call "${vcvars}" >nul 2>&1 && ${body}`
}

/**
 * 生成一个临时的 .bat：加载 MSVC 环境后执行 body。
 * 比在 cmd /c 一行里拼引号可靠得多。
 */
export function msvcScript(body) {
  const vcvars = findVcvars()
  return [
    '@echo off',
    'setlocal',
    `call "${vcvars}" >nul 2>&1`,
    // 【为什么要这段兜底】vcvars64.bat 内部靠 reg.exe 读注册表来定位 Windows
    // SDK。在受限终端 / 沙箱里 reg.exe 被拦时，vcvars 只设得上 VC 自己的
    // INCLUDE/LIB，SDK 那几条全缺 —— cl 立刻死在
    // `crtdbg.h / windows.h: No such file or directory`，
    // 而报错看起来像"没装 C++ 生成工具"，特别难查。
    // 这里按已知的目录布局把 SDK 路径补到 INCLUDE/LIB 后面；正常环境下
    // vcvars 已经设好，补上的只是重复项，编译器会自己去重，行为不变。
    'for /d %%d in ("C:\\Program Files\\Microsoft Visual Studio\\2022\\*") do set "VSROOT=%%~d"',
    'for /d %%d in ("%VSROOT%\\VC\\Tools\\MSVC\\*") do set "VCTOOLS=%%~d"',
    'for /d %%d in ("C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.*") do set "SDKINC=%%~d"',
    'for %%d in ("%SDKINC%") do set "SDKVER=%%~nxd"',
    'set "SDKLIB=C:\\Program Files (x86)\\Windows Kits\\10\\Lib\\%SDKVER%"',
    'if not exist "%VCTOOLS%\\bin\\Hostx64\\x64\\cl.exe" ( echo [msvc] vcvars64 failed 1>&2 & exit /b 1 )',
    'set "PATH=%VCTOOLS%\\bin\\Hostx64\\x64;%PATH%"',
    'if not exist "%SDKINC%\\ucrt\\crtdbg.h" goto :shadow_run',
    'set "INCLUDE=%INCLUDE%;%SDKINC%\\ucrt;%SDKINC%\\um;%SDKINC%\\shared;%SDKINC%\\winrt;%SDKINC%\\cppwinrt"',
    'set "LIB=%LIB%;%SDKLIB%\\ucrt\\x64;%SDKLIB%\\um\\x64"',
    ':shadow_run',
    'cd /d "%~dp0"',
    body,
    'exit /b %errorlevel%',
    '',
  ].join('\r\n')
}
