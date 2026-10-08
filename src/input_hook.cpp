#include "input_hook.h"
#include <cstdio>

InputHook& InputHook::Get() { static InputHook h; return h; }

LRESULT CALLBACK InputHook::KeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code >= 0) {
        auto& self = Get();
        KBDLLHOOKSTRUCT* kb = (KBDLLHOOKSTRUCT*)lParam;
        bool pressed = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        if (self.m_keyCb) self.m_keyCb((uint16_t)kb->vkCode, pressed);
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

LRESULT CALLBACK InputHook::MouseProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code >= 0) {
        auto& self = Get();
        MSLLHOOKSTRUCT* ms = (MSLLHOOKSTRUCT*)lParam;

        if (wParam == WM_MOUSEMOVE && self.m_mouseCb) {
            if (self.m_lastX >= 0) {
                float dx = (float)(ms->pt.x - self.m_lastX);
                float dy = (float)(ms->pt.y - self.m_lastY);
                if (dx != 0 || dy != 0) self.m_mouseCb(dx, dy);
            }
            self.m_lastX = ms->pt.x;
            self.m_lastY = ms->pt.y;
        }
        if (wParam == WM_LBUTTONDOWN && self.m_btnCb) self.m_btnCb(0, true);
        if (wParam == WM_LBUTTONUP && self.m_btnCb) self.m_btnCb(0, false);
        if (wParam == WM_RBUTTONDOWN && self.m_btnCb) self.m_btnCb(1, true);
        if (wParam == WM_RBUTTONUP && self.m_btnCb) self.m_btnCb(1, false);
        if (wParam == WM_MBUTTONDOWN && self.m_btnCb) self.m_btnCb(2, true);
        if (wParam == WM_MBUTTONUP && self.m_btnCb) self.m_btnCb(2, false);
        if (wParam == WM_XBUTTONDOWN && self.m_btnCb) {
            int side = (HIWORD(ms->mouseData) == 1) ? 4 : 5;
            self.m_btnCb(side, true);
        }
        if (wParam == WM_XBUTTONUP && self.m_btnCb) {
            int side = (HIWORD(ms->mouseData) == 1) ? 4 : 5;
            self.m_btnCb(side, false);
        }
    }
    // 不阻止事件传递，保留宏程序功能
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

bool InputHook::Start() {
    if (m_active) return true;
    m_lastX = m_lastY = -1;
    m_kbHook = SetWindowsHookEx(WH_KEYBOARD_LL, KeyboardProc, GetModuleHandle(nullptr), 0);
    m_msHook = SetWindowsHookEx(WH_MOUSE_LL, MouseProc, GetModuleHandle(nullptr), 0);
    if (!m_kbHook || !m_msHook) {
        printf("[ERR] Hook failed\n");
        if (m_kbHook) UnhookWindowsHookEx(m_kbHook);
        if (m_msHook) UnhookWindowsHookEx(m_msHook);
        return false;
    }
    m_active = true;
    
    // 启动监测时隐藏光标
    if (m_hideCursor) {
        ShowCursor(FALSE);
    }
    
    return true;
}

void InputHook::Stop() {
    if (m_kbHook) { UnhookWindowsHookEx(m_kbHook); m_kbHook = nullptr; }
    if (m_msHook) { UnhookWindowsHookEx(m_msHook); m_msHook = nullptr; }
    m_active = false;
    m_lastX = m_lastY = -1;
    
    // 停止监测时恢复光标
    if (m_hideCursor) {
        ShowCursor(TRUE);
    }
}
