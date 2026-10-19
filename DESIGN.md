# ShadowPlay 即时重放 —— 设计稿

目标：把现在的「单路预览 demo」扩展成真正的即时重放软件。

现在的代码只解决了**一路**采集源 → 共享内存 → 浏览器预览。要变成重放软件，
缺的主要是四块：**多配置并发**、**窗口级采集**、**编码与滚动分片**、**导出合并**。

---

## 1. 需求拆解

| # | 需求（m03127） | 归属 | 关键难点 |
|---|---|---|---|
| 1 | GUI 里配置要重放的目标：桌面 / 当前存在的窗口 | 前端 + 服务 | 枚举窗口、采集单个窗口 |
| 2 | 勾选哪些配置需要一直录制 | 前端 + 服务 | 每配置独立的会话生命周期 |
| 3 | 录制结果存到一个录制根目录下，子目录不能冲突 | 服务 | 目录分配与并发写 |
| 4 | 可配置窗口匹配规则 | 前端 + 服务 | 规则模型 + 匹配器 |
| 5 | 可配置录制帧率、比特率、回放时间 | 前端 + 服务 | 编码器参数 |
| 6 | 超过回放时间的片段自动删除（滚动覆盖） | 服务 | 分片轮转 + 安全删除 |
| 7 | 每个配置都能预览录制情况 | 前端 + 服务 | **每配置一路预览流** |
| 8 | 预览只能 60fps | 前端 | 已有（`--render-fps`） |
| 9 | 预览拉伸保持纵横比 | 前端 | `object-fit: contain` 等价逻辑 |
| 10 | 窗口匹配不到就强制关闭该配置 | 服务 + 前端 | 周期校验 + 状态上报 |
| 11 | 导出按钮：分片合并导出到预设文件夹 | 前端 + 服务 | 拼接 |
| 12 | 文件名参考 NVIDIA 即时重放 | 服务 | 命名规则 |
| 13 | 暂时不做音频 | —— | 只做视频轨 |

---

## 2. 名词

- **配置（Profile）**：一条「录什么 + 怎么录」的记录。含匹配规则、帧率、比特率、
  回放时长、启用开关。
- **采集源（Source）**：配置解析出来的实际目标，三种：
  `desktop`（整个虚拟桌面）、`monitor`（某一台显示器）、`window`（某个 HWND）。
- **会话（Session）**：一个配置在服务里的运行实例，拥有自己的采集器、编码器、
  预览帧环、分片目录。
- **分片（Segment）**：一段固定时长的录像文件。回放缓冲由一串分片组成，旧的被删。

---

## 3. 总体架构

```
┌─────────────────────── Electron ───────────────────────┐
│  渲染进程 (Vue3)                                         │
│    ├─ 配置列表 / 编辑表单                                │
│    ├─ 每配置一个 <canvas> 预览（WebGL2，保持纵横比）      │
│    └─ 导出按钮                                          │
│            ↑ IPC + ArrayBuffer（每配置一块共享内存视图）  │
│  preload.cjs                                            │
│    └─ 每配置一块 V8 ArrayBuffer，泵同步（已有机制复用）   │
│  main.cjs                                               │
│    └─ 配置 CRUD、窗口枚举、导出编排、服务生命周期          │
└──────────────┬──────────────────────────────────────────┘
               │ 命名共享内存 + 命名事件 + NDJSON 状态
┌──────────────┴──────────────────────────────────────────┐
│  shadow-capture.exe（常驻服务，单进程多会话）              │
│                                                          │
│   ProfileStore ── 读配置，增删会话                        │
│                                                          │
│   ┌ Session A (桌面 @60fps 20Mbps 120s) ┐                │
│   │  SourceCapture → Encoder → SegmentWriter → GC       │
│   │        └→ PreviewRing (ShadowPlay.Frame.<sessA>)     │
│   └──────────────────────────────────────┘               │
│   ┌ Session B (窗口 "记事本*" @30fps ...) ┐              │
│   │  WindowMatcher(轮询) → SourceCapture → ...           │
│   └──────────────────────────────────────┘               │
└──────────────────────────────────────────────────────────┘
```

**为什么所有会话都在一个服务进程里**：多进程时窗口匹配、分片目录分配、
导出时的文件锁都要跨进程协调；单进程用一个线程池 + 一把配置锁就够了。
采集本身是 GPU/编码器的活，线程数远小于配置数也不会抢 CPU。

**为什么预览还要走共享内存**：编码是给「存下来」用的，预览要的是**实时**。
从编码流解回来会引入一个 GOP 的延迟，而且多一次解码。
所以预览继续走现在的原始 BGRA 共享内存环 —— 这条路已经验证能跑到 116fps。

---

## 4. 采集源

### 4.1 桌面 / 显示器：DXGI 桌面复制（已有）

`native/backend/ScreenCapture.cpp` 已经能跑。多会话的注意点：

- **同一个显示器只能有一个 `IDXGIOutputDuplication`**。两个配置都盯同一台显示器时，
  第二个 `DuplicateOutput` 会失败（`DXGI_ERROR_NOT_CURRENTLY_AVAILABLE`）。
  → 需要**采集源去重**：同一 output 由一个 `CaptureSource` 持有，
  多个配置共享它，各自只做自己的裁剪/编码。
- 每台显示器一套 `ID3D11Device`？不需要，一个 device 可以建多个 duplication。
  （但是 duplication 的帧必须在 `ReleaseFrame` 前被所有共享者用完 →
  改为「共享源发布一份原始帧到进程内队列，各会话各自取」。）

**鼠标指针**：桌面复制拿到的桌面图像**不含光标** —— 光标是 DWM 合成时单独贴的
一层，复制接口不给。所以要自己画：`DXGI_OUTDUPL_FRAME_INFO` 里带
`PointerPosition`，形状靠 `duplication_->GetFramePointerShape()` 取（COLOR /
MASKED_COLOR / MONOCHROME 三种格式，混合规则由 DXGI 定义）。三条实测定下来的语义：

1. `PointerPosition.Position` 是**光标位图左上角**的坐标，热区已经扣掉了。
2. 没有光标更新的帧 `LastMouseUpdateTime` 为 0（同时 Position 是 (0,0)、
   `Visible` 是 FALSE）——**只能看 `LastMouseUpdateTime`**，否则每帧都当
   "光标消失"处理，录出来疯狂闪烁。
3. 形状只在换形状的那一帧带（`PointerShapeBufferSize > 0`），其余帧是 0，
   必须把上一次的形状留着继续用。

叠放位置是**调用方缓冲的副本**，不是 staging 纹理 —— staging 是共享资源，
往里写会污染其他路读到的原始画面。叠在副本上还有个好处：`lastFrame_` 复用
那块内存，桌面静止时重放出来的画面照样带光标。

窗口源（WGC）反过来**一律不画光标**（`put_IsCursorCaptureEnabled(FALSE)`），
这是用户明确要求的：预览自己剪的窗口时画面里多一个鼠标很碍事。

### 4.2 单个窗口：Windows Graphics Capture（新增）

DXGI 桌面复制**不能**只抓一个窗口。抓窗口要用
`Windows::Graphics::Capture`（`GraphicsCaptureItem::CreateFromWindow`）。
本机是 Windows 11 (10.0.26200)，支持。

- 需要 `d3d11.dll` + `windows.graphics.capture.h`。用 C++/WinRT 会拖进一大堆
  头文件依赖；**改用 ABI 直调**（`RoGetActivationFactory` + 手写 `IInspectable`
  派生接口声明），跟现在手写 N-API 声明一个路子，不引入新依赖。
- 窗口关闭 / 目标进程退出时 `GraphicsCaptureItem` 会失效 → 正好作为需求 10 的
  触发点（比轮询标题更可靠）。

**窗口改尺寸时预览要跟着变**（用户需求：画面要缩放到尽可能大）。这里的坑很深，
最终方案是**帧池按屏幕尺寸建一次、之后永不重建**：

