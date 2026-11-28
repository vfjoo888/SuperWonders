// The overlay's state, input queue, metrics and per-frame ImGui work (see overlay.h). The menu's tabs are in
// overlay_menu.cpp, the drawing in overlay_vk.cpp.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string_view>
static inline std::filesystem::path nx_u8path(std::string_view s) { return std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(s.data()), s.size())); }
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "imgui.h"

#include <nx/runtime.h>

#include "overlay/overlay_internal.h"
#include "overlay/overlay_ui.h"
#include "discord_rpc.h"
#include "nextendo_client.h"

namespace nx::overlay {

namespace {

struct Event {
    enum Kind { KeyEv, Move, Button, Wheel, Text } kind;
    int raw = 0;
    Key key = KeyNone;
    bool down = false;
    float x = 0, y = 0;
    uint32_t ch = 0;
};

std::mutex g_lock;  // g_events, g_pending, g_host
std::vector<Event> g_events;
Host g_host;
bool g_has_pending = false;
Settings g_pending;   // from the platform (sync), taken by the next frame
Settings g_settings;  // the presenting thread's copy
std::atomic<bool> g_open{false};
std::atomic<ScreenFn> g_screen{nullptr};  // a full-window screen under the menu (the launcher), or none
// Input goes to the overlay: the menu is open, or a screen is shown.
bool taking_input() { return g_open || g_screen.load(); }
std::atomic<bool> g_metrics{false};
std::atomic<int> g_input_frames{0};  // frames to run after input while closed (hover/close animations)
std::atomic<bool> g_fading{false};   // the menu is still fading out

// The pad as the player holds it (before filter_pad clears it), for navigation.
std::atomic<uint64_t> g_pad_buttons{0};
std::atomic<int32_t> g_pad_lx{0}, g_pad_ly{0};
std::atomic<int> g_tab_request{-1};
uint64_t g_previous_buttons = 0;

int g_capture = -1;  // keyboard binding being captured
int g_tab = 0;
std::atomic<int> g_title_quit_button{CtlMinus};

// Present intervals for the metrics panel (as platform_win.cpp's overlay).
constexpr size_t kHistory = 300;
std::mutex g_metrics_lock;
std::array<double, kHistory> g_intervals{};
size_t g_next = 0, g_count = 0;
uint64_t g_previous_ns = 0;
std::atomic<int> g_game_interval{1};
std::atomic<double> g_game_speed{100.0};
std::atomic<bool> g_speedrun_armed{false};
std::atomic<bool> g_speedrun_in_course{false};
std::atomic<bool> g_speedrun_finished{false};
std::atomic<bool> g_speedrun_exiting{false};
std::atomic<uint64_t> g_speedrun_igt_ns{0};
std::atomic<uint64_t> g_speedrun_rta_ns{0};
std::atomic<uint64_t> g_speedrun_finish_ns{0};
std::atomic<uint64_t> g_speedrun_exit_ns{0};
std::mutex g_speedrun_meta_mu;
std::string g_speedrun_world = "Unknown World";
std::string g_speedrun_course = "Unknown Course";

ImGuiKey imgui_key(Key k) {
    switch (k) {
        case KeyTab: return ImGuiKey_Tab;
        case KeyLeft: return ImGuiKey_LeftArrow;
        case KeyRight: return ImGuiKey_RightArrow;
        case KeyUp: return ImGuiKey_UpArrow;
        case KeyDown: return ImGuiKey_DownArrow;
        case KeyPageUp: return ImGuiKey_PageUp;
        case KeyPageDown: return ImGuiKey_PageDown;
        case KeyHome: return ImGuiKey_Home;
        case KeyEnd: return ImGuiKey_End;
        case KeyEnter: return ImGuiKey_Enter;
        case KeyEscape: return ImGuiKey_Escape;
        case KeyBackspace: return ImGuiKey_Backspace;
        case KeyDelete: return ImGuiKey_Delete;
        case KeySpace: return ImGuiKey_Space;
        case KeyShift: return ImGuiKey_LeftShift;
        case KeyCtrl: return ImGuiKey_LeftCtrl;
        default: return ImGuiKey_None;
    }
}

void push(const Event& e) {
    std::lock_guard lk(g_lock);
    if (g_events.size() < 4096) g_events.push_back(e);
}

// The metrics panel: a small card in the chosen corner, in the menu's colours. Savage Wind's performance overlay
// (menu.mm perf_overlay_draw) is the model: names in colour, values beside them, the frame times as a graph.
void draw_metrics(float height) {
    std::array<float, kHistory> samples{};
    size_t count = 0;
    {
        std::lock_guard lk(g_metrics_lock);
        count = g_count;
        size_t start = (g_next + kHistory - count) % kHistory;
        for (size_t i = 0; i < count; ++i) samples[i] = float(g_intervals[(start + i) % kHistory]);
    }
    double sum = 0;
    size_t recent = std::min<size_t>(count, 60);
    for (size_t i = count - recent; i < count; ++i) sum += samples[i];
    double fps = sum > 0 ? double(recent) * 1000.0 / sum : 0.0;
    double low = 0;
    if (count) {
        std::vector<float> slow(samples.begin(), samples.begin() + long(count));
        std::sort(slow.begin(), slow.end(), std::greater<float>());
        size_t n = std::max<size_t>(1, (count + 99) / 100);
        double s = 0;
        for (size_t i = 0; i < n; ++i) s += slow[i];
        if (s > 0) low = double(n) * 1000.0 / s;
    }
    const int interval = std::max(1, g_game_interval.load());
    const double speed = g_game_speed.load(std::memory_order_relaxed);
    const double target = 60.0 / double(interval);
    const Settings& st = g_settings;
    const float k = std::clamp(height / 1080.0f, 0.5f, 3.0f);
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const float m = 16 * k;
    const int pos = std::clamp(st.overlay_position, 0, 3);
    const ImVec2 at{pos & 1 ? size.x - m : m, pos & 2 ? size.y - m : m};
    ImGui::SetNextWindowPos(at, ImGuiCond_Always, {pos & 1 ? 1.0f : 0.0f, pos & 2 ? 1.0f : 0.0f});
    const ui::Theme& th = ui::theme();
    ImVec4 bg = ImGui::ColorConvertU32ToFloat4(th.panel);
    bg.w = 0.82f;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, bg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * k, 12 * k));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12 * k);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12 * k, 3 * k));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6 * k, 1 * k));
    const ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                               ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                               ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("##metrics", nullptr, f)) {
        constexpr float kScale = 0.74f;
        ImGui::SetWindowFontScale(kScale);
        // green at the target frame rate, amber a little under it, red well under (MangoHud colours its numbers so)
        const auto rate = [&](double v) {
            return ImGui::ColorConvertU32ToFloat4(v >= target * 0.95 ? th.good : v >= target * 0.75 ? th.warn : th.bad);
        };
        const ImVec4 name_c = ImGui::ColorConvertU32ToFloat4(th.accent2), plain = ImGui::ColorConvertU32ToFloat4(th.text);
        const auto line = [&](const char* name, const ImVec4& colour, const char* fmt, double v, const char* unit) {
            ImGui::TableNextColumn();
            ImGui::TextColored(name_c, "%s", name);
            ImGui::TableNextColumn();
            ImGui::PushFont(ui::title());  // (the numbers in the heavier face, at the body size)
            ImGui::SetWindowFontScale(kScale * 40.0f / 72.0f);
            ImGui::TextColored(colour, fmt, v);
            ImGui::PopFont();
            ImGui::SetWindowFontScale(kScale);
            if (*unit) {
                ImGui::SameLine(0, 5 * k);
                ImGui::TextDisabled("%s", unit);
            }
        };
        if (ImGui::BeginTable("##m", 2, ImGuiTableFlags_SizingFixedFit)) {
            if (st.metric_fps) line("FPS", rate(fps), "%.0f", fps, "");
            if (st.metric_fps && fps > 0) line("Frame", plain, "%.1f", 1000.0 / fps, "ms");
            if (st.metric_one_percent_low) line("1% low", rate(low), "%.0f", low, "FPS");
            if (st.metric_game_speed) line("Speed", speed >= 97 ? plain : ImGui::ColorConvertU32ToFloat4(th.warn), "%.0f", speed, "%");
            ImGui::EndTable();
        }
        if (st.metric_frametime_graph && count > 1) {  // the frame times, 0-50 ms, with lines at 16.7 and 33.3 ms
            ImGui::Dummy(ImVec2(0, 2 * k));
            const ImVec2 p = ImGui::GetCursorScreenPos(), gs(300 * k, 64 * k);
            ImGui::Dummy(gs);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p, ImVec2(p.x + gs.x, p.y + gs.y), IM_COL32(255, 255, 255, 12), 6 * k);
            const auto y_of = [&](float ms) { return p.y + gs.y - std::clamp(ms / 50.0f, 0.0f, 1.0f) * gs.y; };
            for (float ms : {16.7f, 33.3f})
                dl->AddLine(ImVec2(p.x, y_of(ms)), ImVec2(p.x + gs.x, y_of(ms)), IM_COL32(255, 255, 255, 36), 1.0f);
            const float dx = gs.x / float(kHistory - 1);
            const float x0 = p.x + gs.x - dx * float(count - 1);
            std::array<ImVec2, kHistory> pts;
            const float late = 17.5f * float(interval);
            for (size_t i = 0; i < count; ++i) {
                const float x = x0 + dx * float(i), y = y_of(samples[i]);
                pts[i] = ImVec2(x, y);
                const ImU32 c = samples[i] > 33.4f ? (th.bad & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 120)
                                : samples[i] > late ? (th.warn & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 90)
                                                    : (th.accent & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 34);
                dl->AddRectFilled(ImVec2(x, y), ImVec2(x + dx + 0.5f, p.y + gs.y), c);
            }
            dl->AddPolyline(pts.data(), int(count), (th.accent & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 230), 0, std::max(1.0f, 1.5f * k));
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor();
}

