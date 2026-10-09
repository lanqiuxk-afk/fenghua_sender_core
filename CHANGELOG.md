# 版本记录

## v1.1

**修复投屏：v1.0 的投屏不可用，请用这个版本。**

问题原因：设备端在 `flags=0` 模式下实际发送的是连续的 scrcpy 帧包
（`[ptsAndFlags 8B][packetSize u32BE][payload]`），而旧代码走的是依赖
`av_parser_parse2` 的裸流分支。随包的 FFmpeg 是 scrcpy 用的精简构建
（configure 为 `--disable-everything ... --enable-parser=png`），**没有编译
h264 parser**，`av_parser_init(AV_CODEC_ID_H264)` 返回 NULL，该分支直接
`return`。失败是静默的：连接成功、字节在收，但解码器一帧都拿不到。

修复：按设备端的包边界解析 12 字节头，每个包直接 `avcodec_send_packet`，
不依赖 parser。实测 128×96 测试流解码 25fps 以上，帧尺寸与像素数精确正确。

其它变化：

- 新增 `--port <n>`，投屏端口可配置（默认仍是 56790）
- 删掉三个依赖 parser 的失效函数（`ResetRawH264Parser` / `ParseRawH264` /
  `FlushRawH264Parser`）

校验：

```
SHA256  fenghua_sender_v1.1_win64.zip
        9ed133afe30e16b87677491c2632a61582786f301d6b8afbc52feeb084e3302c
```

## v1.0

首个版本。

- 键鼠映射：隐藏停靠窗口 + Raw Input 捕获，UDP 发往设备 56789
- 投屏接收：连接设备 56790，FHSC 流头 + H.264 → FFmpeg 解码
  输出到 OBS（MPEG-TS/UDP）或逐帧存 PPM
- 控制台程序，无界面；运行时依赖随包的三个 FFmpeg DLL（LGPL）

已知问题：投屏不可用（见 v1.1 的修复说明）。
