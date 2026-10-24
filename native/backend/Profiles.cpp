// Profiles.cpp — 配置加载/保存 + 一个只够用的小 JSON 读取器
//
// 为什么要自己写：本机没有 json 库，而配置文件是我们自己生成的，
// 语法完全可控。这里的读取器只做「按路径取值」，不做通用 DOM ——
// 通用 DOM 要处理内存所有权、错误恢复，代码量会翻几倍，收益为零。
//
// 明确不支持的（读到就直接跳过，不报错）：
//   * 数字的指数形式（1e5）—— 配置文件里不会出现
//   * 深层嵌套（超过 kMaxDepth 就当读不到）
//   * 注释、尾随逗号（JSON 本来也不允许）

#include "Profiles.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "JsonUtil.h"
#include "EnvPath.h"    // ExpandEnv
#include "Utf8Path.h" // FromUtf8 / ToUtf8

namespace shadow {
namespace {

constexpr int kMaxDepth = 32;

// ---------------------------------------------------------------------------
// 极简 JSON 值：只支持我们需要的类型
// ---------------------------------------------------------------------------

struct Json {
    enum class Type { Null, Bool, Number, String, Object, Array };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;
    // 对象/数组都用「名字→值」的扁平列表表示；数组用下标当名字
    std::vector<std::pair<std::string, Json>> items;

    bool IsObject() const { return type == Type::Object; }
    bool IsArray() const { return type == Type::Array; }
    bool IsString() const { return type == Type::String; }

    // 按 key 找成员，找不到返回 nullptr
    const Json* Get(const std::string& key) const {
        for (const auto& kv : items) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }
    // 数组的第 index 个元素
    const Json* At(size_t index) const {
        return index < items.size() ? &items[index].second : nullptr;
    }
    size_t Size() const { return items.size(); }

    // 带类型检查的取值。类型不匹配就返回默认值 —— 这样前端下错类型
    // 不会让服务崩，只是这项配置退回默认。
    std::string AsString(const std::string& fallback = "") const {
        return type == Type::String ? text : fallback;
    }
    bool AsBool(bool fallback = false) const {
        if (type == Type::Bool) return boolean;
        if (type == Type::Number) return number != 0.0;
        return fallback;
    }
    double AsNumber(double fallback = 0.0) const {
        return type == Type::Number ? number : fallback;
    }
    uint32_t AsU32(uint32_t fallback = 0) const {
        if (type != Type::Number || !std::isfinite(number)) return fallback;
        if (number < 0.0) return 0;
        if (number > 4294967295.0) return 4294967295u;
        return static_cast<uint32_t>(number);
    }
};

// ---------------------------------------------------------------------------
// 读取器
// ---------------------------------------------------------------------------

class Reader {
public:
    explicit Reader(const std::string& src) : s_(src) {}

    // 解析整个字符串。失败时返回 Null 值并置 ok_ = false。
    Json Parse() {
        SkipWs();
        Json v = ParseValue(0);
        SkipWs();
        if (p_ != s_.size()) ok_ = false; // 尾部有多余内容
        return ok_ ? v : Json{};
    }

    bool ok() const { return ok_; }

private:
    void Fail() { ok_ = false; }

    bool Eof() const { return p_ >= s_.size() || !ok_; }
    char Peek() const { return p_ < s_.size() ? s_[p_] : '\0'; }

    void SkipWs() {
        while (p_ < s_.size()) {
            const char c = s_[p_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++p_;
            } else {
                break;
            }
        }
    }

    bool Expect(char c) {
        if (Peek() != c) {
            Fail();
            return false;
        }
        ++p_;
        return true;
    }

    Json ParseValue(int depth) {
        if (depth > kMaxDepth) {
            Fail();
            return Json{};
        }
        SkipWs();
        if (Eof()) return Json{};

        switch (Peek()) {
            case '{': return ParseObject(depth);
            case '[': return ParseArray(depth);
            case '"': {
                Json v;
                v.type = Json::Type::String;
                v.text = ParseString();
                return v;
            }
            case 't':
            case 'f': return ParseBool();
            case 'n': {
                if (s_.compare(p_, 4, "null") == 0) {
                    p_ += 4;
                    return Json{};
                }
                Fail();
                return Json{};
            }
            default: return ParseNumber();
        }
    }

