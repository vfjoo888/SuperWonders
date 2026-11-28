// Windows host: memory, threads, time, files, the game window (F11 / Alt+Enter fullscreen), XInput/SDL gamepads + keyboard,
// XAudio2, the Vulkan loader and win32 surface, crash handler, hardware watchpoints (NX_WATCH) and main().
#include <algorithm>
#include <array>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string_view>
static inline std::filesystem::path nx_u8path(std::string_view s) { return std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(s.data()), s.size())); }
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

// SetProcessDpiAwarenessContext and per-monitor-v2 awareness are Win10 APIs.
// The bundled LLVM-MinGW headers hide their declarations unless the target
// Windows version is explicit (unlike the user's Visual Studio environment).
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <xaudio2.h>
#include <xinput.h>
#include <SDL3/SDL.h>

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <nx/game.h>
#include <nx/runtime.h>

#include "kernel.h"
#include "nvn/nvn_vk.h"
#include "overlay/overlay.h"
#include "pc_menu_win.h"
#include "svc_am.h"
#include "dualsense_haptics.h"
#include "switch2/switch2.h"
#include "platform.h"

namespace nx {
HWND g_window = nullptr;
void boot();
}  // namespace nx

namespace nx::plat {

// ---- memory ---------------------------------------------------------------------------------------------------

bool reserve(uint64_t addr, uint64_t size) {
    return VirtualAlloc(reinterpret_cast<void*>(addr), size, MEM_RESERVE, PAGE_NOACCESS) != nullptr;
}

bool commit(uint64_t addr, uint64_t size) {
    uint64_t a = addr & ~0xFFFull, e = (addr + size + 0xFFF) & ~0xFFFull;
    return VirtualAlloc(reinterpret_cast<void*>(a), e - a, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

void decommit(uint64_t addr, uint64_t size) { VirtualFree(reinterpret_cast<void*>(addr), size, MEM_DECOMMIT); }

// ---- threads and time -----------------------------------------------------------------------------------------

struct ThreadStart {
    ThreadFn fn;
    void* arg;
};

static DWORD WINAPI thread_entry(void* p) {
    ThreadStart s = *static_cast<ThreadStart*>(p);
    delete static_cast<ThreadStart*>(p);
    s.fn(s.arg);
    return 0;
}

void start_thread(ThreadFn fn, void* arg, size_t stack_size) {
    CreateThread(nullptr, stack_size, thread_entry, new ThreadStart{fn, arg}, STACK_SIZE_PARAM_IS_A_RESERVATION,
                 nullptr);
}

void exit_thread() { ExitThread(0); }
void exit_process(int code) {
    nx::nvn::flush_pipeline_cache();
    std::fflush(stderr);  // log_line buffers redirected stderr to avoid periodic render-thread stalls.
    ExitProcess(UINT(code));
}
void sleep_ns(int64_t ns) {
    // A high-resolution waitable timer (Windows 10 1803+): Sleep rounds to the system tick (1 to 15.6 ms), and the
    // 60 Hz pacing jittered by that much.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2
#endif
    thread_local HANDLE timer =
        CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    due.QuadPart = -(ns / 100);  // relative, in 100 ns
    if (!timer || !SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
        return;
    }
    WaitForSingleObject(timer, INFINITE);
}

// D3DKMTWaitForVerticalBlankEvent on the adapter output that shows the game window (gdi32, no WDK header).
bool wait_vblank() {
    struct OpenFromHdc { HDC hdc; UINT adapter; LUID luid; UINT source; };
    struct Blank { UINT adapter; UINT device; UINT source; };
    using OpenFn = LONG(APIENTRY*)(OpenFromHdc*);
    using WaitFn = LONG(APIENTRY*)(const Blank*);
    static const HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    static const auto open = gdi ? reinterpret_cast<OpenFn>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromHdc")) : nullptr;
    static const auto wait = gdi ? reinterpret_cast<WaitFn>(GetProcAddress(gdi, "D3DKMTWaitForVerticalBlankEvent")) : nullptr;
    static const bool timer = std::getenv("NX_TIMER_VSYNC") != nullptr;  // the 60 Hz timer instead (comparing)
    static HMONITOR monitor = nullptr;
    static Blank blank{};
    static int per_tick = 0;  // blanks per 1/60 s; 0: not paced on the display
    if (timer || !open || !wait || !g_window || nx::nvn::frame_cap() >= 120) return false;
    // shortcut: the adapter handle of a monitor the window left is not closed (moving monitors is rare).
    if (HMONITOR m = MonitorFromWindow(g_window, MONITOR_DEFAULTTOPRIMARY); m != monitor) {
        monitor = m, per_tick = 0;
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof mi;
        DEVMODEW dm{};
        dm.dmSize = sizeof dm;
        int hz = 0;
        if (GetMonitorInfoW(m, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
            hz = int(dm.dmDisplayFrequency);  // 59.94 Hz reads 59 or 60
            int n = (hz + 30) / 60;
            if (n >= 1 && std::abs(hz - 60 * n) <= 1)
                if (HDC dc = CreateDCW(nullptr, mi.szDevice, nullptr, nullptr)) {
                    OpenFromHdc o{dc};
                    if (open(&o) == 0) blank = {o.adapter, 0, o.source}, per_tick = n;
                    DeleteDC(dc);
                }
        }
        log("display: %d Hz, the game paced on %s", hz, per_tick ? "its vertical blank" : "a 60 Hz timer");
    }
    for (int k = 0; k < per_tick; k++)
        if (wait(&blank) != 0) return per_tick = 0, false;
    return per_tick != 0;
}
void yield() { SwitchToThread(); }
void set_thread_high_priority() { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL); }
void set_thread_low_priority() { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); }
static void ensure_win_gaming_mode() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (HMODULE winmm = LoadLibraryW(L"winmm.dll")) {
            using TimeBeginFn = UINT(WINAPI*)(UINT);
            if (auto fn = reinterpret_cast<TimeBeginFn>(GetProcAddress(winmm, "timeBeginPeriod"))) fn(1);
        }
        SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
        if (HMODULE k32 = GetModuleHandleW(L"kernel32.dll")) {
            using SetProcInfoFn = BOOL(WINAPI*)(HANDLE, PROCESS_INFORMATION_CLASS, LPVOID, DWORD);
            if (auto fn = reinterpret_cast<SetProcInfoFn>(GetProcAddress(k32, "SetProcessInformation"))) {
                PROCESS_POWER_THROTTLING_STATE ppts{PROCESS_POWER_THROTTLING_CURRENT_VERSION,
                                                    PROCESS_POWER_THROTTLING_EXECUTION_SPEED, 0};
                fn(GetCurrentProcess(), ProcessPowerThrottling, &ppts, sizeof ppts);
            }
        }
    });
}
void set_thread_name(const char* name) {
    ensure_win_gaming_mode();
    if (!name || !*name) return;
    if (HMODULE k32 = GetModuleHandleW(L"kernel32.dll")) {
        using SetDescFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
        if (auto fn = reinterpret_cast<SetDescFn>(GetProcAddress(k32, "SetThreadDescription"))) {
            wchar_t wname[64]{};
            for (size_t i = 0; i + 1 < std::size(wname) && name[i]; ++i) wname[i] = wchar_t(uint8_t(name[i]));
            fn(GetCurrentThread(), wname);
        }
    }
    const bool critical = std::strcmp(name, "nvn replay") == 0 || std::strcmp(name, "vsync") == 0;
    const bool gaming = critical || std::strcmp(name, "MainThread") == 0 || std::strcmp(name, "audio") == 0;
    if (gaming) {
        if (HMODULE avrt = LoadLibraryW(L"avrt.dll")) {
            using AvSetFn = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);
            if (auto fn = reinterpret_cast<AvSetFn>(GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW"))) {
                DWORD task_idx = 0;
                fn(L"Games", &task_idx);
            }
        }
        SetThreadPriority(GetCurrentThread(), critical ? THREAD_PRIORITY_HIGHEST : THREAD_PRIORITY_ABOVE_NORMAL);
    }
}
float default_render_scale() { return 1.0f; }
void release_window(void*) {}
uint64_t window_generation() { return 1; }
void audio_pause(bool) {}

std::string setting(const std::string& key) {
    std::string env = "NX_";
    for (char ch : key) env += char(std::toupper(static_cast<unsigned char>(ch)));
    const char* v = std::getenv(env.c_str());
    return v ? v : "";
}
int64_t current_thread_id() { return int64_t(GetCurrentThreadId()); }
bool profile(const int64_t*, const uint64_t* const*, size_t, int, const std::string&) { return false; }

uint64_t monotonic_ns() {
    static const LARGE_INTEGER f = [] {
        LARGE_INTEGER x;
        QueryPerformanceFrequency(&x);
        return x;
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return uint64_t(double(t.QuadPart) * 1e9 / double(f.QuadPart));
}

// ---- files and output -----------------------------------------------------------------------------------------

FILE* fopen_utf8(const std::string& path, const char* mode) {
    std::wstring wp = nx_u8path(path).wstring();
    std::wstring wm(mode, mode + std::strlen(mode));
    return _wfopen(wp.c_str(), wm.c_str());
}
int64_t ftell64(FILE* f) { return _ftelli64(f); }
int fseek64(FILE* f, int64_t off, int origin) { return _fseeki64(f, off, origin); }
uint64_t read_at(FILE* f, int64_t off, void* buf, uint64_t size) {
    _lock_file(f);  // seek + read as one step for this FILE
    _fseeki64_nolock(f, off, SEEK_SET);
    uint64_t n = _fread_nolock(buf, 1, size, f);
    _unlock_file(f);
    return n;
}
bool make_dirs(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(nx_u8path(path), ec);
    return !ec;
}

void log_line(const char* line) {
    static const bool write_stderr = [] {
        HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
        if (!h || h == INVALID_HANDLE_VALUE) return false;
        if (GetFileType(h) != FILE_TYPE_CHAR && !std::getenv("NX_STDERR_UNBUFFERED"))
            std::setvbuf(stderr, nullptr, _IOFBF, 64 * 1024);
        return true;
    }();
    if (write_stderr) std::fprintf(stderr, "%s\n", line);
}

namespace {

std::mutex g_data_root_mutex;
bool g_has_data_root_override = false;
bool g_data_root_loaded = false;
std::string g_data_root_value;

std::string clean_root_string(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) {
        if (s.size() == 3 && s[1] == ':') break;  // preserve drive root like C:\ or C:/
        s.pop_back();
    }
    return s;
}

std::string path_utf8(const std::filesystem::path& p) {
    const auto u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

bool paths_same(const std::string& a, const std::string& b) {
    const auto pa = nx_u8path(clean_root_string(a)).lexically_normal();
    const auto pb = nx_u8path(clean_root_string(b)).lexically_normal();
    std::error_code ec;
    if (std::filesystem::exists(pa, ec) && std::filesystem::exists(pb, ec)) {
        const bool eq = std::filesystem::equivalent(pa, pb, ec);
        if (!ec) return eq;
    }
    return _wcsicmp(pa.wstring().c_str(), pb.wstring().c_str()) == 0;
}

std::filesystem::path data_root_pointer_path() {
    return nx_u8path(default_data_root()) / "data_root.txt";
}

}  // namespace

std::string default_data_root() {
    // A directly launched build has no batch file to set NX_DATA_ROOT. Keep game data
    // next to the executable so it works regardless of the process working directory.
    std::wstring module_path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, module_path.data(), static_cast<DWORD>(module_path.size()));
    if (length == 0 || length >= module_path.size()) return nx::game::kInfo.pc_data_root;
    module_path.resize(length);
    return path_utf8(std::filesystem::path(module_path).parent_path() / L"data");
}

std::string data_root() {
    std::lock_guard lk(g_data_root_mutex);
    if (g_has_data_root_override) return g_data_root_value;
    if (const char* r = std::getenv("NX_DATA_ROOT"); r && *r) return r;
    if (!g_data_root_loaded) {
        g_data_root_loaded = true;
        std::ifstream f(data_root_pointer_path());
        std::string line;
        if (std::getline(f, line)) {
            line = clean_root_string(std::move(line));
            if (!line.empty()) {
                std::error_code ec;
                const auto p = nx_u8path(line);
                if (std::filesystem::is_directory(p, ec) || (std::filesystem::create_directories(p, ec) && !ec)) {
                    g_data_root_value = std::move(line);
                    return g_data_root_value;
                }
            }
        }
        g_data_root_value.clear();
    }
    if (!g_data_root_value.empty()) return g_data_root_value;
    return default_data_root();
}

