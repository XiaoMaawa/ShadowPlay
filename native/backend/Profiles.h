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

// 匹配方式 —— 界面上的四个选项，**应用程序（exe）永远是必要条件**。
//
// 【为什么把"优先级降级"整个换掉】
// 老模型是「标题 → 类名 → exe 逐级降级，第一个能匹到的字段说了算」。它的致
// 命问题是会**悄悄换源**：用户录的是 B 站客户端的某个视频页，标题一变（切回
// 首页、换个视频），标题就永远匹不上了，于是降级到类名；而 Chrome_WidgetWin_1
// 这种类名满机都是（WorkBuddy、各种 Electron 应用都用它），`pick:largest`
// 会挑中面积最大的那个 —— 用户看到的正是「标题明明完全对不上，为什么录到了
// 别的窗口」。用户原话：「如果匹配不到，不要随便选择一个窗口啊，优先匹配改掉」。
//
// 新模型只有四个显式选项，每个都必须先满足应用程序，再叠加其它条件（全部
// AND，任意一条不满足就**不匹配**、这一路保持未采集）：
//   · 只匹配应用程序          —— 同一程序的哪个窗口都行
//   · 名字 + 应用程序        —— 同一个程序里按标题区分是哪一窗口
//   · 类名 + 应用程序        —— 同一程序里再按窗口类名收窄
//   · 名字 + 类名 + 应用程序  —— 最严格
// 没有"降级"，也就没有"标题对不上却录了别的窗口"这条路。
enum class MatchRule {
    ProcessOnly,       // 只匹配应用程序
    TitleProcess,      // 匹配名字和应用程序
    ClassProcess,      // 匹配类名和应用程序
    TitleClassProcess, // 匹配名字、类名和应用程序
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

    // 有序的匹配字段：老模型的产物（界面让你拖动排优先级），**只用于迁移**。
    //
    // 老服务按 fields 逐级降级，正是"标题对不上却录了别的窗口"的根源（见
    // MatchRule 的说明）。现在真正决定匹配的是 rule；fields 只在读老配置时
    // 用来推断"用户当初把哪个字段放第一位"，推断完就不再参与匹配。
    std::vector<MatchField> fields;

    // 匹配方式（界面下拉里那四个选项）。默认为「名字 + 应用程序」——
    // 同一程序开多个窗口时，只有标题能把它们分开，这是最常用的那一档。
    MatchRule rule = MatchRule::TitleProcess;

    // 用户在窗口下拉里**明确挑过**的那个窗口（hwnd，0 = 没挑过 / 未知）。
    //
    // 【为什么需要它】「只匹配应用程序」这一档里，同一个程序的多个窗口全都
    // 满足规则，按 pick:largest 永远只会挑面积最大的那一个 —— 于是用户在下拉
    // 里换成同一个程序的另一个窗口时，配置里的指纹（exe）一个字都没变，服务
    // 根本没理由换源，看起来就是"下拉能点，但切不过去"。
    //
    // 这个字段是**偏好**，不是条件：它只在"已经满足 rule 的候选窗口"里挑，
    // 绝不因为它而放宽匹配（否则又变成乱录别的窗口）。挑过的窗口关掉了，
    // 就自然退回 pick 逻辑 —— 这正是「只匹配应用程序」该有的行为。
    //
    // 用 hwnd 当偏好是安全的：hwnd 会被系统复用，但复用出来的窗口必须先满足
    // rule 才会被考虑，等于是"同一个程序的另一个窗口"，那本来就该录。
    uint64_t preferredHwnd = 0;

    // 配置文件里是否明确写了 "rule"。
    //
    // false = 老配置（或代码里临时构造的），此时按 fields/by 和已有的字段
    // 指纹**推断**一个 rule（见 Normalize），推断是纯函数，重复调用结果一致。
    bool ruleGiven = false;

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

    // 某个字段**自己那份**指纹（不退回 pattern）。
    //
    // 匹配时用这个而不是 PatternFor：退回 pattern 会让"类名"拿标题去比，
    // 那是老降级链里的隐形 bug。缺指纹就是缺，交给 Complete() 判不匹配。
    const std::string& FieldPattern(MatchField f) const {
        switch (f) {
            case MatchField::Title:
                return titlePattern;
            case MatchField::Class:
                return classPattern;
            case MatchField::Process:
                return processPattern;
        }
        return pattern;
    }

    // 这个 rule 要求**同时满足**哪些字段（全部 AND）。
    std::vector<MatchField> RequiredFields() const {
        switch (rule) {
            case MatchRule::ProcessOnly:
                return {MatchField::Process};
            case MatchRule::ClassProcess:
                return {MatchField::Class, MatchField::Process};
            case MatchRule::TitleClassProcess:
                return {MatchField::Title, MatchField::Class, MatchField::Process};
            case MatchRule::TitleProcess:
            default:
                return {MatchField::Title, MatchField::Process};
        }
    }

