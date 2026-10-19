# xiaozhi 语音机器人固件（KT0366 / KT0367）

> 本文记录这块 2.8" ESP32-S3 屏从「媒体控制器」切到 xiaozhi 机器人的完整过程：
> 厂商 1.7.6 跑通 → 配网激活 → 升级上游 v2.5.2 的调研与操作。
> 决策摘要见 `agents/DECISIONS.md` D26。

## 0. 设备基线

| 项 | 值 |
|---|---|
| 板卡 | 2.8inch ESP32-S3 Display（KT0366 无触摸 / KT0367 带触摸） |
| 芯片 | ESP32-S3，16MB QSPI Flash + 8MB OPI PSRAM |
| Flash 实测 | 16MB（esptool `flash-id`: manufacturer `5e`, device `4018`） |
| 设备 MAC（STA） | `b8:1f:3f:c3:b6:c0` |
| 串口 | `/dev/cu.usbmodem31101`（USB CDC / HWCDC） |
| 工具 | esptool v5.6.0（`/tmp/espenv3/bin/esptool`） |
| 当前固件 | **自定义 v2.5.2**（头像 + 5 行对话，2026-10-18 16:50 实刷；厂商 1.7.6 为回滚锚点） |

## 1. 板型对应关系（关键结论）

上游 `78/xiaozhi-esp32` 的板型 **`freenove-esp32s3-display-2.8-lcd`** 与本板是同一套硬件设计：

- 上游 `ReadMe.md`："Likely the same hardware design as **LCD wiki ES3C28P/ES3N28P**"
- `config.h` 与实测引脚逐项吻合：

| 功能 | GPIO |
|---|---|
| 背光 BL | 45 |
| LCD SCK / DC / CS / MOSI / MISO | 12 / 46 / 10 / 11 / 13 |
| LCD RST | NC（与 EN 共用） |
| 触摸 I2C | SCL 15 / SDA 16，FT6336 @ `0x38` |
| I2S | MCLK 4 / BCLK 5 / DIN 6 / WS 7 / DOUT 8 |
| 功放 PA | 1 |
| 音频编解码 | ES8311 |
| RGB | 42 |
| BOOT | 0 |
| SD | CLK 38 / CMD 40 / D0-D3 39,41,48,47 |

## 2. 厂商 1.7.6（已跑通，回滚锚点）

- 文件：`_shared/vendor/KT0366&KT0367/.../8-快速使用_Quick_Start/bin/xiaozhi/common.bin`
- 大小/sha256：`5,240,080 B` / `fb21f8c3f44e757faaaa39d2f2eb517adad269949ce8e591e00de4d8ff668da9`
- 构建：xiaozhi `1.7.6`，ESP-IDF `v5.4.2`，16MB / DIO / 80MHz
- 镜像结构：`0x0` bootloader，`0x8000` 分区表，`0x100000` app（魔数 `E9`）
- 分区：`nvs 0x9000(16K)` / `otadata 0xD000(8K)` / `phy_init 0xF000(4K)` / `model 0x10000(960K)` / `ota_0 0x100000(6M)` / `ota_1 0x700000(6M)`
- 烧录（已验证 Hash of data verified）：

```bash
/tmp/espenv3/bin/esptool --chip esp32s3 -p /dev/cu.usbmodem31101 -b 921600 \
  --before default_reset --after hard_reset \
  write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x0 common.bin
```

- 启动实证：`App version: 1.7.6` / `LcdDisplay: Turning display on` / `Es8311AudioCodec initialized` / `WifiConfigurationAp: SSID Xiaozhi-B6C1` / `Web server started` / `Alert 配网模式`

## 3. v2.5.2（上游预编译）

