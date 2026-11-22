#pragma once
#include <cstdint>

namespace nx {

// Preset selected in the Windows quick settings; zero leaves the game's default mode unchanged.
void set_internal_resolution_preset(int preset);
int internal_resolution_preset();
float internal_resolution_scale();
uint32_t internal_resolution_target_width();
void set_native_render_width(uint32_t width);  // internal resolution Native: the presented width
void set_native_render_size(uint32_t width, uint32_t height);
void native_display_size(uint32_t& width, uint32_t& height);
void set_game_language(int language);
uint64_t game_language_code();

}  // namespace nx
