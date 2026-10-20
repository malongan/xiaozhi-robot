#ifndef ENGLISH_TOOL_H
#define ENGLISH_TOOL_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// 英语学习数据层：读取 TF 卡词库（tools/english-pipeline 产出）。
//
// 卡上布局（与 docs/english-wordlib-format.md 一致，勿改动格式）：
//   /sdcard/english/index.bin  "ENGL" + u16 version + u16 count + u32 reserved；
//                              条目 16B：u32 audio_off | u32 audio_size |
//                              u8 unit | u8 flags | u16 reserved | u32 cn_size
//                              第 i 条对应 audio/w{i+1:04d}.ogg（英文）
//                              和 audio/c{i+1:04d}.ogg（中文释义，可选）
//   /sdcard/english/words.csv  源词表：word,phonetic,meaning,unit（UTF-8）
//   /sdcard/english/meta.txt   name=... / version=... / count=...
class EnglishStore {
public:
    static EnglishStore& GetInstance();

    bool Init();  // SD 挂载成功后调用；失败可重试
    bool Ready() const { return ready_; }

    int WordCount() const { return count_; }
    const std::string& Name() const { return name_; }

    // 第 idx 个词条（0 起）。空串表示越界。
    // 返回的 string_view 指向内部字符池，调用方需立即使用
    std::string_view WordAt(int idx) const;
    std::string_view PhoneticAt(int idx) const;
    std::string_view MeaningAt(int idx) const;
    uint8_t UnitAt(int idx) const;

    // 词条 ogg 的绝对路径（audio/w0001.ogg）
    std::string AudioPath(int idx) const;
    // 读取整个 ogg 到内存（单文件约 1.4KB）。失败返回 false。
    bool ReadAudio(int idx, std::vector<uint8_t>& out) const;
    // 读取中文释义音频；没有则返回 false。
    bool ReadChineseAudio(int idx, std::vector<uint8_t>& out) const;
    // 该词是否有中文音频
    bool HasChinese(int idx) const;
    // 中文音频路径
    std::string ChineseAudioPath(int idx) const;

    // 单元范围：unit 0 表示复习/词汇表
    int UnitStart(int unit) const;  // 该单元首个 idx（无则 -1）
    int UnitEnd(int unit) const;    // 该单元末个 idx + 1（无则 -1）
    int UnitCount(int unit) const;  // 该单元词数
    // 单元显示名（如 "Unit 3"、"食物水果"）；未知单元返回 "Unit N"
    std::string UnitName(int unit) const;
    // 课本单元标题（来自 words.csv 的 unit_title 列），没有则返回空
    std::string_view UnitTitle(int unit) const;
    // 自检：抽查前 n 个词的音频是否可读。返回有问题��词数。
    int CheckAudio(int sample) const;
    // 有中文音频的词数
    int ChineseCount() const;

private:
    struct Entry {
        uint32_t audio_size;
        uint32_t cn_audio_size;
        uint8_t unit;
        uint8_t flags;
    };

    bool LoadIndex();
    bool LoadWords();

    bool ready_ = false;
    int count_ = 0;
    std::string name_ = "英语词库";
    std::vector<Entry> entries_;
    // 词表文本（每词 4 段，索引与 entries_ 对齐）
    // 用定长字符池而不是 std::vector<std::string>：避免大量小对象占用
    // 内部 SRAM（词库上千词时 std::string 的堆开销会到 100KB+）。
    std::vector<char> text_pool_;
    std::vector<uint32_t> word_offsets_;
    std::vector<uint32_t> phonetic_offsets_;
    std::vector<uint32_t> meaning_offsets_;
    std::vector<uint32_t> title_offsets_;
};

#endif  // ENGLISH_TOOL_H
