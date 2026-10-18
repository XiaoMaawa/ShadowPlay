// RingNames.h — 共享内存 / 命名事件的统一命名规则。
//
// 生产者和消费者必须用完全一致的规则拼接名字，所以放在一个头文件里。
// 名字里只允许出现 [A-Za-z0-9._-]，避免落在 Global\ 命名空间时的转义问题。
#pragma once

#include <string>

namespace shadow {

// 常驻服务的固定会话 id。
//
// 采集服务是一个独立的常驻进程，Electron 只负责"连上它"而不是"拉起它"，
// 所以双方必须约定一个固定的名字，不能由 Electron 随机生成。
// 这样 Electron 重启、刷新页面都不影响采集端继续跑。
inline const char* kServiceSessionId() {
    return "shadowplay-service-v1";
}

// 会话 id 由调用方给出（常驻模式用上面的固定值，也可以由命令行显式指定）。
// 前缀全部加在名字尾部，保证名字本身始终以字母开头。
inline std::string RingName(const std::string& id) {
    return "ShadowPlay.Frame." + id;
}
inline std::string ReadyEventName(const std::string& id) {
    return "ShadowPlay.Ready." + id;
}
inline std::string FrameEventName(const std::string& id) {
    return "ShadowPlay.FrameEvt." + id;
}
inline std::string DisconnectEventName(const std::string& id) {
    return "ShadowPlay.Disc." + id;
}

// 服务互斥体：保证同一台机器上只有一个采集服务在跑。
// 用 Local\ 前缀 —— 作用域是当前登录会话，正是我们要的粒度。
inline std::string ServiceMutexName() {
    return "Local\\ShadowPlay.Service.v1";
}

// 命令通道的命名管道。
//
// 【为什么从 stdin 换成命名管道】
// 命令原来只走服务的 stdin，而 stdin 只有**启动服务的那个进程**能写：
//   * `scripts/launch.mjs --bg` 是用 stdio:'ignore' 拉起的服务，stdin 是
//     NUL，服务端的 ReadFile 立刻读到 EOF，命令通道直接死掉；
//   * 在 cmd 里直接跑 exe 起的服务同样没有可用的 stdin。
// 于是"界面保存配置 → 让服务重新加载"这条路，只要服务不是 Electron 自己
// 拉起来的，就完全走不通 —— 只能退回"杀掉整个服务再拉一个"，把隔壁分路一起
// 掐断。命名管道没有这个限制：谁起的服务都能连上来发命令。
//
// 方向是**客户端写、服务端读**（服务端 PIPE_ACCESS_INBOUND + 反复
// ConnectNamedPipe 接受新连接），所以不存在 StatusFile.h 里记的那个"服务端
// 没法读、挂不上存活探针"的坑 —— 那条教训针对的是**观察者**管道（服务只写），
// 这里是命令管道（服务只读），语义正好相反。
inline std::string CommandPipeName() {
    return "\\\\.\\pipe\\ShadowPlay.Cmd.v1";
}

} // namespace shadow