- 来源：GitHub release `78/xiaozhi-esp32` **v2.5.2**（2026-10-11，commit `e0d2390a`）
- 资产：`v2.5.2_freenove-esp32s3-display-2.8-lcd.zip`（2,734,808 B，sha256 `e3d429d3ef46d5eea896d9d4afddc1efbee18e6c9aa4e33809698f8a4066d657`）
- 解压后：`merged-binary.bin`，10,052,777 B，sha256 `547d79fccf3456c2fd267490b92e8d37b7eddc1128970786965069af521d29c4`
- 构建：xiaozhi `2.5.2`，**ESP-IDF v6.1**，2026-10-11 18:52 编译
- 镜像结构：`0x0` bootloader，`0x8000` 分区表，**`0x20000` app**，`0x800000` assets
- 分区：`nvs 0x9000(16K)` / `otadata 0xD000(8K)` / `phy_init 0xF000(4K)` / `ota_0 0x20000(4M)` / `ota_1 0x410000(4M)` / **`assets 0x800000(8M)`**

### 为什么必须整片刷、且要擦除

1. **分区表变了**：app 从 `0x100000` 挪到 `0x20000`，新增 8MB `assets`。OTA 只能替换 app，改不了分区表 ⇒ 只能 USB 线刷 `0x0`。
2. **镜像只写到 `0x9964A9`**，assets 分区尾部还有 **6.41MB** 未被覆盖；那里面是 1.7.6 的旧数据（旧 `ota_1` 区域）。assets 虽是按索引表读取、尾部不参与解析，但为了干净必须 `--erase-all`。

### 刷机命令

```bash
cd /Users/qiqi/open/github/eps32/_archives
unzip -o v2.5.2_freenove-esp32s3-display-2.8-lcd.zip -d /tmp/xz252

/tmp/espenv3/bin/esptool --chip esp32s3 -p /dev/cu.usbmodem31101 -b 921600 \
  --before default_reset --after hard_reset \
  write-flash --erase-all --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x0 /tmp/xz252/merged-binary.bin
```

### 刷完的既定后果

| 项 | 结果 |
|---|---|
| WiFi | nvs 清空 → 回到 `Xiaozhi-XXXX` 热点，需重配 |
| 音量 | 回默认 70 |
| 激活/绑定 | 设备 ID = STA MAC 不变 ⇒ 服务器端大概率保留；最坏重输一次 6 位码 |
| 首次启动 | assets 初始化，比平时慢几秒 |
| 控制台型号 | board type 变为 `freenove-esp32s3-display-2.8-lcd`，智能体配置可能需重选 |

## 4. 配网 + 激活流程（已实测）

### 4.1 配网

1. 手机/电脑连接设备热点 `Xiaozhi-XXXX`（无密码）
2. 浏览器打开 `http://192.168.4.1`
3. 选择家里 WiFi、填密码、保存
4. 设备重启并连上 WiFi

### 4.2 激活

1. 设备连网后会请求 `https://api.tenclass.net/xiaozhi/ota/`，服务器返回 `activation`
2. 屏幕显示 **「激活设备 / xiaozhi.me」**；**6 位激活码不在屏幕上，是逐位语音播报的**
3. 打开控制台 **https://xiaozhi.me/console/**（已验证 200）
4. 登录（手机号 + 短信验证码，或微信扫码）
5. 「设备管理」→「添加设备」→ 输入听到的 6 位码
6. 设备每 3 秒轮询一次 `/ota/activate`（日志 `Activating... n/10`），成功后打印 `Activation successful` 并进入待机

> 错过语音：拔插 USB 重启，会重新播报一遍。

## 5. 音量控制

### 5.1 固件内置能力（1.7.6 与 v2.5.2 都有）

- MCP 工具：`self.audio_speaker.set_volume`，参数 **`volume`：0–100**
- 查询工具：`self.get_device_status`（返回当前音量）
- 默认值：**70**
- 持久化：写入 NVS `Settings("audio") → output_volume`，**重启保留**
- 为 0 时：屏幕状态栏显示静音图标（`MATERIAL_SYMBOLS_VOLUME_OFF`）
- 硬件路径：`esp_codec_dev_set_out_vol()` → ES8311