bool speedrun_timer_wanted() {
    return g_settings.speedrun_timer &&
           (g_speedrun_in_course.load(std::memory_order_relaxed) || g_speedrun_exiting.load(std::memory_order_relaxed)) &&
           !nextendo::status().enabled;
}

void format_speedrun_time(uint64_t ns, char* buf, size_t n) {
    const uint64_t total_ms = ns / 1'000'000ull;
    const uint64_t ms = total_ms % 1000ull;
    const uint64_t total_s = total_ms / 1000ull;
    const uint64_t s = total_s % 60ull;
    const uint64_t total_m = total_s / 60ull;
    const uint64_t m = total_m % 60ull;
    const uint64_t h = total_m / 60ull;
    if (h > 0) std::snprintf(buf, n, "%llu:%02llu:%02llu.%03llu", (unsigned long long)h, (unsigned long long)m, (unsigned long long)s, (unsigned long long)ms);
    else std::snprintf(buf, n, "%02llu:%02llu.%03llu", (unsigned long long)m, (unsigned long long)s, (unsigned long long)ms);
}

void draw_speedrun_timer(float height) {
    const Settings& st = g_settings;
    const bool exiting = g_speedrun_exiting.load(std::memory_order_relaxed);
    const bool finished = g_speedrun_finished.load(std::memory_order_relaxed);
    const uint64_t now = plat::monotonic_ns();
    constexpr float kSizeMul[4] = {0.78f, 1.0f, 1.30f, 1.65f};
    const float sz_mul = kSizeMul[std::clamp(st.speedrun_timer_size, 0, 3)];
    const float base_k = std::clamp(height / 1080.0f, 0.5f, 3.0f);
    const float k = base_k * sz_mul;
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const float m = 18 * base_k;
    const int pos = std::clamp(st.speedrun_timer_position, 0, 5);
    const int col = pos % 3;
    const int row = pos / 3;
    const float px = col == 0 ? m : col == 1 ? size.x * 0.5f : size.x - m;
    float py = row == 0 ? m : size.y - m;
    const float ax = col == 0 ? 0.0f : col == 1 ? 0.5f : 1.0f;
    const float ay = row == 0 ? 0.0f : 1.0f;

    if (exiting) {
        const double t = double(now - g_speedrun_exit_ns.load(std::memory_order_relaxed)) / 550'000'000.0;
        if (t >= 1.0) {
            g_speedrun_exiting.store(false, std::memory_order_relaxed);
            g_speedrun_finished.store(false, std::memory_order_relaxed);
            return;
        }
        const float ft = float(t);
        const float dip = 18.0f * k;
        if (ft < 0.25f) {
            py += dip * std::sin((ft / 0.25f) * 1.5707963f);
        } else {
            const float u = (ft - 0.25f) / 0.75f;
            const float ease = u * u * u;
            const float fly_to = -(py + 180.0f * k);
            py += dip + (fly_to - dip) * ease;
        }
    } else if (finished) {
        const uint64_t elapsed_ms = (now - g_speedrun_finish_ns.load(std::memory_order_relaxed)) / 1'000'000ull;
        if (((elapsed_ms / 350ull) % 2ull) == 1ull) return;
    }

    static constexpr ImU32 kTimerColors[6] = {
        IM_COL32(255, 255, 255, 255),  // White
        IM_COL32(255, 224, 92, 255),   // Gold
        IM_COL32(118, 234, 255, 255),  // Cyan
        IM_COL32(130, 245, 138, 255),  // Green
        IM_COL32(255, 110, 110, 255),  // Red
        IM_COL32(255, 148, 218, 255),  // Pink
    };
    const ImU32 num_col = kTimerColors[std::clamp(st.speedrun_timer_color, 0, 5)];
    ImGui::SetNextWindowPos({px, py}, ImGuiCond_Always, {ax, ay});

    if (finished) {
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4 * k, 2 * k));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 4 * k));
    } else {
        // Mario Wonder UI aesthetic: deep plum-navy pill, warm golden border, crisp shadowed tabular numerals.
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(22, 18, 36, 222));
        ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(255, 214, 58, 245));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18 * k, 11 * k));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 18 * k);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, std::max(1.5f, 2.5f * k));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10 * k, 4 * k));
    }
    const ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                               ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                               ImGuiWindowFlags_NoSavedSettings |
                               (finished ? ImGuiWindowFlags_NoBackground : 0);
    if (ImGui::Begin("##speedrun_timer", nullptr, f)) {
        const int mode = std::clamp(st.speedrun_timer_mode, 0, 2);
        const uint64_t igt = g_speedrun_igt_ns.load(std::memory_order_relaxed);
        const uint64_t rta = g_speedrun_rta_ns.load(std::memory_order_relaxed);
        const float num_fs = ImGui::GetFontSize() * 1.18f * sz_mul;
        const float tag_fs = ImGui::GetFontSize() * 0.72f * sz_mul;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const auto draw_row = [&](const char* tag, uint64_t ns, ImU32 tag_col) {
            char buf[32];
            format_speedrun_time(ns, buf, sizeof buf);
            const ImVec2 num_sz = ui::title()->CalcTextSizeA(num_fs, FLT_MAX, 0, buf);
            if (finished) {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::Dummy(num_sz);
                dl->AddText(ui::title(), num_fs, ImVec2(p.x + 2.5f * k, p.y + 2.5f * k), IM_COL32(8, 6, 14, 240), buf);
                dl->AddText(ui::title(), num_fs, p, num_col, buf);
                return;
            }
            const ImVec2 tag_sz = ui::body()->CalcTextSizeA(tag_fs, FLT_MAX, 0, tag);
            const float row_h = std::max(tag_sz.y, num_sz.y);
            const float tag_w = 38 * k;
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(tag_w + 8 * k + num_sz.x, row_h));
            const ImVec2 tp{p.x, p.y + (row_h - tag_sz.y) * 0.55f};
            const ImVec2 np{p.x + tag_w + 8 * k, p.y + (row_h - num_sz.y) * 0.5f};
            dl->AddText(ui::body(), tag_fs, ImVec2(tp.x + 1.5f * k, tp.y + 1.5f * k), IM_COL32(8, 6, 14, 210), tag);
            dl->AddText(ui::body(), tag_fs, tp, tag_col, tag);
            dl->AddText(ui::title(), num_fs, ImVec2(np.x + 2.0f * k, np.y + 2.0f * k), IM_COL32(8, 6, 14, 230), buf);
            dl->AddText(ui::title(), num_fs, np, num_col, buf);
        };
        if (mode == 0 || mode == 2) draw_row("IGT", igt, IM_COL32(255, 222, 89, 255));
        if (mode == 1 || mode == 2) draw_row("RTA", rta, IM_COL32(138, 228, 255, 255));
    }
    ImGui::End();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(2);
}