    // 必填字段里**真正有指纹**的那几个。
    //
    // 为什么要跳过空指纹而不是"缺一个就整体不匹配"：老配置可能只记了标题
    // （当年没有 exe 指纹），要求它必须有 exe 等于把这个配置永久判死。但跳过
    // 只针对"用户根本没填"的字段 —— 填了却没匹上，就是彻底不匹配，绝不降级。
    std::vector<MatchField> ActiveFields() const {
        std::vector<MatchField> out;
        for (MatchField f : RequiredFields()) {
            if (!FieldPattern(f).empty()) out.push_back(f);
        }
        return out;
    }

    // 把老配置补成新模型（就地修正，幂等）。Load 解析完会调一次，
    // MatchWindows 入口也会调一次，保证代码里临时构造的规则同样成立。
    void Normalize();

    // 给人看的规则描述，例如「名字包含「xxx」+ 应用程序「chrome.exe」」。
    // 匹配不到时直接写进这一路的状态说明，用户一眼知道差在哪。
    std::string Describe() const;

    // 实际参与匹配的字段序列（老模型遗留，只有迁移逻辑还用得到）。
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

    // 两边的「采集 + 编码参数」是否完全一致。
    //
    // 【为什么需要它】界面保存配置时会给每一路发一条 reload。以前 reload
    // 一律「拆掉旧会话 + 新建一份」—— 哪怕用户只是点开编辑又点了确定、
    // 一个字段都没改，也会重建录制器、重开一轮分片目录、清空帧环。
    // 界面那边看到的是画面黑一下、NO SIGNAL 遮罩闪一下（就是用户报的
    // 「窗口捕获点编辑确定后，导出的画面和预览都会短暂出现 no signal」）。
    //
    // 比较的是**会影响会话行为**的字段。刻意不比：
    //   * name      只用作导出文件名前缀，会话内部用不到；
    //   * enabled   它变了必须重建（开→关要停线程），由调用方单独判；
    //   * num / createdAt  纯界面用的元数据。
    bool SameSettings(const Profile& other) const;

    // 同上，但**忽略帧率**（video.fps）。
    //
    // 【为什么需要】帧率是可以就地改的：录制器的喂帧线程按新帧率走、编码器
    // 收掉当前片按新帧率开新片，墙钟靠 nextSegStartUnix_ 继续顺延。而其它
    // 字段（码率、分片时长、回放窗口、采集源）都写死在录制器/采集器构造时，
    // 改不了。
    //
    // reload 用它来决定走「就地改帧率」还是「拆掉重建」：用户把 144 改成 60
    // 时绝不能重建 —— 那会换轮次目录、recordSegments 归零、时间轴出现空洞，
    // 界面表现就是「短暂的 no signal」「导出按钮被禁用」「该路未在录制」
    // （m13003）。
    bool SameSettingsIgnoringFps(const Profile& other) const;

    // 同上，但连**码率**（video.bitrateMbps）一起忽略。
    //
    // 【为什么需要】码率和帧率一样能就地改：同样写在 SinkWriter 的输出流上，
    // 同样靠「收掉当前片、按新参数开新片」实现，不换轮次目录、不归零分片
    // 序号、时间轴继续顺延（见 MfEncoder::SetBitrate）。
    //
    // 以前只有 SameSettingsIgnoringFps 这一级，改码率就落进「拆掉重建」分支：
    // 用户只是把码率 20 改成 10，界面却走了和重启一样的路径 —— 新轮次目录、
    // recordSegments 归零（导出按钮变灰）、时间轴空洞被补成 NO SIGNAL。
    // 用户的原话（m14071）：「有关编辑的东西一保存立刻落地，像配置改帧率，
    // 改码率什么的」。
    bool SameSettingsIgnoringFpsBitrate(const Profile& other) const;

    // 同上，但**连回放时长一起忽略**（video.replaySeconds）。
    //
    // 【为什么需要】回放时长只决定环形缓冲保留多久，可以在录制过程中就地改
    // （见 MfEncoder::SetKeepSegments）。以前改它落进「拆掉重建」分支：预览
    // 立刻 NO SIGNAL、正在跑的导出被 reload 线程等住 —— 用户的原话是
    // 「改一下回放时间，预览出现 no signal，似乎什么东西重启了，我并不希望
    // 他重启，要求连续的录下去」。
    bool SameSettingsIgnoringFpsBitrateReplay(const Profile& other) const;
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
const char* ToString(MatchRule r);

// 解析失败时返回 false，out 保持原值
bool ParseSourceKind(const std::string& s, SourceKind& out);
bool ParseMatchField(const std::string& s, MatchField& out);
bool ParseMatchMode(const std::string& s, MatchMode& out);
bool ParseMatchPick(const std::string& s, MatchPick& out);
bool ParseMatchRule(const std::string& s, MatchRule& out);

// 把 id 洗成合法的会话名（只含 [A-Za-z0-9._-]，首字符是字母）。
// 全部字符都不合法时返回 "s" + 原串的哈希，保证非空且稳定。
std::string SanitizeSessionId(const std::string& id);

} // namespace shadow
