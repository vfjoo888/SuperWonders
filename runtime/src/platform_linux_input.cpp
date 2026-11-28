// Linux input and player settings: the first SDL gamepad and the keyboard through the settings' bindings, the
// pc-settings.ini settings applied (window, volume, bindings, ...), and keys and mouse handed to the overlay menu
// (overlay/). Split from platform_linux.cpp (700-line rule).
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <string_view>
static inline std::filesystem::path nx_u8path(std::string_view s) { return std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(s.data()), s.size())); }
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

#include <SDL3/SDL.h>

#include <nx/runtime.h>

#include "nvn/nvn_vk.h"
#include "overlay/overlay.h"
#include "overlay/settings.h"
#include "platform.h"
#include "platform_linux.h"
#include "svc_am.h"

namespace nx::plat {

namespace {

using linux_host::g_stream;
using linux_host::g_window;

std::mutex g_pad_lock;
PadState g_pad;
std::array<PadState, 4> g_players{};
std::array<bool, 4> g_connected{true, false, false, false};
std::atomic<bool> g_keyboard_separate{false};

// Player settings (overlay/settings.h). Keyboard bindings are SDL scancodes. g_settings_lock guards g_settings; the
// bindings are copied into atomics for the input sampling.
std::mutex g_settings_lock;
overlay::Settings g_settings;
std::array<std::atomic<int>, overlay::kControlCount> g_key_binding{};
std::array<std::array<std::atomic<int>, overlay::kControlCount>, 4> g_pad_binding{};
std::array<std::atomic<int>, 4> g_assignments{};
std::array<int, 4> g_player_slots{{0, -1, -1, -1}};
std::array<int, 4> g_pad_styles{};
std::atomic<int> g_button_icons{0};
std::atomic<int> g_controller_preset{1};
constexpr uint64_t kActionBits[overlay::kControlCount] = {
    kA, kB, kX, kY, kL, kR, kZL, kZR, kPlus, kMinus, kStickL, kStickR, kLeft, kUp, kRight, kDown,
};

std::atomic<bool> g_fast_forward{false};  // fast-forward hotkey held (sampled on the main thread)
std::array<std::atomic<int>, overlay::kHotkeyCount> g_hotkey_bindings{{{SDL_SCANCODE_F1}, {SDL_SCANCODE_F2}, {SDL_SCANCODE_F11}, {SDL_SCANCODE_TAB}}};
// The open gamepad: sampled on the main thread, rumbled from the game's (both under g_gamepad_lock).
std::mutex g_gamepad_lock;
std::array<SDL_Gamepad*, 4> g_gamepads{};
std::array<std::atomic<uint64_t>, 4> g_pending_rumble{};
std::atomic<int> g_rumble_strength{100};  // percent
std::array<std::atomic<int>, 4> g_pad_left_deadzone{{{16}, {16}, {16}, {16}}};
std::array<std::atomic<int>, 4> g_pad_right_deadzone{{{16}, {16}, {16}, {16}}};
std::atomic<bool> g_window_focused{true};
std::atomic<bool> g_pause_in_background{false};
std::atomic<bool> g_ignore_pad_in_background{false};

void update_audio_gain(const overlay::Settings& s) {
    if (!g_stream) return;
    const bool muted = s.muted || (s.mute_in_background && !g_window_focused.load(std::memory_order_relaxed)) || paused();
    const int percent = std::clamp(s.volume, 0, 200);
    const float gain = percent <= 100 ? (float(percent) / 100.0f) * (float(percent) / 100.0f)
                                      : float(percent) / 100.0f;
    SDL_SetAudioStreamGain(g_stream, muted ? 0.0f : gain);
}

std::mutex g_posted_lock;
std::deque<std::function<void()>> g_posted;

void post_to_main(std::function<void()> fn) {  // runs on the main thread soon; does not wait
    std::lock_guard lk(g_posted_lock);
    g_posted.push_back(std::move(fn));
}

std::string settings_path() { return data_root() + "/pc-settings.ini"; }

void apply_settings(const overlay::Settings& s, bool persist) {
    overlay::Settings before;
    {
        std::lock_guard lk(g_settings_lock);
        before = g_settings;
        g_settings = s;
    }
    for (size_t a = 0; a < overlay::kControlCount; ++a) {
        g_key_binding[a] = s.keyboard_bindings[a];
        for (size_t slot = 0; slot < 4; ++slot) g_pad_binding[slot][a] = overlay::controller_binding_for_slot(s, slot, a);
    }
    for (size_t i = 0; i < 4; ++i) g_assignments[i] = s.player_devices[i];
    g_pause_in_background.store(s.pause_in_background, std::memory_order_relaxed);
    g_ignore_pad_in_background.store(s.ignore_pad_in_background, std::memory_order_relaxed);
    update_audio_gain(s);
    nx::set_audio_output(s.audio_mode, s.stereo_separation);
    nx::set_internal_resolution_preset(s.internal_resolution);  // read at the next boot
    int nw = s.width, nh = s.height;
    if (const SDL_DisplayMode* dm = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay()); dm && dm->w > 0 && dm->h > 0)
        nw = dm->w, nh = dm->h;
    if (nw > 0 && nh > 0) nx::set_native_render_size(uint32_t(nw), uint32_t(nh));
    nx::set_game_language(s.game_language);
    set_memory_limit_gb(s.memory_limit_gb);
    setenv("NX_LOW_VRAM_PROFILE", s.low_vram_profile ? "1" : "0", 1);
    nvn::set_fsr(s.upscaler == 1, float(s.fsr_sharpness) / 100.0f);
    nvn::set_aa_mode(s.antialiasing);
    nvn::set_anisotropy(s.anisotropy);
    nvn::set_shadow_scale(s.shadow_scale);
    nvn::set_vsync(s.vsync);
    nvn::set_frame_cap(s.frame_cap);
    nvn::set_hdr(s.hdr != 0, s.hdr_peak, s.hdr_paper_white, s.hdr_highlights, s.hdr_highlight_start);
    nvn::set_hdr_sdr_eotf(s.hdr_sdr_eotf);
    nvn::set_hdr_grading(s.hdr_exposure, s.hdr_contrast, s.hdr_shadows, s.hdr_saturation, s.hdr_highlight_saturation);
    g_rumble_strength = std::clamp(s.rumble, 0, 100);
    for (size_t slot = 0; slot < 4; ++slot) {
        g_pad_left_deadzone[slot] = std::clamp(s.pad_left_deadzones[slot], 0, 34);
        g_pad_right_deadzone[slot] = std::clamp(s.pad_right_deadzones[slot], 0, 34);
    }
    g_keyboard_separate = s.keyboard_separate;
    g_button_icons = std::clamp(s.button_icons, 0, 3);
    g_controller_preset = std::clamp(s.controller_preset, 0, 1);
    for (size_t h = 0; h < overlay::kHotkeyCount; ++h)
        if (s.hotkey_bindings[h] > 0) g_hotkey_bindings[h] = s.hotkey_bindings[h];
    if (s.monitor != before.monitor || s.fullscreen != before.fullscreen || s.width != before.width ||
        s.height != before.height || s.always_on_top != before.always_on_top)
        post_to_main([s] {
            if (!g_window) return;
            int count = 0;
            SDL_DisplayID* ids = SDL_GetDisplays(&count);
            SDL_Rect bounds{};
            bool has_bounds = false;
            if (ids && count > 0) {
                const int idx = std::clamp(s.monitor, 0, count - 1);
                has_bounds = SDL_GetDisplayBounds(ids[idx], &bounds);
                if (const SDL_DisplayMode* dm = SDL_GetCurrentDisplayMode(ids[idx]); dm && dm->w > 0 && dm->h > 0)
                    nx::set_native_render_size(uint32_t(dm->w), uint32_t(dm->h));
            }
            if (ids) SDL_free(ids);
            if (s.fullscreen) {
                if (has_bounds) {
                    SDL_SetWindowFullscreen(g_window, false);
                    SDL_SetWindowPosition(g_window, bounds.x, bounds.y);
                }
                SDL_SetWindowFullscreen(g_window, true);
            } else {
                SDL_SetWindowFullscreen(g_window, false);
                SDL_SetWindowSize(g_window, s.width, s.height);
                if (has_bounds)
                    SDL_SetWindowPosition(g_window, bounds.x + (bounds.w - s.width) / 2,
                                          bounds.y + (bounds.h - s.height) / 2);
            }
            SDL_SetWindowAlwaysOnTop(g_window, s.always_on_top);
        });
    if (persist) overlay::save_settings(settings_path(), s);
}

const char* key_name(int scancode) {
    const char* n = SDL_GetScancodeName(SDL_Scancode(scancode));
    return n && *n ? n : "-";
}

// Keys the overlay understands, from SDL keycodes.
overlay::Key menu_key(SDL_Keycode k) {
    switch (k) {
        case SDLK_TAB: return overlay::KeyTab;
        case SDLK_LEFT: return overlay::KeyLeft;
        case SDLK_RIGHT: return overlay::KeyRight;
        case SDLK_UP: return overlay::KeyUp;
        case SDLK_DOWN: return overlay::KeyDown;
        case SDLK_PAGEUP: return overlay::KeyPageUp;
        case SDLK_PAGEDOWN: return overlay::KeyPageDown;
        case SDLK_HOME: return overlay::KeyHome;
        case SDLK_END: return overlay::KeyEnd;
        case SDLK_RETURN: case SDLK_KP_ENTER: return overlay::KeyEnter;
        case SDLK_ESCAPE: return overlay::KeyEscape;
        case SDLK_BACKSPACE: return overlay::KeyBackspace;
        case SDLK_DELETE: return overlay::KeyDelete;
        case SDLK_SPACE: return overlay::KeySpace;
        case SDLK_LSHIFT: case SDLK_RSHIFT: return overlay::KeyShift;
        case SDLK_LCTRL: case SDLK_RCTRL: return overlay::KeyCtrl;

        default: return overlay::KeyNone;
    }
}

int detect_gamepad_style(SDL_Gamepad* pad) {
    if (!pad) return 0;
    switch (SDL_GetGamepadType(pad)) {
        case SDL_GAMEPAD_TYPE_PS3:
        case SDL_GAMEPAD_TYPE_PS4:
        case SDL_GAMEPAD_TYPE_PS5:
            return 3;
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
        case SDL_GAMEPAD_TYPE_GAMECUBE:
            return 1;
        case SDL_GAMEPAD_TYPE_XBOX360:
        case SDL_GAMEPAD_TYPE_XBOXONE:
            return 2;
        default:
            break;
    }
    if (const char* name = SDL_GetGamepadName(pad)) {
        std::string lower;
        lower.reserve(std::strlen(name));
        for (const unsigned char ch : std::string_view(name)) lower.push_back(char(std::tolower(ch)));
        if (lower.find("dualsense") != std::string::npos || lower.find("dualshock") != std::string::npos ||
            lower.find("playstation") != std::string::npos || lower.find("ps5") != std::string::npos ||
            lower.find("ps4") != std::string::npos)
            return 3;
        if (lower.find("nintendo") != std::string::npos || lower.find("switch") != std::string::npos ||
            lower.find("joy-con") != std::string::npos || lower.find("pro controller") != std::string::npos)
            return 1;
        if (lower.find("xbox") != std::string::npos || lower.find("xinput") != std::string::npos)
            return 2;
    }
    return 0;
}

}  // namespace

