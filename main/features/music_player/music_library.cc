#include "music_library.h"

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>

#define TAG "MusicLibrary"

namespace {

constexpr uint32_t kCacheVersion = 2;
constexpr size_t kMaxId3Read = 32 * 1024;

uint32_t HashPath(const std::string& path) {
    uint32_t h = 2166136261u;
    for (unsigned char c : path) {
        h ^= c;
        h *= 16777619u;
    }
    return h == 0 ? 1 : h;
}

std::string ToLower(const std::string& in) {
    std::string out = in;
    for (char& c : out) {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

bool EndsWithNoCase(const std::string& value, const char* suffix) {
    const size_t n = strlen(suffix);
    if (value.size() < n) {
        return false;
    }
    return strcasecmp(value.c_str() + value.size() - n, suffix) == 0;
}

std::string Trim(std::string value) {
    while (!value.empty() && (value.back() == '\0' || value.back() == ' ' || value.back() == '\r' ||
                              value.back() == '\n' || value.back() == '\t')) {
        value.pop_back();
    }
    size_t start = 0;
    while (start < value.size() && (value[start] == ' ' || value[start] == '\t')) {
        start++;
    }
    return value.substr(start);
}

std::string SanitizeField(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (c == '\t' || c == '\n' || c == '\r') {
            out.push_back(' ');
        } else {
            out.push_back(c);
        }
    }
    return Trim(out);
}

std::string JoinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) {
        return name;
    }
    if (dir.back() == '/') {
        return dir + name;
    }
    return dir + "/" + name;
}

void AppendUtf8(std::string& out, uint32_t cp) {
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

std::string Latin1ToUtf8(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        if (data[i] == 0) {
            break;
        }
        AppendUtf8(out, data[i]);
    }
    return out;
}

std::string Utf16ToUtf8(const uint8_t* data, size_t len) {
    if (len >= 2 && data[0] == 0xFF && data[1] == 0xFE) {
        data += 2;
        len -= 2;
        std::string out;
        for (size_t i = 0; i + 1 < len; i += 2) {
            uint32_t cp = data[i] | (data[i + 1] << 8);
            if (cp == 0) {
                break;
            }
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < len) {
                uint32_t low = data[i + 2] | (data[i + 3] << 8);
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    i += 2;
                }
            }
            AppendUtf8(out, cp);
        }
        return Trim(out);
    }
    if (len >= 2 && data[0] == 0xFE && data[1] == 0xFF) {
        data += 2;
        len -= 2;
        std::string out;
        for (size_t i = 0; i + 1 < len; i += 2) {
            uint32_t cp = (data[i] << 8) | data[i + 1];
            if (cp == 0) {
                break;
            }
            AppendUtf8(out, cp);
        }
        return Trim(out);
    }
    std::string out;
    for (size_t i = 0; i + 1 < len; i += 2) {
        uint32_t cp = data[i] | (data[i + 1] << 8);  // assume LE without BOM
        if (cp == 0) {
            break;
        }
        AppendUtf8(out, cp);
    }
    return Trim(out);
}

std::string Utf16BeToUtf8(const uint8_t* data, size_t len) {
    if (len >= 2 && data[0] == 0xFE && data[1] == 0xFF) {
        data += 2;
        len -= 2;
    }
    std::string out;
    for (size_t i = 0; i + 1 < len; i += 2) {
        uint32_t cp = (data[i] << 8) | data[i + 1];
        if (cp == 0) {
            break;
        }
        AppendUtf8(out, cp);
    }
    return Trim(out);
}

std::string Id3Text(const uint8_t* data, size_t len) {
    if (len < 2) {
        return {};
    }
    const uint8_t encoding = data[0];
    data += 1;
    len -= 1;
    switch (encoding) {
        case 0:
            return Latin1ToUtf8(data, len);
        case 1:
            return Utf16ToUtf8(data, len);
        case 2:
            return Utf16BeToUtf8(data, len);
        case 3: {
            size_t n = 0;
            while (n < len && data[n] != 0) {
                n++;
            }
            return Trim(std::string(reinterpret_cast<const char*>(data), n));
        }
        default:
            return {};
    }
}

uint32_t SyncSafe32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0] & 0x7F) << 21) |
           (static_cast<uint32_t>(p[1] & 0x7F) << 14) |
           (static_cast<uint32_t>(p[2] & 0x7F) << 7) | (p[3] & 0x7F);
}

uint32_t BigEndian32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

