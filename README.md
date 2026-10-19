# ShadowPlay 预览（C++ 采集服务 + Electron 渲染）

把当前屏幕画面实时渲染到 Electron 窗口里的最小实现。**只做预览，不做重放。**

- 采集端：C++ + DXGI 桌面复制（Desktop Duplication），独立常驻进程。
- 渲染端：Electron + Vue 3 + WebGL2。
- 两者之间：共享内存 + 命名事件，**界面侧零拷贝**（见下文「数据通路」）。

```
┌──────────────────────────┐        共享内存 + 命名事件        ┌─────────────────────────┐
│  shadow-capture.exe      │  ──────────────────────────────►  │  Electron               │
│  (常驻服务，默认权限运行) │   ShadowPlay.Frame.<id>           │  ├ preload              │
│                          │   ShadowPlay.FrameEvt.<id>        │  │   └ N-API: shadow_ring │
│  DXGI 桌面复制           │                                   │  └ 渲染进程 (WebGL2)     │
│  BGRA8, 自上而下         │  ◄──────────────────────────────  │      Vue + canvas       │
└──────────────────────────┘        心跳 / 槽位状态            └─────────────────────────┘
```

> **`contextIsolation` 是关闭的**，这不是疏忽。preload 用 N-API 把最新一帧同步进
> 一块 V8 `ArrayBuffer`，这块内存要以零拷贝方式交给 WebGL；而 `contextBridge`
> 对返回值做**结构化克隆**，`ArrayBuffer` 会被整块拷一份（这里 59MB），主世界
> 拿到的是**死快照** —— 现象就是"界面只画出第一帧，之后不动"。改用 `MessagePort`
> 转移所有权也走不通：经 `contextBridge` 传过去的 `MessagePort` 只剩一个空对象
> （`postMessage` 是 `undefined`）。关掉隔离后 preload 与页面同处一个 JS 世界，
> 页面读到的就是同步泵正在写入的那块内存。风险可控：窗口只加载本地
> `dist/index.html`，不加载远程内容，也没有 `nodeIntegration`。

---

## 快速开始

```bat
:: 1) 编译原生部分（采集服务 + Electron 版 addon + 纯 Node 版 addon）
npm run build:native

:: 2) 编译前端（输出到仓库根的 dist/）
npm run build:web

:: 3) 启动采集服务（前台，Ctrl+C 停止；建议单独开一个终端常驻）
npm run service

:: 4) 另开一个终端启动界面
npm start
```

界面起来后会自动连上采集服务并开始预览，不需要额外点按钮。

`npm start` 是**前台运行**：Electron 的日志（含 preload 的
「帧同步中：xxx 次轮询/秒」）直接打在当前控制台，Ctrl+C 退出。
想让脚本立刻返回、把界面丢到后台，用 `npm run start:bg`。
采集服务无论如何都是独立进程，界面关掉它也继续跑。

### 如果窗口一闪而过

Electron 的崩溃上报组件在受限终端（容器 / 沙箱）里写不了文件，会让进程直接退出。
`npm start` 已经带上 `--disable-breakpad --disable-crash-reporter`。
如果还是立刻退出，说明渲染进程创建子进程需要管道但被拒了，再加一层：

```bat
set SHADOW_NO_SANDBOX=1
npm start
```

（PowerShell 用 `$env:SHADOW_NO_SANDBOX = "1"`）

### Electron 二进制放在哪

`electron` 的 npm 包只带 JS 外壳，二进制要单独下载。本机没有外网，
所以二进制放在两个位置（副本，内容相同）：

- `node_modules/electron/dist/` —— **标准位置**。`path.txt` 里写着
  `electron.exe`，所以 `npx electron .`、`require('electron')` 都能正常工作。
- `.electron/dist/` —— 手动解压的原始位置，作为备用。

`scripts/electron-path.mjs` 会自动找到可用的一份，并把
`ELECTRON_OVERRIDE_DIST_PATH` 指过去，保证所有调用方用的是同一个二进制。

> 注意：`path.txt` 存在但 `dist/electron.exe` 缺失时，electron 包会尝试去
> **下载**二进制；没有网络时它会静默失败 —— 表现就是 `npm start` 打印一行
> 提示后什么都没有。修好这个之后 `npm start` 才真正能开窗口。

### 开发模式