- 帧池尺寸在 `IDirect3D11CaptureFramePool::Create` 那一刻定死。如果按**窗口**
  尺寸建池，窗口一变就对不上：`Capture()` 返回的帧**永远还是 Init 时的尺寸**
  （实测窗口从 500x400 拉到 1600x1000，帧一直是 728x589），只有
  `frame->get_ContentSize()` 会跟着变。
- `IDirect3D11CaptureFramePool::Recreate()` 换尺寸**有效**（`cap.Width/Height`
  每次都更新对了），但**一旦调用，投帧渠道就断了** —— 之后的 FrameArrived
  再也不来。探针跑过三轮（释放旧帧、重挂回调都试了），无效。静止窗口尤其明显：
  它本来就不重绘，断了就永远没帧。
- 于是第一版改成「尺寸稳定 250ms 就返回 `AccessLost`，让 Session 走重建路径
  （`Cleanup()` → `Init(同一句柄)`）」。功能对，但代价惊人：WGC 的泵线程被
  设计成永不 return（return 会触发 `DllMain(THREAD_DETACH)` 在低完整性环境
  崩 0xC0000005），**每次重建泄漏 17 个线程 + 一份 D3D11 设备 + 一个
  DispatcherQueue + 一整套帧池**。实测拖十几次窗口 → 187 线程 / 246MB，
  这就是用户报的「资源管理器卡 + CPU 40% + 内存 800M」的主要来源。
- **正确解法**：帧池尺寸只是「每帧缓冲的容量」，不是「画面尺寸」。按下
  窗口尺寸建池纯属误解 —— 一次建成屏幕那么大（`SM_CXVIRTUALSCREEN` 再加
  1/8 余量，上限 5120x3072），窗口怎么缩放都不会打断投帧。探针
  `probe_pool_grow.cpp` 实测：池 2560x1440 时窗口 900x650 → 1600x930 → 缩回，
  **全程持续投帧**，纹理恒为 2560x1440，`get_ContentSize()` 精确跟随每一步。
- 现在的做法：`Capture()` 每帧读 `get_ContentSize()`，只更新对外宣称的宽高
  （`width_/height_`），**不重建任何东西**；staging 纹理按**内容尺寸**懒建，
  用 `CopySubresourceRegion` 只拷左上角那块内容区（`D3D11_BOX` 的
  right/bottom 是开区间，写 w-1 会少一列）。1432x1030 的窗口每帧只拷 5.9MB，
  而不是整块 14.7MB 的屏幕。

### 4.3 窗口匹配规则（需求 4）

```jsonc
{
  "match": {
    "by": "process | title | class",     // 匹配哪个字段
    "mode": "exact | contains | regex | glob",
    "pattern": "notepad",
    "pick": "foreground | largest | first | topmost"  // 多个命中时选哪个
  },
  "pollMs": 1000,
  "onLost": "disable"                    // 需求 10：匹配不到就关掉这个配置
}
```

匹配器每秒枚举一次顶层窗口（`EnumWindows`，过滤掉不可见 / 无标题 /
`WS_EX_TOOLWINDOW`），按规则筛选。命中数量变化时上报事件。

---

## 5. 编码与分片

### 5.1 编码器选型

本机有 **NVIDIA RTX 4070 Laptop + Intel Iris Xe**，Windows 11。
走 **Media Foundation H.264 硬件 MFT**（`MFT_ENUM_FLAG_HARDWARE`）：

- 优先 Intel QSV / NVIDIA NVENC 的 MFT，拿不到再退 `MFT_ENUM_FLAG_SYNCHRONOUS` 的软编。
- 输入 NV12（GPU 上从 BGRA 转，用 `ID3D11VideoProcessor`，不再回 CPU）。
- 低延迟模式（`CODECAPI_AVEncCommonLowLatency`）+ 固定 GOP（2 秒一个关键帧，
  这样每个分片都能独立播放）。
- **关 B 帧**（`CODECAPI_AVEncMPVDefaultBPictureCount = 0`）：分片要能直拷拼接，
  B 帧会让各片的 POC 对不上。见 §9.2。

> 为什么不用 ffmpeg.exe：需求明确禁止外部 exe/子进程，而 MF 是系统自带的，
> 少一层进程和管道开销，也不用担心换台机器就没装。

### 5.2 分片（需求 6）

```
%TEMP%/shadow-play/app/records/    ← 录制根目录（需求 3；路径来自 temp-const.cjs）
  <配置名>-<后缀>/
    segments/
      <轮次>/                      ← 每次重建编码器开一轮，旧轮次留着供导出拼接
        000000.mp4                 ← 循环复用的槽位名，不是单调递增序号
        000001.mp4
        index.csv                  ← 清单：文件,起点秒,终点秒（格式同 ffmpeg segment_list）
        run.json                   ← 本轮参数（generation/fps/width/height/container=fmp4）
    meta.json
```

> 为什么落在 `%TEMP%` 而不是仓库里：仓库可能被同步盘、杀毒、沙箱盯着，写盘既慢
> 又会给目录打上奇怪的安全标签（被打了 Low 完整性标签的目录，子进程继承之后
> WGC 直接 `0x80070005`）。临时产物本来就该待在临时目录里，整目录随时可删。

- 每个分片约 **2 秒**，封装用 **fMP4**：
  原因不是偏好而是**没有选择** —— Media Foundation 的
  `MFTranscodeContainerType_*` 里没有 MPEG2TS（见 §9.2）。
  fMP4 恰好也满足「半截文件仍可读」：每个 `moof` 自带索引，
  不像普通 MP4 要等 `moov` 落地。
- 分片数由 `keepSegments = ceil(回放时长 / 分片时长)` 限住，槽位循环复写；
  起录前先清掉本轮目录里的旧分片（复写不截断会让磁盘统计虚高）。
- 每写完一个分片就检查：`所有分片总时长 > 回放时长` 就删最旧的 ——
  实际由槽位数天然限住，不需要额外的删除逻辑。

### 5.3 导出（需求 11、12）

导出 = 把 `segments/<轮次>/` 里的分片按时间顺序拼接 + 换成普通 MP4。
两条路：

1. **纯字节拼接 + `ffmpeg -c copy`** —— 最简单，但本机没有 ffmpeg，且需求禁止用 exe。
2. **Media Foundation `IMFSinkWriter` + 多个源读取器** —— 纯系统 API，
   自己要实现「多源顺序读」的适配器。

**已选 2 并落地**（见 `native/backend/Exporter.cpp`），并且**按尺寸分成两条路**：

- **所有分片尺寸一致 → 直拷（passthrough）**：`IMFSourceReader` 逐片打开，
  `GetNativeMediaType` 拿 H.264 原生类型，设回 `SetCurrentMediaType`
  即读**压缩样本**（不解码）；目标 `IMFSinkWriter` 的 `SetInputMediaType`
  传同一类型即 passthrough（等价 `-c copy`）。实测 S_OK，180/180 帧全在。
- **尺寸不一致 → 重编码补黑边**：画布 = 窗口内**最大**那一帧的尺寸，
  小画面贴**左上角**，右边和下面补黑边，**绝不拉伸**（用户明确要求）。
  解码走 `MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING` 输出 RGB32，
  自己贴到画布上，再用 MF 的 H.264 编码器重新编一遍。
  输出帧率取各片里最高的那个；比画布帧率低的片子按**整数倍重复样本**补上
  （60 → 240 每帧写 4 遍），非整数倍不重采样 —— 宁可帧率不是用户设的那个，
  也绝不能变速。

两条路共同的约束：样本时间戳**不用源 PTS、按输出帧序号重编** —— 否则
MP4 sink 会静默丢帧（实测 180 帧只剩 150，`avg_frame_rate` 变成 50）。

两个踩过的坑（都写在代码注释里）：

- 重编码那一路**不能**因为 `MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED` 就停。
  尺寸不符、走了视频处理器时，SourceReader 在第一条样本之前会拿"协商后的
  实际类型"顶回来一次，那是正常握手。当成"流中间换格式"处理会导致
  **只有第一片被导出**（实测 `3 个分片 → 1 个分片`）。