// Controller navigation: the pad (Nintendo button meanings) as ImGui's gamepad keys. A confirms, B cancels.
void feed_pad(ImGuiIO& io) {
    const bool open = taking_input();  // (closed and fading out: the menu takes no input)
    uint64_t b = open ? g_pad_buttons.load() : 0;
    int32_t lx = open ? g_pad_lx.load() : 0, ly = open ? g_pad_ly.load() : 0;
    struct Map {
        uint64_t bit;
        ImGuiKey key;
    };
    static const Map map[] = {
        {plat::kA, ImGuiKey_GamepadFaceDown},     {plat::kB, ImGuiKey_GamepadFaceRight},
        {plat::kX, ImGuiKey_GamepadFaceUp},       {plat::kY, ImGuiKey_GamepadFaceLeft},
        {plat::kLeft, ImGuiKey_GamepadDpadLeft},  {plat::kRight, ImGuiKey_GamepadDpadRight},
        {plat::kUp, ImGuiKey_GamepadDpadUp},      {plat::kDown, ImGuiKey_GamepadDpadDown},
        {plat::kZL, ImGuiKey_GamepadL2},          {plat::kZR, ImGuiKey_GamepadR2},
    };
    for (const Map& m : map) io.AddKeyEvent(m.key, (b & m.bit) != 0);
    auto axis = [&](ImGuiKey neg, ImGuiKey posk, int32_t v) {
        float f = float(v) / float(plat::kStickMax);
        io.AddKeyAnalogEvent(neg, f < -0.3f, f < 0 ? -f : 0.0f);
        io.AddKeyAnalogEvent(posk, f > 0.3f, f > 0 ? f : 0.0f);
    };
    axis(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, lx);
    axis(ImGuiKey_GamepadLStickDown, ImGuiKey_GamepadLStickUp, ly);  // Horizon: up is positive
}

}  // namespace

