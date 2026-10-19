// exe_main.cpp — `shadow-capture.exe` 的入口
//
// 引擎本体（采集/编码/导出/命令管道）住在 `native/backend/*.cpp` 里，本文件
// 只负责"把命令行拆成 UTF-8 的 argv，然后交给 shadow::ServiceMain"。
//
// 为什么单独拆一个入口文件：`main()` 全工程只能有一个。入口逻辑本身只有
// 十几行，塞进 main.cpp 反而会把"服务怎么跑"和"命令行怎么来"混在一起，
// 拆开之后入口层与引擎层边界清楚。
//
// 顺带一个收益：main.cpp 里如果直接写 `int main(int argc, char** argv)`，
// 拿到的是 **ANSI** argv —— 路径里有中文（用户名、目录）时按当前代码页解
// 出来就是乱码，接着 CreateFile 直接 ENOENT。这里改成走
// GetCommandLineW + CommandLineToArgvW 拿 UTF-16，再用 ToUtf8 转成 UTF-8 ——
// 全工程内部一律 UTF-8，中文路径彻底不再是个坑。
//
// 一句话：入口这一层的唯一职责是"编码转换 + 转交"，不含任何业务。

#include "ServiceEntry.h"
#include "Utf8Path.h"

#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

int main() {
    // 控制台输出统一 UTF-8：stdout 上跑的是 NDJSON，界面端按 UTF-8 解。
    // 不设这一行，中文在管道里会变成当前代码页（中文系统是 GBK）。
    SetConsoleOutputCP(CP_UTF8);

    int argcW = 0;
    LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &argcW);
    if (!argvW) {
        // 极端情况（内存不足）才会走到。没有 argv 也照样可以让服务用默认
        // 配置跑起来，所以不 return —— argc 留在 1（只有程序名）。
        static char* fallback[] = {const_cast<char*>("shadow-capture"), nullptr};
        return shadow::ServiceMain(1, fallback);
    }

    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(argcW));
    for (int i = 0; i < argcW; ++i) args.push_back(shadow::ToUtf8(argvW[i]));
    LocalFree(argvW);
    if (args.empty()) args.push_back("shadow-capture");

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);

    return shadow::ServiceMain(static_cast<int>(args.size()), argv.data());
}