```bat
npm run dev          :: Vite 开发服务器 + Electron（前端热更新）
npm run service      :: 采集服务单独跑，改前端不会打断它
npm run service -- --fps 120   :: 同上，但把发布上限提到 120
npm run service -- --fg        :: 前台跑，日志直接打在终端里（排查用）
npm run service:stop :: 停止采集服务
```

> 采集服务**不需要管理员权限**，启动它的任何一条命令都不会弹 UAC。
> 它是个常驻后台的独立进程：关掉界面 / 关掉终端它都继续跑，只有
> `npm run service:stop` 能停。

### 帧率与限流

界面侧的绘制上限默认 **60fps**，采集端发布上限默认也是 **60fps**。两边都可以在
启动时改：

```bat
npm start -- --render-fps 120      :: 界面侧提到 120（采集端仍按 60 发布）
npm start -- --render-fps 0        :: 界面侧不限，跟随显示器刷新率
npm start -- --producer-fps 120    :: 采集端发布上限提到 120
npm start -- --producer-fps 0      :: 采集端不限制（桌面有变化就发）
```

> 只改界面侧是没用的：**最终帧率取两者较小值**。想真的跑满高刷屏，两边都要提。
> 实测（2560×1440）：默认两边 60 → 界面 ~59fps；两边都提到 120 → 界面 ~116fps。

**为什么默认定 60**（用户 m01860 要求的）：采集端桌面复制本身可以跑到 200Hz+
（真实帧来临即发），但没必要把每个新帧都画一遍 —— 屏幕也就 60Hz，多画纯烧 GPU。

### 自检

```bat
npm run probe              :: 跑一遍完整链路并把报告写到 native\build\artifacts\
npm run selftest           :: 真的开窗口跑几秒，核对有没有持续出帧、限流是否生效
npm run offsets            :: 打印 ShadowRingHeader 的真实字段偏移（改结构体后必跑）
```

`npm run selftest` 不只看"画过至少一帧" —— 那正是"只出一帧"这个 bug 的样子。
它会核对测试窗口内的累计帧数，以及绘制间隔中位数是否落在 60fps 预算附近
（`intervalMedianMs` ≈ 16.7）。采集端每秒的状态行也带 `gapMs` / `captureMs`，
用来区分"节流没算对"和"采集本身慢"。

还要看三样不能靠帧数证明的东西：`readySessionCount`（多路会话是不是都连上了）、
`aspectError`（预览有没有被拉伸，见上面的"保持纵横比"）、以及 `gui.*`
（配置表单有没有真读到配置、录制徽标有没有数字、导出按钮是不是可用的）。
这三样任一为空都会 `ok: false` 并在 `reason` 里说清是哪一环断的。

### 验收脚本（录制 / 导出）

前两条是**服务侧**行为，跟界面开没开无关，所以单独起 `shadow-capture.exe` 验，
不走 Electron：

```bat
npm run test:recording     :: 同一目录连录两轮，验分片落地、能解码、数量被回放时长限住
npm run test:export        :: 录 18 秒后导一份出来，验命名、时长、h264、ftyp 头
npm run test:selfheal      :: 验"窗口后开也能自动接上" + "关掉开关真的停止捕获"
npm run test:pipe          :: 只验命名管道命令通道（保存配置走的就是它）
npm run selftest:fresh     :: 自检前清掉本次录制目录，每次从零开始
```

### 临时文件在哪

