#include "english_tool.h"

#include <esp_log.h>
#include <errno.h>
#include <string.h>

#include <cstdio>
#include <cstdlib>

namespace {
constexpr const char* TAG = "EnglishStore";
constexpr const char* kRoot = "/sdcard/english";
}  // namespace

EnglishStore& EnglishStore::GetInstance() {
    static EnglishStore instance;
    return instance;
}

bool EnglishStore::Init() {
    ready_ = false;
    entries_.clear();
    text_pool_.clear();
    word_offsets_.clear();
    phonetic_offsets_.clear();
    meaning_offsets_.clear();
    count_ = 0;

    if (!LoadIndex()) {
        return false;
    }
    // 词表文本是可选的：拿不到就只有发音，没有释义/音标显示。
    LoadWords();
    ready_ = true;
    ESP_LOGI(TAG, "英语词库就绪：%d 词，%s", count_, name_.c_str());
    return true;
}

bool EnglishStore::LoadIndex() {
    char path[64];
    snprintf(path, sizeof(path), "%s/index.bin", kRoot);
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        ESP_LOGW(TAG, "fopen %s 失败 errno=%d (%s)", path, errno, strerror(errno));
        return false;
    }
    uint8_t header[16];
    if (fread(header, 1, sizeof(header), f) != sizeof(header)) {
        ESP_LOGW(TAG, "%s 头部长度不对", path);
        fclose(f);
        return false;
    }
    if (memcmp(header, "ENGL", 4) != 0) {
        ESP_LOGW(TAG, "%s magic 不匹配", path);
        fclose(f);
        return false;
    }
    const uint16_t version = header[4] | (header[5] << 8);
    const uint16_t count = header[6] | (header[7] << 8);
    if (version != 1) {
        ESP_LOGW(TAG, "词库版本不支持：%u", version);
        fclose(f);
        return false;
    }
    if (count == 0 || count > 20000) {
        ESP_LOGW(TAG, "词库词数异常：%u", count);
        fclose(f);
        return false;
    }
    entries_.resize(count);
    for (uint16_t i = 0; i < count; ++i) {
        uint8_t rec[16];
        if (fread(rec, 1, sizeof(rec), f) != sizeof(rec)) {
            ESP_LOGW(TAG, "词条 %u 读取失败", i);
            fclose(f);
            entries_.clear();
            return false;
        }
        Entry& e = entries_[i];
        // 条目布局：u32 audio_offset | u32 audio_size | u8 unit | u8 flags |
        //          u16 reserved | u32 cn_audio_size
        e.audio_size = (uint32_t)rec[4] | ((uint32_t)rec[5] << 8) | ((uint32_t)rec[6] << 16) |
                       ((uint32_t)rec[7] << 24);
        e.unit = rec[8];
        e.flags = rec[9];
        e.topic = (uint16_t)rec[10] | ((uint16_t)rec[11] << 8);
        e.cn_audio_size = (uint32_t)rec[12] | ((uint32_t)rec[13] << 8) | ((uint32_t)rec[14] << 16) |
                          ((uint32_t)rec[15] << 24);
    }
    fclose(f);
    count_ = count;
    int tagged = 0;
    uint16_t max_topic = 0;
    for (const auto& e : entries_) {
        if (e.topic != 0) {
            tagged++;
            if (e.topic > max_topic) {
                max_topic = e.topic;
            }
        }
    }
    ESP_LOGI(TAG, "分类标签：%d 词已分类，最大分类号 %u", tagged, max_topic);

    // 词库名（可选）
    snprintf(path, sizeof(path), "%s/meta.txt", kRoot);
    f = fopen(path, "rb");
    if (f != nullptr) {
        char line[128];
        while (fgets(line, sizeof(line), f) != nullptr) {
            if (strncmp(line, "name=", 5) == 0) {
                char* v = line + 5;
                char* nl = strpbrk(v, "\r\n");
                if (nl != nullptr) {
                    *nl = '\0';
                }
                if (v[0] != '\0') {
                    name_ = v;
                }
                break;
            }
        }
        fclose(f);
    }
    return true;
}

// 解析 CSV 行：支持双引号包裹的字段（RFC4180）
// 按字段分别写入 out[]，out 会被清空。
static void ParseCsvLine(const std::string& line, std::string* out, int max_fields) {
    for (int i = 0; i < max_fields; ++i) {
        out[i].clear();
    }
    std::string field;
    int fields = 0;
    bool in_quotes = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (in_quotes) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    field += '"';
                    ++i;
                } else {
                    in_quotes = false;
                }
            } else {
                field += c;
            }
        } else if (c == '"') {
            in_quotes = true;
        } else if (c == ',') {
            if (fields < max_fields) {
                out[fields] = field;
            }
            field.clear();
            ++fields;
        } else if (c != '\r' && c != '\n') {
            field += c;
        }
    }
    if (fields < max_fields) {
        out[fields] = field;
    }
}