bool fast_forward() { return g_fast_forward.load(std::memory_order_relaxed); }
bool paused() { return g_pause_in_background.load(std::memory_order_relaxed) && (!g_window_focused.load(std::memory_order_relaxed) || window_minimized()); }

void rumble_player(unsigned player, float low, float high) {
    int slot = -1;
    { std::lock_guard lk(g_pad_lock); if (player < g_player_slots.size()) slot = g_player_slots[player]; }
    if (slot < 0 || slot >= int(g_gamepads.size())) return;
    float k = float(g_rumble_strength.load()) / 100.0f;
    const uint32_t low_u16 = Uint16(std::clamp(low * k, 0.0f, 1.0f) * 65535.0f);
    const uint32_t high_u16 = Uint16(std::clamp(high * k, 0.0f, 1.0f) * 65535.0f);
    g_pending_rumble[size_t(slot)].store((1ull << 32) | (uint64_t(low_u16) << 16) | uint64_t(high_u16),
                                         std::memory_order_release);
}
void rumble_player_hd(unsigned, const float*, const float*) {}  // no Switch 2 Bluetooth driver here
void rumble(float low, float high) { rumble_player(0, low, high); }

void read_pad(PadState& p) {
    {
        std::lock_guard lk(g_pad_lock);
        p.buttons |= g_pad.buttons;
        if (g_pad.lx) p.lx = g_pad.lx;
        if (g_pad.ly) p.ly = g_pad.ly;
        if (g_pad.rx) p.rx = g_pad.rx;
        if (g_pad.ry) p.ry = g_pad.ry;
    }
    input_script(p);
    overlay::filter_pad(p);  // controller navigation of the menu; the game sees nothing while it is open
}

