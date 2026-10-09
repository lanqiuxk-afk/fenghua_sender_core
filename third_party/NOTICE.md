# 第三方组件

## FFmpeg (运行时 DLL)

| 项 | 内容 |
|---|---|
| 文件 | `third_party/avcodec-61.dll`、`avutil-59.dll`、`swresample-5.dll` |
| 用途 | 投屏解码：H.264 → RGB32 |
| 版本 | FFmpeg 7.1.1（libavcodec 61.19.101） |
| 许可证 | **LGPL v2.1 或更高** |
| 说明 | 随包分发，必须与 `fenghua_sender.exe` 放在同一目录 |

这三份是共享库（动态链接），符合 LGPL 对动态链接的要求。
它们是一个精简构建（`--disable-everything` + 只启用 h264/hevc/av1 等解码器），
因此**不包含 h264 parser** —— 这一点直接影响解码路径的实现，详见 README 的
「解码实现要点」。

FFmpeg 源码与许可证：<https://ffmpeg.org/legal.html>

如果你要重新编译发布，注意 LGPL 要求：修改过的 FFmpeg 需提供源码，
且用户应能替换这些 DLL（本程序通过动态链接 + 同目录加载满足后一条）。

## 许可

本仓库自身代码为 MIT（见 `LICENSE`），与上述第三方组件的许可证相互独立。
