# 项目文件标记（改造基线 v0.1）

> 生成于改造（Electron 自动起服务 / MF 编码重构）之前。标记含义：
> ✅ 合适（保留并继续用）｜⚠️ 待定（改造中要动或要改设计）｜❌ 不合适（建议剔除/不再增长）

## 根目录

| 标记 | 文件 | 说明 |
|---|---|---|
| ✅ | `package.json` | 工程入口，正常 |
| ✅ | `DESIGN.md` / `README.md` | 架构与用法文档，维护良好 |
| ✅ | `profiles.json` | 运行时配置，真实用户数据 |
| ❌ | `profiles.json.bak` | 手工备份残留，git 已可回滚，建议删 |
| ❌ | `_svc.out.txt` / `_svc.err.txt` | 临时日志落在仓库根，已入 .gitignore，建议删 |

## app/（Electron 主进程）

| 标记 | 文件 | 说明 |
|---|---|---|
| ✅ | `main.cjs` | 核心，注释详实；本次改造主要动它（服务生死、提示） |
| ✅ | `preload.cjs` | 环同步设计正确；「等待采集服务」提示文案要改 |
| ✅ | `service-const.cjs` / `temp-const.cjs` / `temp-clean.cjs` | 常量与临时目录治理，职责清晰 |
| ⚠️ | `probe-core.cjs` | 只被探测/测试用，杂糅在 app/ 里，可挪到 scripts/ |
| ❌ | 缺失 `.gitignore`（根）缺失项 | `export/`、`dist/` 等已补进根 .gitignore |

## native/（C++ 后端）

| 标记 | 文件 | 说明 |
|---|---|---|
| ✅ | `main.cpp` `Session.*` `FrameRing.*` `ScreenCapture.*` `RingNames.h` `StatusFile.*` `Profiles.*` `WindowEnum.*` `EnvPath.*` | 核心采集链路，结构清楚 |
| ⚠️ | `SegmentRecorder.*` | 现在是「起 ffmpeg.exe 子进程」实现；本次重写为 Media Foundation 进程内编码 |
| ⚠️ | `Exporter.*` | concat 靠 ffmpeg 子进程；本次重写 |
| ❌ | `FfmpegUtil.*` | 专职找 ffmpeg.exe；MF 路线下整体废弃（BF 用到 FromUtf8/QuoteArg 则挪走） |
| ✅ | `WindowCapture.*` | WGC 抓窗，保留 |
| ⚠️ | `ShadowRing.h`（根） | 与 backend 头在同一层，位置略别扭，可接受 |
| ✅ | `ring_offsets_check.cpp` | 偏移自检小工具，有用 |

## bridge/（N-API 插件）

| 标记 | 文件 | 说明 |
|---|---|---|
| ✅ | `shadow_ring.cc` `binding.gyp` | 共享内存零拷贝的关键，设计正确 |
| ✅ | `shim/*.h` | 自带 N-API 头，避免依赖 node headers，聪明 |

## frontend/（Vue 界面）

| 标记 | 文件 | 说明 |
|---|---|---|
| ✅ | `src/App.vue` `src/composables/usePreview.ts` `src/lib/*` | 分层合理；`usePreview.ts` 里「等待采集服务」遮罩/文案要简化 |
| ✅ | Vite/TS 配置齐全 | 正常 |
| ✅ | `package-lock.json` | 入库正确 |

## scripts/

| 标记 | 文件 | 说明 |
|---|---|---|
| ✅ | `build-native.mjs` `build-web.mjs` `dev*.mjs` `launch.mjs` `electron-path.mjs` `msvc-env.mjs` | 构建链工作正常 |
| ⚠️ | `service.mjs` | MF+界面托管后仍保留 CLI 用途，但「必须另开终端」的定位要变成可选 |
| ⚠️ | `test-pipe.mjs` `test-selfheal.mjs` … | 测试脚本引用 ffmpeg 行为的，重写后要跟改 |
| ✅ | `probe-*.mjs` | 验收利器，保留 |
| ❌ | `migrate-record-dirs.mjs` | 一次性迁移脚本，任务应已完成，建议归档删除 |

## 明确不入库的状态（.gitignore 已覆盖）

`node_modules/`、`.electron/`（Electron 二进制副本，靠 PATH 或正常安装即可）、`dist/`、`export/`、`native/build/`、`*_svc*.txt`
