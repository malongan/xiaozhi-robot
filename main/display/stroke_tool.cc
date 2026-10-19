#include "stroke_tool.h"

#include <esp_heap_caps.h>
#include <esp_log.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

static const char* TAG = "StrokeStore";

// 类型码与笔名（与 tools/stroke-pipeline/build.py STROKE_TYPES 对齐）
static const char* const kStrokeNames[] = {
    "", "横", "竖", "撇", "捺", "点", "提", "横钩", "横折", "竖钩", "竖提", "撇折", "弯钩", "竖折",
};

static const uint32_t kIndexHeaderSize = 36;  // 4 magic + 2+2+4 + 6*4
static const uint32_t kIndexEntrySize =
    14;  // u16 ucs2 | u8 grade | u8 n | u32 off | u32 off | u16 rsv

static void* ps_malloc(size_t n) {
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static uint16_t rd_u16(const uint8_t* p) {
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}

static uint32_t rd_u32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

StrokeStore& StrokeStore::GetInstance() {
    static StrokeStore inst;
    return inst;
}

StrokeStore::IndexPos StrokeStore::EntryAt(int i) const {
    const uint8_t* p = index_raw_ + kIndexHeaderSize + (size_t)i * kIndexEntrySize;
    IndexPos e;
    e.ucs2 = rd_u16(p);
    e.grade = p[2];
    e.strokes = p[3];
    e.medians_off = rd_u32(p + 4);
    e.glyph_off = rd_u32(p + 8);
    return e;
}

void StrokeStore::Reset() {
    heap_caps_free(index_raw_);
    heap_caps_free(medians_);
    heap_caps_free(py_buf_);
    heap_caps_free(py_keys_);
    heap_caps_free(py_off_);
    heap_caps_free(py_len_);
    index_raw_ = nullptr;
    medians_ = nullptr;
    py_buf_ = nullptr;
    py_keys_ = nullptr;
    py_off_ = nullptr;
    py_len_ = nullptr;
    medians_size_ = 0;
    char_count_ = 0;
    py_count_ = 0;
    for (int g = 0; g <= 6; ++g) {
        grade_start_[g] = -1;
        grade_end_[g] = 0;
    }
    ready_ = false;
}

bool StrokeStore::Init() {
    if (ready_) {
        return true;
    }
    Reset();
    if (!LoadIndex()) {
        ESP_LOGW(TAG, "index.bin 加载失败（换卡/数据缺失？）");
        Reset();
        return false;
    }
    if (!LoadMediansBlob()) {
        ESP_LOGW(TAG, "medians.bin 加载失败");
        Reset();
        return false;
    }
    if (!LoadPinyin()) {
        ESP_LOGW(TAG, "pinyin.json 解析失败（顶栏拼音不可用）");
    }
    ready_ = true;
    ESP_LOGI(TAG, "笔顺数据就绪：%d 字，medians %uB", char_count_, medians_size_);
    return true;
}

bool StrokeStore::LoadIndex() {
    FILE* f = fopen("/sdcard/index.bin", "rb");
    if (f == nullptr) {
        ESP_LOGW(TAG, "fopen /sdcard/index.bin 失败 errno=%d (%s)", errno, strerror(errno));
        return false;
    }
    bool ok = fseek(f, 0, SEEK_END) == 0;
    long sz = ok ? ftell(f) : -1;
    ok = ok && fseek(f, 0, SEEK_SET) == 0;
    ok = ok && sz > (long)(kIndexHeaderSize + kIndexEntrySize) &&
         sz <= (long)(kIndexHeaderSize + kIndexEntrySize * 20000);
    uint8_t hdr[kIndexHeaderSize];
    if (ok && fread(hdr, 1, kIndexHeaderSize, f) != kIndexHeaderSize) {
        ok = false;
    }
    uint16_t ver = 0, n_chars = 0;
    if (!ok) {
        ESP_LOGW(TAG, "index.bin 校验失败 sz=%ld magic=%.4s", sz, hdr);
    }
    if (ok) {
        ok = memcmp(hdr, "SZBI", 4) == 0;
        ver = rd_u16(hdr + 4);
        n_chars = rd_u16(hdr + 6);
        medians_size_ = rd_u32(hdr + 8);
        ok =
            ok && ver == 1 && n_chars > 0 && medians_size_ > 0 && medians_size_ <= 16 * 1024 * 1024;
        ok = ok && (size_t)sz == kIndexHeaderSize + (size_t)n_chars * kIndexEntrySize;
    }
    if (ok) {
        index_raw_ = static_cast<uint8_t*>(ps_malloc(sz));
        ok = index_raw_ != nullptr;
        if (ok) {
            memcpy(index_raw_, hdr, kIndexHeaderSize);
            ok = fread(index_raw_ + kIndexHeaderSize, 1, sz - kIndexHeaderSize, f) ==
                 (size_t)(sz - kIndexHeaderSize);
        }
    }
    fclose(f);
    if (!ok) {
        return false;
    }
    char_count_ = n_chars;
    uint8_t prev_grade = 0;
    for (int i = 0; i < char_count_; ++i) {
        uint8_t g = EntryAt(i).grade;
        if (g < 1 || g > 6) {
            ESP_LOGE(TAG, "年级字段越界 entry %d", i);
            return false;
        }
        if (g != prev_grade) {
            if (prev_grade >= 1 && prev_grade <= 6) {
                grade_end_[prev_grade] = i;
            }
            grade_start_[g] = i;
            prev_grade = g;
        }
    }
    if (prev_grade >= 1 && prev_grade <= 6) {
        grade_end_[prev_grade] = char_count_;
    }
    return true;
}

bool StrokeStore::LoadMediansBlob() {
    FILE* f = fopen("/sdcard/medians.bin", "rb");
    if (f == nullptr) {
        return false;
    }
    bool ok = fseek(f, 0, SEEK_END) == 0;
    long sz = ok ? ftell(f) : -1;
    ok = ok && fseek(f, 0, SEEK_SET) == 0;
    ok = ok && sz > 0 && (uint32_t)sz == medians_size_;
    if (ok) {
        medians_ = static_cast<uint8_t*>(ps_malloc(medians_size_));
        ok = medians_ != nullptr;
        if (ok) {
            ok = fread(medians_, 1, medians_size_, f) == medians_size_;
        }
    }
    fclose(f);
    return ok && medians_ != nullptr;
}

bool StrokeStore::LoadPinyin() {
    FILE* f = fopen("/sdcard/pinyin.json", "rb");
    if (f == nullptr) {
        return false;
    }
    bool ok = fseek(f, 0, SEEK_END) == 0;
    long sz = ok ? ftell(f) : -1;
    ok = ok && fseek(f, 0, SEEK_SET) == 0;
    ok = ok && sz > 2 && sz < 512 * 1024;
    if (ok) {
        py_buf_ = static_cast<uint8_t*>(ps_malloc(sz + 1));
        ok = py_buf_ != nullptr;
        if (ok) {
            ok = fread(py_buf_, 1, sz, f) == (size_t)sz;
            py_buf_[sz] = 0;
        }
    }
    fclose(f);
    if (!ok) {
        return false;
    }

    // 平面扫描 "字": "拼音" 对（键为 3 字节 UTF-8 单个 CJK 字，按 codepoint 升序）
    std::vector<uint16_t> keys;
    std::vector<uint32_t> offs;
    std::vector<uint8_t> lens;
    const char* p = reinterpret_cast<const char*>(py_buf_);
    while (*p != 0 && keys.size() < 8192) {
        if (*p != '"') {
            ++p;
            continue;
        }
        const char* kstart = ++p;
        while (*p != 0 && *p != '"') {
            ++p;
        }
        if (*p == 0) {
            break;
        }
        size_t klen = p - kstart;
        ++p;
        if (*p != ':') {
            continue;
        }
        ++p;
        while (*p == ' ') {
            ++p;
        }
        if (*p != '"') {
            continue;
        }
        const char* vstart = ++p;
        while (*p != 0 && *p != '"') {
            ++p;
        }
        if (*p == 0) {
            break;
        }
        size_t vlen = p - vstart;
        ++p;
        if (klen == 3 && vlen < 255) {
            int cp = ((kstart[0] & 0x0F) << 12) | ((kstart[1] & 0x3F) << 6) | (kstart[2] & 0x3F);
            if (cp >= 0x2E80) {
                keys.push_back(static_cast<uint16_t>(cp));
                offs.push_back(
                    static_cast<uint32_t>(vstart - reinterpret_cast<const char*>(py_buf_)));
                lens.push_back(static_cast<uint8_t>(vlen));
            }
        }
    }
    if (keys.empty()) {
        return false;
    }
    auto* kd = static_cast<uint16_t*>(ps_malloc(keys.size() * 2));
    auto* od = static_cast<uint32_t*>(ps_malloc(keys.size() * 4));
    auto* ld = static_cast<uint8_t*>(ps_malloc(keys.size()));
    if (kd == nullptr || od == nullptr || ld == nullptr) {
        heap_caps_free(kd);
        heap_caps_free(od);
        heap_caps_free(ld);
        return false;
    }
    memcpy(kd, keys.data(), keys.size() * 2);
    memcpy(od, offs.data(), keys.size() * 4);
    memcpy(ld, lens.data(), keys.size());
    py_keys_ = kd;
    py_off_ = od;
    py_len_ = ld;
    py_count_ = static_cast<int>(keys.size());
    return true;
}

int StrokeStore::FindByChar(uint16_t ucs2) const {
    if (!ready_) {
        return -1;
    }
    for (int i = 0; i < char_count_; ++i) {
        if (EntryAt(i).ucs2 == ucs2) {
            return i;
        }
    }
    return -1;
}

uint16_t StrokeStore::Ucs2At(int idx) const {
    if (!ready_ || idx < 0 || idx >= char_count_) {
        return 0;
    }
    return EntryAt(idx).ucs2;
}

uint8_t StrokeStore::GradeAt(int idx) const {
    if (!ready_ || idx < 0 || idx >= char_count_) {
        return 0;
    }
    return EntryAt(idx).grade;
}

uint8_t StrokeStore::StrokeCountAt(int idx) const {
    if (!ready_ || idx < 0 || idx >= char_count_) {
        return 0;
    }
    return EntryAt(idx).strokes;
}

int StrokeStore::GradeStart(uint8_t grade) const {
    if (!ready_ || grade < 1 || grade > 6) {
        return -1;
    }
    return grade_start_[grade];
}

int StrokeStore::GradeEnd(uint8_t grade) const {
    if (!ready_ || grade < 1 || grade > 6) {
        return 0;
    }
    return grade_end_[grade];
}

const uint8_t* StrokeStore::MediansAt(int idx) const {
    if (!ready_ || idx < 0 || idx >= char_count_) {
        return nullptr;
    }
    return medians_ + EntryAt(idx).medians_off;
}

bool StrokeStore::LoadMedians(int idx, std::vector<StrokeMeta>& out) const {
    out.clear();
    const uint8_t* base = MediansAt(idx);
    if (base == nullptr) {
        return false;
    }
    uint8_t n = base[0];
    const uint8_t* cur = base + 1;
    out.resize(n);
    for (int i = 0; i < n; ++i) {
        StrokeMeta& m = out[i];
        m.npts = cur[0];
        m.width = cur[1];
        m.type = cur[2];
        cur += 4;  // npts | width | type | reserved
        m.pts = reinterpret_cast<const StrokePoint*>(cur);
        cur += static_cast<size_t>(m.npts) * 2;
    }
    return cur - medians_ <= static_cast<long>(medians_size_);
}

bool StrokeStore::DecodeGlyph(int idx, uint8_t* dst) const {
    if (!ready_ || idx < 0 || idx >= char_count_ || dst == nullptr) {
        return false;
    }
    char path[32];
    snprintf(path, sizeof(path), "/sdcard/glyphs/g%d.bin", static_cast<int>(EntryAt(idx).grade));
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        return false;
    }
    bool ok = fseek(f, static_cast<long>(EntryAt(idx).glyph_off), SEEK_SET) == 0;
    uint8_t hdr[5];
    if (ok && fread(hdr, 1, 5, f) != 5) {
        ok = false;
    }
    if (ok) {
        ok = hdr[2] == 1 && rd_u16(hdr) == EntryAt(idx).ucs2;
    }
    uint16_t rle_len = ok ? rd_u16(hdr + 3) : 0;
    ok = ok && rle_len > 0 && rle_len <= 25600;
    if (ok) {
        std::vector<uint8_t> rle(rle_len);
        if (fread(rle.data(), 1, rle_len, f) != rle_len) {
            ok = false;
        } else {
            memset(dst, 0, kGlyphBytes);
            int pos = 0;
            const int total = kGlyphSide * kGlyphSide;
            for (uint16_t i = 0; i < rle_len && pos < total; ++i) {
                const int run = rle[i] >> 1;
                const int v = rle[i] & 1;
                for (int k = 0; k < run && pos < total; ++k, ++pos) {
                    if (v) {
                        dst[pos >> 3] |= static_cast<uint8_t>(0x80 >> (pos & 7));
                    }
                }
            }
            ok = pos == total;  // 位流正好铺满 160x160
        }
    }
    fclose(f);
    return ok;
}