bool set_data_root(const std::string& path) {
    const std::string cleaned = clean_root_string(path);
    const std::string def = default_data_root();
    const std::filesystem::path ptr = data_root_pointer_path();
    std::error_code ec;
    if (cleaned.empty() || paths_same(cleaned, def)) {
        std::filesystem::remove(ptr, ec);
        std::lock_guard lk(g_data_root_mutex);
        g_has_data_root_override = true;
        g_data_root_loaded = true;
        g_data_root_value = def;
        return true;
    }
    const auto target = nx_u8path(cleaned);
    if (!std::filesystem::is_directory(target, ec) && (!std::filesystem::create_directories(target, ec) || ec))
        return false;
    std::filesystem::create_directories(ptr.parent_path(), ec);
    const auto tmp = std::filesystem::path(ptr.wstring() + L".tmp");
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) return false;
        f << cleaned << '\n';
        if (!f.flush()) return false;
    }
    if (!MoveFileExW(tmp.c_str(), ptr.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    std::lock_guard lk(g_data_root_mutex);
    g_has_data_root_override = true;
    g_data_root_loaded = true;
    g_data_root_value = cleaned;
    return true;
}

static PcMenuState g_pc_settings;
static std::array<std::atomic<int>, 4> g_player_devices{};
static std::atomic<bool> g_keyboard_separate{false};
static std::atomic<int> g_button_icons{0};
static std::array<std::atomic<int>, 4> g_pad_left_deadzone{{{16}, {16}, {16}, {16}}};
static std::array<std::atomic<int>, 4> g_pad_right_deadzone{{{16}, {16}, {16}, {16}}};
static std::atomic<int> g_controller_preset{0};
static std::array<std::array<std::atomic<int>, kPcControlCount>, 4> g_device_bindings{};
static std::atomic<IXAudio2MasteringVoice*> g_master_voice{nullptr};
static std::array<std::atomic<int>, kPcControlCount> g_keyboard_bindings{};
static std::array<std::atomic<int>, kPcControlCount> g_controller_bindings{};
static std::array<std::atomic<int>, kPcHotkeyCount> g_hotkey_bindings{{{VK_F1}, {VK_F2}, {VK_F11}, {VK_TAB}}};
static std::atomic<bool> g_pause_in_background{false};
static std::atomic<bool> g_ignore_pad_in_background{false};
static std::atomic<HWND> g_metrics_window{nullptr};
static std::atomic<bool> g_metrics_enabled{false}, g_metric_fps{true}, g_metric_one_percent_low{true};
static std::atomic<bool> g_metric_game_speed{true}, g_metric_frametime_graph{true};
static std::atomic<int> g_metrics_position{0};
static std::atomic<double> g_game_speed{100.0};
static constexpr size_t kFrameHistory = 300;
static std::mutex g_metrics_mutex;
static std::array<double, kFrameHistory> g_frame_intervals_ms{};
static size_t g_frame_interval_next = 0, g_frame_interval_count = 0;
static uint64_t g_previous_present_ns = 0;
static constexpr uint64_t kPadActionBits[kPcControlCount] = {
    kA, kB, kX, kY, kL, kR, kZL, kZR, kPlus, kMinus, kStickL, kStickR, kLeft, kUp, kRight, kDown,
};

static void metrics_reposition();

// ---- the Dear ImGui overlay menu (overlay/; NX_IMGUI_MENU=0: the Win32 menu and metrics window as before) ----------

static overlay::Settings to_overlay(const PcMenuState& s) {
    overlay::Settings o;
    o.width = s.width, o.height = s.height, o.aspect_ratio = s.aspect_ratio, o.monitor = s.monitor, o.internal_resolution = s.internal_resolution, o.volume = s.volume;
    o.audio_mode = s.audio_mode, o.stereo_separation = s.stereo_separation;
    o.language = s.language, o.theme = s.theme, o.game_language = s.game_language, o.memory_limit_gb = s.memory_limit_gb;
    o.player_devices = s.player_devices, o.pad_bindings = s.pad_bindings, o.pad_custom_mask = s.pad_custom_mask, o.pad_presets = s.pad_presets;
    o.keyboard_separate = s.keyboard_separate;
    o.controller_preset = s.controller_preset, o.button_icons = s.button_icons, o.muted = s.muted,
        o.mute_in_background = s.mute_in_background, o.pause_in_background = s.pause_in_background,
        o.ignore_pad_in_background = s.ignore_pad_in_background, o.fullscreen = s.fullscreen;
    o.always_on_top = s.always_on_top, o.metrics_enabled = s.metrics_enabled, o.low_vram_profile = s.low_vram_profile, o.discord_rpc = s.discord_rpc;
    o.metric_fps = s.metric_fps, o.metric_one_percent_low = s.metric_one_percent_low;
    o.metric_game_speed = s.metric_game_speed, o.metric_frametime_graph = s.metric_frametime_graph;
    o.overlay_position = s.overlay_position, o.controller_custom_mask = s.controller_custom_mask;
    o.speedrun_timer = s.speedrun_timer, o.speedrun_timer_mode = s.speedrun_timer_mode;
    o.speedrun_timer_position = s.speedrun_timer_position, o.speedrun_timer_size = s.speedrun_timer_size;
    o.speedrun_timer_color = s.speedrun_timer_color;
    o.upscaler = s.upscaler, o.fsr_sharpness = s.fsr_sharpness, o.antialiasing = s.antialiasing;
    o.anisotropy = s.anisotropy, o.shadow_scale = s.shadow_scale, o.rumble = s.rumble,
        o.pad_left_deadzones = s.pad_left_deadzones, o.pad_right_deadzones = s.pad_right_deadzones, o.vsync = s.vsync, o.frame_cap = s.frame_cap;
    o.hdr = s.hdr, o.hdr_peak = s.hdr_peak, o.hdr_paper_white = s.hdr_paper_white;
    o.hdr_highlights = s.hdr_highlights, o.hdr_highlight_start = s.hdr_highlight_start, o.hdr_sdr_eotf = s.hdr_sdr_eotf;
    o.hdr_exposure = s.hdr_exposure, o.hdr_contrast = s.hdr_contrast, o.hdr_shadows = s.hdr_shadows;
    o.hdr_saturation = s.hdr_saturation, o.hdr_highlight_saturation = s.hdr_highlight_saturation;
    for (size_t i = 0; i < kPcControlCount; ++i)
        o.keyboard_bindings[i] = s.keyboard_bindings[i], o.controller_bindings[i] = s.controller_bindings[i];
    for (size_t i = 0; i < kPcHotkeyCount; ++i) o.hotkey_bindings[i] = s.hotkey_bindings[i];
    o.title_quit_button = s.title_quit_button;
    return o;
}

static PcMenuState from_overlay(const overlay::Settings& o) {
    PcMenuState s = g_pc_settings;
    s.width = o.width, s.height = o.height, s.aspect_ratio = o.aspect_ratio, s.monitor = o.monitor, s.internal_resolution = o.internal_resolution, s.volume = o.volume;
    s.audio_mode = o.audio_mode, s.stereo_separation = o.stereo_separation;
    s.language = o.language, s.theme = o.theme, s.game_language = o.game_language, s.memory_limit_gb = o.memory_limit_gb;
    s.player_devices = o.player_devices, s.pad_bindings = o.pad_bindings, s.pad_custom_mask = o.pad_custom_mask, s.pad_presets = o.pad_presets;
    s.keyboard_separate = o.keyboard_separate;
    s.controller_preset = o.controller_preset, s.button_icons = o.button_icons, s.muted = o.muted,
        s.mute_in_background = o.mute_in_background, s.pause_in_background = o.pause_in_background,
        s.ignore_pad_in_background = o.ignore_pad_in_background, s.fullscreen = o.fullscreen;
    s.always_on_top = o.always_on_top, s.metrics_enabled = o.metrics_enabled, s.low_vram_profile = o.low_vram_profile, s.discord_rpc = o.discord_rpc;
    s.metric_fps = o.metric_fps, s.metric_one_percent_low = o.metric_one_percent_low;
    s.metric_game_speed = o.metric_game_speed, s.metric_frametime_graph = o.metric_frametime_graph;
    s.overlay_position = o.overlay_position, s.controller_custom_mask = o.controller_custom_mask;
    s.speedrun_timer = o.speedrun_timer, s.speedrun_timer_mode = o.speedrun_timer_mode;
    s.speedrun_timer_position = o.speedrun_timer_position, s.speedrun_timer_size = o.speedrun_timer_size;
    s.speedrun_timer_color = o.speedrun_timer_color;
    s.upscaler = o.upscaler, s.fsr_sharpness = o.fsr_sharpness, s.antialiasing = o.antialiasing;
    s.anisotropy = o.anisotropy, s.shadow_scale = o.shadow_scale, s.rumble = o.rumble,
        s.pad_left_deadzones = o.pad_left_deadzones, s.pad_right_deadzones = o.pad_right_deadzones, s.vsync = o.vsync, s.frame_cap = o.frame_cap;
    s.hdr = o.hdr, s.hdr_peak = o.hdr_peak, s.hdr_paper_white = o.hdr_paper_white;
    s.hdr_highlights = o.hdr_highlights, s.hdr_highlight_start = o.hdr_highlight_start, s.hdr_sdr_eotf = o.hdr_sdr_eotf;
    s.hdr_exposure = o.hdr_exposure, s.hdr_contrast = o.hdr_contrast, s.hdr_shadows = o.hdr_shadows;
    s.hdr_saturation = o.hdr_saturation, s.hdr_highlight_saturation = o.hdr_highlight_saturation;
    for (size_t i = 0; i < kPcControlCount; ++i)
        s.keyboard_bindings[i] = o.keyboard_bindings[i], s.controller_bindings[i] = o.controller_bindings[i];
    for (size_t i = 0; i < kPcHotkeyCount; ++i) s.hotkey_bindings[i] = o.hotkey_bindings[i];
    s.title_quit_button = o.title_quit_button;
    return s;
}

// The overlay applies on the presenting thread; window changes belong to the window thread, so they go there.
constexpr UINT kOverlayApplyMessage = WM_APP + 60;
struct OverlayApply {
    PcMenuState state;
    bool persist;
};

static void overlay_apply(const overlay::Settings& s, bool persist) {
    auto* a = new OverlayApply{from_overlay(s), persist};
    if (!g_window || !PostMessageW(g_window, kOverlayApplyMessage, 0, reinterpret_cast<LPARAM>(a))) delete a;
}

static const char* overlay_key_name(int vk) {
    thread_local char name[64];
    UINT scan = MapVirtualKeyW(UINT(vk), MAPVK_VK_TO_VSC);
    LONG l = LONG(scan) << 16;
    if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN || vk == VK_INSERT || vk == VK_DELETE ||
        vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT || vk == VK_RCONTROL || vk == VK_RMENU)
        l |= 1 << 24;  // extended keys
    if (GetKeyNameTextA(l, name, sizeof name) > 0) return name;
    std::snprintf(name, sizeof name, "VK %d", vk);
    return name;
}

static overlay::Key overlay_key(WPARAM vk) {
    switch (vk) {
        case VK_TAB: return overlay::KeyTab;
        case VK_LEFT: return overlay::KeyLeft;
        case VK_RIGHT: return overlay::KeyRight;
        case VK_UP: return overlay::KeyUp;
        case VK_DOWN: return overlay::KeyDown;
        case VK_PRIOR: return overlay::KeyPageUp;
        case VK_NEXT: return overlay::KeyPageDown;
        case VK_HOME: return overlay::KeyHome;
        case VK_END: return overlay::KeyEnd;
        case VK_RETURN: return overlay::KeyEnter;
        case VK_ESCAPE: return overlay::KeyEscape;
        case VK_BACK: return overlay::KeyBackspace;
        case VK_DELETE: return overlay::KeyDelete;
        case VK_SPACE: return overlay::KeySpace;
        case VK_SHIFT: return overlay::KeyShift;
        case VK_CONTROL: return overlay::KeyCtrl;
        default: return overlay::KeyNone;
    }
}

static bool metrics_has_content() {
    return g_metrics_enabled.load(std::memory_order_relaxed) &&
           (g_metric_fps.load(std::memory_order_relaxed) || g_metric_one_percent_low.load(std::memory_order_relaxed) ||
            g_metric_game_speed.load(std::memory_order_relaxed) ||
            g_metric_frametime_graph.load(std::memory_order_relaxed));
}

static void apply_metrics_settings(const PcMenuState& state) {
    g_metrics_enabled.store(state.metrics_enabled && !overlay::enabled(), std::memory_order_relaxed);
    g_metric_fps.store(state.metric_fps, std::memory_order_relaxed);
    g_metric_one_percent_low.store(state.metric_one_percent_low, std::memory_order_relaxed);
    g_metric_game_speed.store(state.metric_game_speed, std::memory_order_relaxed);
    g_metric_frametime_graph.store(state.metric_frametime_graph, std::memory_order_relaxed);
    g_metrics_position.store(std::clamp(state.overlay_position, 0, 3), std::memory_order_relaxed);
    metrics_reposition();
}

static void apply_input_bindings(const PcMenuState& state) {
    g_keyboard_separate.store(state.keyboard_separate);
    g_button_icons.store(std::clamp(state.button_icons, 0, 3));
    for (size_t slot = 0; slot < 4; ++slot) {
        g_pad_left_deadzone[slot].store(std::clamp(state.pad_left_deadzones[slot], 0, 34), std::memory_order_relaxed);
        g_pad_right_deadzone[slot].store(std::clamp(state.pad_right_deadzones[slot], 0, 34), std::memory_order_relaxed);
    }
    g_controller_preset.store(std::clamp(state.controller_preset, 0, 1));
    for (size_t slot = 0; slot < 4; ++slot) g_player_devices[slot].store(state.player_devices[slot]);
    for (size_t i = 0; i < kPcControlCount; ++i) {
        for (size_t slot = 0; slot < 4; ++slot) {
            int binding = slot == 0 ? (state.controller_custom_mask & (1u << i) ? state.controller_bindings[i] : pc_default_controller_binding(i, state.controller_preset)) :
                (state.pad_custom_mask[slot] & (1u << i) ? state.pad_bindings[slot][i] : pc_default_controller_binding(i, std::clamp(state.pad_presets[slot], 0, 1)));
            g_device_bindings[slot][i].store(std::clamp(binding, 0, int(kPcControlCount) - 1));
        }
        g_keyboard_bindings[i].store(std::clamp(state.keyboard_bindings[i], 0, 255), std::memory_order_relaxed);
        const int controller = state.controller_custom_mask & (1u << i)
                                   ? state.controller_bindings[i]
                                   : pc_default_controller_binding(i, state.controller_preset);
        g_controller_bindings[i].store(std::clamp(controller, 0, int(kPcControlCount) - 1),
                                       std::memory_order_relaxed);
    }
    for (size_t i = 0; i < kPcHotkeyCount; ++i)
        if (state.hotkey_bindings[i] > 0)
            g_hotkey_bindings[i].store(std::clamp(state.hotkey_bindings[i], 0, 255), std::memory_order_relaxed);
}

static void save_pc_settings() {
    std::ofstream file(nx_u8path(data_root()) / "pc-settings.ini", std::ios::trunc);
    if (!file) return;
    file << "width=" << g_pc_settings.width << "\nheight=" << g_pc_settings.height << "\nvolume=" << g_pc_settings.volume
         << "\naudio_mode=" << g_pc_settings.audio_mode << "\nstereo_separation=" << g_pc_settings.stereo_separation
         << "\naspect_ratio=" << std::max(0, g_pc_settings.aspect_ratio)
         << "\nmonitor=" << std::max(0, g_pc_settings.monitor)
         << "\ninternal_resolution=" << g_pc_settings.internal_resolution << "\nmuted=" << g_pc_settings.muted
         << "\nmute_in_background=" << g_pc_settings.mute_in_background << "\npause_in_background=" << g_pc_settings.pause_in_background
         << "\nignore_pad_in_background=" << g_pc_settings.ignore_pad_in_background << "\nfullscreen=" << g_pc_settings.fullscreen
         << "\nalways_on_top=" << g_pc_settings.always_on_top << "\nlanguage=" << g_pc_settings.language
         << "\ntheme=" << g_pc_settings.theme
         << "\ngame_language=" << g_pc_settings.game_language
         << "\ngame_language_version=2"
         << "\nmemory_limit_gb=" << g_pc_settings.memory_limit_gb
         << "\nlow_vram_profile=" << g_pc_settings.low_vram_profile
         << "\ndiscord_rpc=" << g_pc_settings.discord_rpc
         << "\ncontroller_preset=" << g_pc_settings.controller_preset
         << "\nbutton_icons=" << g_pc_settings.button_icons
         << "\nmetrics_enabled=" << g_pc_settings.metrics_enabled << "\nmetric_fps=" << g_pc_settings.metric_fps
         << "\nmetric_one_percent_low=" << g_pc_settings.metric_one_percent_low
         << "\nmetric_game_speed=" << g_pc_settings.metric_game_speed
         << "\nmetric_frametime_graph=" << g_pc_settings.metric_frametime_graph
         << "\noverlay_position=" << g_pc_settings.overlay_position
         << "\nspeedrun_timer=" << g_pc_settings.speedrun_timer
         << "\nspeedrun_timer_mode=" << g_pc_settings.speedrun_timer_mode
         << "\nspeedrun_timer_position=" << g_pc_settings.speedrun_timer_position
         << "\nspeedrun_timer_size=" << g_pc_settings.speedrun_timer_size
         << "\nspeedrun_timer_color=" << g_pc_settings.speedrun_timer_color
         << "\ncontroller_custom_mask=" << g_pc_settings.controller_custom_mask
         << "\nupscaler=" << g_pc_settings.upscaler << "\nfsr_sharpness=" << g_pc_settings.fsr_sharpness
         << "\nantialiasing=" << g_pc_settings.antialiasing << "\nanisotropy=" << g_pc_settings.anisotropy
         << "\nshadow_scale=" << g_pc_settings.shadow_scale
         << "\nrumble=" << g_pc_settings.rumble
         << "\nvsync=" << g_pc_settings.vsync
         << "\nframe_cap=" << g_pc_settings.frame_cap
         << "\nhdr=" << g_pc_settings.hdr << "\nhdr_peak=" << g_pc_settings.hdr_peak
         << "\nhdr_paper_white=" << g_pc_settings.hdr_paper_white
         << "\nhdr_highlights=" << g_pc_settings.hdr_highlights
         << "\nhdr_highlight_start=" << g_pc_settings.hdr_highlight_start
         << "\nhdr_sdr_eotf=" << g_pc_settings.hdr_sdr_eotf << "\nhdr_exposure=" << g_pc_settings.hdr_exposure
         << "\nhdr_contrast=" << g_pc_settings.hdr_contrast << "\nhdr_shadows=" << g_pc_settings.hdr_shadows
         << "\nhdr_saturation=" << g_pc_settings.hdr_saturation
         << "\nhdr_highlight_saturation=" << g_pc_settings.hdr_highlight_saturation << '\n';
    file << "keyboard_separate=" << g_pc_settings.keyboard_separate << '\n';
    for (size_t slot = 0; slot < 4; ++slot) {
        file << "player_device_" << slot << '=' << g_pc_settings.player_devices[slot] << '\n';
        file << "pad_deadzone_l_" << slot << '=' << std::clamp(g_pc_settings.pad_left_deadzones[slot], 0, 34) << '\n';
        file << "pad_deadzone_r_" << slot << '=' << std::clamp(g_pc_settings.pad_right_deadzones[slot], 0, 34) << '\n';
        if (slot) {
            file << "pad_preset_" << slot << '=' << std::clamp(g_pc_settings.pad_presets[slot], 0, 1) << '\n';
            file << "pad_mask_" << slot << '=' << g_pc_settings.pad_custom_mask[slot] << '\n';
            for (size_t action = 0; action < kPcControlCount; ++action)
                file << "pad_" << slot << '_' << action << '=' << g_pc_settings.pad_bindings[slot][action] << '\n';
        }
    }
    for (size_t i = 0; i < kPcControlCount; ++i)
        file << "keyboard_" << i << '=' << g_pc_settings.keyboard_bindings[i] << '\n'
             << "controller_" << i << '=' << g_pc_settings.controller_bindings[i] << '\n';
    for (size_t i = 0; i < kPcHotkeyCount; ++i)
        file << "hotkey_" << i << '=' << g_pc_settings.hotkey_bindings[i] << '\n';
    file << "title_quit_button=" << std::clamp(g_pc_settings.title_quit_button, 0, int(kPcControlCount) - 1) << '\n';
}