### 5.2 怎么用

直接语音：

- 「小智，音量调到 30」
- 「声音小一点」/「声音大一点」
- 「音量调到最大」
- 「静音」
- 「现在音量多少？」

原理：服务端 AI 识别意图 → 调用设备注册的 MCP 工具 → 写 ES8311。

### 5.3 没有物理音量键

- 本板只有 **BOOT 键（IO0）**：单击 = 开始/停止对话；启动阶段 = 进配网
- 想要物理音量键或屏幕滑块 ⇒ 需源码编译（ESP-IDF 6.1）自行添加

### 5.4 语音调不动的排查

1. 控制台里绑定的智能体需支持**工具调用（MCP / function call）**
2. 说具体数字比「大声点」可靠
3. 可在智能体提示词里加：「用户要求调音量时调用 `self.audio_speaker.set_volume`」

## 6. 回滚（三选一）

| 回到 | 操作 |
|---|---|
| 厂商 1.7.6 | 重烧 `common.bin` @ `0x0`（见 §2） |
| 媒体控制器 v4 | `cd /Users/qiqi/open/github/eps32 && ./build.sh upload` |
| 出厂原样 | `_shared/factory/esp32s3-original-20261017.bin` @ `0x0` |

## 7. 版本对照速查

| | 1.7.6（厂商） | 2.5.2（上游） |
|---|---|---|
| IDF | v5.4.2 | v6.1 |
| app 偏移 | `0x100000` | `0x20000` |
| 资源 | `model` 960KB | `assets` 8MB |
| 刷法 | `0x0` 整片 | `0x0` 整片 + `--erase-all` |
| 板型名 | 厂商自有 | `freenove-esp32s3-display-2.8-lcd` |

## 8. 坑与注意

- **不要只写 app 段**：分区表不同，只写 app 起不来。
- **不要用 OTA 跨这两个版本**：分区表无法 OTA 更新。
- **`--erase-all` 不会动 eFuse/MAC**，绑定身份不变。
- 厂商 `wechat.bin` 与 `common.bin` 只差编译期配置（`model` 分区逐字节相同），优先 `common.bin`。
- macOS 无 `timeout`、无 `pyserial`；串口抓日志用 `cat … & CP=$!; sleep N; kill $CP`。
- `look_at` 不支持 PDF，读 PDF 用 `pip install --target /tmp/pylibs pypdf`。

## 9. 自定义界面：竖屏 + 头像 + 5 行分页对话（源码构建，2026-10-18）

上游 v2.5.2 的默认 UI 是「滚动聊天记录 + 顶部状态栏 + 按钮」。现在改为竖屏单页 + 分页历史。

> **画布方向（关键）**：这块板最初是横屏 320×240；现在按**竖屏 240×320** 显示，充电口朝下。
> 板级配置：`DISPLAY_WIDTH=240 / DISPLAY_HEIGHT=320 / SWAP_XY=false / MIRROR_X=true / MIRROR_Y=false`；
> 触摸同步 `swap_xy=false, mirror_x=true, mirror_y=false`（`x_max=240, y_max=320` 保持 raw 值）。
> 启动日志 `Layout canvas: 240x320` 可确认方向。

