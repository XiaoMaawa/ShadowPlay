# 项目文件标记（改造基线 v0.1）

> 生成于改造（Electron 自动起服务 / MF 编码重构）之前。标记含义：
> ✅ 合适（保留并继续用）｜⚠️ 待定（改造中要动或要改设计）｜❌ 不合适（建议剔除/不再增长）

## 根目录

| 标记 | 文件 | 说明 |
|---|---|---|
| ✅ | `package.json` | 工程入口，正常 |
| ✅ | `DESIGN.md` / `README.md` | 架构与用法文档，维护良好 |
| ✅ | `profiles.json` | 运行时配置，真实用户数据。**但被 git 跟踪**是隐患：基线提交 `e666d0c` 里 `p-desk` 就是 `fps: 144`，用户改成的 240 只是工作区改动 —— 一次 `git checkout` / `stash` 就打回 144。源码里没有任何地方写死 144（只有 `App.vue:988` 的 `FPS_CHOICES` 下拉选项目录含 144）。**根治的命令（等主人执行，本鱼不动 git）**：<br>`git rm --cached profiles.json profiles.json.bak`<br>`printf '\nprofiles.json\nprofiles.json.bak\n' >> .gitignore`<br>`git add .gitignore && git commit -m "配置不入库"`<br>注意 `--cached` 只撤出索引、**不删工作区文件**，主人当前的 `fps: 240` 会原样保留 |
| ⚠️ | `Profiles.cpp` 的 `SameSettingsIgnoringFpsBitrate` | 为了让「改帧率/码率不重建录制器」成立，判定分三级：完全相同 → 保留会话；只差 fps/bitrate → `ApplyLiveSettings` 就地改；其余 → 整路重建。**加字段时三处都要跟着改**（`SameSettings` / `SameSettingsIgnoringFpsBitrate` / `ApplyLiveSettings`），漏一处就是「改了没反应」或「改了就掉画面」 |

## app/（Electron 主进程）

| 标记 | 文件 | 说明 |
|---|---|---|
| ✅ | `main.cjs` | 核心，注释详实；本次改造主要动它（服务生死、提示） |
| ✅ | `preload.cjs` | 环同步设计正确；「等待采集服务」提示文案要改 |
| ✅ | `service-const.cjs` / `temp-const.cjs` / `temp-clean.cjs` | 常量与临时目录治理，职责清晰 |
| ✅ | `probe-core.cjs` | **保留在 `app/` 是对的**：它被 `app/main.cjs:24` 直接 `require`，服务于 `--probe` 自检模式（`IS_PROBE`），和主进程是同一套运行时依赖。挪到 `scripts/` 反而要跨目录 require 并复制模块解析路径，得不偿失 |
| ✅ | 根 `.gitignore` | `export/`、`dist/`、`native/build/`、`node_modules/`、`.electron/`、`*_svc*.txt` 均已覆盖。**`profiles.json` / `profiles.json.bak` 仍在跟踪中**，是「每次启动都变成 144fps」的根因，待主人授权后移除 |

## native/（C++ 后端）