struct WinMonitorInfo {
    HMONITOR hmon = nullptr;
    RECT rc{0, 0, 1920, 1080};
    RECT work{0, 0, 1920, 1080};
    int width = 1920;
    int height = 1080;
    bool primary = true;
    std::wstring gdi_name;
    std::string friendly_name;
};
static WinMonitorInfo target_win_monitor(int index);

static bool parse_pc_settings_file(const std::filesystem::path& ini_path, PcMenuState& state) {
    std::ifstream file(ini_path);
    if (!file) return false;
    std::string line;
    int game_language_version = 0;
    while (std::getline(file, line)) {
        int value = 0;
        if (std::sscanf(line.c_str(), "width=%d", &value) == 1 && value >= 640 && value <= 7680) state.width = value;
        else if (std::sscanf(line.c_str(), "height=%d", &value) == 1 && value >= 360 && value <= 4320) state.height = value;
        else if (std::sscanf(line.c_str(), "aspect_ratio=%d", &value) == 1) state.aspect_ratio = std::clamp(value, 0, 3);
        else if (std::sscanf(line.c_str(), "monitor=%d", &value) == 1) state.monitor = std::max(0, value);
        else if (std::sscanf(line.c_str(), "internal_resolution=%d", &value) == 1) state.internal_resolution = std::clamp(value, 0, 9);
        else if (std::sscanf(line.c_str(), "volume=%d", &value) == 1) state.volume = std::clamp(value, 0, 200);
        else if (std::sscanf(line.c_str(), "audio_mode=%d", &value) == 1) state.audio_mode = std::clamp(value, 0, 2);
        else if (std::sscanf(line.c_str(), "stereo_separation=%d", &value) == 1)
            state.stereo_separation = std::clamp(value, 0, 100);
        else if (std::sscanf(line.c_str(), "muted=%d", &value) == 1) state.muted = value != 0;
        else if (std::sscanf(line.c_str(), "mute_in_background=%d", &value) == 1)
            state.mute_in_background = value != 0;
        else if (std::sscanf(line.c_str(), "pause_in_background=%d", &value) == 1)
            state.pause_in_background = value != 0;
        else if (std::sscanf(line.c_str(), "ignore_pad_in_background=%d", &value) == 1)
            state.ignore_pad_in_background = value != 0;
        else if (std::sscanf(line.c_str(), "fullscreen=%d", &value) == 1) state.fullscreen = value != 0;
        else if (std::sscanf(line.c_str(), "always_on_top=%d", &value) == 1) state.always_on_top = value != 0;
        else if (std::sscanf(line.c_str(), "language=%d", &value) == 1) state.language = std::clamp(value, 0, 12);
        else if (std::sscanf(line.c_str(), "theme=%d", &value) == 1) state.theme = std::clamp(value, 0, 4);
        else if (std::sscanf(line.c_str(), "game_language=%d", &value) == 1)
            state.game_language = std::clamp(value, 0, 17);
        else if (std::sscanf(line.c_str(), "game_language_version=%d", &value) == 1)
            game_language_version = value;
        else if (std::sscanf(line.c_str(), "memory_limit_gb=%d", &value) == 1) state.memory_limit_gb = std::clamp(value, 2, 32);
        else if (std::sscanf(line.c_str(), "low_vram_profile=%d", &value) == 1) state.low_vram_profile = value != 0;
        else if (std::sscanf(line.c_str(), "discord_rpc=%d", &value) == 1) state.discord_rpc = value != 0;
        else if (std::sscanf(line.c_str(), "keyboard_separate=%d", &value) == 1) state.keyboard_separate = value != 0;
        else if (std::sscanf(line.c_str(), "controller_preset=%d", &value) == 1) {
            state.controller_preset = std::clamp(value, 0, 1);
            state.pad_presets[0] = state.controller_preset;
        }
        else if (std::sscanf(line.c_str(), "button_icons=%d", &value) == 1)
            state.button_icons = std::clamp(value, 0, 3);
        else if (std::sscanf(line.c_str(), "metrics_enabled=%d", &value) == 1) state.metrics_enabled = value != 0;
        else if (std::sscanf(line.c_str(), "metric_fps=%d", &value) == 1) state.metric_fps = value != 0;
        else if (std::sscanf(line.c_str(), "metric_one_percent_low=%d", &value) == 1)
            state.metric_one_percent_low = value != 0;
        else if (std::sscanf(line.c_str(), "metric_game_speed=%d", &value) == 1)
            state.metric_game_speed = value != 0;
        else if (std::sscanf(line.c_str(), "metric_frametime_graph=%d", &value) == 1)
            state.metric_frametime_graph = value != 0;
        else if (std::sscanf(line.c_str(), "overlay_position=%d", &value) == 1)
            state.overlay_position = std::clamp(value, 0, 3);
        else if (std::sscanf(line.c_str(), "speedrun_timer=%d", &value) == 1)
            state.speedrun_timer = value != 0;
        else if (std::sscanf(line.c_str(), "speedrun_timer_mode=%d", &value) == 1)
            state.speedrun_timer_mode = std::clamp(value, 0, 2);
        else if (std::sscanf(line.c_str(), "speedrun_timer_position=%d", &value) == 1)
            state.speedrun_timer_position = std::clamp(value, 0, 5);
        else if (std::sscanf(line.c_str(), "speedrun_timer_size=%d", &value) == 1)
            state.speedrun_timer_size = std::clamp(value, 0, 3);
        else if (std::sscanf(line.c_str(), "speedrun_timer_color=%d", &value) == 1)
            state.speedrun_timer_color = std::clamp(value, 0, 5);
        else if (std::sscanf(line.c_str(), "controller_custom_mask=%u", &state.controller_custom_mask) == 1) {}
        else if (std::sscanf(line.c_str(), "upscaler=%d", &value) == 1) state.upscaler = std::clamp(value, 0, 1);
        else if (std::sscanf(line.c_str(), "fsr_sharpness=%d", &value) == 1)
            state.fsr_sharpness = std::clamp(value, 0, 200);
        else if (std::sscanf(line.c_str(), "antialiasing=%d", &value) == 1)
            state.antialiasing = std::clamp(value, 0, 2);
        else if (std::sscanf(line.c_str(), "anisotropy=%d", &value) == 1)
            state.anisotropy = std::clamp(value, 0, 16);
        else if (std::sscanf(line.c_str(), "shadow_scale=%d", &value) == 1)
            state.shadow_scale = (value == 2 || value == 4) ? value : 1;
        else if (std::sscanf(line.c_str(), "rumble=%d", &value) == 1) state.rumble = std::clamp(value, 0, 100);
        else if (std::sscanf(line.c_str(), "stick_deadzone=%d", &value) == 1)
            state.pad_left_deadzones.fill(std::clamp(value, 0, 34)), state.pad_right_deadzones.fill(std::clamp(value, 0, 34));
        else if (std::sscanf(line.c_str(), "left_stick_deadzone=%d", &value) == 1)
            state.pad_left_deadzones.fill(std::clamp(value, 0, 34));
        else if (std::sscanf(line.c_str(), "right_stick_deadzone=%d", &value) == 1)
            state.pad_right_deadzones.fill(std::clamp(value, 0, 34));
        else if (std::sscanf(line.c_str(), "vsync=%d", &value) == 1) state.vsync = std::clamp(value, 0, 3);
        else if (std::sscanf(line.c_str(), "hdr=%d", &value) == 1) state.hdr = value != 0;
        else if (std::sscanf(line.c_str(), "hdr_peak=%d", &value) == 1) state.hdr_peak = std::clamp(value, 400, 10000);
        else if (std::sscanf(line.c_str(), "hdr_paper_white=%d", &value) == 1)
            state.hdr_paper_white = std::clamp(value, 80, 500);
        else if (std::sscanf(line.c_str(), "hdr_highlights=%d", &value) == 1)
            state.hdr_highlights = std::clamp(value, 0, 300);
        else if (std::sscanf(line.c_str(), "hdr_highlight_start=%d", &value) == 1)
            state.hdr_highlight_start = std::clamp(value, 50, 300);
        else if (std::sscanf(line.c_str(), "hdr_sdr_eotf=%d", &value) == 1) state.hdr_sdr_eotf = std::clamp(value, 0, 2);
        else if (std::sscanf(line.c_str(), "hdr_exposure=%d", &value) == 1) state.hdr_exposure = std::clamp(value, 25, 300);
        else if (std::sscanf(line.c_str(), "hdr_contrast=%d", &value) == 1) state.hdr_contrast = std::clamp(value, 25, 200);
        else if (std::sscanf(line.c_str(), "hdr_shadows=%d", &value) == 1) state.hdr_shadows = std::clamp(value, 25, 175);
        else if (std::sscanf(line.c_str(), "hdr_saturation=%d", &value) == 1) state.hdr_saturation = std::clamp(value, 0, 300);
        else if (std::sscanf(line.c_str(), "hdr_highlight_saturation=%d", &value) == 1)
            state.hdr_highlight_saturation = std::clamp(value, 0, 300);
        else if (std::sscanf(line.c_str(), "frame_cap=%d", &value) == 1)
            state.frame_cap = value == 30 || value == 60 || value == 90 || value == 120 || value == 144 ||
                                      value == 240
                                  ? value
                                  : 60;
        else if (std::sscanf(line.c_str(), "title_quit_button=%d", &value) == 1)
            state.title_quit_button = std::clamp(value, 0, int(kPcControlCount) - 1);
        else {
            int index = -1, binding = -1, slot = -1;
            unsigned mask = 0;
            if (std::sscanf(line.c_str(), "player_device_%d=%d", &index, &binding) == 2 && index >= 0 && index < 4 && binding >= 0 && binding <= 6)
                state.player_devices[size_t(index)] = binding;
            else if (std::sscanf(line.c_str(), "pad_preset_%d=%d", &slot, &binding) == 2 && slot > 0 && slot < 4)
                state.pad_presets[size_t(slot)] = std::clamp(binding, 0, 1);
            else if (std::sscanf(line.c_str(), "pad_deadzone_l_%d=%d", &slot, &binding) == 2 && slot >= 0 && slot < 4)
                state.pad_left_deadzones[size_t(slot)] = std::clamp(binding, 0, 34);
            else if (std::sscanf(line.c_str(), "pad_deadzone_r_%d=%d", &slot, &binding) == 2 && slot >= 0 && slot < 4)
                state.pad_right_deadzones[size_t(slot)] = std::clamp(binding, 0, 34);
            else if (std::sscanf(line.c_str(), "pad_mask_%d=%u", &slot, &mask) == 2 && slot > 0 && slot < 4)
                state.pad_custom_mask[size_t(slot)] = mask;
            else if (std::sscanf(line.c_str(), "pad_%d_%d=%d", &slot, &index, &binding) == 3 && slot > 0 && slot < 4 && index >= 0 && index < int(kPcControlCount) && binding >= 0 && binding < int(kPcControlCount))
                state.pad_bindings[size_t(slot)][size_t(index)] = binding;
            else if (std::sscanf(line.c_str(), "keyboard_%d=%d", &index, &binding) == 2 &&
                index >= 0 && index < int(kPcControlCount) && binding >= 0 && binding <= 255)
                state.keyboard_bindings[size_t(index)] = binding;
            else if (std::sscanf(line.c_str(), "controller_%d=%d", &index, &binding) == 2 &&
                     index >= 0 && index < int(kPcControlCount) && binding >= 0 && binding < int(kPcControlCount))
                state.controller_bindings[size_t(index)] = binding;
            else if (std::sscanf(line.c_str(), "hotkey_%d=%d", &index, &binding) == 2 &&
                     index >= 0 && index < int(kPcHotkeyCount) && binding >= 0 && binding <= 255)
                state.hotkey_bindings[size_t(index)] = binding;
        }
    }
    if (game_language_version < 2 && (state.game_language == 8 || state.game_language == 10))
        state.game_language = 0;
    if (state.aspect_ratio >= 0) {
        const int ratio = state.aspect_ratio;
        const double target_aspect = ratio == 1 ? (64.0 / 27.0) : ratio == 2 ? (32.0 / 9.0) : ratio == 3 ? (16.0 / 10.0) : (16.0 / 9.0);
        const double current_aspect = state.height > 0 ? double(state.width) / double(state.height) : 0.0;
        if (state.width > 0 && std::fabs(current_aspect - target_aspect) / target_aspect > 0.03)
            state.height = std::max(360, ratio == 1 ? state.width * 27 / 64 :
                                          ratio == 2 ? state.width * 9 / 32 : ratio == 3 ? state.width * 10 / 16 :
                                                       state.width * 9 / 16);
        const WinMonitorInfo init_mon = target_win_monitor(state.monitor);
        const int disp_w = state.fullscreen ? init_mon.width : state.width;
        const int disp_h = state.fullscreen ? init_mon.height : state.height;
        const double disp_aspect = disp_h > 0 ? double(disp_w) / double(disp_h) : 0.0;
        const bool uw = ratio == 1 || ratio == 2; char uw_res[32];
        if (uw && disp_w > 0 && disp_h > 0 && std::fabs(disp_aspect - target_aspect) / target_aspect <= 0.10)
            std::snprintf(uw_res, sizeof uw_res, "%dx%d", disp_w, disp_h);
        else
            std::snprintf(uw_res, sizeof uw_res, "%s", ratio == 1 ? "2560x1080" : ratio == 2 ? "3840x1080" : "0");
        _putenv_s("NX_ASPECT_PROFILE", ratio == 1 ? "21:9" : ratio == 2 ? "32:9" : "16:9");
        _putenv_s("NX_ULTRAWIDE", uw ? uw_res : "0");
        _putenv_s("NX_RESOLUTION", uw ? uw_res : "1920x1080");
        const char* flags[] = {"NX_TERRAIN_MOD_ASPECT_TEST", "NX_UW_POWERUP_TEST", "NX_UW_FLOWER_TEST",
            "NX_UW_MAP_ALIGN_TEST", "NX_UW_MAP_COMPOSITE_TEST", "NX_UW_MAP_CARD_TEST",
            "NX_UW_MAP_MARKERS_TEST", "NX_UW_MAP_DETAIL_TEST", "NX_UW_SETTINGS_SCISSOR_TEST"};
        for (const char* flag : flags) _putenv_s(flag, uw ? "1" : "0");
        _putenv_s("NX_UW_FLOWER_ALT_TEST", "0");
    }
    return true;
}

static void ensure_dpi_aware() {
    static const bool done = [] {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        return true;
    }();
    (void)done;
}