- 补黑边只清"这一帧覆盖不到"的两条（右侧竖条 + 底部横条），不做全屏清零：
  上一帧更大的部分会被这两条覆盖掉。4K 画布全清一次 33MB，4800 帧就是 150GB。

命名参考 NVIDIA 即时重放（`Replay_2026.10.16_18.03.12.mp4` 这种）：
```
<导出目录>/
  Replay_2026.10.16_18-03-12.mp4
  Replay_2026.10.16_18-05-40.mp4
```
配置名可以做成前缀（`desktop-01_Replay_...`），避免同名覆盖。

---

## 6. 预览（需求 7、8、9）

- 命名规则不用改：把现在的固定会话 id 换成**每个配置一个 id**
  （`RingNames.h` 的 `RingName(id)` 已经是参数化的，直接用）。
- `app/preload.cjs` 现在是单例（一个 `addon`、一块 buffer）。要改成
  **一个配置一个映射**：`open(sessionId)` 已经带 session 参数，扩成
  「打开多个」+ 每块 V8 缓冲区。
- 前端每配置一张 `<canvas>`，各自的 WebGL2 context。
  **注意**：浏览器对同时存在的 WebGL context 数量有限制（通常 16 个），
  配置多了要 LRU 复用 context，或者只给「可见的」配置分配 context。
- 纵横比（需求 9）：canvas 的 CSS 尺寸按容器做 **contain** 计算
  （`aspect = srcW/srcH`，取 `min(容器W/srcW, 容器H/srcH)`），
  WebGL 视口按 canvas 实际像素尺寸设置，不用拉伸采样。
- 60fps 上限（需求 8）：现在是全局一个 `nextFrameAt`。多配置时改成
  **一个 rAF 驱动所有画布**，每配置各自记 `nextFrameAt`，
  这样多配置也不会叠加 rAF 开销。

---

## 7. 配置的存储与生命周期

配置文件放 `app/config/profiles.json`（用户数据，不进 git）。

```jsonc
{
  "version": 1,
  "exportDir": "D:\\IT\\JS\\shadow-play-app\\exports",
  "recordRoot": "D:\\IT\\JS\\shadow-play-app\\test",
  "profiles": [
    {
      "id": "p-7f3a1c",
      "name": "桌面-主显示器",
      "enabled": true,                 // 需求 2：勾选就一直录
      "source": { "kind": "desktop", "monitor": 0 },
      "video": { "fps": 60, "bitrateMbps": 20, "replaySeconds": 120 },
      "match": null,
      "createdAt": "2026-10-16T18:30:00+08:00"
    }
  ]
}
```

生命周期：

```
用户新建/勾选 → main.cjs 写 profiles.json → 通知服务 reload
   → 服务分配子目录 → 起 Session 线程 → 开始采集 + 编码 + 预览
窗口匹配不到（需求 10）→ 服务上报 profile_lost → 前端把开关置灰关掉
```

**目录不冲突**（需求 3）：录制根下的 `.index.json` 记 `profileId → 目录名`。
目录名 = `<源类型>-<简短名字>-<id 后4位>`，重名时加 `-2`、`-3`。
删配置时**不删目录**（录像还在），只在 index 里标记归档。

---

## 8. 实施阶段

按「先证明风险最大的部分」排序，每个阶段都能独立验证。

| 阶段 | 内容 | 验证方式 |
|---|---|---|
| **P1** | 服务改成多会话骨架：配置模型 + 配置加载 + 多会话帧环 + 窗口枚举/匹配 | 两个配置同时预览出画面 |
| **P2** | 窗口级采集（WGC）+ 采集源去重共享 | 单窗口预览能跟随窗口移动/缩放 |
| **P3** | Media Foundation H.264 编码 + fMP4 分片落盘 | `test/<dir>/segments/<轮次>/` 出现能播放的 mp4 |
| **P4** | 滚动删除（回放时长）+ 目录索引 + 丢窗口自动关配置 | 播放一小时，目录大小稳定在设定值 |
| **P5** | 导出合并（SourceReader 直拷 → SinkWriter）+ 命名规则 | 导出的 mp4 能完整播放 |
| **P6** | GUI：配置列表 + 表单 + 多预览网格 + 导出按钮 | 全流程手工走一遍 |

---

## 9. 已确定的决策

### 9.1 窗口采集：Windows Graphics Capture（WGC）

- 用 `GraphicsCaptureItem::CreateFromWindow` 直接抓 HWND，窗口被遮挡、移出屏幕、
  被缩放都能正确抓到。窗口关闭时 `Item` 自动失效 —— 正好当需求 10 的触发点。
- 本机没装 C++/WinRT 头文件，所以走 **ABI 直调**（`RoGetActivationFactory` +
  手写 `IInspectable` 派生接口），跟现在手写 N-API 声明一个路子，不引入新依赖。
- **兜底**：WGC 对某些窗口（受保护内容、UWP 特殊情况）会返回黑帧。
  检测到全黑就上报 `source_black` 事件，界面提示。

### 9.2 编码：进程内 Media Foundation（已落地，ffmpeg 已移除）

> 这一节原来写的是「ffmpeg 子进程 + stdin 喂裸帧」。**已作废并重写**：
> 需求明确要求不要 exe、不要子进程，现在编码和换封装全在服务进程内用
> Windows 自带的 Media Foundation 完成，整机不需要装 ffmpeg。

调查结果（当时的判断依据，保留备查）：

| 途径 | 结论 |
|---|---|
| `C:\Windows\system32\ffmpeg.exe` | 本机有 full build，但**不能依赖**：换台机器就没了，而且用户要求不用 exe |
| ffmpeg DLL | 只有 DLL、没有开发头文件/导入库，手写 `AVCodecContext` 布局风险太大 |
| **Media Foundation** | ✅ 系统自带、零依赖、有硬件编码 MFT（NVENC / QSV / AMF 都以 MF 变换形式暴露） |

**现在的做法**：`IMFSinkWriter` 进程内编码，`native/backend/MfEncoder.{h,cpp}`。

```
采集帧 (BGRA, 已对齐)
   └─ SegmentRecorder::Feed()  --投进容量1的覆盖槽，立刻返回-->
        └─ 喂帧线程 --按恒定帧率出图，落后就补重复帧-->
             └─ MfEncoder::Write()  --IMFSinkWriter 自动做 BGRA→NV12→H.264-->
                  └─ <录制根>/<配置>/segments/<轮次>/%06u.mp4  (fMP4 分片，槽位循环)
```

三条实测出来的硬约束（都写进了 `MfEncoder.h` 的注释）：

1. **MF 写不了 TS**：`MFTranscodeContainerType_*` 里根本没有 MPEG2TS
   （见 SDK 的 `mfidl.h`），所以分片容器只能用 fMP4。这反而更好 ——
   fMP4 每个 `moof` 自带索引，进程被杀时已写的部分仍可读；
   普通 MP4 的 `moov` 要等 `Finalize()` 才写，崩一次整片报废。
2. **必须关 B 帧**：分片各自从 POC 0 开始，带 B 帧的话拼接时解码器报
   `co located POCs unavailable`，成品解不出来。建 writer 时通过
   `MF_SINK_WRITER_ENCODER_CONFIG` 传 `CODECAPI_AVEncMPVDefaultBPictureCount = 0`。
   屏幕录制本来就要低延迟不要压缩率，没有损失。
3. **每片时间戳从 0 起算**，拼接时由导出端按全局帧序号重编 —— 见下。

还有一条**必须开**的开关：`MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS`
（默认关）。不开的话 SinkWriter 只挑到微软自带的软件编码器
`mfh264enc.dll`，1440p@108 全靠 CPU 算，进程稳定吃 6.8 个核（全机 34%）。
开了之后变成 `nvEncMFTH264x.dll`，CPU 降到 3~4 核，GPU 的 VideoEncode
引擎吃到 40% 左右。