uint32_t LittleEndian32(const uint8_t* p) {
    return p[0] | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

bool ReadAt(FILE* f, long offset, void* buffer, size_t len) {
    if (fseek(f, offset, SEEK_SET) != 0) {
        return false;
    }
    return fread(buffer, 1, len, f) == len;
}

// Returns the size of the ID3v2 tag (including header).
uint32_t Id3v2Size(FILE* f) {
    uint8_t header[10];
    if (!ReadAt(f, 0, header, sizeof(header)) || memcmp(header, "ID3", 3) != 0) {
        return 0;
    }
    return SyncSafe32(header + 6) + 10;
}

void ParseId3v2(FILE* f, uint32_t tag_size, MusicTrack* track) {
    if (tag_size <= 10) {
        return;
    }
    const size_t read_len = std::min<size_t>(tag_size - 10, kMaxId3Read);
    uint8_t* raw = static_cast<uint8_t*>(heap_caps_malloc(read_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (raw == nullptr) {
        raw = static_cast<uint8_t*>(heap_caps_malloc(read_len, MALLOC_CAP_8BIT));
    }
    if (raw == nullptr) {
        return;
    }
    std::unique_ptr<uint8_t, decltype(&free)> buf(raw, &free);
    if (!ReadAt(f, 10, buf.get(), read_len)) {
        return;
    }
    size_t pos = 0;
    uint8_t version = 3;
    {
        uint8_t header[10];
        if (ReadAt(f, 0, header, sizeof(header))) {
            version = header[3];
        }
    }
    if (version == 2) {
        return;  // ID3v2.2 uses 3-byte frame ids; fall back to the filename
    }
    while (pos + 10 <= read_len) {
        if (buf.get()[pos] == 0) {
            break;
        }
        char id[5] = {static_cast<char>(buf.get()[pos]), static_cast<char>(buf.get()[pos + 1]),
                      static_cast<char>(buf.get()[pos + 2]), static_cast<char>(buf.get()[pos + 3]), 0};
        uint32_t frame_size = (version >= 4) ? SyncSafe32(buf.get() + pos + 4)
                                             : BigEndian32(buf.get() + pos + 4);
        if (frame_size == 0) {
            break;
        }
        if (pos + 10 + frame_size > read_len) {
            break;
        }
        const uint8_t* data = buf.get() + pos + 10;
        std::string value = Id3Text(data, frame_size);
        if (!value.empty()) {
            if (strcmp(id, "TIT2") == 0 && track->title.empty()) {
                track->title = value;
            } else if (strcmp(id, "TPE1") == 0 && track->artist.empty()) {
                track->artist = value;
            } else if (strcmp(id, "TALB") == 0 && track->album.empty()) {
                track->album = value;
            }
        }
        pos += 10 + frame_size;
    }
}

void ParseId3v1(FILE* f, MusicTrack* track) {
    uint8_t tag[128];
    if (fseek(f, 0, SEEK_END) != 0) {
        return;
    }
    long size = ftell(f);
    if (size < 128 || !ReadAt(f, size - 128, tag, sizeof(tag)) || memcmp(tag, "TAG", 3) != 0) {
        return;
    }
    if (track->title.empty()) {
        track->title = Latin1ToUtf8(tag + 3, 30);
    }
    if (track->artist.empty()) {
        track->artist = Latin1ToUtf8(tag + 33, 30);
    }
    if (track->album.empty()) {
        track->album = Latin1ToUtf8(tag + 63, 30);
    }
}

uint32_t Mp3Duration(const std::string& path, FILE* f, uint32_t tag_size, uint32_t file_size) {
    static const int kBitrateV1L3[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256,
                                      320};
    static const int kBitrateV2L3[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144,
                                      160};
    static const int kSampleV1[] = {44100, 48000, 32000};
    static const int kSampleV2[] = {22050, 24000, 16000};
    static const int kSampleV25[] = {11025, 12000, 8000};

    const long audio_offset = static_cast<long>(tag_size);
    const size_t probe = 32 * 1024;
    std::vector<uint8_t> buf(probe);
    size_t got = 0;
    if (fseek(f, audio_offset, SEEK_SET) == 0) {
        got = fread(buf.data(), 1, probe, f);
    }
    if (got < 4) {
        return 0;
    }
    for (size_t i = 0; i + 4 <= got; ++i) {
        if (buf[i] != 0xFF || (buf[i + 1] & 0xE0) != 0xE0) {
            continue;
        }
        const uint8_t b1 = buf[i + 1];
        const uint8_t b2 = buf[i + 2];
        const uint8_t b3 = buf[i + 3];
        const int version_bits = (b1 >> 3) & 0x03;  // 0: 2.5, 2: 2, 3: 1
        const int layer_bits = (b1 >> 1) & 0x03;    // 1: layer III
        if (layer_bits != 1 || version_bits == 1) {
            continue;
        }
        const int bitrate_index = (b2 >> 4) & 0x0F;
        const int sample_index = (b2 >> 2) & 0x03;
        if (bitrate_index == 0 || bitrate_index == 15 || sample_index == 3) {
            continue;
        }
        const bool mpeg1 = version_bits == 3;
        const int bitrate = (mpeg1 ? kBitrateV1L3[bitrate_index] : kBitrateV2L3[bitrate_index]) *
                            1000;
        const int sample_rate = mpeg1 ? kSampleV1[sample_index]
                                      : (version_bits == 2 ? kSampleV2[sample_index]
                                                           : kSampleV25[sample_index]);
        if (bitrate <= 0 || sample_rate <= 0) {
            return 0;
        }
        const size_t frame_offset = i;
        const size_t side_info = mpeg1 ? ((b3 & 0xC0) == 0xC0 ? 17 : 32)
                                       : ((b3 & 0xC0) == 0xC0 ? 9 : 17);
        const size_t xing_offset = frame_offset + 4 + side_info;
        if (xing_offset + 12 <= got &&
            (memcmp(&buf[xing_offset], "Xing", 4) == 0 ||
             memcmp(&buf[xing_offset], "Info", 4) == 0)) {
            const uint32_t flags = BigEndian32(&buf[xing_offset + 4]);
            if (flags & 0x0001) {
                const uint32_t frames = BigEndian32(&buf[xing_offset + 8]);
                const uint32_t samples_per_frame = mpeg1 ? 1152 : 576;
                if (frames > 0) {
                    return static_cast<uint32_t>(static_cast<uint64_t>(frames) * samples_per_frame *
                                                 1000 / sample_rate);
                }
            }
        }
        if (file_size > audio_offset + frame_offset) {
            const uint64_t audio_bytes = file_size - audio_offset - frame_offset;
            return static_cast<uint32_t>(audio_bytes * 8 * 1000 / bitrate);
        }
        return 0;
    }
    (void)path;
    return 0;
}

void ParseFlac(FILE* f, MusicTrack* track) {
    uint8_t magic[4];
    if (!ReadAt(f, 0, magic, 4) || memcmp(magic, "fLaC", 4) != 0) {
        return;
    }
    long offset = 4;
    for (int block = 0; block < 64; ++block) {
        uint8_t header[4];
        if (!ReadAt(f, offset, header, 4)) {
            return;
        }
        const bool last = (header[0] & 0x80) != 0;
        const int type = header[0] & 0x7F;
        const uint32_t len = (static_cast<uint32_t>(header[1]) << 16) |
                             (static_cast<uint32_t>(header[2]) << 8) | header[3];
        if (type == 0 && len >= 18) {
            uint8_t info[34];
            const size_t to_read = std::min<uint32_t>(len, sizeof(info));
            if (ReadAt(f, offset + 4, info, to_read) && to_read >= 18) {
                const uint32_t sample_rate = (static_cast<uint32_t>(info[10]) << 12) |
                                             (static_cast<uint32_t>(info[11]) << 4) |
                                             (info[12] >> 4);
                const uint64_t total_samples = (static_cast<uint64_t>(info[13] & 0x0F) << 32) |
                                               (static_cast<uint64_t>(info[14]) << 24) |
                                               (static_cast<uint64_t>(info[15]) << 16) |
                                               (static_cast<uint64_t>(info[16]) << 8) | info[17];
                if (sample_rate > 0 && total_samples > 0) {
                    track->duration_ms =
                        static_cast<uint32_t>(total_samples * 1000 / sample_rate);
                }
            }
        } else if (type == 4 && len > 8 && len < 64 * 1024) {
            std::vector<uint8_t> buf(len);
            if (ReadAt(f, offset + 4, buf.data(), len)) {
                size_t pos = 0;
                if (pos + 4 <= buf.size()) {
                    const uint32_t vendor_len = LittleEndian32(&buf[pos]);
                    pos += 4;
                    if (pos + vendor_len + 4 <= buf.size()) {
                        pos += vendor_len;
                        const uint32_t count = LittleEndian32(&buf[pos]);
                        pos += 4;
                        for (uint32_t i = 0; i < count && pos + 4 <= buf.size(); ++i) {
                            const uint32_t clen = LittleEndian32(&buf[pos]);
                            pos += 4;
                            if (pos + clen > buf.size()) {
                                break;
                            }
                            std::string comment(reinterpret_cast<const char*>(&buf[pos]), clen);
                            pos += clen;
                            const size_t eq = comment.find('=');
                            if (eq == std::string::npos) {
                                continue;
                            }
                            std::string key = ToLower(comment.substr(0, eq));
                            std::string value = Trim(comment.substr(eq + 1));
                            if (key == "title" && track->title.empty()) {
                                track->title = value;
                            } else if (key == "artist" && track->artist.empty()) {
                                track->artist = value;
                            } else if (key == "album" && track->album.empty()) {
                                track->album = value;
                            }
                        }
                    }
                }
            }
        }
        offset += 4 + len;
        if (last) {
            break;
        }
    }
}

void ParseWav(FILE* f, MusicTrack* track) {
    long offset = 12;
    uint32_t sample_rate = 0;
    uint16_t channels = 0;
    uint16_t bits = 0;
    for (int i = 0; i < 32; ++i) {
        uint8_t header[8];
        if (!ReadAt(f, offset, header, 8)) {
            return;
        }
        const uint32_t len = LittleEndian32(header + 4);
        if (memcmp(header, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            if (!ReadAt(f, offset + 8, fmt, sizeof(fmt))) {
                return;
            }
            channels = fmt[2] | (fmt[3] << 8);
            sample_rate = LittleEndian32(fmt + 4);
            bits = fmt[14] | (fmt[15] << 8);
        } else if (memcmp(header, "data", 4) == 0) {
            if (sample_rate > 0 && channels > 0 && bits > 0) {
                const uint32_t bytes_per_second = sample_rate * channels * (bits / 8);
                if (bytes_per_second > 0) {
                    track->duration_ms = static_cast<uint32_t>(
                        static_cast<uint64_t>(len) * 1000 / bytes_per_second);
                }
            }
            return;
        }
        offset += 8 + len + (len & 1);
    }
}

void ParseM4a(FILE* f, MusicTrack* track) {
    uint8_t header[8];
    if (!ReadAt(f, 0, header, 8)) {
        return;
    }
    long offset = 0;
    for (int i = 0; i < 64; ++i) {
        if (!ReadAt(f, offset, header, 8)) {
            return;
        }
        uint64_t size = BigEndian32(header);
        if (size == 1) {
            uint8_t large[8];
            if (!ReadAt(f, offset + 8, large, 8)) {
                return;
            }
            size = (static_cast<uint64_t>(BigEndian32(large)) << 32) | BigEndian32(large + 4);
            if (size < 16 || size > 32 * 1024 * 1024) {
                return;
            }
        } else if (size < 8 || size > 32 * 1024 * 1024) {
            return;
        }
        if (memcmp(header + 4, "moov", 4) == 0) {
            std::vector<uint8_t> buf(size);
            if (!ReadAt(f, offset, buf.data(), size)) {
                return;
            }
            for (size_t p = 0; p + 24 <= buf.size(); ++p) {
                if (memcmp(&buf[p], "mvhd", 4) != 0) {
                    continue;
                }
                const uint8_t version = buf[p + 4];
                if (version == 1 && p + 36 <= buf.size()) {
                    const uint32_t timescale = BigEndian32(&buf[p + 24]);
                    const uint64_t duration = (static_cast<uint64_t>(BigEndian32(&buf[p + 28]))
                                               << 32) |
                                              BigEndian32(&buf[p + 32]);
                    if (timescale > 0) {
                        track->duration_ms =
                            static_cast<uint32_t>(duration * 1000 / timescale);
                    }
                } else if (p + 24 <= buf.size()) {
                    const uint32_t timescale = BigEndian32(&buf[p + 16]);
                    const uint32_t duration = BigEndian32(&buf[p + 20]);
                    if (timescale > 0) {
                        track->duration_ms =
                            static_cast<uint32_t>(static_cast<uint64_t>(duration) * 1000 /
                                                  timescale);
                    }
                }
                return;
            }
            return;
        }
        offset += static_cast<long>(size);
    }
}

void ParseAac(FILE* f, MusicTrack* track) {
    static const int kSampleRates[] = {96000, 88200, 64000, 48000, 44100, 32000,
                                       24000, 22050, 16000, 12000, 11025, 8000, 7350};
    uint8_t header[7];
    if (!ReadAt(f, 0, header, sizeof(header))) {
        return;
    }
    if (header[0] != 0xFF || (header[1] & 0xF0) != 0xF0) {
        return;
    }
    const int sr_index = (header[2] >> 2) & 0x0F;
    if (sr_index >= static_cast<int>(sizeof(kSampleRates) / sizeof(kSampleRates[0]))) {
        return;
    }
    const uint32_t sample_rate = kSampleRates[sr_index];
    const uint32_t frame_len = (static_cast<uint32_t>(header[3] & 0x03) << 11) |
                               (static_cast<uint32_t>(header[4]) << 3) | (header[5] >> 5);
    if (sample_rate == 0 || frame_len < 7) {
        return;
    }
    // ADTS has no bitrate field; assume CBR and derive it from the first frame.
    const uint64_t bitrate = static_cast<uint64_t>(frame_len) * 8 * sample_rate / 1024;
    if (bitrate > 0) {
        track->duration_ms =
            static_cast<uint32_t>(static_cast<uint64_t>(track->file_size) * 8 * 1000 / bitrate);
    }
}

void ParseMetadata(const std::string& path, MusicTrack* track) {
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return;
    }
    const uint32_t tag_size = Id3v2Size(f);
    if (tag_size > 0) {
        ParseId3v2(f, tag_size, track);
    }
    if (EndsWithNoCase(path, ".mp3")) {
        track->duration_ms = Mp3Duration(path, f, tag_size, track->file_size);
        if (track->title.empty() || track->artist.empty()) {
            ParseId3v1(f, track);
        }
    } else if (EndsWithNoCase(path, ".flac")) {
        ParseFlac(f, track);
    } else if (EndsWithNoCase(path, ".wav")) {
        ParseWav(f, track);
    } else if (EndsWithNoCase(path, ".m4a")) {
        ParseM4a(f, track);
    } else if (EndsWithNoCase(path, ".aac")) {
        ParseAac(f, track);
    }
    fclose(f);
}

bool IsSupportedExtension(const std::string& name) {
    return EndsWithNoCase(name, ".mp3") || EndsWithNoCase(name, ".m4a") ||
           EndsWithNoCase(name, ".aac") || EndsWithNoCase(name, ".flac") ||
           EndsWithNoCase(name, ".wav") || EndsWithNoCase(name, ".ogg") ||
           EndsWithNoCase(name, ".opus");
}

std::string BaseNameWithoutExtension(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    const size_t start = slash == std::string::npos ? 0 : slash + 1;
    const size_t dot = path.find_last_of('.');
    const size_t end = (dot == std::string::npos || dot < start) ? path.size() : dot;
    return path.substr(start, end - start);
}

void DeriveName(const std::string& path, MusicTrack* track) {
    const std::string stem = BaseNameWithoutExtension(path);
    if (stem.empty()) {
        track->title = path;
        return;
    }
    const size_t dash = stem.find(" - ");
    if (dash != std::string::npos && dash > 0) {
        track->artist = Trim(stem.substr(0, dash));
        track->title = Trim(stem.substr(dash + 3));
    } else {
        track->title = stem;
    }
}

bool AppendTrack(const std::string& path, const struct stat& st, std::vector<MusicTrack>& out) {
    MusicTrack track;
    track.path = path;
    track.id = HashPath(path);
    track.file_size = static_cast<uint32_t>(st.st_size);
    track.extension = path.substr(path.find_last_of('.') + 1);
    ParseMetadata(path, &track);
    DeriveName(path, &track);
    track.title = SanitizeField(track.title);
    track.artist = SanitizeField(track.artist);
    track.album = SanitizeField(track.album);
    if (track.title.empty()) {
        return false;
    }
    out.push_back(std::move(track));
    return true;
}

void ScanDir(const std::string& dir, std::vector<MusicTrack>& out, int depth) {
    if (depth > 3 || static_cast<int>(out.size()) >= MusicLibrary::kMaxTracks) {
        return;
    }
    DIR* handle = opendir(dir.c_str());
    if (handle == nullptr) {
        return;
    }
    struct dirent* entry = nullptr;
    while ((entry = readdir(handle)) != nullptr) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        const std::string path = JoinPath(dir, entry->d_name);
        struct stat st;
        if (stat(path.c_str(), &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            ScanDir(path, out, depth + 1);
        } else if (S_ISREG(st.st_mode) && IsSupportedExtension(entry->d_name)) {
            AppendTrack(path, st, out);
            if (static_cast<int>(out.size()) >= MusicLibrary::kMaxTracks) {
                break;
            }
        }
    }
    closedir(handle);
}

// Songs dropped directly into the card root are also picked up, without
// descending into unrelated folders (English/stroke data live in siblings).
void ScanCardRoot(std::vector<MusicTrack>& out) {
    DIR* handle = opendir(MusicLibrary::kCardRootPath);
    if (handle == nullptr) {
        return;
    }
    struct dirent* entry = nullptr;
    while ((entry = readdir(handle)) != nullptr) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        const std::string path = JoinPath(MusicLibrary::kCardRootPath, entry->d_name);
        struct stat st;
        if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        if (!IsSupportedExtension(entry->d_name)) {
            continue;
        }
        AppendTrack(path, st, out);
        if (static_cast<int>(out.size()) >= MusicLibrary::kMaxTracks) {
            break;
        }
    }
    closedir(handle);
}

bool ParseUint(const std::string& value, uint32_t* out) {
    if (value.empty()) {
        return false;
    }
    char* end = nullptr;
    const unsigned long parsed = strtoul(value.c_str(), &end, 10);
    if (end == value.c_str()) {
        return false;
    }
    *out = static_cast<uint32_t>(parsed);
    return true;
}

uint32_t MixSignature(uint32_t hash, uint32_t value) {
    hash ^= value + 0x9E3779B9u + (hash << 6) + (hash >> 2);
    return hash;
}

// Walks directories only (plus a stat per file) so a changed tree is detected
// without reading tags. This also covers files added inside subdirectories,
// which the root directory mtime alone does not catch.
void WalkSignature(const std::string& dir, int depth, uint32_t* hash, int* files) {
    if (depth > 3) {
        return;
    }
    DIR* handle = opendir(dir.c_str());
    if (handle == nullptr) {
        return;
    }
    struct dirent* entry = nullptr;
    while ((entry = readdir(handle)) != nullptr) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        const std::string path = JoinPath(dir, entry->d_name);
        struct stat st;
        if (stat(path.c_str(), &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            WalkSignature(path, depth + 1, hash, files);
        } else if (S_ISREG(st.st_mode) && IsSupportedExtension(entry->d_name)) {
            ++(*files);
            *hash = MixSignature(*hash, HashPath(path));
            *hash = MixSignature(*hash, static_cast<uint32_t>(st.st_size));
            *hash = MixSignature(*hash, static_cast<uint32_t>(st.st_mtime));
        }
    }
    closedir(handle);
}

void WalkCardSignature(uint32_t* hash, int* files) {
    struct stat st;
    if (stat(MusicLibrary::kRootPath, &st) == 0 && S_ISDIR(st.st_mode)) {
        WalkSignature(MusicLibrary::kRootPath, 0, hash, files);
    }
    DIR* handle = opendir(MusicLibrary::kCardRootPath);
    if (handle == nullptr) {
        return;
    }
    struct dirent* entry = nullptr;
    while ((entry = readdir(handle)) != nullptr) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        const std::string path = JoinPath(MusicLibrary::kCardRootPath, entry->d_name);
        struct stat file_stat;
        if (stat(path.c_str(), &file_stat) != 0 || !S_ISREG(file_stat.st_mode)) {
            continue;
        }
        if (!IsSupportedExtension(entry->d_name)) {
            continue;
        }
        ++(*files);
        *hash = MixSignature(*hash, HashPath(path));
        *hash = MixSignature(*hash, static_cast<uint32_t>(file_stat.st_size));
        *hash = MixSignature(*hash, static_cast<uint32_t>(file_stat.st_mtime));
    }
    closedir(handle);
}

}  // namespace