| 元素 | 位置/规格 | 说明 |
|---|---|---|
| 顶栏 | 高 = 当前字体行高（28/31px）；左 WiFi、右 静音+电量 | 图标由 `UpdateStatusBar()` 驱动 |
| 顶栏中间 | 宽 220px | **空闲显示 HH:MM 时钟；对话时显示状态**（聆听中/说话中/连接中）；翻页时临时显示 `页码/总页数`（1.2s 后恢复时钟） |
| 头像 | **80×80 程序化圆脸**（白色圆 + 两条深色竖条眼，深色主题下带微弱光晕），水平居中，顶栏下 5px（y≈36~116） | **单击 = 继续对话**（历史保留、跳回最新页）；**按住 2 秒 = 新会话**（清空屏幕即反馈）；按住时显示 2px、40% 透明的细环 |
| 唤醒词 | 「你好小智」在待机时检测到 = 同样开新会话 | 先关旧会话、清历史，延迟 300ms 再开新会话（避免 goodbye 未完成时重连失败） |
| 对话区 | 宽 224px（左右各 8px），高 = 6 行 × 27px ≈ 158px，y≈139~297 | **18px** 字体（`font_chat_common_18_4.bin`，行高 23 + 行距 4），单行 ≈ 12 汉字，一页 ≈ 72 字 |
| 翻页 | **点文字区左半 = 下一页（更新），右半 = 上一页（更早）** | 顶栏临时显示 `3/12` 页码；到边界不循环 |
| 分页规则 | **一页一条消息**；一条消息超过 5 行自动拆成多页 | 每条消息第一页带角色前缀「你：」「小智：」；正文限长 1000 字节，历史最多 40 条（RAM，重启清空） |
| 新消息 | 任何新消息/新句子**自动跳回最新页**并按正常流程显示 | 若当时正在回顾旧页，跳回时顶栏提示「新消息」 |
| 系统通知 | 不进入历史 | 连接/OTA/音量等 `system` 文本只在顶栏临时显示 3 秒 |
| 滚动条 | 屏幕 / 背景容器去掉 `SCROLLABLE`；所有固定控件 `SCROLLBAR_MODE_OFF` | 固定页面不允许出现任何滚动条 |

- 已删除：说话/新会话/翻页按钮、确认弹窗；`KeepsChatHistory()` 为 true，待机不再清屏。
- 手势技术点：全局长按时间设为 2000ms；`LV_EVENT_LONG_PRESSED` 触发后 2 秒时立即清屏（新会话），并用 `lv_indev_wait_release()` 吞掉松手补发的 CLICKED；新会话先关旧 MQTT 会话再清历史，并用 300ms 一次性定时器延迟重开（实测立即重开会 `connecting → idle` 失败）。
- 18px 字体来源：`main/assets/chat-font/font_chat_common_18_4.bin`（Noto Sans CJK SC，18px/4bpp，common 字符集 6452 字，缺字回退到 20px 主题字体），由 assets 分区随固件一起烧录。
- 字体：改用**主题文本字体**（assets 挂载后为 `font_noto_sans_common_20_4.bin`，20px/6845 字），换字体/换主题时重新分页。
- 主题：**深色**（黑底白字）。板级启动时用 NVS 键 `dark_default` 保证首次刷入即深色，之后手动切换主题仍会保留。
- 布局预览图：`docs/ui-layout-portrait-20261018.png`（240×320 深色竖屏近似渲染，非实机截图）。
- 待机视线动画预览：`docs/avatar-idle-gaze.gif`、关键帧 `docs/avatar-idle-gaze-frames.png`；
- 假 3D 透视版预览：`docs/avatar-persp-gaze.gif`、关键帧 `docs/avatar-persp-frames.png`。

### 9.4 头像程序动画（2026-10-18）

头像不再是静止图：LVGL 定时器以 25fps 驱动 `emoji_box_` 的变换，不依赖任何新素材。

| 状态/情绪 | 动画 |
|---|---|
| 待机 | 缓慢呼吸（缩放 256±8，4 秒周期）+ 每 2.6~6.1 秒眨一次眼（纵向压扁到 25%，140ms） |
| 聆听 | 呼吸加快加深（2.4 秒周期，±10），提示"在听你说" |
| 说话 | 轻微上下浮动（≈2px，0.6 秒周期）+ 轻呼吸 |
| 连接/等待回复 | 缓慢左右微转（±4.5°，3 秒周期） |
| 预览图片/OTA | 头像隐藏时自动跳过更新 |

