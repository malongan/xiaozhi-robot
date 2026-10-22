#ifndef STROKE_TOOL_H
#define STROKE_TOOL_H

#include <cstdint>
#include <string>
#include <vector>

struct StrokePoint {
    uint8_t x, y;  // 0..255，屏幕坐标（y 向下）
};

struct StrokeMeta {
    uint8_t npts;
    uint8_t width;  // 7..18 显示线宽
    uint8_t type;   // 1..13 类型码
    const StrokePoint* pts;
};

// 笔顺学习数据层：读取 TF 卡数据包（tools/stroke-pipeline 产出）。
//
// 卡上布局（与 build.py 一致，勿改动格式）：
//   /sdcard/szver.txt            "1\n"
//   /sdcard/index.bin            "SZBI" + u16 ver + u16 count + u32 medians_size
//                                 + u32 glyph_size[6]；条目 14B：
//                                 u16 ucs2 | u8 grade | u8 strokes |
//                                 u32 medians_off | u32 glyph_off | u16 rsv，
//                                 按 (grade, ucs2) 升序
//   /sdcard/medians.bin          per 字：u8 笔数；每笔 u8 点数 | u8 宽 | u8 类型 |
//                                 u8 rsv | 点×N {u8 x,u8 y}（0..255，屏幕坐标 y 向下）
//   /sdcard/glyphs/g1..6.bin     per 记录：u16 ucs2 | u8 mode=1 | u16 rle_len |
//                                 位级 RLE（byte=(run<<1)|color, run<=127，
//                                 160x160 1bpp 全图 MSB-first 位流）
//   /sdcard/pinyin.json          {"字":"pīnyīn",...}，键按 codepoint 升序
class StrokeStore {
public:
    static StrokeStore& GetInstance();

    static constexpr int kGlyphSide = 160;
    static constexpr int kGlyphBytes = kGlyphSide * kGlyphSide / 8;  // 3200
    static constexpr int kGridMax = 255;                             // medians 坐标域

    bool Init();  // SD 挂载成功后调用；失败可重试
    bool Ready() const { return ready_; }

    int CharCount() const { return char_count_; }
    int FindByChar(uint16_t ucs2) const;  // -1 = 不在库
    uint16_t Ucs2At(int idx) const;
    uint8_t GradeAt(int idx) const;  // 1..6
    uint8_t StrokeCountAt(int idx) const;
    int GradeStart(uint8_t grade) const;  // 该年级首个 idx（无字返回 -1）
    int GradeEnd(uint8_t grade) const;    // 该年级末个 idx + 1

    // 返回的 StrokeMeta.pts 直接指入常驻 medians 缓冲，无需拷贝
    bool LoadMedians(int idx, std::vector<StrokeMeta>& out) const;
    bool DecodeGlyph(int idx, uint8_t* dst) const;  // dst 需 kGlyphBytes，填前清零

    const char* StrokeNameCn(uint8_t type) const;
    std::string StrokeNamesText(int idx) const;
    std::string PinyinOf(int idx) const;

    // MCP 入参归一化：去空白/引号，取末个汉字；非法返回 -1
    static int Utf8SingleCjk(const std::string& s);

private:
    struct IndexPos {
        uint16_t ucs2;
        uint8_t grade;
        uint8_t strokes;
        uint32_t medians_off;
        uint32_t glyph_off;
    };
    IndexPos EntryAt(int i) const;

    bool LoadIndex();
    bool LoadMediansBlob();
    bool LoadPinyin();
    bool EnsureMedians() const;  // 首次用到笔顺时才读 medians.bin，缩短启动时间
    const uint8_t* MediansAt(int idx) const;
    void Reset();

    bool ready_ = false;
    int char_count_ = 0;
    uint8_t* index_raw_ = nullptr;  // PSRAM：index.bin 全量 41.6KB
    mutable uint8_t* medians_ = nullptr;  // PSRAM：medians.bin 全量，懒加载
    mutable uint32_t medians_size_ = 0;
    int grade_start_[7] = {-1, -1, -1, -1, -1, -1, -1};
    int grade_end_[7] = {0, 0, 0, 0, 0, 0, 0};
    uint8_t* py_buf_ = nullptr;    // PSRAM：pinyin.json 原文 ~45KB
    uint16_t* py_keys_ = nullptr;  // 升序 codepoint
    uint32_t* py_off_ = nullptr;   // 值切片偏移
    uint8_t* py_len_ = nullptr;
    int py_count_ = 0;
};

#endif  // STROKE_TOOL_H
