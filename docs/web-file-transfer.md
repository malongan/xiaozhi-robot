# 网页传文件（Wi-Fi SD 卡管理器）

开启后，设备在连上 Wi-Fi 时启动一个极小的 HTTP 服务，用手机或电脑浏览器即可浏览、上传、下载、删除 SD 卡里的文件，无需数据线。

## 启用

1. 板级 `config.json` 的 `sdkconfig_append` 加入：

   ```json
   "CONFIG_WEB_FILE_SERVER=y"
   ```

   或 `idf.py menuconfig` → **Web File Transfer** → *Enable web file server*。

2. 相关配置（同一菜单）：

   | 配置 | 默认 | 说明 |
   | --- | --- | --- |
   | `CONFIG_WEB_FILE_SERVER_PORT` | 80 | 监听端口；非 80 时设备显示的网址会带 `:端口` |
   | `CONFIG_WEB_FILE_SERVER_PIN` | 空 | 访问口令；为空表示局域网内免口令 |

3. 编译烧录后，设备连上 Wi-Fi 即自动启动服务；断开或进入配网模式时自动停止。

## 设备端入口

- 首页底部把手 → 「网络」磁贴 → **文件传输**页：显示网址、运行状态、SD 剩余空间、上传进度，可手动启动/停止。
- 设置页新增只读 **IP** 行，方便对照。

## 浏览器使用

打开 `http://<设备IP>`（设置了 PIN 时用 `http://<设备IP>/?token=<PIN>`）。页面支持：

- 面包屑导航、进入子目录；
- 多选上传 / 拖拽上传（顺序上传，逐个显示进度；同名文件自动改名为 `name.1.ext`，不覆盖）；
- 下载、删除（删除不可恢复）、新建文件夹。

## HTTP API

所有接口在设置了 PIN 时都需要 `?token=<PIN>` 或请求头 `X-Auth-Token: <PIN>`。路径参数为 SD 卡内绝对路径，如 `/dict/word.txt`。

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/` | 内置单页页面（gzip） |
| GET | `/api/info` | 板名、固件版本、IP、SD 容量 |
| GET | `/api/list?path=/` | 目录列表（目录在前，按名排序，最多 500 条） |
| GET | `/api/file?path=/a.txt` | 下载文件 |
| PUT | `/api/file?path=/a.txt` | 上传，请求体为原始字节，必须带 `Content-Length` |
| POST | `/api/mkdir` | `{"path":"/新目录"}` |
| POST | `/api/rm` | `{"path":"/a.txt"}`；非空目录返回 409 |

示例：

```sh
# 列出根目录
curl -s "http://192.168.1.50/api/list?path=/" | jq

# 上传（注意 --data-binary，避免 curl 改动换行）
curl -T ./demo.mp3 "http://192.168.1.50/api/file?path=/music/demo.mp3"

# 下载
curl -OJ "http://192.168.1.50/api/file?path=/music/demo.mp3"

# 新建目录 / 删除
curl -X POST -H 'Content-Type: application/json' \
     -d '{"path":"/notes"}' "http://192.168.1.50/api/mkdir"
curl -X POST -H 'Content-Type: application/json' \
     -d '{"path":"/notes/old.txt"}' "http://192.168.1.50/api/rm"
```

返回体为 JSON（上传成功 201，携带最终路径与字节数），出错时形如 `{"error":"..."}`，并配合 4xx/5xx 状态码。

## 限制与注意事项

- 仅监听 STA 网卡（同一局域网）；不开热点，不暴露到公网。
- `max_open_sockets = 4`，即最多 4 个并发连接；上传按 8 KB 分块写入，任一连接超时 15 秒。
- 上传前检查剩余空间（额外预留少量 FAT 元数据余量），不足返回 507；写入失败会删除残留文件。
- 路径含 `..`、反斜杠或冒号会被拒绝；单文件路径长度上限 384 字节。
- 删除是直接 `unlink`/`rmdir`，不经过回收站。
- 板级 FATFS `max_files` 建议 ≥10（本板已从 5 调到 10），避免并发句柄不足。
- PIN 是明文比对、无加密（HTTP）。在不可信网络请设置 PIN，或仅在可信局域网使用。
- 首页 `/` 返回的是构建期预压缩的 gzip，浏览器会自动解压；用 `curl` 直接看首页需加 `--compressed`（JSON/文件接口不受影响）。
- 实测吞吐：PC 跨网段（RTT ≈ 180 ms）约 60–90 KB/s。瓶颈是设备侧 lwIP TCP 窗口仅 5760 B（`CONFIG_LWIP_TCP_WND_DEFAULT`），同网段/低延迟下会明显更快；如需大文件高速传输，可提高窗口并开启 `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`（占用 PSRAM，需回归 Wi-Fi 稳定性）。