- 实现：`Display::SetAvatarMood()`（idle/listening/speaking/thinking）由 `application.cc` 状态机驱动；`LcdDisplay::AvatarAnimTimerCb` 每 40ms 计算呼吸+眨眼+状态位移/旋转，写入 `transform_scale_x/y`、`translate_y`、`transform_rotation`。
- 服务端 `emotion` 表情仍走原 `SetEmotion()`：换图后动画继续作用在整个头像外框上（缩放/眨眼同样生效），两者不冲突。

### 9.5 程序化圆脸头像（2026-10-18）

头像不再是图片，而是 LVGL 对象实时绘制的圆脸（与用户提供的参考图一致：白色圆脸 + 两条竖条圆角眼，无嘴）：

- 结构：`emoji_box_`（80px，可点击/长按）→ `avatar_face_` 白色圆形 → 两条 `avatar_eye_*` 圆角竖条（8×22，深色）；开心时换成两条 `lv_arc` 弧线；思考时右侧出现 `···`。
- 核心表情（当前实现）：待机、眨眼、聆听、说话、思考、开心。
  - 待机（位置驱动的假 3D）：每 1.5~3.1 秒挑一个新视线目标（八个方向之一）；眼睛大小随位置变化——**往下最大（+16%）、往上偏小（-16%）、往左右两侧偏小（-10%）、中间最大**；两只眼的大小差只在转向两侧时出现（最大 ±10%），且**靠转向侧边缘的那只眼变小**；眼距随侧看轻微压缩（-10%）；整脸同步平移、呼吸 ±4%，每 2.6~6.1 秒眨眼（12% 概率单眼眨），观感像"头在左右摇、上下看"；
  - 闭眼 / 单眼闭：服务端 `sleepy` → 闭眼（两条细横线），`winking` → 单眼闭（一侧竖条 + 一侧细线），2.5 秒后回到状态表情；
  - 聆听：同样的视线系统但眼睛更窄、呼吸更快（±12%）；
  - 说话：眼睛随句子节奏眨动、轻微上下浮动、头部轻摆；
  - 思考：视线固定右上、右侧 `···`、头部慢转；
  - 开心：眼睛变 `∩ ∩`、跳动、头部摆动；
- 状态来源：`Application` 状态机 → `SetAvatarMood()`；服务端 `emotion` → `SetAvatarEmotion()` 覆盖 2.5 秒后回到状态表情（happy/laughing/funny…→开心；thinking/confused→思考；neutral 清除覆盖；其余暂忽略）。
- 全部为几何对象，无素材、任意尺寸清晰；动画由 40ms LVGL 定时器驱动（25fps）。

### 9.6 媒体按键界面 + BLE HID（4×4，2026-10-18）

- 待机：底部键盘图标进入；聊天 5 行；对话中 6 行。
- 媒体页 **4×4 共 16 键**（键 52×52）：
  - **媒体键 3×2**：第 1 行 上一首 / 播放暂停 / 下一首；第 2 行 静音 / 音量− / 音量+
  - 第 4 列：调度 / 切换
  - 第 3 行：锁屏 / 截图 / 聚焦 / 全屏
  - 第 4 行：关闭 / 桌面 / 复制 / 粘贴
- 6 个媒体键走原有 HID 消费者报告（Mac 已验证可用，不改动）；后 10 个走**键盘报告发 macOS 快捷键**：
  - 调度 Ctrl+↑、切换 Cmd+Tab、锁屏 Ctrl+Cmd+Q、截图 Cmd+Shift+3、聚焦 Cmd+Space、全屏 Ctrl+Cmd+F、关闭 Cmd+W、桌面 F11、复制 Cmd+C、粘贴 Cmd+V
- 按键时顶部标题临时显示按键名，900ms 后恢复"媒 体"。
- 右上蓝牙图标：灰色=未连接/广播中，绿色=已连接。
- 已配对设备页、蓝牙清空/断开保持不变。
- 游戏「数字大炮」已拆分为独立工程：`qiqigame/esp32-math-cannon/`（不包含在小智固件里），小智固件已回退到归档版本。
- **归档快照**：`snapshots/20261018-xiaozhi-media-final/`（固件整片/app、源码打包、sdkconfig、说明）；同时打了 tag `media-final-20261018`。