std::array<std::string, 4> controller_names() {
    std::array<std::string, 4> names;
    std::lock_guard lk(g_gamepad_lock);
    for (size_t i = 0; i < g_gamepads.size(); ++i) if (g_gamepads[i]) {
        const char* name = SDL_GetGamepadName(g_gamepads[i]);
        names[i] = name ? name : "Controller";
    }
    return names;
}
bool player_connected(unsigned player) {
    std::lock_guard lk(g_pad_lock);
    return player < g_connected.size() && g_connected[player];
}
void read_pad_player(unsigned player, PadState& p) {
    if (player == 0) { read_pad(p); return; }
    std::lock_guard lk(g_pad_lock);
    if (player < g_players.size() && !overlay::is_open() && !overlay::wants_keyboard()) p = g_players[player];
}
ButtonPromptConfig button_prompt_config() {
    std::array<int, 4> styles{};
    int active_slot = -1;
    {
        std::lock_guard lk(g_pad_lock);
        styles = g_pad_styles;
        active_slot = g_player_slots[0];
    }
    const int assigned = g_assignments[0].load();
    int slot = assigned >= 3 && assigned <= 6 ? assigned - 3 : (active_slot >= 0 ? active_slot : 0);
    slot = std::clamp(slot, 0, 3);
    ButtonPromptConfig cfg;
    for (size_t a = 0; a < overlay::kControlCount; ++a) cfg.bindings[a] = g_pad_binding[slot][a].load();
    const int pref = g_button_icons.load();
    if (pref >= 1 && pref <= 3) {
        cfg.style = pref;
    } else if (styles[slot] >= 1 && styles[slot] <= 3) {
        cfg.style = styles[slot];
    } else {
        int found = 0;
        for (int s : styles) {
            if (s >= 1 && s <= 3) {
                found = s;
                break;
            }
        }
        cfg.style = found ? found : (g_controller_preset.load() == 1 ? 1 : 2);
    }
    return cfg;
}

