# 功能包开发指导

本文是新增/拆分定制功能的执行规范。目标：功能可独立编译、关闭后不残留入口逻辑，同时 `full` 行为与当前版本一致。

## 1. 先判断拆分边界

- 一个用户可感知的能力对应一个 `FEATURE_` 开关。
- 同一能力的多个页面/面板共用一个开关，例如蓝牙键盘的媒体、全键盘、数字键。
- 只有能独立交付、独立验证的能力才拆成第二个开关。
- 导航、抽屉、状态栏、共享字体等核心设施不放进功能包。
- 关闭功能后若入口仍需保留，入口置灰并给出原因，不要留下能点进去的空页面。

## 2. 目录与命名

```text
main/features/<id>/
├── feature.json
├── README.md
├── <实现>.cc
├── <实现>.h
└── <资源>/          # 仅本功能使用的页面、字体、前端资源
```

- 功能 ID 用小写蛇形：`english_dictation`、`stroke_order`、`ble_keyboard`、`web_file_transfer`。
- Kconfig 统一为 `CONFIG_FEATURE_<ID大写下划线>`。
- 源码用 `git mv` 迁移，保留 Git 历史，不要复制后再删。
- 前端/字体等资源跟着功能目录走；共享资源才留在 `main/` 或 `main/display/`。
- `feature.json` 至少包含 `id`、`name`、`description`、`config`、`default`、`sources`；有资源时补 `assets`，`default` 必须与 Kconfig 一致。

```json
{
  "id": "web_file_transfer",
  "name": "网络文件传输",
  "description": "同一 Wi-Fi 下用浏览器管理 SD 卡文件",
  "config": "CONFIG_FEATURE_WEB_FILE_TRANSFER",
  "default": false,
  "sources": ["features/web_file_transfer/web_file_server.cc"],
  "assets": ["features/web_file_transfer/web_assets/index.html"]
}
```

## 3. Kconfig

在 `main/Kconfig.projbuild` 增加开关：

```kconfig
config FEATURE_WEB_FILE_TRANSFER
    bool "Enable SD card web file transfer"
    default n
    help
        Serve the SD card over HTTP. Disable it to build without the
        web transfer backend, assets and LCD page.
```

规则：

- 依赖某个子系统时写 `depends on BT_ENABLED` 等，不要假设子系统一定开启。
- 端口、PIN、路径等参数继续放在同一菜单下，并 `depends on` 主开关。
- 新功能默认值跟产品策略一致：目标板默认交付的能力可在板级 `config.json` 打开；通用功能不要随意默认 `y`。
- 重命名旧开关时，同步更新板级 `config.json`、代码和文档；根 `sdkconfig` 是本地生成文件，不入库，旧配置不会自动迁移。合并后要求开发者重新生成配置，或在 menuconfig 中打开新开关。
- 已发生的迁移：`CONFIG_WEB_FILE_SERVER*` -> `CONFIG_FEATURE_WEB_FILE_TRANSFER*`。新增迁移时在 `docs/custom/feature-packs.md` 的“配置迁移”表登记。

## 4. CMake

基础列表只放始终存在的东西；功能源码、字体、嵌入资源放到条件块：

```cmake
if(CONFIG_FEATURE_WEB_FILE_TRANSFER)
    list(APPEND SOURCES "features/web_file_transfer/web_file_server.cc")
    set(WEB_INDEX_HTML "${CMAKE_CURRENT_SOURCE_DIR}/features/web_file_transfer/web_assets/index.html")
    set(WEB_INDEX_GZ "${CMAKE_CURRENT_BINARY_DIR}/features/web_file_transfer/index.html.gz")
    # 生成/嵌入资源...
    list(APPEND WEB_ASSETS "${WEB_INDEX_GZ}")
endif()
```

- 只有功能真正用到的组件才加进 `PRIV_REQUIRES`。
- 嵌入文件的生成路径变化可能导致 `_binary_*` 符号变化，改完必须编译验证。
- 关闭功能时不能留下未定义的 `SRCS` 或 `EMBED_FILES`。

## 5. 代码裁剪

### 5.1 头文件

```cpp
#if CONFIG_FEATURE_WEB_FILE_TRANSFER
#include "features/web_file_transfer/web_file_server.h"
#endif
```

- 功能专属类型/成员用 `#if` 包住。
- 核心类仍要使用的状态位（例如 `file_service_visible_`）保留。
- 不要为了关功能而删掉共享导航控件，例如抽屉把手 `media_bar_`。

### 5.2 页面与回调

在 `lcd_display.cc` 中至少检查五处：

1. 页面创建 `SetupUI()`：整块用开关包住。
2. 回调注册：只在页面创建块内注册。
3. 回调/更新函数定义：用开关包住，避免关闭后仍编进固件。
4. 被核心流程调用的方法：提供关闭时的空实现，例如 `ShowXxxScreen(bool) {}`。
5. 抽屉入口：保留 tile，增加 `DrawerXxxEnabled()`，关闭时返回 `false` 并显示“功能未启用”。

