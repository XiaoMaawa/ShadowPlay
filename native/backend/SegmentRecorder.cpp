// SegmentRecorder.cpp —— 分片录制的实现。设计说明见 SegmentRecorder.h。
#include "SegmentRecorder.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#include "JsonUtil.h"
#include "MfEncoder.h"
#include "Utf8Path.h"

namespace shadow {
namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// 目录分配
// ---------------------------------------------------------------------------

/** 把任意显示名压成只含 [a-z0-9-] 的短名，空则返回 "src"。 */
std::string Slugify(const std::string& name, size_t maxLen) {
    std::string out;
    bool lastDash = false;
    for (unsigned char c : name) {
        if (c >= 'a' && c <= 'z') {
            out.push_back(static_cast<char>(c));
            lastDash = false;
        } else if (c >= 'A' && c <= 'Z') {
            out.push_back(static_cast<char>(c - 'A' + 'a'));
            lastDash = false;
        } else if (c >= '0' && c <= '9') {
            out.push_back(static_cast<char>(c));
            lastDash = false;
        } else {
            // 中文字节、空格、符号一律折成连字符，但不连续出两个
            if (!lastDash && !out.empty()) {
                out.push_back('-');
                lastDash = true;
            }
        }
        if (out.size() >= maxLen) break;
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? std::string("src") : out;
}

const char* SourceKindSlug(SourceKind k) {
    switch (k) {
        case SourceKind::Desktop: return "desktop";
        case SourceKind::Monitor: return "monitor";
        case SourceKind::Window:  return "win";
    }
    return "src";
}

/** 读 .index.json 里的 profileId -> dir 映射。 */
bool ReadIndex(const fs::path& indexPath, std::vector<std::pair<std::string, std::string>>& out) {
    std::ifstream in(indexPath, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    if (text.empty()) return true;

    // 极简解析：只找 "profiles": { "id": "dir", ... } 里的一层键值对。
    // 写这份文件的只有我们自己，格式受控。
    const size_t key = text.find("\"profiles\"");
    if (key == std::string::npos) return true;
    const size_t open = text.find('{', key);
    if (open == std::string::npos) return true;

    int depth = 0;
    size_t i = open;
    std::string pendingKey;
    const auto skipWs = [&](size_t& p) {
        while (p < text.size() &&
               (text[p] == ' ' || text[p] == '\n' || text[p] == '\r' || text[p] == '\t')) {
            ++p;
        }
    };
    for (; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '{') {
            ++depth;
            continue;
        }
        if (c == '}') {
            --depth;
            if (depth < 1) break;
            continue;
        }
        if (c != '"') continue;
        // 读一个字符串
        std::string s;
        ++i;
        while (i < text.size() && text[i] != '"') {
            if (text[i] == '\\' && i + 1 < text.size()) ++i;
            s.push_back(text[i]);
            ++i;
        }
        size_t p = i + 1;
        skipWs(p);
        if (p < text.size() && text[p] == ':' && depth == 1) {
            size_t q = p + 1;
            skipWs(q);
            if (q < text.size() && text[q] == '"') {
                std::string v;
                ++q;
                while (q < text.size() && text[q] != '"') {
                    if (text[q] == '\\' && q + 1 < text.size()) ++q;
                    v.push_back(text[q]);
                    ++q;
                }
                out.emplace_back(s, v);
            }
        } else if (depth == 1) {
            pendingKey = s;
        }
        (void)pendingKey;
        i = p;
        if (i >= text.size()) break;
        --i;
    }
    return true;
}

/**
 * 分配目录这一整段是**读-改-写**：读 .index.json → 挑一个没被占的名字 →
 * 建目录 → 整份重写 .index.json。多路配置同时启动时会并发跑到这里
 * （Session.cpp 里每个会话各自在自己的线程上调 AllocateProfileDir），
 * 于是：
 *   - 两路都读到旧的 index，挑中同一个名字；
 *   - 两路都写自己的 .tmp，然后都去 rename 覆盖 .index.json —— Windows 的
 *     MoveFileEx 在目标已被别的线程抢先改名时不覆盖，直接 ERROR_ACCESS_DENIED，
 *     输的那路就报「替换 .index.json 失败」而整路录制起不来。
 * 用一把进程内锁把这段串起来。跨进程（服务 + 预览）仍有窗口，靠下面的
 * rename 重试兜住。
 */
std::mutex& IndexFileMutex() {
    static std::mutex mu;
    return mu;
}

/**
 * 一路一个分配记录文件，路径是 `<recordRoot>/.alloc/<profileId>.json`。
 *
 * 为什么不继续用共享的 `.index.json`：那是一份**读-改-写**的文件，多路配置
 * 同时启动时必然竞争。踩过的坑是一路 rename 覆盖时撞上
 * `ERROR_SHARING_VIOLATION`（The process cannot access the file because it is
 * being used by another process.），重试 20 次仍失败 —— 那一路录制直接起不来，
 * 界面显示「替换 .index.json 失败」，而且失败会随机落在哪一路上。
 *
 * 根除办法不是"把锁再加厚一层"，而是**取消共享的可变文件**：
 *   * 谁占哪个目录，由 create_directory 的原子性决定（成功即是我的）；
 *   * 谁分配过什么，记在自己那个文件里 —— 只有自己在写，没有第二个人竞争。
 *
 * profileId 会被用作文件名，所以必须过滤掉路径分隔符等危险字符。
 */
std::string SafeFileStem(const std::string& id) {
    std::string out;
    out.reserve(id.size());
    for (unsigned char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
        out.push_back(ok ? static_cast<char>(c) : '_');
    }
    if (out.empty()) out = "profile";
    return out;
}

} // namespace

std::string AllocateProfileDir(const std::string& recordRoot, const Profile& profile,
                               std::string& outError) {
    outError.clear();
    if (recordRoot.empty()) {
        outError = "没有配置录制根目录";
        return {};
    }

    std::error_code ec;
    fs::path root = fs::u8path(recordRoot);
    fs::create_directories(root, ec);
    if (ec) {
        outError = "建不了录制根目录：" + ec.message();
        return {};
    }

    // ---- 0) 有新式数字编号就直接用它，一了百了 ---------------------------
    //
    // 目录名 = `records/<num>`，num 来自 profiles.json 的 "num" 字段，
    // 由界面在创建配置时分配、之后永不变。这样：
    //   * 名字和配置一一对应，不用再"猜"哪堆录像属于哪条配置；
    //   * 不再有重名补 -2/-3 的问题，也不会因为改名/换源类型又长出一个新目录；
    //   * 中文名不再被 Slugify 折成一串连字符导致所有配置撞名。
    //
    // 老的 `<源>-<名字>-<尾号>` 目录仍然认领（下面第 1、2 步），保证升级后
    // 主人已有的录像不会被丢下；认领到的老目录会**继续沿用**。
    if (profile.num > 0) {
        const std::string name = std::to_string(profile.num);
        fs::path dir = root / fs::u8path(name);
        ec.clear();
        fs::create_directories(dir / "segments", ec);
        if (ec) {
            outError = "录像目录建不起来：" + ec.message();
            return {};
        }

        // 每建一次就写一份 meta.json，用来说明这堆录像属于哪条配置。
        // （界面把配置名改了，这里的名字也跟着更新，但目录名 num 不动。）
        {
            std::ofstream meta(dir / "meta.json", std::ios::binary | std::ios::trunc);
            if (meta) {
                meta << "{\n"
                     << "  \"profileId\": \"" << JsonEscape(profile.id) << "\",\n"
                     << "  \"profileName\": \"" << JsonEscape(profile.name) << "\",\n"
                     << "  \"sourceKind\": \"" << SourceKindSlug(profile.source.kind) << "\",\n"
                     << "  \"directory\": \"" << JsonEscape(name) << "\"\n"
                     << "}\n";
            }
        }
        return PathToUtf8(dir);
    }

    const fs::path allocDir = root / ".alloc";
    fs::create_directories(allocDir, ec);
    if (ec) {
        outError = "建不了分配记录目录：" + ec.message();
        return {};
    }

    const fs::path indexPath = root / ".index.json";
    const fs::path allocPath = allocDir / (SafeFileStem(profile.id) + ".json");

    // ---- 1) 本路自己的分配记录：有就直接复用 -----------------------------
    //
    // 这是重启后"录像还在同一个目录里"的保证。自己的文件、只有自己在写，
    // 所以读它不需要任何锁。
    {
        std::ifstream in(allocPath, std::ios::binary);
        if (in) {
            std::stringstream ss;
            ss << in.rdbuf();
            const std::string text = ss.str();
            // 格式极简，只有一个字段：{"dir": "目录名"}
            const size_t key = text.find("\"dir\"");
            if (key != std::string::npos) {
                const size_t colon = text.find(':', key);
                const size_t open = colon == std::string::npos ? std::string::npos
                                                               : text.find('"', colon);
                const size_t close = open == std::string::npos ? std::string::npos
                                                               : text.find('"', open + 1);
                if (close != std::string::npos && close > open + 1) {
                    const std::string name = text.substr(open + 1, close - open - 1);
                    if (!name.empty() && name.find("..") == std::string::npos &&
                        name.find('/') == std::string::npos &&
                        name.find('\\') == std::string::npos) {
                        fs::path dir = root / fs::u8path(name);
                        ec.clear();
                        fs::create_directories(dir / "segments", ec);
                        if (!ec) return PathToUtf8(dir);
                        outError = "录像目录建不起来：" + ec.message();
                        return {};
                    }
                }
            }
        }
    }

    // ---- 2) 兼容旧的共享索引：读它来找本路的旧分配 -------------------------
    //
    // 只读，不写。主人已有的录像目录靠这一步保住 —— 升级之后那些目录会被
    // 认领回同一个 profile，不会被重新分配成新名字。
    {
        std::vector<std::pair<std::string, std::string>> known;
        IndexFileMutex().lock();
        ReadIndex(indexPath, known);
        IndexFileMutex().unlock();

        for (const auto& kv : known) {
            if (kv.first != profile.id || kv.second.empty()) continue;
            fs::path dir = root / fs::u8path(kv.second);
            ec.clear();
            fs::create_directories(dir / "segments", ec);
            if (ec) continue;
            // 认领成功，写进自己的分配文件，下次就不用再读索引了。
            std::ofstream rec(allocPath, std::ios::binary | std::ios::trunc);
            if (rec) rec << "{\"dir\": \"" << JsonEscape(kv.second) << "\"}\n";
            return PathToUtf8(dir);
        }
    }

    // ---- 3) 新分配 --------------------------------------------------------
    //
    // 名字用 <源类型>-<短名>-<id 后4位>，重名就加 -2、-3。
    //
    // 关键：判定"这个名字被占了没有"**不靠任何共享表**，而是直接
    // create_directory —— 它是原子的，成功就说明这个名字是本路的，
    // 失败就说明已经被别人（别的配置、上一次的服务、甚至另一个进程）占了。
    // 这样即使索引文件整个丢了或者写坏了，也绝不会两路录进同一个目录。
    std::string tail = profile.id;
    if (tail.size() > 4) tail = tail.substr(tail.size() - 4);
    const std::string base = std::string(SourceKindSlug(profile.source.kind)) + "-" +
                             Slugify(profile.name, 24) + "-" + Slugify(tail, 8);

    fs::path dir;
    std::string chosen;
    for (int attempt = 1; attempt < 1000; ++attempt) {
        chosen = (attempt == 1) ? base : (base + "-" + std::to_string(attempt));
        fs::path candidate = root / fs::u8path(chosen);
        ec.clear();
        // 已经存在时 create_directory 不算错误（ec 为空），所以必须分开判：
        // 先看存在性，再看"由本路创建成功"。
        const bool existed = fs::exists(candidate, ec);
        if (ec) continue;
        if (existed) {
            // 目录可能是上一次录制的残留（索引丢了），也可能是别人正在用。
            // 只要它的 meta.json 不是别的 profile，就认领它。
            std::ifstream meta(candidate / "meta.json", std::ios::binary);
            bool sameProfile = false;
            if (meta) {
                std::stringstream ss;
                ss << meta.rdbuf();
                const std::string t = ss.str();
                sameProfile = t.find("\"profileId\": \"" + JsonEscape(profile.id) + "\"") !=
                              std::string::npos;
            }
            if (!sameProfile) continue;
            // meta 是我们的，但还得确认它没有正在被别的会话用 —— 由调用方
            // （每路一个 id）保证 id 唯一，这里不再重复判断。
            dir = candidate;
            break;
        }

        ec.clear();
        const bool made = fs::create_directories(candidate / "segments", ec);
        if (ec) {
            // 别人在同一瞬间抢先建了 —— 换下一个候选名，不是错误。
            continue;
        }
        if (!made) continue; // 竞态下没建成，重试下一个名字
        dir = candidate;
        break;
    }

    if (dir.empty()) {
        outError = "分配不到录像目录（重名候选都试过了）";
        return {};
    }

    // 记进本路自己的分配文件。
    {
        std::ofstream rec(allocPath, std::ios::binary | std::ios::trunc);
        if (!rec) {
            outError = "写不了分配记录：" + PathToUtf8(allocPath);
            return {};
        }
        rec << "{\"dir\": \"" << JsonEscape(chosen) << "\"}\n";
    }

    // 每个目录一份 meta.json，说明这堆录像属于哪个配置。
    {
        std::ofstream meta(dir / "meta.json", std::ios::binary | std::ios::trunc);
        if (meta) {
            meta << "{\n"
                 << "  \"profileId\": \"" << JsonEscape(profile.id) << "\",\n"
                 << "  \"profileName\": \"" << JsonEscape(profile.name) << "\",\n"
                 << "  \"sourceKind\": \"" << SourceKindSlug(profile.source.kind) << "\",\n"
                 << "  \"directory\": \"" << JsonEscape(chosen) << "\"\n"
                 << "}\n";
        }
    }

    return PathToUtf8(dir);
}

// ---------------------------------------------------------------------------
// SegmentRecorder
// ---------------------------------------------------------------------------

SegmentRecorder::SegmentRecorder(std::string dir, VideoSpec video, uint32_t width,
                                 uint32_t height)
    : dir_(std::move(dir)), video_(video), width_(width), height_(height) {
    if (!dir_.empty()) {
        segmentsDir_ = PathToUtf8(fs::u8path(dir_) / "segments");
    }
    fps_ = video_.fps ? video_.fps : 60u;
    if (fps_ > 240) fps_ = 240; // 离谱值兜一下，别让 GOP 算爆
}

SegmentRecorder::~SegmentRecorder() { Stop(); }

std::string SegmentRecorder::ListFilePath() const { return listFile_; }
std::string SegmentRecorder::SegmentsDir() const { return segmentsDir_; }

bool SegmentRecorder::Running() const {
    std::lock_guard<std::mutex> lk(mu_);
    return stats_.active;
}

// 清掉本轮运行目录里上一轮留下的分片和清单。
//
// 为什么必须清：分片槽位是**覆盖写**的 —— SinkWriter 打开已存在的 MP4 会
// 追加（或者直接失败），行为不好预测。上一轮这个槽位里有 6.4MB、这一轮只
// 写了 2MB，文件大小就还是 6.4MB，磁盘统计凭空翻几倍（ffmpeg 时代实测过：
// 120s 窗口显示 356MB，按码率算只该 120MB）。
//
// index.csv 也一起删：清单是跨轮次累积时间轴的，不删的话用它算"录了多久"
// 就完全不对了。
//
// ---- 为什么只清"本轮的目录"而不是整个 segments/ ----
// 用户改帧率后希望前面的录像也一起导出（用插帧/抽帧补到新帧率），所以上一轮
// 的分片不能再删。每一轮都有自己的子目录（见 .h 里的目录结构），新一轮只往
// 自己的空目录里写，天然不会撞到旧的槽位。
//
// 旧轮次由下面 Start() 里的回收逻辑按回放窗口清理（超窗的整目录删掉），
// 所以磁盘占用仍然被"回放时长"限住，不会无限涨。
void SegmentRecorder::ClearStaleSlots() {
    std::error_code ec;
    const fs::path runPath = fs::u8path(runDir_);
    if (fs::exists(runPath, ec)) {
        for (const auto& e : fs::directory_iterator(runPath, ec)) {
            std::error_code ec2;
            if (!e.is_regular_file(ec2)) continue;
            const std::string ext = e.path().extension().u8string();
            // 老版本留的是 .ts（ffmpeg），新的是 .mp4（MF），两种都清。
            if (ext == u8".mp4" || ext == u8".ts") fs::remove(e.path(), ec2);
        }
    }
    if (!listFile_.empty()) {
        std::error_code ec3;
        fs::remove(fs::u8path(listFile_), ec3);
    }
}

bool SegmentRecorder::Start() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (started_) return stats_.active;
        started_ = true;
    }

    if (dir_.empty() || width_ == 0 || height_ == 0) {
        std::lock_guard<std::mutex> lk(mu_);
        stats_.failed = true;
        stats_.detail = "录制参数不全（目录或分辨率是空的）";
        return false;
    }

    // H.264 的宏块是 16x16，宽高必须是偶数。采集端给奇数很常见
    // （WGC 抓的是窗口真实客户区，1545x773 这种），ffmpeg 时代软编会直接
    // 拒绝开工，现在在编码器这一层向下取偶兜住 —— 只丢最多 1 行/1 列像素。
    if (width_ & 1u) --width_;
    if (height_ & 1u) --height_;

    std::error_code ec;
    fs::create_directories(fs::u8path(segmentsDir_), ec);
    if (ec) {
        std::lock_guard<std::mutex> lk(mu_);
        stats_.failed = true;
        stats_.detail = "建不了分片目录：" + ec.message();
        return false;
    }

    // ---- 本轮运行目录 ----------------------------------------------------
    //
    // 每一轮录制（每起一次编码器）单独一个子目录，名字就是**代次数字**：
    // `1`、`2`、`3`…（老目录叫 `0001-fps60`，那是历史，仍然能认出来）。
    //
    // 为什么要分代：新一轮的槽位循环只在**自己的**目录里绕圈，不会覆盖
    // 上一轮的分片 —— 这是"改帧率后前面的录像还能一起导出"的前提。
    //
    // 【为什么不把帧率写进目录名了】以前叫 `0001-fps60`，看着像"第 1 代、
    // 60 帧"，可它只解决了导出端少跑一次探测的便利，代价是名字一长串、
    // 同一台机器上换个帧率就多一个目录。真正决定输出帧率的是**配置里的
    // 目标帧率**，导出时本来就要处理重采样；源帧率在 run.json 里，所以
    // 名字里的 fps 其实没必要。
    //
    // 代次取现有目录里最大的 +1（找不到就用 1）。
    {
        uint32_t nextSeq = 1;
        std::error_code ec2;
        if (fs::exists(fs::u8path(segmentsDir_), ec2)) {
            for (const auto& e : fs::directory_iterator(fs::u8path(segmentsDir_), ec2)) {
                std::error_code ec3;
                if (!e.is_directory(ec3)) continue;
                const std::string nm = e.path().filename().u8string();
                const size_t dash = nm.find('-');
                const std::string num = dash == std::string::npos ? nm : nm.substr(0, dash);
                if (num.empty()) continue;
                bool allDigit = true;
                for (char c : num) {
                    if (!std::isdigit(static_cast<unsigned char>(c))) {
                        allDigit = false;
                        break;
                    }
                }
                if (!allDigit) continue;
                const uint32_t v = static_cast<uint32_t>(std::strtoul(num.c_str(), nullptr, 10));
                if (v >= nextSeq) nextSeq = v + 1;
            }
        }
        char name[32];
        std::snprintf(name, sizeof(name), "%u", nextSeq);
        runDir_ = PathToUtf8(fs::u8path(segmentsDir_) / fs::u8path(name));
        listFile_ = PathToUtf8(fs::u8path(runDir_) / fs::u8path("index.csv"));
    }

    fs::create_directories(fs::u8path(runDir_), ec);
    if (ec) {
        std::lock_guard<std::mutex> lk(mu_);
        stats_.failed = true;
        stats_.detail = "建不了本轮分片目录：" + ec.message();
        return false;
    }

    const uint32_t segmentSeconds = video_.segmentSeconds ? video_.segmentSeconds : 2u;
    // 保留几个分片 = ceil(回放时长 / 分片时长)，至少 2 个（1 个没法滚动）。
    const uint32_t replaySeconds = video_.replaySeconds ? video_.replaySeconds : 60u;
    uint32_t keep = (replaySeconds + segmentSeconds - 1) / segmentSeconds;
    if (keep < 2) keep = 2;

    // 把本轮的参数记一份，导出端不用再猜。
    // （帧率、尺寸都写在这里，目录名只留代次数字。）
    //
    // 【注意顺序】真正落盘在下面「起编码器」之后 —— 因为 run.json 里的 fps
    // 必须写编码器**实际**跑的那个。硬件编码器对「分辨率 × 帧率」有吞吐上限，
    // 超了 MfEncoder 会自动降档（见 MfEncoder.h 的 EffectiveFps()）。用户配
    // 240 帧、实际录成 165 帧时，如果这里写 240，导出端会按 240 帧铺时间轴
    // → 20 秒的分片只铺出 13.7 秒，剩下 6 秒补 NO SIGNAL。

    ClearStaleSlots();

    // 回收超窗的旧轮次目录（本轮刚建的空目录不动）。
    //
    // 保留最近 N 轮，N = max(2, keep / 4)。keep 是环形槽位数（回放时长/单片时长），
    // 每轮最多也只有 keep 片，所以留 keep/4 轮已经能覆盖"改了几次帧率、窗口内
    // 的旧画面还在"。太老的整目录删掉，磁盘不会无限涨。
    {
        std::error_code ec4;
        std::vector<std::pair<int64_t, fs::path>> runs;
        for (const auto& e : fs::directory_iterator(fs::u8path(segmentsDir_), ec4)) {
            std::error_code ec5;
            if (!e.is_directory(ec5)) continue;
            if (e.path() == fs::u8path(runDir_)) continue;
            const auto t = e.last_write_time(ec5);
            runs.emplace_back(ec5 ? 0 : static_cast<int64_t>(t.time_since_epoch().count()),
                              e.path());
        }
        std::sort(runs.begin(), runs.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        const size_t keepRuns = std::max<size_t>(2, static_cast<size_t>(keep) / 4);
        for (size_t i = keepRuns; i < runs.size(); ++i) {
            std::error_code ec8;
            fs::remove_all(runs[i].second, ec8);
        }
    }

    // ---- 起编码器 --------------------------------------------------------
    //
    // 一个 MfEncoder = 一个进程内 SinkWriter，没有子进程、没有 ffmpeg.exe。
    // 自己把 BGRA 转成 NV12 再喂（见 MfEncoder.h 里 ConvertToNv12 的注释）。
    encoder_ = std::make_unique<MfEncoder>();
    if (!encoder_->Start(runDir_, width_, height_, fps_,
                         video_.bitrateMbps ? video_.bitrateMbps : 20u, segmentSeconds, keep)) {
        const MfEncoderStatus es = encoder_->Status();
        std::lock_guard<std::mutex> lk(mu_);
        stats_.failed = true;
        stats_.detail = es.detail.empty() ? "编码器起不来" : es.detail;
        return false;
    }

    // 【run.json 必须写编码器实际用的帧率】见上面那段注释。MfEncoder 可能
    // 因为硬件吞吐上限把帧率降下来（EffectiveFps()），导出端按它铺时间轴。
    const uint32_t actualFps = encoder_->EffectiveFps();
    {
        std::ofstream man(PathToUtf8(fs::u8path(runDir_) / fs::u8path("run.json")),
                          std::ios::binary | std::ios::trunc);
        if (man) {
            man << "{\n"
                << "  \"generation\": "
                << std::strtoul(fs::path(runDir_).filename().u8string().c_str(), nullptr, 10)
                << ",\n"
                << "  \"fps\": " << actualFps << ",\n"
                << "  \"requestedFps\": " << fps_ << ",\n"
                << "  \"width\": " << width_ << ",\n"
                << "  \"height\": " << height_ << ",\n"
                << "  \"segmentSeconds\": " << segmentSeconds << ",\n"
                << "  \"container\": \"fmp4\"\n"
                << "}\n";
        }
    }

    std::lock_guard<std::mutex> lk(mu_);
    stats_.active = true;
    stats_.failed = false;
    // 【这里必须沿用编码器的 detail，不能无条件写「录制中」】
    // MfEncoder::StartWithFpsFallback 降过档时会在 detail 里写
    // 「录制中（硬件编码器最高接受 N 帧，已从配置的 M 帧降下来）」，
    // 那是用户唯一能知道"我配的 240 其实录成了 165"的地方。以前这里
    // 无条件覆盖成「录制中」，降档说明在界面上一闪就没了（实测
    // scripts/tmp-fpsfallback.mjs 读到 detail="录制中" 而 run.json 里是
    // fps=165/requestedFps=240）。
    {
        const MfEncoderStatus es = encoder_->Status();
        stats_.detail = es.detail.empty() ? "录制中" : es.detail;
    }
    stats_.segmentSeconds = segmentSeconds;
    stats_.keepSegments = keep;
    StartFeeder();
    return true;
}

