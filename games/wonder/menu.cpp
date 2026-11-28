// The recomp's entries in Wonder's own menus (docs/MENU.md). tools/wonder_menu_mod.py adds them to the game's data;
// this file gives them meaning. All three quit the way closing the window does (plat::exit_process: the pipeline
// cache is saved, the log flushed).
//
// Pause menu "Quit Game": the button (data) reports game::sequence::PauseResult "QuitToDesktop", a name the game does
// not know. Enum types are registered with
// callbacks the game only reaches through function pointers, so swapping a function-table entry replaces one for the
// whole game. The pause screen turns the chosen button's name into its value with the type's from-string callback
// when the button is chosen (not before): that call is the moment to quit. Ported from the josiahg2502-glitch fork
// (b2e9708), which targets 1.2.0; the offsets here are 1.0.0's. (The fork's world-map menu button, b17c70a, is not
// here: at 1.0.0 that menu's cursor never stops on an added button. game::sequence::MainMenuResult's from-string
// callback is main+0x1c0beec, should it be wanted.)
//
// Title screen: it has no buttons (it waits for L + R), so the mod adds a "(-) Quit Game" line and holding Minus there
// quits. The title's sequence waits in an AINB node, WaitPressKeyInTitle, whose update (a virtual function, so also
// swappable) runs every frame until L + R: while it runs, the title is up.
#include <algorithm>
#include <chrono>
#include <cstring>

#include <nx/game.h>
#include <nx/gamelib.h>
#include <nx/module.h>
#include <nx/runtime.h>

#include "overlay/overlay.h"
#include "platform.h"


namespace nx::wonder {

namespace {

struct HookVariant {
    const char* version;
    uint64_t offset;
    uint32_t code[8];
};

// Known targets in main (1.0.0 and 1.2.1): each variant checks its first instructions before swapping.
struct Hook {
    const char* what;
    HookVariant variants[2];
    Fn replacement;
    Fn game = nullptr;
};

// PauseResult's from-string callback: bool (int* out, const char** name).
void pause_result_from_string(CPU& c);
// WaitPressKeyInTitle's update (this): done once the title's key state reads 3.
void wait_press_key_in_title(CPU& c);

Hook g_pause = {
    "pause menu",
    {
        {"1.0.0", 0x1c0bd9c, {0xa9be7bfd, 0xf9000bf3, 0x910003fd, 0xf9400021, 0xaa0003f3, 0x910063a0, 0xb9001bbf, 0x940015a4}},
        {"1.2.1", 0x1d3415c, {0xa9be7bfd, 0xf9000bf3, 0x910003fd, 0xf9400021, 0xaa0003f3, 0x910063a0, 0xb9001bbf, 0x940016e4}},
    },
    pause_result_from_string,
};
Hook g_title = {
    "title screen",
    {
        {"1.0.0", 0x1bfcddc, {0x9000d1a8, 0xf9454d08, 0xb9404d08, 0x71000d1f, 0x54000040, 0xd65f03c0, 0xb9401808, 0x52b00009}},
        {"1.2.1", 0x1d2519c, {0x9000d4c8, 0xf9431908, 0xb9404d08, 0x71000d1f, 0x54000040, 0xd65f03c0, 0xb9401808, 0x52b00009}},
    },
    wait_press_key_in_title,
};

[[noreturn]] void quit(const char* where) {
    log("menu: Quit Game chosen on the %s", where);
    plat::exit_process(0);  // what closing the window does
}

void pause_result_from_string(CPU& c) {
    if (std::strcmp(reinterpret_cast<const char*>(ld<uint64_t>(c.x[1])), "QuitToDesktop") == 0) quit(g_pause.what);
    g_pause.game(c);
}

// Title-screen quit button held on its own this long quits: a held press, so a tap (or the start of Minus + Plus,
// the overlay's combo, which the game never sees) does not.
constexpr auto kHold = std::chrono::milliseconds(400);

// The title takes any button as "start": while the configured quit button is held, the game's update is held back (the
// title keeps waiting), so a hold can quit; let go sooner and the update runs, and the title starts as it always did.
void wait_press_key_in_title(CPU& c) {
    const int btn = std::clamp(overlay::title_quit_button(), 0, int(overlay::kControlCount) - 1);
    const uint64_t quit_bit = 1ull << unsigned(btn);
    static int logged_btn = -1;
    if (logged_btn != btn) {
        log("menu: the title screen is up (hold pad action %d to quit)", btn);
        logged_btn = btn;
    }
    static std::chrono::steady_clock::time_point since{};
    if (hid_buttons() == quit_bit) {
        auto now = std::chrono::steady_clock::now();
        if (since == std::chrono::steady_clock::time_point{}) since = now, log("menu: quit button held on the title screen");
        if (now - since >= kHold) quit(g_title.what);
        return;
    }
    since = {};
    g_title.game(c);
}

void install(Hook& h) {
    const ModuleInfo* main = game_lib().main;  // (nx/gamelib.h: linked in, or the game code library)
    const LoadedModule* m = main ? module_at(main->base) : nullptr;
    if (!main || !m) return;
    for (const HookVariant& v : h.variants) {
        if (v.offset + sizeof(v.code) > main->text_size || (v.offset & 3u) != 0) continue;
        const uint64_t at = main->base + v.offset;
        bool match = true;
        for (int k = 0; k < 8; k++)
            if (ld<uint32_t>(at + 4 * k) != v.code[k]) {
                match = false;
                break;
            }
        if (!match) continue;
        h.game = m->table[v.offset / 4];
        if (!h.game) {
            log("menu: no function at main+%llx (%s): no Quit Game on the %s", (unsigned long long)v.offset,
                v.version, h.what);
            return;
        }
        m->table[v.offset / 4] = h.replacement;
        log("menu: Quit Game hooked on the %s (%s at main+%llx)", h.what, v.version, (unsigned long long)v.offset);
        return;
    }
    log("menu: code is not a supported game version (1.0.0 / 1.2.1): no Quit Game on the %s", h.what);
}

}  // namespace

void menu_init() {
    if (plat::setting("quit_game") == "0") return;  // NX_QUIT_GAME=0: the game's menus as they are
    install(g_pause);
    install(g_title);
}

}  // namespace nx::wonder