**已知遗留**：每切一个分片 SinkWriter 都不会把内部的编码 MFT、转换器、
文件句柄全回收。句柄增长与「建拆 SinkWriter 的次数」成正比 —— 2 秒一片时
每秒 +10 个（跑 3 分钟 974→2472），10 秒一片降到每秒 +2~3。线程数不涨
（65~70 波动）、内存也稳（200~230MB），所以是句柄泄漏不是资源失控。
试过用 `MF_SINK_WRITER_D3D_MANAGER` 交一份常驻 D3D11 设备，但 SinkWriter
一旦拿到 manager 就要求输入是 D3D 纹理，喂系统内存 RGB32 会得到
`MF_E_UNSUPPORTED_D3D_TYPE 0xC00D6D76`（而且前 70 多帧照常写盘，之后才
永久失败，极具欺骗性）。根治要把整条通路改成全程留在 GPU 上，属于独立改造。

导出（`native/backend/Exporter.{h,cpp}`）：`IMFSourceReader` 读各分片的
H.264 压缩样本 → `IMFSinkWriter` 的 `SetInputMediaType` 传原生类型即
**passthrough 直拷**（等价 `ffmpeg -c copy`）→ 普通 MP4 成品。
`targetFps_` 比源帧率大且为整数倍时（60→240），每帧写 N 遍，等价
`-vf fps=240` 但零重编码。非整数倍不重采样，靠每帧自带的 sample duration
让播放器按正确速度播。

导出端的 SinkWriter 也**必须**开 `MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS`
（`Exporter.cpp` 建 `sinkAttrs` 时）。这里曾经漏过：录制端开了、导出端没开，
于是走重编码路径（多路不同尺寸 / 要补 NO SIGNAL）时全靠 `mfh264enc.dll`
软算，2560x1440@144 的 20 秒成片要跑 18 秒以上；开了之后 `elapsedMs`
降到 0.5~1 秒量级。直拷路径不建编码器，不受影响，所以这个漏配只在
「尺寸不一致」或「有遮罩」时暴露 —— 很容易漏测。

- 采集线程与编码线程解耦：`Feed()` 只投**容量 1 的覆盖槽**，绝不在采集
  线程上做编码/IO —— 早期同步写 ffmpeg stdin 时管道背压把采集周期从
  15.2ms 拖到 29.5ms，帧率 60→45。这个教训在 MF 版本里同样适用。
- 时间轴**按帧数**维护、不按节拍：落后多少就补多少重复帧。
  「允许卡，不允许变速」。

#### 9.2.1 输入格式：自己转 NV12，别把 BGRA 甩给 SinkWriter

这是本项目最大的一次 CPU 优化，**踩过的坑值得完整记下来**。

最初的实现把采集来的 BGRA 直接当 SinkWriter 的输入类型
（`MFVideoFormat_RGB32` + `MF_MT_DEFAULT_STRIDE = w*4`）。功能没问题，
但 2560x1440@144 实测 **4.34 核 / GPU videoencode 44%**。

拆开测（探针 `probe_capture_cpu` 三段对照，2560x1440@144 各 8 秒）：

| 模式 | CPU 核 | 说明 |
|---|---|---|
| `capture`（只 DXGI 桌面复制 + 拷进缓冲） | 0.45 | 采集本身很便宜 |
| `encode`（只 `MfEncoder::Write`，喂同一张静态帧） | 2.58 | **全在这里** |
| `capture+encode` | 3.59 | 与服务实测吻合 |

`MfEncoder::Write` 自己只做一次 non-temporal memcpy（实测 1.23ms/帧 ≈
0.18 核），剩下的 2.4 核全在 `IMFSinkWriter` 内部。原因是：
**没有 D3D manager 时，SinkWriter 必须用 `msvproc.dll` 在 CPU 上把
BGRA 转成 NV12**（NVENC 只吃 NV12/P010）。这个转换对 1440p 每帧要读
14.7MB、写 5.5MB，144fps 下就是 2.9GB/s 的纯 CPU 带宽。

修法：`MfEncoder` 自己写 `ConvertToNv12()`（SSE2，BT.601 有限范围：
`Y = ((66R+129G+25B+128)>>8)+16`、`U/V` 按 2x2 平均），输入类型改成
`MFVideoFormat_NV12`。**实测 4.34 核 → 1.84 核（降 59%）**，
内存 194MB → 168MB，`captureMs` 3.9→3.4、`feedMs` 1.36→1.27。
顺带颜色更准（自己控制 BT.601 系数，而 msvproc 的默认矩阵与解码端不一致，
早期出现过偏色）。

#### 9.2.2 NVENC 编码档位：QualityVsSpeed

即使换了 NV12，NVENC 默认走「质量优先」档，1440p@144 时 GPU
`videoencode` 引擎被顶到 44~51%。录制回放不需要电影级画质：

```
encAttrs->SetUINT32(CODECAPI_AVEncCommonQualityVsSpeed, 20);  // 0=最快 100=最好
```

实测 `videoencode` **44.2% → 29~35%**，CPU 不变，画质肉眼无差。
导出端 `Exporter.cpp` 也必须同步设（重编码路径），否则导出会慢好几倍。

**不要关 CABAC**（`CODECAPI_AVEncH264CABACEnable=FALSE`）：试过再快一点，
但压缩率掉得离谱 —— 20 秒 1440p 成片从 6.2MB 涨到 **57MB（9 倍）**。
回放场景磁盘占用比那点 GPU 更重要。

**也不要改成 VBR/Quality 码率控制**（`eAVEncCommonRateControlMode_Quality`）：
实测 `videoencode` 反而升到 49.7%，因为质量档下编码器会为了画质反复重编。

#### 9.2.3 导出成片的码率：必须按源分片压上限

重编码路径原来按 `W*H*0.15*fps/1e6` 估码率，1440p@144 算出 **79Mbps**，
20 秒成片 60MB —— 而同样画面录制端分片只有 2.5Mbps（用户配的
`bitrateMbps=5`）。源里只有那么多信息，把码率放大 10 倍纯粹拿体积换
肉眼看不出的细节。

修法：取各片 `file_size / 实际时长` 得源码率，上限 = 源码率 × 2
（留重编码的重新量化余量）。实测 60MB → **15MB**，画质不变。

#### 9.2.4 桌面采集只拷变化区域（实测收益为 0，但代码保留）

`IDXGIOutputDuplication::GetFrameDirtyRects()` 能拿到本帧变化过的矩形，
`CopySubresourceRegion` 只拷这些块，staging 未覆盖的区域保留上一帧内容
（屏幕没变的地方本来就该是上一帧）。理论收益：1440p@144 的全屏
`CopyResource` 是 2.1GB/s 纯 GPU 带宽，占 `3d` 引擎约 18%。

**实测无效**：`capture` 模式 GPU_3d 仍 18.7%。原因是桌面上的光标一直在闪、
局部窗口在刷新，dirty 区域占比经常超过一半（代码里超过一半就回退全屏拷，
因为分块调用的固定开销会吃掉收益）。代码保留着 —— 全屏播视频/全屏游戏时
它能省下来，只是日常桌面场景用不上。

### 9.3 分片的时间戳：必须记**墙钟**

分片文件名是 `%06u.mp4`，**不含任何时间信息**。导出端只能靠文件 mtime 排序、
靠 `index.csv` 里的相对秒数拼时间轴 —— 这有两个致命后果：

1. **服务重启后会把昨天的片子当成"最近 20 秒"。** mtime 是"最后写入时刻"，
   程序不写它就不变，所以昨天录完留下的 `000003.mp4` 在今天启动后依然在
   目录里，排在今天录的片旁边，导出时被当成回放窗口的一部分。
2. **中间断档无法表达。** 录了 5 秒 → 服务退出 5 秒 → 再录 5 秒，两条记录
   在 `index.csv` 里首尾相接，看起来是连贯的 10 秒，实际中间空了 5 秒。

