// What the runtime needs from the host OS. platform_win.cpp, platform_android.cpp, platform_mac.cpp and
// platform_linux.cpp implement it.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace nx::plat {

// ---- memory: fixed-address reservations (guest pointers are host pointers) ----
bool reserve(uint64_t addr, uint64_t size);  // exactly at addr, no access
bool commit(uint64_t addr, uint64_t size);   // read/write
void decommit(uint64_t addr, uint64_t size);

// ---- threads and time ----
using ThreadFn = void (*)(void*);
void start_thread(ThreadFn fn, void* arg, size_t stack_size);
[[noreturn]] void exit_thread();
[[noreturn]] void exit_process(int code);
void sleep_ns(int64_t ns);
// Waits out the display's next 1/60 s of vertical blanks (2 on a 120 Hz display) and returns true; false where the
// blank is not available or the refresh is no multiple of 60 Hz (the caller then times 60 Hz itself).
bool wait_vblank();
void yield();
void set_thread_name(const char* name);
int64_t current_thread_id();  // the host OS thread id
// Samples the given threads (thread CPU time, 1 kHz) for `seconds`: host pc, a few host frames and the guest lr
// (read through guest_lr[k], the thread's x30). Writes one line per sample to `path`:
//   <tid> <guest lr> <module>:<offset> [<module>:<offset> ...]   (host pc first, then callers)
// False where not implemented.
bool profile(const int64_t* tids, const uint64_t* const* guest_lr, size_t count, int seconds, const std::string& path);  // the host thread's name (debuggers, profilers, top)
uint64_t monotonic_ns();

// ---- files ----
FILE* fopen_utf8(const std::string& path, const char* mode);
int64_t ftell64(FILE* f);
int fseek64(FILE* f, int64_t off, int origin);
// Positional read, safe with other threads reading the same FILE (no shared file position). Bytes read.
uint64_t read_at(FILE* f, int64_t off, void* buf, uint64_t size);
bool make_dirs(const std::string& path);

// ---- output ----
void log_line(const char* line);  // stderr / logcat
std::string default_data_root();
std::string data_root();
bool set_data_root(const std::string& path);
void reload_pc_settings();
std::string setting(const std::string& key);  // a player setting: config.txt (Android), NX_<KEY> (PC); "" if unset          // where private/ lives ("private" on PC, the app's external files dir on Android)

// ---- the launcher (launcher/launcher.cpp; Windows and Linux) ----
// The system's folder picker, titled `title`, starting in `start` (may be ""). Returns at once; `done` gets the folder
// chosen, or "" when the player cancelled, from another thread.
void pick_folder(const std::string& title, const std::string& start, std::function<void(std::string)> done);
void pick_image(const std::function<void(std::string)>& done);
void open_folder(const std::string& path);  // shows it in the system's file manager (an http(s) address: the browser)
std::vector<std::string> program_args();     // the command line, without the program's own path
#if defined(__ANDROID__)
const char* android_internal_data_dir();
const char* android_native_lib_dir();
bool ensure_android_kit();
#endif

// ---- display, input, audio ----
void* create_window();       // native window handle for the Vulkan surface (HWND / ANativeWindow*), kept alive
void release_window(void* w);  // done with a handle from create_window
// First-launch Vulkan shader preparation UI. Progress counts successfully created pipelines.
void shader_compile_progress(uint64_t compiled, uint64_t total, bool visible);
uint64_t window_generation();  // changes when the OS replaced the window (Android: app back from the background)
bool window_minimized();     // no surface to draw to (minimized / app in background)
void window_size(uint32_t& w, uint32_t& h);  // client size in pixels
void screen_size(uint32_t& w, uint32_t& h);  // the display's own size, landscape (Android; the window on PC)
// The render scale to use when the player sets none: 1 on fast GPUs, less on slower ones (Android).
float default_render_scale();

// NpadButton bits (Nintendo layout: A is the right face button, B the bottom one).
enum : uint64_t {
    kA = 1ull << 0, kB = 1ull << 1, kX = 1ull << 2, kY = 1ull << 3, kStickL = 1ull << 4, kStickR = 1ull << 5,
    kL = 1ull << 6, kR = 1ull << 7, kZL = 1ull << 8, kZR = 1ull << 9, kPlus = 1ull << 10, kMinus = 1ull << 11,
    kLeft = 1ull << 12, kUp = 1ull << 13, kRight = 1ull << 14, kDown = 1ull << 15,
    kStickLLeft = 1ull << 16, kStickLUp = 1ull << 17, kStickLRight = 1ull << 18, kStickLDown = 1ull << 19,
    kStickRLeft = 1ull << 20, kStickRUp = 1ull << 21, kStickRRight = 1ull << 22, kStickRDown = 1ull << 23,
};
constexpr int32_t kStickMax = 0x7FFF;

