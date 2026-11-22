// Between the overlay's parts (overlay.cpp: state, input, metrics, frame; overlay_menu.cpp: the menu's tabs;
// overlay_vk.cpp: drawing). Everything here runs on the presenting thread.
#pragma once

#include <cstdint>

#include "overlay/overlay.h"

struct ImDrawData;

namespace nx::overlay {

// Whether this frame has anything to show (the menu is open, the metrics panel is on, or input is pending): when not,
// ImGui is not run at all.
bool frame_wanted();
void ensure_context();  // the ImGui context, style and fonts
// The frame's ImGui content for an image of `w` x `h` pixels; null when there is nothing to draw.
ImDrawData* build_frame(uint32_t w, uint32_t h);

// overlay_menu.cpp
struct MenuContext {
    Settings& settings;
    const Host& host;
    int& capture_action;       // keyboard binding being captured (-1: none)
    int tab_request;           // -1, or the tab to select (shoulder buttons)
    bool& open;
    float scale;     // the image height / 720
    bool appearing;  // first frame after opening
    float height;    // the image height in pixels (the style is sized from it)
    float reveal;    // 0..1: the open / close fade
};
void draw_menu(MenuContext& c);
int menu_tab_count();
const char* const* menu_language_names();  // 10 entries

// overlay.cpp: apply and persist after a change made in the menu
void settings_changed(bool persist);

// overlay_progress.cpp: the startup shader-compile screen (shader_progress in overlay.h feeds it)
bool progress_wanted();
void draw_progress(float height);

// overlay_vk.cpp: upload an RGBA8 image for ImGui rendering (returns ImTextureID as uint64_t, 0 on failure)
uint64_t vk_upload_rgba_texture(const uint8_t* rgba, int w, int h);

}  // namespace nx::overlay