修法：`MfEncoder::DoneSegment` 增加 `startUnixMs`（**该片写入时的墙钟 Unix
毫秒**，`std::chrono::system_clock`），写进 `index.csv` 的第四列。`index.csv`
每行变成：

```
文件名,起始秒,结束秒,起始Unix毫秒
000000.mp4,0.000000,2.000000,1760000000123
```

**单位必须是毫秒，不能是秒。** 秒级分辨率下，两片紧挨着的 2 秒分片墙钟差
可能是 2000ms 也可能是 3000ms（截断误差），而导出端正是靠这个差判"中间是不是
断了" —— 用秒就会在两片正常接缝处凭空补出 1 秒 NO SIGNAL（实测出现过：成片
里每 2 秒闪一下黑）。

老文件只有三列 → 解析时第四列缺失就当 0，走"没有墙钟信息"的旧路径，
不影响已有录像的导出。

导出端据此做三件事：
- **过滤**：`now - startSec > maxSeconds + 余量` 的片子直接不算（这才是
  "最近 20 秒"的正确含义，而不是"目录里最后几个文件"）。
- **排序**：有墙钟就按墙钟排，**不能只按 mtime**。分片是按槽位覆盖写的：
  一个只写了 1 秒就切走的片（改尺寸、改帧率、服务重启都会）会在很久之后才被
  别的片覆盖成完整长度，于是它的 mtime 比时间上更晚的片还新。实测出现过某轮次
  的相邻两片按 mtime 排是**倒着接**的，逐片累加的时间轴一路往回跳。
- **补 no signal**：相邻两片的墙钟之间如果差得比"两片的标称时长"多，说明
  中间断了，缺多少秒就插多少秒的黑底 `NO SIGNAL` 帧。门槛不能太小 —— 分片
  之间天然有几十毫秒空隙（SinkWriter 收片到下一片第一个关键帧，实测
  30~130ms），门槛取 `max(0.3, 标称片长 × 0.25)`，否则每个接缝都会闪一帧黑。

#### 9.3.1 下一片的起点要**顺延**，不能现取 `NowUnixMs()`

`startUnixMs` 记的是"开片那一刻的墙钟"。但开片并不是瞬间完成的：
`MfEncoder::OpenSegment()` 里 `MFCreateSinkWriterFromURL` + `AddStream` +
`SetInputMediaType` 在 NVENC 上要 300~400ms（探针实测，与分辨率无关），
而**上一片的 `Finalize()` 还要先把编码队列排空**。这段时间里墙钟一直在走，
但新片的第一帧在内容上紧接着上一片的最后一帧 —— 于是 `index.csv` 里相邻
两片的墙钟差会比"上一片的真实时长"多出 1 秒以上。

导出端拿这个差去判断档，门槛只有 0.5 秒，于是**每一片的接缝处都补进 1 秒多
NO SIGNAL**。表现就是：成片里每 2 秒闪一段黑底 `NO SIGNAL`，而预览里画面
一直连贯。用户报的"调整窗口大小的时候隔壁那一路导出出现 no signal"就是这个
—— 调尺寸会让编码器频繁开新片，接缝变多，空洞随之变多。

修法：`MfEncoder` 增加 `int64_t nextSegStartUnix_ = 0`。
- `CloseSegment()` 收尾时算好下一片的起点：
  `nextSegStartUnix_ = segStartUnix_ + framesInSegment_ / fps_ * 1000`（用帧数
  算时长，不用 `NowUnixMs()` —— 后者把 `Finalize` 的耗时算进了片长）。
- `OpenSegment()` 里 `segStartUnix_ = nextSegStartUnix_ > 0 ? nextSegStartUnix_ : nowMs;`
- `Start()` 时清零，所以进程刚起来的第一片仍然用真墙钟。

这样时间轴在 `index.csv` 里严格首尾相接，**只有真的没录到画面时才留空洞**
（服务退出、窗口匹配不上），导出端补的 NO SIGNAL 才名副其实。

注意别把这个顺延做过头：如果进程真的停过一段（服务退出、切走尺寸后编码器
反复重建失败），`nextSegStartUnix_` 会落后于真实墙钟，这时候时间轴宁可
"压缩"也不能凭空补黑 —— 导出端的 `cutoffEnd` 仍然以最后一片的真实墙钟为准，
不会因为顺延而把整条时间轴推到未来。

### 9.4 回放不足时长时用 NO SIGNAL 补齐

用户要的是"20 秒回放就是 20 秒"，而不是"有多少给多少"。所以导出成品
**必须正好 `maxSeconds` 秒**，缺的部分用黑底 + `NO SIGNAL` 字样补。

规则（按时间轴从左到右填）：

```
[no signal 5s][实拍 5s][no signal 5s][实拍 5s]   ← 断档也补
```

- 开头不足：如果窗口内第一片之前没有画面（比如服务刚启动只录了 5 秒），
  前面补黑。
- 中间断档：两片墙钟间隔超过门槛时，中间补黑（门槛见 9.3 最后一条）。
- 结尾不足：最后一片之后补到 `maxSeconds`。

**这套补齐是"还没录满"的正确表现，不是 bug。** 服务刚起来 3 秒就点导出，
20 秒的成品里就该有 17 秒 NO SIGNAL。真正要盯的是**导出端有没有白扔已经
录到的分片**（见 9.4.1 / 9.5.1），以及自检脚本有没有在录满之前就催导出 ——
`scripts/test-export.mjs` 和 `app/main.cjs` 的 `--gui-export` 都吃过这个亏：
判据写成"任意路加起来 ≥3 段"或"等待时长比回放窗口还短"，录两秒就点导出，
成片全黑却一路判"通过"。现在的判据是：**选中那一路**的段数 × 单片秒数
≥ 回放窗口，并且成片里补的 NO SIGNAL 不得超过一半。

#### 9.4.0 自检要等的段数必须多要一片 —— `recordSegments` 含正在写的那片

`recordSegments` 是服务**扫盘**数出来的（`SegmentRecorder::RefreshDiskStatsLocked`
递归数 `segments/` 下的 `.mp4`），它把**正在写的那一片**也算进去了。而那一片还在
SinkWriter 手里、没 `Finalize`、`index.csv` 里也没有它，导出端 `ListSegments` 根本
读不出来（它还会被 `f.bytes < kMinSegmentBytes` 过滤掉）。

于是"盘上 10 片"实际上只有 9 片可用：9 × 2 秒 = 18 秒，剩下 2 秒只能补 NO SIGNAL。
实测 `node scripts/test-export.mjs 30` 反复稳定复现这一片之差 ——
`selectedSegments=10` 恒定导成「9 个分片 / 20.0 秒 / 其中 2.0 秒没录到」。

所以自检判据必须是 `Math.ceil(回放窗口 / 单片秒数) + 1`。曾经为了绕开"环形缓冲
卡在边缘"而把 `+1` 去掉，结果正好踩中这个坑：等到的永远是"含半片"的数。

环形缓冲保留 `回放窗口 + 两片`（见 `MfEncoder::PurgeExpired` 的 `keepSlack`），
20 秒窗口 + 2 秒片最多 12 片，所以等 11 片（10 片已完成 + 1 片在写）是安全的。

**铺法必须是"一条时间轴从左往右走"，不能是"片头补一块 + 逐片补缺口 + 片尾兜底"。**
后者看着等价，实测两处都错：

1. 缺口总量按"总共差多少秒"算，而"片头缺 + 中间缺"加起来可能比它大也可能比
   它小，成品于是随机变长变短（实测出现过 21.5 秒和 13.3 秒两个极端，用户设的
   是 20 秒）。
2. 游标初始化容易写错：如果写成"有片头遮罩时才给墙钟游标赋值，否则给 0"，
   那么片头没缺口时游标就是 0，第一片的真实墙钟（17 亿秒）被当成"距离 0 点
   有十七亿秒的空档"，一个遮罩项吃满 20 秒，计划里再没有真实分片 —— 导出
   直接报「分片里读不到任何可用的视频帧」。游标必须**无条件**从"回放窗口起点"
   开始算。