bool enabled() {
    static const bool on = [] {
        const char* e = std::getenv("NX_IMGUI_MENU");
        return !(e && e[0] == '0');
    }();
    return on;
}

std::string g_game_version;

int menu_language() { return std::clamp(g_settings.language, 0, 12); }

int game_language() { return std::clamp(g_settings.game_language, 0, 17); }

int title_quit_button() { return std::clamp(g_title_quit_button.load(std::memory_order_relaxed), 0, int(kControlCount) - 1); }

void set_game_version(const std::string& version) { g_game_version = version; }

const std::string& game_version() { return g_game_version; }

void init(const Host& host, const Settings& s) {
    std::lock_guard lk(g_lock);
    g_host = host;
    g_pending = s, g_has_pending = true;
    g_metrics = s.metrics_enabled;
    g_title_quit_button.store(std::clamp(s.title_quit_button, 0, int(kControlCount) - 1), std::memory_order_relaxed);
    ui::set_theme(s.theme);
    discord::set_enabled(s.discord_rpc);
}

void sync(const Settings& s) {
    std::lock_guard lk(g_lock);
    g_pending = s, g_has_pending = true;
    g_metrics = s.metrics_enabled;
    g_title_quit_button.store(std::clamp(s.title_quit_button, 0, int(kControlCount) - 1), std::memory_order_relaxed);
    ui::set_theme(s.theme);
    discord::set_enabled(s.discord_rpc);
}