| 标记 | 文件 | 说明 |
|---|---|---|
| ✅ | `main.cpp` `Session.*` `FrameRing.*` `ScreenCapture.*` `RingNames.h` `StatusFile.*` `Profiles.*` `WindowEnum.*` `EnvPath.*` | 核心采集链路，结构清楚。**界面点导出前必须先让服务 `flush` 收尾正在写的那一片，且要等到回执**（`SessionStatus::flushSeq` 变化）再调 `exportStart` —— 管道是单向的、`pipeSend` 写完就返回，第一版发完直接往下走，导出端扫目录时那一片还没 `Finalize`，成片末尾比点击时刻早 1~2 秒（见 DESIGN.md 9.4.1.1）。**`state` 必须由采集健康度决定，跟有没有消费者无关**：`status_.state = Live` 原来只写在 `WaitForConsumer(0)` 成功那一支，导致服务单独跑（常驻录制的常态）时状态恒为 `waiting` 而录制一直正常；补进 `else` 分支后又发现**两个分支都在 `if (!connected)` 里**，于是「先起服务、后开界面」会把 `waiting` 永久焊死（`consumerPid` 非零、`recordFrames` 在涨、只有这个字段撒谎）。最终修法是把判据从分支里提出来**每拍判一次**，且只覆盖 `Waiting`/`Starting`，不许冲掉 `Reconnecting`/`Failed`/`Disabled`（见 DESIGN.md 9.10 + 9.10.1） |
| ✅ | `SegmentRecorder.*` | **已重写**：改调 `MfEncoder`（进程内 SinkWriter），不再起子进程。喂帧线程「按帧数补帧、允许卡不允许变速」的核心时间轴原样保留。**帧率硬上限 120fps**（`MfEncoder::kMaxFps`），`Start`/`SetFps` 都把离谱值夹回来。**喂帧节拍必须用 `encoder_->EffectiveFps()`**，不能存配置值 —— 配 240 帧实际降到 165 时两边节拍错位 1.45 倍，画面慢放、片间解码内容完全相同（见 DESIGN.md 9.6.3）。**投帧的 memcpy 必须在 `slotMu_` 之外**（先拷进采集线程私有的 `writeBuf_`、锁里只做 swap），否则 1440p 每次投帧把喂帧线程堵 1.5ms，165fps 的 6.06ms 预算直接吃掉四分之一（见 DESIGN.md 9.2.4.1）。**带吞吐看门狗**：每 1.5 秒统计重复帧占比，连续两窗 > 40% 就自动降一档（`120→90→60`，只降不升）—— 硬件「接受」某个帧率不等于「喂得满」（见 DESIGN.md 9.2.4.2） |
| ✅ | `Exporter.*` | **已重写**：`IMFSourceReader` 读分片压缩样本 + `IMFSinkWriter` 重封装，无子进程。尺寸一致的片子走 passthrough（等价 `ffmpeg -c copy`）；尺寸不一致时画布取**最大**尺寸、小画面贴左上角补黑边、**绝不拉伸**（重编码路径）。分片快照、`targetFps_` 整数倍插帧、导出时长校验都保留；`targetFps_` 同样夹到 `MfEncoder::kMaxFps`（120）。**时间轴帧率必须按源分片真能给出的帧率钳**（`canRaiseFps`），否则配 240 帧实际录成 165fps 时成片被压成 13.7 秒（见 DESIGN.md 9.6.1）——两条路（直拷 / 重编码）都要钳，只改一条等于没改。**帧率不一致（混合帧率）也不许直拷**：直拷会产出可变帧率成片（实测 `avg_frame_rate=106920000/1200047`≈89，播放器解码就是"卡成 PPT"），必须强制走重编码，那条路按 `canvasFps` 铺固定网格、源样本按真实时间戳贴上去（密则重复上一帧、疏则丢帧），成片严格恒定帧率（见 DESIGN.md 9.6.2）。**`ListSegments` 扫全部轮次目录**（服务中途退出再起来，回放窗口里既有旧片也有新片，中间按设计补 NO SIGNAL） |
| ✅ | `MfEncoder.*`（新增） | 进程内 MF 分片编码器：**自己把 BGRA 转 NV12** 再喂 → fMP4 分片槽位循环。**帧率硬上限 `kMaxFps = 120`**（主人拍板「120 以上的帧率全删」）：1440p 物理上限 172 所以 240 从来录不到，165 那档每帧成本顶破帧预算、真实画面只剩一半，120 及以下实测真实画面 89~93%、落盘 100%。`Start`/`SetFps`/`Profiles.cpp` 加载三处都夹。**必须关 B 帧**（`SetInputMediaType` 第三参数传 `CODECAPI_AVEncMPVDefaultBPictureCount=0`），否则分片拼接 POC 对不上、成品解不出来。**必须开 `MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS`**（默认关），否则只挑到软件编码器 `mfh264enc.dll`、1440p 稳定吃 6.8 核。**必须自己转 NV12、别直喂 BGRA**：没有 D3D manager 时 SinkWriter 只能用 `msvproc.dll` 在 CPU 上转，实测 3.79~6.82 核且颜色算偏（黄里掺绿），自转后 0.80~0.88 核（见 DESIGN.md 9.2.1）。**帧率超硬件吞吐上限仍会自动降档**（阶梯 `{120,90,60,30}`，见 DESIGN.md 9.6.1）。**输入样本必须轮转一个池、不能复用同一个 `IMFSample`**：复用单样本时 `WriteSample` 返回 `S_OK` 但帧被静默丢弃（165fps 只落盘 31%、144fps 23%）；池深按 `fps_/6` 缩放、夹在 [8,24]，24 份之后 120 及以下全部 100%（见 DESIGN.md 9.2.4）。**尺寸变化**：变大立刻切片（新片精确尺寸）、变小贴左上角补黑，文件帧尺寸永远等于真实画面尺寸 —— 过渡画布 2 倍预留方案已废弃（会把导出画布污染成 4096x2912、NVENC 超限录制定格，见 DESIGN.md 9.5.2）。**已知遗留**：每切一片 SinkWriter 不完全回收句柄（2 秒一片≈每秒 +10 个），`MF_SINK_WRITER_D3D_MANAGER` 那条路会撞 `MF_E_UNSUPPORTED_D3D_TYPE 0xC00D6D76`，根治需全 GPU 通路 |
| ✅ | `Utf8Path.*`（新增） | 从 FfmpegUtil 抽出的纯路径/编码工具（FromUtf8/ToUtf8/PathToUtf8/PathFromUtf8），与 ffmpeg 无关，继续被 Profiles/EnvPath/StatusFile 使用 |
| ❌ | ~~`FfmpegUtil.*`~~ | **已删除**。`ResolveFfmpeg`/`BuildCommandLine`/`QuoteArg` 随 ffmpeg 一起废弃，纯工具函数迁到 `Utf8Path.*` |
| ✅ | `WindowCapture.*` | WGC 抓窗。**帧池按屏幕尺寸建一次、永不重建**（原按窗口尺寸建，窗口一变就要重建整个会话，而泵线程被设计成永不 return，每次重建泄漏 17 个线程；探针实测大池子全程持续投帧）。`Capture()` 每帧读 `get_ContentSize()` 只更新对外宣称的宽高，staging 按内容尺寸建 + `CopySubresourceRegion` 只拷左上角那块（`box.right/bottom` 是开区间）。窗口源一律 `put_IsCursorCaptureEnabled(FALSE)`（不画光标） |
| ✅ | `ScreenCapture.*` | DXGI 桌面复制。**桌面源自己画鼠标指针**：`DXGI_OUTDUPL_FRAME_INFO` 的 `PointerPosition` + `GetFramePointerShape()`，叠在调用方缓冲的副本上（不叠 staging，避免污染共享帧）。判"有没有光标更新"只能看 `LastMouseUpdateTime`（为 0 时 Position 是 (0,0)、Visible 是 FALSE，看它会让光标疯狂闪烁） |
| ⚠️ | `ShadowRing.h`（根） | 与 backend 头在同一层，位置略别扭，可接受。**改布局必须同步** `frontend/src/lib/ringLayout.ts`（`RING_HEADER_SIZE = 432`）并跑 `npm run offsets` |
| ✅ | `ring_offsets_check.cpp` | 偏移自检小工具，有用 |

