# 音乐播放器功能包

在设备本地播放 SD 卡里的音乐，不经过服务器：

- 索引 `/sdcard/music`（最多 3 层子目录、1000 首），读取 ID3v1/ID3v2、FLAC、
  WAV、M4A、ADTS 元数据并缓存到 `/sdcard/music/.music_index.json`
- 用 `esp_audio_codec` 解码 MP3/AAC/M4A/FLAC/WAV/OGG，下混单声道后按声卡输出
  采样率重采样，再交给 `AudioService` 的播放队列
- 列表翻页：标题右侧 `‹ 1/12 ›`，两个 39 px 点击区延伸到中间页码，点页码左半
  为上一页、右半为下一页；首页/末页箭头置灰，只有一页时整组隐藏
- 抽屉新增“音乐”入口：iPod 风格列表（Now Playing / Shuffle All / Songs /
  Artists / Albums）、播放进度、上一首/播放暂停/下一首、长按中键开始对话
- 语音工具 `self.music.play/pause/resume/stop/next/previous/status/rescan`
- 索引自动失效：整棵目录树的签名（子目录、文件名、大小、mtime）变化时重新扫描；
  音乐页打开时和每 30 秒各检查一次，说“重新扫描歌曲”可立即触发 `self.music.rescan`
- 唤醒或开始说话时自动暂停，回到 Idle 后 600 ms 自动续播；对话中发起的
  “播放”请求会排队到回复结束后开始

## 音乐模式（语音流程）

`self.music.play` 成功后由 `Application` 进入“音乐模式”，页面被固定：

1. 点播成功 → 自动打开音乐页；助手回复（TTS）结束、设备回到 Idle 后**立即开始播放**
   （不再等下一轮 Idle，也不会被连续聆听一直暂停）
2. 播放中唤醒词**不关闭**音乐页，改为打开 5 秒指令窗口
3. 窗口内音乐暂停（本板无硬件回采，AEC 关闭，播放会串进麦克风），可以连续点歌、
   上一首/下一首、暂停/继续；指令的 TTS 结束后再给 5 秒
4. 5 秒内没有指令：自动退出聆听，音乐继续、页面保留；再次控制需要先说唤醒词
5. 只有 `self.music.stop`（“退出听歌”这类指令）才关闭页面并停止播放

实现位置：`Application::EnterMusicMode/LeaveMusicMode/ExtendMusicCommandWindow`、
`MediaPlayer::SetCommandWindowActive`、`Display::ShowMusicPlayer`。

## 开关

```text
CONFIG_FEATURE_MUSIC_PLAYER=y
```

默认 `n`。关闭后不编译音乐库、播放器、音乐页与 MCP 工具，也不链接本地解码器；
抽屉入口保留为不可用状态（提示“音乐功能未启用”）。

## 文件

- `music_library.*`：SD 卡扫描、标签/时长解析、索引缓存、检索与最近播放
- `media_player.*`：解码任务、重采样、分块推入播放队列、状态机与 MCP JSON 输出

## SD 卡布局

```text
/sdcard/
├── Song.mp3               # 卡根目录的歌曲也会被收录
├── music/                 # 推荐目录（最多 3 层子目录）
│   ├── Artist - Title.mp3
│   └── Album/01 - Track.flac
└── .music_index           # 自动生成的索引，不要手工编辑
```

只扫描 `/sdcard/music` 整棵树，以及卡根目录下的音频文件；不会递归卡根目录的其它
文件夹（避免把英语/笔顺音频当成歌曲）。

新增/删除歌曲后首次打开音乐页或播放会自动重新扫描（对比整棵目录树的签名）；扫描在后台任务
`music_scan` 中进行，期间列表显示 “Scanning SD card...”。

## 构建

```bash
python3 scripts/feature_build.py --profile music
python3 scripts/feature_build.py --profile full
```

## 验证要点

- 关闭侧（`base`）无 `MediaPlayer`/`MusicLibrary` 符号，抽屉入口置灰
- 开启侧（`music`/`full`）符号存在，`self.music.status` 返回 `ok:true`
- 真机：点播后自动开页并播放、唤醒不关页、5 秒窗口内切歌、超时后音乐继续、
  “退出听歌”关页并停止、无卡/空目录提示

## 限制

- 只做顺序播放（可洗牌），不做拖动定位与循环单曲
- 索引首次扫描需要读取每个文件的标签头，曲目很多时会持续数秒
- 语音“播放”在索引尚未建立时会等待扫描完成再响应，冷启动首次调用可能较慢
