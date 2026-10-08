// ============================================================================
//  Fenghua sender (Windows, console)
//
//  两个功能, 控制台命令行开关控制:
//
//    [键鼠] 捕获 PC 键盘鼠标 -> UDP 发给 Android 端 (协议见 include/protocol.h)
//           实现要点: 隐藏的"停靠窗口" + Raw Input。光标被钉在窗口里,
//           位移 = 相对停靠点的偏移; 事件驱动、无轮询延迟, 也不会点到桌面/游戏。
//
//    [投屏] 连接设备 56790 -> 解析 FHSC / H.264 -> FFmpeg 解码
//           输出方式可并存:
//             --obs [端口]      转发为 MPEG-TS/UDP 给 OBS (默认 9998)
//             --dump <目录>     把解码帧写成 PPM 序列
//
//  用法:
//    fenghua_sender                        交互式输入设备 IP (只开键鼠)
//    fenghua_sender 192.168.1.23           键鼠
//    fenghua_sender 192.168.1.23 --stream              键鼠 + 投屏(统计)
//    fenghua_sender 192.168.1.23 --stream --obs        键鼠 + 投屏 + OBS
//    fenghua_sender 192.168.1.23 --stream --dump out   键鼠 + 投屏 + 存帧
//
//  运行中: F12 开始/停止键鼠捕获, Ctrl+C 退出
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "protocol.h"
#include "video_player.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace {

constexpr int kParkSize = 40;       // 停靠窗口边长
constexpr float kMaxJump = 500.0f;  // 单次位移上限 (过滤宏工具绝对定位导致的瞬移)

SOCKET g_sock = INVALID_SOCKET;
sockaddr_in g_dest = {};
HWND g_park = nullptr;
POINT g_origin = {};
POINT g_parkPt = {};
std::atomic<bool> g_capturing{false};
std::atomic<bool> g_running{true};
std::atomic<bool> g_cursorDirty{false};
std::atomic<long long> g_sent{0};
std::atomic<long long> g_pong{0};
bool g_streaming = false;

#pragma pack(push, 1)
struct PktHeader { uint8_t type; uint16_t len; };
#pragma pack(pop)

void Send(uint8_t type, const void* data, uint16_t len) {
    if (g_sock == INVALID_SOCKET) return;
    uint8_t buf[64];
    PktHeader h{type, len};
    memcpy(buf, &h, sizeof(h));
    if (len) memcpy(buf + sizeof(h), data, len);
    if (sendto(g_sock, (const char*)buf, sizeof(h) + len, 0,
               (sockaddr*)&g_dest, sizeof(g_dest)) > 0) {
        g_sent.fetch_add(1);
    }
}

// ---- Raw Input 接收窗 (消息专用窗口, 由主线程消息循环驱动) ----
LRESULT CALLBACK RawProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp);   // fwd

bool RegisterRawWindow() {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = RawProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = L"FhSenderRawWnd";
    RegisterClassW(&wc);   // 已注册过不算失败
    HWND hw = CreateWindowExW(0, L"FhSenderRawWnd", L"", 0, 0, 0, 0, 0,
                              HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!hw) {
        printf("[!] create raw-input window failed: %lu\n", GetLastError());
        return false;
    }
    RAWINPUTDEVICE rid = {};
    rid.usUsagePage = 0x01;   // Generic Desktop
    rid.usUsage = 0x02;       // Mouse
    rid.dwFlags = RIDEV_INPUTSINK;
    rid.hwndTarget = hw;
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        printf("[!] RegisterRawInputDevices failed: %lu\n", GetLastError());
        return false;
    }
    return true;
}

// ---- 停靠窗口: 置顶 + 禁用 + 不激活, 光标停里面点不到任何东西 ----
void CreateParkWindow() {
    if (g_park) return;
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = L"FhParkWnd";
    RegisterClassW(&wc);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    g_park = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                             L"FhParkWnd", L"", WS_POPUP | WS_DISABLED,
                             sw - kParkSize, sh - kParkSize, kParkSize, kParkSize,
                             nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(g_park, SW_SHOWNOACTIVATE);
    RECT r;
    GetWindowRect(g_park, &r);
    g_parkPt.x = (r.left + r.right) / 2;
    g_parkPt.y = (r.top + r.bottom) / 2;
}

void DestroyParkWindow() {
    if (!g_park) return;
    DestroyWindow(g_park);
    g_park = nullptr;
}

