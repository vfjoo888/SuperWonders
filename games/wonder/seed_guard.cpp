// The Wonder Seed at high frame rates: 60 FPS while a seed comes out of a pipe.
//
// Above 60 FPS the real-time frame step (game.cpp) gives the game steps shorter than a 60 Hz frame. Most of Wonder
// does not mind, but the seed a Wonder leaves, when it comes out of a pipe (AI ItemAppearNextGoto, its pickup
// sensor "Target" off until it has landed), races two things after touching down: its "Land" animation ends 0.20 s
// later (which ends the appearance and turns the sensor back on), and its ground contact goes away about 0.18 s
// later. At 60 FPS the game's checks fall at 0.183 s (contact) and 0.200 s (landed: done); at 120 one falls at
// 0.175 s, sees no contact and plays the falling animation instead, which never ends: the seed stays in its
// appearance with the sensor off, and cannot be taken (1-2 and others, also in plain 0.4.0).
//
// So while a Wonder Seed is in that state (from the moment it turns its Target sensor off until it turns it on again,
// about 1 s, 3 s at most) the guard gives its ground-contact check (IsHitInfoRepOut) the answer it gets at 60 FPS:
// once the seed has touched down it counts as touching until the appearance ends. The Land animation then ends,
// the appearance ends and the sensor comes back on, at any frame rate. Only that check, only on Wonder Seeds, only
// in that window.
// NX_SEED_GUARD=60: instead, run that window at the game's own rate (the frame cap held at 60 and a 60 Hz frame
// step, game.cpp). NX_SEED_GUARD=0: off.
//
// The sensor switch: PhysicsRigidBodyOnOff nodes (vtable main+0x33326b8) switch a rigid body by name through the
// shared functions main+0x2b7238 (off, on enter) and main+0x4d42f0 (on again, on leave), both called through their
// vtables, so swapping their function-table entries sees every switch. The node's actor: node+8 -> +8; its type
// name: actor+0x138 (a C string, "ItemWonderFinishWonderSead").
#include <atomic>
#include <cstdlib>
#include <cstring>

#include <nx/game.h>
#include <nx/gamelib.h>
#include <nx/module.h>
#include <nx/runtime.h>

#include "platform.h"

namespace nx::nvn {
void hold_frame_cap(int fps);
int held_frame_cap();
}  // namespace nx::nvn