void settings_changed(bool persist) {
    g_metrics = g_settings.metrics_enabled;
    g_title_quit_button.store(std::clamp(g_settings.title_quit_button, 0, int(kControlCount) - 1), std::memory_order_relaxed);
    discord::set_enabled(g_settings.discord_rpc);
    Host h;
    {
        std::lock_guard lk(g_lock);
        h = g_host;
    }
    if (h.apply) h.apply(g_settings, persist);
}

int aspect_ratio() { return std::clamp(g_settings.aspect_ratio, 0, 3); }

void set_aspect_ratio(int ratio) {
    const int clamped = std::clamp(ratio, 0, 3);
    if (clamped != g_settings.aspect_ratio) {
        adjust_window_size_for_aspect(clamped, g_settings.width, g_settings.height);
        g_settings.aspect_ratio = clamped;
    }
    settings_changed(true);
}

bool low_vram_profile() { return g_settings.low_vram_profile; }

void set_low_vram_profile(bool on) {
    g_settings.low_vram_profile = on;
    settings_changed(true);
}

void set_theme_index(int index) {
    const int clamped = std::clamp(index, 0, ui::theme_count() - 1);
    ui::set_theme(clamped);
    g_settings.theme = clamped;
    settings_changed(true);
}

void key_event(int raw, Key key, bool down) {
    if (key == KeyF1 && down && g_capture < 0) {
        toggle();
        return;
    }
    if (!taking_input() && key != KeyNone) return;
    push({Event::KeyEv, raw, key, down});
}
bool capturing_key() { return g_capture >= 0; }
void mouse_move(float x, float y) {
    if (taking_input()) push({Event::Move, 0, KeyNone, false, x, y});
}
void mouse_button(int button, bool down) {
    if (taking_input()) push({Event::Button, button, KeyNone, down});
}
void mouse_wheel(float dy) {
    if (taking_input()) push({Event::Wheel, 0, KeyNone, false, 0, dy});
}
void text_input(uint32_t ch) {
    if (taking_input()) push({Event::Text, 0, KeyNone, false, 0, 0, ch});
}
void set_open(bool open, const char* why) {
    if (g_open.exchange(open) == open) return;
    g_input_frames = 30;
    log("overlay: menu %s (%s)", open ? "opened" : "closed", why);
}