MusicLibrary& MusicLibrary::GetInstance() {
    static MusicLibrary instance;
    return instance;
}

std::string MusicLibrary::DisplayTitle(const MusicTrack& track) {
    return track.title.empty() ? BaseNameWithoutExtension(track.path) : track.title;
}

std::string MusicLibrary::DisplayArtist(const MusicTrack& track) {
    return track.artist.empty() ? "未知歌手" : track.artist;
}

std::string MusicLibrary::DisplayAlbum(const MusicTrack& track) {
    return track.album.empty() ? "未知专辑" : track.album;
}

std::string MusicLibrary::FormatDuration(uint32_t duration_ms) {
    const uint32_t total_seconds = duration_ms / 1000;
    const uint32_t seconds = total_seconds % 60;
    const uint32_t minutes = (total_seconds / 60) % 60;
    const uint32_t hours = total_seconds / 3600;
    char buffer[16];
    if (hours > 0) {
        snprintf(buffer, sizeof(buffer), "%" PRIu32 ":%02" PRIu32 ":%02" PRIu32, hours, minutes, seconds);
    } else {
        snprintf(buffer, sizeof(buffer), "%02" PRIu32 ":%02" PRIu32, minutes, seconds);
    }
    return buffer;
}

void MusicLibrary::MarkDirty() {
    dirty_.store(true);
    unlink(kCachePath);
}