/** 起喂帧线程。必须在 Start() 成功之后调（编码器已就绪）。 */
void SegmentRecorder::StartFeeder() {
    {
        std::lock_guard<std::mutex> lk(slotMu_);
        feedStop_ = false;
        slotFull_ = false;
        slotSeq_ = 0;
        slotTakenSeq_ = 0;
    }
    feeder_ = std::thread([this] { FeederLoop(); });
}

bool SegmentRecorder::Feed(const void* pixels, uint32_t stride, uint32_t width, uint32_t height) {
    // 【尺寸不再参与"收不收这一帧"的判断】见 DESIGN.md 9.5。
    //
    // 以前这里是 `if (!pixels || width != width_ || height != height_) 丢弃`，
    // 于是窗口一换尺寸，上层就只能重建整个录制器 —— 那会分配新的轮次目录、
    // 切片从 000000 重来，回放里前面的画面就没了（而且服务里其他路也受牵连）。
    // 现在尺寸跟着帧一起进槽，编码器自己切分片。
    if (!pixels || width == 0 || height == 0) {
        std::lock_guard<std::mutex> lk(mu_);
        ++stats_.droppedFrames;
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!stats_.active) return false;
    }

    const uint8_t* p = static_cast<const uint8_t*>(pixels);
    const size_t rowBytes = static_cast<size_t>(width) * 4u;
    const size_t need = rowBytes * height;

    // 拷进槽里就得返回。这一步是纯内存拷贝（一次 4.78MB 的那种），
    // 不会阻塞在编码器上 —— 那才是原来把采集节拍拖垮的东西。
    {
        std::lock_guard<std::mutex> lk(slotMu_);
        slot_.resize(need);
        // 行距和紧凑行距不一致（DXGI 的 staging 纹理会对齐到 256 字节）：
        // 一行一行拷，别把行尾的填充字节也当成像素（那样画面会斜）。
        if (stride == rowBytes) {
            std::memcpy(slot_.data(), p, need);
        } else {
            for (uint32_t y = 0; y < height; ++y) {
                std::memcpy(slot_.data() + static_cast<size_t>(y) * rowBytes,
                            p + static_cast<size_t>(y) * stride, rowBytes);
            }
        }
        slotW_ = width;
        slotH_ = height;
        slotFull_ = true;
        ++slotSeq_;
    }
    slotCv_.notify_one();
    return true;
}

