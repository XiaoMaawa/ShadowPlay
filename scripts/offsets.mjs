// scripts/offsets.mjs — 打印 ShadowRingHeader 的真实字段偏移
//
// JS 侧（app/preload.cjs、frontend/src/lib/ringLayout.ts、app/probe-core.cjs）
// 里的偏移常量必须和 C++ 结构体一致。改过 native/ShadowRing.h 之后跑一次这个，
// 把输出抄回 JS 侧。

import { spawnSync } from 'node:child_process'
import { existsSync, mkdirSync, writeFileSync } from 'node:fs'
import path from 'node:path'
import { projectRoot } from './electron-path.mjs'
import { msvcScript } from './msvc-env.mjs'

const buildDir = path.join(projectRoot, 'native', 'build')
mkdirSync(buildDir, { recursive: true })

const batPath = path.join(buildDir, '_tmp-offsets.bat')
writeFileSync(
  batPath,
  msvcScript(
    [
      `cd /d "${projectRoot}"`,
      'cl /nologo /utf-8 /EHsc /std:c++17 /I"native" /Fo"native\\build\\\\" ' +
        '/Fe"native\\build\\offsets.exe" native\\ring_offsets_check.cpp',
      'if errorlevel 1 exit /b 1',
      'native\\build\\offsets.exe',
    ].join('\r\n'),
  ),
  'ascii',
)

const r = spawnSync('cmd.exe', ['/c', batPath], { encoding: 'utf8', cwd: projectRoot })
process.stdout.write(r.stdout ?? '')
if (r.status !== 0) {
  process.stderr.write(r.stderr ?? '')
  process.exit(r.status ?? 1)
}
if (!existsSync(path.join(buildDir, 'offsets.exe'))) {
  console.error('[offsets] 没有生成 offsets.exe')
  process.exit(1)
}