    Json ParseObject(int depth) {
        Json v;
        v.type = Json::Type::Object;
        if (!Expect('{')) return Json{};
        SkipWs();
        if (Peek() == '}') {
            ++p_;
            return v;
        }
        while (true) {
            SkipWs();
            if (Peek() != '"') {
                Fail();
                return Json{};
            }
            std::string key = ParseString();
            if (!ok_) return Json{};
            SkipWs();
            if (!Expect(':')) return Json{};
            Json child = ParseValue(depth + 1);
            if (!ok_) return Json{};
            v.items.emplace_back(std::move(key), std::move(child));
            SkipWs();
            if (Peek() == ',') {
                ++p_;
                continue;
            }
            if (Peek() == '}') {
                ++p_;
                return v;
            }
            Fail();
            return Json{};
        }
    }

    Json ParseArray(int depth) {
        Json v;
        v.type = Json::Type::Array;
        if (!Expect('[')) return Json{};
        SkipWs();
        if (Peek() == ']') {
            ++p_;
            return v;
        }
        while (true) {
            Json child = ParseValue(depth + 1);
            if (!ok_) return Json{};
            v.items.emplace_back(std::string(), std::move(child));
            SkipWs();
            if (Peek() == ',') {
                ++p_;
                continue;
            }
            if (Peek() == ']') {
                ++p_;
                return v;
            }
            Fail();
            return Json{};
        }
    }

    // 读一个 UTF-8 字符串（含 \uXXXX 转义）
    std::string ParseString() {
        std::string out;
        if (!Expect('"')) return out;
        while (p_ < s_.size()) {
            const unsigned char c = static_cast<unsigned char>(s_[p_]);
            if (c == '"') {
                ++p_;
                return out;
            }
            if (c == '\\') {
                ++p_;
                if (p_ >= s_.size()) break;
                const char esc = s_[p_++];
                switch (esc) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': AppendUnicodeEscape(out); break;
                    default:
                        Fail();
                        return out;
                }
                continue;
            }
            // 普通字节直接抄（UTF-8 本身就是字节流）
            out.push_back(static_cast<char>(c));
            ++p_;
        }
        Fail();
        return out;
    }

    // \uXXXX → UTF-8。代理对（\uD800-\uDBFF + \uDC00-\uDFFF）要合并成一个码点，
    // 否则中文/emoji 在配置里会变成乱码。
    void AppendUnicodeEscape(std::string& out) {
        unsigned cp = 0;
        if (!ReadHex4(cp)) {
            Fail();
            return;
        }
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            // 期望紧跟一个低代理
            if (p_ + 1 < s_.size() && s_[p_] == '\\' && s_[p_ + 1] == 'u') {
                p_ += 2;
                unsigned low = 0;
                if (ReadHex4(low) && low >= 0xDC00 && low <= 0xDFFF) {
                    cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
                } else {
                    // 代理不完整：按替换字符处理，不报错
                    AppendUtf8(out, 0xFFFD);
                    return;
                }
            } else {
                AppendUtf8(out, 0xFFFD);
                return;
            }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            AppendUtf8(out, 0xFFFD); // 孤立低代理
            return;
        }
        AppendUtf8(out, cp);
    }

    bool ReadHex4(unsigned& out) {
        if (p_ + 4 > s_.size()) return false;
        unsigned v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[p_ + i];
            v <<= 4;
            if (c >= '0' && c <= '9') {
                v |= static_cast<unsigned>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                v |= static_cast<unsigned>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                v |= static_cast<unsigned>(c - 'A' + 10);
            } else {
                return false;
            }
        }
        p_ += 4;
        out = v;
        return true;
    }

    static void AppendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    Json ParseBool() {
        Json v;
        v.type = Json::Type::Bool;
        if (s_.compare(p_, 4, "true") == 0) {
            p_ += 4;
            v.boolean = true;
            return v;
        }
        if (s_.compare(p_, 5, "false") == 0) {
            p_ += 5;
            v.boolean = false;
            return v;
        }
        Fail();
        return Json{};
    }

    Json ParseNumber() {
        const size_t start = p_;
        if (Peek() == '-' || Peek() == '+') ++p_;
        bool any = false;
        while (p_ < s_.size()) {
            const char c = s_[p_];
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' ||
                c == '+' || c == '-') {
                ++p_;
                any = true;
            } else {
                break;
            }
        }
        Json v;
        if (!any) {
            Fail();
            return v;
        }
        v.type = Json::Type::Number;
        v.number = std::strtod(s_.substr(start, p_ - start).c_str(), nullptr);
        if (!std::isfinite(v.number)) {
            v.number = 0.0;
            Fail();
        }
        return v;
    }

    const std::string& s_;
    size_t p_ = 0;
    bool   ok_ = true;
};

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