void SegmentRecorder::FeederLoop() {
    const double fps = fps_ > 0 ? static_cast<double>(fps_) : 60.0;

    // 本地复用的帧缓冲：取出槽里的帧之后立刻放开槽锁，编码时不再占着它。
    std::vector<uint8_t> frame;
    uint64_t lastSeq = 0;
    // 槽里那一帧的尺寸（会随窗口缩放变化，见 Feed 的注释）。
    uint32_t frameW = width_;
    uint32_t frameH = height_;

    // ---- 时间轴为什么按"帧数"而不是按"节拍"维护 --------------------------
    //
    // 编码器的时间轴由**收到多少帧**决定：第 N 帧的 PTS 就是 N / fps。
    // 所以帧数必须严格等于 fps × 真实时长，否则 PTS 被压缩/拉伸，回放就
    // 加速或变慢。
    //
    // 原来的写法是"落后超过一个 interval 就把 nextDue 重新对齐到 now"，那一段
    // 该发的帧被**静默丢掉**了。240fps 每拍预算只有 4.17ms，GPU 一忙、编码器
    // 一堵就成片落后，于是 240fps 录出来的回放明显加速，而 60fps 因为留有
    // 三倍余量不容易触发 —— 正是"只测出 240 有问题"的原因。
    //
    // 现在改成：每拍结束前，按墙钟算出"到现在总共应该写多少帧"，补齐差额
    // （补的内容就是上一帧，等价于重复插帧）。时间轴由帧数保证，与采集掉帧、
    // 编码器堵塞完全解耦 —— 采不到新画面只会让画面变"卡"，绝不会让回放变速。
    uint64_t framesWritten = 0;
    auto t0 = std::chrono::steady_clock::now();

    auto writeOnce = [&](const std::vector<uint8_t>& buf) -> bool {
        if (buf.empty()) return true; // 还没采到第一帧，没法补
        if (!EncodeFrame(buf)) return false;
        ++framesWritten;
        return true;
    };

    bool alive = true;
    while (alive) {
        // 还没采到第一帧：帧数是 0 时没有任何东西可写，等第一帧到了再开始
        // 计时。否则 t0 会在"空转"期间把欠账攒成天文数字。
        if (frame.empty()) {
            std::unique_lock<std::mutex> lk(slotMu_);
            if (feedStop_) break;
            slotCv_.wait(lk, [this] { return feedStop_ || slotFull_; });
            if (feedStop_) break;
            if (slotFull_ && slotSeq_ != lastSeq) {
                frame.swap(slot_);
                lastSeq = slotSeq_;
                slotFull_ = false;
                frameW = slotW_;
                frameH = slotH_;
                if (frameW > 0 && frameH > 0) {
                    width_ = frameW;
                    height_ = frameH;
                }
            }
            t0 = std::chrono::steady_clock::now();
            framesWritten = 0;
            continue;
        }

        // ---- 睡到下一个节拍 ----------------------------------------------
        const auto nextDue =
            t0 + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                     std::chrono::duration<double>(static_cast<double>(framesWritten) / fps));
        if (std::chrono::steady_clock::now() < nextDue) {
            // 条件变量带超时：Stop() 置 feedStop_ 时能立刻醒。
            std::unique_lock<std::mutex> lk(slotMu_);
            if (feedStop_) break;
            slotCv_.wait_until(lk, nextDue, [this] { return feedStop_ || slotFull_; });
            if (feedStop_) break;
        }

        // ---- 取槽里的最新帧 ----------------------------------------------
        bool haveNew = false;
        {
            std::lock_guard<std::mutex> lk(slotMu_);
            if (feedStop_) break;
            if (slotFull_ && slotSeq_ != lastSeq) {
                frame.swap(slot_); // 交换而不是拷贝：连一次 memcpy 都省掉
                lastSeq = slotSeq_;
                slotFull_ = false;
                haveNew = true;
                // 尺寸跟着帧走：槽里这一帧有多大，这拍就按多大编码。
                frameW = slotW_;
                frameH = slotH_;
            }
        }
        // 换尺寸时把本地缓冲整理成紧凑排布（swap 之后行距就是新尺寸的）。
        const bool sizeChanged = (frameW != width_ || frameH != height_);
        if (sizeChanged && frameW > 0 && frameH > 0) {
            width_ = frameW;
            height_ = frameH;
        }

        // ---- 补到"现在该有的帧数" ------------------------------------------
        //
        // 这一步同时覆盖两种情况：
        //   * 正常节奏：due 比已写的多 1，就是正常写一帧；
        //   * 落后了：due 比已写的多 k 张，连续写 k 张（内容是同一帧），
        //     把欠账一次补齐，时间轴立刻回到真实时间。
        const double elapsedSec =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const uint64_t due = static_cast<uint64_t>(elapsedSec * fps) + 1;

        // ---- 为什么这里**绝对不能**给补帧设上限 ----------------------------
        //
        // 原来这里是 `if (due > framesWritten + 240) { 重起基准 }`：机器被彻底
        // 抢占、或者编码器堵住时，把这一段该有的时长直接**丢掉**，只写一帧。
        // 后果就是主人看到的那种情况 —— 画面卡但时间轴被压短，回放成了快进。
        // 帧率越高、机器越忙，这种缺口越多，240fps 尤其明显。
        //
        // 现在的取舍（用户明确要求）：**允许卡，不允许变速**。落后多少就补多少
        // 重复帧，重复帧编同一张画面代价极低，内存占用不随缺口大小增长 ——
        // 补帧用的始终是本地那一帧缓冲，不会额外分配。
        //
        // 唯一的例外是"从没采到过任何帧"（frame 为空），那种情况下面补不了，
        // 只能等，等到了重新计时。
        const uint64_t need = due > framesWritten ? (due - framesWritten) : 0;
        for (uint64_t i = 0; i < need; ++i) {
            if (!writeOnce(frame)) {
                alive = false;
                break;
            }
        }
        (void)haveNew;

        // ---- 有人要求"立刻切片"（导出前）---------------------------------
        //
        // 放在补帧**之后**：这一刻该写的帧都写完了，切出来的分片右端正好是
        // "现在"，导出的时间轴右端才等于用户按下按钮的时刻。
        // 放在取槽**之后**：刚取到的新画面已经进 `frame`，切完片新片第一帧
        // 就是最新画面，不会丢内容。
        if (alive) {
            uint64_t want = 0;
            {
                std::lock_guard<std::mutex> lk(flushMu_);
                want = flushWant_;
            }
            if (want != flushDone_) {
                const bool ok = encoder_ && encoder_->FlushSegment();
                {
                    std::lock_guard<std::mutex> lk(flushMu_);
                    flushDone_ = want; // 失败也算"处理过了"，否则命令线程会一直等
                    flushOk_ = ok;
                }
                flushCv_.notify_all();
            }
        }
    }

    if (!alive) {
        std::lock_guard<std::mutex> lk(mu_);
        stats_.active = false;
        stats_.failed = true;
        if (stats_.detail == "录制中" || stats_.detail.empty()) {
            const MfEncoderStatus es = encoder_ ? encoder_->Status() : MfEncoderStatus{};
            stats_.detail = es.detail.empty() ? "编码器提前退出（分片写入中断）" : es.detail;
        }
    }
}