namespace linux_host {

void initial_window(int& w, int& h, bool& fullscreen) {
    std::lock_guard lk(g_settings_lock);
    w = g_settings.width, h = g_settings.height, fullscreen = g_settings.fullscreen;
}

void load_player_settings() {
    overlay::Settings s;
    const int keys[overlay::kControlCount] = {
        SDL_SCANCODE_SPACE, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_J, SDL_SCANCODE_K, SDL_SCANCODE_Q, SDL_SCANCODE_E,
        SDL_SCANCODE_Z, SDL_SCANCODE_C, SDL_SCANCODE_RETURN, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_M, SDL_SCANCODE_N,
        SDL_SCANCODE_LEFT, SDL_SCANCODE_UP, SDL_SCANCODE_RIGHT, SDL_SCANCODE_DOWN,
    };
    for (size_t a = 0; a < overlay::kControlCount; ++a)
        s.keyboard_bindings[a] = keys[a], s.controller_bindings[a] = overlay::default_controller_binding(a, s.controller_preset);
    bool found = overlay::load_settings(settings_path(), s);
    // Aspect-dependent resources and render targets are selected at startup. Keep explicit environment overrides
    // for developer tests, otherwise configure the same profiles as the Windows launcher.
    const char* profile = std::getenv("NX_ASPECT_PROFILE");
    int ratio = profile && std::strcmp(profile, "21:9") == 0 ? 1 :
                profile && std::strcmp(profile, "32:9") == 0 ? 2 :
                profile && std::strcmp(profile, "16:9") == 0 ? 0 : std::clamp(s.aspect_ratio, 0, 3);
    int disp_w = s.width, disp_h = s.height;
    if (s.fullscreen) {
        if (const SDL_DisplayMode* dm = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay()); dm && dm->w > 0 && dm->h > 0)
            disp_w = dm->w, disp_h = dm->h;
    }
    const double target_aspect = ratio == 1 ? (64.0 / 27.0) : ratio == 2 ? (32.0 / 9.0) : ratio == 3 ? (16.0 / 10.0) : (16.0 / 9.0);
    const double disp_aspect = disp_h > 0 ? double(disp_w) / double(disp_h) : 0.0;
    const bool uw = ratio == 1 || ratio == 2; char uw_res[32];
    if (uw && disp_w > 0 && disp_h > 0 && std::fabs(disp_aspect - target_aspect) / target_aspect <= 0.10)
        std::snprintf(uw_res, sizeof uw_res, "%dx%d", disp_w, disp_h);
    else
        std::snprintf(uw_res, sizeof uw_res, "%s", ratio == 1 ? "2560x1080" : ratio == 2 ? "3840x1080" : "0");
    setenv("NX_ASPECT_PROFILE", ratio == 1 ? "21:9" : ratio == 2 ? "32:9" : "16:9", 0);
    setenv("NX_ULTRAWIDE", uw ? uw_res : "0", 0);
    const char* flags[] = {"NX_TERRAIN_MOD_ASPECT_TEST", "NX_UW_POWERUP_TEST", "NX_UW_FLOWER_TEST",
        "NX_UW_MAP_ALIGN_TEST", "NX_UW_MAP_COMPOSITE_TEST", "NX_UW_MAP_CARD_TEST",
        "NX_UW_MAP_MARKERS_TEST", "NX_UW_MAP_DETAIL_TEST", "NX_UW_SETTINGS_SCISSOR_TEST"};
    for (const char* flag : flags) setenv(flag, uw ? "1" : "0", 0);
    // The alternate flower offset shifts normal Linux bubbles away from their flower.
    setenv("NX_UW_FLOWER_ALT_TEST", "0", 0);
    if (s.width > 0) s.height = std::max(360, ratio == 1 ? s.width * 27 / 64 :
                                              ratio == 2 ? s.width * 9 / 32 : ratio == 3 ? s.width * 10 / 16 : s.width * 9 / 16);
    if (const char* e = std::getenv("NX_MEM_LIMIT_GB"); e && std::atof(e) > 0) s.memory_limit_gb = std::atoi(e);
    apply_settings(s, false);
    overlay::Host host;
    host.apply = apply_settings;
    host.monitors = []() -> std::vector<std::string> {
        std::vector<std::string> out;
        int count = 0;
        if (SDL_DisplayID* ids = SDL_GetDisplays(&count)) {
            for (int i = 0; i < count; ++i) {
                const char* name = SDL_GetDisplayName(ids[i]);
                const SDL_DisplayMode* dm = SDL_GetCurrentDisplayMode(ids[i]);
                char buf[256];
                if (dm && dm->w > 0 && dm->h > 0)
                    std::snprintf(buf, sizeof buf, "Monitor %d: %s (%dx%d)", i + 1, name && *name ? name : "Display", dm->w, dm->h);
                else
                    std::snprintf(buf, sizeof buf, "Monitor %d: %s", i + 1, name && *name ? name : "Display");
                out.emplace_back(buf);
            }
            SDL_free(ids);
        }
        return out;
    };
    host.key_name = key_name;
    for (size_t a = 0; a < overlay::kControlCount; ++a) host.default_keyboard_bindings[a] = keys[a];
    for (size_t h = 0; h < overlay::kHotkeyCount; ++h) host.default_hotkey_bindings[h] = hotkeys[h];
    // Wayland has no "always on top" for clients; X11 does.
    const char* driver = SDL_GetCurrentVideoDriver();
    host.can_always_on_top = !driver || std::strcmp(driver, "wayland") != 0;
    overlay::init(host, s);
    log("platform: settings %s (%s)", found ? "loaded" : "defaults", settings_path().c_str());
}

