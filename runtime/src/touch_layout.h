// Drawn touch controls: the per-game button layout (touch_layout.cpp).
#pragma once

#include <cstddef>
#include <cstdint>

#include "platform.h"

namespace nx::plat {

// Sentinel bit for the on-screen F1 menu toggle button (not a Switch pad button).
inline constexpr uint64_t kTouchMenu = 1ull << 63;

struct TouchButton {
    float x, y, r;  // centre across the landscape screen (0..1), radius as a fraction of its height
    uint64_t button;
    const char* label;
};
// The game's layout (count 0: the game handles touch its own way).
const TouchButton* touch_buttons(size_t& count);
TouchButton* touch_buttons_mut(size_t& count);
// The button under (x, y) (0..1 across the screen; aspect = width / height), or 0.
uint64_t touch_hit(float x, float y, float aspect);

// Layout persistence & interactive editor state (Android).
void touch_edit_enter();
bool touch_edit_active();
void touch_edit_save();
void touch_edit_cancel();
void touch_reset_layout();
void touch_save_layout();

// Live touch state for drawing labels/highlights in overlay.cpp.
uint64_t touch_held_buttons();
bool touch_using_gamepad();
bool touch_stick_visual(float& base_x, float& base_y, float& knob_x, float& knob_y, bool& active);

}  // namespace nx::plat