仓库里**一个字节的临时产物都不留**，全在 `%TEMP%\shadow-play\app\` 下
（`dist/` 构建产物、`bin/` 服务运行副本、`records/` 录制分片、`exports/`
导出的 mp4、`proofs/` 验收脚本的证据、`status/` 状态镜像、`logs/` 日志、
`userdata/` Electron 缓存）。整个目录随时可以删掉，下次运行会自动重建。

删除规则：主进程每次启动先清掉上一次运行留下的产物（`dist/` 是构建产物，
保留），再扫一遍 `%TEMP%\shadow-play\` 下别的实例目录 —— 每个目录里有
`.owner` 标记，拥有者进程已经不在了就整棵删掉。所以被强杀、断电留下的
死目录不会一直堆积。

约定的唯一来源是 `app/temp-const.cjs`；临时根可以用环境变量
`SHADOW_TEMP_ROOT` 指到别处（测试、绿色版之类的场景）。

`test:recording` 的判据是**分片数被限住 + 时间轴持续推进**（`index.csv` 末行
一直在涨），而**不是**"最早那片文件名变了" —— 分片名是循环复用的槽位，回放 10 秒
/ 分片 2 秒就是 5 个槽位在 `000000.mp4`..`000004.mp4` 之间打转，任何时刻第一个文件
都叫 `000000.mp4`。同理 `index.csv` 是追加日志，不是"当前窗口清单"。

它默认**连录两轮进同一个目录**，这是有意的：槽位复写时**不会先截断**
文件，所以第二轮如果没清掉上一轮的分片，磁盘占用会虚高几倍
（回放 120 秒那档配置实测显示 356 MB，而窗口预算只有 120 MB 上下）。起录前清空
由 `MfEncoder::Start()` / `SegmentRecorder::ClearStaleSlots()` 负责，第二轮就是它的
回归闸门 —— `SHADOW_RECORD_ROUNDS=1` 可以只跑一轮，但那样就验不到这一条。

`test:export` 的产物名字会带配置名。**别删它里面那条"文件名里出现替换字符
U+FFFD 就失败"的断言** —— 中文配置名曾经因为外部工具拿到 GBK 路径而导出一堆
`????` 文件名，且换封装会"成功"产出一个 0 字节文件，光看退出码发现不了。
现在全程走 8 位 UTF-8 → 宽字符的显式转换（`Utf8Path.h`），没有这个转码层了。

---

## 目录结构

```
app/                          Electron 侧
  main.cjs                    主进程：窗口 + 采集服务状态（CommonJS）
  preload.cjs                 N-API 映射 + 每 4ms 同步最新一帧（与页面同一 JS 世界）
  probe-core.cjs              共享内存链路自检（可在纯 Node 里跑）
  service-const.cjs           会话 id 等常量（必须与 RingNames.h 一致）
frontend/                     Vue 3 + Vite
  src/composables/usePreview.ts   取帧循环 + sequence lock 校验
  src/lib/frameRenderer.ts        WebGL2 渲染 BGRA 帧
  src/lib/ringLayout.ts           头部字段偏移（JS 侧权威表）
native/
  ShadowRing.h                共享内存布局契约（C/C++ 双方唯一真源）
  ring_offsets_check.cpp      打印 offsetof，供 JS 侧抄写
  backend/                    C++ 采集服务
    main.cpp                    服务主循环、命令行、NDJSON 状态
    ScreenCapture.{h,cpp}       DXGI 桌面复制
    FrameRing.{h,cpp}           共享内存生产者
    RingNames.h                 命名规则 + 固定会话 id
bridge/
  shadow_ring.cc              N-API 消费者（映射、同步、序列锁）
  shim/                       本机没有 Node 头文件，手写的最小 N-API 声明
scripts/                      构建 / 启动 / 服务管理脚本
dist/                         前端构建产物
```

---

## 数据通路（为什么这么快）

1. **C++ 服务**用 DXGI 桌面复制拿到整屏 BGRA 帧，拷进共享内存的一个槽。
   槽布局是 `[槽头 432B][像素 stride*height]`，槽头里带着这一帧自己的
   宽/高/行距，消费者不需要依赖全局字段。
2. **addon**（`shadow_ring.node`）把服务端最新发布的槽 `memcpy` 进一块
   **V8 管理的 ArrayBuffer**，然后才写 `syncVersion`。
3. 这块 `ArrayBuffer` **由 preload 直接挂在 `window` 上**（不走 `contextBridge`，
   理由见开头那段说明）；渲染进程直接在它上面建 `Uint8Array` 视图并
   `texSubImage2D` 上传纹理。**渲染进程侧没有中间拷贝。**
4. 唯一的拷贝发生在服务端 → V8 缓冲区那一次（4K 约 14.7MB/帧，1080p 约 8.3MB/帧），
   由 preload 里一个 ~240Hz 的同步泵驱动；桌面静止时只比一个整数就返回。
5. 渲染循环用 `requestAnimationFrame` + `nextFrameAt` 把上限**锁在 60fps**；
   `syncVersion` 没变就直接返回，画面留在 canvas 上。

### 为什么不用 `napi_create_external_arraybuffer`（零拷贝的常规做法）

那才是"完全不拷贝"的写法，但在 **Electron 42 上不能用**：Electron 默认启用
V8 sandbox（指针压缩 + 内存笼），不允许 ArrayBuffer 指向笼子外的原生内存。
实测表现是：调用不报错，但拿到的属性是 `undefined`。
（Node 有 `napi_create_external_arraybuffer_not_in_sandbox` 可绕过，
但那等于关掉 V8 的内存保护，不适合作为默认路径。）

所以改成"服务端拷一次进 V8 内存"，代价是多占一份帧环大小的常驻内存
（4K 约 59MB），换来在所有运行时上都能跑。

### 同步：sequence lock

跨进程 + 跨 JS 世界，这里不用锁，用序号：

- 服务端每发布一帧，`header.counter` +1。
- addon 同步时先把 `syncVersion` 置 0，写完槽内容和头部标量，最后把
  `syncVersion` 设成 `counter`。
- 消费者读像素**前后各读一次** `syncVersion`；两次相同且非 0，说明期间
  没有被覆盖，这一帧完整；否则丢掉等下一帧。

因此服务端**不需要知道消费者读到哪了**，可以放心轮转覆盖所有槽位，
永远不会因为"消费者还没读完"而丢帧。

---

## 命令行参数

```
shadow-capture.exe [--session ID] [--output N] [--fps N] [--consumer-pid PID] [--once] [--status]

  --session <id>    自定义会话 id（默认固定服务 id shadowplay-service-v1）
  --output <n>      显示器序号，默认 0
  --fps <n>         目标帧率上限，0 = 桌面有变化就发（默认）
  --consumer-pid    手工登记消费者 pid（自检用）
  --once            打印几次状态后退出
  --status          只探测服务是否在运行（退出码 0 = 在运行）
