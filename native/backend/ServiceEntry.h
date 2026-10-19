// ServiceEntry.h — 采集服务的进程无关入口
//
// main.cpp 里原来有一个从头顶跑到尾的 main()。把它抽成 ServiceMain() 的
// 好处有两个，都不涉及"要不要子进程"这件事：
//
//   1. 入口文件和引擎本体分开（native/backend/exe_main.cpp）。argv 走
//      CommandLineToArgvW 取 UTF-16 再转 UTF-8，中文用户名/目录不会变成
//      乱码；原来 `main(int argc, char** argv)` 拿到的是 ANSI argv，按当前
//      代码页解出来是 GBK，紧接着 CreateFile 就 ENOENT。
//
//   2. 一次性查询（状态 / 窗口列表 / 显示器列表）可以直接返回字符串，
//      不必每个调用方都去 CreateProcess 一个 shadow-capture.exe。同一进程内
//      复用这层时就是一次函数调用，省掉几十毫秒的进程启动。
//
// **服务仍然是独立进程**：app/main.cjs 用 spawn 拉起 shadow-capture.exe，
// detached 常驻，界面退出它继续跑。本文件没有把引擎搬进 Electron 进程。
//
// 退出码语义与改造前完全一致，别改：
//   0 正常 / 2 参数错 / 3 采集初始化失败 / 4 共享内存或互斥体失败 / 5 已有实例
#pragma once

#include <string>

namespace shadow {

// 跑一次采集服务（阻塞到服务结束），返回值就是进程退出码。
//
// argv[0] 约定是"程序名"（随便填什么都行，不参与解析）；从 argv[1] 开始按
// main.cpp 里 ParseCommandLine 的规则解析。
//
// 线程安全：同一时刻只应该有一个 ServiceMain 在跑（互斥体会把第二个挡回去，
// 返回 5）。
int ServiceMain(int argc, char** argv);

// 让正在跑的 ServiceMain 尽快返回。
//
// 只置停止标志，具体收尾（停会话、Finalize 分片、落状态文件）仍然由
// ServiceMain 自己按正常路径做完 —— 不要在别处直接杀进程/杀线程，那样分片
// 索引和状态文件会停在半截。
void RequestServiceStop();

// 服务互斥体是否被占（等价于老的 `shadow-capture --status` 的退出码 0/1）。
bool QueryServiceRunning();

// 下面三个返回**完整的多行 NDJSON 文本**（末尾不带换行），与打印到 stdout
// 的内容逐字节相同。exe 的 --status / --list-windows / --list-monitors
// 就是直接把它们的返回值 printf 出去。
std::string QueryStatusJson();
std::string QueryWindowsJson();
std::string QueryMonitorsJson();

} // namespace shadow