实现上不重新编码：直拷路径没法插入黑帧（H.264 流里凭空多出解码帧必须
重编码），所以**一旦需要补 no signal，就必须走重编码路径**。这也顺带解决
了窗口尺寸变化的问题 —— 重编码路径本来就支持"画布取最大尺寸、小画面贴
左上角补黑边"。

`NO SIGNAL` 的字样用 GDI 画一次到一张 BGRA 缓冲里缓存起来，之后每帧
只需要 memcpy（4K 一帧 33MB，如果每帧都重新画字会吃掉一个核）。

#### 9.4.1 回放窗口的收尾时刻必须用**真实**片长

用户原话：「调节完窗口大小后，点击导出视频，最后两秒会出现 no signal」。

那一小段黑不是"没录到"，而是**算错了窗口右边界**。收尾时刻取"现在"与"最新一片
的结尾"里较早的那个（理由：录制刚停下来时用现在算会把最后几片挤出去），而
"最新一片的结尾"原先写成 `起点 + 标称片长`。改窗口尺寸会把一片切短 —— 实测有
只剩 4 帧（0.13 秒）的，而标称永远按 `segmentSeconds`（2 秒）算，于是收尾时刻
被判到 1.9 秒之后，那 1.9 秒里根本没有画面，计划器只好补成 NO SIGNAL。

改法：只对**最新那一片**调一次 `ProbeSegment()` 取真实时长，探不出来（正在写、
索引还没落盘）才退回标称值。只探一片是因为这一步只需要一个"最近的时刻"，
而最新的一片必然最靠后，它的真实长度就够了。

**右边界再往后还有一层：`FlushSegment()`。** 用户又说「我点击导出视频的时候，
前一秒，或者说前半秒，画面都丢失了，应该一直录到我点击按钮才停止才对啊」——
因为点导出那一刻总有一片正在写，它没 `Finalize`、`index.csv` 里也没有它，
导出端只能读到最后一片**已完成**的分片。所以 `Session::ExportNow` 一进来就先
`recorder_->FlushSegment()`：同步把当前片收尾落盘、紧接着开新片继续录
（不换轮次目录、切片序号继续递增，代价只是那一片比标称短）。
`MfEncoder::FlushSegment()` 由 `SegmentRecorder` 转交给喂帧线程执行 ——
SinkWriter 只由那个线程碰，从命令线程直接 `Finalize` 会和正在进行的
`WriteSample` 撞车。

### 9.4.2 导出的计划必须能打出来（`export_plan`）

「导出里莫名出现 NO SIGNAL」有三种完全不同的成因：分片集合里真有空洞（录制端
丢片）、计划器把片算漏了（`lead` / `skip` / `gap` 判错）、某片 `ProbeSegment`
失败被 `continue` 掉。只看摘要（「9 个分片 / 20.0 秒 / 其中 2 秒补的」）分不清，
猜一次就是半小时。

所以 `Exporter::Run()` 在计划铺好之后打一行 `export_plan`，逐片列出
`name / ok / dur / wall` 与每个计划项（`VIDEO n sec=… skip=…` 或
`NOSIGNAL sec=…`）。接法有两处，**两处都必须接**：

- 服务进程：`main.cpp` 的 `SetExporterPlanLog(...)` 接到 NDJSON 日志出口。
- **插件（界面点导出走这条）**：`bridge/shadow_ring.cc` 的 `ExportStart` 里接到
  **stderr**。导出在 Electron 主进程里跑，服务根本不知道有这回事 —— 这一处
  曾经漏接，于是界面导出的诊断一直没落到任何地方。走 stderr 而不是 stdout，
  因为主进程的 stdout 正被拿去解析服务的 NDJSON。

`app/main.cjs` 把这一行存进 `live.lastExportPlan` 并写进验收报告的
`report.exportPlan`，报告自己就带着"为什么补了遮罩"。

### 9.5 改窗口尺寸不能重开录制器

用户的原话：「如果窗口大小经过调整，会出现服务从头开始录」。

**已落地。** 原现状是 `Session::EnsureRecorder` 发现宽高变了就
`recorder_->Stop()` + 新建一个 `SegmentRecorder`，于是 `SegmentRecorder::Start()`
分配一个**新的轮次目录**，切片从 `000000.mp4` 重新开始 —— 从用户视角就是
"回放里只剩下调整尺寸之后的画面"。

现在改成**同一路录制器跨尺寸存活**：

- `SegmentRecorder::Feed()` 不再拿尺寸做校验（只校验非零），槽里记住这一帧
  多大（`slotW_/slotH_`），`FeederLoop` 每拍按槽里的尺寸编码。
- `MfEncoder::Write()` 检测到尺寸变了就调 `ResizeTo()`：收掉当前片、按新尺寸
  重估码率、用新尺寸开下一片，**切片序号继续递增**。`index.csv` 里各片尺寸
  可以不同。
- `Session::EnsureRecorder` 只在"没有录制器"或"录制器已停"时才新建，尺寸变化
  不做任何事。
- 导出端本来就有"画布取窗口内最大尺寸、小画面贴左上角补黑边"的能力
  （见 9.2 的重编码路径），所以混合尺寸的片子天然能够导好。

实测（资源管理器窗口 1723x787 → 1332x966 → 2232x1341）：只产生**一轮**目录、
切片序号连续、`recordFrames` 一路增长；导出成品 2232x1340，前面小画幅的片
贴左上角补黑边，后面的满画布。

顺带解决了另一个浪费 —— 以前窗口拖大拖小十几次就重建十几次录制器，每次都要
停 encoder、`Finalize()`、重开文件。

#### 9.5.1 环形缓冲必须按**时间**保留，不能按片数

改尺寸就切一片（9.5），于是 2 秒的标称片长只在"画面稳定"时才成立。而环形
缓冲原先是按**片数**算的：`keep = replaySeconds / segmentSeconds`，20 秒 / 2 秒
= 10 个槽位。拖窗口时 Windows 是逐像素连续改尺寸的，30fps 下几乎每帧一个新
尺寸 —— 一次拖拽就能切出十几片，**10 个槽位瞬间被零点几秒的过渡碎片占满**，
真正能回放的时长只剩一点点。

用户看到的现象（原话）：「导出的视频，一直都是 no signal，然后只有后面高度
减小恢复的画面，我明明已经录制超过 20 秒了」。

三处修改：

1. **保留改成按墙钟**：`MfEncoder::PurgeExpired(nowMs)` 按
   `now - WindowMs() - 一片余量` 算截止点，把更早的片连文件一起删掉。
   `WindowMs()` 仍是"回放秒数"，但槽位数 `keepSlots_` 给了 `keepSegments_ × 8`
   （夹在 `[2, 512]`）的额外空间。
2. **过渡碎片直接吞掉**：`ResizeTo()` 里如果当前片只写了
   `<= kTransientFrames`（4 帧）就遇到新尺寸，**Finalize 掉它、删掉文件、
   原地换尺寸重新开**，不推进 `segmentSeq_`、不占槽位。已录够帧数的片绝不走
   这条捷径 —— 那正是用户要的"尺寸一变化立刻切片"。
   取 4 而不是 0，是因为"尺寸变了"到"采集跟上"通常还差一两帧旧尺寸数据；
   而用户真想看的画面不会只有 4 帧（30fps 下 0.13 秒）。
3. **开槽先挑空位**：`PickSlot()` 先找没被 `done_` 占用的槽号，全满时才复用
   最旧那片（并删掉它的文件）。以前是 `segmentSeq_ % keepSegments_` 纯轮转，
   会把还有效的片覆盖掉。

