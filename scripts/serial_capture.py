"""把串口日志原样落盘，用于抓 panic / 重启现场。

用法：
    python scripts/serial_capture.py COM4 180
    python scripts/serial_capture.py COM4 180 logs/crash.log

第二个参数是抓取时长（秒）。日志同时写 stdout 和文件，进程被 Ctrl+C
或时长到期都会正常关闭串口。抓完直接看文件尾部的 Guru Meditation /
assert / rst: 段落。
"""

import sys
import time

import serial


def main() -> int:
    port = sys.argv[1] if len(sys.argv) > 1 else "COM4"
    duration = int(sys.argv[2]) if len(sys.argv) > 2 else 120
    out_path = sys.argv[3] if len(sys.argv) > 3 else "logs/serial_capture.log"

    ser = serial.Serial(port, 115200, timeout=0.5)
    ser.setDTR(False)
    ser.setRTS(False)
    ser.reset_input_buffer()

    start = time.time()
    with open(out_path, "w", encoding="utf-8", errors="replace") as fh:
        try:
            while time.time() - start < duration:
                chunk = ser.read(4096)
                if chunk:
                    text = chunk.decode("utf-8", errors="replace")
                    fh.write(text)
                    fh.flush()
                    sys.stdout.write(text)
                    sys.stdout.flush()
        except KeyboardInterrupt:
            pass
        finally:
            ser.close()

    print(f"\n[serial_capture] wrote {out_path} ({duration}s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