static std::string win_narrow_utf8(const wchar_t* ws) {
    if (!ws || !*ws) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(size_t(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, out.data(), n, nullptr, nullptr);
    return out;
}

static std::vector<WinMonitorInfo> enumerate_win_monitors() {
    ensure_dpi_aware();
    std::vector<std::pair<std::wstring, std::string>> edid_names;
    UINT32 path_count = 0, mode_count = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) == ERROR_SUCCESS && path_count > 0) {
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr) == ERROR_SUCCESS) {
            for (UINT32 i = 0; i < path_count; ++i) {
                DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
                src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                src.header.size = sizeof src;
                src.header.adapterId = paths[i].sourceInfo.adapterId;
                src.header.id = paths[i].sourceInfo.id;
                DISPLAYCONFIG_TARGET_DEVICE_NAME dst{};
                dst.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
                dst.header.size = sizeof dst;
                dst.header.adapterId = paths[i].targetInfo.adapterId;
                dst.header.id = paths[i].targetInfo.id;
                if (DisplayConfigGetDeviceInfo(&src.header) == ERROR_SUCCESS &&
                    DisplayConfigGetDeviceInfo(&dst.header) == ERROR_SUCCESS &&
                    dst.monitorFriendlyDeviceName[0]) {
                    edid_names.emplace_back(src.viewGdiDeviceName, win_narrow_utf8(dst.monitorFriendlyDeviceName));
                }
            }
        }
    }

    std::vector<WinMonitorInfo> list;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hm, HDC, LPRECT, LPARAM lp) -> BOOL {
        auto* out = reinterpret_cast<std::vector<WinMonitorInfo>*>(lp);
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof mi;
        if (!GetMonitorInfoW(hm, &mi)) return TRUE;
        WinMonitorInfo info;
        info.hmon = hm;
        info.rc = mi.rcMonitor;
        info.work = mi.rcWork;
        info.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        info.gdi_name = mi.szDevice;
        DEVMODEW dm{};
        dm.dmSize = sizeof dm;
        if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmPelsWidth > 0 && dm.dmPelsHeight > 0) {
            info.width = int(dm.dmPelsWidth);
            info.height = int(dm.dmPelsHeight);
        } else {
            info.width = std::max(1, int(mi.rcMonitor.right - mi.rcMonitor.left));
            info.height = std::max(1, int(mi.rcMonitor.bottom - mi.rcMonitor.top));
        }
        out->push_back(std::move(info));
        return TRUE;
    }, reinterpret_cast<LPARAM>(&list));

    for (auto& info : list) {
        for (const auto& [gdi, fname] : edid_names) {
            if (gdi == info.gdi_name) { info.friendly_name = fname; break; }
        }
        if (info.friendly_name.empty()) {
            DISPLAY_DEVICEW dd{};
            dd.cb = sizeof dd;
            if (EnumDisplayDevicesW(info.gdi_name.c_str(), 0, &dd, 0) && dd.DeviceString[0])
                info.friendly_name = win_narrow_utf8(dd.DeviceString);
        }
        if (info.friendly_name.empty()) info.friendly_name = win_narrow_utf8(info.gdi_name.c_str());
        if (info.friendly_name.empty()) info.friendly_name = "Display";
    }
    std::sort(list.begin(), list.end(), [](const WinMonitorInfo& a, const WinMonitorInfo& b) {
        if (a.primary != b.primary) return a.primary > b.primary;
        if (a.rc.left != b.rc.left) return a.rc.left < b.rc.left;
        return a.rc.top < b.rc.top;
    });
    if (list.empty()) {
        WinMonitorInfo fallback;
        fallback.width = std::max(640, GetSystemMetrics(SM_CXSCREEN));
        fallback.height = std::max(360, GetSystemMetrics(SM_CYSCREEN));
        fallback.rc = {0, 0, fallback.width, fallback.height};
        fallback.work = fallback.rc;
        fallback.friendly_name = "Primary Display";
        list.push_back(std::move(fallback));
    }
    return list;
}

static WinMonitorInfo target_win_monitor(int index) {
    auto list = enumerate_win_monitors();
    const int idx = std::clamp(index, 0, int(list.size()) - 1);
    return list[size_t(idx)];
}

static std::vector<std::string> overlay_monitors() {
    auto list = enumerate_win_monitors();
    std::vector<std::string> labels;
    labels.reserve(list.size());
    for (size_t i = 0; i < list.size(); ++i) {
        char buf[256];
        std::snprintf(buf, sizeof buf, "Monitor %zu: %s (%dx%d)", i + 1, list[i].friendly_name.c_str(), list[i].width, list[i].height);
        labels.emplace_back(buf);
    }
    return labels;
}

static void apply_loaded_pc_settings(const PcMenuState& state) {
    ensure_dpi_aware();
    nx::set_internal_resolution_preset(state.internal_resolution);
    nx::set_audio_output(state.audio_mode, state.stereo_separation);
    nx::set_game_language(state.game_language);
    apply_input_bindings(state);
    g_pause_in_background.store(state.pause_in_background, std::memory_order_relaxed);
    g_ignore_pad_in_background.store(state.ignore_pad_in_background, std::memory_order_relaxed);
    set_memory_limit_gb(state.memory_limit_gb);
    _putenv_s("NX_LOW_VRAM_PROFILE", state.low_vram_profile ? "1" : "0");
    nx::nvn::set_fsr(state.upscaler == 1, float(state.fsr_sharpness) / 100.0f);
    nx::nvn::set_aa_mode(state.antialiasing);
    nx::nvn::set_anisotropy(state.anisotropy);
    nx::nvn::set_shadow_scale(state.shadow_scale);
    nx::nvn::set_vsync(state.vsync);
    nx::nvn::set_frame_cap(state.frame_cap);
    nx::nvn::set_hdr(state.hdr != 0, state.hdr_peak, state.hdr_paper_white, state.hdr_highlights,
                     state.hdr_highlight_start);
    nx::nvn::set_hdr_sdr_eotf(state.hdr_sdr_eotf);
    nx::nvn::set_hdr_grading(state.hdr_exposure, state.hdr_contrast, state.hdr_shadows, state.hdr_saturation,
                             state.hdr_highlight_saturation);
    const WinMonitorInfo mon = target_win_monitor(state.monitor);
    const uint32_t nw = uint32_t(mon.width);
    // monitor native size applied below
    const uint32_t nh = uint32_t(mon.height);
    // (height)
    nx::set_native_render_size(nw, nh);
}

static void load_pc_settings() {
    parse_pc_settings_file(nx_u8path(data_root()) / "pc-settings.ini", g_pc_settings);
    apply_loaded_pc_settings(g_pc_settings);
}

void reload_pc_settings() {
    const auto ini = nx_u8path(data_root()) / "pc-settings.ini";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(ini, ec)) return;
    PcMenuState fresh;
    if (!parse_pc_settings_file(ini, fresh)) return;
    if (!g_window) {
        g_pc_settings = fresh;
        apply_loaded_pc_settings(g_pc_settings);
        return;
    }
    auto* msg = new OverlayApply{fresh, false};
    if (!PostMessageW(g_window, kOverlayApplyMessage, 0, reinterpret_cast<LPARAM>(msg))) delete msg;
    if (overlay::enabled()) overlay::sync(to_overlay(fresh));
}

static std::atomic<bool> g_window_focused{true};

static void set_game_volume() {
    if (IXAudio2MasteringVoice* voice = g_master_voice.load()) {
        const bool muted = g_pc_settings.muted ||
                           (g_pc_settings.mute_in_background && !g_window_focused.load(std::memory_order_relaxed)) || paused();
        const int percent = std::clamp(g_pc_settings.volume, 0, 200);
        const float gain = percent <= 100 ? (float(percent) / 100.0f) * (float(percent) / 100.0f)
                                          : float(percent) / 100.0f;
        voice->SetVolume(muted ? 0.0f : gain, XAUDIO2_COMMIT_NOW);
    }
}

// ---- window ---------------------------------------------------------------------------------------------------

static std::wstring widen(const char* s) { return std::wstring(s, s + std::strlen(s)); }  // ASCII titles
// The game renders its swapchain at the render resolution; the window can be any size (the compositor scales).

static constexpr DWORD kWindowed = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
static bool g_fullscreen = false;
static WINDOWPLACEMENT g_placement{sizeof(WINDOWPLACEMENT)};
static std::atomic<uint64_t> g_shader_compiled{0}, g_shader_total{0};
static std::atomic<bool> g_shader_progress_visible{false};
static void toggle_fullscreen(HWND h);

static void resize_window_client(HWND h, int width, int height) {
    if (!h || g_fullscreen) return;
    RECT r{0, 0, width, height};
    const DWORD style = DWORD(GetWindowLongPtrW(h, GWL_STYLE));
    const DWORD ex_style = DWORD(GetWindowLongPtrW(h, GWL_EXSTYLE));
    AdjustWindowRectEx(&r, style, FALSE, ex_style);
    RECT current{};
    GetWindowRect(h, &current);
    const int outer_w = r.right - r.left, outer_h = r.bottom - r.top;
    const int x = current.left + ((current.right - current.left) - outer_w) / 2;
    const int y = current.top + ((current.bottom - current.top) - outer_h) / 2;
    SetWindowPos(h, nullptr, x, y, outer_w, outer_h, SWP_NOZORDER | SWP_NOACTIVATE);
}

static int metrics_overlay_height(bool graph, int rows) { return 42 + rows * 23 + (graph ? 92 : 0); }

static void metrics_paint(HWND h) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(h, &ps);
    RECT client{};
    GetClientRect(h, &client);
    HBRUSH key = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(dc, &client, key);
    DeleteObject(key);

    const bool show_fps = g_metric_fps.load(std::memory_order_relaxed);
    const bool show_low = g_metric_one_percent_low.load(std::memory_order_relaxed);
    const bool show_speed = g_metric_game_speed.load(std::memory_order_relaxed);
    const bool show_graph = g_metric_frametime_graph.load(std::memory_order_relaxed);
    const int rows = int(show_fps) + int(show_low) + int(show_speed);
    RECT panel{2, 2, client.right - 2, client.bottom - 2};
    HBRUSH panel_brush = CreateSolidBrush(RGB(20, 27, 40));
    HPEN border = CreatePen(PS_SOLID, 1, RGB(83, 112, 151));
    HGDIOBJ old_brush = SelectObject(dc, panel_brush);
    HGDIOBJ old_pen = SelectObject(dc, border);
    RoundRect(dc, panel.left, panel.top, panel.right, panel.bottom, 12, 12);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(panel_brush);
    DeleteObject(border);

    std::array<double, kFrameHistory> samples{};
    size_t count = 0;
    {
        std::lock_guard lock(g_metrics_mutex);
        count = g_frame_interval_count;
        const size_t start = (g_frame_interval_next + kFrameHistory - count) % kFrameHistory;
        for (size_t i = 0; i < count; ++i) samples[i] = g_frame_intervals_ms[(start + i) % kFrameHistory];
    }
    double sum = 0.0;
    const size_t recent = std::min<size_t>(count, 60);
    for (size_t i = count - recent; i < count; ++i) sum += samples[i];
    const double fps = sum > 0.0 ? double(recent) * 1000.0 / sum : 0.0;
    double one_low = 0.0;
    if (count) {
        std::vector<double> slow(samples.begin(), samples.begin() + count);
        std::sort(slow.begin(), slow.end(), std::greater<double>());
        const size_t slow_count = std::max<size_t>(1, (count + 99) / 100);
        double slow_sum = 0.0;
        for (size_t i = 0; i < slow_count; ++i) slow_sum += slow[i];
        if (slow_sum > 0.0) one_low = double(slow_count) * 1000.0 / slow_sum;
    }
    const double speed = g_game_speed.load(std::memory_order_relaxed);

    SetBkMode(dc, TRANSPARENT);
    HFONT font = CreateFontW(-16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ old_font = SelectObject(dc, font);
    SetTextColor(dc, RGB(235, 242, 253));
    int y = 9;
    wchar_t line[96];
    auto draw_metric = [&](const wchar_t* name, double value, const wchar_t* unit, int precision) {
        if (precision == 0) swprintf_s(line, L"%s  %.0f%s", name, value, unit);
        else swprintf_s(line, L"%s  %.1f%s", name, value, unit);
        RECT text{14, y, client.right - 12, y + 22};
        DrawTextW(dc, line, -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y += 23;
    };
    if (show_fps) draw_metric(L"FPS", fps, L"", 1);
    if (show_low) draw_metric(L"1% low", one_low, L" FPS", 1);
    if (show_speed) draw_metric(L"Game speed", speed, L"%", 1);
    SelectObject(dc, old_font);
    DeleteObject(font);

    if (show_graph) {
        SetTextColor(dc, RGB(170, 190, 218));
        RECT graph_title{14, y + 1, client.right - 12, y + 19};
        DrawTextW(dc, L"Frametime (ms)", -1, &graph_title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT graph{14, y + 23, client.right - 12, client.bottom - 10};
        HBRUSH graph_brush = CreateSolidBrush(RGB(31, 40, 56));
        FillRect(dc, &graph, graph_brush);
        DeleteObject(graph_brush);
        HPEN grid_pen = CreatePen(PS_SOLID, 1, RGB(62, 74, 95));
        HGDIOBJ previous_pen = SelectObject(dc, grid_pen);
        for (int row = 1; row < 4; ++row) {
            const int gy = graph.top + (graph.bottom - graph.top) * row / 4;
            MoveToEx(dc, graph.left, gy, nullptr);
            LineTo(dc, graph.right, gy);
        }
        SelectObject(dc, previous_pen);
        DeleteObject(grid_pen);
        if (count > 1) {
            std::vector<POINT> points;
            points.reserve(count);
            for (size_t i = 0; i < count; ++i) {
                const int x = graph.left + int((graph.right - graph.left - 1) * i / (count - 1));
                const double ms = std::clamp(samples[i], 0.0, 50.0);
                const int py = graph.bottom - 1 - int((graph.bottom - graph.top - 1) * ms / 50.0);
                points.push_back({x, py});
            }
            HPEN line_pen = CreatePen(PS_SOLID, 2, RGB(89, 190, 255));
            HGDIOBJ previous = SelectObject(dc, line_pen);
            Polyline(dc, points.data(), int(points.size()));
            SelectObject(dc, previous);
            DeleteObject(line_pen);
        }
    }
    EndPaint(h, &ps);
}

static LRESULT CALLBACK metrics_wndproc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
        case WM_PAINT:
            metrics_paint(h);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
    }
    return DefWindowProcW(h, msg, w, l);
}

static void metrics_init(HWND owner) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = metrics_wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"WonderRecompMetricsOverlay";
    RegisterClassW(&wc);
    HWND overlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                   wc.lpszClassName, L"Wonder Metrics", WS_POPUP, 0, 0, 320, 180, owner, nullptr,
                                   wc.hInstance, nullptr);
    if (overlay) {
        SetLayeredWindowAttributes(overlay, RGB(0, 0, 0), 230, LWA_COLORKEY | LWA_ALPHA);
        g_metrics_window.store(overlay, std::memory_order_release);
    }
}