导出端配套：**"按回放时长截断"改成按每片真实时长累加**。原来是
`keep = maxSeconds / segmentSeconds + 1` 个片数上限，有大量过渡短片时 40 片
只留 11 片 ≈ 3 秒，剩下 17 秒全被补成 NO SIGNAL。现在从最后一片往前
`ProbeSegment()` 累加真实时长，累到"目标 + 一片标称"就停。

#### 9.5.2 尺寸抖动期间的开片策略：先按 2 倍预留、后来**关成 1:1**

9.5 解决了"改尺寸不要重开录制器"，9.5.1 解决了"过渡碎片不要占满槽位"。剩下的
硬问题是**开一片要 385ms**。

探针实测（`native/probe_type_switch.cpp`，已完成使命后删除）：一次开片
**385ms**，几乎全花在 `SetInputMediaType` 的硬件 H.264 MFT 初始化上，与分辨率
无关（2560x1440 385.1ms / 1920x1080 385.4ms / 1280x720 366.2ms / 640x480
394.2ms）；而"不 Finalize、直接 `SetInputMediaType` 换一个不同分辨率的输入
类型"虽然返回 `S_OK`，但**完全不生效**（后写的 30 帧被静默丢弃，成品只有前
30 帧 640x480）。片内跨尺寸这件事在 MF 里做不到。

**为什么多线程也救不了**（主人提过"编码需要时间你就用多线程"）：385ms 是每个
硬件编码器实例的初始化，不是排队等待。一次拖拽几十个尺寸，8 个 worker 并行
吞吐 8/0.385 ≈ 21 片/秒，仍追不上 30 帧/秒 × 每帧一个新尺寸，且追赶期间原始
帧要全压内存（4K 几秒 = 几 GB）。只能**减少开片次数**。

**第一版做法：过渡期把画布放大 2 倍预留**（`kTransitionFactorNum/Den = 2/1`、
`kTransitionMaxDim = 4096`、`kTransitionWindowMs = 2000`、`kSizeStableMs = 400`
稳住后切精确片）。窗口变大超出画布立刻切一片，变小就 1:1 贴左上角补黑。

**这一版后来被推翻了**（主人 m12458/m12477）：过渡画布让**分片文件的帧尺寸
变成真实画面的 2 倍**（窗口 1499x822 → 文件 4096x2912，画面只占左上角一小
块）。导出端按 `index.csv` 的真实尺寸裁剪本来没问题，可一旦某片的 index.csv
行读不到（槽位复写后旧行被整份重写掉，文件却还在盘上），contentW 只能退回
文件帧尺寸 → 20 秒内的"最大尺寸"被这个虚高值污染 → **用户导出一个比显示器
还大的视频（4096x2912），画面缩成左上角一小块**。且 2 倍画布把 NVENC 拖过
吞吐上限，实测录制直接**定格**（120 帧只 31 distinct）。

**现在的做法（`kTransitionFactorNum/Den = 1/1`）**：

- 窗口变大 → `Write()` 看到 `width > width_` → `ResizeTo()` **立刻切一片**，
  新片按新尺寸精确开（满足主人"为了那一变大的帧单独切一个片"的要求）。
- 窗口变小 → `WritePadded()` 1:1 贴左上角补黑继续写；尺寸稳住
  `kSizeStableMs = 400` 后再切一片精确尺寸的（`ResizeTo` 的稳定判定兜底）。
- **文件帧尺寸永远等于真实画面尺寸**，宁可多切几片 —— 这是主人明确选的取舍
  （m12213：「改变一下窗口大小就立刻切一次片……按照 20 秒内最大的视频尺寸
  作为最终的视频尺寸」）。

`WritePadded()` 的两条分支（`native/backend/MfEncoder.cpp`）：帧比画布小 → 1:1
贴左上角补黑。绝不缩放（主人 m08329 明确要求）。先 memset 清黑（一帧 1440p
8.3MB，比逐块算"哪块没覆盖到"简单可靠），再拷；整块 memcpy 的前提是"源行距
== 目标行距 == 拷贝宽度"，否则逐行拷（DXGI staging 纹理会对齐到 256 字节，
不判会把行尾填充当像素、画面斜切）。

替代方案（已记为否决）：每片一个独立编码线程。见上"多线程救不了"。

#### 9.5.3 贴图路径的片必须把真实尺寸写进 index.csv

`WritePadded` 路径（窗口拖小）画布比真实画面大、右下是补的黑边，**分片文件里
的帧尺寸不再等于用户看到的画面尺寸**。导出端如果按文件帧尺寸定画布，成品就会
被这块黑边撑大、用户画面缩成左上角一个小方块。

所以 `index.csv` 从四列扩到**六列**：

```
文件名,起始秒,结束秒,起始墙钟Unix毫秒,真实内容宽,真实内容高
000000.mp4,0.000000,2.000000,1792405999022,2560,1440
```

- 第五、六列由 `MfEncoder` 的 `segContentW_/segContentH_` 记（`Write()` 里每帧
  取真实帧尺寸的最大值，`WritePadded` 回头调 `Write` 时用 `padRecurse_` 跳过，
  否则记账会被补出来的黑边污染）。
- 0 表示不知道（老数据），导出端退回文件帧尺寸。
- 导出端（`native/backend/Exporter.cpp`）的 `struct SegmentFile` 有
  `contentW/contentH` 字段：画布取**所有片 contentW/contentH 的最大值**（不是
  文件帧尺寸最大值），贴图时对每片裁掉右侧/下方超出 content 的黑边再 1:1 贴上
  画布左上角。

实测（`%TEMP%\shadow-play\tr5`，window 源 explorer，窗口右侧拖出屏幕再拖回来）：
导出成品 `1982x692`，t=2/8/15 三帧沿中线横扫 **transitions: none**（没有白条、
没有黑边台阶），时长 19.666633 秒 / 590 帧。

### 9.6 录制帧率中途改变：按**新帧率**导出，旧的抽帧/补帧

这是需求里原有的一条，**核查结论：功能没被改掉，但只有"整数倍"是精确的**。

现状（`native/backend/Exporter.cpp` 的 `targetFps_` 路径，两条路都有）：

- 导出端拿到的 `targetFps_` 来自**当前配置**的 `video.fps`（`Session::Export`
  现取 `profile_.video.fps`），不是分片录制当时的帧率。
- 直拷路径（3b）：`repeat = outFps / fps`（仅当 `outFps >= fps && outFps % fps == 0`），
  每帧写 `repeat` 遍，时间戳按 `帧号 × 1e7 / outFps` 重算。
- 重编码路径（3c）：同样的 `repeat` 规则，画布帧率取 `targetFps`。
- **两种路径的每帧时长都是按目标帧率算的**，所以哪怕没做重复样本，播放器
  看到的时间轴长度也和真实时长一致（不会变速）。

所以真实行为是：

| 场景 | 结果 |
|---|---|
| 60fps 录 → 改成 240fps 导出 | ✅ 每帧重复 4 遍，成品 240/1，时长精确 |
| 240fps 录 → 改成 60fps 导出 | ✅ 每 4 帧留 1 帧（靠时间戳对齐到 1/60 网格），时长精确 |
| 60fps 录 → 改成 100fps 导出 | ⚠️ 不做重采样，按源 60fps 写，时长正确但帧率不是 100 |

**为什么非整数倍不插帧**：60 → 100 的每一帧要落在 1/60 和 1/100 的公倍数
网格上（300Hz），第 k 个输出帧该取源的第 `floor(k * 60/100)` 帧 —— 这要求
按输出帧号**重新采样源帧**，而源是压缩过的 H.264 裸流，直拷路径下我们手里
只有样本、没有"按输出帧号挑样本"的能力（挑出来的样本时间戳会跳）。重编码
路径理论上能做，但收益不抵复杂度：用户改帧率主要是 60/120/144/240 这些
整数倍关系。

真正的抽帧（240→60）要不要显式丢弃样本？**不用**。SinkWriter 的直拷通道
按 sample time 落盘，播放器按时间轴取帧；我们把每帧的 duration 设成
`1e7 / outFps`，60fps 的成品就是每秒 60 个时间格，多出来的源帧会因为
"落在同一个时间格"被播放器跳过 —— 视觉等价于抽帧，且时间轴严格不漂。

