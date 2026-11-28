#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <windows.h>

namespace nx::plat {

constexpr size_t kPcControlCount = 16;
constexpr size_t kPcHotkeyCount = 4;

enum PcControl : size_t {
    PcA, PcB, PcX, PcY, PcL, PcR, PcZL, PcZR, PcPlus, PcMinus, PcLStick, PcRStick,
    PcDpadLeft, PcDpadUp, PcDpadRight, PcDpadDown,
};

enum PcPhysicalButton : int {
    PcSouth, PcEast, PcWest, PcNorth, PcShoulderL, PcShoulderR, PcTriggerL, PcTriggerR,
    PcStart, PcBack, PcStickL, PcStickR, PcLeft, PcUp, PcRight, PcDown,
};

inline int pc_default_controller_binding(size_t action, int preset) {
    constexpr int standard[kPcControlCount] = {
        PcSouth, PcEast, PcWest, PcNorth, PcShoulderL, PcShoulderR, PcTriggerL, PcTriggerR,
        PcStart, PcBack, PcStickL, PcStickR, PcLeft, PcUp, PcRight, PcDown,
    };
    constexpr int nintendo[kPcControlCount] = {
        PcEast, PcSouth, PcNorth, PcWest, PcShoulderL, PcShoulderR, PcTriggerL, PcTriggerR,
        PcStart, PcBack, PcStickL, PcStickR, PcLeft, PcUp, PcRight, PcDown,
    };
    return action < kPcControlCount ? (preset == 1 ? nintendo[action] : standard[action]) : PcSouth;
}

struct PcMenuState {
    int width = 0;
    int height = 0;
    int aspect_ratio = 0;  // 0 original, 1 21:9, 2 32:9, 3 16:10
    int monitor = 0;       // 0..N-1 display monitor index
    int internal_resolution = 0;
    int volume = 100;
    int audio_mode = 0;
    int stereo_separation = 100;
    int language = 0;  // English, Italian, German, French
    int theme = 0;     // UI style color preset (0..4)
    int game_language = 0;  // Language returned to the game at startup: en-US, it, de, fr, pt-BR
    int memory_limit_gb = 8;  // Process private-commit guard; not a dedicated VRAM budget
    std::array<int, 4> player_devices{{0, 0, 0, 0}};
    std::array<std::array<int, kPcControlCount>, 4> pad_bindings{};
    std::array<uint32_t, 4> pad_custom_mask{};
    std::array<int, 4> pad_presets{{0, 0, 0, 0}};
    bool keyboard_separate = false;
    int controller_preset = 0;  // Xbox / DualSense, Nintendo
    int button_icons = 0;       // 0 Auto, 1 Nintendo, 2 Xbox, 3 PlayStation
    bool muted = false;
    bool mute_in_background = false;
    bool pause_in_background = false;
    bool ignore_pad_in_background = false;
    bool fullscreen = false;
    bool always_on_top = false;
    bool metrics_enabled = false;
    bool low_vram_profile = false;  // ASTC textures use BC3 instead of expanded RGBA8 where ASTC is unavailable
    bool discord_rpc = true;
    bool metric_fps = true;
    bool metric_one_percent_low = true;
    bool metric_game_speed = true;
    bool metric_frametime_graph = true;
    int overlay_position = 0;  // top left, top right, bottom left, bottom right
    bool speedrun_timer = false;
    int speedrun_timer_mode = 0;
    int speedrun_timer_position = 2;
    int speedrun_timer_size = 1;
    int speedrun_timer_color = 0;
    int upscaler = 1;          // overlay menu: 0 bilinear, 1 FSR 1
    int fsr_sharpness = 25;    // overlay menu: RCAS stops x 100
    int antialiasing = 0;      // overlay menu: 0 off, 1 FXAA
    int vsync = 1;             // overlay menu: 0 On, 1 Fast, 2 Adaptive, 3 Off
    int frame_cap = 60;        // 30, 60, 90, 120, 144, or 240 FPS
    int hdr = 0, hdr_peak = 1000, hdr_paper_white = 203;  // HDR output (hdr applies after a restart; docs/HDR.md)
    int hdr_highlights = 100, hdr_highlight_start = 100;  // percent
    int hdr_sdr_eotf = 1;                                 // 0 sRGB, 1 gamma 2.2, 2 gamma 2.4
    int hdr_exposure = 100, hdr_contrast = 100, hdr_shadows = 100, hdr_saturation = 100, hdr_highlight_saturation = 100;
    int anisotropy = 0;        // overlay menu: 0 the game's, else 2..16
    int shadow_scale = 1;      // overlay menu: 1, 2, 4
    int rumble = 100;          // overlay menu: rumble strength, percent
    std::array<int, 4> pad_left_deadzones{16, 16, 16, 16};   // per-pad left stick deadzone, percent (0..34)
    std::array<int, 4> pad_right_deadzones{16, 16, 16, 16};  // per-pad right stick deadzone, percent (0..34)
    std::array<int, kPcControlCount> keyboard_bindings = {
        VK_SPACE, VK_SHIFT, 'J', 'K', 'Q', 'E', 'Z', 'C', VK_RETURN, VK_ESCAPE, 'M', 'N',
        VK_LEFT, VK_UP, VK_RIGHT, VK_DOWN,
    };
    std::array<int, kPcControlCount> controller_bindings{};
    uint32_t controller_custom_mask = 0;
    std::array<int, kPcHotkeyCount> hotkey_bindings = {VK_F1, VK_F2, VK_F11, VK_TAB};
    int title_quit_button = PcMinus;
};

using PcMenuApply = void (*)(const PcMenuState&, bool persist);

void pc_menu_init(HWND owner, const PcMenuState& state, PcMenuApply apply);
void pc_menu_toggle();
bool pc_menu_open();
void pc_menu_sync(const PcMenuState& state);
void pc_menu_capture_controller_button(int physical_button);  // may be called by the game input thread

}  // namespace nx::plat