static void metrics_reposition() {
    HWND overlay = g_metrics_window.load(std::memory_order_acquire);
    HWND owner = nx::g_window;
    if (!overlay || !owner) return;
    if (!metrics_has_content()) {
        ShowWindow(overlay, SW_HIDE);
        return;
    }
    const bool show_graph = g_metric_frametime_graph.load(std::memory_order_relaxed);
    const int rows = int(g_metric_fps.load(std::memory_order_relaxed)) +
                     int(g_metric_one_percent_low.load(std::memory_order_relaxed)) +
                     int(g_metric_game_speed.load(std::memory_order_relaxed));
    const int width = 320, height = metrics_overlay_height(show_graph, rows);
    RECT client{};
    GetClientRect(owner, &client);
    POINT origin{0, 0};
    ClientToScreen(owner, &origin);
    const int position = g_metrics_position.load(std::memory_order_relaxed);
    const bool right = position == 1 || position == 3;
    const bool bottom = position == 2 || position == 3;
    const int x = origin.x + (right ? std::max(12L, client.right - width - 16) : 16);
    const int y = origin.y + (bottom ? std::max(12L, client.bottom - height - 16) : 16);
    SetWindowPos(overlay, HWND_TOP, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(overlay, nullptr, FALSE);
}

static void apply_pc_menu(const PcMenuState& state, bool persist) {
    const bool monitor_changed = state.monitor != g_pc_settings.monitor;
    const bool size_changed = state.width != g_pc_settings.width || state.height != g_pc_settings.height;
    const bool topmost_changed = state.always_on_top != g_pc_settings.always_on_top;
    g_pc_settings = state;
    g_pc_settings.width = std::clamp(g_pc_settings.width, 640, 7680);
    g_pc_settings.height = std::clamp(g_pc_settings.height, 360, 4320);
    g_pc_settings.volume = std::clamp(g_pc_settings.volume, 0, 200);
    g_pc_settings.audio_mode = std::clamp(g_pc_settings.audio_mode, 0, 2);
    g_pc_settings.stereo_separation = std::clamp(g_pc_settings.stereo_separation, 0, 100);
    g_pc_settings.internal_resolution = std::clamp(g_pc_settings.internal_resolution, 0, 9);
    g_pc_settings.monitor = std::max(0, g_pc_settings.monitor);
    g_pc_settings.language = std::clamp(g_pc_settings.language, 0, 12);
    g_pc_settings.game_language = std::clamp(g_pc_settings.game_language, 0, 17);
    g_pc_settings.memory_limit_gb = std::clamp(g_pc_settings.memory_limit_gb, 2, 32);
    g_pc_settings.controller_preset = std::clamp(g_pc_settings.controller_preset, 0, 1);
    g_pc_settings.button_icons = std::clamp(g_pc_settings.button_icons, 0, 3);
    g_pc_settings.overlay_position = std::clamp(g_pc_settings.overlay_position, 0, 3);
    apply_input_bindings(g_pc_settings);
    g_pause_in_background.store(g_pc_settings.pause_in_background, std::memory_order_relaxed);
    g_ignore_pad_in_background.store(g_pc_settings.ignore_pad_in_background, std::memory_order_relaxed);
    set_game_volume();
    nx::set_game_language(g_pc_settings.game_language);
    apply_metrics_settings(g_pc_settings);
    set_memory_limit_gb(g_pc_settings.memory_limit_gb);
    _putenv_s("NX_LOW_VRAM_PROFILE", g_pc_settings.low_vram_profile ? "1" : "0");
    nx::nvn::set_fsr(g_pc_settings.upscaler == 1, float(g_pc_settings.fsr_sharpness) / 100.0f);
    nx::nvn::set_aa_mode(g_pc_settings.antialiasing);
    nx::nvn::set_anisotropy(g_pc_settings.anisotropy);
    nx::nvn::set_shadow_scale(g_pc_settings.shadow_scale);
    nx::nvn::set_vsync(g_pc_settings.vsync);
    nx::nvn::set_frame_cap(g_pc_settings.frame_cap);
    nx::nvn::set_hdr(g_pc_settings.hdr != 0, g_pc_settings.hdr_peak, g_pc_settings.hdr_paper_white,
                     g_pc_settings.hdr_highlights, g_pc_settings.hdr_highlight_start);
    nx::nvn::set_hdr_sdr_eotf(g_pc_settings.hdr_sdr_eotf);
    nx::nvn::set_hdr_grading(g_pc_settings.hdr_exposure, g_pc_settings.hdr_contrast, g_pc_settings.hdr_shadows,
                             g_pc_settings.hdr_saturation, g_pc_settings.hdr_highlight_saturation);
    const WinMonitorInfo target_mon = target_win_monitor(g_pc_settings.monitor);
    nx::set_native_render_size(uint32_t(target_mon.width), uint32_t(target_mon.height));
    if (g_fullscreen != g_pc_settings.fullscreen) toggle_fullscreen(g_window);
    else if (monitor_changed && g_window) {
        if (g_fullscreen) {
            RECT r{0, 0, g_pc_settings.width, g_pc_settings.height};
            AdjustWindowRectEx(&r, kWindowed, FALSE, 0);
            const int ow = r.right - r.left, oh = r.bottom - r.top;
            const int wx = target_mon.work.left + ((target_mon.work.right - target_mon.work.left) - ow) / 2;
            const int wy = target_mon.work.top + ((target_mon.work.bottom - target_mon.work.top) - oh) / 2;
            g_placement.length = sizeof g_placement;
            g_placement.rcNormalPosition = {wx, wy, wx + ow, wy + oh};
            SetWindowPos(g_window, g_pc_settings.always_on_top ? HWND_TOPMOST : HWND_NOTOPMOST,
                         target_mon.rc.left, target_mon.rc.top,
                         target_mon.rc.right - target_mon.rc.left,
                         target_mon.rc.bottom - target_mon.rc.top, SWP_FRAMECHANGED | SWP_NOACTIVATE);
        } else {
            RECT r{0, 0, g_pc_settings.width, g_pc_settings.height};
            AdjustWindowRectEx(&r, DWORD(GetWindowLongPtrW(g_window, GWL_STYLE)), FALSE,
                               DWORD(GetWindowLongPtrW(g_window, GWL_EXSTYLE)));
            const int ow = r.right - r.left, oh = r.bottom - r.top;
            const int wx = target_mon.work.left + ((target_mon.work.right - target_mon.work.left) - ow) / 2;
            const int wy = target_mon.work.top + ((target_mon.work.bottom - target_mon.work.top) - oh) / 2;
            SetWindowPos(g_window, nullptr, wx, wy, ow, oh, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    else if (size_changed && !g_fullscreen) resize_window_client(g_window, g_pc_settings.width, g_pc_settings.height);
    if (topmost_changed && g_window)
        SetWindowPos(g_window, g_pc_settings.always_on_top ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    set_game_volume();
    nx::set_audio_output(g_pc_settings.audio_mode, g_pc_settings.stereo_separation);
    if (persist) save_pc_settings();
    pc_menu_sync(g_pc_settings);
}

static void toggle_fullscreen(HWND h) {
    if (!g_fullscreen) {
        MONITORINFO mi{sizeof mi};
        GetWindowPlacement(h, &g_placement);
        GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi);
        // (SetWindowLongW applied above)
        const WinMonitorInfo mon = target_win_monitor(g_pc_settings.monitor);
        const RECT& mrc = mon.rc;
        SetWindowLongW(h, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        // (SetWindowLongW applied above)
        SetWindowPos(h, HWND_TOP, mrc.left, mrc.top, mrc.right - mrc.left,
                     mrc.bottom - mrc.top, SWP_FRAMECHANGED);
        // (target monitor rect applied above)
    } else {
        SetWindowLongW(h, GWL_STYLE, kWindowed);
        SetWindowPlacement(h, &g_placement);
        SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    g_fullscreen = !g_fullscreen;
    SetWindowPos(h, g_pc_settings.always_on_top ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    g_pc_settings.fullscreen = g_fullscreen;
    if (!g_fullscreen) resize_window_client(h, g_pc_settings.width, g_pc_settings.height);
    pc_menu_sync(g_pc_settings);
    if (overlay::enabled()) overlay::sync(to_overlay(g_pc_settings));
    save_pc_settings();
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == kOverlayApplyMessage) {
        std::unique_ptr<OverlayApply> a(reinterpret_cast<OverlayApply*>(l));
        apply_pc_menu(a->state, a->persist);
        return 0;
    }
    // The launcher is shown as an overlay screen while the F1 settings menu is closed. Mouse input still belongs
    // to that screen; checking is_open() here dropped every click until the in-game menu had been opened.
    if (overlay::enabled() && overlay::wants_mouse()) {
        switch (msg) {
            case WM_MOUSEMOVE:
                overlay::mouse_move(float(GET_X_LPARAM(l)), float(GET_Y_LPARAM(l)));
                return 0;
            case WM_LBUTTONDOWN: case WM_LBUTTONUP:
                overlay::mouse_button(0, msg == WM_LBUTTONDOWN);
                return 0;
            case WM_RBUTTONDOWN: case WM_RBUTTONUP:
                overlay::mouse_button(1, msg == WM_RBUTTONDOWN);
                return 0;
            case WM_MOUSEWHEEL:
                overlay::mouse_wheel(float(GET_WHEEL_DELTA_WPARAM(w)) / float(WHEEL_DELTA));
                return 0;
        }
    }
    if (overlay::enabled() && overlay::wants_mouse()) {
        switch (msg) {
            case WM_KEYDOWN:
            case WM_KEYUP:
                if (!overlay::capturing_key() &&
                    (w == WPARAM(g_hotkey_bindings[0].load(std::memory_order_relaxed)) ||
                     w == WPARAM(g_hotkey_bindings[1].load(std::memory_order_relaxed)) ||
                     w == WPARAM(g_hotkey_bindings[2].load(std::memory_order_relaxed))))
                    break;  // handled below
                if (msg == WM_KEYUP || !(l & (1ll << 30))) overlay::key_event(int(w), overlay_key(w), msg == WM_KEYDOWN);
                return 0;
            case WM_CHAR: {
                // WM_CHAR delivers UTF-16 code units, including surrogate pairs.
                static uint16_t high = 0;
                const uint16_t unit = uint16_t(w);
                if (unit >= 0xD800 && unit <= 0xDBFF) { high = unit; return 0; }
                if (unit >= 0xDC00 && unit <= 0xDFFF && high) {
                    overlay::text_input(0x10000u + ((uint32_t(high) - 0xD800u) << 10) + unit - 0xDC00u);
                } else if (unit < 0xD800 || unit > 0xDFFF) overlay::text_input(unit);
                high = 0;
                return 0;
            }
        }
    }
    switch (msg) {
        case WM_ACTIVATEAPP:
            g_window_focused.store(w != FALSE, std::memory_order_relaxed);
            set_game_volume();
            break;
        case WM_ACTIVATE:
            g_window_focused.store(LOWORD(w) != WA_INACTIVE, std::memory_order_relaxed);
            set_game_volume();
            break;
        case WM_MOVE:
        case WM_SIZE:
            metrics_reposition();
            set_game_volume();
            break;
        case WM_PAINT: {
            if (!g_shader_progress_visible.load(std::memory_order_acquire) || overlay::enabled())
                return DefWindowProcW(h, msg, w, l);
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(h, &ps);
            RECT client{};
            GetClientRect(h, &client);
            HBRUSH background = CreateSolidBrush(RGB(17, 22, 32));
            FillRect(dc, &client, background);
            DeleteObject(background);
            const int width = int(std::max(1L, client.right - client.left));
            const int height = int(std::max(1L, client.bottom - client.top));
            const int left = std::max(32, width / 8);
            const int right = width - left;
            const int center_y = height / 2;
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(245, 247, 250));
            HFONT font = CreateFontW(-MulDiv(22, GetDeviceCaps(dc, LOGPIXELSY), 72), 0, 0, 0, FW_SEMIBOLD,
                                     FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                     CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            HGDIOBJ old_font = SelectObject(dc, font);
            RECT title{left, center_y - 62, right, center_y - 25};
            DrawTextW(dc, L"Compiling shaders", -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            wchar_t count[96];
            const uint64_t compiled = g_shader_compiled.load(std::memory_order_relaxed);
            const uint64_t total = g_shader_total.load(std::memory_order_relaxed);
            swprintf_s(count, L"%llu / %llu compiled", (unsigned long long)compiled, (unsigned long long)total);
            SetTextColor(dc, RGB(188, 198, 214));
            RECT label{left, center_y - 15, right, center_y + 15};
            DrawTextW(dc, count, -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, old_font);
            DeleteObject(font);

            RECT bar{left, center_y + 30, right, center_y + 43};
            HBRUSH track = CreateSolidBrush(RGB(49, 59, 74));
            FillRect(dc, &bar, track);
            DeleteObject(track);
            if (total) {
                const uint64_t bounded = std::min(compiled, total);
                bar.right = bar.left + int64_t(bar.right - bar.left) * bounded / total;
                HBRUSH fill = CreateSolidBrush(RGB(54, 139, 235));
                FillRect(dc, &bar, fill);
                DeleteObject(fill);
            }
            EndPaint(h, &ps);
            return 0;
        }
        case WM_CLOSE:
            log("window closed");
            nx::nvn::flush_pipeline_cache();
            ExitProcess(0);
        case WM_KEYDOWN:
            if (!overlay::capturing_key()) {
                if (w == WPARAM(g_hotkey_bindings[0].load(std::memory_order_relaxed)) && !(l & (1ll << 30))) {
                    if (overlay::enabled())
                        overlay::toggle();
                    else
                        pc_menu_toggle();
                    return 0;
                }
                if (w == WPARAM(g_hotkey_bindings[1].load(std::memory_order_relaxed)) && !(l & (1ll << 30))) {
                    nx::nvn::request_screenshot();
                    return 0;
                }
                if (w == WPARAM(g_hotkey_bindings[2].load(std::memory_order_relaxed)) && !(l & (1ll << 30))) {
                    toggle_fullscreen(h);
                    return 0;
                }
            }
            break;
        case WM_SYSKEYDOWN:
            if (w == VK_RETURN) {  // Alt+Enter
                toggle_fullscreen(h);
                return 0;
            }
            break;
        case WM_SYSCHAR:
            if (w == VK_RETURN) return 0;  // no beep
            break;
    }
    return DefWindowProcW(h, msg, w, l);
}

// The window lives on its own thread so its message loop keeps running whatever the game threads do.
static void window_thread(std::atomic<bool>* ready) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    const std::wstring cls = widen(nx::game::kInfo.title) + L"Recomp";  // tools/capture.ps1 finds the window by it
    wc.lpszClassName = cls.c_str();
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    // The render resolution's aspect (NX_RESOLUTION, default 16:9), about 80% of the screen height, centred.
    unsigned rw = 16, rh = 9;
    if (const char* r = std::getenv("NX_RESOLUTION")) std::sscanf(r, "%ux%u", &rw, &rh);
    const WinMonitorInfo init_mon = target_win_monitor(g_pc_settings.monitor);
    int sh = init_mon.height;
    int ch = std::max(540, sh * 8 / 10), cw = int(int64_t(ch) * rw / std::max(1u, rh));
    if (g_pc_settings.width > 0 && g_pc_settings.height > 0) cw = g_pc_settings.width, ch = g_pc_settings.height;
    RECT r{0, 0, cw, ch};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    int w = r.right - r.left, h = r.bottom - r.top;
    int x = init_mon.work.left + ((init_mon.work.right - init_mon.work.left) - w) / 2;
    int y = init_mon.work.top + ((init_mon.work.bottom - init_mon.work.top) - h) / 2;
    // NX_HEADLESS: created hidden (the surface still needs a window), never brought to the front.
    const DWORD style = headless() ? (kWindowed & ~WS_VISIBLE) : kWindowed;
    g_window = CreateWindowW(wc.lpszClassName, (widen(nx::game::kInfo.title) + L" (Switch recomp)  -  F11: fullscreen").c_str(), style,
                             x, y, w, h, nullptr, nullptr, wc.hInstance, nullptr);
    const char* fullscreen_env = std::getenv("NX_FULLSCREEN");
    // RecompLauncher's Big Picture mode (RECOMPLAUNCHER_MODE=bigpicture: a controller on the couch) starts full screen
    const char* rl_mode = std::getenv("RECOMPLAUNCHER_MODE");
    const bool big_picture = rl_mode && std::strcmp(rl_mode, "bigpicture") == 0;
    if (!headless() && (g_pc_settings.fullscreen || big_picture || (fullscreen_env && *fullscreen_env == '1')))
        toggle_fullscreen(g_window);
    RECT client{};
    GetClientRect(g_window, &client);
    if (!g_pc_settings.width) g_pc_settings.width = client.right - client.left;
    if (!g_pc_settings.height) g_pc_settings.height = client.bottom - client.top;
    if (g_pc_settings.always_on_top)
        SetWindowPos(g_window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    pc_menu_init(g_window, g_pc_settings, apply_pc_menu);
    if (overlay::enabled()) {
        overlay::Host host;
        host.apply = overlay_apply;
        host.monitors = overlay_monitors;
        host.key_name = overlay_key_name;
        host.default_keyboard_bindings = PcMenuState{}.keyboard_bindings;
        host.default_hotkey_bindings = PcMenuState{}.hotkey_bindings;
        overlay::init(host, to_overlay(g_pc_settings));
    }
    metrics_init(g_window);
    apply_metrics_settings(g_pc_settings);
    if (!headless()) SetForegroundWindow(g_window);
    *ready = true;
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

void* create_window() {
    if (!g_window) {
        static std::atomic<bool> ready{false};
        std::thread(window_thread, &ready).detach();
        while (!ready) Sleep(1);
    }
    return g_window;
}

bool window_minimized() { return g_window && IsIconic(g_window); }

void shader_compile_progress(uint64_t compiled, uint64_t total, bool visible) {
    g_shader_compiled.store(compiled, std::memory_order_relaxed);
    g_shader_total.store(total, std::memory_order_relaxed);
    g_shader_progress_visible.store(visible, std::memory_order_release);
    // With the ImGui overlay the compile screen is presented through Vulkan (overlay_progress.cpp); painting the window
    // too would flicker over it.
    if (g_window && !overlay::enabled()) InvalidateRect(g_window, nullptr, TRUE);
}

void overlay_game_interval(int interval) {
    overlay::game_interval(interval);
}

void overlay_game_speed(double percent) {
    g_game_speed.store(std::clamp(percent, 0.0, 1000.0), std::memory_order_relaxed);
    overlay::game_speed(percent);
}

void overlay_presented(uint64_t now_ns) {
    overlay::presented(now_ns);
    {
        std::lock_guard lock(g_metrics_mutex);
        if (g_previous_present_ns && now_ns > g_previous_present_ns) {
            const double interval_ms = double(now_ns - g_previous_present_ns) / 1e6;
            if (interval_ms > 0.0 && interval_ms < 10000.0) {
                g_frame_intervals_ms[g_frame_interval_next] = interval_ms;
                g_frame_interval_next = (g_frame_interval_next + 1) % kFrameHistory;
                g_frame_interval_count = std::min(kFrameHistory, g_frame_interval_count + 1);
            }
        }
        g_previous_present_ns = now_ns;
    }
    if (metrics_has_content()) {
        if (HWND overlay = g_metrics_window.load(std::memory_order_acquire)) InvalidateRect(overlay, nullptr, FALSE);
    }
}

void window_size(uint32_t& w, uint32_t& h) {
    ensure_dpi_aware();
    if (g_window) {
        RECT r{};
        GetClientRect(g_window, &r);
        w = uint32_t(r.right - r.left);
        h = uint32_t(r.bottom - r.top);
        return;
    }

    // Game hooks can ask for the output size before nnMain creates the HWND. Use the size the
    // PC menu loaded from pc-settings.ini so game-side camera setup starts with the right aspect.
    if (g_pc_settings.fullscreen) {
        const WinMonitorInfo mon = target_win_monitor(g_pc_settings.monitor);
        w = uint32_t(mon.width);
        h = uint32_t(mon.height);
        // (monitor height applied above)
    } else if (g_pc_settings.width > 0 && g_pc_settings.height > 0) {
        w = uint32_t(g_pc_settings.width);
        h = uint32_t(g_pc_settings.height);
    } else {
        unsigned rw = 16, rh = 9;
        if (const char* resolution = std::getenv("NX_RESOLUTION"))
            std::sscanf(resolution, "%ux%u", &rw, &rh);
        const int screen_h = GetSystemMetrics(SM_CYSCREEN);
        h = uint32_t(std::max(540, screen_h * 8 / 10));
        w = uint32_t(int64_t(h) * rw / std::max(1u, rh));
    }
}

void screen_size(uint32_t& w, uint32_t& h) { window_size(w, h); }

// ---- input: XInput / SDL gamepad + per-action keyboard bindings -----------------------------------------------

static bool key(int vk) {
    return vk > 0 && g_window && GetForegroundWindow() == g_window && (GetAsyncKeyState(vk) & 0x8000);
}
static std::array<bool, kPcControlCount> g_previous_controller_state{};
static std::atomic<bool> g_fast_forward_down{false};
static void ensure_input_poller();

bool fast_forward() {
    ensure_input_poller();
    return g_fast_forward_down.load(std::memory_order_relaxed);
}
bool paused() {
    return g_pause_in_background.load(std::memory_order_relaxed) && (!g_window_focused.load(std::memory_order_relaxed) || window_minimized());
}

bool headless() {  // NX_HEADLESS (platform.h)
    static const bool on = std::getenv("NX_HEADLESS") != nullptr;
    return on;
}

// SDL owns each physical gamepad once, including Xbox/XInput and HIDAPI controllers. Joy-Cons come one by one
// (SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS=0), upright (SDL_HINT_JOYSTICK_HIDAPI_VERTICAL_JOY_CONS=1), Switch 1
// ones from SDL's HIDAPI driver and Switch 2 ones from it over USB or from switch2/ over Bluetooth LE in the same
// layout; the Joy-Con manager below makes controllers of them as a Switch does:
// - a left and a right Joy-Con, both upright, play together as one controller (paired in the order they came);
// - SL and SR of a Joy-Con held together for a second: it leaves its pair and plays alone, held sideways (its own
//   player); the same again turns it upright again;
// - L of a sideways left Joy-Con and R of a sideways right one held together for a second: the two pair again.
// Up to kMaxDevices devices make up to 4 controllers (the "slots" the player assignment and bindings use).
constexpr size_t kMaxDevices = 8;
constexpr uint64_t kJoyConGestureMs = 1000;
static std::mutex g_gamepad_lock;
static std::array<SDL_Gamepad*, kMaxDevices> g_gamepads{};
static uint64_t g_last_scan_ms = 0;
static std::array<std::atomic<uint64_t>, 4> g_pending_rumble{};
struct PlayerInputs {
    std::array<PadState, 4> players{};
    std::array<bool, 4> connected{};
    std::array<int, 4> slots{{-1, -1, -1, -1}};
    std::array<int, 4> styles{};  // 0 unknown, 1 Nintendo, 2 Xbox, 3 PlayStation
    std::array<std::string, 4> names{};
};

enum class JoyCon { None, Left, Right };
struct DeviceState {
    SDL_JoystickID id = 0;    // the device this state is for (another id in the slot: a fresh state)
    JoyCon side = JoyCon::None;
    bool sideways = false;    // played alone, held sideways
    int partner = -1;         // the device slot of the other Joy-Con of its pair
    uint64_t rails_since = 0; // SL + SR held since (ms), 0 not held
    bool rails_done = false;  // that hold has acted: nothing more until they are let go
    uint64_t join_since = 0;  // its join button (L of a left Joy-Con, R of a right one) held since, while sideways
    int player_shown = -2;    // the player index last given to SDL (player lights)
};
static std::array<DeviceState, kMaxDevices> g_devices{};
// What each controller slot is made of, for rumble: up to two devices, and which side of the game's two actuators
// each plays (0 left, 1 right, 2 the stronger of both: a Joy-Con alone, 3 both: a full controller)
struct SlotMembers {
    std::array<int, 2> device{{-1, -1}};
    std::array<int, 2> role{{3, 3}};
    std::array<SDL_JoystickID, 2> id{};
};
static std::array<SlotMembers, 4> g_slot_members{};  // under g_gamepad_lock

static JoyCon joycon_side(SDL_Gamepad* pad) {
    switch (pad ? SDL_GetGamepadType(pad) : SDL_GAMEPAD_TYPE_UNKNOWN) {
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT: return JoyCon::Left;
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: return JoyCon::Right;
        default: return JoyCon::None;
    }
}

static int detect_gamepad_style(SDL_Gamepad* pad) {
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

static void scan_gamepads_locked() {
    SDL_UpdateGamepads();
    bool any_connected = false;
    for (size_t slot = 0; slot < g_gamepads.size(); ++slot) {
        auto& pad = g_gamepads[slot];
        if (pad && !SDL_GamepadConnected(pad)) {
            log("hid: gamepad slot %zu disconnected", slot);
            SDL_CloseGamepad(pad);
            pad = nullptr;
        }
        if (pad) any_connected = true;
    }
    const uint64_t now = SDL_GetTicks();
    const uint64_t interval_ms = any_connected ? 2000 : 1000;
    if (g_last_scan_ms && now - g_last_scan_ms < interval_ms) return;
    g_last_scan_ms = now;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count; ++i) {
        bool known = false;
        for (auto* pad : g_gamepads) if (pad && SDL_GetGamepadID(pad) == ids[i]) known = true;
        if (known) continue;
        for (size_t slot = 0; slot < g_gamepads.size(); ++slot) {
            if (!g_gamepads[slot]) {
                g_gamepads[slot] = SDL_OpenGamepad(ids[i]);
                if (g_gamepads[slot]) {
                    const char* name = SDL_GetGamepadName(g_gamepads[slot]);
                    log("hid: gamepad slot %zu connected: %s (type %d)", slot,
                        name ? name : "Controller", int(SDL_GetGamepadType(g_gamepads[slot])));
                }
                break;
            }
        }
    }
    SDL_free(ids);
}

// One device's buttons and axes as SDL reports them
struct RawPad {
    std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> b{};
    std::array<int, SDL_GAMEPAD_AXIS_COUNT> a{};
};
static RawPad read_raw(SDL_Gamepad* pad) {
    RawPad r;
    for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; ++i) r.b[size_t(i)] = SDL_GetGamepadButton(pad, SDL_GamepadButton(i));
    for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; ++i) r.a[size_t(i)] = SDL_GetGamepadAxis(pad, SDL_GamepadAxis(i));
    return r;
}
static int stick_value(int value, size_t slot, bool right) {  // the dead zone, then -kStickMax..kStickMax (no inversion)
    const auto& atom = (right ? g_pad_right_deadzone : g_pad_left_deadzone)[slot];
    const int dz = (std::clamp(atom.load(std::memory_order_relaxed), 0, 34) * kStickMax) / 100;
    const int av = std::min(std::abs(value), kStickMax);
    if (av <= dz) return 0;
    const int mag = ((av - dz) * kStickMax) / (kStickMax - dz);
    return value < 0 ? -mag : mag;
}
static bool trigger_down(int value) { return value > 12000; }
// SL and SR (SDL's paddles for a Joy-Con: left SL / SR = left paddle 1 / 2, right SR / SL = right paddle 1 / 2)
static bool rails_held(const RawPad& r, JoyCon side) {
    return side == JoyCon::Left ? r.b[SDL_GAMEPAD_BUTTON_LEFT_PADDLE1] && r.b[SDL_GAMEPAD_BUTTON_LEFT_PADDLE2]
                                : r.b[SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1] && r.b[SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2];
}
static bool join_held(const RawPad& r, JoyCon side) {
    return side == JoyCon::Left ? r.b[SDL_GAMEPAD_BUTTON_LEFT_SHOULDER] && !r.b[SDL_GAMEPAD_BUTTON_LEFT_PADDLE1] &&
                                      !r.b[SDL_GAMEPAD_BUTTON_LEFT_PADDLE2]
                                : r.b[SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER] && !r.b[SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1] &&
                                      !r.b[SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2];
}

// A controller made of one or two devices: PcPhysicalButton presses and sticks (y up)
struct ControllerInput {
    bool down[kPcControlCount]{};
    int lx = 0, ly = 0, rx = 0, ry = 0;
};
static void add_full(ControllerInput& c, const RawPad& r, size_t slot) {
    c.down[PcSouth] = r.b[SDL_GAMEPAD_BUTTON_SOUTH], c.down[PcEast] = r.b[SDL_GAMEPAD_BUTTON_EAST];
    c.down[PcWest] = r.b[SDL_GAMEPAD_BUTTON_WEST], c.down[PcNorth] = r.b[SDL_GAMEPAD_BUTTON_NORTH];
    c.down[PcShoulderL] = r.b[SDL_GAMEPAD_BUTTON_LEFT_SHOULDER], c.down[PcShoulderR] = r.b[SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER];
    c.down[PcTriggerL] = trigger_down(r.a[SDL_GAMEPAD_AXIS_LEFT_TRIGGER]);
    c.down[PcTriggerR] = trigger_down(r.a[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER]);
    c.down[PcStart] = r.b[SDL_GAMEPAD_BUTTON_START], c.down[PcBack] = r.b[SDL_GAMEPAD_BUTTON_BACK];
    c.down[PcStickL] = r.b[SDL_GAMEPAD_BUTTON_LEFT_STICK], c.down[PcStickR] = r.b[SDL_GAMEPAD_BUTTON_RIGHT_STICK];
    c.down[PcLeft] = r.b[SDL_GAMEPAD_BUTTON_DPAD_LEFT], c.down[PcUp] = r.b[SDL_GAMEPAD_BUTTON_DPAD_UP];
    c.down[PcRight] = r.b[SDL_GAMEPAD_BUTTON_DPAD_RIGHT], c.down[PcDown] = r.b[SDL_GAMEPAD_BUTTON_DPAD_DOWN];
    c.lx = stick_value(r.a[SDL_GAMEPAD_AXIS_LEFTX], slot, false), c.ly = -stick_value(r.a[SDL_GAMEPAD_AXIS_LEFTY], slot, false);
    c.rx = stick_value(r.a[SDL_GAMEPAD_AXIS_RIGHTX], slot, true), c.ry = -stick_value(r.a[SDL_GAMEPAD_AXIS_RIGHTY], slot, true);
}
// An upright Joy-Con: its half of a full controller
static void add_upright(ControllerInput& c, const RawPad& r, JoyCon side, size_t slot) {
    if (side == JoyCon::Left) {
        c.down[PcShoulderL] = r.b[SDL_GAMEPAD_BUTTON_LEFT_SHOULDER];
        c.down[PcTriggerL] = trigger_down(r.a[SDL_GAMEPAD_AXIS_LEFT_TRIGGER]);
        c.down[PcBack] = r.b[SDL_GAMEPAD_BUTTON_BACK], c.down[PcStickL] = r.b[SDL_GAMEPAD_BUTTON_LEFT_STICK];
        c.down[PcLeft] = r.b[SDL_GAMEPAD_BUTTON_DPAD_LEFT], c.down[PcUp] = r.b[SDL_GAMEPAD_BUTTON_DPAD_UP];
        c.down[PcRight] = r.b[SDL_GAMEPAD_BUTTON_DPAD_RIGHT], c.down[PcDown] = r.b[SDL_GAMEPAD_BUTTON_DPAD_DOWN];
        c.lx = stick_value(r.a[SDL_GAMEPAD_AXIS_LEFTX], slot, false), c.ly = -stick_value(r.a[SDL_GAMEPAD_AXIS_LEFTY], slot, false);
    } else {
        c.down[PcSouth] = r.b[SDL_GAMEPAD_BUTTON_SOUTH], c.down[PcEast] = r.b[SDL_GAMEPAD_BUTTON_EAST];
        c.down[PcWest] = r.b[SDL_GAMEPAD_BUTTON_WEST], c.down[PcNorth] = r.b[SDL_GAMEPAD_BUTTON_NORTH];
        c.down[PcShoulderR] = r.b[SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER];
        c.down[PcTriggerR] = trigger_down(r.a[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER]);
        c.down[PcStart] = r.b[SDL_GAMEPAD_BUTTON_START], c.down[PcStickR] = r.b[SDL_GAMEPAD_BUTTON_RIGHT_STICK];
        c.rx = stick_value(r.a[SDL_GAMEPAD_AXIS_RIGHTX], slot, true), c.ry = -stick_value(r.a[SDL_GAMEPAD_AXIS_RIGHTY], slot, true);
    }
}
// A Joy-Con held sideways (rail up, SL on the left): a small full controller, as a Switch plays it. The left one is
// turned a quarter turn counter-clockwise from upright, the right one clockwise; its buttons are named by where they
// are now (the bottom one is south), SL / SR are L / R, its stick is the left stick, Minus / Plus is Plus.
static void add_sideways(ControllerInput& c, const RawPad& r, JoyCon side, size_t slot) {
    if (side == JoyCon::Left) {
        c.down[PcSouth] = r.b[SDL_GAMEPAD_BUTTON_DPAD_LEFT], c.down[PcEast] = r.b[SDL_GAMEPAD_BUTTON_DPAD_DOWN];
        c.down[PcWest] = r.b[SDL_GAMEPAD_BUTTON_DPAD_UP], c.down[PcNorth] = r.b[SDL_GAMEPAD_BUTTON_DPAD_RIGHT];
        c.down[PcShoulderL] = r.b[SDL_GAMEPAD_BUTTON_LEFT_PADDLE1];   // SL
        c.down[PcShoulderR] = r.b[SDL_GAMEPAD_BUTTON_LEFT_PADDLE2];   // SR
        c.down[PcStart] = r.b[SDL_GAMEPAD_BUTTON_BACK], c.down[PcStickL] = r.b[SDL_GAMEPAD_BUTTON_LEFT_STICK];
        // upright x right / y up (SDL y down) turned: right is upright down, up is upright right
        c.lx = stick_value(r.a[SDL_GAMEPAD_AXIS_LEFTY], slot, false), c.ly = stick_value(r.a[SDL_GAMEPAD_AXIS_LEFTX], slot, false);
    } else {
        c.down[PcSouth] = r.b[SDL_GAMEPAD_BUTTON_EAST], c.down[PcEast] = r.b[SDL_GAMEPAD_BUTTON_NORTH];
        c.down[PcWest] = r.b[SDL_GAMEPAD_BUTTON_SOUTH], c.down[PcNorth] = r.b[SDL_GAMEPAD_BUTTON_WEST];
        c.down[PcShoulderL] = r.b[SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2];  // SL
        c.down[PcShoulderR] = r.b[SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1];  // SR
        c.down[PcStart] = r.b[SDL_GAMEPAD_BUTTON_START], c.down[PcStickL] = r.b[SDL_GAMEPAD_BUTTON_RIGHT_STICK];
        // right is upright up, up is upright left
        c.lx = -stick_value(r.a[SDL_GAMEPAD_AXIS_RIGHTY], slot, true), c.ly = -stick_value(r.a[SDL_GAMEPAD_AXIS_RIGHTX], slot, true);
    }
}

static std::string device_name(SDL_Gamepad* pad) {
    const char* name = pad ? SDL_GetGamepadName(pad) : nullptr;
    return name && *name ? name : "Controller";
}
static void joycon_feedback(size_t slot) {  // a short buzz on the device that changed
    if (g_gamepads[slot]) SDL_RumbleGamepad(g_gamepads[slot], 0x5000, 0x7000, 150);
}

// SL + SR held (sideways / upright) and L + R on two sideways Joy-Cons (a pair again); pairs of upright Joy-Cons
static void update_joycons_locked(const std::array<RawPad, kMaxDevices>& raw, uint64_t now) {
    for (size_t i = 0; i < kMaxDevices; ++i) {
        DeviceState& d = g_devices[i];
        SDL_Gamepad* pad = g_gamepads[i];
        const SDL_JoystickID id = pad ? SDL_GetGamepadID(pad) : 0;
        if (d.id != id) {
            if (d.partner >= 0 && size_t(d.partner) < kMaxDevices && g_devices[size_t(d.partner)].partner == int(i))
                g_devices[size_t(d.partner)].partner = -1;
            d = DeviceState{};
            d.id = id;
            d.side = joycon_side(pad);
        }
    }
    for (size_t i = 0; i < kMaxDevices; ++i) {
        DeviceState& d = g_devices[i];
        if (d.side == JoyCon::None) continue;
        if (rails_held(raw[i], d.side)) {
            if (!d.rails_since) d.rails_since = now;
            if (!d.rails_done && now - d.rails_since >= kJoyConGestureMs) {
                d.rails_done = true;
                d.sideways = !d.sideways;
                if (d.partner >= 0) g_devices[size_t(d.partner)].partner = -1, d.partner = -1;
                const std::string name = device_name(g_gamepads[i]);
                log("hid: %s (slot %zu) %s", name.c_str(), i, d.sideways ? "held sideways, plays alone" : "upright again");
                overlay::notify((name + (d.sideways ? ": sideways, plays alone" : ": upright")).c_str());
                joycon_feedback(i);
            }
        } else {
            d.rails_since = 0, d.rails_done = false;
        }
        d.join_since = d.sideways && join_held(raw[i], d.side) ? (d.join_since ? d.join_since : now) : 0;
    }
    // L + R held on a sideways left and a sideways right Joy-Con for a second: they pair again
    for (size_t l = 0; l < kMaxDevices; ++l) {
        DeviceState& a = g_devices[l];
        if (a.side != JoyCon::Left || !a.join_since || now - a.join_since < kJoyConGestureMs) continue;
        for (size_t r = 0; r < kMaxDevices; ++r) {
            DeviceState& b = g_devices[r];
            if (b.side != JoyCon::Right || !b.join_since || now - b.join_since < kJoyConGestureMs) continue;
            a.sideways = b.sideways = false;
            a.partner = int(r), b.partner = int(l);
            a.join_since = b.join_since = 0;
            log("hid: Joy-Cons in slots %zu and %zu paired again", l, r);
            overlay::notify("Joy-Cons paired: one controller");
            joycon_feedback(l), joycon_feedback(r);
            break;
        }
    }
    // Upright Joy-Cons without a partner: a left and a right make a pair, in the order they came
    for (size_t l = 0; l < kMaxDevices; ++l) {
        DeviceState& a = g_devices[l];
        if (a.side != JoyCon::Left || a.sideways || a.partner >= 0) continue;
        for (size_t r = 0; r < kMaxDevices; ++r) {
            DeviceState& b = g_devices[r];
            if (b.side != JoyCon::Right || b.sideways || b.partner >= 0) continue;
            a.partner = int(r), b.partner = int(l);
            log("hid: Joy-Cons in slots %zu and %zu paired", l, r);
            break;
        }
    }
}

static PlayerInputs sample_players_uncached() {
    PlayerInputs result;
    std::array<PadState, 4> physical{};
    std::array<bool, 4> present{};
    std::array<SlotMembers, 4> members{};
    {
        std::lock_guard lk(g_gamepad_lock);
        scan_gamepads_locked();
        // The game's rumble for each controller slot, to the devices it was made of
        for (size_t slot = 0; slot < 4; ++slot) {
            const uint64_t req = g_pending_rumble[slot].exchange(0, std::memory_order_acq_rel);
            if (!(req >> 32)) continue;
            for (int dev : g_slot_members[slot].device)
                if (dev >= 0 && g_gamepads[size_t(dev)] && !dualsense::owns(SDL_GetGamepadID(g_gamepads[size_t(dev)])))
                    SDL_RumbleGamepad(g_gamepads[size_t(dev)], Uint16((req >> 16) & 0xFFFFu), Uint16(req & 0xFFFFu), 250);
        }
        std::array<RawPad, kMaxDevices> raw{};
        const bool ignore_pad = g_ignore_pad_in_background.load(std::memory_order_relaxed) &&
                                !g_window_focused.load(std::memory_order_relaxed);
        if (!ignore_pad) {
            for (size_t i = 0; i < kMaxDevices; ++i)
                if (g_gamepads[i]) raw[i] = read_raw(g_gamepads[i]);
        }
        update_joycons_locked(raw, SDL_GetTicks());
        // Controller slots, in device order: a device, or a Joy-Con pair at its first device's place
        size_t slot = 0;
        for (size_t i = 0; i < kMaxDevices && slot < 4; ++i) {
            SDL_Gamepad* pad = g_gamepads[i];
            if (!pad) continue;
            const DeviceState& d = g_devices[i];
            if (d.side != JoyCon::None && d.partner >= 0 && size_t(d.partner) < i && g_gamepads[size_t(d.partner)])
                continue;  // in its partner's slot
            ControllerInput in;
            SlotMembers& m = members[slot];
            m.device[0] = int(i), m.id[0] = SDL_GetGamepadID(pad);
            if (d.side == JoyCon::None) {
                add_full(in, raw[i], slot);
                result.names[slot] = device_name(pad);
                result.styles[slot] = detect_gamepad_style(pad);
                m.role[0] = 3;
            } else if (d.sideways) {
                add_sideways(in, raw[i], d.side, slot);
                result.names[slot] = device_name(pad) + " (sideways)";
                result.styles[slot] = 1;
                m.role[0] = 2;
            } else {
                add_upright(in, raw[i], d.side, slot);
                result.names[slot] = device_name(pad);
                result.styles[slot] = 1;
                m.role[0] = d.side == JoyCon::Left ? 0 : 1;
                if (d.partner >= 0 && g_gamepads[size_t(d.partner)]) {
                    const size_t p = size_t(d.partner);
                    ControllerInput other;
                    add_upright(other, raw[p], g_devices[p].side, slot);
                    for (size_t k = 0; k < kPcControlCount; ++k) in.down[k] = in.down[k] || other.down[k];
                    if (other.lx || other.ly) in.lx = other.lx, in.ly = other.ly;
                    if (other.rx || other.ry) in.rx = other.rx, in.ry = other.ry;
                    m.device[1] = int(p), m.id[1] = SDL_GetGamepadID(g_gamepads[p]);
                    m.role[1] = g_devices[p].side == JoyCon::Left ? 0 : 1;
                    const std::string a = device_name(pad), b = device_name(g_gamepads[p]);
                    result.names[slot] = a.find("Joy-Con 2") != std::string::npos && b.find("Joy-Con 2") != std::string::npos
                                             ? "Joy-Con 2 (L/R)"
                                             : "Joy-Con (L/R)";
                } else {
                    m.role[0] = 2;  // alone, upright: both actuators' values on it
                }
            }
            present[slot] = true;
            if (slot == 0) {
                for (size_t k = 0; k < kPcControlCount; ++k) {
                    if (pc_menu_open() && in.down[k] && !g_previous_controller_state[k]) pc_menu_capture_controller_button(int(k));
                    g_previous_controller_state[k] = in.down[k];
                }
            }
            for (size_t action = 0; action < kPcControlCount; ++action) {
                const int bind = g_device_bindings[slot][action].load();
                if (bind >= 0 && bind < int(kPcControlCount) && in.down[bind]) physical[slot].buttons |= kPadActionBits[action];
            }
            physical[slot].lx = in.lx, physical[slot].ly = in.ly;
            physical[slot].rx = in.rx, physical[slot].ry = in.ry;
            ++slot;
        }
        g_slot_members = members;
    }
    PadState keyboard{};
    const bool focused = g_window && GetForegroundWindow() == g_window;
    auto key_focused = [focused](int vk) { return focused && vk > 0 && (GetAsyncKeyState(vk) & 0x8000); };
    g_fast_forward_down.store(key_focused(g_hotkey_bindings[3].load(std::memory_order_relaxed)), std::memory_order_relaxed);
    if (focused && !overlay::wants_keyboard() && !pc_menu_open()) {
        for (size_t action = 0; action < kPcControlCount; ++action) {
            if (!key_focused(g_keyboard_bindings[action].load())) continue;
            keyboard.buttons |= kPadActionBits[action];
            if (action == PcDpadLeft) keyboard.lx = -kStickMax;
            else if (action == PcDpadRight) keyboard.lx = kStickMax;
            else if (action == PcDpadUp) keyboard.ly = kStickMax;
            else if (action == PcDpadDown) keyboard.ly = -kStickMax;
        }
        if (key_focused('A')) keyboard.lx = -kStickMax;
        if (key_focused('D')) keyboard.lx = kStickMax;
        if (key_focused('W')) keyboard.ly = kStickMax;
        if (key_focused('S')) keyboard.ly = -kStickMax;
    }
    std::array<int, 4> assignment{};
    bool manual = false;
    for (size_t i = 0; i < 4; ++i) { assignment[i] = g_player_devices[i].load(); manual |= assignment[i] != 0; }
    if (!manual) {
        result.players[0] = keyboard;
        result.connected[0] = true;
        const bool separate = g_keyboard_separate.load();
        size_t next_player = separate ? 1 : 0;
        for (size_t slot = 0; slot < 4 && next_player < 4; ++slot) {
            if (!present[slot]) continue;
            const size_t player = next_player++;
            auto& dest = result.players[player];
            dest.buttons |= physical[slot].buttons;
            if (physical[slot].lx) dest.lx = physical[slot].lx;
            if (physical[slot].ly) dest.ly = physical[slot].ly;
            if (physical[slot].rx) dest.rx = physical[slot].rx;
            if (physical[slot].ry) dest.ry = physical[slot].ry;
            result.connected[player] = true;
            result.slots[player] = int(slot);
        }
    } else {
        bool used[7]{};
        for (int a : assignment) if (a >= 2 && a <= 6) used[a] = true;
        for (size_t player = 0; player < 4; ++player) {
            int a = assignment[player];
            if (!a) {
                if (!used[2]) a = 2;
                else for (int candidate = 3; candidate <= 6; ++candidate)
                    if (!used[candidate] && present[candidate - 3]) { a = candidate; break; }
                if (a >= 2) used[a] = true;
            }
            if (a == 2) result.players[player] = keyboard, result.connected[player] = true;
            else if (a >= 3 && a <= 6 && present[a - 3]) {
                result.players[player] = physical[a - 3];
                result.connected[player] = true;
                result.slots[player] = a - 3;
            }
        }
    }
    // Player lights: each device shows the player its controller plays as (Joy-Cons, Pro Controllers)
    {
        std::array<int, kMaxDevices> want;
        want.fill(-1);
        for (size_t player = 0; player < 4; ++player) {
            const int slot = result.slots[player];
            if (slot < 0 || slot >= 4) continue;
            for (int dev : members[size_t(slot)].device)
                if (dev >= 0) want[size_t(dev)] = int(player);
        }
        std::lock_guard lk(g_gamepad_lock);
        for (size_t i = 0; i < kMaxDevices; ++i)
            if (g_gamepads[i] && g_devices[i].player_shown != want[i]) {
                g_devices[i].player_shown = want[i];
                SDL_SetGamepadPlayerIndex(g_gamepads[i], want[i]);
            }
    }
    return result;
}

static std::mutex g_player_sample_lock;
static PlayerInputs g_cached_players = [] {
    PlayerInputs init;
    init.connected[0] = true;
    return init;
}();

static void ensure_input_poller() {
    static std::once_flag once;
    std::call_once(once, [] {
        static std::atomic<bool> ready{false};
        std::thread([] {
            set_thread_name("win-input");
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
            // Joy-Cons one by one and upright: the Joy-Con manager pairs them or turns them sideways (above)
            SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS, "0");
            SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_VERTICAL_JOY_CONS, "1");
            if (!SDL_Init(SDL_INIT_GAMEPAD))
                log("hid: SDL gamepad input unavailable: %s", SDL_GetError());
            else
                switch2_start();  // Switch 2 controllers over Bluetooth LE (NX_SWITCH2=0: off)
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {}
            PlayerInputs first = sample_players_uncached();
            {
                std::lock_guard lk(g_player_sample_lock);
                g_cached_players = std::move(first);
            }
            ready.store(true, std::memory_order_release);
            for (;;) {
                while (SDL_PollEvent(&ev)) {
                    if (ev.type == SDL_EVENT_GAMEPAD_ADDED || ev.type == SDL_EVENT_GAMEPAD_REMOVED ||
                        ev.type == SDL_EVENT_JOYSTICK_ADDED || ev.type == SDL_EVENT_JOYSTICK_REMOVED) {
                        std::lock_guard glk(g_gamepad_lock);
                        g_last_scan_ms = 0;
                    }
                }
                PlayerInputs next = sample_players_uncached();
                {
                    std::lock_guard lk(g_player_sample_lock);
                    for (size_t i = 0; i < 4; ++i)
                        if (g_cached_players.connected[i] != next.connected[i])
                            log("hid: player %zu %s", i + 1, next.connected[i] ? "connected" : "disconnected");
                    g_cached_players = std::move(next);
                }
                sleep_ns(2'000'000ll);
            }
        }).detach();
        while (!ready.load(std::memory_order_acquire)) Sleep(1);
    });
}

static PlayerInputs sample_players() {
    ensure_input_poller();
    std::lock_guard lk(g_player_sample_lock);
    return g_cached_players;
}

std::array<std::string, 4> controller_names() {
    return sample_players().names;
}
bool player_connected(unsigned player) {
    if (player >= 4) return false;
    return sample_players().connected[player];
}
void rumble_player(unsigned player, float low, float high) {
    if (player >= 4) return;
    const int slot = sample_players().slots[player];
    if (slot < 0 || slot >= 4) return;
    const float strength = float(std::clamp(g_pc_settings.rumble, 0, 100)) / 100.0f;
    const uint32_t low_u16 = Uint16(std::clamp(low * strength, 0.0f, 1.0f) * 65535.0f);
    const uint32_t high_u16 = Uint16(std::clamp(high * strength, 0.0f, 1.0f) * 65535.0f);
    g_pending_rumble[size_t(slot)].store((1ull << 32) | (uint64_t(low_u16) << 16) | uint64_t(high_u16),
                                         std::memory_order_release);
}
// HD rumble (the game's amplitude and frequency per band, left and right actuator) for the Switch 2 controllers of
// that player's controller, which play it as it is (switch2/switch2.h); every other device keeps rumble_player's.
void rumble_player_hd(unsigned player, const float left[4], const float right[4]) {
    if (player >= 4) return;
    const int slot = sample_players().slots[player];
    if (slot < 0 || slot >= 4) return;
    SlotMembers m;
    {
        std::lock_guard lk(g_gamepad_lock);
        m = g_slot_members[size_t(slot)];
        // A DualSense's voice coils (renhdrum.dll): the game's values as they are, the strength comes after its curve
        for (size_t k = 0; k < 2; ++k) {
            SDL_Gamepad* pad = m.device[k] >= 0 ? g_gamepads[size_t(m.device[k])] : nullptr;
            if (!pad || SDL_GetGamepadType(pad) != SDL_GAMEPAD_TYPE_PS5) continue;
            if (SDL_GetGamepadConnectionState(pad) == SDL_JOYSTICK_CONNECTION_WIRELESS) continue;  // Bluetooth: the plain rumble
            dualsense::play(uint32_t(m.id[k]), left, right, g_pc_settings.rumble);
        }
    }
    const float strength = float(std::clamp(g_pc_settings.rumble, 0, 100)) / 100.0f;
    float l[4], r[4], strong[4];
    for (int k = 0; k < 4; ++k) {
        const bool amp = k == 0 || k == 2;
        l[k] = amp ? std::clamp(left[k] * strength, 0.0f, 1.0f) : left[k];
        r[k] = amp ? std::clamp(right[k] * strength, 0.0f, 1.0f) : right[k];
    }
    for (int band = 0; band < 4; band += 2) {  // the stronger side in each band, with its frequency
        const bool use_left = l[band] >= r[band];
        strong[band] = use_left ? l[band] : r[band];
        strong[band + 1] = use_left ? l[band + 1] : r[band + 1];
    }
    for (size_t k = 0; k < 2; ++k) {
        if (m.device[k] < 0 || switch2_kind(uint32_t(m.id[k])) == SWITCH2_NONE) continue;
        switch (m.role[k]) {
            case 0: switch2_hd_rumble(uint32_t(m.id[k]), l, l); break;
            case 1: switch2_hd_rumble(uint32_t(m.id[k]), r, r); break;
            case 2: switch2_hd_rumble(uint32_t(m.id[k]), strong, strong); break;
            default: switch2_hd_rumble(uint32_t(m.id[k]), l, r); break;
        }
    }
}
void rumble(float low, float high) { rumble_player(0, low, high); }
void read_pad_player(unsigned player, PadState& p) {
    if (player >= 4) return;
    const PlayerInputs inputs = sample_players();
    if (inputs.connected[player]) p = inputs.players[player];
    if (player == 0) {
        input_script(p);
        if (overlay::enabled()) overlay::filter_pad(p);
        if (pc_menu_open()) p = {};
    } else if (overlay::is_open() || overlay::wants_keyboard() || pc_menu_open()) p = {};
}
ButtonPromptConfig button_prompt_config() {
    const PlayerInputs inputs = sample_players();
    const int assigned = g_player_devices[0].load();
    int slot = assigned >= 3 && assigned <= 6 ? assigned - 3 : (inputs.slots[0] >= 0 ? inputs.slots[0] : 0);
    slot = std::clamp(slot, 0, 3);
    ButtonPromptConfig cfg;
    for (size_t a = 0; a < kPcControlCount; ++a) cfg.bindings[a] = g_device_bindings[slot][a].load();
    const int pref = g_button_icons.load();
    if (pref >= 1 && pref <= 3) {
        cfg.style = pref;
    } else if (inputs.styles[slot] >= 1 && inputs.styles[slot] <= 3) {
        cfg.style = inputs.styles[slot];
    } else {
        int found = 0;
        for (int s : inputs.styles) {
            if (s >= 1 && s <= 3) {
                found = s;
                break;
            }
        }
        cfg.style = found ? found : (g_controller_preset.load() == 1 ? 1 : 2);
    }
    return cfg;
}
void read_pad(PadState& p) {
    PadState mapped{};
    read_pad_player(0, mapped);
    p.buttons |= mapped.buttons;
    if (mapped.lx) p.lx = mapped.lx;
    if (mapped.ly) p.ly = mapped.ly;
    if (mapped.rx) p.rx = mapped.rx;
    if (mapped.ry) p.ry = mapped.ry;
}

// ---- audio: XAudio2 -------------------------------------------------------------------------------------------

namespace {

std::atomic<uint32_t> g_queued{0};
AudioDone g_done = nullptr;

struct Voice : IXAudio2VoiceCallback {
    void STDMETHODCALLTYPE OnBufferEnd(void* ctx) noexcept override {
        auto* p = static_cast<std::pair<void*, std::vector<uint8_t>>*>(ctx);
        g_queued--;
        g_done(p->first);
        delete p;
    }
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) noexcept override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() noexcept override {}
    void STDMETHODCALLTYPE OnStreamEnd() noexcept override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) noexcept override {}
    void STDMETHODCALLTYPE OnLoopEnd(void*) noexcept override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) noexcept override {}
};

IXAudio2* g_xa = nullptr;
IXAudio2SourceVoice* g_voice = nullptr;
Voice g_callback;

}  // namespace

bool audio_open(AudioDone done, int channels) {
    g_done = done;
    // Headless test runs (and NX_MUTE=1) are silent: no device, the callers pace themselves with a timer.
    if (headless() || std::getenv("NX_MUTE")) return false;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(XAudio2Create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR))) return false;
    IXAudio2MasteringVoice* master = nullptr;
    if (FAILED(g_xa->CreateMasteringVoice(&master))) return false;
    g_master_voice = master;
    set_game_volume();
    if (channels == 6) {
        WAVEFORMATEXTENSIBLE wfx{};
        wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wfx.Format.nChannels = 6;
        wfx.Format.nSamplesPerSec = 48000;
        wfx.Format.wBitsPerSample = 16;
        wfx.Format.nBlockAlign = 12;
        wfx.Format.nAvgBytesPerSec = 48000 * 12;
        wfx.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        wfx.Samples.wValidBitsPerSample = 16;
        wfx.dwChannelMask = 0x3F;  // FL | FR | FC | LFE | BL | BR (5.1)
        wfx.SubFormat = GUID{0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
        return SUCCEEDED(g_xa->CreateSourceVoice(&g_voice, &wfx.Format, 0, 2.0f, &g_callback)) &&
               SUCCEEDED(g_voice->Start());
    }
    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = 48000;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = 48000 * 4;
    if (FAILED(g_xa->CreateSourceVoice(&g_voice, &wf, 0, 2.0f, &g_callback))) return false;
    g_voice->Start();
    return true;
}