### 9.1 编译（需要 Python 3.12 的 IDF venv）

```bash
cd src/xiaozhi-esp32
export PATH="$HOME/.espressif/python_env/idf6.1_py3.12_env/bin:$PATH"  # 关键：否则 export.sh 报 venv 不存在
source ~/esp/esp-idf/export.sh
idf.py build                      # 或 python scripts/build.py freenove-esp32s3-display-2.8-lcd
```

### 9.2 烧录（不擦 NVS，保留 WiFi 与激活）

```bash
cd src/xiaozhi-esp32/build
/tmp/espenv3/bin/esptool --chip esp32s3 -p /dev/cu.usbmodem31101 -b 921600 \
  --before default-reset --after hard-reset \
  write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB "@flash_args"
```

- `build/flash_args` 已含全部 5 个镜像及偏移（bootloader/分区表/otadata/**assets 0x800000**/app 0x20000）。
- **不要 `--erase-all`**：分区表没变，擦除只会白丢 WiFi/激活。
- 只改了 app 时也可只写 `0x20000 xiaozhi.bin`；改了字体/表情等 assets 内容必须连 `0x800000 generated_assets.bin` 一起写。
- 本次改动前的分页 UI 工作区备份：`backups/20261018-1630-chat-paged-wip.patch` 与同名 `.cc/.h`。

### 9.3 分支保存（源码在 `src/xiaozhi-esp32`，三分支同源 v2.5.2，分区表一致可互刷）

| 分支 | commit | 界面版本 |
|---|---|---|
| `avatar-5line` | `961b42f` | **当前实刷**：头像居中 + 5 行文本单页（无按钮/无历史/无滚动） |
| `chat-paged` | `696dfa3` | 分页聊天：单页一条消息 + ◀ 页码 ▶ 翻页 + 「新会话」确认弹窗（改动前未提交的工作区状态，字节级还原） |
| `chat-history` | `c677807` | 多行滚动气泡 + 常驻聊天历史 + 顶部 28px 状态栏 + 说话/新会话按钮（最早提交的历史版） |

还原到任一版本（源码分支，需要重新编译 + 烧录）：

```bash
cd src/xiaozhi-esp32
git checkout chat-history          # 或 chat-paged / avatar-5line
export PATH="$HOME/.espressif/python_env/idf6.1_py3.12_env/bin:$PATH"
source ~/esp/esp-idf/export.sh && idf.py build
cd build && /tmp/espenv3/bin/esptool --chip esp32s3 -p /dev/cu.usbmodem31101 -b 921600 \
  --before default-reset --after hard-reset \
  write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB "@flash_args"
```

> 注意：切换分支后 `build/` 里的固件属于**别的版本**，务必重新 `idf.py build` 再刷。
> `avatar-5line` 的预编译整片镜像在 `firmware/custom-avatar-5line-20261018.bin`，可直接 `./flash.sh custom`；
> 另两个分支目前只有源码，没有预编译镜像（需要时按上面命令构建）。



## 固件版本（两个保存版本，2026-10-18）

- **定制完善版**：`snapshots/20261018-xiaozhi-custom-final/`（tag `custom-final-20261018`，commit `682392c`）
  - 竖屏 UI、圆脸、6 行分页、BLE 媒体键/快捷键、大脸待机、番茄钟睡眠（待机 20 分钟→睡 5 分钟→自动醒）
  - 镜像：`firmware/xiaozhi-custom-final.bin`（整片）/`xiaozhi-app-only.bin`（应用区）
- **官方版**：`snapshots/20261018-xiaozhi-official-v2.5.2/`（上游 tag `v2.5.2`，commit `49b4380`）
  - 官方原版界面，无自定义功能，用于对照/回退