const char* StrokeStore::StrokeNameCn(uint8_t type) const {
    return (type < sizeof(kStrokeNames) / sizeof(kStrokeNames[0])) ? kStrokeNames[type] : "";
}

std::string StrokeStore::StrokeNamesText(int idx) const {
    std::vector<StrokeMeta> ms;
    if (!LoadMedians(idx, ms)) {
        return "";
    }
    std::string out;
    for (size_t i = 0; i < ms.size(); ++i) {
        if (i != 0) {
            out += "、";
        }
        out += StrokeNameCn(ms[i].type);
    }
    return out;
}

std::string StrokeStore::PinyinOf(int idx) const {
    if (!ready_ || idx < 0 || idx >= char_count_ || py_count_ == 0) {
        return "";
    }
    const uint16_t target = Ucs2At(idx);
    if (target == 0) {
        return "";
    }
    int lo = 0, hi = py_count_ - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (py_keys_[mid] < target) {
            lo = mid + 1;
        } else if (py_keys_[mid] > target) {
            hi = mid - 1;
        } else {
            return std::string(reinterpret_cast<const char*>(py_buf_) + py_off_[mid], py_len_[mid]);
        }
    }
    return "";
}

int StrokeStore::Utf8SingleCjk(const std::string& s) {
    size_t i = 0;
    int last_cp = -1;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        int len = 0;
        uint32_t cp = 0;
        if (c < 0x80) {
            ++i;
            continue;  // 空白/引号/拉丁字符直接跳过
        }
        if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
            len = 2;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
            len = 3;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
            len = 4;
            cp = c & 0x07;
        } else {
            break;
        }
        bool cont_ok = true;
        for (int k = 1; k < len; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) {
                cont_ok = false;
                break;
            }
        }
        if (cont_ok) {
            for (int k = 1; k < len; ++k) {
                cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
            }
            if (cp >= 0x2E80 && cp <= 0x9FFF) {
                last_cp = static_cast<int>(cp);  // 扫全串，保留末个汉字
            }
        }
        i += static_cast<size_t>(len);
    }
    return last_cp;
}