bool audio_submit(const void* data, size_t bytes, void* ctx) {
    if (!g_voice) return false;
    auto* p = new std::pair<void*, std::vector<uint8_t>>(
        ctx, std::vector<uint8_t>(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + bytes));
    XAUDIO2_BUFFER xb{};
    xb.AudioBytes = UINT32(bytes);
    xb.pAudioData = p->second.data();
    xb.pContext = p;
    g_queued++;
    g_voice->SubmitSourceBuffer(&xb);
    return true;
}

uint32_t audio_queued() { return g_queued; }

// ---- Vulkan ---------------------------------------------------------------------------------------------------

void* vulkan_get_instance_proc_addr() {
    HMODULE m = LoadLibraryW(L"vulkan-1.dll");
    return m ? reinterpret_cast<void*>(GetProcAddress(m, "vkGetInstanceProcAddr")) : nullptr;
}

const char* vulkan_surface_extension() { return VK_KHR_WIN32_SURFACE_EXTENSION_NAME; }

int vulkan_create_surface(void* instance, void* native_window, uint64_t* surface, void* gipa) {
    auto create = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(gipa)(static_cast<VkInstance>(instance), "vkCreateWin32SurfaceKHR"));
    VkWin32SurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    ci.hinstance = GetModuleHandleW(nullptr);
    ci.hwnd = static_cast<HWND>(native_window);
    VkSurfaceKHR s = VK_NULL_HANDLE;
    VkResult r = create(static_cast<VkInstance>(instance), &ci, nullptr, &s);
    *surface = reinterpret_cast<uint64_t>(s);
    return r;
}

}  // namespace nx::plat

