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
    // 帧率是可热改的（见 SetFps），所以存进 atomic 成员。
    {
        uint32_t f = video_.fps ? video_.fps : 60u;
        // 硬上限 120fps（见 MfEncoder.h 的 kMaxFps）：离谱值兜一下，别让 GOP 算爆
        if (f > MfEncoder::kMaxFps) f = MfEncoder::kMaxFps;
        fps_.store(f, std::memory_order_relaxed);
    }
}

SegmentRecorder::~SegmentRecorder() { Stop(); }

std::string SegmentRecorder::ListFilePath() const { return listFile_; }
std::string SegmentRecorder::SegmentsDir() const { return segmentsDir_; }

bool SegmentRecorder::Running() const {
    std::lock_guard<std::mutex> lk(mu_);
    return stats_.active;
}

uint32_t SegmentRecorder::CurrentFps() const {
    // encoder_ 的指针在整个运行期不会换（换帧率是就地改，不重建编码器），
    // EffectiveFps() 内部自己带锁，所以这里不用再拿 mu_。
    if (!encoder_ || !encoder_->Running()) return 0;
    return encoder_->EffectiveFps();
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
    if (!encoder_->Start(runDir_, width_, height_, fps_.load(std::memory_order_relaxed),
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

    // 【喂帧节拍必须立刻切到编码器实际能接受的帧率】
    //
    // 用户配 240 帧、NVENC 只接受 165 帧时，MfEncoder 会自己降档到 165
    // （StartWithFpsFallback），它按 165 打时间戳、每 165*segmentSeconds 帧
    // 切一片。但 fps_ 原来是构造函数里存进去的**配置值 240**，喂帧线程于是
    // 按 240 的节奏灌帧 —— 每片 330 帧只用 330/240 = 1.375 秒墙钟就灌满了，
    // 编码器却按 165 打成 2 秒内容。结果：
    //
    //   * 编码器时间轴（帧数/165）比真实墙钟快 1.45 倍；
    //   * index.csv 的 nextSegStartUnix_ 也按 frames/165 顺延，同样快 1.45 倍；
    //   * 导出端按 index.csv 排出来的 20 秒，实际只覆盖 13.8 秒真实画面 ——
    //     画面被慢放，看起来就是「录 240 结果比 5fps 还卡」。
    //
    // 实测（%TEMP%\shadow-play\freeze-f240，配 240 帧、实际 165 帧）：14 秒
    // 墙钟落了 12 片（每片 330 帧 = 24 秒时间轴），而 recordFrames 只有 1979
    // （=141fps）。修法就是把 fps_ 设成 EffectiveFps()，让喂帧线程和编码器
    // 用同一个节拍。SetFps() 那条热改路径本来就已经这么做了（见下面
    // `fps_.store(encoder_->EffectiveFps())`），只有 Start() 漏了。
    fps_.store(actualFps ? actualFps : 60u, std::memory_order_relaxed);

    {
        std::ofstream man(PathToUtf8(fs::u8path(runDir_) / fs::u8path("run.json")),
                          std::ios::binary | std::ios::trunc);
        if (man) {
            man << "{\n"
                << "  \"generation\": "
                << std::strtoul(fs::path(runDir_).filename().u8string().c_str(), nullptr, 10)
                << ",\n"
                << "  \"fps\": " << actualFps << ",\n"
                << "  \"requestedFps\": " << encoder_->RequestedFps() << ",\n"
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
        // 队列深度按第一帧的尺寸现算（见 Feed），这里先清空。
        ringDepth_ = 0;
        ringWrite_ = 0;
        ringRead_ = 0;
        ringW_ = 0;
        ringH_ = 0;
        ring_.clear();
        slotOverwritten_.store(0, std::memory_order_relaxed);
        slotFed_.store(0, std::memory_order_relaxed);
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

    // 【拷贝在锁外做】原来整段 memcpy 都在 slotMu_ 里面（1440p 是 14.7MB，
    // 实测约 1.5ms），而喂帧线程取帧要的正是这把锁 —— 等于每次投帧都把
    // 配帧线程堵住 1.5ms。165fps 每帧预算只有 6.06ms，这一堵就吃掉四分之一，
    // 于是喂帧线程总是"晚到"、槽里永远是上一拍那帧，只能补重复帧（实测
    // repeatFrames 占一半、真实画面只剩 50%）。
    //
    // Feed() 只有一个生产者（Session 的采集线程按路各一个），所以 writeBuf_
    // 不需要保护：先拷进 writeBuf_，再在锁里 O(1) 换进 slot_。锁只护住那个
    // swap，喂帧线程几乎感觉不到我们在投帧。
    writeBuf_.resize(need);
    if (stride == rowBytes) {
        std::memcpy(writeBuf_.data(), p, need);
    } else {
        // 行距和紧凑行距不一致（DXGI 的 staging 纹理会对齐到 256 字节）：
        // 一行一行拷，别把行尾的填充字节也当成像素（那样画面会斜）。
        for (uint32_t y = 0; y < height; ++y) {
            std::memcpy(writeBuf_.data() + static_cast<size_t>(y) * rowBytes,
                        p + static_cast<size_t>(y) * stride, rowBytes);
        }
    }

    {
        std::lock_guard<std::mutex> lk(slotMu_);

        // 尺寸变了要按新的一帧重算队列深度（窗口拖大拖小都会走到这里）。
        if (ringDepth_ == 0 || width != ringW_ || height != ringH_ || ring_.size() != ringDepth_) {
            const size_t frameBytes = static_cast<size_t>(rowBytes) * height;
            size_t depth = frameBytes ? (kPendingBytes / frameBytes) : 0;
            // 再按"时间"夹一次：至少盖住一次切片轮转（见 .h 里的说明）。
            const double f = static_cast<double>(fps_.load(std::memory_order_relaxed));
            const size_t byTime = static_cast<size_t>((f > 0.0 ? f : 60.0) * kPendingSeconds + 0.5);
            if (byTime < depth) depth = byTime;
            if (depth < 4) depth = 4;      // 至少要能盖住"一两帧"的抖动
            if (depth > 40) depth = 40;    // 再深就是白吃内存（滞后也变明显）
            ringDepth_ = static_cast<uint32_t>(depth);
            ring_.clear();
            ring_.resize(ringDepth_);
            // 尺寸换代的瞬间队列里的旧帧尺寸已经对不上，直接作废。
            ringWrite_ = 0;
            ringRead_ = 0;
            ringW_ = width;
            ringH_ = height;
        }

        // 队列满了：顶掉最旧的一帧。这才是真丢内容，要计量。
        if (ringWrite_ - ringRead_ >= ringDepth_) {
            ++ringRead_;
            slotOverwritten_.fetch_add(1, std::memory_order_relaxed);
        }

        // O(1) 交换：把刚拷好的缓冲换进队尾那格，那格的旧缓冲回到 writeBuf_
        // （下次 resize/写会被覆盖）。锁只护住这个 swap。
        PendingFrame& p = ring_[static_cast<size_t>(ringWrite_ % ringDepth_)];
        p.buf.swap(writeBuf_);
        p.w = width;
        p.h = height;
        ++ringWrite_;
        slotFed_.fetch_add(1, std::memory_order_relaxed);
    }
    slotCv_.notify_one();
    return true;
}

void SegmentRecorder::FeederLoop() {
    // 帧率每一拍都重新读（见 SetFps）：它可以在录制过程中被改，
    // 循环外固定成 const 会让新帧率永远不生效。
    auto currentFps = [this]() {
        const uint32_t f = fps_.load(std::memory_order_relaxed);
        return f > 0 ? static_cast<double>(f) : 60.0;
    };
    double fps = currentFps();

    // 本地复用的帧缓冲：取出槽里的帧之后立刻放开槽锁，编码时不再占着它。
    std::vector<uint8_t> frame;
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

    // 【吞吐看门狗已经删掉了】这里原来有一套"喂不满就自动降一档（120→90→60）"
    // 的机制：重复帧占比连续两窗超过 40% 就 `encoder_->SetFps()` 降档，而且
    // **只降不升**。主人的判断是：降档该由他自己决定，不该由程序偷偷改 ——
    // 配置写 120 就按 120 录，跑不动那是可见的事实（状态里 repeatFrames 会
    // 涨、画面会卡），比"悄悄降到 60 还不告诉用户"强。
    // 留下的影响：录制帧率始终等于配置帧率（除非硬件**拒收**这个媒体类型，
    // 那种降档在 MfEncoder::StartWithFpsFallback 里，必须留着）。

    // 写一帧**新画面**（槽里取出来的那份像素）。
    auto writeOnce = [&](const std::vector<uint8_t>& buf) -> bool {
        if (buf.empty()) return true; // 还没采到第一帧，没法补
        if (!EncodeFrame(buf)) return false;
        ++framesWritten;
        return true;
    };
    // 补一帧**重复画面**：让编码器复用样本缓冲，不碰 BGRA。
    //
    // 与 writeOnce 分开是为了不再把同一份 BGRA 转一遍 —— 见 EncodeRepeat()。
    // 空槽的判据不同：writeOnce 看 buf 空不空，这里看"编码器里有没有可复用的
    // 上一帧"（刚开片/刚换尺寸时没有，那种情况直接空过这一拍）。
    auto writeRepeat = [&]() -> bool {
        if (frame.empty()) return true;
        if (!EncodeRepeat()) return false;
        ++framesWritten;
        return true;
    };

    // ---- 取帧的三个助手（都在 slotMu_ 里挪指针，O(1)）------------------
    //
    // 队列是"采集线程写、喂帧线程读"的单生产者单消费者环。取帧只挪读指针，
    // 缓冲用 swap 换成自己的，不做 memcpy。
    auto queued = [&]() {
        std::lock_guard<std::mutex> lk(slotMu_);
        return ringWrite_ - ringRead_;
    };
    // 取最旧的一帧（落后补帧时用：按顺序补上，画面才连续）。
    auto popFront = [&]() -> bool {
        std::lock_guard<std::mutex> lk(slotMu_);
        if (ringRead_ >= ringWrite_ || ringDepth_ == 0) return false;
        PendingFrame& p = ring_[static_cast<size_t>(ringRead_ % ringDepth_)];
        frame.swap(p.buf);
        frameW = p.w;
        frameH = p.h;
        ++ringRead_;
        return true;
    };
    // 取最新的一帧（正常节奏用）：中间那些跳过，计进 captureDropped。
    auto popNewest = [&]() -> bool {
        std::lock_guard<std::mutex> lk(slotMu_);
        if (ringRead_ >= ringWrite_ || ringDepth_ == 0) return false;
        const uint64_t skip = (ringWrite_ - ringRead_) - 1;
        if (skip > 0) {
            ringRead_ += skip;
            slotOverwritten_.fetch_add(skip, std::memory_order_relaxed);
        }
        PendingFrame& p = ring_[static_cast<size_t>(ringRead_ % ringDepth_)];
        frame.swap(p.buf);
        frameW = p.w;
        frameH = p.h;
        ++ringRead_;
        return true;
    };

    bool alive = true;
    while (alive) {
        // 还没采到第一帧：帧数是 0 时没有任何东西可写，等第一帧到了再开始
        // 计时。否则 t0 会在"空转"期间把欠账攒成天文数字。
        if (frame.empty() && queued() == 0) {
            std::unique_lock<std::mutex> lk(slotMu_);
            if (feedStop_) break;
            slotCv_.wait(lk, [this] { return feedStop_ || ringRead_ < ringWrite_; });
            if (feedStop_) break;
            if (frame.empty() && ringRead_ < ringWrite_ && ringDepth_ > 0) {
                PendingFrame& p = ring_[static_cast<size_t>(ringRead_ % ringDepth_)];
                frame.swap(p.buf);
                frameW = p.w;
                frameH = p.h;
                ++ringRead_;
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
            //
            // 【谓词里**不能**再放 slotFull_】原来写的是 `feedStop_ || slotFull_`，
            // 后果很严重：采集端每投一帧就把喂帧线程叫醒一次，而醒来时往往还没到
            // 节拍点（need 算出来是 0）。那种"提前醒"会走到下面去取槽 —— 取出
            // 一整帧、算完 need=0、什么也不写，**这一帧就凭空没了**；紧接着下一次
            // 真到节拍时槽是空的，只能补重复帧。实测 60fps 下 recordFrames=1089、
            // repeatFrames=1072（98.2% 全是重复帧），而 captureFrames=1090 说明
            // 采集端一帧没少给 —— 帧是在这里被自己扔掉的，不是采集不到。
            //
            // 现在只等 feedStop_：节拍由 nextDue 决定，采集端投帧不再参与唤醒，
            // 槽里自然会一直躺着"最新那一帧"，到点再取。
            std::unique_lock<std::mutex> lk(slotMu_);
            if (feedStop_) break;
            slotCv_.wait_until(lk, nextDue, [this] { return feedStop_; });
            if (feedStop_) break;
        }

        // ---- 先算"现在一共该写多少帧"，再决定要不要从槽里取 ---------------
        //
        // 【顺序不能反】这两步以前是"先取槽、后算 need"。一旦算出 need=0，
        // 刚取出来的那一帧已经被 swap 走了，只能丢弃 —— 这就是上面那段注释里
        // 98% 重复帧的来源。先算 need，need==0 时**根本不动槽**，帧留在那里等
        // 下一个节拍。
        const double elapsedSec =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const uint64_t due = static_cast<uint64_t>(elapsedSec * fps) + 1;
        const uint64_t need = due > framesWritten ? (due - framesWritten) : 0;
        if (need == 0) continue; // 还没到点，槽里的帧留着下一步用

        // ---- 取这一拍要用的真实帧 -----------------------------------------
        //
        // 【两种取法，取决于落后多少】
        //   need <= 1：正常节奏 —— 只取**最新**那一帧，队列里更早的跳过（算丢帧）。
        //              画面必须跟得上实时，攒着的旧帧没有价值。
        //   need > 1 ：被堵过（切片轮转 150~250ms、编码器抖动）—— 队列里躺的
        //              正是这段时间真实发生过的画面，按顺序取出来写，画面就是
        //              连续的。这就是"每两秒顿一下"的解法：补真画面，不是补重复。
        bool haveNew = false;
        uint64_t takeReal = 0;
        {
            // 【节拍到了但队列还没新帧时，给它半帧时间】采集和编码同速却异相时，
            // 一半节拍会白白补重复帧。最多等半个帧周期（165fps 是 3.0ms）。
            // wait_for 期间会释放 slotMu_，采集线程照常入队并叫醒我们。
            {
                std::unique_lock<std::mutex> lk(slotMu_);
                if (feedStop_) break;
                if (ringRead_ == ringWrite_) {
                    const auto budget =
                        std::chrono::microseconds(static_cast<long long>(500000.0 / fps));
                    const uint64_t want = ringWrite_;
                    slotCv_.wait_for(lk, budget,
                                     [this, want] { return feedStop_ || ringWrite_ != want; });
                    if (feedStop_) break;
                }
            }

            if (need <= 1) {
                haveNew = popNewest();
                takeReal = haveNew ? 1 : 0;
            } else {
                const uint64_t avail = queued();
                if (avail > 0) {
                    takeReal = avail < need ? avail : need;
                    haveNew = popFront(); // 第一张先取出来（循环里 i>0 再取）
                    if (!haveNew) takeReal = 0;
                }
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
        //   * 落后了：due 比已写的多 k 张，把队列里这段时间的真实画面**按顺序**
        //     写 k 张（不够的才补重复），时间轴回到真实时间，画面还是连续的。
        //
        // ---- 为什么这里**绝对不能**给补帧设上限 ----------------------------
        //
        // 原来这里是 `if (due > framesWritten + 240) { 重起基准 }`：机器被彻底
        // 抢占、或者编码器堵住时，把这一段该有的时长直接**丢掉**，只写一帧。
        // 后果就是主人看到的那种情况 —— 画面卡但时间轴被压短，回放成了快进。
        // 帧率越高、机器越忙，这种缺口越多，240fps 尤其明显。
        //
        // 现在的取舍（用户明确要求）：**允许卡，不允许变速**。落后多少就补多少。
        //
        // 【2026-10-21 改：补的是真画面，不是重复帧】以前这里只写"一张新画面 +
        // N 张重复"，于是切片轮转堵 200ms 就定格 200ms —— 用户原话是
        // 「两秒顿一次」。现在队列里存着堵住期间采到的真实帧，按顺序补进去。
        // 只有队列空了（刚开录 / 采集端自己卡住）才退回补重复。
        uint64_t wroteReal = 0;
        uint64_t wroteRepeat = 0;
        for (uint64_t i = 0; i < need; ++i) {
            bool isReal = false;
            if (i < takeReal) {
                if (i == 0) {
                    isReal = haveNew;
                } else {
                    isReal = popFront(); // 换进 frame；失败就退回补重复
                }
            }
            // 补帧之间的尺寸可能变（拖窗口），跟着这一帧改。
            if (isReal && frameW > 0 && frameH > 0 &&
                (frameW != width_ || frameH != height_)) {
                width_ = frameW;
                height_ = frameH;
            }
            const bool ok = isReal ? writeOnce(frame) : writeRepeat();
            if (!ok) {
                alive = false;
                break;
            }
            if (isReal) {
                ++wroteReal;
            } else {
                ++wroteRepeat;
            }
        }
        // wroteRepeat 只用来标记"这一拍补了几帧重复"；重复帧总数由
        // writeRepeat() 自己累加进 stats_.repeatFrames（见下面那个函数）。

        // ---- 吞吐看门狗：已删除 --------------------------------------------
        //
        // 这里原来是"重复帧占比连续两窗 >40% 就自动降一档（120→90→60，只降
        // 不升）"。主人拍板删掉：配置写多少就录多少，跑不动是**可见的事实**
        // （状态里的 repeatFrames 会涨、画面会卡），不该由程序偷偷改写成 60
        // 还把痕迹抹干净 —— 那次正是它把 requestedFps 也改成 60，导致界面
        // 上的降档提示判定失效，用户只看到"配 120 却显示 60"而没有任何解释。
        //
        // 硬件**拒收**媒体类型时的降档（MfEncoder::StartWithFpsFallback）不在
        // 此列，那个必须留着：不降就整路录不起来。

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

        // ---- 有人要求"改帧率"（界面改配置时）------------------------------
        //
        // 与 flush 同一套「序号 + 条件变量」的机制，也放在补帧**之后**：
        // 这一刻该写的帧都写完了，切片的时间轴右端正好是"现在"。
        //
        // 【为什么不重建会话】原来改帧率走 reload 整路拆建 —— 新轮次目录、
        // recordSegments 归零、时间轴出现空洞，用户看到的就是「短暂的
        // no signal」「导出按钮被禁用」「刚切过去点导出报该路未在录制」
        // （m13003）。这里只是让编码器收掉当前片、按新帧率开新片，墙钟靠
        // nextSegStartUnix_ 继续顺延，导出端看到的是一条连续的时间轴。
        if (alive) {
            uint32_t wantFps = 0;
            uint64_t wantSeq = 0;
            {
                std::lock_guard<std::mutex> lk(fpsMu_);
                wantFps = fpsWantValue_;
                wantSeq = fpsWant_;
            }
            if (wantSeq != fpsDone_) {
                const bool ok = encoder_ && encoder_->SetFps(wantFps);
                if (ok) {
                    // 喂帧线程的节拍立刻切到新帧率（可能被编码器降过档）。
                    fps_.store(encoder_ ? encoder_->EffectiveFps() : wantFps,
                               std::memory_order_relaxed);
                    const double newFps = currentFps();
                    if (newFps > 0 && newFps != fps) {
                        // ---- 【必须把基准时刻 t0 往前挪】--------------------
                        //
                        // 节拍是 `nextDue = t0 + framesWritten / fps`。帧率从
                        // 144 降到 60 时 framesWritten 已经有一千多，用**新**
                        // 帧率去除会算出一个远在未来的时刻（实测 1684/60 =
                        // 28 秒，而实际才过了 14 秒）→ 喂帧线程一路睡到那时，
                        // recordFrames 冻结十几秒（实测 13 秒），界面看着就是
                        // 「改完帧率录制卡死」。
                        //
                        // 把 t0 挪到「按新帧率算，已写帧数正好对应现在」的
                        // 位置：帧数（= 编码器时间轴）不动，成片时长不变，
                        // 只是节拍立刻接上。反过来升帧率时 t0 会往后挪一点，
                        // 同样立刻接上，不会睡。
                        const auto nowTp = std::chrono::steady_clock::now();
                        t0 = nowTp -
                             std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                 std::chrono::duration<double>(
                                     static_cast<double>(framesWritten) / newFps));
                        fps = newFps;
                    }
                }
                {
                    std::lock_guard<std::mutex> lk(fpsMu_);
                    fpsDone_ = wantSeq;
                    fpsOk_ = ok;
                }
                fpsCv_.notify_all();
            }
        }

        // ---- 有人要求"改码率"（界面改配置时）------------------------------
        //
        // 与改帧率同一套机制、同样放在补帧之后。码率写在 SinkWriter 的输出
        // 流上，所以同样要收掉当前片再重开 —— 但**不换轮次目录、不归零分片
        // 序号、时间轴继续顺延**，导出端看到的还是一条连续的时间轴。
        //
        // 用户的诉求（m14071）：「有关编辑的东西一保存立刻落地，像配置改
        // 帧率，改码率什么的」。以前改码率只能靠 reload 拆会话重建。
        if (alive) {
            uint32_t wantMbps = 0;
            uint64_t wantSeq = 0;
            {
                std::lock_guard<std::mutex> lk(brMu_);
                wantMbps = brWantValue_;
                wantSeq = brWant_;
            }
            if (wantSeq != brDone_) {
                const bool ok = encoder_ && encoder_->SetBitrate(wantMbps);
                if (ok) {
                    // 让 RecorderStats / run.json 里的码率描述跟上（只是记录值，
                    // 真正的码率在编码器里）。
                    video_.bitrateMbps = wantMbps;
                }
                {
                    std::lock_guard<std::mutex> lk(brMu_);
                    brDone_ = wantSeq;
                    brOk_ = ok;
                }
                brCv_.notify_all();
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

bool SegmentRecorder::EncodeRepeat() {
    if (!encoder_) return false;

    // 【补帧走 nullptr，不再把同一份 BGRA 再转一遍】见 MfEncoder::Write 的注释：
    // 1440p 一帧的 ConvertToNv12 + memcpy 实测 8~9ms，165fps 的预算只有 6.06ms。
    // 补帧也走全量转换的话，欠账只会越滚越大，最后写进去的绝大部分都是重复帧
    // —— 主人报的「切片刻成 PPT」就是这么来的（实测真实更新率 0.9%）。
    //
    // 复用样本只花 WriteSample 的代价（实测把 165fps 的真实吞吐从 ~107fps
    // 提到接近编码器上限），同样的欠账能真补上，画面不动但时间轴是对的。
    if (!encoder_->Write(nullptr, 0, width_, height_)) {
        return false;
    }

    std::lock_guard<std::mutex> lk(mu_);
    ++stats_.framesIn;
    // 补帧计入 repeatFrames：帧数没少，但这一拍画面是上一帧重复的。
    // framesIn 与 repeatFrames 一起看才有意义（见 RecorderStats 的注释）。
    ++stats_.repeatFrames;
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

bool SegmentRecorder::SetFps(uint32_t fps, uint32_t timeoutMs) {
    if (fps == 0) fps = 60;
    if (fps > MfEncoder::kMaxFps) fps = MfEncoder::kMaxFps;

    // 喂帧线程没起来（Start 失败 / 已经 Stop）：只改记录值，下次 Start 会用。
    if (!feeder_.joinable() || !encoder_ || !encoder_->Running()) {
        fps_.store(fps, std::memory_order_relaxed);
        return true;
    }

    // 跟当前一致就不用麻烦喂帧线程（改一次要重开 SinkWriter，约 300~400ms）。
    if (fps == fps_.load(std::memory_order_relaxed)) return true;

    uint64_t want = 0;
    {
        std::lock_guard<std::mutex> lk(fpsMu_);
        fpsWantValue_ = fps;
        want = ++fpsWant_;
        fpsOk_ = false;
    }
    slotCv_.notify_all(); // 让喂帧线程从 wait 里醒过来，别等满一个节拍

    std::unique_lock<std::mutex> lk(fpsMu_);
    const bool done = fpsCv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                                     [&] { return fpsDone_ >= want; });
    if (!done) return false;
    return fpsOk_;
}

bool SegmentRecorder::SetBitrate(uint32_t bitrateMbps, uint32_t timeoutMs) {
    if (bitrateMbps > 200) bitrateMbps = 200;

    // 喂帧线程没起来（Start 失败 / 已经 Stop）：只改记录值，下次 Start 会用。
    if (!feeder_.joinable() || !encoder_ || !encoder_->Running()) {
        video_.bitrateMbps = bitrateMbps;
        return true;
    }

    // 跟当前一致就不用麻烦喂帧线程（改一次要重开 SinkWriter，约 300~400ms）。
    //
    // 注意比的是**请求值**不是实际值：从「自动估出 5 Mbps」改成「显式配
    // 5 Mbps」实际码率没变，也没必要切片。
    if (bitrateMbps == encoder_->RequestedBitrateMbps() &&
        bitrateMbps == encoder_->EffectiveBitrateMbps()) {
        video_.bitrateMbps = bitrateMbps;
        return true;
    }

    uint64_t want = 0;
    {
        std::lock_guard<std::mutex> lk(brMu_);
        brWantValue_ = bitrateMbps;
        want = ++brWant_;
        brOk_ = false;
    }
    slotCv_.notify_all(); // 让喂帧线程从 wait 里醒过来，别等满一个节拍

    std::unique_lock<std::mutex> lk(brMu_);
    const bool done = brCv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                                     [&] { return brDone_ >= want; });
    if (!done) return false;
    return brOk_;
}

void SegmentRecorder::Stop() {
    // 先收喂帧线程：它可能正卡在 WriteSample 上等编码器。
    // 必须等它退出来再 Finish 编码器，否则它可能往已经关掉的 writer 里写。
    if (feeder_.joinable()) {
        {
            std::lock_guard<std::mutex> lk(slotMu_);
            feedStop_ = true;
        }
        slotCv_.notify_all();
        feeder_.join();
    }

    // ---- 把队列里剩下的帧写完再收工 --------------------------------------
    //
    // 【为什么必须补这一步】喂帧线程是"到点才写"的（节拍由墙钟 × 帧率决定），
    // 所以停服务的那一刻，队列里通常还躺着几帧已经采到、但还没轮到写的画面。
    // 以前这里直接把线程收掉就 Finish 了 —— 那几帧凭空消失，用户看到的就是
    // 「点重启前最后一两秒的切片没了、画面在点击之前就断了」。
    //
    // 队列深度最多 depth 帧（0.6 秒 / 40 帧封顶），一帧编码 8~9ms，所以这一步
    // 最多花三四百毫秒；调用方（优雅停机）给它留的是几秒。
    //
    // 顺序不能反：必须在 feeder_.join() 之后、encoder_->Finish() 之前 ——
    // 前者保证没有别人同时在写编码器，后者保证这几帧还在同一个分片里。
    if (encoder_) {
        std::vector<uint8_t> buf;
        uint32_t w = 0;
        uint32_t h = 0;
        int drained = 0;
        for (;;) {
            {
                std::lock_guard<std::mutex> lk(slotMu_);
                if (ringDepth_ == 0 || ringRead_ >= ringWrite_) break;
                PendingFrame& p = ring_[static_cast<size_t>(ringRead_ % ringDepth_)];
                buf.swap(p.buf);
                w = p.w;
                h = p.h;
                ++ringRead_;
            }
            if (w == 0 || h == 0) continue;
            const size_t rowBytes = static_cast<size_t>(w) * 4u;
            if (w != width_ || h != height_) continue; // 尺寸换代前的残帧，编码器接不了
            if (buf.size() < rowBytes * h) continue;
            if (!encoder_->Write(buf.data(), static_cast<uint32_t>(rowBytes), w, h)) break;
            {
                std::lock_guard<std::mutex> lk(mu_);
                ++stats_.framesIn;
                stats_.bytesIn += static_cast<uint64_t>(rowBytes) * h;
            }
            ++drained;
        }
        if (drained > 0) {
            std::lock_guard<std::mutex> lk(mu_);
            stats_.drainedOnStop = static_cast<uint64_t>(drained);
        }
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
    // 【只扫**本轮**目录，不扫整个 segments 根】
    //
    // 以前这里 `recursive_directory_iterator(segmentsDir_)` 把每一轮的历史
    // 目录全算进来，于是 `recordSegments` 里混着上一次运行留下的片 ——
    // 服务重启后它一上来就是 10 片，界面据此立刻认为"够导了"，点导出时
    // 真正属于本次录制的只有 2~3 秒，其余全被补成 NO SIGNAL。
    // 实测（`launch.mjs --gui-export`）：`exportSegments=2`、
    // `noSignalSeconds=17.1`，就在这个坑里。
    //
    // 导出端**仍然**扫全部轮次（`Exporter::ListSegments`）—— 那是刻意的：
    // 服务中途退出再起来，回放窗口里既有旧片也有新片，中间那段按设计补
    // NO SIGNAL（见 DESIGN 9.4）。但"我手上还有多少可导的片"必须只数本轮。
    const fs::path root = fs::u8path(runDir_.empty() ? segmentsDir_ : runDir_);
    if (!root.empty()) {
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
    RecorderStats out = stats_;
    // 帧率/码率现问编码器：它们能被就地改（SetFps / SetBitrate），也可能被
    // 硬件吞吐上限降档。界面要能看到「我配的是 X、实际跑的是 Y」。
    if (encoder_) {
        out.requestedFps = encoder_->RequestedFps();
        out.effectiveFps = encoder_->EffectiveFps();
        out.requestedBitrateMbps = encoder_->RequestedBitrateMbps();
        out.effectiveBitrateMbps = encoder_->EffectiveBitrateMbps();
    }
    // 采集端被顶掉的帧数（只在 Feed 里累加，那边用的是 slotMu_）。
    out.captureDropped = slotOverwritten_.load(std::memory_order_relaxed);
    out.captureFrames = slotFed_.load(std::memory_order_relaxed);
    return out;
}

} // namespace shadow