void reload_player_settings() {
    std::error_code ec;
    const std::string path = settings_path();
    if (!std::filesystem::is_regular_file(nx_u8path(path), ec)) return;
    overlay::Settings s;
    const int keys[overlay::kControlCount] = {
        SDL_SCANCODE_SPACE, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_J, SDL_SCANCODE_K, SDL_SCANCODE_Q, SDL_SCANCODE_E,
        SDL_SCANCODE_Z, SDL_SCANCODE_C, SDL_SCANCODE_RETURN, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_M, SDL_SCANCODE_N,
        SDL_SCANCODE_LEFT, SDL_SCANCODE_UP, SDL_SCANCODE_RIGHT, SDL_SCANCODE_DOWN,
    };
    for (size_t a = 0; a < overlay::kControlCount; ++a)
        s.keyboard_bindings[a] = keys[a], s.controller_bindings[a] = overlay::default_controller_binding(a, s.controller_preset);
    if (!overlay::load_settings(path, s)) return;
    const int ratio = std::clamp(s.aspect_ratio, 0, 3);
    if (s.width > 0) s.height = std::max(360, ratio == 1 ? s.width * 27 / 64 :
                                              ratio == 2 ? s.width * 9 / 32 : ratio == 3 ? s.width * 10 / 16 : s.width * 9 / 16);
    int disp_w = s.width, disp_h = s.height;
    if (s.fullscreen) {
        if (const SDL_DisplayMode* dm = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay()); dm && dm->w > 0 && dm->h > 0)
            disp_w = dm->w, disp_h = dm->h;
    }
    const double target_aspect = ratio == 1 ? (64.0 / 27.0) : ratio == 2 ? (32.0 / 9.0) : ratio == 3 ? (16.0 / 10.0) : (16.0 / 9.0);
    const double disp_aspect = disp_h > 0 ? double(disp_w) / double(disp_h) : 0.0;
    const bool uw = ratio == 1 || ratio == 2; char uw_res[32];
    if (uw && disp_w > 0 && disp_h > 0 && std::fabs(disp_aspect - target_aspect) / target_aspect <= 0.10)
        std::snprintf(uw_res, sizeof uw_res, "%dx%d", disp_w, disp_h);
    else
        std::snprintf(uw_res, sizeof uw_res, "%s", ratio == 1 ? "2560x1080" : ratio == 2 ? "3840x1080" : "0");
    setenv("NX_ASPECT_PROFILE", ratio == 1 ? "21:9" : ratio == 2 ? "32:9" : "16:9", 1);
    setenv("NX_ULTRAWIDE", uw ? uw_res : "0", 1);
    const char* flags[] = {"NX_TERRAIN_MOD_ASPECT_TEST", "NX_UW_POWERUP_TEST", "NX_UW_FLOWER_TEST",
        "NX_UW_MAP_ALIGN_TEST", "NX_UW_MAP_COMPOSITE_TEST", "NX_UW_MAP_CARD_TEST",
        "NX_UW_MAP_MARKERS_TEST", "NX_UW_MAP_DETAIL_TEST", "NX_UW_SETTINGS_SCISSOR_TEST"};
    for (const char* flag : flags) setenv(flag, uw ? "1" : "0", 1);
    apply_settings(s, false);
    overlay::sync(s);
    log("platform: settings reloaded (%s)", path.c_str());
}