// ---- crash handler, watchpoints, entry --------------------------------------------------------------------------

namespace {

// NX_WATCH=<hex address>: hardware write watchpoint (DR0, 8 bytes) on the main thread; each write (up to 30) logs
// the host module/offset and the guest context.
uint64_t g_watch = 0;

void arm_watch(HANDLE thread) {
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    GetThreadContext(thread, &ctx);
    ctx.Dr0 = g_watch;
    ctx.Dr7 = (1ull << 0) | (1ull << 16) | (3ull << 18);  // L0, R/W0 = write, LEN0 = 8 bytes
    SetThreadContext(thread, &ctx);
}

void dump_guest(nx::CPU& c) {
    using namespace nx;
    log("  guest lr %s  sp %llx  tpidr %llx  tpidrro %llx", describe(c.x[30]).c_str(), (unsigned long long)c.x[31],
        (unsigned long long)c.tpidr, (unsigned long long)c.tpidrro);
    for (int k = 0; k < 32; k += 4)
        log("  x%-2d %016llx %016llx %016llx %016llx", k, (unsigned long long)c.x[k], (unsigned long long)c.x[k + 1],
            (unsigned long long)c.x[k + 2], (unsigned long long)c.x[k + 3]);
    backtrace(c);
}

uint64_t exe_va(void* p) { return uint64_t(p) - uint64_t(GetModuleHandleW(nullptr)) + 0x140000000ull; }

LONG WINAPI watch_handler(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !g_watch) return EXCEPTION_CONTINUE_SEARCH;
    nx::log("WATCH: %llx written (now %llx) at %s.exe+%llx (tools/symbolize.py)", (unsigned long long)g_watch,
            (unsigned long long)nx::ld<uint64_t>(g_watch), nx::game::kInfo.id, (unsigned long long)exe_va(e->ExceptionRecord->ExceptionAddress));
    dump_guest(nx::current_cpu());
    static int hits = 0;
    if (++hits >= 30) e->ContextRecord->Dr7 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

// "<module>+<offset>" for a host code address; our own exe as the linker's VA (tools/symbolize.py reads the map).
std::string host_where(uint64_t pc) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(pc), &m))
        return "?";
    char b[MAX_PATH + 32];
    if (m == GetModuleHandleW(nullptr)) {
        std::snprintf(b, sizeof b, "%s.exe+%llx", nx::game::kInfo.id, (unsigned long long)exe_va(reinterpret_cast<void*>(pc)));
        return b;
    }
    char path[MAX_PATH] = "?";
    GetModuleFileNameA(m, path, MAX_PATH);
    const char* name = std::max(std::strrchr(path, '\\'), std::strrchr(path, '/'));
    name = name ? name + 1 : path;
    uint64_t at = pc - uint64_t(m);
