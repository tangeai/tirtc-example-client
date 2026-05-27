# TiRTC Client Example

这是一个最小 TiRTC 客户端下行示例。
它连接到一个正在上行送流的设备端，直接接收音频和视频流，通过回调统计收到的帧数和字节数，并在运行过程中输出 `TIRTC_CLIENT_PROGRESS`，退出前输出一行 `TIRTC_CLIENT_SUMMARY`。

**注意：** 当前示例按 **“设备端连接后直接发流、客户端连接后直接收流”** 的方式设计，不包含 `subscribe_audio` / `subscribe_video` 这类订阅交互逻辑。

## 准备 TiRTC SDK

```sh
./script/prepare.sh
```

默认会下载 `tangeai/tirtc-example-client` 最新 GitHub Release 里的 runtime 包，并写入本地 `3rd/`。

本地测试时也可以指定 release zip：

```sh
./script/prepare.sh --zip /path/to/YYYYMMDDHHMMSS.zip
```

prepare 只替换 `3rd/`，不会修改 `src/`、`script/`、`README.md` 或 `Makefile`。

## 构建

```sh
./script/build.sh
```

当前预置 runtime 支持 `macos-arm64` 和 `linux-x64`。如需指定平台：

```sh
PLATFORM=linux-x64 ./script/build.sh
```

## 运行

先启动设备端，让设备端使用约定的 stream id 上行音频和视频。然后运行客户端：

```sh
./script/run.sh \
  --app-id "$TIRTC_APP_ID" \
  --endpoint "$TIRTC_ENDPOINT" \
  --device-id "$TIRTC_DEVICE_ID" \
  --token "$TIRTC_TOKEN" \
  --audio-stream-id 10 \
  --video-stream-id 11 \
  --duration-seconds 60
```

参数说明：

- `--app-id`：TiRTC app id。
- `--endpoint`：TiRTC 服务入口。
- `--device-id`：要连接的设备端 id。
- `--token`：连接 token。
- `--audio-stream-id`：客户端要接收的音频 stream id，必须和设备端实际上行使用的音频 stream id 一致。
- `--video-stream-id`：客户端要接收的视频 stream id，必须和设备端实际上行使用的视频 stream id 一致。
- `--duration-seconds`：客户端保持连接和收流的时长。

这个示例不会把音频播放到系统设备，也不会打开窗口渲染视频。音频和视频回调只用于统计帧数、字节数以及输出状态。

## 开发者文档

更详细的 TiRTC 接入说明、概念解释和平台文档参考：

https://docs.tange.ai/products/tirtc/