void toggle() { set_open(!g_open.load(), "toggle"); }
bool is_open() { return g_open; }
bool wants_mouse() { return taking_input(); }
bool wants_keyboard() { return taking_input(); }
void set_screen(ScreenFn fn) {
    g_screen = fn;
    g_input_frames = 30;
}
bool screen_shown() { return g_screen.load() != nullptr; }
uint64_t pad_buttons() { return g_pad_buttons.load(); }

void filter_pad(plat::PadState& p) {
    if (!enabled()) return;
    g_pad_buttons = p.buttons, g_pad_lx = p.lx, g_pad_ly = p.ly;
    const uint64_t combo = plat::kPlus | plat::kMinus;
    bool combo_now = (p.buttons & combo) == combo, combo_before = (g_previous_buttons & combo) == combo;
    if (combo_now && !combo_before) toggle();
    if (g_open) {  // L / R switch tabs
        uint64_t pressed = p.buttons & ~g_previous_buttons;
        if (pressed & plat::kL) g_tab_request = (g_tab + menu_tab_count() - 1) % menu_tab_count();
        if (pressed & plat::kR) g_tab_request = (g_tab + 1) % menu_tab_count();
    }
    g_previous_buttons = p.buttons;
    if (taking_input() || combo_now) p = plat::PadState{};
}

void presented(uint64_t now_ns) {
    std::lock_guard lk(g_metrics_lock);
    if (g_previous_ns && now_ns > g_previous_ns) {
        double ms = double(now_ns - g_previous_ns) / 1e6;
        if (ms > 0 && ms < 10000) {
            g_intervals[g_next] = ms;
            g_next = (g_next + 1) % kHistory;
            g_count = std::min(kHistory, g_count + 1);
        }
    }
    g_previous_ns = now_ns;
}

void game_interval(int interval) { g_game_interval = std::max(1, interval); }
void game_speed(double percent) { g_game_speed.store(std::clamp(percent, 0.0, 1000.0), std::memory_order_relaxed); }

void speedrun_set_course(bool in_course, const char* world_name, const char* course_name) {
    if (in_course) {
        {
            std::lock_guard<std::mutex> lk(g_speedrun_meta_mu);
            if (world_name && *world_name) g_speedrun_world = world_name;
            if (course_name && *course_name) g_speedrun_course = course_name;
        }
        g_speedrun_igt_ns.store(0, std::memory_order_relaxed);
        g_speedrun_rta_ns.store(0, std::memory_order_relaxed);
        g_speedrun_finished.store(false, std::memory_order_relaxed);
        g_speedrun_exiting.store(false, std::memory_order_relaxed);
        g_speedrun_in_course.store(false, std::memory_order_relaxed);
        g_speedrun_armed.store(true, std::memory_order_relaxed);
    } else {
        g_speedrun_armed.store(false, std::memory_order_relaxed);
        if (g_speedrun_in_course.exchange(false, std::memory_order_relaxed) && g_settings.speedrun_timer &&
            !nextendo::status().enabled) {
            g_speedrun_exit_ns.store(plat::monotonic_ns(), std::memory_order_relaxed);
            g_speedrun_exiting.store(true, std::memory_order_relaxed);
        } else if (!g_speedrun_exiting.load(std::memory_order_relaxed)) {
            g_speedrun_finished.store(false, std::memory_order_relaxed);
        }
    }
}