#ifdef NX_GAMELIB_FILE
    // The game code library (nx/gamelib.h) as the linker's VA too, for tools/symbolize.py and its own map.
    if (_stricmp(name, NX_GAMELIB_FILE) == 0) {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const char*>(m) + dos->e_lfanew);
        at += nt->OptionalHeader.ImageBase;
    }
#endif
    std::snprintf(b, sizeof b, "%s+%llx", name, (unsigned long long)at);
    return b;
}

// The host call stack at the fault, unwound from its context with the x64 unwind tables.
void host_backtrace(const CONTEXT& at) {
    CONTEXT c = at;
    for (int k = 0; k < 16 && c.Rip; k++) {
        nx::log("  host #%-2d %s", k, host_where(c.Rip).c_str());
        DWORD64 base = 0;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
        if (!f) {  // a leaf function: the return address is at rsp
            c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
            c.Rsp += 8;
            continue;
        }
        void* handler_data = nullptr;
        DWORD64 frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, f, &c, &handler_data, &frame, nullptr);
    }
}

LONG WINAPI crash_handler(EXCEPTION_POINTERS* e) {
    auto* r = e->ExceptionRecord;
    uint64_t info = r->NumberParameters > 1 ? r->ExceptionInformation[1] : 0;
    nx::log("CRASH: exception %08lx at %s (tools/symbolize.py for the exe), data address %llx", r->ExceptionCode,
            host_where(uint64_t(r->ExceptionAddress)).c_str(), (unsigned long long)info);
    host_backtrace(*e->ContextRecord);
    dump_guest(nx::current_cpu());
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

// NX_THROW_TRACE=1: log the host call stack of every C++ throw that is not a guest longjmp (first chance).
LONG WINAPI throw_tracer(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != 0xE06D7363) return EXCEPTION_CONTINUE_SEARCH;
    void* frames[24];
    USHORT n = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
    std::string s = "C++ throw, host stack:";
    for (USHORT k = 0; k < n; k++) s += " " + host_where(uint64_t(frames[k]));  // (the game code is in its own library)
    nx::log("%s", s.c_str());
    return EXCEPTION_CONTINUE_SEARCH;
}

int main() {
    nx::plat::make_dirs(nx::plat::data_root());
    nx::plat::load_pc_settings();
    nx::plat::start_memory_guard();  // before anything can grow: a leak must never freeze the machine (memguard_win.cpp)
    SetUnhandledExceptionFilter(crash_handler);
    if (const char* t = std::getenv("NX_THROW_TRACE"); t && *t == '1') AddVectoredExceptionHandler(1, throw_tracer);
    if (const char* w = std::getenv("NX_WATCH")) {
        g_watch = std::strtoull(w, nullptr, 16);
        AddVectoredExceptionHandler(1, watch_handler);
        arm_watch(GetCurrentThread());
        nx::log("watching writes to %llx", (unsigned long long)g_watch);
    }
    try {
        nx::boot();
    } catch (const nx::LongJmp& j) {
        nx::fatal("longjmp to buffer %llx with no setjmp frame", (unsigned long long)j.buf);
    } catch (const std::exception& e) {
        nx::fatal("host exception: %s", e.what());
    }
    return 0;
}

// ---- the launcher's helpers (platform.h) ----
namespace nx::plat {

static std::wstring utf16(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(size_t(std::max(1, n)) - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

void pick_folder(const std::string& title, const std::string& start, std::function<void(std::string)> done) {
    // The dialog is modal to the window but runs on a thread of its own, so the launcher keeps drawing.
    std::thread([title, start, done] {
        std::string out;
        if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
            IFileOpenDialog* d = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) {
                DWORD opt = 0;
                d->GetOptions(&opt);
                d->SetOptions(opt | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
                d->SetTitle(utf16(title).c_str());
                IShellItem* at = nullptr;
                if (!start.empty() && SUCCEEDED(SHCreateItemFromParsingName(utf16(start).c_str(), nullptr, IID_PPV_ARGS(&at))))
                    d->SetFolder(at), at->Release();
                IShellItem* item = nullptr;
                if (SUCCEEDED(d->Show(nx::g_window)) && SUCCEEDED(d->GetResult(&item))) {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                        int n = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
                        out.assign(size_t(std::max(1, n)) - 1, '\0');
                        WideCharToMultiByte(CP_UTF8, 0, path, -1, out.data(), n, nullptr, nullptr);
                        CoTaskMemFree(path);
                    }
                    item->Release();
                }
                d->Release();
            }
            CoUninitialize();
        }
        done(out);
    }).detach();
}

void pick_image(const std::function<void(std::string)>& done) {
    std::thread([done] {
        std::string out;
        if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
            IFileOpenDialog* d = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) {
                DWORD opt = 0; d->GetOptions(&opt); d->SetOptions(opt | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
                const COMDLG_FILTERSPEC filters[] = {{L"Images", L"*.jpg;*.jpeg;*.png"}};
                d->SetFileTypes(1, filters);
                IShellItem* item = nullptr;
                if (SUCCEEDED(d->Show(nx::g_window)) && SUCCEEDED(d->GetResult(&item))) {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                        int n = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
                        std::vector<char> text(size_t(std::max(1, n)));
                        WideCharToMultiByte(CP_UTF8, 0, path, -1, text.data(), n, nullptr, nullptr);
                        out = text.data(); CoTaskMemFree(path);
                    }
                    item->Release();
                }
                d->Release();
            }
            CoUninitialize();
        }
        done(out);
    }).detach();
}

void open_folder(const std::string& path) {
    if (path.rfind("http", 0) != 0) make_dirs(path);  // (or a web page)
    ShellExecuteW(nullptr, L"open", utf16(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::vector<std::string> program_args() {
    std::vector<std::string> out;
    int n = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &n);
    for (int i = 1; argv && i < n; i++) {
        int len = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string a(size_t(std::max(1, len)) - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, a.data(), len, nullptr, nullptr);
        out.push_back(a);
    }
    if (argv) LocalFree(argv);
    return out;
}

bool upgrade_button_font(const std::string& out_path) {
    std::wstring exe_path(32768, L'\0');
    const DWORD len = GetModuleFileNameW(nullptr, exe_path.data(), DWORD(exe_path.size()));
    if (len == 0 || len >= exe_path.size()) return false;
    exe_path.resize(len);
    const auto exe_dir = std::filesystem::path(exe_path).parent_path();
    std::error_code ec;
    const auto script = exe_dir / L"game-build" / L"tools" / L"fonts" / L"make_button_font.py";
    if (!std::filesystem::is_regular_file(script, ec)) return false;
    const auto bundled_py = exe_dir / L"game-build" / L"python" / L"python.exe";
    const std::wstring py = std::filesystem::is_regular_file(bundled_py, ec) ? bundled_py.wstring() : L"python.exe";
    std::wstring base;
    for (const wchar_t* cand : {L"C:/Windows/Fonts/arialuni.ttf", L"C:/Windows/Fonts/arial.ttf"}) {
        if (std::filesystem::is_regular_file(cand, ec)) {
            base = cand;
            break;
        }
    }
    if (base.empty()) return false;
    std::wstring cmd = L"\"" + py + L"\" \"" + script.wstring() + L"\" --base \"" + base +
                       L"\" --out \"" + utf16(out_path) + L"\"";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, exe_dir.c_str(), &si, &pi))
        return false;
    const DWORD waited = WaitForSingleObject(pi.hProcess, 15000);
    DWORD code = 1;
    if (waited == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return waited == WAIT_OBJECT_0 && code == 0;
}

}  // namespace nx::plat

// switch2/'s log lines (switch2.h), into the runtime's log
extern "C" void rt_log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    nx::log("%s", line);
}