bool MusicLibrary::EnsureLoaded(bool force) {
    int file_count = 0;
    const uint32_t signature = ComputeTreeSignature(&file_count);
    if (signature == 0) {
        ESP_LOGW(TAG, "SD card %s not available", kCardRootPath);
        ready_.store(false);
        dirty_.store(false);
        signature_.store(0);
        return false;
    }
    if (!force && ready_.load() && !dirty_.load() && signature == signature_.load()) {
        return Count() > 0;
    }
    static std::mutex scan_mutex;
    std::unique_lock<std::mutex> scan_lock(scan_mutex);
    if (!force && ready_.load() && !dirty_.load() && signature == signature_.load()) {
        return Count() > 0;
    }
    if (scanning_.load()) {
        scan_lock.unlock();
        for (int i = 0; i < 600 && scanning_.load(); ++i) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        return ready_.load() && !tracks_.empty();
    }

    scanning_.store(true);
    bool cache_hit = !force && LoadCacheLocked(signature);
    if (!cache_hit) {
        std::vector<MusicTrack> scanned;
        scanned.reserve(128);
        ScanLocked(scanned);
        std::sort(scanned.begin(), scanned.end(), [](const MusicTrack& a, const MusicTrack& b) {
            const std::string ta = ToLower(DisplayTitle(a));
            const std::string tb = ToLower(DisplayTitle(b));
            const int cmp = ta.compare(tb);
            return cmp < 0 || (cmp == 0 && a.path < b.path);
        });
        // Re-read the signature: files may have been added while scanning.
        int scanned_files = 0;
        const uint32_t scanned_signature = ComputeTreeSignature(&scanned_files);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tracks_ = std::move(scanned);
            signature_.store(scanned_signature);
            LoadRecentLocked();
            SaveCacheLocked(scanned_signature);
        }
        ESP_LOGI(TAG, "Indexed %d tracks (signature %08" PRIx32 ")",
                 static_cast<int>(tracks_.size()), scanned_signature);
    } else {
        ESP_LOGI(TAG, "Loaded index from cache (%d files, %d tracks)", file_count,
                 Count());
    }

    scanning_.store(false);
    ready_.store(true);
    dirty_.store(false);
    return Count() > 0;
}

