# Windows 构建指南与提速记录

适用于 ESP-IDF v6.1 + `scripts/feature_build.py`。
macOS 侧由同事自行维护，本文不涉及。

## 一、日常只要一条命令

```powershell
cd D:\GITHUB\EPS32
.\scripts\build-win.ps1                    # 默认 media 组合
.\scripts\build-win.ps1 -Profile daily
.\scripts\build-win.ps1 -Profile media -Jobs 4
```

脚本自动处理三件容易忘的事：

1. 把 ESP-IDF 自带的 python 环境前置到 `PATH`（系统里的 3.13 会让 export 找不到依赖）
2. 加载 `D:\esp-idf\export.ps1`
3. 调 `feature_build.py`，后者内部已处理**输出落文件、默认并行度、卡死自动重试**

首次使用可能需要：

```powershell
Set-ExecutionPolicy -Scope CurrentUser RemoteSigned
```

只想用 `feature_build.py` 也可以，`build-win.ps1` 只是把环境准备包装了一层：

```powershell
. D:\esp-idf\export.ps1
python scripts\feature_build.py --profile media
```

## 二、之前为什么慢

三个独立原因，前两个加起来占了绝大部分时间。

### 原因 1：每次构建都整仓重编（已修）

`prepare_config()` 原来无条件重写 `sdkconfig`。只要文件被写，mtime 就变，cmake
判定配置文件有变 → reconfigure → 所有包含 `sdkconfig.h` 的编译单元全部重编。

更隐蔽的是：**这不只是"多写了一次文件"的问题**。Kconfig 在 configure 时会把
`sdkconfig` 规范化——补 `# default:` 注释、重排、丢掉当前不可见的项，磁盘上的
版本与"从根 sdkconfig 重新生成"的版本**永远差 50 行左右**。所以哪怕做了内容比对，
也永远判为"变了"，形成每次构建都全量重编的死循环。

修复：记录**目标配置**的指纹，而不是比对文件内容。

```python
digest = hashlib.sha256(text.encode("utf-8")).hexdigest()
stamp = output.parent / ".sdkconfig.stamp"
if output.is_file() and stamp.is_file() and stamp.read_text().strip() == digest:
    return False          # 目标配置没变，就别碰磁盘上那份被规范化过的文件
```

切换 profile 不受影响：每个 profile 有独立 build 目录和独立指纹。

### 原因 2：构建卡死（已加自动恢复）

**症状**：ninja、ccache、xtensa-gcc 全部 CPU 归零，build 目录几分钟无新文件，
偶尔残留 `.ninja_lock`。卡住的甚至可能是 `lwip/src/core/stats.c` 这种小文件——
说明 gcc 不是在算，是在等。

**根因**：构建输出量很大（近两千个目标）。当输出经由终端管道 / 捕获通道搬运时，
消费者跟不上形成背压，ninja 写阻塞 → ccache 转发阻塞 → gcc 写 stderr 阻塞 → 整链冻死。
**与磁盘无关**，在 SSD 上同样复现。

**修复**：脚本内部把子进程输出直接写进日志文件（不经管道），stdin 置空，
并用 `.ninja_log` 的体积作为进度信号；连续 300 秒无增长即判定卡死，
自动杀进程树、清锁、用 `ninja` 续跑（已完成的目标不会重编），最多重试 2 次。

```
... 构建中（.ninja_log 336 KB）
!! 300s 无任何进展，判定卡死，终止后重试
  清理残留锁: C:\esp-build\media\.ninja_lock
  第 1 次重试：ninja -j 6
```

手动恢复时同理：杀 `ninja|xtensa|ccache` 进程 → 删 `.ninja_lock` → 重新跑。
进度不会丢。

### 原因 3：build 目录在机械盘（已改）

D 盘是 2TB 机械盘，而 build 产物有 5.5 GB / 3.6 万文件。
`feature_build.py` 支持把 build 目录挪走：

```powershell
[Environment]::SetEnvironmentVariable("XIAOZHI_BUILD_ROOT", "C:\esp-build", "User")
```

优先级：`--build-dir` > `XIAOZHI_BUILD_ROOT` > `build/profiles`。

注意 `setx` / `SetEnvironmentVariable` 只对**之后启动**的进程生效，已经在跑的终端读不到。
所以脚本在环境变量缺失时会回退去读 `HKCU\Environment`，保证任何入口都能拿到正确路径。

## 三、参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `--profile` | `english` | `profiles/<name>.json` |
| `--build-dir` | `$XIAOZHI_BUILD_ROOT/<profile>` | 构建目录 |
| `--jobs` | Windows 6 / 其他平台核数 | 经 `CMAKE_BUILD_PARALLEL_LEVEL` 透传给 ninja |
| `--log` | `<build-dir>/build.log` | 构建日志 |
| `--stall-timeout` | 300 | 多少秒无进展判定卡死 |
| `--retries` | 2 | 卡死后重试次数 |
| `--no-build` | — | 只生成 profile 配置 |

并行度：`-j 6` 稳定；`-j 8` 及以上本机偶发僵死。

`idf.py` 自己不认 `-j`，所以走环境变量透传。

## 四、实测数据

| 场景 | 优化前 | 优化后 |
|---|---|---|
| 重复构建（源码无改动） | 6 分 06 秒 | **4.2 秒** |
| cmake 配置阶段（新 build 目录，一次性） | — | 147 秒（135.7 配置 + 11.6 生成） |
| 全量编译（约 1686 个目标，`-j 4`） | 11 分 43 秒 | — |
| 剩余 254 个目标续跑（`-j 6`，输出落文件） | 卡死 | 4 分 19 秒，无卡死 |

147 秒的配置阶段无法避免：176 个组件 + 74 个托管依赖要跑 Kconfig、ldgen、资源打包。
**只要 build 目录不被删，这笔钱只付一次**——所以别动不动 `fullclean`。

ccache 上限也从默认 5 GB 提到 25 GB（11 个 profile 的产物需要驻留，
不够会互相挤掉，切 profile 时命中率骤降）：

```powershell
ccache --set-config=max_size=25G
ccache --set-config=sloppiness=file_stat_matches,time_macros
```

## 五、排错

**构建卡住不动**：先看 `<build-dir>/build.log` 尾部停在哪一步。脚本会自动处理，
若连续 3 次都卡在同一处，检查是否有杀毒软件/同步盘在扫描 build 目录。

**改了 profile 但行为没变**：删掉对应 build 目录下的 `.sdkconfig.stamp` 再构建，
或直接 `--build-dir` 换一个新目录。

**产物大小异常**：`python -m esptool ... read-flash` 前先确认分区表是 `16m_app6m`
（`0x8000` 处 MD5 应为 `269F7B90718529A9768992D9D00893D5`），否则 `0x20000` 写入会破坏 NVS。
