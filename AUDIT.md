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
| ✅ | `SegmentRecorder.*` | **已重写**：改调 `MfEncoder`（进程内 SinkWriter），不再起子进程。喂帧线程「按帧数补帧、允许卡不允许变速」的核心时间轴原样保留 |
| ✅ | `Exporter.*` | **已重写**：`IMFSourceReader` 读分片压缩样本 + `IMFSinkWriter` 重封装，无子进程。尺寸一致的片子走 passthrough（等价 `ffmpeg -c copy`）；尺寸不一致时画布取**最大**尺寸、小画面贴左上角补黑边、**绝不拉伸**（重编码路径）。分片快照、`targetFps_` 整数倍插帧、导出时长校验都保留。**时间轴帧率必须按源分片真能给出的帧率钳**（`canRaiseFps`），否则配 240 帧实际录成 165fps 时成片被压成 13.7 秒（见 DESIGN.md 9.6.1）——两条路（直拷 / 重编码）都要钳，只改一条等于没改 |
| ✅ | `MfEncoder.*`（新增） | 进程内 MF 分片编码器：**自己把 BGRA 转 NV12** 再喂 → fMP4 分片槽位循环。**必须关 B 帧**（`SetInputMediaType` 第三参数传 `CODECAPI_AVEncMPVDefaultBPictureCount=0`），否则分片拼接 POC 对不上、成品解不出来。**必须开 `MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS`**（默认关），否则只挑到软件编码器 `mfh264enc.dll`、1440p 稳定吃 6.8 核。**必须自己转 NV12、别直喂 BGRA**：没有 D3D manager 时 SinkWriter 只能用 `msvproc.dll` 在 CPU 上转，实测 3.79~6.82 核且颜色算偏（黄里掺绿），自转后 0.80~0.88 核（见 DESIGN.md 9.2.1）。**帧率超硬件吞吐上限会自动降档**（1440p 上限 172fps，见 DESIGN.md 9.6.1）。**尺寸变化**：变大立刻切片（新片精确尺寸）、变小贴左上角补黑，文件帧尺寸永远等于真实画面尺寸 —— 过渡画布 2 倍预留方案已废弃（会把导出画布污染成 4096x2912、NVENC 超限录制定格，见 DESIGN.md 9.5.2）。**已知遗留**：每切一片 SinkWriter 不完全回收句柄（2 秒一片≈每秒 +10 个），`MF_SINK_WRITER_D3D_MANAGER` 那条路会撞 `MF_E_UNSUPPORTED_D3D_TYPE 0xC00D6D76`，根治需全 GPU 通路 |
| ✅ | `Utf8Path.*`（新增） | 从 FfmpegUtil 抽出的纯路径/编码工具（FromUtf8/ToUtf8/PathToUtf8/PathFromUtf8），与 ffmpeg 无关，继续被 Profiles/EnvPath/StatusFile 使用 |
| ❌ | ~~`FfmpegUtil.*`~~ | **已删除**。`ResolveFfmpeg`/`BuildCommandLine`/`QuoteArg` 随 ffmpeg 一起废弃，纯工具函数迁到 `Utf8Path.*` |
| ✅ | `WindowCapture.*` | WGC 抓窗。**帧池按屏幕尺寸建一次、永不重建**（原按窗口尺寸建，窗口一变就要重建整个会话，而泵线程被设计成永不 return，每次重建泄漏 17 个线程；探针实测大池子全程持续投帧）。`Capture()` 每帧读 `get_ContentSize()` 只更新对外宣称的宽高，staging 按内容尺寸建 + `CopySubresourceRegion` 只拷左上角那块（`box.right/bottom` 是开区间）。窗口源一律 `put_IsCursorCaptureEnabled(FALSE)`（不画光标） |
| ✅ | `ScreenCapture.*` | DXGI 桌面复制。**桌面源自己画鼠标指针**：`DXGI_OUTDUPL_FRAME_INFO` 的 `PointerPosition` + `GetFramePointerShape()`，叠在调用方缓冲的副本上（不叠 staging，避免污染共享帧）。判"有没有光标更新"只能看 `LastMouseUpdateTime`（为 0 时 Position 是 (0,0)、Visible 是 FALSE，看它会让光标疯狂闪烁） |
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
| ⚠️ | `test-pipe.mjs` `test-selfheal.mjs` … | 部分测试脚本假设「分片是 `.ts`、靠 ffmpeg 合并」，MF 改造后要跟改断言（现分片是 `.mp4`/fMP4） |
| ✅ | `probe-*.mjs` | 验收利器，保留 |
| ❌ | `migrate-record-dirs.mjs` | 一次性迁移脚本，任务应已完成，建议归档删除 |

## 明确不入库的状态（.gitignore 已覆盖）

`node_modules/`、`.electron/`（Electron 二进制副本，靠 PATH 或正常安装即可）、`dist/`、`export/`、`native/build/`、`*_svc*.txt`