void run_posted() {
    std::deque<std::function<void()>> posted;
    {
        std::lock_guard lk(g_posted_lock);
        posted.swap(g_posted);
    }
    for (auto& fn : posted) fn();
}

bool handle_event(const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_TEXT_INPUT: {
            if (overlay::enabled() && e.text.text) {
                const char* text = e.text.text;
                while (uint32_t ch = SDL_StepUTF8(&text, nullptr)) overlay::text_input(ch);
            }
            return true;
        }
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            const bool down = e.type == SDL_EVENT_KEY_DOWN;
            if (!overlay::capturing_key() && down && !e.key.repeat) {
                const int sc = int(e.key.scancode);
                if (sc == g_hotkey_bindings[0].load(std::memory_order_relaxed)) {
                    if (overlay::enabled()) overlay::toggle();
                    return true;
                }
                if (sc == g_hotkey_bindings[1].load(std::memory_order_relaxed)) {
                    nvn::request_screenshot();
                    return true;
                }
                if (sc == g_hotkey_bindings[2].load(std::memory_order_relaxed) && g_window) {
                    bool full = !(SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN);
                    SDL_SetWindowFullscreen(g_window, full);
                    std::lock_guard lk(g_settings_lock);
                    g_settings.fullscreen = full;
                    overlay::sync(g_settings);
                    return true;
                }
            }
            if (overlay::enabled() && !e.key.repeat) overlay::key_event(int(e.key.scancode), menu_key(e.key.key), down);
            return true;
        }
        case SDL_EVENT_MOUSE_MOTION:
            if (overlay::wants_mouse() && g_window) {
                float d = SDL_GetWindowPixelDensity(g_window);
                overlay::mouse_move(e.motion.x * d, e.motion.y * d);
            }
            return true;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (overlay::wants_mouse()) {
                int b = e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1 : 2;
                overlay::mouse_button(b, e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
            }
            return true;
        case SDL_EVENT_MOUSE_WHEEL:
            if (overlay::wants_mouse()) overlay::mouse_wheel(e.wheel.y);
            return true;
        default:
            return false;
    }
}

