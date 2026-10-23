# 2026-10-23 版本说明：本地音乐功能包 + 头像动画降频

本版把 SD 卡本地音乐作为第五个功能包加入组合构建，并针对实测中发现的
「空闲头像动画占 41% CPU」做了不改变样式的降频处理。

- 分支：`feat/music-player`
- 标签：`custom-final-20261023`
- 功能包拆分基线：`custom-feature-packs-20261023`
- 详细机制与数据：[`feature-packs.md`](feature-packs.md)

## 一、适用硬件与升级方式

- 板型：**Freenove ESP32-S3 Display 2.8"**（16 MB flash / 8 MB PSRAM，SD 4-bit SDMMC）
- 分区：沿用 `partitions/v2/16m_app6m.csv`，**本版不改分区**
- 从上一版升级（保留 NVS、Wi-Fi 凭据与蓝牙配对）：

```powershell
python -m esptool --chip esp32s3 -p COM4 -b 460800 write_flash `
  --flash_mode dio --flash_freq 80m --flash_size 16MB 0x20000 build\xiaozhi.bin
```

- 全新烧录（含 bootloader / 分区表 / assets）：

```powershell
python -m esptool --chip esp32s3 -p COM4 -b 460800 write_flash `
  --flash_mode dio --flash_freq 80m --flash_size 16MB `
  0x0 bootloader.bin 0x8000 partition-table.bin 0xd000 ota_data_initial.bin `
  0x20000 xiaozhi.bin 0xc00000 generated_assets.bin
```

- `xiaozhi.bin`：4,061,760 字节，SHA256
  `fa16f336dee437ffece8074a9d6b989541b122ec669edee292b593b665caede8`
- 仅音乐固件（`music` profile）：3,685,936 字节，SHA256
  `38288277dee8e9e7806f8da8f79e3e2391321b3fffa3ea968157a18d67692f8c`

## 二、新增功能：音乐播放器功能包

开关：`CONFIG_FEATURE_MUSIC_PLAYER`（默认 `n`，目标板与 `full` 启用）。
关闭后不编译音乐库、播放器、音乐页、解码器与 MCP 工具，抽屉入口保留并置灰。

- **曲库**：扫描 `/sdcard/music` 整棵树（≤3 层、1000 首）以及 SD 卡根目录的音频文件；
  不递归其它同级目录（避免把英语/笔顺音频当歌曲）
- **格式**：MP3 / AAC / M4A / FLAC / WAV / OGG，`esp_audio_codec` 解码后下混单声道、
  重采样到声卡输出率，再推入统一播放队列
- **元数据**：ID3v1/ID3v2、FLAC Vorbis comment、WAV/M4A/ADTS 时长
- **索引**：`/sdcard/.music_index`（版本 2）。整棵目录树签名（文件名/大小/mtime）变化时
  自动重建；打开音乐页、每 30 秒、每次点播前都会检查；也支持语音
  「重新扫描歌曲」→ `self.music.rescan`
- **页面**：iPod 风格列表（Now Playing / Shuffle All / Songs / Artists / Albums）、
  封面式播放页、滚轮控制、播放进度
- **翻页**：标题右侧 `‹ 1/12 ›`，两个 39 px 点击区延伸到中间页码（点页码左半=上一页、
  右半=下一页）；首页/末页箭头置灰；只有一页时整组隐藏；列表右缘有细滚动条
- **语音工具**：`self.music.play/pause/resume/stop/next/previous/status/rescan`
- **音乐模式**：
  1. 点播成功后自动打开音乐页；回复（TTS）结束立即开始播放，不再被连续聆听卡住
  2. 播放中唤醒词**不关闭**音乐页，改为打开 5 秒指令窗口
  3. 窗口内音乐暂停（本板无硬件回采、AEC 关闭，避免歌声串麦），可连续切歌；
     指令的 TTS 结束后再给 5 秒
  4. 5 秒无指令自动退出聆听，音乐继续、页面保留；再次控制需先说唤醒词
  5. 只有 `self.music.stop`（「退出听歌」）才关闭页面并停止播放

