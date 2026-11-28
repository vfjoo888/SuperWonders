#include "settings.h"

#include <algorithm>
#include <cstdio>
#include <fstream>

namespace nx::overlay {

int default_controller_binding(size_t action, int preset) {
    constexpr int standard[kControlCount] = {
        PhySouth, PhyEast, PhyWest, PhyNorth, PhyShoulderL, PhyShoulderR, PhyTriggerL, PhyTriggerR,
        PhyStart, PhyBack, PhyStickL, PhyStickR, PhyLeft, PhyUp, PhyRight, PhyDown,
    };
    constexpr int nintendo[kControlCount] = {
        PhyEast, PhySouth, PhyNorth, PhyWest, PhyShoulderL, PhyShoulderR, PhyTriggerL, PhyTriggerR,
        PhyStart, PhyBack, PhyStickL, PhyStickR, PhyLeft, PhyUp, PhyRight, PhyDown,
    };
    return action < kControlCount ? (preset == 1 ? nintendo[action] : standard[action]) : PhySouth;
}

int controller_binding(const Settings& s, size_t action) {
    if (action >= kControlCount) return PhySouth;
    return s.controller_custom_mask & (1u << action) ? std::clamp(s.controller_bindings[action], 0, int(kControlCount) - 1)
                                                     : default_controller_binding(action, s.controller_preset);
}

int controller_preset_for_slot(const Settings& s, size_t slot) {
    if (slot == 0 || slot >= 4) return std::clamp(s.controller_preset, 0, 1);
    return std::clamp(s.pad_presets[slot], 0, 1);
}

int controller_binding_for_slot(const Settings& s, size_t slot, size_t action) {
    if (slot == 0 || slot >= 4 || action >= kControlCount) return controller_binding(s, action);
    return s.pad_custom_mask[slot] & (1u << action) ? std::clamp(s.pad_bindings[slot][action], 0, int(kControlCount) - 1)
                                                     : default_controller_binding(action, controller_preset_for_slot(s, slot));
}

bool load_settings(const std::string& path, Settings& s) {
    std::ifstream file(path);
    if (!file) return false;
    std::string line;
    int game_language_version = 0;
    while (std::getline(file, line)) {
        int v = 0;
        unsigned u = 0;
        const char* l = line.c_str();
        if (std::sscanf(l, "width=%d", &v) == 1 && v >= 640 && v <= 7680) s.width = v;
        else if (std::sscanf(l, "height=%d", &v) == 1 && v >= 360 && v <= 4320) s.height = v;
        else if (std::sscanf(l, "aspect_ratio=%d", &v) == 1) s.aspect_ratio = std::clamp(v, 0, 3);
        else if (std::sscanf(l, "monitor=%d", &v) == 1) s.monitor = std::max(0, v);
        else if (std::sscanf(l, "internal_resolution=%d", &v) == 1) s.internal_resolution = std::clamp(v, 0, 9);
        else if (std::sscanf(l, "volume=%d", &v) == 1) s.volume = std::clamp(v, 0, 200);
        else if (std::sscanf(l, "audio_mode=%d", &v) == 1) s.audio_mode = std::clamp(v, 0, 2);
        else if (std::sscanf(l, "stereo_separation=%d", &v) == 1) s.stereo_separation = std::clamp(v, 0, 100);
        else if (std::sscanf(l, "muted=%d", &v) == 1) s.muted = v != 0;
        else if (std::sscanf(l, "mute_in_background=%d", &v) == 1) s.mute_in_background = v != 0;
        else if (std::sscanf(l, "pause_in_background=%d", &v) == 1) s.pause_in_background = v != 0;
        else if (std::sscanf(l, "ignore_pad_in_background=%d", &v) == 1) s.ignore_pad_in_background = v != 0;
        else if (std::sscanf(l, "fullscreen=%d", &v) == 1) s.fullscreen = v != 0;
        else if (std::sscanf(l, "always_on_top=%d", &v) == 1) s.always_on_top = v != 0;
        else if (std::sscanf(l, "language=%d", &v) == 1) s.language = std::clamp(v, 0, 12);
        else if (std::sscanf(l, "theme=%d", &v) == 1) s.theme = std::clamp(v, 0, 4);
        else if (std::sscanf(l, "game_language=%d", &v) == 1) s.game_language = std::clamp(v, 0, 17);
        else if (std::sscanf(l, "game_language_version=%d", &v) == 1) game_language_version = v;
        else if (std::sscanf(l, "memory_limit_gb=%d", &v) == 1) s.memory_limit_gb = std::clamp(v, 2, 32);
        else if (std::sscanf(l, "low_vram_profile=%d", &v) == 1) s.low_vram_profile = v != 0;
        else if (std::sscanf(l, "discord_rpc=%d", &v) == 1) s.discord_rpc = v != 0;
        else if (std::sscanf(l, "keyboard_separate=%d", &v) == 1) s.keyboard_separate = v != 0;
        else if (std::sscanf(l, "controller_preset=%d", &v) == 1) {
            s.controller_preset = std::clamp(v, 0, 1);
            s.pad_presets[0] = s.controller_preset;
        }
        else if (std::sscanf(l, "button_icons=%d", &v) == 1) s.button_icons = std::clamp(v, 0, 3);
        else if (std::sscanf(l, "metrics_enabled=%d", &v) == 1) s.metrics_enabled = v != 0;
        else if (std::sscanf(l, "metric_fps=%d", &v) == 1) s.metric_fps = v != 0;
        else if (std::sscanf(l, "metric_one_percent_low=%d", &v) == 1) s.metric_one_percent_low = v != 0;
        else if (std::sscanf(l, "metric_game_speed=%d", &v) == 1) s.metric_game_speed = v != 0;
        else if (std::sscanf(l, "metric_frametime_graph=%d", &v) == 1) s.metric_frametime_graph = v != 0;
        else if (std::sscanf(l, "overlay_position=%d", &v) == 1) s.overlay_position = std::clamp(v, 0, 3);
        else if (std::sscanf(l, "speedrun_timer=%d", &v) == 1) s.speedrun_timer = v != 0;
        else if (std::sscanf(l, "speedrun_timer_mode=%d", &v) == 1) s.speedrun_timer_mode = std::clamp(v, 0, 2);
        else if (std::sscanf(l, "speedrun_timer_position=%d", &v) == 1) s.speedrun_timer_position = std::clamp(v, 0, 5);
        else if (std::sscanf(l, "speedrun_timer_size=%d", &v) == 1) s.speedrun_timer_size = std::clamp(v, 0, 3);
        else if (std::sscanf(l, "speedrun_timer_color=%d", &v) == 1) s.speedrun_timer_color = std::clamp(v, 0, 5);
        else if (std::sscanf(l, "upscaler=%d", &v) == 1) s.upscaler = std::clamp(v, 0, 1);
        else if (std::sscanf(l, "fsr_sharpness=%d", &v) == 1) s.fsr_sharpness = std::clamp(v, 0, 200);
        else if (std::sscanf(l, "antialiasing=%d", &v) == 1) s.antialiasing = std::clamp(v, 0, 2);
        else if (std::sscanf(l, "anisotropy=%d", &v) == 1) s.anisotropy = std::clamp(v, 0, 16);
        else if (std::sscanf(l, "shadow_scale=%d", &v) == 1) s.shadow_scale = (v == 2 || v == 4) ? v : 1;
        else if (std::sscanf(l, "rumble=%d", &v) == 1) s.rumble = std::clamp(v, 0, 100);
        else if (std::sscanf(l, "stick_deadzone=%d", &v) == 1)
            s.pad_left_deadzones.fill(std::clamp(v, 0, 34)), s.pad_right_deadzones.fill(std::clamp(v, 0, 34));
        else if (std::sscanf(l, "left_stick_deadzone=%d", &v) == 1) s.pad_left_deadzones.fill(std::clamp(v, 0, 34));
        else if (std::sscanf(l, "right_stick_deadzone=%d", &v) == 1) s.pad_right_deadzones.fill(std::clamp(v, 0, 34));
        else if (std::sscanf(l, "vsync=%d", &v) == 1) s.vsync = std::clamp(v, 0, 3);
        else if (std::sscanf(l, "frame_cap=%d", &v) == 1)
            s.frame_cap = v == 30 || v == 60 || v == 90 || v == 120 || v == 144 || v == 240 ? v : 60;
        else if (std::sscanf(l, "hdr=%d", &v) == 1) s.hdr = v != 0;
        else if (std::sscanf(l, "hdr_peak=%d", &v) == 1) s.hdr_peak = std::clamp(v, 400, 10000);
        else if (std::sscanf(l, "hdr_paper_white=%d", &v) == 1) s.hdr_paper_white = std::clamp(v, 80, 500);
        else if (std::sscanf(l, "hdr_highlights=%d", &v) == 1) s.hdr_highlights = std::clamp(v, 0, 300);
        else if (std::sscanf(l, "hdr_highlight_start=%d", &v) == 1) s.hdr_highlight_start = std::clamp(v, 50, 300);
        else if (std::sscanf(l, "hdr_sdr_eotf=%d", &v) == 1) s.hdr_sdr_eotf = std::clamp(v, 0, 2);
        else if (std::sscanf(l, "hdr_exposure=%d", &v) == 1) s.hdr_exposure = std::clamp(v, 25, 300);
        else if (std::sscanf(l, "hdr_contrast=%d", &v) == 1) s.hdr_contrast = std::clamp(v, 25, 200);
        else if (std::sscanf(l, "hdr_shadows=%d", &v) == 1) s.hdr_shadows = std::clamp(v, 25, 175);
        else if (std::sscanf(l, "hdr_saturation=%d", &v) == 1) s.hdr_saturation = std::clamp(v, 0, 300);
        else if (std::sscanf(l, "hdr_highlight_saturation=%d", &v) == 1) s.hdr_highlight_saturation = std::clamp(v, 0, 300);
        else if (std::sscanf(l, "controller_custom_mask=%u", &u) == 1) s.controller_custom_mask = u;
        else if (std::sscanf(l, "title_quit_button=%d", &v) == 1)
            s.title_quit_button = std::clamp(v, 0, int(kControlCount) - 1);
        else {
            int index = -1, binding = -1, slot = -1;
            unsigned mask = 0;
            if (std::sscanf(l, "player_device_%d=%d", &index, &binding) == 2 && index >= 0 && index < 4 && binding >= 0 && binding <= 6)
                s.player_devices[size_t(index)] = binding;
            else if (std::sscanf(l, "pad_preset_%d=%d", &slot, &binding) == 2 && slot > 0 && slot < 4)
                s.pad_presets[size_t(slot)] = std::clamp(binding, 0, 1);
            else if (std::sscanf(l, "pad_deadzone_l_%d=%d", &slot, &binding) == 2 && slot >= 0 && slot < 4)
                s.pad_left_deadzones[size_t(slot)] = std::clamp(binding, 0, 34);
            else if (std::sscanf(l, "pad_deadzone_r_%d=%d", &slot, &binding) == 2 && slot >= 0 && slot < 4)
                s.pad_right_deadzones[size_t(slot)] = std::clamp(binding, 0, 34);
            else if (std::sscanf(l, "pad_mask_%d=%u", &slot, &mask) == 2 && slot > 0 && slot < 4)
                s.pad_custom_mask[size_t(slot)] = mask;
            else if (std::sscanf(l, "pad_%d_%d=%d", &slot, &index, &binding) == 3 && slot > 0 && slot < 4 && index >= 0 && index < int(kControlCount) && binding >= 0 && binding < int(kControlCount))
                s.pad_bindings[size_t(slot)][size_t(index)] = binding;
            else if (std::sscanf(l, "keyboard_%d=%d", &index, &binding) == 2 && index >= 0 && index < int(kControlCount) &&
                binding >= 0 && binding <= 511)
                s.keyboard_bindings[size_t(index)] = binding;
            else if (std::sscanf(l, "controller_%d=%d", &index, &binding) == 2 && index >= 0 &&
                     index < int(kControlCount) && binding >= 0 && binding < int(kControlCount))
                s.controller_bindings[size_t(index)] = binding;
            else if (std::sscanf(l, "hotkey_%d=%d", &index, &binding) == 2 && index >= 0 &&
                     index < int(kHotkeyCount) && binding >= 0 && binding <= 511)
                s.hotkey_bindings[size_t(index)] = binding;
        }
    }
    // Before version 2, IDs 8 and 10 were Arabic and Turkish. They are no
    // longer selectable game languages; do not reinterpret saved values as Dutch/Chinese.
    if (game_language_version < 2 && (s.game_language == 8 || s.game_language == 10)) s.game_language = 0;
    return true;
}

void save_settings(const std::string& path, const Settings& s) {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return;
    f << "width=" << s.width << "\nheight=" << s.height << "\nvolume=" << s.volume
      << "\naudio_mode=" << s.audio_mode << "\nstereo_separation=" << s.stereo_separation
      << "\naspect_ratio=" << std::max(0, s.aspect_ratio)
      << "\nmonitor=" << std::max(0, s.monitor)
      << "\ninternal_resolution=" << s.internal_resolution << "\nmuted=" << s.muted
      << "\nmute_in_background=" << s.mute_in_background << "\npause_in_background=" << s.pause_in_background
      << "\nignore_pad_in_background=" << s.ignore_pad_in_background << "\nfullscreen=" << s.fullscreen
      << "\nalways_on_top=" << s.always_on_top << "\nlanguage=" << s.language << "\ntheme=" << s.theme << "\ngame_language=" << s.game_language
      << "\ngame_language_version=2"
      << "\nmemory_limit_gb=" << s.memory_limit_gb << "\nlow_vram_profile=" << s.low_vram_profile
      << "\ndiscord_rpc=" << s.discord_rpc
      << "\nkeyboard_separate=" << s.keyboard_separate
      << "\ncontroller_preset=" << s.controller_preset << "\nbutton_icons=" << s.button_icons
      << "\nmetrics_enabled=" << s.metrics_enabled
      << "\nmetric_fps=" << s.metric_fps << "\nmetric_one_percent_low=" << s.metric_one_percent_low
      << "\nmetric_game_speed=" << s.metric_game_speed << "\nmetric_frametime_graph=" << s.metric_frametime_graph
      << "\noverlay_position=" << s.overlay_position
      << "\nspeedrun_timer=" << s.speedrun_timer << "\nspeedrun_timer_mode=" << s.speedrun_timer_mode
      << "\nspeedrun_timer_position=" << s.speedrun_timer_position
      << "\nspeedrun_timer_size=" << s.speedrun_timer_size
      << "\nspeedrun_timer_color=" << s.speedrun_timer_color
      << "\ncontroller_custom_mask=" << s.controller_custom_mask
      << "\nupscaler=" << s.upscaler << "\nfsr_sharpness=" << s.fsr_sharpness << "\nantialiasing=" << s.antialiasing
      << "\nanisotropy=" << s.anisotropy << "\nshadow_scale=" << s.shadow_scale << "\nrumble=" << s.rumble
      << "\nvsync=" << s.vsync
      << "\nframe_cap=" << s.frame_cap
      << "\nhdr=" << s.hdr << "\nhdr_peak=" << s.hdr_peak << "\nhdr_paper_white=" << s.hdr_paper_white
      << "\nhdr_highlights=" << s.hdr_highlights << "\nhdr_highlight_start=" << s.hdr_highlight_start
      << "\nhdr_sdr_eotf=" << s.hdr_sdr_eotf << "\nhdr_exposure=" << s.hdr_exposure
      << "\nhdr_contrast=" << s.hdr_contrast << "\nhdr_shadows=" << s.hdr_shadows
      << "\nhdr_saturation=" << s.hdr_saturation << "\nhdr_highlight_saturation=" << s.hdr_highlight_saturation << '\n';
    for (size_t slot = 0; slot < 4; ++slot) {
        f << "player_device_" << slot << '=' << s.player_devices[slot] << '\n';
        f << "pad_deadzone_l_" << slot << '=' << std::clamp(s.pad_left_deadzones[slot], 0, 34) << '\n';
        f << "pad_deadzone_r_" << slot << '=' << std::clamp(s.pad_right_deadzones[slot], 0, 34) << '\n';
        if (slot) {
            f << "pad_preset_" << slot << '=' << std::clamp(s.pad_presets[slot], 0, 1) << '\n';
            f << "pad_mask_" << slot << '=' << s.pad_custom_mask[slot] << '\n';
            for (size_t action = 0; action < kControlCount; ++action)
                f << "pad_" << slot << '_' << action << '=' << s.pad_bindings[slot][action] << '\n';
        }
    }
    for (size_t i = 0; i < kControlCount; ++i)
        f << "keyboard_" << i << '=' << s.keyboard_bindings[i] << '\n'
          << "controller_" << i << '=' << s.controller_bindings[i] << '\n';
    for (size_t i = 0; i < kHotkeyCount; ++i)
        f << "hotkey_" << i << '=' << s.hotkey_bindings[i] << '\n';
    f << "title_quit_button=" << std::clamp(s.title_quit_button, 0, int(kControlCount) - 1) << '\n';
}

}  // namespace nx::overlay