std::string ToLower(std::string v) {
    for (char& c : v) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return v;
}

// JsonEscape 已经统一挪到 JsonUtil.h（inline），这里不再重复定义。

// ExpandEnv（路径里的 %TEMP% 展开）已经挪到 EnvPath.h/.cpp：
// addon 里也有一份 Exporter 要用它，而 addon 不编译本文件。

// 从 Json 里读一个字符串字段
std::string Str(const Json* obj, const char* key, const std::string& fallback = "") {
    if (!obj || !obj->IsObject()) return fallback;
    const Json* v = obj->Get(key);
    return v ? v->AsString(fallback) : fallback;
}

uint32_t Num(const Json* obj, const char* key, uint32_t fallback) {
    if (!obj || !obj->IsObject()) return fallback;
    const Json* v = obj->Get(key);
    return v ? v->AsU32(fallback) : fallback;
}

bool Flag(const Json* obj, const char* key, bool fallback) {
    if (!obj || !obj->IsObject()) return fallback;
    const Json* v = obj->Get(key);
    return v ? v->AsBool(fallback) : fallback;
}

} // namespace

// ---------------------------------------------------------------------------
// 枚举 ↔ 字符串
//
// 定义放在匿名 namespace **外面**，否则会和头文件里的声明冲突
// （C2668 "对重载函数的调用不明确"）。
//
// 解析时接受几种常见写法（process / Process / proc），这样前端改名不会
// 让整条配置失效；取不到就返回 false，调用方保留默认值。
// ---------------------------------------------------------------------------

const char* ToString(SourceKind k) {
    switch (k) {
        case SourceKind::Desktop: return "desktop";
        case SourceKind::Monitor: return "monitor";
        case SourceKind::Window:  return "window";
    }
    return "desktop";
}

const char* ToString(MatchField f) {
    switch (f) {
        case MatchField::Process: return "process";
        case MatchField::Title:   return "title";
        case MatchField::Class:   return "class";
    }
    return "title";
}

const char* ToString(MatchMode m) {
    switch (m) {
        case MatchMode::Exact:    return "exact";
        case MatchMode::Contains: return "contains";
        case MatchMode::Prefix:   return "prefix";
        case MatchMode::Regex:    return "regex";
    }
    return "contains";
}

const char* ToString(MatchPick p) {
    switch (p) {
        case MatchPick::Foreground: return "foreground";
        case MatchPick::Largest:    return "largest";
        case MatchPick::First:      return "first";
        case MatchPick::Oldest:     return "oldest";
    }
    return "largest";
}

const char* ToString(MatchRule r) {
    switch (r) {
        case MatchRule::ProcessOnly:       return "process";
        case MatchRule::TitleProcess:      return "title+process";
        case MatchRule::ClassProcess:      return "class+process";
        case MatchRule::TitleClassProcess: return "title+class+process";
    }
    return "title+process";
}