void EmitRawButtons(USHORT flags) {
    static USHORT prev = 0;
    struct { USHORT mask; int btn; } map[] = {
        {(USHORT)RI_MOUSE_BUTTON_1_DOWN, FH_BTN_LEFT},
        {(USHORT)RI_MOUSE_BUTTON_2_DOWN, FH_BTN_RIGHT},
        {(USHORT)RI_MOUSE_BUTTON_3_DOWN, FH_BTN_MIDDLE},
        {(USHORT)RI_MOUSE_BUTTON_4_DOWN, FH_BTN_X1},
        {(USHORT)RI_MOUSE_BUTTON_5_DOWN, FH_BTN_X2},
    };
    for (auto& m : map) {
        bool was = (prev & m.mask) != 0;
        bool now = (flags & m.mask) != 0;
        if (was == now) continue;
        prev = now ? (USHORT)(prev | m.mask) : (USHORT)(prev & ~m.mask);
        uint8_t b[2] = {(uint8_t)m.btn, (uint8_t)(now ? 1 : 0)};
        Send(FH_PKT_BUTTON, b, 2);
    }
}

void HandleRawInput(LPARAM lParam) {
    if (!g_capturing.load()) return;
    UINT size = 0;
    if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, nullptr, &size,
                        sizeof(RAWINPUTHEADER)) != 0 || size == 0) {
        return;
    }
    BYTE buf[256];
    if (size > sizeof(buf)) size = sizeof(buf);
    if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, buf, &size,
                        sizeof(RAWINPUTHEADER)) == (UINT)-1) {
        return;
    }
    auto* ri = (RAWINPUT*)buf;
    if (ri->header.dwType != RIM_TYPEMOUSE) return;
    const RAWMOUSE& m = ri->data.mouse;

    LONG dx = m.lLastX, dy = m.lLastY;
    if (fabsf((float)dx) < kMaxJump && fabsf((float)dy) < kMaxJump) {
        if (dx != 0 || dy != 0) {
            float d[2] = {(float)dx, (float)dy};
            Send(FH_PKT_MOUSE, d, 8);
        }
    }
    if (m.usButtonFlags & RI_MOUSE_WHEEL) {
        int16_t wheel = (int16_t)((SHORT)m.usButtonData / WHEEL_DELTA);
        if (wheel != 0) {
            uint8_t b[2] = {(uint8_t)(wheel & 0xFF), (uint8_t)((wheel >> 8) & 0xFF)};
            Send(FH_PKT_WHEEL, b, 2);
        }
    }
    EmitRawButtons(m.usButtonFlags);
    g_cursorDirty = true;
}

LRESULT CALLBACK RawProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_INPUT) {
        HandleRawInput(lp);
        return 0;
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

// ---- 键盘钩子: 抓按键 + F12 开关 ----
LRESULT CALLBACK KBProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code < 0) return CallNextHookEx(nullptr, code, wParam, lParam);
    auto* kb = (KBDLLHOOKSTRUCT*)lParam;
    // SendInput 注入的键 (宏工具) 放行给 PC
    if (kb->flags & (LLKHF_INJECTED | LLKHF_LOWER_IL_INJECTED))
        return CallNextHookEx(nullptr, code, wParam, lParam);

    bool pressed = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    if (kb->vkCode == VK_F12) {
        if (pressed) {
            if (!g_capturing.load()) {
                g_capturing = true;
                GetCursorPos(&g_origin);
                CreateParkWindow();
                SetCursorPos(g_parkPt.x, g_parkPt.y);
                printf("\n[capture] ON\n");
            } else {
                g_capturing = false;
                DestroyParkWindow();
                SetCursorPos(g_origin.x, g_origin.y);
                printf("\n[capture] OFF\n");
            }
        }
        return 1;   // F12 是本地开关, 永远不发给设备
    }
    if (g_capturing.load()) {
        uint8_t b[3] = {(uint8_t)(kb->vkCode & 0xFF), (uint8_t)(kb->vkCode >> 8),
                        (uint8_t)(pressed ? 1 : 0)};
        Send(FH_PKT_KEY, b, 3);
        return 1;   // 捕获期间吃掉键盘, 不让 PC 也响应
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

BOOL WINAPI ConsoleHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT) {
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

void PrintUsage(const char* exe) {
    printf(
        "usage: %s [device-ip] [options]\n"
        "  --stream          开启投屏接收 (默认 56790), 打印解码统计\n"
        "  --port <n>        投屏端口 (默认 56790)\n"
        "  --obs [port]      把 H.264 转发成 MPEG-TS/UDP 给 OBS (默认 9998)\n"
        "                    OBS 媒体源填 udp://127.0.0.1:9998\n"
        "  --dump <dir>      把解码帧写成 PPM 序列到目录\n"
        "  --help\n"
        "运行中: F12 开始/停止键鼠捕获, Ctrl+C 退出\n",
        exe);
}

}  // namespace