## bridge/（N-API 插件）

| 标记 | 文件 | 说明 |
|---|---|---|
| ✅ | `shadow_ring.cc` `binding.gyp` | 共享内存零拷贝的关键，设计正确 |
| ✅ | `shim/*.h` | 自带 N-API 头，避免依赖 node headers，聪明 |

## frontend/（Vue 界面）

| 标记 | 文件 | 说明 |
|---|---|---|
| ✅ | `src/App.vue` `src/composables/usePreview.ts` `src/lib/*` | 分层合理。**「等待采集服务」的文案已经统一**：`app/preload.cjs:308` 发 `waiting-service` 事件、`usePreview.ts:383` 的 `serviceHint`、`App.vue:336` 的遮罩三层共用同一句「采集服务还没就绪，正在等待…」，不再各说各的。`frameRenderer.draw()` 的 GPU uniform 位置改成构造时查一次、字段缓存（原来每帧 `getUniformLocation`，144Hz 下 288 次/秒的同步 JS↔GL 往返） |
| ✅ | Vite/TS 配置齐全 | 正常 |
| ✅ | `package-lock.json` | 入库正确 |

## scripts/

| 标记 | 文件 | 说明 |
|---|---|---|
| ✅ | `build-native.mjs` `build-web.mjs` `dev*.mjs` `launch.mjs` `electron-path.mjs` `msvc-env.mjs` | 构建链工作正常 |
| ✅ | `service.mjs` | CLI 仍可用（`npm run service` / `status` / `stop`），但**已不是必需步骤** —— 界面启动时会自动拉起服务（`app/main.cjs` 的 `launchService()`，`detached: true` + `CREATE_BREAKAWAY_FROM_JOB` 让服务不随 Electron 退出）。文档里的「必须另开终端」说法已改 |
| ✅ | `test-pipe.mjs` `test-selfheal.mjs` `test-recording.mjs` `test-export.mjs` | **已全部跟改到 fMP4 语义**，实际检查：`test-pipe.mjs` / `test-selfheal.mjs` 里已无 `ffmpeg` / `.ts` / `ffprobe` 字样（grep 为空）；`test-recording.mjs` 按六列 `index.csv` 断言、`test-export.mjs` 断言 `noSignalSeconds`。四条回归均通过（`test-pipe` / `test-selfheal` 实测 EXIT=0，服务 pid 全程未重启） |
| ✅ | `test-reload.mjs` | 验「保存只重载改动的那一路、服务 pid 不变」。**曾经从写出来那天起就没跑通过**：它靠 CDP 端口 9222 驱动界面，而 `launch.mjs` 从来不带 `--remote-debugging-port`，于是永远 `ECONNREFUSED`。已给 `launch.mjs` 加 `--cdp` 开关并接进 `npm run test:reload` |
| ✅ | `test-budget.mjs` | 把一拍拆成「采集 / 发布 / 喂录制器」三笔并与帧周期相减，回答「为什么 fps 只到这个数」。纯读状态文件，不启动任何进程，排查帧率时第一手工具 |
| ✅ | `probe-build.mjs` | 一次性探针的统一构建工具（`node scripts/probe-build.mjs <源.cpp> <输出.exe> [--with-backend] [--with-mf] [--with-wgc]`），保留备用 |
| ⚠️ | `migrate-record-dirs.mjs` | 一次性迁移脚本，`package.json` / `README.md` / `DESIGN.md` 均无引用（老目录结构早已迁移完）。**本鱼不主动删** —— 删文件属于主人物权，且它仍可能被主人手工用来处理历史录像目录 |

## 明确不入库的状态（.gitignore 已覆盖）

`node_modules/`、`.electron/`（Electron 二进制副本，靠 PATH 或正常安装即可）、`dist/`、`export/`、`native/build/`、`*_svc*.txt`
