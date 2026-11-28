// The in-game overlay: the F1 settings menu and the metrics panel, drawn with Dear ImGui into the swapchain image
// just before it is presented (overlay_vk.cpp, called from nvn_vk.cpp's gpu_present). Shared by Windows and Linux;
// the platform feeds input events and applies settings (Host). NX_IMGUI_MENU=0: off (Windows keeps its Win32 menu).
//
// Threads: input functions may be called from any thread (they queue); everything that touches ImGui runs on the
// thread that presents, under the GPU lock.
#pragma once

#include <cstdint>
#include <vector>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "overlay/settings.h"
#include "platform.h"

namespace nx::overlay {

struct Host {
    // Applies `s` (window size, fullscreen, volume, bindings, ...). persist: also write pc-settings.ini. Called on
    // the presenting thread: post anything that needs the UI thread instead of waiting for it.
    void (*apply)(const Settings& s, bool persist) = nullptr;
    // A short name for a keyboard binding (a platform key code), for the Controls tab.
    const char* (*key_name)(int key) = nullptr;
    std::array<int, kControlCount> default_keyboard_bindings{};
    std::array<int, kHotkeyCount> default_hotkey_bindings{};
    // Enumerates available display monitors (e.g. "Monitor 1: LG ULTRAGEAR (1920x1080)").
    std::vector<std::string> (*monitors)() = nullptr;
    bool can_always_on_top = true;  // the window system supports it (not Wayland)
    bool can_resize = true;         // the window can be given a size
};

bool enabled();  // NX_IMGUI_MENU, default on
bool capturing_key();  // true while the Controls tab is waiting for a key press to bind
int menu_language();  // selected launcher / in-game menu language, 0..12
int game_language();  // selected game text language, 0..17
int title_quit_button();        // 0..15 (Control enum, default CtlMinus): pad button held on title screen to quit
void set_game_version(const std::string& version);
const std::string& game_version();
int aspect_ratio();
void set_aspect_ratio(int ratio);
bool low_vram_profile();
void set_low_vram_profile(bool on);
void set_theme_index(int index);
void init(const Host& host, const Settings& s);  // before the first frame
void sync(const Settings& s);                    // the platform changed settings itself (F11, window resize)

// ---- input (any thread) ----
// Navigation/edit keys the menu understands, independent of the platform's key codes.
enum Key : int {
    KeyNone, KeyTab, KeyLeft, KeyRight, KeyUp, KeyDown, KeyPageUp, KeyPageDown, KeyHome, KeyEnd, KeyEnter, KeyEscape,
    KeyBackspace, KeyDelete, KeySpace, KeyShift, KeyCtrl, KeyF1,
};
// `raw` is the platform key code (used when capturing a keyboard binding), `key` its menu meaning (KeyNone if any).
void key_event(int raw, Key key, bool down);
void mouse_move(float x, float y);  // pixels in the swapchain image
void mouse_button(int button, bool down);  // 0 left, 1 right, 2 middle
void mouse_wheel(float dy);
void text_input(uint32_t codepoint);
void toggle();
bool is_open();
// A full-window screen drawn under the menu every frame, with the overlay's look and input (the launcher,
// launcher/launcher.cpp): called inside the ImGui frame with the image size. Null: none. Input goes to the overlay
// while one is shown; the menu (toggle) opens over it.
using ScreenFn = void (*)(float w, float h);
void set_screen(ScreenFn fn);
bool screen_shown();
uint64_t pad_buttons();  // the player's pad as the overlay last saw it (NpadButton bits, through the bindings)
bool wants_mouse();     // the pointer is over the menu or the menu is open
bool wants_keyboard();  // keys go to the menu, not the game
// Called from plat::read_pad with the player's pad: navigates the menu with a controller, toggles it with
// Minus + Plus held together, and clears `p` while the menu is open so the game does not see the presses.
void filter_pad(plat::PadState& p);

// ---- metrics (any thread) ----
void presented(uint64_t now_ns);   // after each present (plat::overlay_presented)
void game_interval(int interval);  // the game's swap interval in 60 Hz ticks (plat::overlay_game_interval)
void game_speed(double percent);  // applied simulation time divided by elapsed wall time

// ---- speedrun timer (any thread) ----
void speedrun_set_course(bool in_course, const char* world_name = nullptr, const char* course_name = nullptr);
void speedrun_reset_course();
void speedrun_begin_play();
bool speedrun_is_armed();
void speedrun_finish_course();
void speedrun_tick(double game_seconds, double wall_seconds, bool game_paused = false);

// ---- startup shader preparation (any thread) ----
// What the compile screen shows: `done` of `total`, kind 0 compiling the game's shaders, 1 preparing pipelines seen in
// earlier sessions. visible false: finished (the screen fades out over the first game frames).
void shader_progress(uint64_t done, uint64_t total, bool visible, int kind);

// ---- notices (any thread) ----
void notify(const char* text);  // a short notice in the bottom-right corner (overlay_ui.h)
void notify_screenshot(const uint8_t* rgba, int w, int h);  // top-right preview toast ("Screenshot Saved")

// ---- rendering (the presenting thread, GPU lock held) ----
// Draws into swapchain image `index` (in PRESENT_SRC layout, left in it). `images` are the swapchain's images; a
// change re-makes the framebuffers. False when nothing was drawn.
bool record(VkCommandBuffer cb, uint32_t index, const std::vector<VkImage>& images, VkFormat format, VkExtent2D extent);
void release();  // the swapchain or device goes away

}  // namespace nx::overlay