bool EnglishStore::LoadWords() {
    char path[64];
    snprintf(path, sizeof(path), "%s/words.csv", kRoot);
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        ESP_LOGW(TAG, "fopen %s 失败 errno=%d (%s)", path, errno, strerror(errno));
        return false;
    }
    char line[512];
    // 跳过表头
    if (fgets(line, sizeof(line), f) == nullptr) {
        fclose(f);
        return false;
    }
    // word, phonetic, meaning, unit, unit_title
    std::string fields[5];
    while (fgets(line, sizeof(line), f) != nullptr) {
        if (line[0] == '\0' || line[0] == '\n' || line[0] == '\r') {
            continue;
        }
        ParseCsvLine(line, fields, 5);
        if (fields[0].empty()) {
            continue;
        }
        // 全部字段拼进同一个字符池，用偏移表索引
        word_offsets_.push_back(static_cast<uint32_t>(text_pool_.size()));
        text_pool_.insert(text_pool_.end(), fields[0].begin(), fields[0].end());
        text_pool_.push_back('\0');

        phonetic_offsets_.push_back(static_cast<uint32_t>(text_pool_.size()));
        text_pool_.insert(text_pool_.end(), fields[1].begin(), fields[1].end());
        text_pool_.push_back('\0');

        meaning_offsets_.push_back(static_cast<uint32_t>(text_pool_.size()));
        text_pool_.insert(text_pool_.end(), fields[2].begin(), fields[2].end());
        text_pool_.push_back('\0');

        // 单元标题按单元号索引：同一个单元的标题只存一次
        const int unit_no = atoi(fields[3].c_str());
        if (unit_no >= 0 && unit_no < 64) {
            if (title_offsets_.size() <= (size_t)unit_no) {
                title_offsets_.resize(unit_no + 1, 0);
            }
            if (title_offsets_[unit_no] == 0) {
                title_offsets_[unit_no] = static_cast<uint32_t>(text_pool_.size());
                text_pool_.insert(text_pool_.end(), fields[4].begin(), fields[4].end());
                text_pool_.push_back('\0');
            }
        }
    }
    fclose(f);
    ESP_LOGI(TAG, "词表文本：%u 条，字符池 %u B", (unsigned)word_offsets_.size(),
             (unsigned)text_pool_.size());
    return true;
}

std::string_view EnglishStore::WordAt(int idx) const {
    if (idx < 0 || idx >= (int)word_offsets_.size()) {
        return std::string_view();
    }
    return std::string_view(text_pool_.data() + word_offsets_[idx]);
}

std::string_view EnglishStore::PhoneticAt(int idx) const {
    if (idx < 0 || idx >= (int)phonetic_offsets_.size()) {
        return std::string_view();
    }
    return std::string_view(text_pool_.data() + phonetic_offsets_[idx]);
}

std::string_view EnglishStore::MeaningAt(int idx) const {
    if (idx < 0 || idx >= (int)meaning_offsets_.size()) {
        return std::string_view();
    }
    return std::string_view(text_pool_.data() + meaning_offsets_[idx]);
}

uint8_t EnglishStore::UnitAt(int idx) const {
    if (idx < 0 || idx >= (int)entries_.size()) {
        return 0;
    }
    return entries_[idx].unit;
}

uint16_t EnglishStore::TopicAt(int idx) const {
    if (idx < 0 || idx >= (int)entries_.size()) {
        return 0;
    }
    return entries_[idx].topic;
}

const char* EnglishStore::TopicName(int topic) {
    static const char* kNames[] = {
        "",         "水果",     "蔬菜",     "食物饮料", "身体部位", "颜色",     "数字",
        "时间日期", "动物",     "学校学习", "场所地点", "家居物品", "人物职业", "家人朋友",
        "情绪感受", "动作",     "形容词",   "自然天气", "交通出行", "运动爱好", "节日文化",
        "科技通信", "购物金钱", "健康安全", "服装",     "国家语言", "其他",     "形状",
    };
    if (topic < 0 || topic >= (int)(sizeof(kNames) / sizeof(kNames[0]))) {
        return "";
    }
    return kNames[topic];
}

int EnglishStore::TopicCount(int topic) const {
    int n = 0;
    for (const auto& e : entries_) {
        if (e.topic == topic) {
            n++;
        }
    }
    return n;
}

int EnglishStore::CountFilter(int unit, int topic) const {
    int n = 0;
    for (int i = 0; i < count_; ++i) {
        if (unit > 0 && entries_[i].unit != unit) {
            continue;
        }
        if (topic > 0 && entries_[i].topic != topic) {
            continue;
        }
        n++;
    }
    return n;
}

std::string EnglishStore::AudioPath(int idx) const {
    if (idx < 0 || idx >= count_) {
        return std::string();
    }
    char path[80];
    snprintf(path, sizeof(path), "%s/audio/w%04d.ogg", kRoot, idx + 1);
    return std::string(path);
}

