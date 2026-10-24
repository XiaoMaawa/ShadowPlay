// scripts/probe-aac.mjs —— 编译并运行 AAC passthrough 可行性探针
import { spawnSync } from 'node:child_process'
import { writeFileSync, rmSync } from 'node:fs'
import path from 'node:path'
import { projectRoot } from './electron-path.mjs'
import { msvcScript } from './msvc-env.mjs'

const BUILD_DIR = path.join(projectRoot, 'native', 'build')
const batPath = path.join(BUILD_DIR, '_probe-aac.bat')

const body = [
  `cl /nologo /utf-8 /std:c++17 /EHsc /O2 /MD /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^`,
  `   native\\probe_aac_passthrough.cpp ^`,
  `   /Fe"native\\build\\probe_aac_passthrough.exe" ^`,
  `   /link mfplat.lib mfreadwrite.lib mfuuid.lib mf.lib wmcodecdspuuid.lib ole32.lib oleaut32.lib`,
].join('\r\n')

const script = msvcScript(`cd /d "${projectRoot}"\r\n${body}`)
writeFileSync(batPath, script, 'ascii')

const r = spawnSync('cmd.exe', ['/c', batPath], { stdio: 'inherit', cwd: projectRoot })
try { rmSync(batPath, { force: true }) } catch {}
if (r.status !== 0) {
  process.stderr.write('\n[probe-aac] 编译失败\n')
  process.exit(r.status ?? 1)
}

const exe = path.join(BUILD_DIR, 'probe_aac_passthrough.exe')
const run = spawnSync(exe, [], { stdio: 'inherit', cwd: projectRoot })
process.exit(run.status ?? 0)