void speedrun_reset_course() {
    g_speedrun_igt_ns.store(0, std::memory_order_relaxed);
    g_speedrun_rta_ns.store(0, std::memory_order_relaxed);
    g_speedrun_finished.store(false, std::memory_order_relaxed);
    g_speedrun_exiting.store(false, std::memory_order_relaxed);
    g_speedrun_in_course.store(false, std::memory_order_relaxed);
    g_speedrun_armed.store(true, std::memory_order_relaxed);
}

void speedrun_begin_play() {
    if (!g_speedrun_armed.exchange(false, std::memory_order_relaxed)) return;
    g_speedrun_igt_ns.store(0, std::memory_order_relaxed);
    g_speedrun_rta_ns.store(0, std::memory_order_relaxed);
    g_speedrun_finished.store(false, std::memory_order_relaxed);
    g_speedrun_in_course.store(true, std::memory_order_relaxed);
}

bool speedrun_is_armed() { return g_speedrun_armed.load(std::memory_order_relaxed); }

void speedrun_finish_course() {
    if (!g_speedrun_in_course.load(std::memory_order_relaxed)) return;
    if (g_speedrun_finished.exchange(true, std::memory_order_relaxed)) return;
    g_speedrun_finish_ns.store(plat::monotonic_ns(), std::memory_order_relaxed);
    if (!g_settings.speedrun_timer || nextendo::status().enabled) return;

    std::string world, course;
    {
        std::lock_guard<std::mutex> lk(g_speedrun_meta_mu);
        world = g_speedrun_world;
        course = g_speedrun_course;
    }
    const double igt_s = double(g_speedrun_igt_ns.load(std::memory_order_relaxed)) * 1e-9;
    const double rta_s = double(g_speedrun_rta_ns.load(std::memory_order_relaxed)) * 1e-9;
    const int mode = std::clamp(g_settings.speedrun_timer_mode, 0, 2);
    char line[256];
    if (mode == 0) {
        std::snprintf(line, sizeof line, "%s: %s - %.3fs\n", world.c_str(), course.c_str(), igt_s);
    } else if (mode == 1) {
        std::snprintf(line, sizeof line, "%s: %s - %.3fs\n", world.c_str(), course.c_str(), rta_s);
    } else {
        std::snprintf(line, sizeof line, "%s: %s - IGT %.3fs | RTA %.3fs\n", world.c_str(), course.c_str(), igt_s, rta_s);
    }
    const std::string& root = plat::data_root();
    if (!root.empty()) {
        std::ofstream f(nx_u8path(root) / "speedrun_times.txt", std::ios::app);
        if (f) f << line;
    }
    nx::log("speedrun: %s", line);
}

void speedrun_tick(double game_seconds, double wall_seconds, bool game_paused) {
    if (!g_speedrun_in_course.load(std::memory_order_relaxed)) return;
    if (g_speedrun_finished.load(std::memory_order_relaxed)) return;
    if (!g_settings.speedrun_timer || nextendo::status().enabled) return;
    if (wall_seconds > 0.0) {
        const uint64_t add = uint64_t(wall_seconds * 1e9);
        g_speedrun_rta_ns.fetch_add(add, std::memory_order_relaxed);
    }
    if (!g_open && !game_paused && game_seconds > 0.0) {
        const uint64_t add = uint64_t(game_seconds * 1e9);
        g_speedrun_igt_ns.fetch_add(add, std::memory_order_relaxed);
    }
}

bool frame_wanted() {
    return g_open || g_screen.load() || g_metrics || speedrun_timer_wanted() || g_input_frames > 0 || g_fading ||
           progress_wanted() || ui::notices_active();
}

void notify(const char* text) { ui::notify(text); }
void notify_screenshot(const uint8_t* rgba, int w, int h) { ui::notify_screenshot(rgba, w, h); }

void ensure_context() {
    if (ImGui::GetCurrentContext()) return;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // no imgui.ini next to the game
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "nx_overlay";
    io.ConfigNavMoveSetMousePos = false;
    ui::load_fonts();
}