**这条以后要改的话**，改点在 `Exporter::Run()` 里两处 `repeat` 计算：
把"只在 `outFps % srcFps == 0` 时才重复"换成"按输出帧号从源帧队列里取"，
需要把源样本先读进一个小环形队列。记在这里免得下次又从头推理一遍。

#### 9.6.1 帧率超过硬件上限：录制端降档，导出端按**源帧率**铺时间轴

用户原话 (m11559)：「如果帧率设的太高，导出的视频卡成ppt的，不能按照尽可能
高的帧率去录制的吗」。

**录制端：硬件编码器有「分辨率 × 帧率」吞吐上限，超了整条路直接废掉。**
探针 `native/probe_fps_limit.cpp` 实测 `SetInputMediaType` 的 HRESULT：

| 分辨率 | 通过 | 失败（`0xC00D36B4` `MF_E_INVALIDMEDIATYPE`） |
|---|---|---|
| 2560x1440 | 165 / 166 / 168 / 170 / **172** | **174** / 176 / 178 / 180 / 200 / 220 / 240 |
| 1920x1080 | 165 / 180 / 240 | 300 |
| 1280x720 | 165 / 180 / 240 / 300 | — |
| 3840x2160 | 165 | 180 / 240 / 300 |

上限按宏块吞吐阶梯式变化。超限时的症状极具误导性：`status` 里 `actualFps`
照样报 240、`captureMs` 正常、界面看着在录，但 `recordFrames=0`、
`recordDetail` 是「编码器不接受 NV12 输入（hr=0xC00D36B4）」、
`000000.mp4` 只有 0 字节 —— 用户看到的就是「录出来卡成 PPT」。

修法 `MfEncoder::StartWithFpsFallback()`：阶梯
`{240, 165, 144, 120, 90, 60, 30}`，被拒就**在 OpenSegment 外层重开 writer**
（输出流上挂着旧的 `MF_MT_FRAME_RATE`，同一个 writer 里改不掉），降到编码器
接受的最高档继续录。只有 `status_.detail` 含 `MF_E_INVALIDMEDIATYPE` /
`0xC00D36B4` 才降档（权限、磁盘满降帧率没用）。`run.json` 写 `fps`（实际）+
`requestedFps`（用户配的）。

**导出端：时间轴必须按源分片真能给出的帧率铺，不能按配置值。** 两条路都要钳：

- 直拷路径（3b）：`canRaiseFps = outFps && fps > 0 && outFps >= fps && (outFps % fps) == 0`，
  `segOutFps = canRaiseFps ? outFps : fps`，`repeat = canRaiseFps ? outFps / fps : 1`。
- 重编码路径（3c）：`canvasFps` 同理，`outFps > srcMaxFps && (outFps % srcMaxFps) != 0`
  时把画布帧率降到 `srcMaxFps`。

**不钳的后果（实测）**：配 240 帧、实际录成 165fps、12 片 × 2 秒 = 24 秒内容，
导出成片 `avg_frame_rate=240/1`、`duration=13.708327`、`nb_frames=3290` ——
20 秒的内容被压成 13.7 秒（每秒只喂 165 个样本、每个样本只占 1/240 秒）。
钳完是 `avg_frame_rate=165/1`、`duration=19.939394`、`nb_frames=3290`，时长正确。

**教训**：这个钳制第一版只写在重编码分支里，而「所有分片尺寸一致 + 没有 NO
SIGNAL」时走的是**直拷**分支 —— 所以钳制形同不存在，白改一次。两条路必须同时改。

### 9.7 录制根目录：`recordings/`

``` 
recordings/                        ← 需求 3 说的「录到 test 里」，改用 recordings/ 更清楚
  .index.json                      ← profileId → 目录名 的分配表（保证不冲突）
  desktop-01-a3f2/
    segments/<轮次>/000000.mp4
    meta.json
  win-notepad-7c19/
    segments/...
```

导出默认目录 `exports/`，文件名：

```
exports/desktop-01_Replay_2026.10.16_18-03-12.mp4
```

（NVIDIA 是 `Replay_2026.10.16_18.03.12.mp4`；这里把 `:` 换成 `-`，
并在前面加配置名，避免多配置导出互相覆盖。）

（注：`segments/<轮次>/` 下每轮还有 `run.json`（帧率/尺寸/分片时长）和
`index.csv`（分片时间轴，含墙钟，见 9.3）。轮次目录只在**进程重启**或
配置变更时新建；同一路录制器在窗口尺寸变化时不再重开，见 9.5；
帧率变了则照旧重开（时间轴基准变了），导出端按 9.6 的办法对齐。）

### 9.8 暂不做音频

只出视频轨。ffmpeg 命令行先不加 `-f dshow` 之类，后续要加是在
编码线程里再挂一个 WASAPI loopback 捕获。

---

## 10. 已知风险

| 风险 | 影响 | 应对 |
|---|---|---|
| 管道喂不动 1440p60 | 帧率掉或延迟涨 | 实测；退路是命名管道 + 共享内存 |
| 同一显示器只能有一个 `IDXGIOutputDuplication` | 两个配置盯同一显示器时第二个建不出来 | 采集源按 output 去重共享（见 4.1） |
| WebGL context 数量上限（通常 16） | 配置多了预览黑屏 | 只给可见的配置分配 context，LRU 复用 |
| WGC 对受保护窗口出黑帧 | 录到黑屏 | 上报 `source_black`，界面提示 |
| 机器缺 H.264 编码器（Media Foundation） | 录不了 | `MfAcquire()` 自检时探测，失败就在状态里报 `建分片写入器失败`，预览仍可用 |
| 每切一片 SinkWriter 漏句柄 | 跑久了句柄数持续上涨（2 秒一片约 +10/秒） | 已量化，非线程/内存泄漏；根治需全 GPU 通路（见 9.2 已知遗留） |
| 导出时"没有样本就 `Finalize()`" | `0xC00D4A44` = `MF_E_SINK_NO_SAMPLES_PROCESSED` | 导出前先确认 `totalFrames > 0`；补 NO SIGNAL 后天然不会空（见 9.4） |
| 槽位轮转瞬间分片是 0 字节 | 复制分片报「字节数不对（期望 N，实际 0）」导致整次导出失败 | 复制重试 3 次，仍失败就跳过那一项继续导出，摘要里说明少了几片 |
| 窗口刚变宽时 WGC 的 `ContentSize` 比已绘制的区域大 | 若只按 `ContentSize` 取内容，右侧会带一条未绘制区 | 变大走"立刻切一片"路径（新片按精确尺寸开）；拖小走 `WritePadded` 1:1 贴左上角补黑，不会把未绘制区当成画面内容；导出端再按 `index.csv` 第五/六列裁真实内容尺寸（9.5.2/9.5.3） |
| 窗口被拖到屏幕外侧 | 屏幕外的部分 WGC 拿不到真实像素（实测是一块纯白），旧版本按"最小内容宽"取值时会把它算进画面 | 实测确认白条宽度随窗口宽成固定比例（169/2420、242/2568），导出端按 `contentW/contentH` 裁剪后不再出现在成品里 |
| 片内跨尺寸 `SetInputMediaType` 返回 `S_OK` 但不生效 | 若拿它做"不切片的跨尺寸"会录出画面错乱的成品 | 探针实测确认；因此只能"立刻切一片"（见 9.5.2），不能"切了不重开" |
| 过渡画布按 2 倍预留（已废弃的方案） | 文件帧尺寸 = 真实画面 ×2；index.csv 行丢失时导出画布被污染成 4096x2912；NVENC 超吞吐上限录制定格（120 帧只 31 distinct） | **已关成 1:1**（`kTransitionFactorNum/Den = 1/1`，见 9.5.2）：窗口变大立刻切精确片，变小贴左上角补黑，文件尺寸永远等于真实画面 |