bool MusicLibrary::Rescan() { return EnsureLoaded(true); }

bool MusicLibrary::TreeChanged() const {
    if (!ready_.load()) {
        return true;
    }
    const uint32_t signature = ComputeTreeSignature();
    return signature == 0 || signature != signature_.load();
}

void MusicLibrary::LoadRecentLocked() {
    recent_ids_.clear();
    FILE* f = fopen(kRecentPath, "rb");
    if (f == nullptr) {
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), f) != nullptr) {
        std::string path = Trim(line);
        if (path.empty()) {
            continue;
        }
        for (const auto& track : tracks_) {
            if (track.path == path) {
                recent_ids_.push_back(track.id);
                break;
            }
        }
        if (static_cast<int>(recent_ids_.size()) >= kMaxRecent) {
            break;
        }
    }
    fclose(f);
}

void MusicLibrary::SaveRecentLocked() const {
    FILE* f = fopen(kRecentPath, "wb");
    if (f == nullptr) {
        return;
    }
    for (uint32_t id : recent_ids_) {
        for (const auto& track : tracks_) {
            if (track.id == id) {
                fprintf(f, "%s\n", track.path.c_str());
                break;
            }
        }
    }
    fclose(f);
}

void MusicLibrary::ScanLocked(std::vector<MusicTrack>& out) {
    struct stat st;
    if (stat(kRootPath, &st) == 0 && S_ISDIR(st.st_mode)) {
        ScanDir(kRootPath, out, 0);
    }
    ScanCardRoot(out);
}

