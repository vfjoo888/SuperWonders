// am services (appletOE): application proxy, focus/operation-mode state, launch parameters.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iterator>
#include <string_view>

#include "ipc.h"
#include "platform.h"
#include "svc_am.h"
#if NX_GAME_LIBRARY
#include "launcher/game_version.h"
#endif

namespace nx {

namespace {
std::atomic<int> g_internal_resolution_preset{0};
std::atomic<int> g_game_language{0};
std::atomic<uint32_t> g_native_width{1920};  // preset 6: equivalent 16:9 width from the window's or screen's size
std::atomic<uint32_t> g_native_display_width{1920};
std::atomic<uint32_t> g_native_display_height{1080};
}

void set_internal_resolution_preset(int preset) {
    if (preset < 0) preset = 0;
    if (preset > 9) preset = 9;
    g_internal_resolution_preset.store(preset, std::memory_order_relaxed);
}

int internal_resolution_preset() { return g_internal_resolution_preset.load(std::memory_order_relaxed); }

float internal_resolution_scale() {
    // 0: original, 1: 720p, 2: 900p, 3: 1080p, 4: 1440p, 5: 2160p, 6: native, 7: 540p, 8: 480p, 9: 360p
    static constexpr float scales[] = {1.0f, 2.0f / 3.0f, 5.0f / 6.0f, 1.0f, 4.0f / 3.0f, 2.0f, 0.0f,
                                       0.5f, 854.0f / 1920.0f, 1.0f / 3.0f};
    const int p = g_internal_resolution_preset.load(std::memory_order_relaxed);
    return p == 6 ? float(g_native_width.load(std::memory_order_relaxed)) / 1920.0f : scales[p];
}

uint32_t internal_resolution_target_width() {
    // 5: 3840 (4K); 6: Native; 7..9: 540p, 480p, 360p (appended to preserve saved 0..6 indices).
    static constexpr uint32_t widths[] = {0, 1280, 1600, 1920, 2560, 3840, 0, 960, 854, 640};
    const int p = g_internal_resolution_preset.load(std::memory_order_relaxed);
    return p == 6 ? g_native_width.load(std::memory_order_relaxed) : widths[p];
}

void set_native_render_width(uint32_t width) {
    if (width >= 640 && width <= 7680) {
        g_native_width.store(width, std::memory_order_relaxed);
        g_native_display_width.store(width, std::memory_order_relaxed);
    }
}

void set_native_render_size(uint32_t width, uint32_t height) {
    if (width >= 640 && width <= 7680 && height >= 360 && height <= 4320) {
        g_native_display_width.store(width, std::memory_order_relaxed);
        g_native_display_height.store(height, std::memory_order_relaxed);
        const uint32_t eq169 = uint32_t(std::lround(double(height) * 16.0 / 9.0));
        const uint32_t effective = std::clamp(std::min(width, eq169), 640u, 7680u);
        g_native_width.store(effective, std::memory_order_relaxed);
    } else {
        set_native_render_width(width);
    }
}

void native_display_size(uint32_t& width, uint32_t& height) {
    width = g_native_display_width.load(std::memory_order_relaxed);
    height = g_native_display_height.load(std::memory_order_relaxed);
}

void set_game_language(int language) {
    if (language < 0) language = 0;
    if (language > 17) language = 17;
    g_game_language.store(language, std::memory_order_relaxed);
}

uint64_t game_language_code() {
    static constexpr const char* codes[] = {
        "en-US", "it", "de", "fr", "pt-BR", "es", "ko", "ja", "nl", "ru", "zh-Hans", "zh-Hant",
        "pt", "es-419", "fr-CA", "en-GB", "th", "pl",
    };
    const int language = g_game_language.load(std::memory_order_relaxed);
    const char* selected = codes[language >= 0 && language < int(std::size(codes)) ? language : 0];
    uint64_t code = 0;
    std::memcpy(&code, selected, std::strlen(selected));
    return code;
}

namespace {

constexpr uint32_t kAmNoMessage = 0x680;  // am error 3
constexpr uint32_t kMsgFocusStateChanged = 15;
constexpr uint32_t kMsgOperationModeChanged = 30;
constexpr uint32_t kMsgPerformanceModeChanged = 31;

struct AppletState {
    std::deque<uint32_t> messages{kMsgFocusStateChanged, kMsgOperationModeChanged, kMsgPerformanceModeChanged};
    std::shared_ptr<EventState> message_event = std::make_shared<EventState>();
};

AppletState& applet() {
    static AppletState s;
    return s;
}

uint32_t event_handle(std::shared_ptr<EventState> st) {
    auto e = std::make_shared<KEvent>();
    e->state = std::move(st);
    return handle_add(e);
}

std::shared_ptr<Service> svc(const char* name, std::function<void(Service&)> setup) {
    auto s = std::make_shared<Service>(name);
    setup(*s);
    return s;
}

std::shared_ptr<Service> common_state_getter() {
    return svc("ICommonStateGetter", [](Service& s) {
        s.cmds[0] = [](IpcCtx& c) {  // GetEventHandle
            applet().message_event->signaled = !applet().messages.empty();
            c.out_copy.push_back(event_handle(applet().message_event));
            return 0u;
        };
        s.cmds[1] = [](IpcCtx& c) {  // ReceiveMessage
            auto& a = applet();
            if (a.messages.empty()) {
                a.message_event->signaled = false;
                return kAmNoMessage;
            }
            c.push<uint32_t>(a.messages.front());
            a.messages.pop_front();
            if (a.messages.empty()) a.message_event->signaled = false;
            return 0u;
        };
        // NX_HANDHELD=1: handheld mode, so the game renders for 1280x720 (a lighter GPU load) instead of docked 1080p.
        static const bool handheld = std::getenv("NX_HANDHELD") != nullptr;
        s.cmds[5] = [](IpcCtx& c) { c.push<uint8_t>(handheld ? 0 : 1); return 0u; };   // GetOperationMode
        s.cmds[6] = [](IpcCtx& c) { c.push<uint32_t>(handheld ? 0 : 1); return 0u; };  // GetPerformanceMode
        s.cmds[9] = [](IpcCtx& c) { c.push<uint8_t>(1); return 0u; };   // GetCurrentFocusState: in focus
        s.cmds[60] = [](IpcCtx& c) {  // GetDefaultDisplayResolution
            const int preset = g_internal_resolution_preset.load(std::memory_order_relaxed);
            static constexpr uint32_t widths[] = {1920, 1280, 1600, 1920, 2560, 3840, 0, 960, 854, 640};
            static constexpr uint32_t heights[] = {1080, 720, 900, 1080, 1440, 2160, 0, 540, 480, 360};
            uint32_t width = preset ? widths[preset] : handheld ? 1280 : 1920;
            const uint32_t height = preset ? heights[preset] : handheld ? 720 : 1080;
            // Optional Hor+ experiment: widen guest-side culling/camera resolution to match the host aspect ratio.
            // This is opt-in because ultrawide behavior still needs testing on a physical ultrawide display.
            static const bool wide_guest_resolution = std::getenv("NX_WONDER_WIDE_GUEST_RES") != nullptr;
            if (wide_guest_resolution) {
                uint32_t host_width = 0, host_height = 0;
                plat::window_size(host_width, host_height);
                if (host_width && host_height) {
                    const uint32_t wide_width = uint32_t((uint64_t(height) * host_width + host_height / 2) / host_height);
                    if (uint64_t(wide_width) * 9 >= uint64_t(height) * 16) {
                        width = wide_width;
                        static uint32_t logged_width = 0, logged_height = 0;
                        if (logged_width != width || logged_height != height) {
                            log("am: experimental wide guest resolution %ux%u (host %ux%u)", width, height,
                                host_width, host_height);
                            logged_width = width;
                            logged_height = height;
                        }
                    }
                }
            }
            static int logged_preset = -1;
            if (logged_preset != preset) {
                log("am: GetDefaultDisplayResolution preset %d -> %ux%u", preset, width, height);
                logged_preset = preset;
            }
            c.push<uint32_t>(width);
            c.push<uint32_t>(height);
            return 0u;
        };
        s.cmds[61] = [](IpcCtx& c) {  // GetDefaultDisplayResolutionChangeEvent
            c.out_copy.push_back(event_handle(std::make_shared<EventState>()));
            return 0u;
        };
    });
}

std::shared_ptr<Service> self_controller() {
    return svc("ISelfController", [](Service& s) {
        s.cmds[40] = [](IpcCtx& c) { c.push<uint64_t>(1); return 0u; };  // CreateManagedDisplayLayer
        s.cmds[0] = [](IpcCtx&) { return 0u; };                             // Exit
        s.cmds[62] = [](IpcCtx&) { return 0u; };                            // SetIdleTimeDetectionExtension
        for (uint32_t cmd : {19u, 50u, 71u, 110u})                           // AccumulatedSuspendedTickChangedEvent etc.
            s.cmds[cmd] = [](IpcCtx& c) {
                c.out_copy.push_back(event_handle(std::make_shared<EventState>()));
                return 0u;
            };
    });
}

std::shared_ptr<Service> window_controller() {
    return svc("IWindowController", [](Service& s) {
        s.cmds[1] = [](IpcCtx& c) { c.push<uint64_t>(0x51); return 0u; };  // GetAppletResourceUserId
    });
}

std::shared_ptr<Service> application_functions() {
    return svc("IApplicationFunctions", [](Service& s) {
        s.cmds[1] = [](IpcCtx& c) {  // PopLaunchParameter(kind): the preselected user, once
            static bool popped = false;
            if (c.arg<uint32_t>(0) != 2 || popped) return 0x2A80u;
            popped = true;
            c.push_object(preselected_user_storage());
            return 0u;
        };
        s.cmds[20] = [](IpcCtx& c) { c.push<uint64_t>(0); return 0u; };  // EnsureSaveData
        s.cmds[21] = [](IpcCtx& c) {                                      // GetDesiredLanguage
            const uint64_t code = game_language_code();
            static uint64_t logged_code = 0;
            if (logged_code != code) {
                char selected[9] = {};
                std::memcpy(selected, &code, sizeof code);
                log("oe: GetDesiredLanguage -> %s", selected);
                logged_code = code;
            }
            c.push<uint64_t>(code);
            return 0u;
        };
        s.cmds[23] = [](IpcCtx& c) {  // GetDisplayVersion -> nn::oe::DisplayVersion { char name[16]; }
            static const std::string ver = []() -> std::string {
#if NX_GAME_LIBRARY
                const std::string root = plat::data_root();
                const std::filesystem::path p(
                    std::u8string_view(reinterpret_cast<const char8_t*>(root.data()), root.size()));
                const auto info = launcher::inspect_output_dir(p, "", "");
                if (!info.installed_version.empty() && info.installed_version.size() < 16)
                    return info.installed_version;
#endif
                return "1.0.0";
            }();
            char out[16] = {};
            std::memcpy(out, ver.data(), std::min(ver.size(), sizeof(out) - 1));
            for (char ch : out) c.push<char>(ch);
            static bool logged = false;
            if (!logged) {
                logged = true;
                log("oe: GetDisplayVersion -> %s", out);
            }
            return 0u;
        };
        s.cmds[40] = [](IpcCtx& c) { c.push<uint8_t>(0); return 0u; };  // NotifyRunning
        s.cmds[50] = [](IpcCtx& c) {                                     // GetPseudoDeviceId
            c.push<uint64_t>(0x1234);
            c.push<uint64_t>(0x5678);
            return 0u;
        };
        s.cmds[130] = [](IpcCtx& c) {  // GetGpuErrorDetectedSystemEvent
            c.out_copy.push_back(event_handle(std::make_shared<EventState>()));
            return 0u;
        };
        s.cmds[210] = [](IpcCtx&) { return 0u; };  // UnsetGameCardEject... (stub)
    });
}

std::shared_ptr<Service> application_proxy() {
    return svc("IApplicationProxy", [](Service& s) {
        s.cmds[0] = [](IpcCtx& c) { c.push_object(common_state_getter()); return 0u; };
        s.cmds[1] = [](IpcCtx& c) { c.push_object(self_controller()); return 0u; };
        s.cmds[2] = [](IpcCtx& c) { c.push_object(window_controller()); return 0u; };
        s.cmds[3] = [](IpcCtx& c) { c.push_object(std::make_shared<Service>("IAudioController")); return 0u; };
        s.cmds[4] = [](IpcCtx& c) { c.push_object(std::make_shared<Service>("IDisplayController")); return 0u; };
        s.cmds[10] = [](IpcCtx& c) { c.push_object(std::make_shared<Service>("IProcessWindingController")); return 0u; };
        s.cmds[11] = [](IpcCtx& c) { c.push_object(std::make_shared<Service>("ILibraryAppletCreator")); return 0u; };
        s.cmds[20] = [](IpcCtx& c) { c.push_object(application_functions()); return 0u; };
        s.cmds[1000] = [](IpcCtx& c) { c.push_object(std::make_shared<Service>("IDebugFunctions")); return 0u; };
    });
}

ServiceRegistrar reg_applet_oe("appletOE", [] {
    return svc("appletOE", [](Service& s) {
        s.cmds[0] = [](IpcCtx& c) { c.push_object(application_proxy()); return 0u; };  // OpenApplicationProxy
    });
});

}  // namespace

}  // namespace nx
