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

#include "platform.h"

namespace nx {

const game::Info game::kInfo = {"wonder", "Super Mario Bros. Wonder", 0x010015100B514000ull,
                                "io.github.belmantegu.wonderrecomp", "private/wonder",
                                0x1000000 /* main.npdm: 16 MiB system resource */};

namespace {

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

static void init_frame_step() {
    if (!frame_step_page()) {
        log("wonder: real-time frame step page was not reserved; retaining the game's original frame timing");
        return;
    }
    store_frame_step(1.0f / 60.0f);
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
int held_frame_cap();
}

namespace wonder {
void menu_init();
void seed_guard_init();
void seed_guard_tick(uint64_t now_ns);
}

void game::after_init() {
    init_frame_step();
    trace_screen_globals();
    wonder::menu_init();
    wonder::seed_guard_init();
}
void game::renderer_start() {}
void game::first_frame() {}

void game::frame_presented(uint64_t now_ns, int present_interval) {
    // recomp/wonder_realtime.py reserves one aligned page at the end of main's BSS.
    // Two adjacent floats are consumed by the patched game code: seconds * 60 and seconds.
    static const bool realtime = [] {
        const char* value = std::getenv("NX_REALTIME_FRAME_STEP");
        return !(value && value[0] == '0');
    }();
    static uint64_t previous_ns = 0;
    static double samples[5]{};
    static unsigned sample_count = 0, sample_next = 0;
    static uint64_t report_ns = 0, report_frames = 0;
    static double report_seconds = 0, report_game_seconds = 0;
    static double speed_elapsed = 0, speed_simulated = 0;

    if (!frame_step_page()) return;

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
        sample_count = sample_next = 0;
    } else if (realtime) {
        samples[sample_next++ % 5] = dt;
        if (sample_count < 5) ++sample_count;
        seconds = 0;
        for (unsigned i = 0; i < sample_count; ++i) seconds += samples[i];
        seconds /= sample_count;
        seconds = std::min(seconds, 1.0 / 15.0);  // spread brief hitches; avoid an enormous one-frame simulation step
    }

    store_frame_step(float(seconds));
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