int main(int argc, char** argv) {
    SetProcessDPIAware();   // 高缩放下不感知 DPI 会拿到虚拟化坐标, 位移全错

    std::string ip;
    std::string dumpDir;
    bool wantStream = false, wantObs = false;
    unsigned short obsPort = 9998;
    unsigned short streamPort = FH_STREAM_PORT;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") { PrintUsage(argv[0]); return 0; }
        else if (a == "--stream") wantStream = true;
        else if (a == "--port") {
            if (i + 1 >= argc) { printf("[!] --port needs a value\n"); return 2; }
            streamPort = (unsigned short)atoi(argv[++i]);
        }
        else if (a == "--obs") {
            wantObs = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') obsPort = (unsigned short)atoi(argv[++i]);
        } else if (a == "--dump") {
            if (i + 1 >= argc) { printf("[!] --dump needs a directory\n"); return 2; }
            dumpDir = argv[++i];
        } else if (!a.empty() && a[0] == '-') {
            printf("[!] unknown option: %s\n", a.c_str());
            PrintUsage(argv[0]);
            return 2;
        } else if (ip.empty()) {
            ip = a;
        }
    }
    if (ip.empty()) {
        char buf[64] = {};
        printf("device ip: ");
        if (!fgets(buf, sizeof(buf), stdin)) return 1;
        char* nl = strchr(buf, '\n');
        if (nl) *nl = 0;
        ip = buf;
    }

    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) {
        printf("[!] WSAStartup failed\n");
        return 1;
    }

    g_dest = {};
    g_dest.sin_family = AF_INET;
    g_dest.sin_port = htons(FH_DEFAULT_PORT);
    if (inet_pton(AF_INET, ip.c_str(), &g_dest.sin_addr) != 1) {
        printf("[!] bad ip: %s\n", ip.c_str());
        return 1;
    }
    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock == INVALID_SOCKET) {
        printf("[!] socket() failed\n");
        return 1;
    }
    u_long nb = 1;
    ioctlsocket(g_sock, FIONBIO, &nb);   // 收 pong 不阻塞主循环

    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    printf("==================================================\n");
    printf("  Fenghua sender -> %s:%d\n", ip.c_str(), FH_DEFAULT_PORT);
    printf("  key/mouse: on   stream: %s   obs: %s\n",
           wantStream ? "on" : "off",
           wantObs ? "on(udp 9998)" : "off");
    printf("==================================================\n");

    if (!RegisterRawWindow()) {
        printf("[!] raw input unavailable, mouse will not work\n");
    }
    SetWindowsHookEx(WH_KEYBOARD_LL, KBProc, GetModuleHandle(nullptr), 0);

    if (wantStream) {
        VideoPlayerSetPort(streamPort);
        if (!VideoPlayerStart(ip.c_str())) {
            printf("[!] stream start failed\n");
        } else {
            g_streaming = true;
            if (wantObs) VideoPlayerSetObs(true, obsPort);
            if (!dumpDir.empty()) VideoPlayerSetFrameDump(true, dumpDir.c_str());
        }
    }

    // 心跳: 每 500ms 一次 ping, 收到 pong 说明设备在线
    std::thread heart([&] {
        char buf[64];
        while (g_running.load()) {
            Send(FH_PKT_PING, nullptr, 0);
            sockaddr_in from = {};
            int fl = sizeof(from);
            int n = recvfrom(g_sock, buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
            if (n >= 1 && (uint8_t)buf[0] == FH_PKT_PONG) g_pong.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    });

    MSG msg;
    int tick = 0;
    while (g_running.load()) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        // 每次鼠标移动后把光标钉回停靠点, 保证位移始终相对它
        if (g_capturing.load() && g_cursorDirty.exchange(false)) {
            SetCursorPos(g_parkPt.x, g_parkPt.y);
        }
        if (++tick % 4 == 0) {
            char st[160] = "";
            if (g_streaming) VideoPlayerGetStatus(st, sizeof(st));
            printf("\r[%s] sent=%-7lld pong=%-4lld %s        ",
                   g_capturing.load() ? "capturing" : "idle",
                   g_sent.load(), g_pong.load(), st);
            fflush(stdout);
        }
        Sleep(50);
    }

    printf("\nshutting down...\n");
    g_capturing = false;
    if (g_streaming) VideoPlayerStop();
    DestroyParkWindow();
    if (g_sock != INVALID_SOCKET) closesocket(g_sock);
    WSACleanup();
    return 0;
}
