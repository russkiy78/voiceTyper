// Windows paste keystroke synthesis via SendInput (Ctrl+V).

#include "clipboard/KeyboardPaster.h"

#include <windows.h>

namespace vt {

namespace {

void pressKey(WORD vk, bool down, INPUT& in) {
    in = {};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
}

class WinKeyboardPaster final : public KeyboardPaster {
public:
    bool sendPaste() override {
        INPUT inputs[4] = {};
        pressKey(VK_CONTROL, true, inputs[0]);
        pressKey('V', true, inputs[1]);
        pressKey('V', false, inputs[2]);
        pressKey(VK_CONTROL, false, inputs[3]);
        const UINT sent = SendInput(4, inputs, sizeof(INPUT));
        return sent == 4;
    }
};

} // namespace

std::unique_ptr<KeyboardPaster> KeyboardPaster::create() {
    return std::make_unique<WinKeyboardPaster>();
}

} // namespace vt
