#pragma once

// ---------------------------------------------------------------------------
// StatusFile —— 把服务状态镜像到一个状态文件，让**不是它启动者**的进程也看得见。
//
// 为什么需要这个东西：
//   采集服务的状态原来是走 stdout 的 NDJSON 流。stdout 有个硬限制 —— 只有
//   启动它的那个进程读得到。用 `npm run service`（或 cmd 里直接跑 exe）起的
//   服务，Electron 界面根本看不到它的任何状态：顶栏显示"外部服务"、录制徽标
//   永远是空的、导出按钮永久灰着，可服务其实跑得好好的。
//
// 为什么是**文件**而不是命名管道（这一段是血泪，别改回去）：
//   命名管道版本实现过、也验证失败了，失败原因很硬：
//     * 管道是 PIPE_ACCESS_OUTBOUND（只写），服务端**没法**读，所以挂不上
//       "对面还活着吗"的探针（同步零字节 ReadFile 在重叠管道上会阻塞死线程；
//       真去读又拿 ERROR_ACCESS_DENIED）。
//     * 观察者 close 之后，服务端这一侧的 WriteFile **一直成功**（出缓冲远没
//       填满，内核不认为对端已走），于是写线程永远挂在那个死掉的实例上，新
//       观察者被排到旧实例后面，一个字都收不到 —— 实测现象就是"界面关掉再开
//       就再也连不上"，连管道名都会从系统里消失。
//   文件没有"连接"这个状态，也就没有"卡在一个死连接上"这回事：每次都是
//   "整份覆盖写"，界面每次读到的都是最新完整的一份。
//
// 路径：<仓库根>\test\.status\status-v1.json
//   （仓库根 = 服务 exe 往上找到的、带 scripts 目录的那一级；环境变量
//    SHADOW_STATUS_FILE 可整体覆盖，测试脚本用它引到临时目录）
//   版本后缀改一次就是新协议，老界面读不到新文件时会安静地退化，
//   不会读到半懂不懂的字段。
//
// 写法：同目录写完临时文件再 MoveFileEx(..., MOVEFILE_REPLACE_EXISTING) 原子替换。
//   界面**不会**读到写了一半的 JSON —— 否则解析失败会被当成"服务没在跑"。
//
// 线程模型：
//   - Publish() 由服务主循环每秒调一次，只做"存下最新一份 + 唤醒写线程"，
//     不碰任何阻塞调用，主循环节拍不受"有没有人看"影响。
//   - 一个后台线程被唤醒后整份覆盖写文件；没人看的时候它只是每秒写 1KB。
//   - 界面关掉 = 再也不读那个文件，服务这边毫无感知，也不需要感知。
//
// 只写不读：命令还是走 stdin。谁起的服务谁发命令，这条边界不模糊。
// ---------------------------------------------------------------------------

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace shadow {

// 状态文件的固定路径（UTF-8）：<临时根>/status/status-v1.json。
// 临时根是 %TEMP%\shadow-play\app，可以被环境变量 SHADOW_TEMP_ROOT 覆盖。
// 界面侧（app/temp-const.cjs）用同一套规则复现它，两边必须一致。
std::string StatusFilePath();

class StatusFile {
public:
    StatusFile();
    ~StatusFile();

    StatusFile(const StatusFile&) = delete;
    StatusFile& operator=(const StatusFile&) = delete;

    // 起后台写线程。失败（极少见）只记在内部状态里，不阻碍服务运行。
    void Start();

    // 存下最新一份状态并唤醒写线程覆盖旧的。payload 是一整份完整的 JSON
    // 文档（末尾有没有换行都行，这里会补齐）—— 不是 NDJSON 行。
    void Publish(std::string payload);

    // 让后台线程收工（最多等约 2 秒，避免拖住服务退出）。退出前会把最后一
    // 份状态落盘，好让界面看到一次"服务要走了"之外的准确状态。
    void Stop();

    bool Running() const { return running_.load(std::memory_order_acquire); }
    uint64_t Writes() const { return writes_.load(std::memory_order_relaxed); }
    std::string LastError() const;

private:
    void ThreadMain();

    std::mutex* mu_ = nullptr;         // 保护 pending_ / seq_
    std::string* pending_ = nullptr;   // 最新一份状态（还没落盘的那份）
    uint64_t* pubSeq_ = nullptr;       // 已发布的次数，写线程靠它判断有没有新的一份
    std::string* lastError_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> writes_{0};
    void* thread_ = nullptr;           // HANDLE
};

} // namespace shadow