- 切换建议：只刷 `0x20000` 应用区（保留 NVS/WiFi/激活）；整片 `0x0` 写入会覆盖 NVS
- 版本总览：`snapshots/README.md`；历史早期版本在 `snapshots/_archive/`

## 10. 生字笔顺学习工具（TF 卡 + MCP 语音，2026-10-19）

数据管线 `tools/stroke-pipeline/`（hanzi-writer-data 2.0.0，2972 字 / 一至六年级；笔序权威，
笔画名近似 ~75-85% 为已接受取舍）。固件：SD 挂载（SDMMC 4-bit 优先，D3 兼作 SDSPI 回退 CS，
60s 热插拔重试）+ `display/stroke_tool.{h,cc}` 数据层 + 笔顺页 UI（逐笔动画、25fps、序号圆标、
示例底字、拼音/笔画信息栏、播放完停 4s 自动下一字）+ 右下角「笔」入口（待机时显示）。

### 10.1 TF 卡准备

- 把 `tools/stroke-pipeline/sdcard/` 里全部文件拷到一张 **FAT32** 卡根目录（卡 ≤32G，注意 exFAT 不挂载）：

  ```
  /sdcard/szver.txt index.bin medians.bin pinyin.json NOTICE.txt glyphs/g1..6.bin   # 共 2.7MB
  ```

### 10.2 使用

- 点右下角「笔」进入；顶栏「年级·笔顺」、拼音、当前笔画信息；底部 ⟳ 重播 / ◀ / ▶（年级内循环）；
- 语音（MCP，`freenove-esp32s3-display-2.8-lcd.cc` InitializeTools）：
  - `self.stroke.show {char}` —「antoñ字怎么写」自动跳转并播报拼音/笔数/笔画名
  - `self.stroke.next {dir:-1|1}` — 翻字
  - `self.stroke.close` — 退出笔顺页
- SD 未插/未挂载：入口与工具自检返回「字库还没准备好」，60s 后自动重试挂载。

### 10.3 编译状态与验证

- `python scripts/build.py freenove-esp32s3-display-2.8-lcd` BUILD_EXIT=0；app `0x2f92e0`（app 分区余 24%）。
- 未经真机验证项：D1-D3 卡座走线（SDMMC 失败会自动降级 SDSPI）、exFAT 卡提示、笔画名抽样（火/女/出/万/张）。

## 2026-10-20 更新：设置页、笔顺学习、交互打磨

### 设置页
- 待机左下角齿轮进入，包含：音量 −/+（每次 5，持久化）、亮度 −/+（每次 10）、WiFi 状态 + 重新配网、蓝牙断开/清空、重启设备
- 所有标签在 `SetTheme` 开头重绑，避免主题刷新字体指针崩溃

### 笔顺学习（TF 卡数据）
- 入口：待机右下角"笔"（数据就绪时显示）；语音"写一下学字"（MCP `self.stroke.show`）
- 数据：`tools/stroke-pipeline/` 生成，9470 字；卡上根目录放 `szver.txt/index.bin/medians.bin/pinyin.json/glyphs/`
- UI：顶部仅拼音，中间 176px 画布，底部 重播/上一个/下一个，右上角关闭
- 动画：单笔 0.09~0.3 秒，笔触半粗；画完停 2.5 秒重播同一个字（不自动跳字）
- 退出：用户新语音（STT）/ 唤醒词 / 会话结束（拜拜）/ 手动关闭 / 5 分钟无操作

### 触摸与交互
- 所有按钮改按下触发（`LV_EVENT_PRESSED`）并扩大 8–12px 热区

### 睡眠周期
- 待机 20 分钟无操作 → 睡觉（横线闭眼 + Zzz + 点头）5 分钟 → 自动醒；无呼吸音

### 经验教训
- 详见 `docs/lessons-20261020.md`（字体指针、坏卡假写、dd+fsync、字模裁切、自动聆听状态机、NVS 磨损等 9 条）
