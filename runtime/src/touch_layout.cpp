// Drawn touch controls (docs/nvn/ANDROID-PORT.md A4): a fixed layout of round buttons per game, hit-tested by
// platform_android_input.cpp and drawn with labels by overlay.cpp (or present shader on non-Android demos).
#include "touch_layout.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#include <nx/game.h>

namespace nx::plat {

namespace {

// x, y: 0..1 across the landscape screen; r: a fraction of the screen height.
const TouchButton kWonderDefaults[] = {
    {0.905f, 0.74f, 0.105f, kA, "A"},       {0.775f, 0.86f, 0.085f, kB, "B"},
    {0.775f, 0.60f, 0.085f, kY, "Y"},       {0.905f, 0.47f, 0.075f, kX, "X"},
    {0.665f, 0.74f, 0.075f, kY, "RUN"},     {0.955f, 0.25f, 0.055f, kZR, "ZR"},
    {0.955f, 0.07f, 0.045f, kR, "R"},       {0.045f, 0.07f, 0.045f, kL, "L"},
    {0.570f, 0.07f, 0.038f, kPlus, "+"},    {0.500f, 0.07f, 0.036f, kTouchMenu, "F1"},
    {0.430f, 0.07f, 0.038f, kMinus, "-"},
};
constexpr size_t kWonderCount = sizeof kWonderDefaults / sizeof kWonderDefaults[0];

TouchButton g_wonder[kWonderCount];
TouchButton g_snapshot[kWonderCount];
bool g_loaded = false;
std::atomic<bool> g_editing{false};

std::string layout_path() { return data_root() + "/touch_layout.ini"; }

void ensure_loaded() {
    if (g_loaded) return;
    g_loaded = true;
    std::memcpy(g_wonder, kWonderDefaults, sizeof kWonderDefaults);
    std::ifstream f(layout_path());
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        size_t idx = 0;
        float x = 0, y = 0, r = 0;
        if (std::sscanf(line.c_str(), "btn%zu=%f,%f,%f", &idx, &x, &y, &r) == 4 && idx < kWonderCount) {
            g_wonder[idx].x = std::clamp(x, 0.02f, 0.98f);
            g_wonder[idx].y = std::clamp(y, 0.02f, 0.98f);
            g_wonder[idx].r = std::clamp(r, 0.025f, 0.18f);
        }
    }
}

}  // namespace

const TouchButton* touch_buttons(size_t& count) {
    static const bool wonder = std::strcmp(game::kInfo.id, "wonder") == 0;
    if (!wonder) {
        count = 0;
        return nullptr;
    }
    ensure_loaded();
    count = kWonderCount;
    return g_wonder;
}

TouchButton* touch_buttons_mut(size_t& count) {
    static const bool wonder = std::strcmp(game::kInfo.id, "wonder") == 0;
    if (!wonder) {
        count = 0;
        return nullptr;
    }
    ensure_loaded();
    count = kWonderCount;
    return g_wonder;
}

uint64_t touch_hit(float x, float y, float aspect) {
    size_t n = 0;
    const TouchButton* b = touch_buttons(n);
    uint64_t best = 0;
    float best_d = 1e9f;
    for (size_t k = 0; k < n; k++) {
        float d = std::hypot((x - b[k].x) * aspect, y - b[k].y) / b[k].r;
        if (d < 1.35f && d < best_d) best = b[k].button, best_d = d;  // a little past the drawn edge still counts
    }
    return best;
}

void touch_edit_enter() {
    ensure_loaded();
    std::memcpy(g_snapshot, g_wonder, sizeof g_wonder);
    g_editing.store(true);
}

bool touch_edit_active() { return g_editing.load(); }

void touch_edit_save() {
    touch_save_layout();
    g_editing.store(false);
}

void touch_edit_cancel() {
    ensure_loaded();
    std::memcpy(g_wonder, g_snapshot, sizeof g_wonder);
    g_editing.store(false);
}

void touch_reset_layout() {
    g_loaded = true;
    std::memcpy(g_wonder, kWonderDefaults, sizeof kWonderDefaults);
}

void touch_save_layout() {
    ensure_loaded();
    make_dirs(data_root());
    std::ofstream f(layout_path(), std::ios::trunc);
    for (size_t i = 0; i < kWonderCount; i++)
        f << "btn" << i << "=" << g_wonder[i].x << "," << g_wonder[i].y << "," << g_wonder[i].r << "\n";
}

#if !defined(__ANDROID__)
size_t touch_overlay(OverlayShape* out, size_t max) {
    static const bool demo = std::getenv("NX_OVERLAY_DEMO") != nullptr;
    if (!demo) return 0;
    size_t n = 0, k = 0;
    const TouchButton* b = touch_buttons(n);
    for (; k < n && k < max; k++) out[k] = {b[k].x, b[k].y, b[k].r, 0.22f};
    if (k < max) out[k++] = {0.15f, 0.72f, 0.12f, 0.10f};  // where the stick usually goes
    return k;
}
uint64_t touch_held_buttons() { return 0; }
bool touch_using_gamepad() { return false; }
bool touch_stick_visual(float&, float&, float&, float&, bool&) { return false; }
#endif

}  // namespace nx::plat
