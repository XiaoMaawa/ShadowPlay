// Profiles.h — 采集配置（Profile）的数据模型 + 加载/保存
//
// 一个 Profile 就是一条「录什么 + 怎么录」的记录。服务启动时读这份配置，
// 给每个 enabled 的配置起一个会话（采集线程 + 帧环 + 编码）。
//
// 配置文件的写法见 DESIGN.md 第 7 节。这里只做**内嵌 JSON 解析**：
// 本机没有 nlohmann/json 之类的库，而我们的配置文件是自己写的、
// 格式完全可控，所以手写一个小解析器比引入依赖划算。
//
// 解析器故意写得保守：
//   * 只认配置文件里实际出现的语法（对象、数组、字符串、数字、true/false/null）
//   * 遇到不认识的结构直接跳过，不报错 —— 这样 Electron 那边加了新字段
//     旧服务也能跑
//   * 不做任何隐式类型转换（"60" 和 60 区别对待）

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace shadow {

// ---------------------------------------------------------------------------
// 窗口匹配规则（DESIGN.md 4.3）
// ---------------------------------------------------------------------------

// 按窗口的哪个字段匹配
enum class MatchField {
    Process, // 进程可执行文件名，例如 notepad.exe
    Title,   // 窗口标题
    Class,   // 窗口类名
};

// 匹配方式
enum class MatchMode {
    Exact,    // 完全相同（大小写不敏感）
    Contains, // 包含（大小写不敏感）
    Prefix,   // 前缀（大小写不敏感，用来省掉自己写 .*）
    Regex,    // 正则（ECMAScript 语法，std::regex）
};

// 多个窗口都命中时选哪个
enum class MatchPick {
    Foreground, // 当前前台窗口（没有就用 Largest）
    Largest,    // 面积最大的
    First,      // 枚举顺序里第一个
    Oldest,     // 创建最早的
};

struct WindowMatch {
    MatchField field = MatchField::Title;
    MatchMode  mode = MatchMode::Contains;
    MatchPick  pick = MatchPick::Largest;
    std::string pattern;
    uint32_t    pollMs = 1000;   // 多久重新匹配一次
    bool        excludeSelf = true; // 排除本服务自己的窗口（其实没有窗口）
    bool        requireVisible = true; // 只考虑可见、未最小化的窗口

    // 各字段自己的匹配值。
    //
    // 【为什么不能只有一个 pattern】界面是从"窗口下拉"里选一个源，选完会把那个
    // 窗口的**标题、类名、exe 名**三份指纹一起记下来，然后按用户拖的优先级
    // 逐个字段去试。可是以前真实参与匹配的只有 `pattern` 这一个值，而它装的是
    // **标题** —— 于是优先级降到 class 时，等于拿"本地磁盘 (D:) - 文件资源管理器"
    // 这个整串去和 `CabinetWClass` 比，永远不可能命中。降级链整个是断的：
    // 标题一变（用户换了个文件夹），这路就报"匹配不到窗口"，而且看起来像
    // "多个都匹配时选"这个选项坏了。
    //
    // 现在每个字段各存各的值，降级时用自己那份（见 PatternFor）。
    // 任一为空表示这个字段没有可用的指纹，匹配时跳过它、继续往后降级。
    std::string titlePattern;
    std::string classPattern;
    std::string processPattern;

    // 有序的匹配字段：界面让用户拖动排优先级，第一个是最优先的。
    //
    // 为什么要"有序"而不是单一字段：用户从下拉里选了一个窗口，界面会同时
    // 拿到它的标题、类名、exe 名。同一个程序开两个窗口时标题才是唯一区分，
    // 但标题会随文档变（"未命名 - 记事本" → "a.txt - 记事本"）；类名稳定
    // 但一个 exe 的多个窗口往往同类；exe 名最稳但最粗。按用户排的顺序逐个
    // 试，第一个能匹配到的字段说了算，就同时具备了"精确"和"能跟上变化"。
    //
    // 空 = 老配置，退回只用 field 一个字段（向后兼容）。
    std::vector<MatchField> fields;

    bool Valid() const { return !pattern.empty() || !titlePattern.empty() || !classPattern.empty() || !processPattern.empty(); }

    // 某个字段该用哪段文字去比。
    //
    // 该字段没有自己的指纹时退回 `pattern`（老配置只有它），这样旧文件
    // 的行为和以前完全一致。
    const std::string& PatternFor(MatchField f) const {
        switch (f) {
            case MatchField::Title:
                return titlePattern.empty() ? pattern : titlePattern;
            case MatchField::Class:
                return classPattern.empty() ? pattern : classPattern;
            case MatchField::Process:
                return processPattern.empty() ? pattern : processPattern;
        }
        return pattern;
    }

    // 实际参与匹配的字段序列（fields 为空时退回 {field}）。
    std::vector<MatchField> FieldOrder() const {
        if (!fields.empty()) return fields;
        return {field};
    }
};

// ---------------------------------------------------------------------------
// 采集源
// ---------------------------------------------------------------------------

enum class SourceKind {
    Desktop, // 整个虚拟桌面
    Monitor, // 某一台显示器（靠 monitorIndex 指定）
    Window,  // 某个窗口（靠 WindowMatch 匹配）
};

struct SourceSpec {
    SourceKind kind = SourceKind::Desktop;
    uint32_t  monitorIndex = 0;       // kind == Monitor 时有效
    bool      clientAreaOnly = false; // 窗口源：只录客户区（去掉标题栏边框）
    // 窗口源是否优先用 WGC（Windows Graphics Capture）。
    // 打开时窗口被别的窗口挡住/移出屏幕也能录到它自己；关掉就走裁剪法
    // （桌面复制抠窗口矩形那片像素），被挡住就录到遮挡物。
    bool      wgc = true;
};

// ---------------------------------------------------------------------------
// 视频参数
// ---------------------------------------------------------------------------

struct VideoSpec {
    uint32_t fps = 60;             // 录制帧率上限（0 = 不限制，跟随源）
    uint32_t bitrateMbps = 20;     // 目标码率
    uint32_t replaySeconds = 120;  // 回放缓冲时长：超过这个时长的旧分片会被删
    uint32_t segmentSeconds = 2;   // 每个分片的时长
    // 缩放（0 = 保持源尺寸）
    uint32_t scaleWidth = 0;
    uint32_t scaleHeight = 0;
};

// ---------------------------------------------------------------------------
// 配置
// ---------------------------------------------------------------------------

struct Profile {
    std::string id;       // 稳定唯一标识，同时用作共享内存会话名
    std::string name;     // 显示名
    bool        enabled = false; // 是否「一直录制」
    // 录像目录用的数字编号（1、2、3…）。
    //
    // 【为什么必须有一个数字】录像目录以前叫 `monitor-src-desk-3`：源类型 +
    // 名字里能留下的字母 + id 后四位，重名再补 -2/-3。中文配置名会被 Slugify
    // 整个折成 "-"，于是三条"整屏桌面"分别叫 src、src-2、src-3 —— 谁也说不清
    // 哪个目录对应哪条配置，删一条重建一条就又长一个。
    //
    // 现在目录名就是这个数字：`records/1`、`records/2`。看一眼就知道是配置 1。
    // 数字由界面分配、写进 profiles.json 的 "num" 字段，**创建后永不变**：
    // 删掉配置后它的号也不回收，避免新配置继承下一条旧录像。
    // 0 = 老配置没有这个字段，加载时补一个（见 ProfileStore::Load）。
    uint32_t    num = 0;
    SourceSpec  source;
    VideoSpec   video;
    // 只有 kind == Window 时才用得上；为空表示用 source 里带的默认匹配
    bool        hasMatch = false;
    WindowMatch match;
    std::string createdAt;

    // 用作 RingName 的会话 id：只保留 [A-Za-z0-9._-]，且必须以字母开头。
    // 配置 id 由 Electron 生成（p-xxxxxx），这里只做防御性过滤。
    std::string SessionId() const;
};

// ---------------------------------------------------------------------------
// 配置集合
// ---------------------------------------------------------------------------

struct ProfileDoc {
    uint32_t version = 1;
    std::string recordRoot;  // 录制根目录（空则用服务默认值）
    std::string exportDir;   // 导出目录
    std::vector<Profile> profiles;

    const Profile* Find(const std::string& id) const;
    Profile*       Find(const std::string& id);
};

class ProfileStore {
public:
    // 加载配置。文件不存在时返回一份空文档（不是错误 —— 首次运行就是这样）。
    // 解析失败时 outError 带上原因，返回值仍然可能含部分内容。
    static bool Load(const std::string& path, ProfileDoc& out, std::string& outError);

    // 保存配置（服务不改配置，这个主要给自检/测试用）
    static bool Save(const std::string& path, const ProfileDoc& doc, std::string& outError);
};

// ---------------------------------------------------------------------------
// 字符串 ↔ 枚举（给状态输出和解析用）
// ---------------------------------------------------------------------------

const char* ToString(SourceKind k);
const char* ToString(MatchField f);
const char* ToString(MatchMode m);
const char* ToString(MatchPick p);

// 解析失败时返回 false，out 保持原值
bool ParseSourceKind(const std::string& s, SourceKind& out);
bool ParseMatchField(const std::string& s, MatchField& out);
bool ParseMatchMode(const std::string& s, MatchMode& out);
bool ParseMatchPick(const std::string& s, MatchPick& out);

// 把 id 洗成合法的会话名（只含 [A-Za-z0-9._-]，首字符是字母）。
// 全部字符都不合法时返回 "s" + 原串的哈希，保证非空且稳定。
std::string SanitizeSessionId(const std::string& id);

} // namespace shadow