uint32_t MusicLibrary::ComputeTreeSignature(int* file_count) const {
    struct stat st;
    if (stat(kCardRootPath, &st) != 0 || !S_ISDIR(st.st_mode)) {
        if (file_count != nullptr) {
            *file_count = 0;
        }
        return 0;
    }
    uint32_t hash = 2166136261u;
    int files = 0;
    WalkCardSignature(&hash, &files);
    if (file_count != nullptr) {
        *file_count = files;
    }
    return hash == 0 ? 1 : hash;
}

bool MusicLibrary::LoadCacheLocked(uint32_t signature) {
    FILE* f = fopen(kCachePath, "rb");
    if (f == nullptr) {
        return false;
    }
    char line[2048];
    uint32_t version = 0;
    uint32_t cached_signature = 0;
    uint32_t count = 0;
    if (fgets(line, sizeof(line), f) == nullptr || sscanf(line, "MLIB%" SCNu32, &version) != 1 ||
        version != kCacheVersion) {
        fclose(f);
        return false;
    }
    if (fgets(line, sizeof(line), f) == nullptr ||
        sscanf(line, "SIG %" SCNu32, &cached_signature) != 1 ||
        cached_signature != signature) {
        fclose(f);
        return false;
    }
    if (fgets(line, sizeof(line), f) == nullptr || sscanf(line, "COUNT %" SCNu32, &count) != 1 ||
        count == 0 || count > static_cast<uint32_t>(kMaxTracks)) {
        fclose(f);
        return false;
    }
    std::vector<MusicTrack> loaded;
    loaded.reserve(count);
    while (static_cast<uint32_t>(loaded.size()) < count && fgets(line, sizeof(line), f) != nullptr) {
        std::string entry = Trim(line);
        if (entry.empty()) {
            continue;
        }
        // path \t id \t size \t duration \t ext \t title \t artist \t album
        std::vector<std::string> fields;
        size_t start = 0;
        while (true) {
            const size_t tab = entry.find('\t', start);
            if (tab == std::string::npos) {
                fields.push_back(entry.substr(start));
                break;
            }
            fields.push_back(entry.substr(start, tab - start));
            start = tab + 1;
        }
        if (fields.size() != 8) {
            break;
        }
        MusicTrack track;
        track.path = fields[0];
        uint32_t id = 0;
        uint32_t size = 0;
        uint32_t duration = 0;
        if (!ParseUint(fields[1], &id) || !ParseUint(fields[2], &size) ||
            !ParseUint(fields[3], &duration)) {
            break;
        }
        track.id = id;
        track.file_size = size;
        track.duration_ms = duration;
        track.extension = fields[4];
        track.title = fields[5];
        track.artist = fields[6];
        track.album = fields[7];
        loaded.push_back(std::move(track));
    }
    if (loaded.size() != count) {
        fclose(f);
        return false;
    }
    fclose(f);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tracks_ = std::move(loaded);
        signature_.store(signature);
        LoadRecentLocked();
    }
    ESP_LOGI(TAG, "Loaded %d tracks from cache", static_cast<int>(tracks_.size()));
    return true;
}