```cpp
bool LcdDisplay::DrawerWebFileTransferEnabled() const {
#if CONFIG_FEATURE_WEB_FILE_TRANSFER
    return true;
#else
    return false;
#endif
}
```

- 统一用 `#if CONFIG_...`，不要依赖裸 `#ifdef`。
- 关闭分支的 stub 要覆盖所有仍会被调用的入口，避免链接错误。
- 析构/停止回调时把外部回调置空，防止页面销毁后回调访问悬空对象。

### 5.3 网络/任务生命周期

- 功能启动、停止点跟网络事件绑定：Wi-Fi 连接后启动，断线/配网热点时停止。
- 关闭功能时，这些调用点和 include 一起裁剪。
- 关闭功能包不等于关闭底层协议栈。例如关闭蓝牙键盘不会自动关闭 `BT_ENABLED`；需要单独评估，不要混在一个开关里。

## 6. Profile 配置

每个 profile 都显式写新功能，避免默认值变化导致组合漂移：

```json
{
  "name": "base",
  "features": {
    "english_dictation": false,
    "stroke_order": false,
    "ble_keyboard": false,
    "web_file_transfer": false
  }
}
```

同时新增一个只开该功能的隔离 profile，例如 `profiles/web_file_transfer.json`。`full` 必须打开全部功能并保持既有行为。

## 7. 构建与验证矩阵

```bash
export IDF_PYTHON_ENV_PATH=$HOME/.espressif/python_env/idf6.1_py3.12_env
source "$HOME/esp/esp-idf/export.sh"

python3 scripts/feature_build.py --list
for p in base english stroke keyboard web_file_transfer full; do
    python3 scripts/feature_build.py --profile "$p"
done
```

每次至少检查：

| 检查项 | 命令/方法 |
|---|---|
| 配置生效 | 查看 `profile:` 输出和 `build/profiles/<name>/sdkconfig` |
| 体积变化 | `idf.py -B build/profiles/<name> size` |
| 符号隔离 | `nm -C build/profiles/<name>/xiaozhi.elf \| rg 'WebFileServer'` |
| 主机测试 | `python3 -m unittest discover -s scripts/tests` |
| 敏感信息 | `ops run security-scan .` |
| 补丁格式 | `git diff --check` |

符号检查要覆盖“该有”和“不该有”两侧：关闭的 profile 必须没有功能符号，隔离 profile 必须有。

## 8. 真机验证

只刷 app，不动分区和 assets：

```bash
python -m esptool --chip esp32s3 -p /dev/cu.usbmodem31101 -b 460800 \
  --before default-reset --after hard-reset \
  write-flash 0x20000 build/profiles/full/xiaozhi.bin
```

- 禁止 `erase-flash`。
- 看启动日志：SD 挂载、词库/笔顺数据、功能启动、无 panic。
- 网络功能检查：Wi-Fi 连接后服务自动启动，浏览器可打开地址，断线/配网时停止。
- 不能自动验证的交互（触摸面板、实体按键）要明确记录“未人工点按”，不要写成已验证。
- 验证完重新刷回最终 `full` 构建，避免把临时调试固件留在设备上。

## 9. 常见坑

1. **只包了页面创建，没包回调和函数定义**：关闭后代码仍占空间，甚至引用不存在的类型。
2. **只包了定义，没留 stub**：核心流程仍调用 `ShowXxx`，链接失败。
3. **把共享控件当功能控件删掉**：例如底部抽屉把手、状态栏、返回键。
4. **改了 Kconfig 名称但没改板级配置/sdkconfig**：功能静默变默认值。
5. **嵌入资源路径变了但没验证 `_binary_*` 符号**：链接失败或页面 404。
6. **关闭功能却保留启动调用**：空实现能链接，但运行期仍做无效工作。
7. **NVS 持久化开关干扰验证**：例如蓝牙开关之前被关过，启动日志不会出现 BLE 初始化；先确认持久化状态。
8. **把 `full` 体积变化当成功能回归**：函数重排/入口判定可能带来几十字节差异，行为验证更重要。
9. **提交了 `build/`、日志、备份或含凭据的 NVS**：提交前跑安全扫描并检查 `git status`。
10. **未确认就提交/推送大改**：先报告构建与真机结果，再提交。

## 10. 完成清单

- [ ] 功能目录、`feature.json`、README 完成
- [ ] Kconfig/CMake/板级配置/根 sdkconfig 同步
- [ ] UI 创建、回调、函数定义、抽屉入口全部按开关处理
- [ ] 关闭时 stub 完整，核心导航保留
- [ ] 所有 profile 显式配置并新增隔离 profile
- [ ] 全部 profile 编译通过，符号/体积符合预期
- [ ] 主机测试、安全扫描、`git diff --check` 通过
- [ ] `full` 真机启动与功能验证，记录未覆盖项
- [ ] 更新 `docs/custom/feature-packs.md` 和本指导
- [ ] 确认后提交并推送；tag/Release 单独确认
