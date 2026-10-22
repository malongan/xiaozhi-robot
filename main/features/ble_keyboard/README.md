# 蓝牙键盘功能包

通过 BLE HID 把设备模拟成蓝牙键盘/媒体控制器，提供：

- 媒体控制面板（播放、音量、静音）
- 全键盘与数字键盘面板
- macOS 快捷键（调度、切换、锁屏、截图等）
- 电脑/手机配对、解绑与连接状态管理
- 设置页蓝牙开关与状态

## 开关

```text
CONFIG_FEATURE_BLE_KEYBOARD=y
```

依赖 `CONFIG_BT_ENABLED=y`。关闭后不编译 `media_ble.cc`，不创建媒体/蓝牙页面和设置页蓝牙行；抽屉“键盘”入口保留为不可用状态。底部抽屉把手（`media_bar_`）仍保留，因为它负责打开抽屉，不属于键盘功能。

## 构建

```bash
python3 scripts/feature_build.py --profile full
```