```

stdout 是 NDJSON 状态流：

```json
{"type":"service_started","session":"...","pid":1234,"width":2560,"height":1440}
{"type":"waiting_for_consumer"}
{"type":"consumer_connected","pid":5678}
{"type":"status","fps":60.0,"frames":3600,"dropped":0,"width":2560,"height":1440,
 "modeChanges":2,"captureErrors":0,"connected":true,"consumerPid":5678}
{"type":"capture_reset"}   // UAC 弹窗 / 锁屏 / 分辨率切换后重建桌面复制
```

---

## 常见问题

**「还没有检测到采集服务」**
采集服务没在跑。另开一个终端执行 `npm run service`，界面会自动连上
（或点界面上的「重试连接」）。

**画面不出来 / 一片黑**
1. `npm run probe` 看链路哪一段断了；
2. `npm run offsets` 确认 JS 侧偏移和 C++ 结构体一致（改过
   `native/ShadowRing.h` 就必须同步 `app/preload.cjs`、`app/probe-core.cjs`、
   `frontend/src/lib/ringLayout.ts` 三处常量）；
3. 看采集服务 stdout 的 `capture_init_failed` / `captureErrors`。

**`无法初始化桌面复制`**
DXGI 桌面复制在远程桌面会话、或显示器序号不存在时不可用。
用 `--output 1` 试试另一块屏。

**升级显卡驱动或切换独显/集显后画面卡死**
正常现象：桌面复制会在驱动切换时失效。服务会自动 `capture_reset` 重建，
看 stdout 有没有刷 `capture_reset`。

**界面能开但窗口一闪而过 / 进程以 0x80000003 退出**
Electron 的崩溃上报组件在受限终端里写不了文件。`scripts/launch.mjs`
已经带上 `--disable-breakpad --disable-crash-reporter`。
如果你在容器 / 沙箱里跑，还需要 `SHADOW_NO_SANDBOX=1 npm start`
（会补上 `--no-sandbox`，因为渲染进程创建子进程需要管道）。

**`ELECTRON_RUN_AS_NODE`**
这个环境变量会让 `electron.exe` 退化成纯 Node（表现为
`require('electron')` 只返回一个路径字符串）。所有启动脚本都会**删除**
它 —— 注意是删除，不是置空；置成空字符串同样会退化成 Node 模式。

---

## 已知限制

- **单消费者**：共享内存里只有一个 `consumerPid`，同一时间只允许一个界面
  实例连接（多个界面实例会互相抢）。
- **分辨率变化会重建帧环**：共享内存容量在创建时固定，桌面分辨率变大时
  服务会重建整块环，消费者需要重新握手（代码里已处理）。
- **WebGL2 要求**：渲染用 `texture.bgra` 做 BGRA→RGBA 换序，需要有 WebGL2。
- 预览帧率上限取决于显示器实际刷新；桌面静止时服务端不发帧（这是有意的，
  省 GPU 和内存带宽）。
