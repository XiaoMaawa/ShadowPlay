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

### 4.2 单个窗口：Windows Graphics Capture（新增）

DXGI 桌面复制**不能**只抓一个窗口。抓窗口要用
`Windows::Graphics::Capture`（`GraphicsCaptureItem::CreateFromWindow`）。
本机是 Windows 11 (10.0.26200)，支持。

- 需要 `d3d11.dll` + `windows.graphics.capture.h`。用 C++/WinRT 会拖进一大堆
  头文件依赖；**改用 ABI 直调**（`RoGetActivationFactory` + 手写 `IInspectable`
  派生接口声明），跟现在手写 N-API 声明一个路子，不引入新依赖。
- 窗口关闭 / 目标进程退出时 `GraphicsCaptureItem` 会失效 → 正好作为需求 10 的
  触发点（比轮询标题更可靠）。

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
- 低延迟模式（`CODECAPI_AVLowLatencyMode`）+ 固定 GOP（2 秒一个关键帧，
  这样每个分片都能独立播放）。

> 为什么不用 ffmpeg.exe：本机没网络装不了，而且多一层进程和管道开销。

### 5.2 分片（需求 6）

```
%TEMP%/shadow-play/app/records/    ← 录制根目录（需求 3；路径来自 temp-const.cjs）
  <配置名>-<后缀>/
    segments/
      000000.ts                    ← 循环复用的槽位名，不是单调递增序号
      000001.ts
    segments.csv                   ← 追加日志：文件,起点秒,终点秒
    meta.json
```

> 为什么落在 `%TEMP%` 而不是仓库里：仓库可能被同步盘、杀毒、沙箱盯着，写盘既慢
> 又会给目录打上奇怪的安全标签（被打了 Low 完整性标签的目录，子进程继承之后
> WGC 直接 `0x80070005`）。临时产物本来就该待在临时目录里，整目录随时可删。

- 每个分片约 **2 秒**，封装用 **MPEG-TS**：
  选 TS 而不是 MP4，因为滚动删除时正在写的那个文件是「半截」的，
  TS 天生能容忍截断，且拼接时直接字节追加即可（不需要重写 moov）。
- 每写完一个分片就检查：`所有分片总时长 > 回放时长` 就删最旧的。
- 删除前必须确认该分片没有被导出流程占用（一个 `shared_ptr` 引用计数的句柄表）。

### 5.3 导出（需求 11、12）

导出 = 把 `segments/` 里的 TS 按序号拼接 + 换封装成 MP4。
两条路：

1. **纯字节拼接 + `ffmpeg -c copy`** —— 最简单，但本机没有 ffmpeg。
2. **Media Foundation `IMFSinkWriter` + 两个源读取器** —— 纯系统 API，
   但要自己实现「多源顺序读」的适配器。

倾向 **2**，因为不引入外部依赖。拼接时用 `MFCreateSourceReaderFromByteStream`
依次打开每个 TS，把样本转到 SinkWriter。

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
| **P3** | Media Foundation H.264 编码 + TS 分片落盘 | `test/<dir>/segments/` 出现能播放的 ts |
| **P4** | 滚动删除（回放时长）+ 目录索引 + 丢窗口自动关配置 | 播放一小时，目录大小稳定在设定值 |
| **P5** | 导出合并（SinkWriter 或 ffmpeg）+ 命名规则 | 导出的 mp4 能完整播放 |
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

### 9.2 编码：ffmpeg 子进程（不是链接 ffmpeg.dll）

调查结果：

| 途径 | 结论 |
|---|---|
| `C:\Windows\system32\ffmpeg.exe` | ✅ 2023-12-14 full build，`h264_nvenc` / `h264_qsv` / `h264_amf` / `h264_mf` 全都有 |
| `D:\DeepSeek\ffmpeg.dll` | 是真库（导出 `av_buffer_create` / `av_frame_alloc` / …），但**只有 DLL** |
| ffmpeg 开发头文件 / `.lib` 导入库 | ❌ 全盘都没有 |

**为什么不用那个 DLL**：FFmpeg 的 `AVCodecContext`、`AVFrame`、`AVPacket` 是
**公开布局的结构体**。没有头文件就要手写它们的完整定义 —— 版本一差、字段错位
就是内存踩踏，比手写 N-API 声明危险得多（那全是函数指针，没有这种坑）。
真要走 DLL 路线，需要用户提供 **ffmpeg shared 开发包**（含 `include/` + `lib/*.lib`）。
如果要，把包放到项目下，我可以切过去。

**现在的做法**：`ffmpeg.exe` 子进程 + 管道喂原始帧。

```
C++ 服务                           ffmpeg.exe
  采集帧 (BGRA)  --stdin 管道-->  -f rawvideo -pix_fmt bgra
                                    -c:v h264_nvenc -b:v <配置>
                                    -f segment -segment_time 2
                                    <录制根>/<配置>/segments/%06d.ts
```

- ffmpeg 自带的 `segment` muxer 天然做「固定时长切片」，序号单调递增，
  滚动删除只要按序号删旧的即可。
- 管道带宽：2560×1440×4 ≈ 14.7MB/帧，60fps ≈ **884MB/s**。
  匿名管道可以跑到 GB/s 级，但这是**需要实测**的一环；如果喂不动，
  退路是让 ffmpeg 读共享内存（用 `-f rawvideo -i \\.\pipe\<名字>` 命名管道，
  或者写一小段 DirectShow 源滤镜）。
- 编码线程与采集线程解耦：采集侧写一个**有界队列**（丢旧帧而不是阻塞采集）。

### 9.3 录制根目录：`recordings/`

``` 
recordings/                        ← 需求 3 说的「录到 test 里」，改用 recordings/ 更清楚
  .index.json                      ← profileId → 目录名 的分配表（保证不冲突）
  desktop-01-a3f2/
    segments/000000.ts
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

### 9.4 暂不做音频

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
| ffmpeg.exe 依赖系统路径 | 换机器可能没有 | 启动时探测，找不到就禁用录制（预览仍可用） |