ImDrawData* build_frame(uint32_t w, uint32_t h) {
    ImGuiIO& io = ImGui::GetIO();
    static uint64_t last_ns = 0;
    uint64_t now = plat::monotonic_ns();
    io.DeltaTime = last_ns && now > last_ns ? std::min(float(double(now - last_ns) / 1e9), 0.25f) : 1.0f / 60.0f;
    last_ns = now;
    io.DisplaySize = {float(w), float(h)};
    const float scale = std::clamp(float(h) / 720.0f, 0.75f, 4.0f);
    io.FontGlobalScale = ui::font_scale(float(h));
    ui::apply_style(float(h));

    std::vector<Event> events;
    {
        std::lock_guard lk(g_lock);
        events.swap(g_events);
        if (g_has_pending) g_settings = g_pending, g_has_pending = false;
    }
    bool changed = false;
    for (const Event& e : events) {
        switch (e.kind) {
            case Event::KeyEv:
                if (g_capture >= 0 && e.down) {  // the next key becomes the binding; Esc cancels
                    if (e.key != KeyEscape) {
                        if (g_capture < int(kControlCount))
                            g_settings.keyboard_bindings[size_t(g_capture)] = e.raw;
                        else if (g_capture < int(kControlCount + kHotkeyCount))
                            g_settings.hotkey_bindings[size_t(g_capture) - kControlCount] = e.raw;
                        changed = true;
                    }
                    g_capture = -1;
                    break;
                }
                if (e.key == KeyEscape && e.down && g_open && g_capture < 0 &&
                    !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup)) {
                    set_open(false, "Esc");
                    break;
                }
                if (ImGuiKey k = imgui_key(e.key); k != ImGuiKey_None) io.AddKeyEvent(k, e.down);
                break;
            case Event::Move: io.AddMousePosEvent(e.x, e.y); break;
            case Event::Button: io.AddMouseButtonEvent(e.raw, e.down); break;
            case Event::Wheel: io.AddMouseWheelEvent(0, e.y); break;
            case Event::Text: io.AddInputCharacter(e.ch); break;
        }
    }
    if (changed) settings_changed(true);
    // B on a controller backs out: inside a dropdown or an edit ImGui handles it; at the top it closes the menu.
    static uint64_t previous_pad = 0;
    const uint64_t pad = g_pad_buttons.load();
    if (g_open && (pad & ~previous_pad & plat::kB) && g_capture < 0 && !ImGui::IsAnyItemActive() &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup))
        set_open(false, "B");
    previous_pad = pad;
    feed_pad(io);
    if (g_input_frames > 0) g_input_frames--;

    ImGui::NewFrame();
    bool open = g_open;
    static bool was_open = false;
    const bool appearing = open && !was_open;
    was_open = open;
    // the menu fades in over about 0.15 s and out a little faster (drawn, without input, until it's gone)
    static float reveal = 0;
    if (appearing) reveal = std::max(reveal, 0.04f);
    reveal = ui::approach(reveal, open ? 1.0f : 0.0f, open ? 18.0f : 26.0f);
    g_fading = !open && reveal > 0.0f;
    const ScreenFn screen = g_screen.load();
    if (screen) screen(float(w), float(h));  // under the menu, which the screen can open (its Settings)
    if (progress_wanted()) draw_progress(float(h));
    if (g_settings.metrics_enabled && !progress_wanted() && !screen) draw_metrics(float(h));  // (no game frames to measure yet)
    if (speedrun_timer_wanted() && !progress_wanted() && !screen) draw_speedrun_timer(float(h));
    if (open || reveal > 0.0f) {
        const float a = ui::smoothstep(reveal);
        ImGui::GetBackgroundDrawList()->AddRectFilled({0, 0}, io.DisplaySize, IM_COL32(4, 2, 10, int(120 * a)));
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, a);
        bool still_open = open;
        MenuContext c{g_settings, g_host, g_capture, open ? g_tab_request.exchange(-1) : -1, still_open, scale,
                      appearing, float(h), reveal};
        draw_menu(c);
        ImGui::PopStyleVar();
        if (open && c.tab_request >= 0 && c.tab_request != g_tab) log("overlay: tab %d", c.tab_request);
        if (open && c.tab_request >= 0) g_tab = c.tab_request;
        if (open && !still_open) set_open(false, "menu"), g_capture = -1;
    }
    ui::draw_notices(float(h));
    ImGui::Render();
    return ImGui::GetDrawData();
}

}  // namespace nx::overlay
