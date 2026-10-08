#ifndef INPUT_HOOK_H
#define INPUT_HOOK_H

#include <windows.h>
#include <cstdint>
#include <functional>

class InputHook {
public:
    using KeyCallback = std::function<void(uint16_t keyCode, bool pressed)>;
    using MouseCallback = std::function<void(float dx, float dy)>;
    using MouseBtnCallback = std::function<void(int button, bool pressed)>;

    static InputHook& Get();

    bool Start();
    void Stop();
    bool IsActive() const { return m_active; }

    void SetKeyCallback(KeyCallback cb) { m_keyCb = std::move(cb); }
    void SetMouseCallback(MouseCallback cb) { m_mouseCb = std::move(cb); }
    void SetMouseBtnCallback(MouseBtnCallback cb) { m_btnCb = std::move(cb); }
    
    void SetHideCursor(bool hide) { m_hideCursor = hide; }

private:
    InputHook() = default;
    static LRESULT CALLBACK KeyboardProc(int code, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK MouseProc(int code, WPARAM wParam, LPARAM lParam);

    HHOOK m_kbHook = nullptr, m_msHook = nullptr;
    bool m_active = false;
    bool m_hideCursor = true;  // 默认隐藏光标
    KeyCallback m_keyCb;
    MouseCallback m_mouseCb;
    MouseBtnCallback m_btnCb;
    int m_lastX = -1, m_lastY = -1;
};

#endif