void MusicLibrary::SaveCacheLocked(uint32_t signature) const {
    const std::string temp_path = std::string(kCachePath) + ".tmp";
    FILE* f = fopen(temp_path.c_str(), "wb");
    if (f == nullptr) {
        return;
    }
    fprintf(f, "MLIB%" PRIu32 "\n", kCacheVersion);
    fprintf(f, "SIG %" PRIu32 "\n", signature);
    fprintf(f, "COUNT %" PRIu32 "\n", static_cast<uint32_t>(tracks_.size()));
    for (const auto& track : tracks_) {
        fprintf(f, "%s\t%" PRIu32 "\t%" PRIu32 "\t%" PRIu32 "\t%s\t%s\t%s\t%s\n", track.path.c_str(),
                track.id, track.file_size, track.duration_ms, track.extension.c_str(), track.title.c_str(),
                track.artist.c_str(), track.album.c_str());
    }
    fclose(f);
    rename(temp_path.c_str(), kCachePath);
}

void MusicLibrary::StartBackgroundScan() {
    // Re-check the SD tree (directories only) so songs added while the device
    // was running, or inside sub-folders, are picked up on the next open.
    if (!dirty_.load() && !TreeChanged()) {
        return;
    }
    if (scanning_.load() || scan_task_pending_.exchange(true)) {
        return;
    }
    BaseType_t created = xTaskCreateWithCaps(
        [](void*) {
            auto& library = MusicLibrary::GetInstance();
            library.EnsureLoaded(false);
            library.scan_task_pending_.store(false);
            vTaskDeleteWithCaps(nullptr);
        },
        "music_scan", 10240, nullptr, 2, nullptr, MALLOC_CAP_SPIRAM);
    if (created != pdPASS) {
        // Boards without PSRAM fall back to internal RAM; the scan is a
        // one-shot task, so the temporary cost is acceptable.
        created = xTaskCreateWithCaps(
            [](void*) {
                auto& library = MusicLibrary::GetInstance();
                library.EnsureLoaded(false);
                library.scan_task_pending_.store(false);
                vTaskDeleteWithCaps(nullptr);
            },
            "music_scan", 10240, nullptr, 2, nullptr, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (created != pdPASS) {
        scan_task_pending_.store(false);
        ESP_LOGE(TAG, "Cannot create scan task");
    }
}

int MusicLibrary::Count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(tracks_.size());
}

bool MusicLibrary::TrackAt(int index, MusicTrack* out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index < 0 || index >= static_cast<int>(tracks_.size())) {
        return false;
    }
    if (out != nullptr) {
        *out = tracks_[index];
    }
    return true;
}

std::vector<MusicTrack> MusicLibrary::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tracks_;
}

int MusicLibrary::IndexOfId(uint32_t id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (int i = 0; i < static_cast<int>(tracks_.size()); ++i) {
        if (tracks_[i].id == id) {
            return i;
        }
    }
    return -1;
}