void sample_input() {
    PadState p;
    std::array<PadState, 4> players{}, physical_pads{};
    std::array<bool, 4> connected{}, pad_connected{};
    std::array<int, 4> player_slots{{-1, -1, -1, -1}};
    std::array<int, 4> pad_styles{};
    {
    std::lock_guard gamepad_lock(g_gamepad_lock);
    bool any_connected = false;
    for (auto& pad : g_gamepads) {
        if (pad && !SDL_GamepadConnected(pad)) SDL_CloseGamepad(pad), pad = nullptr;
        if (pad) any_connected = true;
    }
    static uint64_t last_scan = 0;
    const uint64_t now = SDL_GetTicks();
    const uint64_t interval_ms = any_connected ? 2000 : 1000;
    if (!last_scan || now - last_scan >= interval_ms) {
        last_scan = now;
        int count = 0; SDL_JoystickID* ids = SDL_GetGamepads(&count);
        for (int i = 0; ids && i < count; ++i) {
            bool known = false;
            for (auto* open : g_gamepads) if (open && SDL_GetGamepadID(open) == ids[i]) known = true;
            if (known) continue;
            for (auto& open : g_gamepads) if (!open) { open = SDL_OpenGamepad(ids[i]); break; }
        }
        SDL_free(ids);
    }
    for (size_t slot = 0; slot < g_gamepads.size(); ++slot) {
        const uint64_t req = g_pending_rumble[slot].exchange(0, std::memory_order_acq_rel);
        if ((req >> 32) && g_gamepads[slot]) {
            SDL_RumbleGamepad(g_gamepads[slot], Uint16((req >> 16) & 0xFFFFu), Uint16(req & 0xFFFFu), 250);
        }
    }
    const bool ignore_pad = g_ignore_pad_in_background.load(std::memory_order_relaxed) &&
                            !g_window_focused.load(std::memory_order_relaxed);
    for (unsigned slot = 0; slot < g_gamepads.size(); ++slot) {
        SDL_Gamepad* pad = g_gamepads[slot];
        if (!pad) continue;
        PadState& q = physical_pads[slot];
        pad_connected[slot] = true;
        pad_styles[slot] = detect_gamepad_style(pad);
        if (ignore_pad) continue;
        // Physical buttons by position (overlay::Physical order), then each action takes its bound button
        // (the menu's preset or remapping; the default preset puts A at the bottom, as on Windows).
        static const SDL_GamepadButton physical[] = {
            SDL_GAMEPAD_BUTTON_SOUTH,       SDL_GAMEPAD_BUTTON_EAST,          SDL_GAMEPAD_BUTTON_WEST,
            SDL_GAMEPAD_BUTTON_NORTH,       SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
            SDL_GAMEPAD_BUTTON_INVALID,     SDL_GAMEPAD_BUTTON_INVALID,  // the triggers are axes
            SDL_GAMEPAD_BUTTON_START,       SDL_GAMEPAD_BUTTON_BACK,          SDL_GAMEPAD_BUTTON_LEFT_STICK,
            SDL_GAMEPAD_BUTTON_RIGHT_STICK, SDL_GAMEPAD_BUTTON_DPAD_LEFT,     SDL_GAMEPAD_BUTTON_DPAD_UP,
            SDL_GAMEPAD_BUTTON_DPAD_RIGHT,  SDL_GAMEPAD_BUTTON_DPAD_DOWN,
        };
        bool down[overlay::kControlCount] = {};
        for (size_t k = 0; k < overlay::kControlCount; ++k)
            down[k] = physical[k] != SDL_GAMEPAD_BUTTON_INVALID && SDL_GetGamepadButton(pad, physical[k]);
        down[overlay::PhyTriggerL] = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 12000;
        down[overlay::PhyTriggerR] = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 12000;
        for (size_t a = 0; a < overlay::kControlCount; ++a)
            if (down[std::clamp(g_pad_binding[slot][a].load(), 0, int(overlay::kControlCount) - 1)]) q.buttons |= kActionBits[a];
        const int dz_l = (std::clamp(g_pad_left_deadzone[slot].load(std::memory_order_relaxed), 0, 34) * kStickMax) / 100;
        const int dz_r = (std::clamp(g_pad_right_deadzone[slot].load(std::memory_order_relaxed), 0, 34) * kStickMax) / 100;
        auto axis = [&](SDL_GamepadAxis a, bool invert, int dz) {
            const int raw = SDL_GetGamepadAxis(pad, a);
            const int av = std::min(std::abs(raw), kStickMax);
            if (av <= dz) return 0;
            const int mag = ((av - dz) * kStickMax) / (kStickMax - dz);
            const int v = raw < 0 ? -mag : mag;
            return invert ? -v : v;
        };
        q.lx = axis(SDL_GAMEPAD_AXIS_LEFTX, false, dz_l);
        q.ly = axis(SDL_GAMEPAD_AXIS_LEFTY, true, dz_l);  // SDL: down is positive; Horizon: up is positive
        q.rx = axis(SDL_GAMEPAD_AXIS_RIGHTX, false, dz_r);
        q.ry = axis(SDL_GAMEPAD_AXIS_RIGHTY, true, dz_r);
    }
    }
    // Keyboard: WASD / arrows move the stick; the 16 actions follow their bindings (defaults: Space A, Shift B, J X,
    // K Y, Q/E L/R, Z/C ZL/ZR, Enter +, Esc -, M/N stick clicks, arrows D-pad). Not while the menu has the keys.
    bool tab = false;
    if (g_window && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS) && !overlay::wants_keyboard()) {
        int count = 0;
        const bool* k = SDL_GetKeyboardState(&count);
        const int ff_sc = g_hotkey_bindings[3].load(std::memory_order_relaxed);
        tab = ff_sc > 0 && ff_sc < count && k[ff_sc];  // fast-forward (hle_vi.cpp)
        auto held = [&](int sc) { return sc > 0 && sc < count && k[sc]; };
        auto is_bound = [](int sc) {
            for (const auto& b : g_key_binding)
                if (b.load() == sc) return true;
            return false;
        };
        if ((!is_bound(SDL_SCANCODE_A) && k[SDL_SCANCODE_A]) || k[SDL_SCANCODE_LEFT]) p.lx = -kStickMax;
        if ((!is_bound(SDL_SCANCODE_D) && k[SDL_SCANCODE_D]) || k[SDL_SCANCODE_RIGHT]) p.lx = kStickMax;
        if ((!is_bound(SDL_SCANCODE_W) && k[SDL_SCANCODE_W]) || k[SDL_SCANCODE_UP]) p.ly = kStickMax;
        if ((!is_bound(SDL_SCANCODE_S) && k[SDL_SCANCODE_S]) || k[SDL_SCANCODE_DOWN]) p.ly = -kStickMax;
        for (size_t a = 0; a < overlay::kControlCount; ++a)
            if (held(g_key_binding[a].load())) p.buttons |= kActionBits[a];
        if (k[SDL_SCANCODE_RSHIFT] && g_key_binding[overlay::CtlB].load() == SDL_SCANCODE_LSHIFT) p.buttons |= kB;
    }
    g_fast_forward = tab;
    std::array<int, 4> assignments{};
    bool manual = false;
    for (size_t i = 0; i < 4; ++i) { assignments[i] = g_assignments[i].load(); manual |= assignments[i] != 0; }
    if (!manual) {
        const bool separate = g_keyboard_separate.load();
        players[0] = p;
        connected[0] = true;
        size_t next_player = separate ? 1 : 0;
        for (size_t slot = 0; slot < 4 && next_player < 4; ++slot) {
            if (!pad_connected[slot]) continue;
            const size_t player = next_player++;
            player_slots[player] = int(slot); connected[player] = true;
            players[player].buttons |= physical_pads[slot].buttons;
            if (physical_pads[slot].lx) players[player].lx = physical_pads[slot].lx;
            if (physical_pads[slot].ly) players[player].ly = physical_pads[slot].ly;
            if (physical_pads[slot].rx) players[player].rx = physical_pads[slot].rx;
            if (physical_pads[slot].ry) players[player].ry = physical_pads[slot].ry;
        }
    } else {
        bool used[7]{};
        for (int a : assignments) if (a >= 2 && a <= 6) used[a] = true;
        for (size_t player = 0; player < 4; ++player) {
            int a = assignments[player];
            if (a == 0) {
                if (!used[2]) a = 2;
                else for (int candidate = 3; candidate <= 6; ++candidate)
                    if (!used[candidate] && pad_connected[candidate - 3]) { a = candidate; break; }
                if (a >= 2) used[a] = true;
            }
            if (a == 2) players[player] = p, connected[player] = true;
            else if (a >= 3 && a <= 6 && pad_connected[a - 3]) {
                players[player] = physical_pads[a - 3]; connected[player] = true; player_slots[player] = a - 3;
            }
        }
    }
    p = players[0];
    std::lock_guard lk(g_pad_lock);
    g_pad = p;
    players[0] = p;
    g_players = players;
    g_player_slots = player_slots;
    g_pad_styles = pad_styles;
    for (size_t i = 0; i < connected.size(); ++i)
        if (g_connected[i] != connected[i])
            log("hid: player %zu %s", i + 1, connected[i] ? "connected" : "disconnected");
    g_connected = connected;
}

void linux_host::set_window_focused(bool focused) {
    g_window_focused.store(focused, std::memory_order_relaxed);
    std::lock_guard lk(g_settings_lock);
    update_audio_gain(g_settings);
}

}  // namespace linux_host

}  // namespace nx::plat
