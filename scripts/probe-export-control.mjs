// scripts/probe-export-control.mjs —— 对照组：不改配置直接导出，检查音频是否也短（排除「改回放时长」因素）。
//
// 用法：node scripts/probe-export-control.mjs
import { spawn } from 'node:child_process'
import { existsSync, mkdirSync, readFileSync, readdirSync, rmSync, writeFileSync, statSync } from 'node:fs'
import path from 'node:path'
import { projectRoot, tempRoot } from './electron-path.mjs'

const backend = path.join(projectRoot, 'native', 'build', 'shadow-capture.exe')
const PROOF = path.join(tempRoot, 'proofs', 'export-control')
const RECORD_ROOT = path.join(PROOF, 'records')
const EXPORT_DIR = path.join(PROOF, 'exports')
const PROFILES = path.join(PROOF, 'profiles.json')
const STATUS = path.join(tempRoot, 'status', 'status-v1.json')
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))

rmSync(PROOF, { recursive: true, force: true })
mkdirSync(RECORD_ROOT, { recursive: true }); mkdirSync(EXPORT_DIR, { recursive: true }); rmSync(STATUS, { force: true })
writeFileSync(PROFILES, JSON.stringify({
  version: 1, recordRoot: RECORD_ROOT, exportDir: EXPORT_DIR,
  profiles: [{ id: 'p-desk', num: 1, name: 'ctrl', enabled: true,
    source: { kind: 'monitor', monitorIndex: 0, clientAreaOnly: false, wgc: true },
    video: { fps: 30, bitrateMbps: 6, replaySeconds: 120, segmentSeconds: 2, scaleWidth: 0, scaleHeight: 0 },
    createdAt: '2026-01-01T00:00:00.000Z' }],
}, null, 2))

const readStatus = () => { try { return JSON.parse(readFileSync(STATUS, 'utf8')) } catch { return null } }
const desk = () => readStatus()?.items?.find((s) => s.id === 'p-desk') ?? null

const beeper = spawn('powershell', ['-NoProfile', '-Command', 'for ($i=0;$i -lt 40;$i++){[System.Media.SystemSounds]::Beep.Play();Start-Sleep -Milliseconds 400}'], { windowsHide: true, stdio: 'ignore' })
const child = spawn(backend, ['--profiles', PROFILES], { cwd: projectRoot, stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true })
const logs = []
child.stdout.on('data', (d) => logs.push(...d.toString().split(/\r?\n/).filter((l) => l.trim())))
child.stderr.on('data', (d) => logs.push(...d.toString().split(/\r?\n/).filter((l) => l.trim())))
const send = (c) => child.stdin.write(JSON.stringify(c) + '\n')

async function main() {
  const t0 = Date.now()
  while (Date.now() - t0 < 20000) { await sleep(200); const s = desk(); if (s && s.state === 'live' && s.audioActive) break }
  await sleep(6000)
  send({ cmd: 'flush', id: 'p-desk' }); await sleep(600)
  const before = readdirSync(EXPORT_DIR).filter((f) => f.endsWith('.mp4')).length
  send({ cmd: 'export', id: 'p-desk' })
  const te = Date.now(); let result = null
  while (Date.now() - te < 60000) { await sleep(200); const s = desk(); if (!s) continue; const files = readdirSync(EXPORT_DIR).filter((f) => f.endsWith('.mp4')).length; if (files > before && !s.exportRunning) { result = s; break } }
  child.stdin.end(); child.kill(); beeper.kill()
  if (!result) { console.log('[ctrl] 导出超时'); process.exit(2) }
  const out = String(result.exportOutPath || '')
  console.log('[ctrl] 导出 ok=' + result.exportOk + ' detail=' + result.exportDetail)
  console.log('[ctrl] 产物=' + out)
  for (const l of logs) if (/audio:|export_plan/.test(l)) console.log('[log] ' + l)
  process.exit(0)
}
main()
