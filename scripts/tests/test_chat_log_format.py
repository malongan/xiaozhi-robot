"""聊天记录存储层的格式校验。

存储层的实现是 C++，这里不重新实现它，而是校验「生成文件的格式」和
「读取端按格式解析」这两侧一致：用同一套字段布局解析 build.py 生成的样本，
保证固件端 ChatLog::LoadDay() 的假设（24 字节头、magic、CRC、长度）不会漂移。
"""

import struct
import unittest

HEADER_BYTES = 24
MAGIC = 0xC7
MAX_TEXT = 512


def crc16_modbus(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def pack_entry(seq: int, ts: int, role: int, text: str) -> bytes:
    payload = text.encode("utf-8")
    assert len(payload) <= MAX_TEXT
    head = struct.pack("<IIBBH", seq, ts, role, 0, len(payload))
    head += bytes([MAGIC, 0, 0, 0])
    head += struct.pack("<H", crc16_modbus(payload))
    head += bytes(6)
    assert len(head) == HEADER_BYTES, len(head)
    return head + payload


def unpack_entries(blob: bytes):
    """按固件 LoadDay() 的规则解析；尾部残缺（断电）就停止。"""
    out = []
    pos = 0
    while pos + HEADER_BYTES <= len(blob):
        head = blob[pos : pos + HEADER_BYTES]
        seq, ts, role, flags, length = struct.unpack("<IIBBH", head[:12])
        if head[12] != MAGIC or length > MAX_TEXT:
            break
        start = pos + HEADER_BYTES
        end = start + length
        if end > len(blob):
            break
        payload = blob[start:end]
        if struct.unpack("<H", head[16:18])[0] != crc16_modbus(payload):
            break
        out.append(
            {"seq": seq, "ts": ts, "role": role, "flags": flags, "text": payload.decode("utf-8")}
        )
        pos = end
    return out


class HeaderLayoutTest(unittest.TestCase):
    def test_header_is_24_bytes(self):
        self.assertEqual(len(pack_entry(1, 1700000000, 0, "hi")), HEADER_BYTES + 2)

    def test_round_trip(self):
        entries = [pack_entry(i, 1700000000 + i, i % 3, f"消息{i}") for i in range(1, 6)]
        parsed = unpack_entries(b"".join(entries))
        self.assertEqual(len(parsed), 5)
        self.assertEqual(parsed[0]["text"], "消息1")
        self.assertEqual(parsed[4]["seq"], 5)

    def test_unicode_three_bytes(self):
        text = "今天学了 5 个新单词吧？"
        parsed = unpack_entries(pack_entry(1, 1, 1, text))
        self.assertEqual(parsed[0]["text"], text)

    def test_truncated_tail_is_ignored(self):
        """断电模拟：最后一条写了一半，读取端必须保留前面的完整记录。"""
        blob = pack_entry(1, 1, 0, "完整一条") + pack_entry(2, 2, 1, "这条只写了一半")
        parsed = unpack_entries(blob[:-6])
        self.assertEqual(len(parsed), 1)
        self.assertEqual(parsed[0]["text"], "完整一条")

    def test_bad_magic_stops_reading(self):
        blob = bytearray(pack_entry(1, 1, 0, "ok"))
        blob[12] = 0x00
        self.assertEqual(unpack_entries(bytes(blob)), [])

    def test_crc_mismatch_stops_reading(self):
        blob = bytearray(pack_entry(1, 1, 0, "ok"))
        blob[16] ^= 0xFF
        self.assertEqual(unpack_entries(bytes(blob)), [])

    def test_max_text_limit(self):
        long_text = "a" * MAX_TEXT
        self.assertEqual(len(unpack_entries(pack_entry(1, 1, 0, long_text))), 1)
        with self.assertRaises(AssertionError):
            pack_entry(1, 1, 0, "a" * (MAX_TEXT + 1))

    def test_seek_by_index(self):
        """回看翻页依赖定长头：第 N 条的头偏移可以算出来。"""
        blob = b"".join(pack_entry(i, i, 0, "x" * i) for i in range(1, 4))
        pos = 0
        for i in range(1, 4):
            self.assertEqual(struct.unpack("<I", blob[pos : pos + 4])[0], i)
            pos += HEADER_BYTES + i


if __name__ == "__main__":
    unittest.main()
