# Fenghua Sender (Windows)

配套 [Fenghua Core](https://github.com/lanqiuxk-afk/fenghua_core) 的 PC 端发送程序。控制台程序，无界面。

两个功能，命令行开关控制：

| 功能 | 说明 | 端口 |
|---|---|---|
| **键鼠映射** | 捕获 PC 键盘鼠标 → UDP 发给 Android 端 | 56789 (UDP) |
| **投屏接收** | 连接设备投屏流 → FFmpeg 解码 → 转发给 OBS / 存帧 | 56790 (TCP) |

## 编译

需要 **Visual Studio 2022**（或 Build Tools）和 **CMake 3.16+**。

```cmd
build.bat
```

或者手动：

```cmd
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

FFmpeg 开发包（头文件 + `avcodec-61.lib` / `avutil-59.lib`）默认找
`D:/fenghua_build/ffmpeg_dev`，换机器时：

```cmd
cmake -B build -G "Visual Studio 17 2022" -A x64 -DFFMPEG_DIR=你的路径
```

找不到 FFmpeg 时**只编译键鼠功能**，投屏相关代码不参与编译（会有 CMake 警告）。

产物：`build/Release/fenghua_sender.exe`，把 `third_party/*.dll` 复制到 exe 同目录。

## 运行

```cmd
fenghua_sender                                交互式输入设备 IP（只开键鼠）
fenghua_sender 192.168.1.23                    键鼠
fenghua_sender 192.168.1.23 --stream           键鼠 + 投屏（打印解码统计）
fenghua_sender 192.168.1.23 --stream --obs     键鼠 + 投屏 + OBS 输出
fenghua_sender 192.168.1.23 --stream --dump out  键鼠 + 投屏 + 逐帧存 PPM
```

| 开关 | 说明 |
|---|---|
| `--stream` | 连接设备 56790 接收投屏（设备端需已开启投屏） |
| `--obs [端口]` | 把 H.264 流转成 MPEG-TS/UDP，OBS 媒体源填 `udp://127.0.0.1:9998` |
| `--dump <目录>` | 解码帧按 PPM 序列落盘（`frame_000000.ppm`…），便于取图/二次处理 |
| `--help` | 帮助 |

运行中：**F12** 开始/停止键鼠捕获，**Ctrl+C** 退出。

设备 IP 那一栏就是 Android 端启动时打印的 `device ip:`。

## 键鼠部分实现要点

### 光标停靠 + Raw Input

低级钩子（`WH_MOUSE_LL`）在高刷/重负载下会被限流，拿不到可靠的相对位移。所以：

1. 在屏幕角落创建一个**隐藏的停靠窗口**（置顶、`WS_DISABLED`、不激活）；
2. 捕获开启时把光标移到窗口中心，并注册 `RIDEV_INPUTSINK` 的 Raw Input —— 鼠标原始数据直接送到本进程；
3. 每次收到移动事件后把光标**钉回**停靠点，于是 `WM_INPUT` 里的 `lLastX/lLastY` 就是干净的相对位移；
4. 位移/按键/滚轮即时打包发出，事件驱动，无轮询延迟。

副作用：捕获期间光标不在桌面上，点不到任何 PC 程序；但用 Raw Input 的游戏仍能正常拿到物理鼠标数据。

- `LLKHF_INJECTED` 的键盘事件（宏工具用 `SendInput` 发的）会被放行给 PC，不转发。
- 单次位移超过 500px 的会被丢弃（宏工具做绝对定位时会产生这种跳变）。

### 协议

与 Android 端共用 `include/protocol.h`：

```
[1B type][2B payload 长度(小端)][payload]
```

| type | 名称 | payload |
|---|---|---|
| `0x10` | 按键 | `u16 vk + u8 pressed` |
| `0x11` | 鼠标移动 | `f32 dx + f32 dy` |
| `0x12` | 鼠标按键 | `u8 btn + u8 pressed`（0 左 1 右 2 中 4 X1 5 X2）|
| `0x13` | 滚轮 | `i16 delta`（正=上）|
| `0x30` | ping | 无（每秒一次，用来判断设备在线）|
| `0x31` | pong | 无（设备回包，状态栏显示 pong 计数）|

## 投屏部分实现要点

设备端在 56790 上推流，格式（与 `fenghua_elf/src/screen_stream.cpp` 一致）：

```
[16B 流头] "FHSC" + w(u32BE) + h(u32BE) + u32 flags
flags & 0x01 != 0  ->  [4B 大端帧长][完整 H.264 访问单元]  循环
flags & 0x01 == 0  ->  裸 Annex-B 字节流
```

本程序：TCP 连接 → 解析流头初始化解码器 → 逐帧 `avcodec_send_packet` →
`avcodec_receive_frame` → 转 RGB32 → 统计 / 存 PPM / 转 MPEG-TS。

### 解码实现要点

1. **不用 `av_parser_parse2`。**
   常见的精简 FFmpeg 构建（例如 scrcpy 用的那份，configure 里是
   `--disable-everything ... --enable-parser=png`）没有编译 h264 parser，
   `av_parser_init(AV_CODEC_ID_H264)` 返回 NULL。framed 流里每个包本身就是完整访问单元，
   直接 `avcodec_send_packet` 即可，不需要 parser。

2. **帧输出走 OBS 或 PPM。**
   本仓库是控制台程序，没有 D3D/窗口渲染。看画面用 `--obs` 或 `--dump`，都不需要额外依赖。

### OBS 输出

`--obs` 会手工打包 PAT/PMT/PES 成 MPEG-TS 通过 UDP 发出（无外部依赖），
OBS 里添加「媒体源」→ 取消勾选「本地文件」→ 输入 `udp://127.0.0.1:9998`。

## 目录结构

```
fenghua_sender_core/
├── CMakeLists.txt
├── build.bat
├── include/protocol.h           与 Android 端共用的协议定义
├── src/
│   ├── main.cpp                 参数解析 + 停靠窗口/Raw Input + 状态显示
│   ├── input_hook.{h,cpp}       低级钩子封装 (可选, 主流程用 main.cpp 里的实现)
│   └── video_player.{h,cpp}     投屏: FHSC 解析 + FFmpeg 解码 + OBS/PPM 输出
└── third_party/                 FFmpeg 运行时 (LGPL), exe 同目录需放一份
    ├── avcodec-61.dll
    ├── avutil-59.dll
    └── swresample-5.dll
```

## License

MIT