namespace nx::wonder {

namespace {

struct Hook {
    const char* what;
    uint64_t offset;
    uint32_t code[8];
    Fn replacement;
    Fn game = nullptr;
};

void body_off(CPU& c);
void body_on(CPU& c);
void hit_enter(CPU& c);
void hit_update(CPU& c);

Hook g_off = {"rigid body off", 0x2b7238,
              {0xa9be7bfd, 0xf9000bf3, 0x910003fd, 0xf9400408, 0xf9400508, 0xf940fd09, 0xf9410528, 0xb4000348},
              body_off};
Hook g_on = {"rigid body on", 0x4d42f0,
             {0xd10103ff, 0xa9027bfd, 0xa9034ff4, 0x910083fd, 0xf9400408, 0xf9400508, 0xf940fd09, 0xf9410528},
             body_on};

// IsHitInfoRepOut (vtable main+0x33c7728): enter +0xd0 and update +0x118; the result is written through node+0x30.
Hook g_hit_enter = {"IsHitInfoRepOut enter", 0x16d877c,
                    {0xa9be7bfd, 0xf9000bf3, 0x910003fd, 0xaa0003f3, 0x94000016, 0xb9401a68, 0x52b00009, 0x33007509},
                    hit_enter};
Hook g_hit_update = {"IsHitInfoRepOut update", 0x16d87b4,
                     {0xa9be7bfd, 0xf9000bf3, 0x910003fd, 0xaa0003f3, 0x94000008, 0xb9401a68, 0x52b00009, 0x33007509},
                     hit_update};

constexpr uint64_t kRigidBodyOnOffVtable = 0x33326b8;
bool g_hold_mode = false;               // NX_SEED_GUARD=60
std::atomic<bool> g_landed{false};      // the seed has touched down since its appearance began
std::atomic<uint64_t> g_kept{0};        // checks answered "touching" by the guard, this appearance
constexpr uint64_t kHoldMaxNs = 3'000'000'000ull;
uint64_t g_main = 0;
std::atomic<uint64_t> g_seed{0};      // the seed the hold is for
std::atomic<uint64_t> g_since_ns{0};  // when the hold began

// The node's actor when it switches "Target" on a Wonder Seed, else 0.
uint64_t seed_actor(uint64_t node) {
    if (ld<uint64_t>(node) != g_main + kRigidBodyOnOffVtable) return 0;
    const uint64_t name_at = ld<uint64_t>(node + 0x40) & ~3ull;
    const char* body = name_at ? reinterpret_cast<const char*>(ld<uint64_t>(name_at)) : nullptr;
    if (!body || std::strcmp(body, "Target") != 0) return 0;
    const uint64_t ai = ld<uint64_t>(node + 8);
    const uint64_t actor = ai ? ld<uint64_t>(ai + 8) : 0;
    const char* type = actor ? reinterpret_cast<const char*>(ld<uint64_t>(actor + 0x138)) : nullptr;
    if (!type) return 0;
    return std::strcmp(type, "ItemWonderFinishWonderSead") == 0 || std::strcmp(type, "ItemOriFinishWonderSead") == 0
               ? actor
               : 0;
}

void release(const char* why) {
    const uint64_t since = g_since_ns.exchange(0);
    g_seed = 0;
    g_landed = false;
    if (g_hold_mode) nvn::hold_frame_cap(0);
    if (since)
        log("wonder: Wonder Seed %s after %.2f s (%llu contact checks kept)", why,
            double(plat::monotonic_ns() - since) / 1e9, (unsigned long long)g_kept.exchange(0));
}

// After the game's check: the seed's result, latched once it has touched down.
void keep_contact(uint64_t node) {
    const uint64_t seed = g_seed.load();
    if (!seed) return;
    const uint64_t ai = ld<uint64_t>(node + 8);
    if (!ai || ld<uint64_t>(ai + 8) != seed) return;
    const uint64_t out = ld<uint64_t>(node + 0x30) & ~3ull;
    if (!out) return;
    if (ld<uint8_t>(out)) g_landed = true;
    else if (g_landed) st<uint8_t>(out, 1), ++g_kept;
}

void hit_enter(CPU& c) {
    const uint64_t node = c.x[0];
    g_hit_enter.game(c);
    keep_contact(node);
}

void hit_update(CPU& c) {
    const uint64_t node = c.x[0];
    g_hit_update.game(c);
    keep_contact(node);
}

void body_off(CPU& c) {
    const uint64_t node = c.x[0];
    g_off.game(c);
    const uint64_t seed = seed_actor(node);
    if (!seed) return;
    g_landed = false;
    g_kept = 0;
    g_seed = seed;
    g_since_ns = plat::monotonic_ns();
    if (g_hold_mode) nvn::hold_frame_cap(60);
    log("wonder: a Wonder Seed is coming out (pickup off until it lands)%s",
        g_hold_mode ? ": frame cap held at 60" : "");
}

void body_on(CPU& c) {
    const uint64_t node = c.x[0];
    g_on.game(c);
    const uint64_t seed = seed_actor(node);
    if (seed && seed == g_seed.load()) release("can be taken");
}

bool install(Hook& h) {
    const LoadedModule* m = module_at(g_main);
    const uint64_t at = g_main + h.offset;
    for (int k = 0; k < 8; k++)
        if (ld<uint32_t>(at + 4 * k) != h.code[k]) {
            log("wonder: main+%llx (%s) is not the 1.0.0 code: no Wonder Seed guard", (unsigned long long)h.offset, h.what);
            return false;
        }
    h.game = m->table[h.offset / 4];
    if (!h.game) return false;
    m->table[h.offset / 4] = h.replacement;
    return true;
}

}  // namespace

void seed_guard_init() {
    const char* e = std::getenv("NX_SEED_GUARD");
    if (e && e[0] == '0') {
        log("wonder: Wonder Seed guard off (NX_SEED_GUARD=0)");
        return;
    }
    g_main = game_lib().main->base;
    g_hold_mode = e && std::strcmp(e, "60") == 0;
    const bool ok = install(g_off) && install(g_on) && (g_hold_mode || (install(g_hit_enter) && install(g_hit_update)));
    if (ok)
        log("wonder: Wonder Seed guard on (%s while a seed comes out of a pipe)",
            g_hold_mode ? "60 FPS" : "its landing kept");
}

// Each present (game.cpp): ends a hold that has lasted too long (the seed taken by something else, or gone).
void seed_guard_tick(uint64_t now_ns) {
    const uint64_t since = g_since_ns.load();
    if (since && now_ns - since > kHoldMaxNs) release("not seen landing (3 s)");
}

}  // namespace nx::wonder
