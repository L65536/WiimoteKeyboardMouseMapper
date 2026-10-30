#include "OutputRouter.h"
#include "ConfigManager.h"
#include <string>

namespace {
    constexpr WORD MOUSE_WHEEL_UP = 0x1000;
    constexpr WORD MOUSE_WHEEL_DOWN = 0x1001;
}

OutputRouter::OutputRouter() {
    // Initialize state
}

void OutputRouter::Route(const WiiRemoteState& state) {
    auto& config = ConfigManager::GetInstance();
    auto now = std::chrono::steady_clock::now();
    
    auto processButton = [&](const std::string& name, bool current, OutputRouter::RepeatState& rs) {
        if (current) {
            if (!rs.isHeld) {
                // Initial press
                const auto& keys = config.GetMappedKeys(name);
                for (WORD vKey : keys) {
                    SendKey(vKey, true);
                }
                rs.isHeld = true;
                rs.nextRepeat = now + std::chrono::milliseconds(500);
            } else if (now >= rs.nextRepeat) {
                // Auto-repeat
                const auto& keys = config.GetMappedKeys(name);
                for (WORD vKey : keys) {
                    SendKey(vKey, true);
                }
                rs.nextRepeat = now + std::chrono::milliseconds(33); // ~30Hz
            }
        } else if (rs.isHeld) {
            // Release (Reverse order for shortcuts like Ctrl+C)
            const auto& keys = config.GetMappedKeys(name);
            for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
                SendKey(*it, false);
            }
            rs.isHeld = false;
        }
    };

    processButton("A", state.a_pressed, m_repeatStates.a);
    processButton("B", state.b_pressed, m_repeatStates.b);
    processButton("UP", state.dpad_up, m_repeatStates.up);
    processButton("DOWN", state.dpad_down, m_repeatStates.down);
    processButton("LEFT", state.dpad_left, m_repeatStates.left);
    processButton("RIGHT", state.dpad_right, m_repeatStates.right);
    processButton("PLUS", state.plus_pressed, m_repeatStates.plus);
    processButton("MINUS", state.minus_pressed, m_repeatStates.minus);
    processButton("HOME", state.home_pressed, m_repeatStates.home);
    processButton("ONE", state.one_pressed, m_repeatStates.one);
    processButton("TWO", state.two_pressed, m_repeatStates.two);
}

void OutputRouter::Reset() {
    auto& config = ConfigManager::GetInstance();

    auto releaseIfPressed = [&](const std::string& name, OutputRouter::RepeatState& rs) {
        if (rs.isHeld) {
            const auto& keys = config.GetMappedKeys(name);
            for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
                SendKey(*it, false); // Send KEYUP in reverse
            }
            rs.isHeld = false;
        }
    };

    releaseIfPressed("A", m_repeatStates.a);
    releaseIfPressed("B", m_repeatStates.b);
    releaseIfPressed("UP", m_repeatStates.up);
    releaseIfPressed("DOWN", m_repeatStates.down);
    releaseIfPressed("LEFT", m_repeatStates.left);
    releaseIfPressed("RIGHT", m_repeatStates.right);
    releaseIfPressed("PLUS", m_repeatStates.plus);
    releaseIfPressed("MINUS", m_repeatStates.minus);
    releaseIfPressed("HOME", m_repeatStates.home);
    releaseIfPressed("ONE", m_repeatStates.one);
    releaseIfPressed("TWO", m_repeatStates.two);
}

void OutputRouter::SendKey(WORD vKey, bool down) {
    INPUT input = { 0 };
    
    // Check if this is a mouse button or wheel
    if (vKey == VK_LBUTTON || vKey == VK_RBUTTON || vKey == VK_MBUTTON ||
        vKey == MOUSE_WHEEL_UP || vKey == MOUSE_WHEEL_DOWN) {
        input.type = INPUT_MOUSE;
        if (vKey == VK_LBUTTON) {
            input.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        } else if (vKey == VK_RBUTTON) {
            input.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        } else if (vKey == VK_MBUTTON) {
            input.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        } else if (vKey == MOUSE_WHEEL_UP) {
            input.mi.dwFlags = MOUSEEVENTF_WHEEL;
            input.mi.mouseData = WHEEL_DELTA;
            if (!down) return;
        } else if (vKey == MOUSE_WHEEL_DOWN) {
            input.mi.dwFlags = MOUSEEVENTF_WHEEL;
            input.mi.mouseData = static_cast<DWORD>(-static_cast<LONG>(WHEEL_DELTA));
            if (!down) return;
        }
    } else {
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = vKey;
        input.ki.wScan = (WORD)MapVirtualKey(vKey, MAPVK_VK_TO_VSC);
        input.ki.dwFlags = KEYEVENTF_SCANCODE;
        if (!down) input.ki.dwFlags |= KEYEVENTF_KEYUP;

        // Extended key flags for Win keys, Arrows, etc.
        if (vKey == VK_LWIN || vKey == VK_RWIN || vKey == VK_LEFT || vKey == VK_UP || 
            vKey == VK_RIGHT || vKey == VK_DOWN || vKey == VK_PRIOR || vKey == VK_NEXT || 
            vKey == VK_END || vKey == VK_HOME || vKey == VK_INSERT || vKey == VK_DELETE ||
            vKey == VK_DIVIDE || vKey == VK_RCONTROL || vKey == VK_RMENU ||
            vKey == VK_VOLUME_MUTE || vKey == VK_VOLUME_UP || vKey == VK_VOLUME_DOWN) {
            input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        }
    }

    SendInput(1, &input, sizeof(INPUT));
}