bool SegmentRecorder::EncodeFrame(const std::vector<uint8_t>& frame) {
    if (!encoder_) return false;
    const size_t rowBytes = static_cast<size_t>(width_) * 4u;
    if (frame.size() < rowBytes * height_) return true; // 不完整，跳过这一拍

    // 槽里已经是紧凑排布（stride = rowBytes），直接交给编码器。
    if (!encoder_->Write(frame.data(), static_cast<uint32_t>(rowBytes), width_, height_)) {
        return false;
    }

    std::lock_guard<std::mutex> lk(mu_);
    ++stats_.framesIn;
    stats_.bytesIn += static_cast<uint64_t>(rowBytes) * height_;
    return true;
}

bool SegmentRecorder::FlushSegment(uint32_t timeoutMs) {
    // 喂帧线程没起来（Start 失败 / 已经 Stop）就没有"正在写的那一片"。
    if (!feeder_.joinable()) return false;
    if (!encoder_ || !encoder_->Running()) return false;

    uint64_t want = 0;
    {
        std::lock_guard<std::mutex> lk(flushMu_);
        want = ++flushWant_; // 递增的请求号：连续点两次导出不会互相吞掉
        flushOk_ = false;
    }
    slotCv_.notify_all(); // 让喂帧线程从 wait 里醒过来，别等满一个节拍

    std::unique_lock<std::mutex> lk(flushMu_);
    const bool done = flushCv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                                        [&] { return flushDone_ >= want; });
    if (!done) return false;
    return flushOk_;
}

