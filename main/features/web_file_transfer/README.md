# 网络文件传输功能包

在设备与电脑/手机处于同一 Wi-Fi 时，通过浏览器管理 SD 卡：

- 浏览目录、下载、上传、新建文件夹、删除
- 显示服务状态、访问地址、Wi-Fi/IP、SD 卡容量与上传进度
- Wi-Fi 连接后自动启动，断线或进入配网热点时停止
- 支持可选访问 PIN，保护 `/api` 请求

## 开关

```text
CONFIG_FEATURE_WEB_FILE_TRANSFER=y
```

端口与 PIN 由同一菜单下的 `FEATURE_WEB_FILE_TRANSFER_PORT`、
`FEATURE_WEB_FILE_TRANSFER_PIN` 配置。关闭后不编译 HTTP 服务、不嵌入网页，
不创建“网络”页面；抽屉入口保留为不可用状态。

## 构建

```bash
python3 scripts/feature_build.py --profile full
```
