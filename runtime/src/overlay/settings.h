// Player settings shared by every PC platform: what the F1 menu shows and changes, kept in <data root>/pc-settings.ini
// (the same file and keys as the Win32 menu's, pc_menu_win.cpp, so either menu reads the other's file). Key bindings
// are platform key codes: Windows virtual keys, SDL scancodes on Linux.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace nx::overlay {

constexpr size_t kControlCount = 16;

// The 16 NX actions, in pad bit order of the bindings arrays.
enum Control : size_t {
    CtlA, CtlB, CtlX, CtlY, CtlL, CtlR, CtlZL, CtlZR, CtlPlus, CtlMinus, CtlLStick, CtlRStick,
    CtlLeft, CtlUp, CtlRight, CtlDown,
};

// Physical controller buttons, by position (SDL's south/east/west/north naming).
enum Physical : int {
    PhySouth, PhyEast, PhyWest, PhyNorth, PhyShoulderL, PhyShoulderR, PhyTriggerL, PhyTriggerR,
    PhyStart, PhyBack, PhyStickL, PhyStickR, PhyLeft, PhyUp, PhyRight, PhyDown,
};

struct Settings {
    int width = 1280, height = 720;
    int aspect_ratio = 0;  // 0 original, 1 21:9, 2 32:9
    int internal_resolution = 0;  // svc_am.cpp presets: 0 original/dynamic, 1..5 720p..2160p, 6 native, 7 540p, 8 480p, 9 360p
    int volume = 100;
    int audio_mode = 0;         // 0 Stereo, 1 Mono, 2 5.1 Surround (PCM)
    int stereo_separation = 100;  // 0..100% (used in Stereo mode)
    int language = 0;       // menu language: English, Italian, German, French, Spanish, Korean, Japanese, Arabic, Russian, Turkish, Dutch, Simplified Chinese, Traditional Chinese
    int theme = 0;          // UI style color preset: 0 Obsidian, 1 Cobalt, 2 Emerald, 3 Amber, 4 Crimson
    int game_language = 0;  // en-US, it, de, fr, pt-BR, es, ko, ja, nl, ru, zh-Hans, zh-Hant, pt, es-419, fr-CA, en-GB, th, pl
    int memory_limit_gb = 8;
    std::array<int, 4> player_devices{{0, 0, 0, 0}}; // 0 automatic, 1 none, 2 keyboard, 3..6 controller slots
    std::array<std::array<int, kControlCount>, 4> pad_bindings{};
    std::array<uint32_t, 4> pad_custom_mask{};
    std::array<int, 4> pad_presets{{1, 1, 1, 1}};
    bool keyboard_separate = false;  // keyboard is player 1 and controllers start at player 2
    int controller_preset = 1;  // 1 Nintendo positions (the default, as on the Switch: A on the right), 0 Xbox / DualSense
    int button_icons = 0;       // 0 Auto, 1 Nintendo, 2 Xbox, 3 PlayStation (applies after a restart)
    bool muted = false, fullscreen = false, always_on_top = false;
    bool touch_overlay = true;
    bool metrics_enabled = false, low_vram_profile = false;
    bool discord_rpc = true;
    bool metric_fps = true, metric_one_percent_low = true, metric_game_speed = true, metric_frametime_graph = true;
    int overlay_position = 0;  // top left, top right, bottom left, bottom right
    bool speedrun_timer = false;
    int speedrun_timer_mode = 0;      // 0: IGT, 1: RTA, 2: Both (IGT + RTA)
    int speedrun_timer_position = 2;  // 0: top left, 1: top center, 2: top right, 3: bottom left, 4: bottom center, 5: bottom right
    int speedrun_timer_size = 1;      // 0: small, 1: normal, 2: large, 3: extra large
    int speedrun_timer_color = 0;     // 0: white, 1: gold, 2: cyan, 3: green, 4: red, 5: pink
    int upscaler = 1;          // 0 bilinear, 1 FSR 1 (when the game's image is smaller than the window)
    int fsr_sharpness = 25;    // RCAS sharpness in hundredths of a stop: 0 the sharpest, 200 the softest
    int antialiasing = 0;      // 0 off, 1 FXAA (on the presented image)
    int anisotropy = 0;        // 0 the game's own, else 2, 4, 8, 16 (applies after a restart)
    int rumble = 100;          // controller rumble strength, percent (0: off)
    int vsync = 1;             // 0 On (FIFO), 1 Fast (MAILBOX, else IMMEDIATE: the default), 2 Adaptive (FIFO_RELAXED), 3 Off
    int frame_cap = 60;        // 30, 60, 90, 120, 144, or 240 FPS
    std::array<int, kControlCount> keyboard_bindings{};
    std::array<int, kControlCount> controller_bindings{};
    uint32_t controller_custom_mask = 0;  // bit per action: controller_bindings[action] instead of the preset
};

// The physical button a preset gives an action.
int default_controller_binding(size_t action, int preset);
// The physical button an action is bound to now.
int controller_binding(const Settings& s, size_t action);
int controller_preset_for_slot(const Settings& s, size_t slot);
int controller_binding_for_slot(const Settings& s, size_t slot, size_t action);

// Window size presets per aspect ratio (0: 16:9, 1: 21:9, 2: 32:9).
inline constexpr int kWindowSizesByAspect[3][4][2] = {
    {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}},
    {{1706, 720}, {2134, 900}, {2560, 1080}, {3440, 1440}},
    {{2560, 720}, {3200, 900}, {3840, 1080}, {5120, 1440}},
};

inline void adjust_window_size_for_aspect(int new_aspect, int& width, int& height) {
    const int dst = new_aspect < 0 ? 0 : new_aspect > 2 ? 2 : new_aspect;
    for (int a = 0; a < 3; ++a) {
        for (int i = 0; i < 4; ++i) {
            if (kWindowSizesByAspect[a][i][0] == width && kWindowSizesByAspect[a][i][1] == height) {
                width = kWindowSizesByAspect[dst][i][0];
                height = kWindowSizesByAspect[dst][i][1];
                return;
            }
        }
    }
    if (height > 0) {
        const int num = dst == 1 ? 64 : dst == 2 ? 32 : 16;
        const int den = dst == 1 ? 27 : 9;
        int w = (height * num + den / 2) / den;
        w &= ~1;
        if (w < 640) w = 640;
        if (w > 7680) w = 7680;
        width = w;
    }
}

// Reads `path` over `s` (missing or invalid keys keep their value). False when there is no file.
bool load_settings(const std::string& path, Settings& s);
void save_settings(const std::string& path, const Settings& s);

}  // namespace nx::overlay