std::vector<std::string> MusicLibrary::Artists() const {
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& track : tracks_) {
        const std::string name = DisplayArtist(track);
        if (std::find(out.begin(), out.end(), name) == out.end()) {
            out.push_back(name);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> MusicLibrary::Albums() const {
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& track : tracks_) {
        const std::string name = DisplayAlbum(track);
        if (std::find(out.begin(), out.end(), name) == out.end()) {
            out.push_back(name);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<int> MusicLibrary::IndicesByArtist(const std::string& artist) const {
    std::vector<int> out;
    std::lock_guard<std::mutex> lock(mutex_);
    for (int i = 0; i < static_cast<int>(tracks_.size()); ++i) {
        if (DisplayArtist(tracks_[i]) == artist) {
            out.push_back(i);
        }
    }
    return out;
}

std::vector<int> MusicLibrary::IndicesByAlbum(const std::string& album) const {
    std::vector<int> out;
    std::lock_guard<std::mutex> lock(mutex_);
    for (int i = 0; i < static_cast<int>(tracks_.size()); ++i) {
        if (DisplayAlbum(tracks_[i]) == album) {
            out.push_back(i);
        }
    }
    return out;
}

std::vector<int> MusicLibrary::AllIndices() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<int> out;
    out.reserve(tracks_.size());
    for (int i = 0; i < static_cast<int>(tracks_.size()); ++i) {
        out.push_back(i);
    }
    return out;
}

std::vector<int> MusicLibrary::RecentIndices() const {
    std::vector<int> out;
    std::lock_guard<std::mutex> lock(mutex_);
    for (uint32_t id : recent_ids_) {
        for (int i = 0; i < static_cast<int>(tracks_.size()); ++i) {
            if (tracks_[i].id == id) {
                out.push_back(i);
                break;
            }
        }
    }
    return out;
}

void MusicLibrary::NotePlayed(int index) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index < 0 || index >= static_cast<int>(tracks_.size())) {
            return;
        }
        const uint32_t id = tracks_[index].id;
        recent_ids_.erase(std::remove(recent_ids_.begin(), recent_ids_.end(), id),
                          recent_ids_.end());
        recent_ids_.insert(recent_ids_.begin(), id);
        if (static_cast<int>(recent_ids_.size()) > kMaxRecent) {
            recent_ids_.resize(kMaxRecent);
        }
        SaveRecentLocked();
    }
}

std::vector<int> MusicLibrary::Search(const std::string& query, const std::string& artist,
                                      const std::string& album, int max_results) const {
    std::vector<int> scoped;
    std::vector<std::pair<int, int>> scored;
    const std::string query_lower = ToLower(Trim(query));
    std::vector<std::string> tokens;
    {
        std::string current;
        for (char c : query_lower) {
            if (c == ' ' || c == '\t') {
                if (!current.empty()) {
                    tokens.push_back(current);
                    current.clear();
                }
            } else {
                current.push_back(c);
            }
        }
        if (!current.empty()) {
            tokens.push_back(current);
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    for (int i = 0; i < static_cast<int>(tracks_.size()); ++i) {
        const MusicTrack& track = tracks_[i];
        const std::string track_artist = DisplayArtist(track);
        const std::string track_album = DisplayAlbum(track);
        if (!artist.empty() && track_artist != artist) {
            continue;
        }
        if (!album.empty() && track_album != album) {
            continue;
        }
        const std::string title = ToLower(DisplayTitle(track));
        const std::string artist_lower = ToLower(track_artist);
        const std::string album_lower = ToLower(track_album);
        if (query_lower.empty()) {
            scored.emplace_back(0, i);
            continue;
        }
        int score = 0;
        bool matched = true;
        for (const auto& token : tokens) {
            int token_score = 0;
            if (title == token) {
                token_score = 120;
            } else if (title.find(token) != std::string::npos) {
                token_score = 40;
            }
            if (artist_lower.find(token) != std::string::npos) {
                token_score += 15;
            }
            if (album_lower.find(token) != std::string::npos) {
                token_score += 8;
            }
            if (token_score == 0) {
                matched = false;
                break;
            }
            score += token_score;
        }
        if (!matched) {
            continue;
        }
        if (title == query_lower) {
            score += 500;
        }
        scored.emplace_back(score, i);
    }

    std::sort(scored.begin(), scored.end(), [this](const auto& a, const auto& b) {
        if (a.first != b.first) {
            return a.first > b.first;
        }
        return ToLower(DisplayTitle(tracks_[a.second])) <
               ToLower(DisplayTitle(tracks_[b.second]));
    });
    std::vector<int> out;
    const int limit = std::min<int>(max_results, static_cast<int>(scored.size()));
    out.reserve(limit);
    for (int i = 0; i < limit; ++i) {
        out.push_back(scored[i].second);
    }
    return out;
}

std::string MusicLibrary::StatusJson() const {
    char buffer[128];
    std::lock_guard<std::mutex> lock(mutex_);
    snprintf(buffer, sizeof(buffer), "{\"ok\":true,\"track_count\":%d,\"indexed\":%s}",
             static_cast<int>(tracks_.size()), ready_.load() ? "true" : "false");
    return buffer;
}
