#ifndef ENGLISH_TOOL_H
#define ENGLISH_TOOL_H

#include <cstdint>
#include <string>
#include <vector>

// 英语学习数据层：读取 TF 卡词库（tools/english-pipeline 产出）。
//
// 卡上布局（与 docs/english-wordlib-format.md 一致，勿改动格式）：
//   /sdcard/english/index.bin  "ENGL" + u16 version + u16 count + u32 reserved；
//                              条目 12B：u32 audio_off | u32 audio_size |
//                              u8 unit | u8 flags | u16 reserved
//                              第 i 条对应 audio/w{i+1:04d}.ogg
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
    const std::string& WordAt(int idx) const;
    const std::string& PhoneticAt(int idx) const;
    const std::string& MeaningAt(int idx) const;
    uint8_t UnitAt(int idx) const;

    // 词条 ogg 的绝对路径（audio/w0001.ogg）
    std::string AudioPath(int idx) const;
    // 读取整个 ogg 到内存（单文件约 1.4KB）。失败返回 false。
    bool ReadAudio(int idx, std::vector<uint8_t>& out) const;

    // 单元范围：unit 0 表示复习/词汇表
    int UnitStart(int unit) const;  // 该单元首个 idx（无则 -1）
    int UnitEnd(int unit) const;    // 该单元末个 idx + 1（无则 -1）
    int UnitCount(int unit) const;  // 该单元词数
    // 单元显示名（如 "Unit 3"、"食物水果"）；未知单元返回 "Unit N"
    std::string UnitName(int unit) const;

private:
    struct Entry {
        uint32_t audio_size;
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
    std::vector<std::string> words_;
    std::vector<std::string> phonetics_;
    std::vector<std::string> meanings_;
};

#endif  // ENGLISH_TOOL_H
