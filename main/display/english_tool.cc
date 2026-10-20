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
    words_.clear();
    phonetics_.clear();
    meanings_.clear();
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
        uint8_t rec[12];
        if (fread(rec, 1, sizeof(rec), f) != sizeof(rec)) {
            ESP_LOGW(TAG, "词条 %u 读取失败", i);
            fclose(f);
            entries_.clear();
            return false;
        }
        Entry& e = entries_[i];
        e.audio_size = (uint32_t)rec[0] | ((uint32_t)rec[1] << 8) | ((uint32_t)rec[2] << 16) |
                       ((uint32_t)rec[3] << 24);
        e.unit = rec[8];
        e.flags = rec[9];
    }
    fclose(f);
    count_ = count;

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
static void ParseCsvLine(const std::string& line, std::string* out, int max_fields) {
    out->clear();
    std::string field;
    int fields = 0;
    bool in_quotes = false;
    for (size_t i = 0; i < line.size() && fields < max_fields; ++i) {
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
            out->append(field);
            field.clear();
            ++fields;
        } else if (c != '\r') {
            field += c;
        }
    }
    if (fields < max_fields) {
        out->append(field);
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
    std::string fields[4];
    while (fgets(line, sizeof(line), f) != nullptr) {
        if (line[0] == '\0' || line[0] == '\n' || line[0] == '\r') {
            continue;
        }
        ParseCsvLine(line, fields, 4);
        if (fields[0].empty()) {
            continue;
        }
        words_.push_back(fields[0]);
        phonetics_.push_back(fields[1]);
        meanings_.push_back(fields[2]);
    }
    fclose(f);
    ESP_LOGI(TAG, "词表文本：%u 条", (unsigned)words_.size());
    return true;
}

const std::string& EnglishStore::WordAt(int idx) const {
    static const std::string kEmpty;
    if (idx < 0 || idx >= (int)words_.size()) {
        return kEmpty;
    }
    return words_[idx];
}

const std::string& EnglishStore::PhoneticAt(int idx) const {
    static const std::string kEmpty;
    if (idx < 0 || idx >= (int)phonetics_.size()) {
        return kEmpty;
    }
    return phonetics_[idx];
}

const std::string& EnglishStore::MeaningAt(int idx) const {
    static const std::string kEmpty;
    if (idx < 0 || idx >= (int)meanings_.size()) {
        return kEmpty;
    }
    return meanings_[idx];
}

uint8_t EnglishStore::UnitAt(int idx) const {
    if (idx < 0 || idx >= (int)entries_.size()) {
        return 0;
    }
    return entries_[idx].unit;
}

std::string EnglishStore::AudioPath(int idx) const {
    if (idx < 0 || idx >= count_) {
        return std::string();
    }
    char path[80];
    snprintf(path, sizeof(path), "%s/audio/w%04d.ogg", kRoot, idx + 1);
    return std::string(path);
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

int EnglishStore::UnitCount(int unit) const {
    const int s = UnitStart(unit);
    const int e = UnitEnd(unit);
    return (s < 0 || e < 0) ? 0 : (e - s);
}