## 三、头像动画降频

头像动画保留原有全部动效，只调整帧率并去掉不可见时的重绘：

| 状态 | 之前 | 现在 |
|---|---|---|
| 空闲呼吸 | 25 fps（40 ms） | **15 fps**（66 ms） |
| 打瞌睡 | 25 fps | **10 fps**（100 ms） |
| 聆听/说话/思考/表情/眨眼 | 25 fps | 25 fps（不变） |
| 音乐/设置/网络/抽屉等整屏覆盖 | 仍重绘 | **停止重绘** |

实测依据：关闭动画后 `taskLVGL` 从 **41% → 0%**（IDLE 从 47% → 91%），
说明这 41% 基本全部来自头像重绘；按帧数比例，空闲 CPU 预计降到约 25%。

## 四、资源占用（full）

| 指标 | 数值 |
|---|---:|
| xiaozhi.bin | 4,061,760 B（app 分区余量 35%） |
| Flash Code / Data | 2,937,204 B / 973,464 B |
| 静态 DIRAM | 182,607 B（55.7%） |
| 内部 RAM（idle / 播放） | 54.8 KB / 52.4 KB 空闲 |
| 内部最大连续块 | 25.6 KB |
| PSRAM（idle / 播放） | 3.62 MB / 3.55 MB 空闲 |
| 音乐播放开销 | 内部 −2.4 KB，PSRAM −72 KB，CPU ≈10% |

组合体积：

| profile | bin | DIRAM |
|---|---:|---:|
| base | 3,116,176 B | 153,995 B |
| english | 3,191,952 B | 154,219 B |
| stroke | 3,167,216 B | 154,091 B |
| keyboard | 3,352,448 B | 178,043 B |
| web_file_transfer | 3,151,792 B | 154,043 B |
| music | 3,685,936 B | 158,207 B |
| full | 4,061,760 B | 182,607 B |

启动（INFO 日志，略高于发布版）：TF 卡 0.30 s、笔顺 0.52 s、词库 0.67 s、
LVGL 0.87 s、首页布局 0.96 s、BLE 广播 1.97 s、板级/音频完成 2.07 s。

## 五、验证记录

- 七个 profile 全部编译通过；`base/english/stroke/keyboard/web_file_transfer` 中
  `MediaPlayer`/`MusicLibrary` 符号为 0，`music/full` 各 85 个
- 主机测试：110 项中 97 通过、1 跳过，12 项为 Windows 缺少 `python3`/`cmake` 的
  `FileNotFoundError`（环境问题，与改动无关）
- 真机：`full` 刷入 `0x20000` 后启动正常、无 panic，唤醒词加载正常；
  播放 48 kHz 立体声 MP3、索引 9 首、切歌/暂停路径已通过临时诊断固件验证
- **未人工验证**：语音点播全流程（唤醒词开窗、5 秒超时、退出听歌关页）、
  翻页按钮与页码点击区、音乐页触摸操作

## 六、已知问题

1. `mbedtls_ssl_fetch_input error=76` / `SSL receive failed: -76` 仍为历史现象，不影响会话。
2. 无 AEC：5 秒指令窗口内音乐必须暂停，否则识别率会明显下降。
3. `taskLVGL` 栈余量仅 560 B；音乐包没有加深它，但后续加动效前需要留意。
4. 首次建立索引需要读每个文件的标签头，曲目很多时会持续数秒。
5. `full` 固件分区余量 2.1 MB，继续加解码器/资源前要核算。

## 七、回退点

| 标签 | 内容 |
|---|---|
| `custom-feature-packs-20261023` | 功能包拆分完成、无音乐包 |
| `mem-opt-20261022` | 内存/启动优化与文档 |
| `custom-final-20261022-r2` | 上一版发布固件 |
| `bt-switch-v4` | 蓝牙开关稳定版 |

纯固件回退：重新烧上一版 `xiaozhi.bin` 到 `0x20000`，NVS 与配对不受影响。
