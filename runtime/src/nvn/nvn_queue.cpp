// NVN queues, command buffers, windows and syncs (docs/NVN.md, N3). Synchronously every command completes at
// submit: fences are signaled right away, the window hands out its textures in turn, and presents are counted. With
// NX_ASYNC_GPU a fence is the submit it follows, and waiting for it waits for that submit (nvn_frames.cpp). With
// NX_PIPELINED the GPU work is in flight when a submit returns but its compute results are in guest memory, so
// syncs and nvnQueueFinish have nothing to wait for, as synchronously (gpu_serial() is 0). Return
// values follow the SDK driver (nvnSyncWait sdk+45a960: 0 once the fence has passed; nvnWindowAcquireTexture
// sdk+45e050: 0 on success).
#include <algorithm>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <array>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <atomic>
#include <vector>

#include "nvn.h"
#include "platform.h"
#include "frametrace.h"
#include "play.h"
#include <nx/game.h>

namespace nx {
uint64_t vsync_wait(uint64_t target);  // hle_vi.cpp
}

namespace nx {
uint64_t vsync_wait(uint64_t target);  // hle_vi.cpp
}

namespace nx::nvn {

// Submits the game made and submits whose replay (and GPU work, write-backs) finished. The game records the next frame
// on one thread while another submits the last one, and our replay runs inside nvnQueueSubmitCommands: a fence the
// game waits on must not pass before the submits it follows were replayed, or the game rewrites the next frame's
// vertex and uniform data while this one is still read (the horizon band, exploded meshes, DEBUG.md #59).
// NX_SYNC_LOOSE=1: the old behaviour (fences passed at once).
namespace {
std::mutex g_sub_lock;
std::condition_variable g_sub_cv;
uint64_t g_sub_called = 0, g_sub_done = 0;
std::atomic<uint64_t> g_sub_blocked{0};

bool sync_loose() {
    static const bool on = std::getenv("NX_SYNC_LOOSE") != nullptr;
    return on;
}

void submit_done() {
    {
        std::lock_guard lk(g_sub_lock);
        g_sub_done++;
    }
    g_sub_cv.notify_all();
}

// Where the game waits for the replay (docs/nvn/android-perf/PHASE3.md I1): calls, the ones that blocked and for how
// long, per 600 presents; frame_ns is this main-thread frame's blocked time, for the hitch lines (frame_time_probe).
struct WaitSite {
    const char* name;
    std::atomic<uint64_t> calls{0}, blocked{0}, ns{0}, frame_ns{0};
};
WaitSite g_wait_sites[kWaitSites] = {{"sync wait"}, {"queue finish"}, {"pool invalidate"}, {"pool finalize"}};

// Waits until the submits up to `seq` were replayed (2 s at most: a hung replay is logged, not waited forever).
void wait_submits(uint64_t seq, int site) {
    if (sync_loose() || !seq) return;
    WaitSite& w = g_wait_sites[site];
    w.calls++;
    std::unique_lock lk(g_sub_lock);
    if (g_sub_done >= seq) return;
    uint64_t n = ++g_sub_blocked;
    if (n == 1 || n % 1000 == 0) log("nvn: %llu CPU waits blocked on a submit still being replayed", (unsigned long long)n);
    uint64_t t0 = plat::monotonic_ns();
    if (!g_sub_cv.wait_for(lk, std::chrono::seconds(2), [seq] { return g_sub_done >= seq; }))
        log("nvn: a CPU wait gave up after 2 s on submit %llu (%llu done)", (unsigned long long)seq,
            (unsigned long long)g_sub_done);
    uint64_t dt = plat::monotonic_ns() - t0;
    w.blocked++, w.ns += dt, w.frame_ns += dt;
}

void log_wait_sites() {
    std::string line;
    for (WaitSite& w : g_wait_sites) {
        char b[128];
        std::snprintf(b, sizeof b, "; %s %llu calls, %llu blocked %.1f ms", w.name, (unsigned long long)w.calls.exchange(0),
                      (unsigned long long)w.blocked.exchange(0), double(w.ns.exchange(0)) / 1e6);
        line += b;
    }
    log("nvn: game waits on the replay over 600 presents%s", line.c_str());
}

uint64_t submits_called() {
    std::lock_guard lk(g_sub_lock);
    return g_sub_called;
}
}  // namespace

// MemoryPoolInvalidateMappedRange / Finalize (nvn_objects.cpp): the CPU reads or frees what submits made so far use.
void wait_submits_so_far(int site) { wait_submits(submits_called(), site); }

namespace {

// ---- syncs ------------------------------------------------------------------------------------------------------

struct Sync : Object {
    std::atomic<bool> signaled{false};
    std::atomic<uint64_t> serial{0};  // NX_ASYNC_GPU: the submit it waits for (0: none; always 0 otherwise)
    std::atomic<uint64_t> job{0};     // the replay-thread job that fences it (0: none)
    std::atomic<uint64_t> seq{0};     // the submits it follows (wait_submits); 0: none
};

void SyncInitialize(CPU& c) {  // NVNboolean (NVNsync*, NVNdevice*)
    make<Sync>(c.x[0]);
    c.x[0] = 1;
}

void SyncFinalize(CPU& c) { destroy(c.x[0]); }

void signal_sync(uint64_t guest) {
    if (auto* s = get<Sync>(guest)) s->serial = 0, s->job = 0, s->seq = 0, s->signaled = true;
}

// NVNsyncWaitResult (const NVNsync*, u64 timeout). Always 0 (already signaled), as before: a fenced submit is waited
// for whatever the timeout, 2 s at most (then the GPU hung and the process ends). Correct results before latency.
void SyncWait(CPU& c) {
    auto* s = get<Sync>(c.x[0]);
    if (s) wait_submits(s->seq.load(), kWaitSync);
    if (s) worker_wait(s->job);
    if (uint64_t serial = s ? s->serial.load() : 0) gpu_wait(serial);
    c.x[0] = 0;
}

// ---- queues -----------------------------------------------------------------------------------------------------

std::atomic<uint64_t> g_presents{0};

void QueueInitialize(CPU& c) { c.x[0] = 1; }  // NVNboolean (NVNqueue*, const NVNqueueBuilder*)
void QueueFinalize(CPU&) {}
void QueueSubmitCommands(CPU& c) {  // (NVNqueue*, int count, const NVNcommandHandle*)
    plat::perf_hint_thread();  // this thread replays: part of every frame (perf_hint.cpp)
    std::vector<std::shared_ptr<CmdList>> lists;
    static uint64_t submitted = 0, missing = 0;
    for (uint32_t k = 0; k < uint32_t(c.x[1]); k++) {
        submitted++;
        if (auto l = command_list(ld<uint64_t>(c.x[2] + 8 * k)))
            lists.push_back(std::move(l));
        else if (++missing <= 5)
            log("nvn: submitted handle %llx is unknown", (unsigned long long)ld<uint64_t>(c.x[2] + 8 * k));
    }
    static const bool trace_lists = std::getenv("NX_NVN_LISTS") != nullptr;
    if (trace_lists && g_presents > 900 && g_presents < 905)
        for (uint32_t k = 0; k < uint32_t(c.x[1]); k++) {
            auto l = command_list(ld<uint64_t>(c.x[2] + 8 * k));
            log("nvn list: submit %llx handle %llu (%zu cmds)", (unsigned long long)c.x[0],
                (unsigned long long)ld<uint64_t>(c.x[2] + 8 * k), l ? l->cmds.size() : 0);
        }
    if ((submitted & 1023) == 0) log("nvn: %llu handles submitted, %llu unknown", (unsigned long long)submitted, (unsigned long long)missing);
    // NX_DEFERRED_SNAPSHOT=1: read the Deferred arrays here; by default they are read when the submit is replayed.
    static const bool snap = std::getenv("NX_DEFERRED_SNAPSHOT") != nullptr;
    auto deferred = snap ? std::make_shared<const DeferredValues>(deferred_snapshot(lists)) : nullptr;
    {
        std::lock_guard lk(g_sub_lock);
        g_sub_called++;
    }
    worker_run([lists = std::move(lists), deferred] {
        gpu_submit(lists, deferred);
        submit_done();
    });
}
void QueueFlush(CPU&) {}
void QueueFinish(CPU&) {
    wait_submits(submits_called(), kWaitFinish);
    worker_finish();
    gpu_wait(gpu_serial());
}
// (NVNqueue*, NVNsync*, NVNsyncCondition, int flags): every condition taken as "all previous work complete".
void QueueFenceSync(CPU& c) {
    uint64_t guest = c.x[1];  // the fence goes after the work queued so far: its serial is taken on the replay thread
    if (auto* s = get<Sync>(guest)) {
        s->signaled = true;
        s->seq = submits_called();
        s->job = worker_run([guest] {
            if (auto* s = get<Sync>(guest)) s->serial = gpu_serial();
        });
    }
}
void QueueWaitSync(CPU& c) { c.x[0] = 1; }  // NVNboolean (NVNqueue*, const NVNsync*): one queue, in order

// ---- windows ----------------------------------------------------------------------------------------------------

void WindowInitialize(CPU& c) {  // NVNboolean (NVNwindow*, const NVNwindowBuilder*)
    uint64_t guest = c.x[0], builder = c.x[1];
    auto& w = make<Window>(guest);
    int n = int(driver(c, "nvnWindowBuilderGetNumTextures", {builder}));
    for (int k = 0; k < n; k++) w.textures.push_back(driver(c, "nvnWindowBuilderGetTexture", {builder, uint64_t(k)}));
    w.active = int(driver(c, "nvnWindowBuilderGetNumActiveTextures", {builder}));
    if (w.active <= 0 || w.active > n) w.active = n;
    w.interval = int(driver(c, "nvnWindowBuilderGetPresentInterval", {builder}));
    log("nvn: window %llx: %d textures (%d active), present interval %d", (unsigned long long)guest, n, w.active, w.interval);
    c.x[0] = 1;
}

void WindowFinalize(CPU& c) { destroy(c.x[0]); }

// NVNwindowAcquireTextureResult (NVNwindow*, NVNsync* textureAvailableSync, int* textureIndex)
void WindowAcquireTexture(CPU& c) {
    auto* w = get<Window>(c.x[0]);
    int index = 0;
    if (w && w->active > 0) {
        index = w->next;
        w->next = (w->next + 1) % w->active;
    }
    if (c.x[2]) st<int32_t>(c.x[2], index);
    signal_sync(c.x[1]);
    c.x[0] = 0;
}

void WindowGetNumTextures(CPU& c) {
    auto* w = get<Window>(c.x[0]);
    c.x[0] = w ? w->textures.size() : 0;
}

void WindowGetNumActiveTextures(CPU& c) {
    auto* w = get<Window>(c.x[0]);
    c.x[0] = w ? uint64_t(w->active) : 0;
}

void WindowSetNumActiveTextures(CPU& c) {
    if (auto* w = get<Window>(c.x[0])) w->active = std::max(1, std::min(int(c.x[1]), int(w->textures.size())));
}

void WindowGetPresentInterval(CPU& c) {
    auto* w = get<Window>(c.x[0]);
    c.x[0] = w ? uint64_t(w->interval) : 1;
}

// Our governor over the game's (NX_GOVERNOR; on by default on Android unless NX_MAX_INTERVAL is set). The game's own
// rule (below) locks 30 fps after 3 frames over 15.83 ms; at 30 fps a phone lowers its clocks, every frame then takes
// longer, and it never leaves 30 (a Galaxy S23 sat at 30 fps in course with frames that run at 60 when allowed).
// This one holds 60 (the game's maximum clamped to 1) while it is kept: it watches the main thread's loop period over
// 120-frame windows, and when two windows in a row had over 20% of their frames past 20 ms (the game would run
// visibly slow; one window is a scene load), it hands the choice back to the game (30 fps at the right speed) for a
// while, then tries 60 again, skipping the first 90 frames of a try (the clocks ramp up from the 30-fps level). The
// wait doubles after each failed try (10 s, 20 s, ... 80 s) and resets once a try holds. Returns 1 (clamp) or 0.
uint32_t governor_max(uint64_t now) {
    static const bool on = [] {
        const char* e = std::getenv("NX_GOVERNOR");
#if defined(__ANDROID__)
        return !(e && e[0] == '0');
#else
        return e && e[0] == '1';
#endif
    }();
    if (!on) return 0;
    static uint64_t last = 0, frames = 0, slow = 0, game_until = 0, warm_until = 0, backoff = 600, n = 0;
    static int bad_windows = 0, held_windows = 0;
    uint64_t period = last ? now - last : 0;
    last = now;
    n++;
    if (n < game_until) return 0;
    if (n == game_until) frames = slow = 0, bad_windows = 0, warm_until = n + 90;
    if (n < warm_until) return 1;
    frames++, slow += period > 20'000'000ull;
    if (frames < 120) return 1;
    bool held = slow * 5 <= frames;
    if (!held || held_windows++ % 30 == 0)
        log("nvn: governor: %llu of %llu frames over 20 ms%s", (unsigned long long)slow, (unsigned long long)frames,
            held ? ": 60 fps holds" : bad_windows ? ": back to the game's choice" : " (once: still 60)");
    frames = slow = 0;
    if (held) {
        bad_windows = 0, backoff = 600;
        return 1;
    }
    held_windows = 0;
    if (++bad_windows < 2) return 1;
    game_until = n + backoff;
    backoff = std::min<uint64_t>(backoff * 2, 4800);
    return 0;
}

// Wonder's sead::GameFramework::procFrame_ override (main+e15ec) picks the interval every frame from the main
// thread's last frame time, fw[0x118] (GetSystemTick ticks from the end of the previous present wait to the end of
// this frame's calc and draw recording): 2 when it reaches 15.83 ms, locked after 3 slow frames, back to 1 after 15
// fast ones (docs/nvn/DEBUG.md #43). From that call site (lr main+e16ac, x19 = the framework): a histogram of that
// time every 600 frames, and NX_MAX_INTERVAL=1 clamps the game's own maximum to 1 (fields 0x24c/0x264/0x268).
std::atomic<int> g_frame_cap{60};  // set_frame_cap: 60, or 30
std::atomic<int> g_cap_hold{0};    // hold_frame_cap: a lower cap for a moment the game needs at its own rate, 0: none
int frame_cap_setting() {
    static const int forced = [] {
        const char* e = std::getenv("NX_FPS_CAP");
        if (!e) return 0;
        const int cap = std::atoi(e);
        return cap == 30 || cap == 60 || cap == 90 || cap == 120 || cap == 144 ? cap : 60;
    }();
    // NX_FPS_CAP_UNTIL=<frame> (tests): 30 until that frame, then the setting, as a player switching back in the menu.
    static const uint64_t until = [] {
        const char* e = std::getenv("NX_FPS_CAP_UNTIL");
        return e ? uint64_t(std::strtoull(e, nullptr, 10)) : uint64_t(0);
    }();
    if (until && g_frame.load(std::memory_order_relaxed) < until) return 30;
    return forced ? forced : g_frame_cap.load(std::memory_order_relaxed);
}
int frame_cap() {
    const int cap = frame_cap_setting(), hold = g_cap_hold.load(std::memory_order_relaxed);
    return hold && hold < cap ? hold : cap;
}

// The 30 fps cap: the game's own interval set to 2 (fw[0x264], fw[0x268] and the value it presents with), the mode
// its governor picks under load. The game then steps its logic two vsyncs per frame and keeps full speed; waiting
// longer between presents alone would halve its speed (its step follows the interval it chose, not wall time).
void frame_time_probe(CPU& c) {
    const LoadedModule* m = module_at(c.x[30]);
    if (!m || c.x[30] - m->base != 0xe16ac || !c.x[19]) return;
    plat::perf_hint_thread();  // the game's main thread makes the frame too (ADPF, PLAN.md R2)
    plat::frame_thread_tick(1);
    static const char* max_env = std::getenv("NX_MAX_INTERVAL");
    static uint32_t game_max = 0;  // the game's own maximum (fw[0x24c]), restored when the governor lets go
    if (!game_max) game_max = ld<uint32_t>(c.x[19] + 0x24c);
    uint32_t max = max_env && std::atoi(max_env) >= 1 ? uint32_t(std::atoi(max_env)) : governor_max(plat::monotonic_ns());
    if (!max && !max_env && game_max) st<uint32_t>(c.x[19] + 0x24c, game_max);
    if (max >= 1) {
        st<uint32_t>(c.x[19] + 0x24c, max);
        if (ld<uint32_t>(c.x[19] + 0x268) > max) st<uint32_t>(c.x[19] + 0x268, max);
        if (ld<uint32_t>(c.x[19] + 0x264) > max) st<uint32_t>(c.x[19] + 0x264, max);
        if (int(c.x[1]) > int(max)) c.x[1] = max;
    }
    static int applied_cap = 60;
    const int cap = frame_cap();
    if (cap == 30) {
        if (ld<uint32_t>(c.x[19] + 0x24c) < 2) st<uint32_t>(c.x[19] + 0x24c, 2);
        st<uint32_t>(c.x[19] + 0x264, 2);
        st<uint32_t>(c.x[19] + 0x268, 2);
        if (int(c.x[1]) < 2) c.x[1] = 2;
    } else if (applied_cap == 30) {  // back to 60: release the game's interval (else it stays locked at 2)
        st<uint32_t>(c.x[19] + 0x264, 1);
        st<uint32_t>(c.x[19] + 0x268, 1);
        if (game_max) st<uint32_t>(c.x[19] + 0x24c, game_max);
        c.x[1] = 1;
        log("nvn: frame cap back to 60: the game's interval released");
    }
    applied_cap = cap;
    static uint64_t bins[5] = {}, frames = 0;
    double ms = double(ld<uint64_t>(c.x[19] + 0x118)) / 19200.0;
    // A main-thread frame over 33 ms: what it waited on (PHASE3.md I1), its CPU time, faults and switches since the
    // last call, and how much of it a memory-guard read covered (PHASE4.md H0). NX_HITCH_LINES lines at most (300).
    static int hitches = 0;
    static const int max_hitches = std::getenv("NX_HITCH_LINES") ? std::atoi(std::getenv("NX_HITCH_LINES")) : 300;
    static uint64_t cpu_prev = 0;
    static long faults_prev[2] = {}, switches_prev[2] = {};
    uint64_t cpu = 0, now = plat::monotonic_ns();
    long faults[2], switches[2];
    plat::thread_counters(cpu, faults, switches);
    if (ms >= 33 && hitches < max_hitches && cpu_prev) {
        hitches++;
        uint64_t span = uint64_t(ms * 1e6);
        log("hitch: frame %llu, main thread %.1f ms (cpu %.1f), blocked: sync %.1f, finish %.1f, invalidate %.1f, "
            "finalize %.1f ms; faults %ld/%ld, switches %ld/%ld; guard read %.1f ms",
            (unsigned long long)g_frame.load(), ms, double(cpu - cpu_prev) / 1e6,
            double(g_wait_sites[0].frame_ns) / 1e6, double(g_wait_sites[1].frame_ns) / 1e6,
            double(g_wait_sites[2].frame_ns) / 1e6, double(g_wait_sites[3].frame_ns) / 1e6, faults[0] - faults_prev[0],
            faults[1] - faults_prev[1], switches[0] - switches_prev[0], switches[1] - switches_prev[1],
            double(plat::memguard_overlap_ns(now > span ? now - span : 0, now)) / 1e6);
    }
    cpu_prev = cpu;
    for (int k = 0; k < 2; k++) faults_prev[k] = faults[k], switches_prev[k] = switches[k];
    for (WaitSite& w : g_wait_sites) w.frame_ns = 0;
    bins[ms < 12 ? 0 : ms < 15.83 ? 1 : ms < 20 ? 2 : ms < 33 ? 3 : 4]++;
    if (++frames % 600 == 0) {
        log("nvn: main-thread frame time over 600 frames: <12 ms %llu, <15.8 %llu, <20 %llu, <33 %llu, more %llu",
            (unsigned long long)bins[0], (unsigned long long)bins[1], (unsigned long long)bins[2],
            (unsigned long long)bins[3], (unsigned long long)bins[4]);
        for (auto& b : bins) b = 0;
    }
}

void WindowSetPresentInterval(CPU& c) {
    frame_time_probe(c);
    if (auto* w = get<Window>(c.x[0])) {
        static int logged = 0;
        if (w->interval != int(c.x[1]) && logged++ < 50)
            log("nvn: window present interval %d -> %d (frame %llu)", w->interval, int(c.x[1]), (unsigned long long)g_frame.load());
        w->interval = int(c.x[1]);
    }
}

// (NVNqueue*, NVNwindow*, int textureIndex)
// Frame pacing: a present returns no earlier than the window's present interval in vsyncs after the previous one.
// On the Switch the texture-available sync of the next acquire does this; ours is signaled at once, so without it
// the game runs as fast as it can once it is faster than 60 fps. The vsyncs are the vi ticks (hle_vi.cpp: the
// display's blank where it can be), so frames reach the display on its refresh, not on a clock drifting against it.
// A late frame does not wait for the next vsync. NX_NO_FRAME_LIMIT=1, or fast-forward held: unpaced when the selected
// cap is 30/60. The explicit 90/120/144 caps use a monotonic host deadline instead of the 60 Hz VI timer.
void pace_present(int interval) {
    static const bool off = std::getenv("NX_NO_FRAME_LIMIT") != nullptr;
    static const int env_fps = [] {
        const char* value = std::getenv("NX_FPS_LIMIT");
        if (!value || !*value) return 0;
        const int fps = std::atoi(value);
        return fps == 90 || fps == 120 || fps == 144 ? fps : 0;
    }();
    static uint64_t custom_next = 0;
    const int custom_fps = env_fps ? env_fps : frame_cap();
    if (custom_fps > 60 && !plat::fast_forward()) {
        // An explicit cap takes precedence over the raw unpaced diagnostic switch. Reset the deadline after a
        // significant hitch instead of trying to "catch up" with a burst of simulation frames.
        const uint64_t period = 1'000'000'000ull / uint64_t(custom_fps);
        const uint64_t now = plat::monotonic_ns();
        if (!custom_next || now > custom_next + period * 2) custom_next = now + period;
        else custom_next += period;
        if (custom_next > now) plat::sleep_ns(int64_t(custom_next - now));
        return;
    }
    static uint64_t next = 0;
    if (off || interval <= 0 || plat::fast_forward()) return;
    uint64_t now = vsync_wait(next);
    static uint64_t presents = 0, at2 = 0;  // how often the game asks for 30 fps (interval 2), per 600 presents
    // Pacing: presents more than a vsync later than asked (a frame shown late or twice), and the longest gap.
    static uint64_t last_ns = 0, late = 0, worst = 0;
    const uint64_t t = plat::monotonic_ns();
    if (last_ns) {
        worst = std::max(worst, t - last_ns);
        late += t - last_ns > uint64_t(interval + 1) * 16'666'667ull;
    }
    last_ns = t;
    at2 += interval >= 2;
    if (++presents % 600 == 0) {
        log("nvn: of the last 600 presents %llu at interval >= 2, %llu late, longest gap %.1f ms",
            (unsigned long long)at2, (unsigned long long)late, double(worst) / 1e6);
        at2 = late = worst = 0;
        log_wait_sites();
    }
    next = now + uint64_t(interval);
}

void QueuePresentTexture(CPU& c) {
    // The frame's work, from the end of the last present's pacing to now (perf_hint.cpp: ADPF on Android).
    static uint64_t work_from = 0;
    uint64_t t_in = plat::monotonic_ns();
    plat::perf_hint_thread();
    if (work_from) plat::perf_hint_frame(int64_t(t_in - work_from));
    if (auto* w = get<Window>(c.x[1])) {
        plat::overlay_game_interval(w->interval);
        // At most two presents queued: the game runs at most two frames ahead of the replay.
        static uint64_t queued[2] = {};
        static int next_slot = 0;
        const uint64_t t_wait = plat::monotonic_ns();
        worker_wait(queued[next_slot]);
        const uint64_t replay_wait_ns = plat::monotonic_ns() - t_wait;
        if (int(c.x[2]) >= 0 && size_t(c.x[2]) < w->textures.size()) {
            uint64_t texture = w->textures[size_t(c.x[2])];
            std::array<int32_t, 4> crop{w->crop[0], w->crop[1], w->crop[2], w->crop[3]};
            queued[next_slot] = worker_run([texture, crop] { gpu_present(texture, crop.data()); });
            next_slot ^= 1;
        }
        const uint64_t t_pace = plat::monotonic_ns();
        pace_present(w->interval);
        ft::guest_present(replay_wait_ns, plat::monotonic_ns() - t_pace, w->interval);  // frametrace.h
    }
    work_from = plat::monotonic_ns();
    uint64_t n = ++g_presents;
    if (n == 1 || (n & (n - 1)) == 0) log("nvn: present #%llu (texture %d)", (unsigned long long)n, int(c.x[2]));
    game::frame_presented(plat::monotonic_ns(), [&] {
        if (auto* w = get<Window>(c.x[1])) return w->interval;
        return 1;
    }());
    g_frame++;
    play::frame();  // probe, anchors, scanner steps (play.cpp)
}

// (NVNwindow*, int x, int y, int width, int height): the part of the window textures shown. In handheld mode
// (NX_HANDHELD) the game renders 1280x720 into its 1920x1080 window textures and crops to it.
void WindowSetCrop(CPU& c) {
    if (auto* w = get<Window>(c.x[0])) {
        int32_t v[4] = {int32_t(c.x[1]), int32_t(c.x[2]), int32_t(c.x[3]), int32_t(c.x[4])};
        if (std::memcmp(v, w->crop, sizeof v) != 0) {
            static int logged = 0;
            if (logged++ < 20) log("nvn: window crop %d,%d %dx%d", v[0], v[1], v[2], v[3]);
            std::memcpy(w->crop, v, sizeof v);
        }
    }
}

}  // namespace

NX_HLE("nvnWindowSetCrop", WindowSetCrop);
NX_HLE("nvnSyncInitialize", SyncInitialize);
NX_HLE("nvnSyncFinalize", SyncFinalize);
NX_HLE("nvnSyncWait", SyncWait);
NX_HLE("nvnQueueInitialize", QueueInitialize);
NX_HLE("nvnQueueFinalize", QueueFinalize);
NX_HLE("nvnQueueSubmitCommands", QueueSubmitCommands);
NX_HLE("nvnQueueFlush", QueueFlush);
NX_HLE("nvnQueueFinish", QueueFinish);
NX_HLE("nvnQueueFenceSync", QueueFenceSync);
NX_HLE("nvnQueueWaitSync", QueueWaitSync);
NX_HLE("nvnQueuePresentTexture", QueuePresentTexture);
NX_HLE("nvnWindowInitialize", WindowInitialize);
NX_HLE("nvnWindowFinalize", WindowFinalize);
NX_HLE("nvnWindowAcquireTexture", WindowAcquireTexture);
NX_HLE("nvnWindowGetNumTextures", WindowGetNumTextures);
NX_HLE("nvnWindowGetNumActiveTextures", WindowGetNumActiveTextures);
NX_HLE("nvnWindowSetNumActiveTextures", WindowSetNumActiveTextures);
NX_HLE("nvnWindowGetPresentInterval", WindowGetPresentInterval);
NX_HLE("nvnWindowSetPresentInterval", WindowSetPresentInterval);

// A game asks for its own rate for a moment (games/wonder/seed_guard.cpp): the cap drops to fps until released
// (0), the setting is untouched. held_frame_cap() is the hold in force (0: none).
void hold_frame_cap(int fps) {
    g_cap_hold.store(fps, std::memory_order_relaxed);
}
int held_frame_cap() {
    const int hold = g_cap_hold.load(std::memory_order_relaxed);
    return hold && hold < frame_cap_setting() ? hold : 0;
}

void set_frame_cap(int fps) {
    const int cap = fps == 30 || fps == 60 || fps == 90 || fps == 120 || fps == 144 ? fps : 60;
    g_frame_cap.store(cap, std::memory_order_relaxed);
}

}  // namespace nx::nvn