struct PadState {
    uint64_t buttons = 0;  // NpadButton bits
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;
};
void read_pad(PadState& p);  // player 1
#if (defined(__linux__) && !defined(__ANDROID__)) || defined(_WIN32)
std::array<std::string, 4> controller_names();
bool player_connected(unsigned player);
void read_pad_player(unsigned player, PadState& p);
void rumble_player(unsigned player, float low, float high);
// The game's HD rumble for that player: {amplitude low, frequency low, amplitude high, frequency high} (0..1, Hz) of
// its left and right actuator. Windows plays it on Switch 2 controllers (switch2/); elsewhere nothing (rumble_player).
void rumble_player_hd(unsigned player, const float left[4], const float right[4]);
struct ButtonPromptConfig {
    int style = 1;                  // 1 Nintendo, 2 Xbox, 3 PlayStation
    std::array<int, 16> bindings{}; // overlay::Control -> overlay::Physical
};
ButtonPromptConfig button_prompt_config();
bool upgrade_button_font(const std::string& out_path);
#endif
bool fast_forward();  // held: the game runs unpaced (Tab on PC; false where there is no key for it)
// Player 1's controller rumble: low (heavy) and high (light) motor, 0..1, scaled by the player's strength setting.
// Called on every vibration value the game sends; the platform keeps it running until the next.
void rumble(float low, float high);
// Touch controls to draw over the picture (touch_layout.cpp, platform_android_input.cpp): circles at x, y (0..1
// across the landscape screen), radius r (a fraction of its height), opacity alpha. 0 where nothing is drawn.
struct OverlayShape {
    float x, y, r, alpha;
};
size_t touch_overlay(OverlayShape* out, size_t max);
// Performance hints (perf_hint.cpp; Android ADPF, nothing elsewhere): the calling thread takes part in making frames;
// a frame's work took `ns`.
void perf_hint_thread();
void perf_hint_frame(int64_t ns);
void gpu_turbo();  // NX_GPU_TURBO=1: the Adreno GPU at its highest clock (perf_hint.cpp; Android)
// Frame threads and present intervals (frame_stats.cpp): the cores they run on, their CPU time, NX_AFFINITY.
// role 0: the thread that replays and presents (frame_presented ticks it); 1: the game's main thread.
void frame_thread_tick(int role);
void frame_presented();  // after each present to the display
void overlay_game_interval(int interval);  // guest's desired interval in 60 Hz ticks
void overlay_game_speed(double percent);    // ratio of applied guest simulation time to elapsed wall time
void overlay_presented(uint64_t now_ns);   // host timestamp for a presented frame
// Write watches (write_watch.cpp; page protection, POSIX): add fails when unsupported or full; take re-arms and
// says whether the range was written since the last take; the crash handler calls fault first (true: handled).
bool write_watch_add(uint64_t addr, uint64_t size);
void write_watch_remove(uint64_t addr);
bool write_watch_take(uint64_t addr);
bool write_watch_fault(void* at);
// NX_HEADLESS=1: no visible window (it is created hidden) and nothing presented; frames still render and
// NX_DUMP_FRAMES still writes them. For unattended test runs.
bool headless();
// Monitors memory against NX_MEM_LIMIT_GB (default 8); desktop builds trim reusable Vulkan images, Android stops.
void start_memory_guard();
void set_memory_limit_gb(double gb);
// How much of [from, to) (monotonic ns) the memory guard spent reading (memguard_android.cpp; 0 elsewhere).
uint64_t memguard_overlap_ns(uint64_t from, uint64_t to);
// The calling thread's CPU time, page faults (major, minor) and context switches (voluntary, involuntary)
// (frame_stats.cpp; Linux and Android, zeros elsewhere).
void thread_counters(uint64_t& cpu_ns, long faults[2], long switches[2]);
void input_script(PadState& p);  // NX_INPUT_SCRIPT presses (input_script.cpp), for unattended runs

// Audio: s16 48 kHz (2 channels stereo, or 6 channels 5.1 PCM). `done` is called when a submitted buffer has been played.
using AudioDone = void (*)(void* ctx);
bool audio_open(AudioDone done, int channels = 2);
bool audio_submit(const void* data, size_t bytes, void* ctx);  // false: no device (caller paces itself)
uint32_t audio_queued();
void audio_pause(bool paused);  // app in the background

// Vulkan: the loader's vkGetInstanceProcAddr and the surface extension of this platform.
void* vulkan_get_instance_proc_addr();
const char* vulkan_surface_extension();
// Creates the platform surface for `native_window` (VkInstance, VkSurfaceKHR*), returns a VkResult.
int vulkan_create_surface(void* instance, void* native_window, uint64_t* surface, void* gipa);

}  // namespace nx::plat

namespace nx {
void set_audio_output(int mode, int stereo_separation);  // mode: 0 Stereo, 1 Mono, 2 5.1 Surround (PCM)
}  // namespace nx
