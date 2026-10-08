#ifndef FH_PROTOCOL_H
#define FH_PROTOCOL_H

// ============================================================================
//  Fenghua 键鼠协议 (PC -> Android, UDP)
//
//  每个数据报 = [1 字节 type][2 字节 payload 长度, 小端][payload]
//  长度字段只描述 payload; 实际收包按"够长就取"的宽松策略处理。
//
//  Android 侧按 type 分派 (见 src/core/engine.cpp):
//    0x10 按键      payload: u16 vkCode(小端) + u8 pressed
//    0x11 鼠标移动  payload: f32 dx + f32 dy      (相对位移, 单位=PC 像素)
//    0x12 鼠标按键  payload: u8 button + u8 pressed
//                            button: 0 左 1 右 2 中 4 X1 5 X2
//    0x13 滚轮      payload: i16 delta(小端, 格数, 正=上)
//    0x30 ping      (无 payload)  设备收到后立刻回 0x31, 用于 RTT
//    0x31 pong      (无 payload)
//
//  映射关系 (Android 侧):
//    - 0x10 的 vkCode 会去"按键映射表"里查屏幕坐标 -> 注入触摸
//    - 0x11 按当前指针模式处理: 自由 / 约束 / 陀螺仪
//    - 0x12 左键在自由模式=按住拖动, 约束模式=点击滑动, 其余键可绑定到屏幕
//    - 0x13 可绑定成"滚轮上/下"两个虚拟键 (0x88/0x89)
// ============================================================================

#include <stdint.h>

#define FH_PKT_KEY    0x10
#define FH_PKT_MOUSE  0x11
#define FH_PKT_BUTTON 0x12
#define FH_PKT_WHEEL  0x13
#define FH_PKT_PING   0x30
#define FH_PKT_PONG   0x31

#define FH_DEFAULT_PORT 56789

// 鼠标按键编号
#define FH_BTN_LEFT   0
#define FH_BTN_RIGHT  1
#define FH_BTN_MIDDLE 2
#define FH_BTN_X1     4
#define FH_BTN_X2     5

// 滚轮映射用的虚拟键码 (Android 侧固定这两个值)
#define FH_VK_WHEEL_UP   0x88
#define FH_VK_WHEEL_DOWN 0x89

#endif  // FH_PROTOCOL_H