std::string EnglishStore::ChineseAudioPath(int idx) const {
    if (idx < 0 || idx >= count_) {
        return std::string();
    }
    char path[80];
    snprintf(path, sizeof(path), "%s/audio/c%04d.ogg", kRoot, idx + 1);
    return std::string(path);
}

bool EnglishStore::HasChinese(int idx) const {
    if (idx < 0 || idx >= (int)entries_.size()) {
        return false;
    }
    return entries_[idx].cn_audio_size > 0;
}

bool EnglishStore::ReadChineseAudio(int idx, std::vector<uint8_t>& out) const {
    if (!HasChinese(idx)) {
        return false;
    }
    const std::string path = ChineseAudioPath(idx);
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return false;
    }
    const size_t size = entries_[idx].cn_audio_size;
    out.resize(size);
    size_t got = size > 0 ? fread(out.data(), 1, size, f) : 0;
    fclose(f);
    if (got != size) {
        out.clear();
        return false;
    }
    return true;
}

bool EnglishStore::ReadAudio(int idx, std::vector<uint8_t>& out) const {
    if (idx < 0 || idx >= count_) {
        return false;
    }
    const std::string path = AudioPath(idx);
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) {
        ESP_LOGW(TAG, "fopen %s 失败 errno=%d", path.c_str(), errno);
        return false;
    }
    const size_t size = entries_[idx].audio_size;
    out.resize(size);
    size_t got = size > 0 ? fread(out.data(), 1, size, f) : 0;
    fclose(f);
    if (got != size) {
        ESP_LOGW(TAG, "%s 读取不完整：%u/%u", path.c_str(), (unsigned)got, (unsigned)size);
        out.clear();
        return false;
    }
    return true;
}

int EnglishStore::UnitStart(int unit) const {
    for (int i = 0; i < count_; ++i) {
        if (entries_[i].unit == unit) {
            return i;
        }
    }
    return -1;
}

int EnglishStore::UnitEnd(int unit) const {
    for (int i = count_ - 1; i >= 0; --i) {
        if (entries_[i].unit == unit) {
            return i + 1;
        }
    }
    return -1;
}

std::string_view EnglishStore::UnitTitle(int unit) const {
    if (unit <= 0 || unit >= (int)title_offsets_.size()) {
        return std::string_view();
    }
    const uint32_t offset = title_offsets_[unit];
    if (offset == 0) {
        return std::string_view();
    }
    return std::string_view(text_pool_.data() + offset);
}

std::string EnglishStore::UnitName(int unit) const {
    switch (unit) {
        case 0:
            return "全部词库";
        case 1:
            return "Unit 1";
        case 2:
            return "Unit 2";
        case 3:
            return "Unit 3";
        case 4:
            return "Unit 4";
        case 5:
            return "Unit 5";
        case 6:
            return "Unit 6";
        case 7:
            return "戏剧";
        case 8:
            return "食物水果";
        case 9:
            return "数字颜色时间";
        case 10:
            return "动物场所物品";
        case 11:
            return "日常动作形容";
        case 34:
            return "拓展";
        default:
            break;
    }
    if (unit >= 12 && unit <= 17) {
        return "五上 U" + std::to_string(unit - 11);
    }
    if (unit >= 18 && unit <= 23) {
        return "五下 U" + std::to_string(unit - 17);
    }
    if (unit >= 24 && unit <= 29) {
        return "六上 U" + std::to_string(unit - 23);
    }
    if (unit >= 30 && unit <= 33) {
        return "六下 U" + std::to_string(unit - 29);
    }
    return "Unit " + std::to_string(unit);
}

int EnglishStore::ChineseCount() const {
    int n = 0;
    for (const Entry& e : entries_) {
        if (e.cn_audio_size > 0) {
            n++;
        }
    }
    return n;
}

int EnglishStore::CheckAudio(int sample) const {
    int bad = 0;
    const int n = count_ < sample ? count_ : sample;
    // 只读 Ogg 头，避免启动时读完整音频文件
    char hdr[4];
    for (int i = 0; i < n; ++i) {
        const std::string path = AudioPath(i);
        FILE* f = path.empty() ? nullptr : fopen(path.c_str(), "rb");
        if (f == nullptr) {
            ESP_LOGW(TAG, "音频缺失：第 %d 条", i);
            bad++;
            continue;
        }
        const size_t got = fread(hdr, 1, sizeof(hdr), f);
        fclose(f);
        if (got != sizeof(hdr) || memcmp(hdr, "OggS", 4) != 0) {
            ESP_LOGW(TAG, "音频格式异常：第 %d 条", i);
            bad++;
        }
    }
    if (n > 0) {
        ESP_LOGI(TAG, "音频自检：共 %d 词，%d 条异常", n, bad);
    }
    return bad;
}

int EnglishStore::UnitCount(int unit) const {
    const int s = UnitStart(unit);
    const int e = UnitEnd(unit);
    return (s < 0 || e < 0) ? 0 : (e - s);
}