bool ParseMatchRule(const std::string& s, MatchRule& out) {
    const std::string v = ToLower(s);
    // 容忍空格和加号、以及中文界面可能写出来的写法
    std::string k;
    for (char c : v) {
        if (c == ' ' || c == '\\t') continue;
        k.push_back(c);
    }
    if (k == "process" || k == "app" || k == "exe" || k == "processonly") {
        out = MatchRule::ProcessOnly;
        return true;
    }
    if (k == "title+process" || k == "titleprocess" || k == "name+process" || k == "titleandprocess") {
        out = MatchRule::TitleProcess;
        return true;
    }
    if (k == "class+process" || k == "classprocess" || k == "classname+process") {
        out = MatchRule::ClassProcess;
        return true;
    }
    if (k == "title+class+process" || k == "titleclassprocess" || k == "title+classname+process" ||
        k == "title+class+app") {
        out = MatchRule::TitleClassProcess;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// WindowMatch::Normalize / Describe
// ---------------------------------------------------------------------------

void WindowMatch::Normalize() {
    // 1) 老配置只写了 pattern（装的其实是标题），把它落到对应字段的指纹里。
    const bool anyField = !titlePattern.empty() || !classPattern.empty() || !processPattern.empty();
    if (!anyField && !pattern.empty()) {
        switch (field) {
            case MatchField::Class:   classPattern = pattern;   break;
            case MatchField::Process: processPattern = pattern; break;
            case MatchField::Title:
            default:                  titlePattern = pattern;   break;
        }
    }

    // 2) 配置里没写 rule：按用户当初排的优先级推断一个。
    //
    // 【推断原则】尊重"首选字段"：老界面里排第一位的就是用户最看重的那个。
    //   · 首选 process            → 只匹配应用程序
    //   · 首选 class              → 类名 + 应用程序
    //   · 首选 title（老默认）    → 名字 + 应用程序
    // 有标题指纹时**不再**把类名塞进来：老配置里的 className 大多只是当年的
    // 中间降级项（Chrome_WidgetWin_1 这种满机都是），带上它只会让新模型更严、
    // 更容易匹配不到。
    if (!ruleGiven) {
        const MatchField primary = fields.empty() ? field : fields.front();
        switch (primary) {
            case MatchField::Process: rule = MatchRule::ProcessOnly;       break;
            case MatchField::Class:   rule = MatchRule::ClassProcess;      break;
            case MatchField::Title:
            default:
                if (!titlePattern.empty()) {
                    rule = MatchRule::TitleProcess;
                } else if (!classPattern.empty()) {
                    rule = MatchRule::ClassProcess;
                } else {
                    rule = MatchRule::ProcessOnly;
                }
                break;
        }
        ruleGiven = true;
    }
}

std::string WindowMatch::Describe() const {
    const auto nameOf = [](MatchField f) -> const char* {
        switch (f) {
            case MatchField::Process: return "应用程序";
            case MatchField::Class:   return "类名";
            case MatchField::Title:
            default:                  return "名字";
        }
    };
    const auto modeOf = [this]() -> const char* {
        switch (mode) {
            case MatchMode::Exact:    return "等于";
            case MatchMode::Prefix:   return "开头是";
            case MatchMode::Regex:    return "匹配正则";
            case MatchMode::Contains:
            default:                  return "包含";
        }
    };

    std::string out;
    for (MatchField f : RequiredFields()) {
        std::string pat = FieldPattern(f);
        if (pat.empty()) pat = "（空）";
        if (!out.empty()) out += " + ";
        out += nameOf(f);
        out += modeOf();
        out += "「";
        out += pat;
        out += "」";
    }
    return out;
}

bool ParseSourceKind(const std::string& s, SourceKind& out) {
    const std::string v = ToLower(s);
    if (v == "desktop" || v == "screen" || v == "all") {
        out = SourceKind::Desktop;
        return true;
    }
    if (v == "monitor" || v == "display" || v == "output") {
        out = SourceKind::Monitor;
        return true;
    }
    if (v == "window" || v == "hwnd" || v == "app") {
        out = SourceKind::Window;
        return true;
    }
    return false;
}

bool ParseMatchField(const std::string& s, MatchField& out) {
    const std::string v = ToLower(s);
    if (v == "process" || v == "proc" || v == "exe") {
        out = MatchField::Process;
        return true;
    }
    if (v == "title" || v == "caption") {
        out = MatchField::Title;
        return true;
    }
    if (v == "class" || v == "classname") {
        out = MatchField::Class;
        return true;
    }
    return false;
}

bool ParseMatchMode(const std::string& s, MatchMode& out) {
    const std::string v = ToLower(s);
    if (v == "exact" || v == "equals") {
        out = MatchMode::Exact;
        return true;
    }
    if (v == "contains" || v == "substr") {
        out = MatchMode::Contains;
        return true;
    }
    if (v == "prefix" || v == "startswith") {
        out = MatchMode::Prefix;
        return true;
    }
    if (v == "regex" || v == "regexp" || v == "re") {
        out = MatchMode::Regex;
        return true;
    }
    return false;
}

bool ParseMatchPick(const std::string& s, MatchPick& out) {
    const std::string v = ToLower(s);
    if (v == "foreground" || v == "active") {
        out = MatchPick::Foreground;
        return true;
    }
    if (v == "largest" || v == "biggest") {
        out = MatchPick::Largest;
        return true;
    }
    if (v == "first") {
        out = MatchPick::First;
        return true;
    }
    if (v == "oldest") {
        out = MatchPick::Oldest;
        return true;
    }
    return false;
}

std::string SanitizeSessionId(const std::string& id) {
    std::string out;
    out.reserve(id.size());
    for (const char c : id) {
        const bool okChar = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        out.push_back(okChar ? c : '_');
    }
    if (out.empty()) return "s";
    // 必须字母开头（RingName 拼接后不能出现非法首字符）
    if (!((out[0] >= 'A' && out[0] <= 'Z') || (out[0] >= 'a' && out[0] <= 'z'))) {
        out.insert(out.begin(), 's');
    }
    return out;
}

std::string Profile::SessionId() const {
    return SanitizeSessionId(id.empty() ? name : id);
}

bool Profile::SameSettings(const Profile& o) const {
    // 采集源
    if (source.kind != o.source.kind) return false;
    if (source.monitorIndex != o.source.monitorIndex) return false;
    if (source.clientAreaOnly != o.source.clientAreaOnly) return false;
    if (source.wgc != o.source.wgc) return false;

    // 视频参数（scaleWidth/scaleHeight 直接决定编码器画布，必须比）
    if (video.fps != o.video.fps) return false;
    if (video.bitrateMbps != o.video.bitrateMbps) return false;
    if (video.replaySeconds != o.video.replaySeconds) return false;
    if (video.segmentSeconds != o.video.segmentSeconds) return false;
    if (video.scaleWidth != o.video.scaleWidth) return false;
    if (video.scaleHeight != o.video.scaleHeight) return false;

    // 窗口匹配规则（只对窗口源有意义，但比一下也无害 —— 桌面源两边都是默认值）
    if (hasMatch != o.hasMatch) return false;
    if (hasMatch) {
        if (match.field != o.match.field) return false;
        if (match.mode != o.match.mode) return false;
        if (match.pick != o.match.pick) return false;
        if (match.pattern != o.match.pattern) return false;
        if (match.pollMs != o.match.pollMs) return false;
        if (match.excludeSelf != o.match.excludeSelf) return false;
        if (match.requireVisible != o.match.requireVisible) return false;
        if (match.titlePattern != o.match.titlePattern) return false;
        if (match.classPattern != o.match.classPattern) return false;
        if (match.processPattern != o.match.processPattern) return false;
        // 匹配方式（四个选项）直接决定"要不要换源"，改了必须重建会话
        if (match.rule != o.match.rule) return false;
        // 用户挑过的窗口：改了它就必须重建会话，否则"换成同程序的另一个窗口"
        // 这一保存不会生效（新偏好只写进了文件，运行中的会话还在录旧窗口）。
        if (match.preferredHwnd != o.match.preferredHwnd) return false;
        // 老模型的优先级顺序：只在迁移时用得到，但仍在文件里，比一下无害
        if (match.fields != o.match.fields) return false;
    }

    return true;
}

bool Profile::SameSettingsIgnoringFps(const Profile& o) const {
    // 唯一和 SameSettings 的区别就是**不比 video.fps**（它可以在录制过程中
    // 就地改，见 Session::ApplyLiveSettings 与 SegmentRecorder::SetFps）。
    //
    // 实现上直接复制一份、把 fps 抹平再交给 SameSettings：这样将来
    // SameSettings 加了新字段，这里自动跟着比，不会漏。
    Profile a = *this;
    Profile b = o;
    a.video.fps = 0.0;
    b.video.fps = 0.0;
    return a.SameSettings(b);
}

bool Profile::SameSettingsIgnoringFpsBitrate(const Profile& o) const {
    // 与 SameSettingsIgnoringFps 同一套实现思路：复制一份、把能就地改的字段
    // （帧率、码率）抹平再交给 SameSettings。将来 SameSettings 加了新字段，
    // 这里自动跟着比，不会漏。
    Profile a = *this;
    Profile b = o;
    a.video.fps = 0.0;
    b.video.fps = 0.0;
    a.video.bitrateMbps = 0;
    b.video.bitrateMbps = 0;
    return a.SameSettings(b);
}

bool Profile::SameSettingsIgnoringFpsBitrateReplay(const Profile& o) const {
    // 与 SameSettingsIgnoringFpsBitrate 同一套思路：把**能就地生效**的三个字段
    // （帧率、码率、回放时长）抹平再交给 SameSettings。
    //
    // 【为什么回放时长也能就地改】它只决定环形缓冲保留多久（换算成保留片数），
    // 和采集器 / 编码器的构造参数无关。以前改它会被判成"配置变了"→ 拆掉整路
    // 会话重建：预览 NO SIGNAL、导出被打断/变慢（用户实测报的就是这个）。
    Profile a = *this;
    Profile b = o;
    a.video.fps = 0.0;
    b.video.fps = 0.0;
    a.video.bitrateMbps = 0;
    b.video.bitrateMbps = 0;
    a.video.replaySeconds = 0;
    b.video.replaySeconds = 0;
    return a.SameSettings(b);
}

const Profile* ProfileDoc::Find(const std::string& id) const {
    for (const auto& p : profiles) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

Profile* ProfileDoc::Find(const std::string& id) {
    for (auto& p : profiles) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// 加载
// ---------------------------------------------------------------------------

bool ProfileStore::Load(const std::string& path, ProfileDoc& out, std::string& outError) {
    out = ProfileDoc{};
    outError.clear();

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        // 文件不存在不是错误：首次运行时就是没有配置
        outError = "配置文件不存在（首次运行是正常的）";
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    if (text.empty()) {
        outError = "配置文件是空的";
        return false;
    }

    Reader reader(text);
    const Json root = reader.Parse();
    if (!reader.ok() || !root.IsObject()) {
        outError = "JSON 解析失败（位置信息未提供，请检查语法）";
        return false;
    }

    out.version = Num(&root, "version", 1);
    out.recordRoot = ExpandEnv(Str(&root, "recordRoot"));
    out.exportDir = ExpandEnv(Str(&root, "exportDir"));

    const Json* arr = root.Get("profiles");
    if (arr && arr->IsArray()) {
        for (size_t i = 0; i < arr->Size(); ++i) {
            const Json* item = arr->At(i);
            if (!item || !item->IsObject()) continue;

            Profile p;
            p.id = Str(item, "id");
            p.name = Str(item, "name");
            p.enabled = Flag(item, "enabled", false);
            p.num = Num(item, "num", 0);
            p.createdAt = Str(item, "createdAt");
            if (p.id.empty()) {
                // 没 id 的配置直接跳过：id 是会话名和目录名的来源，不能缺
                continue;
            }

            // --- source ---
            const Json* src = item->Get("source");
            if (src && src->IsObject()) {
                SourceKind k = p.source.kind;
                if (ParseSourceKind(Str(src, "kind"), k)) p.source.kind = k;
                p.source.monitorIndex = Num(src, "monitorIndex", p.source.monitorIndex);
                p.source.clientAreaOnly = Flag(src, "clientAreaOnly", false);
                p.source.wgc = Flag(src, "wgc", true);
            }

            // --- video ---
            const Json* vid = item->Get("video");
            if (vid && vid->IsObject()) {
                p.video.fps = Num(vid, "fps", p.video.fps);
                p.video.bitrateMbps = Num(vid, "bitrateMbps", p.video.bitrateMbps);
                p.video.replaySeconds = Num(vid, "replaySeconds", p.video.replaySeconds);
                p.video.segmentSeconds = Num(vid, "segmentSeconds", p.video.segmentSeconds);
                p.video.scaleWidth = Num(vid, "scaleWidth", 0);
                p.video.scaleHeight = Num(vid, "scaleHeight", 0);
                // 兜底：0 会让分片永远不轮转，必须给个下限
                if (p.video.segmentSeconds == 0) p.video.segmentSeconds = 2;
                if (p.video.replaySeconds == 0) p.video.replaySeconds = 120;
                // 【帧率上限 120，超过的一律夹回来】硬件编码器在 2560x1440 上
                // 物理上限是 172fps（探针实测），而 165fps 那一档的每帧成本
                // 6.09ms 已经顶破 6.06ms 的预算，喂帧端只能补重复帧 —— 真实
                // 画面只剩一半。与其"配 240 自动降到 165 再被看门狗降到 144"
                // 绕这么一圈，不如从一开始就不给超过 120 的档位：120 及以下
                // 实测真实画面 89~93%、落盘 100%，是稳定区间。
                // 手改过 json 的老配置也在这里被夹，不会带着 240 继续跑。
                if (p.video.fps > 120.0) p.video.fps = 120.0;
                if (p.video.fps < 1.0) p.video.fps = 1.0;
            }

            // --- match ---
            const Json* m = item->Get("match");
            if (m && m->IsObject()) {
                WindowMatch wm;
                MatchField f = wm.field;
                if (ParseMatchField(Str(m, "by"), f)) wm.field = f;
                MatchMode md = wm.mode;
                if (ParseMatchMode(Str(m, "mode"), md)) wm.mode = md;
                MatchPick pk = wm.pick;
                if (ParseMatchPick(Str(m, "pick"), pk)) wm.pick = pk;
                wm.pattern = Str(m, "pattern");
                // 各字段自己的指纹。界面从窗口下拉里选源时会把三份都写进来，
                // 匹配时按 rule 组合使用（见 WindowMatch::RequiredFields）。
                wm.titlePattern = Str(m, "title");
                wm.classPattern = Str(m, "className");
                wm.processPattern = Str(m, "process");
                // 【老配置的 pattern 交给 Normalize 落到字段里】
                // 以前这里无条件写 `titlePattern = pattern`，等于假设 pattern 装的
                // 一定是标题。可老配置里 `by` 完全可能是 "process" 或 "class"，
                // 那样就会把 exe 名塞进标题字段，新模型按 AND 一匹就永远匹不上
                // （而且报的是"名称不匹配"，指向完全错误的方向）。
                // Normalize 会看 `by` 决定落进哪个字段。
                wm.pollMs = Num(m, "pollMs", wm.pollMs);
                if (wm.pollMs < 100) wm.pollMs = 100; // 别让前端写出 0 导致忙轮询
                wm.requireVisible = Flag(m, "requireVisible", true);
                // 用户挑过的窗口（偏好，不参与条件比较）。负数/0 都当没挑过。
                {
                    const double hw = Num(m, "hwnd", 0);
                    wm.preferredHwnd = hw > 0 ? static_cast<uint64_t>(hw) : 0;
                }
                // 匹配方式（四个显式选项）。没写就是老配置，交给 Normalize 推断。
                MatchRule mr = wm.rule;
                if (ParseMatchRule(Str(m, "rule"), mr)) {
                    wm.rule = mr;
                    wm.ruleGiven = true;
                }
                // 老模型的 byOrder 只留着给 Normalize 推断"首选字段"，不再参与匹配。
                const Json* byOrder = m->Get("byOrder");
                if (byOrder && byOrder->IsArray()) {
                    for (size_t k = 0; k < byOrder->Size(); ++k) {
                        const Json* fv = byOrder->At(k);
                        if (!fv || !fv->IsString()) continue;
                        MatchField ff;
                        if (!ParseMatchField(fv->AsString(), ff)) continue;
                        // 去重：同一个字段排两次没有意义，还会让匹配白白多跑一遍
                        bool dup = false;
                        for (auto e : wm.fields) {
                            if (e == ff) { dup = true; break; }
                        }
                        if (!dup) wm.fields.push_back(ff);
                    }
                }
                // 老配置在这里补成新模型（推断 rule、把 pattern 落到字段指纹）。
                wm.Normalize();
                p.match = wm;
                p.hasMatch = true;
            }

            out.profiles.push_back(std::move(p));
        }
    }

    // ---- 补数字编号 ------------------------------------------------------
    //
    // 老配置没有 "num" 字段。这里按"缺哪个补哪个"来补齐：先收集已经用掉的号，
    // 再从 1 往上找空位。**只改内存里的副本，不写盘** —— 写盘由界面做，
    // 服务读到 0 也能靠这个兜底算出稳定的号（同一个文件、同样的顺序 →
    // 同样的分配结果），所以不会出现"服务算一个号、界面算另一个号"。
    {
        std::vector<bool> used;
        for (const auto& p : out.profiles) {
            if (p.num == 0) continue;
            if (used.size() <= p.num) used.resize(static_cast<size_t>(p.num) + 1, false);
            used[p.num] = true;
        }
        uint32_t next = 1;
        for (auto& p : out.profiles) {
            if (p.num != 0) continue;
            while (next < used.size() && used[next]) ++next;
            p.num = next;
            if (used.size() <= next) used.resize(static_cast<size_t>(next) + 1, false);
            used[next] = true;
            ++next;
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// 保存
// ---------------------------------------------------------------------------

bool ProfileStore::Save(const std::string& path, const ProfileDoc& doc, std::string& outError) {
    outError.clear();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        outError = "无法写入配置文件：" + path;
        return false;
    }

    out << "{\n";
    out << "  \"version\": " << doc.version << ",\n";
    out << "  \"recordRoot\": \"" << JsonEscape(doc.recordRoot) << "\",\n";
    out << "  \"exportDir\": \"" << JsonEscape(doc.exportDir) << "\",\n";
    out << "  \"profiles\": [\n";
    for (size_t i = 0; i < doc.profiles.size(); ++i) {
        const Profile& p = doc.profiles[i];
        out << "    {\n";
        out << "      \"id\": \"" << JsonEscape(p.id) << "\",\n";
        out << "      \"num\": " << p.num << ",\n";
        out << "      \"name\": \"" << JsonEscape(p.name) << "\",\n";
        out << "      \"enabled\": " << (p.enabled ? "true" : "false") << ",\n";
        out << "      \"createdAt\": \"" << JsonEscape(p.createdAt) << "\",\n";
        out << "      \"source\": {\"kind\": \"" << ToString(p.source.kind)
            << "\", \"monitorIndex\": " << p.source.monitorIndex
            << ", \"clientAreaOnly\": " << (p.source.clientAreaOnly ? "true" : "false")
            << ", \"wgc\": " << (p.source.wgc ? "true" : "false")
            << "},\n";
        out << "      \"video\": {\"fps\": " << p.video.fps
            << ", \"bitrateMbps\": " << p.video.bitrateMbps
            << ", \"replaySeconds\": " << p.video.replaySeconds
            << ", \"segmentSeconds\": " << p.video.segmentSeconds
            << ", \"scaleWidth\": " << p.video.scaleWidth
            << ", \"scaleHeight\": " << p.video.scaleHeight << "}";
        if (p.hasMatch) {
            out << ",\n      \"match\": {\"rule\": \"" << ToString(p.match.rule)
                << "\", \"by\": \"" << ToString(p.match.field)
                << "\", \"byOrder\": [";
            // 有序优先级：界面拖动排的，第一个最优先。空列表就不写出来
            // （老版本服务不认识这个键，写空数组反而会让它以为用户清空了）。
            const auto order = p.match.fields;
            for (size_t k = 0; k < order.size(); ++k) {
                if (k) out << ", ";
                out << "\"" << ToString(order[k]) << "\"";
            }
            out << "], \"mode\": \"" << ToString(p.match.mode)
                << "\", \"pick\": \"" << ToString(p.match.pick)
                << "\", \"pattern\": \"" << JsonEscape(p.match.pattern)
                // 三份字段指纹要一起写回去。少了它们，服务自己保存一次配置
                // 就会把界面写进来的降级指纹抹掉，下一次重新加载后匹配又变回
                // "只有标题可用"。
                << "\", \"title\": \"" << JsonEscape(p.match.titlePattern)
                << "\", \"className\": \"" << JsonEscape(p.match.classPattern)
                << "\", \"process\": \"" << JsonEscape(p.match.processPattern)
                << "\", \"pollMs\": " << p.match.pollMs
                << ", \"hwnd\": " << p.match.preferredHwnd
                << ", \"requireVisible\": " << (p.match.requireVisible ? "true" : "false")
                << "}\n";
        } else {
            out << "\n";
        }
        out << "    }" << (i + 1 < doc.profiles.size() ? "," : "") << "\n";
    }
    out << "  ]\n";
    out << "}\n";
    return true;
}

} // namespace shadow