void SegmentRecorder::Stop() {    // 先收喂帧线程：它可能正卡在 WriteSample 上等编码器。
    // 必须等它退出来再 Finish 编码器，否则它可能往已经关掉的 writer 里写。
    if (feeder_.joinable()) {
        {
            std::lock_guard<std::mutex> lk(slotMu_);
            feedStop_ = true;
        }
        slotCv_.notify_all();
        feeder_.join();
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        if (stopped_) return;
        stopped_ = true;
        stats_.active = false;
        if (stats_.detail == "录制中") stats_.detail = "已停止";
    }

    // Finish() 会把当前分片的索引写完（等价于 ffmpeg 读到 EOF 后 flush）。
    // 没有超时问题 —— 全是进程内调用，不依赖别的进程。
    if (encoder_) {
        encoder_->Finish();
        const MfEncoderStatus es = encoder_->Status();
        if (es.failed) {
            std::lock_guard<std::mutex> lk(mu_);
            stats_.failed = true;
            if (!es.detail.empty()) stats_.detail = es.detail;
        }
    }
}

void SegmentRecorder::RefreshDiskStatsLocked() const {
    std::error_code ec;
    uint32_t count = 0;
    uint64_t bytes = 0;
    // 分片现在分布在各轮次的子目录里，磁盘统计要连子目录一起扫 ——
    // 只扫一层的话，界面上永远显示"0 个分片 / 0 MB"。
    if (!segmentsDir_.empty()) {
        const fs::path root = fs::u8path(segmentsDir_);
        std::error_code ecDir;
        if (fs::exists(root, ecDir)) {
            for (const auto& e : fs::recursive_directory_iterator(root, ecDir)) {
                if (ecDir) break;
                std::error_code ecFile;
                if (!e.is_regular_file(ecFile) || ecFile) continue;
                const std::string ext = e.path().extension().u8string();
                // 新分片是 .mp4（MF），老录像还留着 .ts（ffmpeg 时代）——
                // 两种都算进磁盘占用，不然升级后旧录像"消失了"。
                if (ext != ".mp4" && ext != ".ts") continue;
                ++count;
                bytes += static_cast<uint64_t>(e.file_size(ecFile));
            }
        }
    }
    stats_.segmentsOnDisk = count;
    stats_.diskBytes = bytes;
}

RecorderStats SegmentRecorder::Stats() const {
    std::lock_guard<std::mutex> lk(mu_);
    RefreshDiskStatsLocked();
    return stats_;
}

} // namespace shadow
