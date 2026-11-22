// Super Mario Bros. Wonder (010015100B514000, 1.0.0): what the shared runtime needs to know about the game.
// The game draws through NVN (docs/NVN.md); what we know about it is in docs/nvn/WONDER.md.
#include <nx/game.h>
#include <nx/runtime.h>
#include <nx/gamelib.h>
#include <nx/module.h>

#include <cstring>
#include <cstdlib>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>

#include "discord_rpc.h"
#include "overlay/overlay.h"
#include "platform.h"
#include "nvn/nvn_ultrawide.h"

namespace nx {

namespace nvn { void hold_scene_frame_cap(int fps); }

static std::atomic<bool> g_stage_load_ended{false};
static std::atomic<bool> g_course_demo_active{false};
static std::atomic<bool> g_in_course_session{false};
static std::atomic<bool> g_in_house_session{false};
static std::atomic<bool> g_house_cap_held{false};
static std::atomic<bool> g_player_died{false};
static std::atomic<uint64_t> g_stage_ready_ns{0};

void game::file_opened(const char* path) {
    static std::mutex world_mu;
    static std::string current_world = "World";
    // Update Discord Rich Presence when entering a world map or course.
    if (const char* bgm = std::strstr(path, "/Sound/Resource/Stream/BGM_World_")) {
        const char* wname = "Exploring the World Map";
        if (std::strstr(bgm, "World_01") || std::strstr(bgm, "World01")) wname = "World 1: Pipe-Rock Plateau";
        else if (std::strstr(bgm, "World_02") || std::strstr(bgm, "World02")) wname = "World 2: Fluff-Puff Peaks";
        else if (std::strstr(bgm, "World_03") || std::strstr(bgm, "World03")) wname = "World 3: Shining Falls";
        else if (std::strstr(bgm, "World_04") || std::strstr(bgm, "World04")) wname = "World 4: Sunbaked Desert";
        else if (std::strstr(bgm, "World_05") || std::strstr(bgm, "World05")) wname = "World 5: Fungi Mines";
        else if (std::strstr(bgm, "World_06") || std::strstr(bgm, "World06")) wname = "World 6: Deep Magma Bog";
        else if (std::strstr(bgm, "Special")) wname = "Special World";
        else if (std::strstr(bgm, "Main") || std::strstr(bgm, "Petal")) wname = "Petal Isles";
        {
            std::lock_guard<std::mutex> lk(world_mu);
            current_world = wname;
        }
        discord::set_activity("On the World Map", wname);
        if (!overlay::speedrun_is_armed()) {
            g_in_course_session.store(false, std::memory_order_relaxed);
            overlay::speedrun_set_course(false);
        }
    }
    // The map can remain resident while entering a course, so returning to it does not necessarily reopen its
    // StageLoadInfo. Its world music does reopen when the player returns from this house.
    if (g_house_cap_held.load(std::memory_order_relaxed) && std::strstr(path, "/Sound/Resource/Stream/BGM_World_") &&
        g_house_cap_held.exchange(false, std::memory_order_relaxed)) {
        nvn::hold_scene_frame_cap(0);
        log("wonder: house scene left for world map: selected frame cap restored");
        return;
    }
    constexpr const char* prefix = "/Stage/StageLoadInfo/";
    const char* scene = std::strstr(path, prefix);
    if (!scene) return;
    scene += std::strlen(prefix);
    if (std::strncmp(scene, "World", 5) == 0) discord::set_activity("On the World Map", "Exploring the Flower Kingdom");
    else if (std::strncmp(scene, "Course", 6) == 0) discord::set_activity("Playing a Course", "Flower Kingdom");
    const bool in_house = std::strncmp(scene, "Course5", 7) == 0 && scene[7] >= '0' && scene[7] <= '9' &&
                          scene[8] >= '0' && scene[8] <= '9' &&
                          std::strcmp(scene + 9, "_Course.game__stage__StageLoadInfo.bgyml") == 0;
    if (std::strncmp(scene, "World", 5) == 0) {
        if (!overlay::speedrun_is_armed()) {
            g_in_course_session.store(false, std::memory_order_relaxed);
            overlay::speedrun_set_course(false);
        }
    } else if (std::strncmp(scene, "Course", 6) == 0) {
        std::string wname;
        {
            std::lock_guard<std::mutex> lk(world_mu);
            wname = current_world;
        }
        char cname[64]{};
        const char* end = std::strstr(scene, "_Course.");
        const size_t len = end ? size_t(end - scene) : std::strlen(scene);
        std::snprintf(cname, sizeof cname, "%.*s", int(std::min(len, sizeof cname - 1)), scene);
        g_in_course_session.store(!in_house, std::memory_order_relaxed);
        g_in_house_session.store(in_house, std::memory_order_relaxed);
        g_player_died.store(false, std::memory_order_relaxed);
        g_stage_load_ended.store(false, std::memory_order_relaxed);
        g_course_demo_active.store(false, std::memory_order_relaxed);
        g_stage_ready_ns.store(0, std::memory_order_relaxed);
        overlay::speedrun_set_course(!in_house, wname.c_str(), cname);
        if (!in_house) log("speedrun: armed course %s (%s)", cname, wname.c_str());
    }
    if (g_house_cap_held.exchange(in_house, std::memory_order_relaxed) != in_house) {
        nvn::hold_scene_frame_cap(in_house ? 60 : 0);
        log("wonder: house scene %s (%s): %s", scene, in_house ? "loading" : "left",
            in_house ? "frame cap held at 60 for dialogue compatibility" : "selected frame cap restored");
    }
}

const game::Info game::kInfo = {"wonder", "Super Mario Bros. Wonder", 0x010015100B514000ull,
                                "io.github.belmantegu.wonderrecomp", "private/wonder",
                                0x1000000 /* main.npdm: 16 MiB system resource */};

namespace {

std::atomic<uint64_t> g_fade_mgr_off{0};
std::atomic<uint64_t> g_gamedata_mgr_off{0};
std::atomic<uint64_t> g_course_mgr_off{0};
std::atomic<uint64_t> g_stage_mgr_off{0};

bool is_faded_in() {
    const uint64_t off = g_fade_mgr_off.load(std::memory_order_relaxed);
    const ModuleInfo* main = game_lib().main;
    if (!off || !main) return true;
    const uint64_t mgr = ld<uint64_t>(main->base + off);
    if (!mgr) return false;
    const uint64_t f = ld<uint64_t>(mgr + 0x20);
    if (!f) return false;
    return (ld<uint32_t>(f + 0x8) | ld<uint32_t>(f + 0x48)) == 0 && ld<uint32_t>(f + 0x10) == 2;
}

bool is_game_paused() {
    const uint64_t off = g_gamedata_mgr_off.load(std::memory_order_relaxed);
    const ModuleInfo* main = game_lib().main;
    if (!off || !main) return false;
    const uint64_t mgr = ld<uint64_t>(main->base + off);
    if (!mgr) return false;
    const uint64_t bitset = mgr + 0x30;
    const uint64_t tbl = ld<uint64_t>(bitset + 0x8);
    if (!tbl) return false;
    const uint32_t cap = ld<uint32_t>(tbl + 0x14);
    if (!cap) return false;
    const uint64_t hashes = ld<uint64_t>(tbl);
    const uint64_t indices = ld<uint64_t>(tbl + 0x8);
    if (!hashes || !indices) return false;
    constexpr uint32_t kHash = 0x35db180eu;  // "IsGamePause"
    const uint32_t start = kHash % cap;
    uint32_t idx = start;
    for (uint32_t step = 0; step < cap; ++step) {
        const uint32_t h = ld<uint32_t>(hashes + uint64_t(idx) * 4);
        if (h == kHash) {
            const uint8_t bit = ld<uint8_t>(indices + idx);
            return ((ld<uint64_t>(bitset) >> (bit & 63u)) & 1ull) != 0;
        }
        if (h == 0) return false;
        idx = (idx + 1 < cap) ? (idx + 1) : 0;
        if (idx == start) return false;
    }
    return false;
}

bool is_all_players_dead() {
    const uint64_t off = g_course_mgr_off.load(std::memory_order_relaxed);
    const ModuleInfo* main = game_lib().main;
    if (!off || !main) return false;
    const uint64_t mgr = ld<uint64_t>(main->base + off);
    if (!mgr) return false;
    const uint64_t pinfo = ld<uint64_t>(mgr + 0x40);
    if (!pinfo) return false;
    const int32_t count = int32_t(ld<uint32_t>(pinfo + 0x7e4));
    if (count < 1) return false;
    uint32_t dead = 0;
    const int32_t n = std::min(count, 4);
    for (int32_t i = 0; i < n; ++i) dead += ld<uint8_t>(pinfo + 0x7ca + uint64_t(i));
    return dead >= uint32_t(n);
}

bool is_world_map_stage() {
    const uint64_t off = g_stage_mgr_off.load(std::memory_order_relaxed);
    const ModuleInfo* main = game_lib().main;
    if (!off || !main) return false;
    const uint64_t mgr = ld<uint64_t>(main->base + off);
    if (!mgr) return false;
    const uint64_t a = ld<uint64_t>(mgr + 0x30);
    const uint64_t b = a ? ld<uint64_t>(a + 0x8) : 0;
    if (!b) return false;
    const uint32_t cnt = ld<uint32_t>(b);
    const uint64_t arr = ld<uint64_t>(b + 0x8);
    if (!arr) return false;
    return ld<uint64_t>(arr + (cnt > 1 ? 8ull : 0ull)) != 0;
}

int32_t course_player_count() {
    const uint64_t off = g_course_mgr_off.load(std::memory_order_relaxed);
    const ModuleInfo* main = game_lib().main;
    if (!off || !main) return 0;
    const uint64_t mgr = ld<uint64_t>(main->base + off);
    if (!mgr) return 0;
    const uint64_t pinfo = ld<uint64_t>(mgr + 0x40);
    if (!pinfo) return 0;
    return int32_t(ld<uint32_t>(pinfo + 0x7e4));
}

std::atomic<uint64_t> g_wide_calls{0};
Fn g_wide_original = nullptr;
Fn g_ortho_original = nullptr;
Fn g_camera_entry_original = nullptr;
Fn g_active_rect_original = nullptr;
uint64_t g_world_camera_caller = 0;
std::atomic<unsigned> g_active_rect_count{0};
std::atomic<unsigned> g_camera_entry_calls{0};
std::atomic<unsigned> g_ortho_samples{0};
std::atomic<unsigned> g_ortho_adjustments{0};
static uint64_t frame_step_page();

void active_rect_hook(CPU& c) {
    const ModuleInfo* main = game_lib().main;
    static const uint64_t chosen_caller = [] {
        const char* v = std::getenv("NX_UW_RECT_CALLER");
        return v ? std::strtoull(v, nullptr, 16) : uint64_t(0x6caba4);
    }();
    static const bool candidate_group = [] {
        const char* v = std::getenv("NX_UW_RECT_GROUP_TEST");
        return v && std::strcmp(v, "1") == 0;
    }();
    static const bool candidate_all = [] {
        const char* v = std::getenv("NX_UW_RECT_ALL_TEST");
        return v && std::strcmp(v, "1") == 0;
    }();
    const uint64_t rel = main && c.x[30] >= main->base ? c.x[30] - main->base : 0;
    const bool relevant = main && (candidate_all || rel == chosen_caller || (candidate_group &&
        (rel == 0x1d1f50 || rel == 0x441328 || rel == 0x82d62c || rel == 0x3293a8 ||
         rel == 0x329008 || rel == 0x49b224)));
    const uint64_t caller = c.x[30];
    const uint64_t result = c.x[0];
    g_active_rect_original(c);
    static const bool rect_trace = std::getenv("NX_UW_RECT_TRACE") &&
                                   std::strcmp(std::getenv("NX_UW_RECT_TRACE"), "1") == 0;
    if (rect_trace && main && result && ld<uint8_t>(result + 0x10)) {
        static std::atomic<uint64_t> seen[64]{};
        bool first = false;
        for (auto& slot : seen) {
            uint64_t value = slot.load(std::memory_order_relaxed);
            if (value == caller) break;
            if (!value && slot.compare_exchange_strong(value, caller, std::memory_order_relaxed)) {
                first = true;
                break;
            }
            if (value == caller) break;
        }
        const bool valid = ld<uint8_t>(result + 0x10) != 0;
        if (first) log("widescreen rect trace: caller=main+%llx valid=%u bounds=%g,%g,%g,%g",
            (unsigned long long)(caller - main->base), unsigned(valid), double(ld<float>(result)),
            double(ld<float>(result + 4)), double(ld<float>(result + 8)), double(ld<float>(result + 12)));
    }
    static const bool active = std::getenv("NX_UW_ACTIVE_AREA_TEST") &&
                               std::strcmp(std::getenv("NX_UW_ACTIVE_AREA_TEST"), "1") == 0;
    if (!active) return;
    if (!relevant || !result || !ld<uint8_t>(result + 0x10)) return;
    const float left = ld<float>(result), right = ld<float>(result + 8);
    const float factor = nvn::uw_factor();
    if (!std::isfinite(left) || !std::isfinite(right) || right <= left || factor <= 1.f) return;
    const float extra = (right - left) * (factor - 1.f) * 0.5f;
    if (!std::isfinite(extra) || extra > 10000.f) return;
    st<float>(result, left - extra);
    st<float>(result + 8, right + extra);
    if (g_active_rect_count.fetch_add(1, std::memory_order_relaxed) < (candidate_all ? 32u : 8u))
        log("widescreen active-area test: caller=main+%llx index=%u bounds=%.3f,%.3f..%.3f,%.3f -> x %.3f..%.3f",
            (unsigned long long)rel, unsigned(c.x[1]), double(left), double(ld<float>(result + 4)),
            double(right), double(ld<float>(result + 12)), double(left - extra), double(right + extra));
}

static bool horplus_enabled() {
    const char* value = std::getenv("NX_WONDER_HORPLUS");
    return value && value[0] && std::strcmp(value, "0") != 0;
}

static float horplus_aspect() {
    const uint64_t page = frame_step_page();
    if (!page) return 16.0f / 9.0f;
    return ld<float>(page + 8);
}

void ortho_camera_hook(CPU& c) {
    const unsigned sample = g_ortho_samples.fetch_add(1, std::memory_order_relaxed);
    const uint64_t p = c.x[0];
    const float top = ld<float>(p + 0xa0), bottom = ld<float>(p + 0xa4);
    float left = ld<float>(p + 0xa8), right = ld<float>(p + 0xac);
    const float current_aspect = (right - left) / std::abs(top - bottom);
    const float target_aspect = nvn::uw_on() ? (16.0f / 9.0f) * nvn::uw_factor() : horplus_aspect();
    const ModuleInfo* main = game_lib().main;
    const bool world_camera = main && g_world_camera_caller && c.x[30] == main->base + g_world_camera_caller;
    if (((nvn::uw_on() && world_camera) || horplus_enabled()) &&
        std::isfinite(current_aspect) && std::isfinite(target_aspect) &&
        current_aspect > 1.76f && current_aspect < 1.80f && target_aspect > current_aspect + 0.01f &&
        target_aspect < 4.1f) {
        const float center = (left + right) * 0.5f;
        const float half_width = std::abs(top - bottom) * target_aspect * 0.5f;
        left = center - half_width;
        right = center + half_width;
        st<float>(p + 0xa8, left);
        st<float>(p + 0xac, right);
        const unsigned adjusted = g_ortho_adjustments.fetch_add(1, std::memory_order_relaxed);
        if (adjusted < 32)
            log("widescreen ortho-camera: adjusted caller=%llx aspect %.5f -> %.5f bounds %.4f..%.4f",
                (unsigned long long)c.x[30], double(current_aspect), double(target_aspect), double(left), double(right));
    } else if (plat::setting("wonder_widescreen_trace") == "1" && (sample < 32 || (sample % 5000) == 0)) {
        log("widescreen ortho-camera: object=%llx caller=%llx left=%.4f right=%.4f top=%.4f bottom=%.4f",
            (unsigned long long)p, (unsigned long long)c.x[30], double(left), double(right), double(top), double(bottom));
    }
    g_ortho_original(c);
}

void wide_probe(CPU& c) {
    g_wide_calls.fetch_add(1, std::memory_order_relaxed);
    g_wide_original(c);
}

void camera_entry_probe(CPU& c) {
    unsigned n = g_camera_entry_calls.fetch_add(1, std::memory_order_relaxed);
    if (n < 16 || n % 10000 == 0)
        log("widescreen camera-entry: count=%u caller=%llx object=%llx runtime-aspect=%.5f", n + 1,
            (unsigned long long)c.x[30], (unsigned long long)c.x[0], double(horplus_aspect()));
    g_camera_entry_original(c);
}

void install_wide_probe() {
    const bool trace = plat::setting("wonder_widescreen_trace") == "1";
    const char* uw = std::getenv("NX_ULTRAWIDE");
    if (!trace && !horplus_enabled() && !(uw && uw[0] && uw[0] != '0')) return;
    const ModuleInfo* main = game_lib().main;
    if (!main) return;
    constexpr uint64_t offset = 0x1c34328;
    constexpr uint32_t code[] = {0xfc1b0fee, 0x6d00b3ed, 0xf90027f3, 0x6d01abeb,
                                 0x6d02a3e9, 0xa903fbfd, 0x9100e3fd, 0xb000cfc8};
    const LoadedModule* module = module_at(main->base);
    if (!module) return;
    const char* active_test = std::getenv("NX_UW_ACTIVE_AREA_TEST");
    const char* rect_trace = std::getenv("NX_UW_RECT_TRACE");
    if ((active_test && std::strcmp(active_test, "1") == 0) ||
        (rect_trace && std::strcmp(rect_trace, "1") == 0)) {
        constexpr uint64_t fn = 0x1d6610;
        constexpr uint32_t words[] = {0x2a2103e8, 0x3900401f, 0x72001d1f, 0xa9007c1f};
        bool ok = fn + 16 <= main->text_size && 0x6caba4 <= main->text_size &&
                  ld<uint32_t>(main->base + 0x6caba0) == 0x97ec2e9c;
        for (unsigned i = 0; ok && i < 4; ++i) ok &= ld<uint32_t>(main->base + fn + 4 * i) == words[i];
        if (ok && module->table[fn / 4]) {
            g_active_rect_original = module->table[fn / 4];
            module->table[fn / 4] = active_rect_hook;
            log("widescreen active-area test: hook installed at main+1d6610");
        } else log("widescreen active-area test: incompatible code or unavailable");
    }
    constexpr uint64_t camera_entry = 0x160de38;
    if (trace && camera_entry + 4 <= main->text_size && ld<uint32_t>(main->base + camera_entry) == 0x14000003 &&
        module->table[camera_entry / 4]) {
        g_camera_entry_original = module->table[camera_entry / 4];
        module->table[camera_entry / 4] = camera_entry_probe;
        log("widescreen camera-entry: probe installed at main+160de38");
    }
    struct OrthoTarget {
        const char* version;
        uint64_t ortho;
        uint64_t world_caller;
    };
    constexpr OrthoTarget ortho_targets[] = {
        {"1.0.0", 0x10578c, 0x1a95e44},
        {"1.2.1", 0xf18f0, 0x1bbbf38},
    };
    constexpr uint32_t ortho_code[] = {0x2d550400, 0x1e3c1003, 0x1e2e1004, 0xd2e7f008};
    bool ortho_installed = false;
    for (const OrthoTarget& target : ortho_targets) {
        if (target.ortho + sizeof(ortho_code) > main->text_size || (target.ortho & 3u) != 0) continue;
        bool compatible = true;
        for (unsigned i = 0; i < 4; ++i)
            compatible &= ld<uint32_t>(main->base + target.ortho + 4 * i) == ortho_code[i];
        if (compatible && module->table[target.ortho / 4]) {
            g_world_camera_caller = target.world_caller;
            g_ortho_original = module->table[target.ortho / 4];
            module->table[target.ortho / 4] = ortho_camera_hook;
            log("widescreen ortho-camera: %s hook installed (%s at main+%llx)",
                (horplus_enabled() || (uw && uw[0] && uw[0] != '0')) ? "ultrawide" : "trace",
                target.version, (unsigned long long)target.ortho);
            ortho_installed = true;
            break;
        }
    }
    if (!ortho_installed)
        log("widescreen ortho-camera: incompatible code or unavailable; probe disabled");
    if (!trace) return;
    if (offset + sizeof(code) > main->text_size) {
        log("widescreen calls: incompatible game code; probe disabled");
        return;
    }
    for (unsigned i = 0; i < 8; ++i) {
        if (ld<uint32_t>(main->base + offset + 4 * i) != code[i]) {
            log("widescreen calls: incompatible game code; probe disabled");
            return;
        }
    }
    g_wide_original = module->table[offset / 4];
    if (!g_wide_original) {
        log("widescreen calls: function unavailable; probe disabled");
        return;
    }
    module->table[offset / 4] = wide_probe;
    log("widescreen calls: probe installed at main+%llx", (unsigned long long)offset);
}

static uint32_t float_bits(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static uint64_t frame_step_page() {
    const ModuleInfo* main = game_lib().main;
    if (!main || !main->frame_step_offset) return 0;
    return main->base + main->frame_step_offset;
}

static void store_frame_step(float seconds) {
    const uint64_t page = frame_step_page();
    if (!page) return;
    const float ratio = seconds * 60.0f;
    uint64_t bits = 0;
    std::memcpy(&bits, &ratio, sizeof ratio);
    std::memcpy(reinterpret_cast<uint8_t*>(&bits) + sizeof ratio, &seconds, sizeof seconds);
    auto* step = reinterpret_cast<uint64_t*>(static_cast<uintptr_t>(page));
    std::atomic_ref<uint64_t>(*step).store(bits, std::memory_order_release);
}

static void update_wide_aspect() {
    static const bool horplus = [] {
        const char* value = std::getenv("NX_WONDER_HORPLUS");
        return value && value[0] && std::strcmp(value, "0") != 0;
    }();
    static uint32_t last_width = 0, last_height = 0;
    uint32_t width = 1920, height = 1080;
    if (horplus) plat::window_size(width, height);
    if (!width || !height) width = 1920, height = 1080;
    if (width == last_width && height == last_height) return;

    const uint64_t page = frame_step_page();
    if (!page) return;
    const float aspect = horplus ? float(width) / float(height) : (16.0f / 9.0f);
    auto* aspect_bits = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(page + 8));
    std::atomic_ref<uint32_t>(*aspect_bits).store(float_bits(aspect), std::memory_order_release);
    last_width = width;
    last_height = height;
    if (horplus)
        log("widescreen perspective: runtime camera aspect %.5f from window %u x %u", double(aspect), width, height);
}

static void init_frame_step() {
    if (!frame_step_page()) {
        log("wonder: real-time frame step page was not reserved; retaining the game's original frame timing");
        return;
    }
    store_frame_step(1.0f / 60.0f);
    update_wide_aspect();
    plat::overlay_game_speed(100.0);
    log("wonder: real-time frame step initialized at 16.67 ms");
}

// Gather evidence about Wonder's own virtual-screen globals before implementing Hor+.
// The final image pass can only fit, crop, or stretch its 16:9 source; finding and
// classifying these game-side values is needed to expand the camera while preserving HUD layout.
static void trace_screen_globals() {
    if (plat::setting("wonder_widescreen_trace") != "1") return;
    const ModuleInfo& nx_module_main = *game_lib().main;
    const uint64_t begin = nx_module_main.base + nx_module_main.data;
    const uint64_t end = nx_module_main.base + nx_module_main.bss + nx_module_main.bss_size;
    const uint32_t pairs[][2] = {
        {1920, 1080}, {1280, 720}, {960, 540}, {640, 360},
        {float_bits(1920.0f), float_bits(1080.0f)},
        {float_bits(1280.0f), float_bits(720.0f)},
        {float_bits(960.0f), float_bits(540.0f)},
        {float_bits(640.0f), float_bits(360.0f)},
    };
    constexpr const char* names[] = {"1920x1080 ints", "1280x720 ints", "960x540 ints", "640x360 ints",
                                    "1920x1080 floats", "1280x720 floats", "960x540 floats", "640x360 floats"};
    log("widescreen probe: scanning main data+bss [%llx,%llx)", (unsigned long long)begin,
        (unsigned long long)end);
    for (size_t p = 0; p < sizeof(pairs) / sizeof(pairs[0]); ++p) {
        unsigned count = 0;
        for (uint64_t address = begin; address + 8 <= end; address += 4) {
            const auto* words = reinterpret_cast<const uint32_t*>(address);
            if (words[0] != pairs[p][0] || words[1] != pairs[p][1]) continue;
            if (count < 24)
                log("widescreen probe: %s candidate main+%llx", names[p],
                    (unsigned long long)(address - nx_module_main.base));
            ++count;
        }
        log("widescreen probe: %s count=%u", names[p], count);
    }
}

}  // namespace

namespace nvn {
int frame_cap();
int held_frame_cap();
}

namespace wonder {
void menu_init();
void seed_guard_init();
void seed_guard_tick(uint64_t now_ns);
}

void game::after_init() {
    discord::set_activity("Title Screen", "Super Mario Bros. Wonder");
    init_frame_step();
    trace_screen_globals();
    install_wide_probe();
    wonder::menu_init();
    wonder::seed_guard_init();

    // Hook Goal Pole slide-down & Course Clear AINB nodes via getNodeName() + adjacent enter(), and
    // resolve IsEndFadeIn / IsGamePause singletons for accurate course start & pause detection on 1.0.0 and 1.2.1.
    static Fn goal_pole_orig = nullptr;
    static Fn course_clear_orig = nullptr;
    static Fn end_load_orig = nullptr;
    static Fn demo_enter_orig = nullptr;
    static Fn demo_update_orig = nullptr;
    static Fn player_dead_orig = nullptr;
    static Fn actor_reset_orig = nullptr;
    static Fn stage_info_parse_orig = nullptr;
    const auto find_ro_string = [](const ModuleInfo* main, const char* needle) -> uint64_t {
        const size_t nlen = std::strlen(needle);
        if (!main || main->ro_size < nlen + 2) return UINT64_MAX;
        const char* base = reinterpret_cast<const char*>(main->base + main->ro);
        for (size_t i = 1; i + nlen < main->ro_size; ++i) {
            if (base[i - 1] == '\0' && base[i] == needle[0] && std::memcmp(base + i, needle, nlen + 1) == 0)
                return main->ro + i;
        }
        return UINT64_MAX;
    };
    const auto find_ainb_node_fn = [&](const char* node_name) -> uint64_t {
        const ModuleInfo* main = game_lib().main;
        if (!main || main->text_size < 0x20) return UINT64_MAX;
        const uint64_t str_off = find_ro_string(main, node_name);
        if (str_off == UINT64_MAX) return UINT64_MAX;
        const uint64_t str_page = str_off & ~0xfffull;
        const uint32_t add_x0 = 0x91000000u | uint32_t((str_off & 0xfffu) << 10);
        for (uint64_t off = main->text; off + 12 <= main->text + main->text_size; off += 4) {
            if (ld<uint32_t>(main->base + off + 4) != add_x0) continue;
            if (ld<uint32_t>(main->base + off + 8) != 0xd65f03c0u) continue;
            const uint32_t i0 = ld<uint32_t>(main->base + off);
            if ((i0 & 0x9f00001fu) != 0x90000000u) continue;
            const int32_t immlo = int32_t((i0 >> 29) & 3u);
            const int32_t immhi = int32_t((i0 >> 5) & 0x7ffffu);
            int64_t imm = (int64_t(immhi) << 2) | immlo;
            if (imm & (1ll << 20)) imm -= (1ll << 21);
            if (((off & ~0xfffull) + uint64_t(imm << 12)) == str_page) return off;
        }
        return UINT64_MAX;
    };
    const auto install_ainb_enter = [&](const char* node_name, const uint32_t (&sig)[3], Fn repl, Fn& orig) {
        const ModuleInfo* main = game_lib().main;
        const LoadedModule* m = main ? module_at(main->base) : nullptr;
        const uint64_t node_fn = find_ainb_node_fn(node_name);
        if (!main || !m || node_fn == UINT64_MAX) {
            log("speedrun: getNodeName for %s not found", node_name);
            return;
        }
        uint64_t target = UINT64_MAX;
        for (uint64_t off = node_fn + 0x18; off + 12 <= std::min(node_fn + 0x80, main->text + main->text_size); off += 4) {
            if (ld<uint32_t>(main->base + off) == sig[0] && ld<uint32_t>(main->base + off + 4) == sig[1] &&
                ld<uint32_t>(main->base + off + 8) == sig[2] && m->table[off / 4]) {
                target = off;
                break;
            }
        }
        if (target == UINT64_MAX) {
            log("speedrun: enter() for %s not found near main+%llx", node_name, (unsigned long long)node_fn);
            return;
        }
        orig = m->table[target / 4];
        m->table[target / 4] = repl;
        log("speedrun: hooked %s at main+%llx", node_name, (unsigned long long)target);
    };
    const auto extract_adrp_ldr_x8 = [&](const char* node_name) -> uint64_t {
        const ModuleInfo* main = game_lib().main;
        const uint64_t node_fn = find_ainb_node_fn(node_name);
        if (!main || node_fn == UINT64_MAX) return 0;
        const uint64_t end = std::min(node_fn + 0x60, main->text + main->text_size);
        for (uint64_t off = node_fn + 0x18; off + 16 <= end; off += 4) {
            const uint32_t i0 = ld<uint32_t>(main->base + off);
            if ((i0 & 0x9f00001fu) != 0x90000008u) continue;  // adrp x8, ...
            const int32_t immlo = int32_t((i0 >> 29) & 3u);
            const int32_t immhi = int32_t((i0 >> 5) & 0x7ffffu);
            int64_t imm = (int64_t(immhi) << 2) | immlo;
            if (imm & (1ll << 20)) imm -= (1ll << 21);
            const uint64_t page = (off & ~0xfffull) + uint64_t(imm << 12);
            for (uint64_t k = off + 4; k <= std::min(off + 16, end - 4); k += 4) {
                const uint32_t i1 = ld<uint32_t>(main->base + k);
                if ((i1 & 0xffc003ffu) == 0xf9400108u) {  // ldr x8, [x8, #imm12*8]
                    const uint64_t imm12 = uint64_t((i1 >> 10) & 0xfffu) << 3;
                    return page + imm12;
                }
            }
        }
        return 0;
    };
    static constexpr uint32_t kSlidePoleSig[3] = {0xb9401808, 0x52b00009, 0x33007509};
    static constexpr uint32_t kCourseClearSig[3] = {0xd10243ff, 0xa9037bfd, 0xa9046ffc};
    install_ainb_enter("NotifyStartSlideDownGoalPoleToCourseMgr", kSlidePoleSig,
                       [](CPU& c) {
                           overlay::speedrun_finish_course();
                           goal_pole_orig(c);
                       },
                       goal_pole_orig);
    install_ainb_enter("SetCourseClearFlagToGameData", kCourseClearSig,
                       [](CPU& c) {
                           overlay::speedrun_finish_course();
                           course_clear_orig(c);
                       },
                       course_clear_orig);
    static constexpr uint32_t kEndLoadSig[3] = {0xd100c3ff, 0xa9017bfd, 0xf90013f3};
    install_ainb_enter("EndLoadStage", kEndLoadSig,
                       [](CPU& c) {
                           if (!g_stage_load_ended.exchange(true, std::memory_order_relaxed))
                               log("speedrun: EndLoadStage");
                           end_load_orig(c);
                       },
                       end_load_orig);
    install_ainb_enter("PlayerCourseInDemo", kEndLoadSig,
                       [](CPU& c) {
                           if (overlay::speedrun_is_armed()) {
                               g_course_demo_active.store(true, std::memory_order_relaxed);
                               log("speedrun: PlayerCourseInDemo::enter");
                           }
                           demo_enter_orig(c);
                       },
                       demo_enter_orig);
    static Fn demo_calc_orig = nullptr;
    if (const uint64_t demo_node = find_ainb_node_fn("PlayerCourseInDemo"); demo_node != UINT64_MAX) {
        const ModuleInfo* main = game_lib().main;
        const LoadedModule* m = main ? module_at(main->base) : nullptr;
        if (main && m) {
            const uint64_t limit = std::min(demo_node + 0x600, main->text + main->text_size);
            for (uint64_t off = demo_node + 0x80; off + 32 <= limit; off += 4) {
                if (ld<uint32_t>(main->base + off) == 0xd10183ffu && m->table[off / 4]) {
                    bool has_ldr60 = false;
                    for (uint64_t k = off + 4; k + 8 <= off + 0x30; k += 4) {
                        if (ld<uint32_t>(main->base + k) == 0xf9403008u) {
                            has_ldr60 = true;
                            break;
                        }
                    }
                    if (!has_ldr60) continue;
                    demo_calc_orig = m->table[off / 4];
                    m->table[off / 4] = [](CPU& c) {
                        demo_calc_orig(c);
                        if (uint32_t(c.x[0]) != 2u) {
                            g_course_demo_active.store(false, std::memory_order_relaxed);
                            g_player_died.store(false, std::memory_order_relaxed);
                            if (overlay::speedrun_is_armed()) {
                                overlay::speedrun_begin_play();
                                log("speedrun: started after PlayerCourseInDemo::calc");
                            }
                        }
                    };
                    log("speedrun: hooked PlayerCourseInDemo calc at main+%llx", (unsigned long long)off);
                    break;
                }
            }
        }
    }
    static constexpr uint32_t kDeadSig[3] = {0xf9400408, 0xf9400508, 0xf940fd09};
    install_ainb_enter("SetPlayerDeadMorphRelease", kDeadSig,
                       [](CPU& c) {
                           if (g_in_course_session.load(std::memory_order_relaxed) &&
                               !g_player_died.exchange(true, std::memory_order_relaxed)) {
                               g_stage_load_ended.store(false, std::memory_order_relaxed);
                               g_course_demo_active.store(false, std::memory_order_relaxed);
                               g_stage_ready_ns.store(0, std::memory_order_relaxed);
                               overlay::speedrun_reset_course();
                               log("speedrun: player died, timer reset and armed");
                           }
                           player_dead_orig(c);
                       },
                       player_dead_orig);
    static constexpr uint32_t kActorResetSig[3] = {0xf9401c08, 0xb9401809, 0x39400108};
    install_ainb_enter("ActorResetOnRetryCourse", kActorResetSig,
                       [](CPU& c) {
                           if (g_in_course_session.load(std::memory_order_relaxed) &&
                               !g_player_died.exchange(true, std::memory_order_relaxed)) {
                               g_stage_load_ended.store(false, std::memory_order_relaxed);
                               g_course_demo_active.store(false, std::memory_order_relaxed);
                               g_stage_ready_ns.store(0, std::memory_order_relaxed);
                               overlay::speedrun_reset_course();
                               log("speedrun: ActorResetOnRetryCourse, timer reset and armed");
                           }
                           actor_reset_orig(c);
                       },
                       actor_reset_orig);
    // Enable Wonder's native Pause Menu "Restart Course" (L_RetryBtn, right above Exit Course) in every level.
    {
        const ModuleInfo* main = game_lib().main;
        const LoadedModule* m = main ? module_at(main->base) : nullptr;
        const uint64_t str_off = main ? find_ro_string(main, "IsEnablePauseMenuRestart") : UINT64_MAX;
        if (main && m && str_off != UINT64_MAX) {
            const uint64_t str_page = str_off & ~0xfffull;
            const uint32_t add_x8 = 0x91000108u | uint32_t((str_off & 0xfffu) << 10);
            for (uint64_t off = main->text + 0x40; off + 8 <= main->text + main->text_size; off += 4) {
                if (ld<uint32_t>(main->base + off + 4) != add_x8) continue;
                const uint32_t i0 = ld<uint32_t>(main->base + off);
                if ((i0 & 0x9f00001fu) != 0x90000008u) continue;
                const int32_t immlo = int32_t((i0 >> 29) & 3u);
                const int32_t immhi = int32_t((i0 >> 5) & 0x7ffffu);
                int64_t imm = (int64_t(immhi) << 2) | immlo;
                if (imm & (1ll << 20)) imm -= (1ll << 21);
                if (((off & ~0xfffull) + uint64_t(imm << 12)) != str_page) continue;
                for (uint64_t fn = off - 0x40; fn < off; fn += 4) {
                    if (ld<uint32_t>(main->base + fn) == 0xd10143ffu &&
                        ld<uint32_t>(main->base + fn + 4) == 0xa9027bfdu && m->table[fn / 4]) {
                        stage_info_parse_orig = m->table[fn / 4];
                        m->table[fn / 4] = [](CPU& c) {
                            const uint64_t info = c.x[0];
                            stage_info_parse_orig(c);
                            if (info) {
                                st<uint8_t>(info + 0x39, 1);
                                st<uint8_t>(info + 0x47, ld<uint8_t>(info + 0x47) | 1u);
                            }
                        };
                        log("menu: enabled Pause Menu Restart at main+%llx", (unsigned long long)fn);
                        break;
                    }
                }
                if (stage_info_parse_orig) break;
            }
        }
    }
    if (const uint64_t demo_node = find_ainb_node_fn("PlayerCourseInDemo"); demo_node != UINT64_MAX) {
        const ModuleInfo* main = game_lib().main;
        const LoadedModule* m = main ? module_at(main->base) : nullptr;
        if (main && m) {
            const uint64_t limit = std::min(demo_node + 0x600, main->text + main->text_size);
            for (uint64_t off = demo_node + 0x100; off + 20 <= limit; off += 4) {
                if (ld<uint32_t>(main->base + off) == 0xd100c3ffu &&
                    ld<uint32_t>(main->base + off + 4) == 0xa9017bfdu &&
                    ld<uint32_t>(main->base + off + 8) == 0xf90013f3u &&
                    ld<uint32_t>(main->base + off + 16) == 0xf9403008u && m->table[off / 4]) {
                    demo_update_orig = m->table[off / 4];
                    m->table[off / 4] = [](CPU& c) {
                        const uint64_t node = c.x[0];
                        demo_update_orig(c);
                        if (node && ((ld<uint32_t>(node + 0x18) >> 30) & 3u) != 2u) {
                            g_course_demo_active.store(false, std::memory_order_relaxed);
                            g_player_died.store(false, std::memory_order_relaxed);
                            if (overlay::speedrun_is_armed()) {
                                overlay::speedrun_begin_play();
                                log("speedrun: started after PlayerCourseInDemo");
                            }
                        }
                    };
                    log("speedrun: hooked PlayerCourseInDemo update at main+%llx", (unsigned long long)off);
                    break;
                }
            }
        }
    }
    g_fade_mgr_off.store(extract_adrp_ldr_x8("IsEndFadeIn"), std::memory_order_relaxed);
    g_gamedata_mgr_off.store(extract_adrp_ldr_x8("IsGamePause"), std::memory_order_relaxed);
    g_course_mgr_off.store(extract_adrp_ldr_x8("IsAllPlayerDead"), std::memory_order_relaxed);
    // Resolve StageMgr from main_01C3B304 (loads "IsWorldMapStage" into x8 via adrp+add, then loads StageMgr into x8).
    {
        const ModuleInfo* main = game_lib().main;
        const uint64_t str_off = main ? find_ro_string(main, "IsWorldMapStage") : UINT64_MAX;
        if (main && str_off != UINT64_MAX) {
            const uint64_t str_page = str_off & ~0xfffull;
            const uint32_t add_x8 = 0x91000108u | uint32_t((str_off & 0xfffu) << 10);
            for (uint64_t off = main->text; off + 20 <= main->text + main->text_size; off += 4) {
                if (ld<uint32_t>(main->base + off + 4) != add_x8) continue;
                const uint32_t i0 = ld<uint32_t>(main->base + off);
                if ((i0 & 0x9f00001fu) != 0x90000008u) continue;
                const int32_t immlo = int32_t((i0 >> 29) & 3u);
                const int32_t immhi = int32_t((i0 >> 5) & 0x7ffffu);
                int64_t imm = (int64_t(immhi) << 2) | immlo;
                if (imm & (1ll << 20)) imm -= (1ll << 21);
                if (((off & ~0xfffull) + uint64_t(imm << 12)) != str_page) continue;
                for (uint64_t k = off + 8; k + 8 <= std::min(off + 0x24, main->text + main->text_size); k += 4) {
                    const uint32_t s0 = ld<uint32_t>(main->base + k);
                    const uint32_t s1 = ld<uint32_t>(main->base + k + 4);
                    if ((s0 & 0x9f00001fu) == 0x90000008u && (s1 & 0xffc003ffu) == 0xf9400108u) {
                        const int32_t slo = int32_t((s0 >> 29) & 3u);
                        const int32_t shi = int32_t((s0 >> 5) & 0x7ffffu);
                        int64_t simm = (int64_t(shi) << 2) | slo;
                        if (simm & (1ll << 20)) simm -= (1ll << 21);
                        const uint64_t mgr_off = ((k & ~0xfffull) + uint64_t(simm << 12)) + (uint64_t((s1 >> 10) & 0xfffu) << 3);
                        g_stage_mgr_off.store(mgr_off, std::memory_order_relaxed);
                        break;
                    }
                }
                if (g_stage_mgr_off.load(std::memory_order_relaxed)) break;
            }
        }
    }
    log("speedrun: fade_mgr=main+%llx gamedata_mgr=main+%llx course_mgr=main+%llx stage_mgr=main+%llx",
        (unsigned long long)g_fade_mgr_off.load(std::memory_order_relaxed),
        (unsigned long long)g_gamedata_mgr_off.load(std::memory_order_relaxed),
        (unsigned long long)g_course_mgr_off.load(std::memory_order_relaxed),
        (unsigned long long)g_stage_mgr_off.load(std::memory_order_relaxed));
}
void game::renderer_start() {}
void game::first_frame() {}

void game::frame_presented(uint64_t now_ns, int present_interval) {
    if (plat::setting("wonder_widescreen_trace") == "1") {
        static uint64_t next_trace = 0;
        if (now_ns >= next_trace) {
            next_trace = now_ns + 10'000'000'000ull;
            log("widescreen calls: main+1c34328 last 10s = %llu",
                (unsigned long long)g_wide_calls.exchange(0, std::memory_order_relaxed));
            const ModuleInfo* module = game_lib().main;
            constexpr uint64_t offsets[] = {0x362db38, 0x362dbb0, 0x362dc00, 0x362dc50};
            if (module) for (uint64_t offset : offsets) {
                if (offset + 8 > module->bss + module->bss_size) continue;
                float pair[2];
                std::memcpy(pair, reinterpret_cast<const void*>(module->base + offset), sizeof pair);
                log("widescreen live: main+%llx = %.3f x %.3f",
                    (unsigned long long)offset, pair[0], pair[1]);
            }
        }
    }
    // recomp/wonder_realtime.py reserves one aligned page at the end of main's BSS.
    // Two adjacent floats are consumed by the patched game code: seconds * 60 and seconds.
    static const bool realtime = [] {
        const char* value = std::getenv("NX_REALTIME_FRAME_STEP");
        return !(value && value[0] == '0');
    }();
    static uint64_t previous_ns = 0;
    static double samples[5]{};
    static unsigned sample_count = 0, sample_next = 0;
    static double prev_seconds = 1.0 / 60.0;
    static uint64_t report_ns = 0, report_frames = 0;
    static double report_seconds = 0, report_game_seconds = 0;
    static double speed_elapsed = 0, speed_simulated = 0;

    if (!frame_step_page()) return;
    update_wide_aspect();

    if (!previous_ns) {
        previous_ns = now_ns;
        report_ns = now_ns;
        return;
    }
    double dt = double(now_ns - previous_ns) * 1e-9;
    previous_ns = now_ns;
    if (dt < 0.001 || dt > 0.25) {
        sample_count = sample_next = 0;
        return;
    }

    double seconds = double(std::max(1, present_interval)) / 60.0;
    wonder::seed_guard_tick(now_ns);
    if (realtime && nvn::held_frame_cap() == 60) {
        // A moment the game needs at its own rate (seed_guard.cpp): presented at 60, a 60 Hz step each frame; the
        // average starts again when the hold ends.
        seconds = 1.0 / 60.0;
        prev_seconds = seconds;
        sample_count = sample_next = 0;
    } else if (realtime) {
        const int cap = std::max(30, nvn::frame_cap());
        const double target_dt = 1.0 / double(cap);
        if (sample_count == 0) prev_seconds = target_dt;
        const double max_step = std::max(target_dt * 1.20, prev_seconds * 1.20);
        const double min_step = std::min(target_dt * 0.85, prev_seconds * 0.85);
        const double filtered_dt = std::clamp(dt, min_step, max_step);
        samples[sample_next++ % 5] = filtered_dt;
        if (sample_count < 5) ++sample_count;
        seconds = 0;
        for (unsigned i = 0; i < sample_count; ++i) seconds += samples[i];
        seconds /= sample_count;
        if (std::fabs(seconds - target_dt) <= target_dt * 0.06) seconds = target_dt;
        seconds = std::min(seconds, 1.0 / 15.0);  // spread brief hitches; avoid an enormous one-frame simulation step
        prev_seconds = seconds;
    }

    store_frame_step(float(seconds));
    // Detect exiting to World Map or entering/re-entering a cached course even when file_opened does not fire.
    {
        static int32_t prev_pcount = 0;
        static int prev_wmap = -1;
        const int32_t pcount = course_player_count();
        const bool wmap = is_world_map_stage();
        if (prev_wmap != int(wmap)) {
            log("speedrun: stage wmap=%d pcount=%d session=%d", int(wmap), pcount,
                int(g_in_course_session.load(std::memory_order_relaxed)));
            prev_wmap = int(wmap);
        }
        if (g_in_course_session.load(std::memory_order_relaxed)) {
            if (wmap || (prev_pcount > 0 && pcount == 0)) {
                g_in_course_session.store(false, std::memory_order_relaxed);
                g_in_house_session.store(false, std::memory_order_relaxed);
                g_player_died.store(false, std::memory_order_relaxed);
                g_stage_load_ended.store(false, std::memory_order_relaxed);
                g_course_demo_active.store(false, std::memory_order_relaxed);
                g_stage_ready_ns.store(0, std::memory_order_relaxed);
                overlay::speedrun_set_course(false);
                log("speedrun: exited course (wmap=%d pcount=%d)", int(wmap), pcount);
            }
        } else if (wmap || (prev_pcount > 0 && pcount == 0)) {
            g_in_house_session.store(false, std::memory_order_relaxed);
            if (g_house_cap_held.exchange(false, std::memory_order_relaxed)) {
                nvn::hold_scene_frame_cap(0);
                log("wonder: house scene left for world map: selected frame cap restored");
            }
        } else if (!wmap && pcount > 0 && !g_in_house_session.load(std::memory_order_relaxed) &&
                   g_stage_mgr_off.load(std::memory_order_relaxed) != 0) {
            g_in_course_session.store(true, std::memory_order_relaxed);
            g_player_died.store(false, std::memory_order_relaxed);
            g_stage_load_ended.store(false, std::memory_order_relaxed);
            g_course_demo_active.store(false, std::memory_order_relaxed);
            g_stage_ready_ns.store(0, std::memory_order_relaxed);
            overlay::speedrun_set_course(true);
            log("speedrun: armed course (re-entry/cached)");
        }
        prev_pcount = pcount;
    }
    if (g_in_course_session.load(std::memory_order_relaxed)) {
        const uint64_t off = g_course_mgr_off.load(std::memory_order_relaxed);
        const ModuleInfo* main = game_lib().main;
        const uint64_t mgr = (off && main) ? ld<uint64_t>(main->base + off) : 0;
        const uint64_t pinfo = mgr ? ld<uint64_t>(mgr + 0x40) : 0;
        if (pinfo) {
            static int32_t prev_flag = -1;
            const int32_t count = int32_t(ld<uint32_t>(pinfo + 0x7e4));
            const int32_t flag0 = count > 0 ? int32_t(ld<uint8_t>(pinfo + 0x7ca)) : -1;
            if (flag0 != prev_flag) {
                log("speedrun: pinfo count=%d flag0=%d armed=%d", count, flag0, int(overlay::speedrun_is_armed()));
                prev_flag = flag0;
            }
        }
        if (!overlay::speedrun_is_armed() && is_all_players_dead()) {
            if (!g_player_died.exchange(true, std::memory_order_relaxed)) {
                g_stage_load_ended.store(false, std::memory_order_relaxed);
                g_course_demo_active.store(false, std::memory_order_relaxed);
                g_stage_ready_ns.store(0, std::memory_order_relaxed);
                overlay::speedrun_reset_course();
                log("speedrun: all players dead, timer reset and armed");
            }
        }
    }
    static bool saw_fade_out = false;
    static uint64_t armed_since_ns = 0;
    static int prev_faded = -1;
    if (overlay::speedrun_is_armed()) {
        if (!armed_since_ns) armed_since_ns = now_ns;
        const bool faded_in = is_faded_in();
        if (prev_faded != int(faded_in)) {
            prev_faded = int(faded_in);
            log("speedrun: armed fade=%d load_ended=%d demo=%d", int(faded_in),
                int(g_stage_load_ended.load(std::memory_order_relaxed)),
                int(g_course_demo_active.load(std::memory_order_relaxed)));
        }
        if (!faded_in) {
            saw_fade_out = true;
            g_stage_ready_ns.store(0, std::memory_order_relaxed);
        } else if (!g_course_demo_active.load(std::memory_order_relaxed)) {
            const bool load_ended = g_stage_load_ended.load(std::memory_order_relaxed);
            const bool respawned = g_player_died.load(std::memory_order_relaxed) && saw_fade_out;
            if (load_ended || respawned) {
                uint64_t ready_ns = g_stage_ready_ns.load(std::memory_order_relaxed);
                if (!ready_ns) {
                    g_stage_ready_ns.store(now_ns, std::memory_order_relaxed);
                } else if (now_ns - ready_ns >= 350'000'000ull) {
                    saw_fade_out = false;
                    armed_since_ns = 0;
                    prev_faded = -1;
                    g_stage_load_ended.store(false, std::memory_order_relaxed);
                    g_player_died.store(false, std::memory_order_relaxed);
                    g_stage_ready_ns.store(0, std::memory_order_relaxed);
                    overlay::speedrun_begin_play();
                    log("speedrun: started (%s)", load_ended ? "EndLoadStage+fade" : "respawn+fade");
                }
            }
        }
        const uint64_t failsafe_ns = g_player_died.load(std::memory_order_relaxed) ? 1'500'000'000ull : 15'000'000'000ull;
        if (overlay::speedrun_is_armed() && now_ns - armed_since_ns >= failsafe_ns) {
            saw_fade_out = false;
            armed_since_ns = 0;
            prev_faded = -1;
            g_stage_load_ended.store(false, std::memory_order_relaxed);
            g_course_demo_active.store(false, std::memory_order_relaxed);
            const bool was_respawn = g_player_died.exchange(false, std::memory_order_relaxed);
            g_stage_ready_ns.store(0, std::memory_order_relaxed);
            overlay::speedrun_begin_play();
            log("speedrun: started (%s)", was_respawn ? "respawn" : "15s failsafe");
        }
    } else {
        saw_fade_out = false;
        armed_since_ns = 0;
        prev_faded = -1;
        g_stage_load_ended.store(false, std::memory_order_relaxed);
        g_course_demo_active.store(false, std::memory_order_relaxed);
        g_stage_ready_ns.store(0, std::memory_order_relaxed);
    }
    overlay::speedrun_tick(seconds, dt, is_game_paused());
    speed_elapsed += dt;
    speed_simulated += seconds;
    if (speed_elapsed >= 0.5) {
        plat::overlay_game_speed(speed_elapsed > 0 ? speed_simulated / speed_elapsed * 100.0 : 100.0);
        speed_elapsed = speed_simulated = 0;
    }

    ++report_frames;
    report_seconds += dt;
    report_game_seconds += seconds;
    if (now_ns - report_ns >= 30'000'000'000ull) {
        log("wonder: real-time frame step %s: %llu presents in %.1f s (%.1f fps), simulation step %.4f of elapsed time",
            realtime ? "enabled" : "disabled", (unsigned long long)report_frames,
            double(now_ns - report_ns) / 1e9, double(report_frames) / (double(now_ns - report_ns) / 1e9),
            report_seconds > 0 ? report_game_seconds / report_seconds : 0.0);
        report_ns = now_ns;
        report_frames = 0;
        report_seconds = report_game_seconds = 0;
    }
}

}  // namespace nx
