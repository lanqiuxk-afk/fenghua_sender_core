#pragma once

#include <cstddef>

// 投屏核心 (无界面):
//   连接设备 56790 -> 解析 FHSC 流头 + 帧长度前缀 + Annex-B H.264
//   -> FFmpeg (avcodec) 解码 -> RGB32 帧
// 输出:
//   - 状态统计 (VideoPlayerGetStatus)
//   - 可选: 解码帧写成 PPM 序列 (VideoPlayerSetFrameDump), 便于取图/二次处理
//   - 可选: 转发为 MPEG-TS/UDP 给 OBS (VideoPlayerSetObs)
//
// 运行时依赖 third_party/ 下的 avcodec-61.dll / avutil-59.dll / swresample-5.dll
// 投屏端口 (默认 56790), 必须在 VideoPlayerStart 之前调用
void VideoPlayerSetPort(unsigned short port);

bool VideoPlayerStart(const char* ip);
void VideoPlayerStop();
bool VideoPlayerRunning();
void VideoPlayerGetStatus(char* buf, size_t n);

// 把解码后的 RGB32 帧按 PPM 写到 dir (每帧一个文件)。on=false 停止。
void VideoPlayerSetFrameDump(bool on, const char* dir);

// OBS 输出: 把 H.264 流转发为 MPEG-TS/UDP (默认 127.0.0.1:9998),
// OBS 媒体源输入 udp://127.0.0.1:9998 即可采集
void VideoPlayerSetObs(bool on, unsigned short port = 9998);
bool VideoPlayerGetObs();
