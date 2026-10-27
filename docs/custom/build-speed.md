# 构建提速与跨平台说明

记录 2026-10-27 对本项目构建流程的排查与优化，以及 macOS 上的构建方式。
适用于 ESP-IDF v6.1 + `scripts/feature_build.py`。

## 一、结论先行

构建慢有三个独立原因，按影响从大到小：

| # | 原因 | 影响 | 处理 |
|---|---|---|---|
| 1 | `feature_build.py` 每次都重写 `sdkconfig`，mtime 变化触发 cmake reconfigure → 整仓重编 | 每次构建 **6 分钟**，即使一行代码没改 | 已修，见下 |
| 2 | 构建输出交给终端/工具捕获通道，量大后背压，堵死 ninja→ccache→gcc 整条链 | 构建随机**卡死**（CPU 全 0、无文件写入） | 输出重定向到文件，见下 |
| 3 | build 目录在机械盘（D: 是 2TB HDD），上万个小文件随机读写 | 全量构建偏慢 | build 目录指向 SSD |

> 注意：卡死与磁盘无关。SSD 上同样会卡，因为它是**管道背压**，不是 I/O 瓶颈。

## 二、已修：重复构建不再全量重编

**症状**：改完代码跑构建要 6 分钟；切回同一个 profile 也一样慢。

**原因**：`prepare_config()` 无条件 `write_text()`。只要写了文件，mtime 就变，
cmake 判定 `sdkconfig` 变化 → reconfigure → 所有包含 `sdkconfig.h` 的编译单元全部重编。
而 Kconfig 规范化本身也会改写文件，于是每次构建都在自我触发。

**修复**：内容比对，一致就不写。

```python
if output.is_file() and output.read_text(encoding="utf-8") == text:
    return False
output.write_text(text, encoding="utf-8")
```

**实测**：同一条命令连跑两次，第二次 **4.2 秒**（原 6 分 6 秒）。

## 三、已修：构建卡死的规避方式

**症状**：ninja、ccache、xtensa-gcc 全部 CPU 归零，build 目录几分钟无新文件，
偶尔残留 `.ninja_lock`。卡住的文件可能是极小的源文件（如 `lwip/src/core/stats.c`），
说明 gcc 不是在算，是在等。

**根因**：构建输出量很大（cmake 配置日志 + 1686 个编译任务），当输出经由
PowerShell 管道 / 工具捕获通道搬运时，消费者处理不过来形成背压，
ninja 写阻塞 → ccache 转发阻塞 → gcc 写 stderr 阻塞 → 整链冻死。

**规避**：让输出直接落文件，不经管道。

```powershell
Set-Location C:\esp-build\media
ninja -j 6 *> out.log
```

实测同样条件下，重定向到文件后 50 秒推进 20 个编译单元，全程无卡死。

**卡死后恢复**（进度不丢）：

```powershell
Get-Process | Where-Object { $_.ProcessName -match 'ninja|xtensa|ccache' } |
  ForEach-Object { Stop-Process -Id $_.Id -Force }
Get-ChildItem C:\esp-build\media -Filter ".ninja_lock" -Recurse -Force |
  ForEach-Object { Remove-Item $_.FullName -Force }
# 重新跑 ninja 即可，已完成的 .obj 会跳过
```

## 四、已改：build 目录可指向 SSD

本机 D 盘是 2TB 机械盘，仓库在 `D:\GITHUB\EPS32`，build 产物 5.5 GB / 3.6 万文件。
`feature_build.py` 新增环境变量支持：

```powershell
[Environment]::SetEnvironmentVariable("XIAOZHI_BUILD_ROOT", "C:\esp-build", "User")
```

优先级：`--build-dir` > `$XIAOZHI_BUILD_ROOT/<profile>` > `build/profiles/<profile>`。

新增 `--jobs`：

```powershell
python scripts\feature_build.py --profile media --jobs 6
```

`idf.py` 自己不认 `-j`，该参数通过 `CMAKE_BUILD_PARALLEL_LEVEL` 透传给 ninja。

并行度经验：`-j 6` 稳定；`-j 8` 及以上本机偶发僵死，不建议。

## 五、实测数据

| 场景 | 优化前 | 优化后 |
|---|---|---|
| 重复构建（源码无改动） | 6 分 06 秒 | **4.2 秒** |
| cmake 配置阶段（新 build 目录，一次性） | — | 147 秒（135.7 配置 + 11.6 生成） |
| 全量编译（1686 个目标，`-j 4`） | 11 分 43 秒（历史记录） | 见下 |
| 剩余 254 个目标续跑（`-j 6`，输出落文件） | — | 4 分 19 秒，无卡死 |

147 秒的配置阶段无法避免：176 个组件 + 74 个托管依赖要跑 Kconfig、ldgen、
资源打包。**只要 build 目录不被删，这笔钱只付一次**——这也是不要动不动 `fullclean` 的原因。

另外 ccache 上限从默认 5 GB 提到 25 GB：

```bash
ccache --set-config=max_size=25G
ccache --set-config=sloppiness=file_stat_matches,time_macros
```

11 个 profile 的编译产物都要驻留，5 GB 会互相挤掉，切 profile 时命中率骤降。

## 六、macOS 上怎么构建

**命令完全一样。** 这个仓库没有任何 Windows 绑定：`feature_build.py` 只用
`pathlib`/`shutil`/`subprocess`，`sdkconfig` 里没有绝对路径，CI 脚本本来就是
`source export.sh` 的写法。

差异只有三处：

| 项目 | Windows | macOS |
|---|---|---|
| 激活环境 | `. D:\esp-idf\export.ps1` | `. ~/esp/esp-idf/export.sh` |
| 串口名 | `COM4` | `/dev/cu.usbmodem*` |
| 构建盘 | D 盘是 HDD，需指向 SSD | 内置 SSD，无需处理 |

### 一次性准备

```bash
# 依赖
brew install cmake ninja dfu-util ccache python3

# ESP-IDF v6.1（与 Windows 必须同版本）
git clone -b v6.1 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
cd ~/esp/esp-idf && ./install.sh esp32s3

# ccache
ccache --set-config=max_size=25G
```

### 每次构建

```bash
. ~/esp/esp-idf/export.sh
cd ~/path/to/EPS32
python scripts/feature_build.py --profile media --jobs $(sysctl -n hw.ncpu)
```

### 烧录

```bash
# 查端口（ESP32-S3 原生 USB 一般是 cu.usbmodem 开头）
ls /dev/cu.*

# 只写 app，保留 NVS（分区表必须是 16m_app6m）
python -m esptool --chip esp32s3 -p /dev/cu.usbmodem1101 -b 460800 \
  write-flash 0x20000 build/profiles/media/xiaozhi.bin
```

Apple Silicon（M 系列）单核性能明显强于这台 i7-11700，全量构建会更快；
但 **ccache 缓存不能跨平台/跨机器复用**（编译器二进制不同），
Mac 上第一次构建必须是全量。

### Mac 上不需要做的事

- 不需要 `XIAOZHI_BUILD_ROOT`，build 目录留在仓库内即可
- 不需要处理 `.ninja_lock` 卡死（那是 Windows 管道背压特有的）
- 不需要 `git config http.schannelCheckRevoke false`（那是 Windows 证书吊销检查的问题）
