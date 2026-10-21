#pragma once

// ---------------------------------------------------------------------------
// GuardianMode —— 采集服务的守护进程，就是**服务 exe 自己的一个模式**。
//
//   shadow-capture --guard [--profiles <配置>]
//
// 【为什么不是独立的 node 脚本、也不由 Electron 拉起】
// 守护的职责是"服务没了就把它拉回来"。如果守护自己要靠 Electron 拉，那 Electron
// 关掉之后守护没了，服务再崩就没人管 —— 恰恰是最需要它的场景没覆盖到。
// 所以做成双向看门，两条边都在服务端：
//
//   服务启动 ──拉起──▶ 守护（shadow-capture --guard）
//   守护     ──盯着──▶ 服务（不在了就 CreateProcess 重新拉起）
//   服务     ──盯着──▶ 守护（心跳没了就重新拉起守护）
//
// 用同一个 exe 实现，好处很直接：不存在"守护要去找 node / 找脚本路径"的问题
// （GetModuleFileName 拿自己就行），也不用多一个进程依赖。
//
// 【为什么守护不能碰服务互斥体】
// 服务靠 `Local\ShadowPlay.Service.v1` 保证单实例。守护只是**观察**，所以用
// OpenMutex（打开已存在的）而不是 CreateMutex —— 后者一旦被守护持有，服务
// 死了互斥体也不销毁，"服务在不在"就永远为真，守护再也不会重启它。
//
// 心跳：<临时根>/status/guardian-v1.json（和状态文件同目录），守护每轮写、
// 服务每几秒读一次。界面也读它，用来显示"守护在岗"。
// ---------------------------------------------------------------------------

#include <cstdint>
#include <string>

namespace shadow {

/**
 * 守护主循环（--guard 模式）。不返回，进程被杀才结束。
 *
 * @param profilesPath 服务要用的配置文件（重新拉起服务时原样传回去）；
 *                     空串表示服务当初没带 --profiles，重启时也不带。
 */
int RunGuardianMode(const std::string& profilesPath);

/**
 * 拉起守护进程（detached，不继承控制台、不属于任何作业对象）。
 *
 * 服务启动成功后调一次；之后服务自己周期性检查心跳，发现守护没了再调。
 *
 * @return 是否成功创建了进程（不保证它已经开始工作，看心跳才知道）。
 */
bool SpawnGuardian(const std::string& profilesPath);

/**
 * 守护还在不在（心跳新鲜 **且** 心跳里记的 pid 还活着）。
 *
 * 两个条件都要：只看文件时间的话，守护被强杀之后它的心跳仍然"新鲜"；
 * 只看 pid 的话，pid 被系统回收复用就会误判成还活着。
 *
 * @param pidOut 可选：把心跳里记的守护 pid 带出去（给界面显示）。
 */
bool GuardianHeartbeatAlive(uint32_t* pidOut = nullptr);

/** 心跳文件路径（界面/服务/守护三方用同一个）。 */
std::string GuardianHeartbeatPath();

/**
 * 是否有人请求**整体停机**。
 *
 * 判定依据：状态目录下有没有 `shutdown-requested` 这个哨兵文件。它一旦存在，
 * 服务和守护都该退出、且不再互相拉起 —— 否则守护会把刚被杀的服务又拉起来，
 * "退出服务"就变成不可能完成的任务。
 *
 * 谁写它：托盘图标的"退出"按钮（native 侧）、`npm run service stop`、
 * `service:stop` IPC。写完再杀进程，双保险。
 */
bool ShutdownRequested();

/** 写下停机哨兵（让守护别再把服务拉起来）。 */
void WriteShutdownSentinel();

} // namespace shadow
