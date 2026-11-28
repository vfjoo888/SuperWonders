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
#include <cmath>
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
    uint32_t code[8];
    int code_words = 8;
    Fn replacement;
    Fn game = nullptr;
};

struct VersionLayout {
    const char* version;
    uint64_t rigid_body_vtable;
    uint64_t off;
    uint64_t on;
    uint64_t hit_enter;
    uint64_t hit_update;
    uint64_t step_info_ptr;
    uint64_t anm_driven_vel;
    uint64_t anm_delta_vel_y;
    uint64_t cam_scroll_update;
    uint64_t grav_scale_update;
    uint64_t player_comp_calc;
    uint64_t cluster_trail_update;
    uint64_t cluster_pose_update;
};

void body_off(CPU& c);
void body_on(CPU& c);
void hit_enter(CPU& c);
void hit_update(CPU& c);
void anm_driven_vel(CPU& c);
void anm_delta_vel_y(CPU& c);
void cam_scroll_update(CPU& c);
void grav_scale_update(CPU& c);
void player_comp_calc(CPU& c);
void cluster_trail_update(CPU& c);
void cluster_pose_update(CPU& c);

Hook g_off = {"rigid body off",
              {0xa9be7bfd, 0xf9000bf3, 0x910003fd, 0xf9400408, 0xf9400508, 0xf940fd09, 0xf9410528, 0xb4000348},
              8,
              body_off};
Hook g_on = {"rigid body on",
             {0xd10103ff, 0xa9027bfd, 0xa9034ff4, 0x910083fd, 0xf9400408, 0xf9400508, 0xf940fd09, 0xf9410528},
             8,
             body_on};

// IsHitInfoRepOut: enter +0xd0 and update +0x118; the result is written through node+0x30.
Hook g_hit_enter = {"IsHitInfoRepOut enter",
                    {0xa9be7bfd, 0xf9000bf3, 0x910003fd, 0xaa0003f3, 0x94000016, 0xb9401a68, 0x52b00009, 0x33007509},
                    8,
                    hit_enter};
Hook g_hit_update = {"IsHitInfoRepOut update",
                     {0xa9be7bfd, 0xf9000bf3, 0x910003fd, 0xaa0003f3, 0x94000008, 0xb9401a68, 0x52b00009, 0x33007509},
                     8,
                     hit_update};

// GetAnmDrivenPosVel::calc and its sibling delta_y*60 helper:
// both multiply the per-step animation delta by a hardcoded 60.0f instead of 60.0f / step_ratio,
// under-scaling launch/exit velocity above 60 FPS (e.g. Course010 Wonder_Space / ItemWonderHole).
Hook g_anm_driven_vel = {"GetAnmDrivenPosVel",
                         {0xd10103ff, 0xfd000be8, 0xf90017f5, 0xa901fbfd, 0xa9034ff4, 0x910063fd, 0x9100c013, 0},
                         7,
                         anm_driven_vel};
Hook g_anm_delta_vel_y = {"AnmDeltaVelY",
                          {0xa9be7bfd, 0xf9000bf3, 0x910003fd, 0xf9400408, 0xaa0003f3, 0xf9400508, 0xf940fd00, 0},
                          7,
                          anm_delta_vel_y};
// CourseCameraScroll::update_ (virtual) calls GameCourseCamera::calc, which computes
// the vertical camera speed cap [cam+0xe4] from player delta_y (already O(step_ratio))
// and clamps it to +-1.5 * step_ratio, then multiplies [cam+0xe4] by step_ratio a second
// time (0x6c6a04 in 1.2.1), making vertical camera tracking O(step_ratio^2) above 60 FPS.
Hook g_cam_scroll_update = {"CourseCameraScroll::update_",
                            {0xd10203ff, 0xfd0013e8, 0xa9037bfd, 0xa90467fa, 0xa9055ff8, 0xa90657f6, 0xa9074ff4, 0x9100c3fd},
                            8,
                            cam_scroll_update};
Hook g_grav_scale_update = {"PlayerSetGravityScale",
                            {0xd102c3ff, 0xfd0023ec, 0x6d052beb, 0x6d0623e9, 0xa9077bfd, 0xa9085ff8, 0xa90957f6, 0xa90a4ff4},
                            8,
                            grav_scale_update};
Hook g_player_comp_calc = {"PlayerComponent::calc",
                           {0xd10383ff, 0x6d0723e9, 0xa9087bfd, 0xa9096ffc, 0xa90a67fa, 0xa90b5ff8, 0xa90c57f6, 0xa90d4ff4},
                           8,
                           player_comp_calc};
Hook g_cluster_trail_update = {"ClusterModelDrawComponent::updateTrail",
                               {0xd104c3ff, 0xfd005bec, 0x6d0c2beb, 0x6d0d23e9, 0xa90e7bfd, 0xa90f67fc, 0xa9105ff8, 0xa91157f6},
                               8,
                               cluster_trail_update};
Hook g_cluster_pose_update = {"ClusterModelDrawComponent::updatePose",
                              {0x6db63bef, 0x6d0133ed, 0x6d022beb, 0x6d0323e9, 0xa9047bfd, 0xa9056ffc, 0xa90667fa, 0xa9075ff8},
                              8,
                              cluster_pose_update};

constexpr VersionLayout kVersions[] = {
    {"1.0.0", 0x33326b8, 0x2b7238, 0x4d42f0, 0x16d877c, 0x16d87b4, 0x36427b0, 0x16553ac, 0x153d750, 0x6b7eb4, 0x152dfd4, 0x273868, 0x1868144, 0x1868770},
    {"1.2.1", 0x34bbc88, 0x2dfb70, 0x4d4c28, 0x17fd50c, 0x17fd544, 0x37cf340, 0x177a76c, 0x16629bc, 0x6c55ac, 0x1653344, 0x2885a4, 0x198e0b8, 0x198e6e4},
};

uint64_t g_rigid_body_vtable = 0;
uint64_t g_step_info_ptr = 0;
bool g_hold_mode = false;               // NX_SEED_GUARD=60
std::atomic<bool> g_landed{false};      // the seed has touched down since its appearance began
std::atomic<uint64_t> g_kept{0};        // checks answered "touching" by the guard, this appearance
constexpr uint64_t kHoldMaxNs = 3'000'000'000ull;
uint64_t g_main = 0;
std::atomic<uint64_t> g_seed{0};      // the seed the hold is for
std::atomic<uint64_t> g_since_ns{0};  // when the hold began

// The node's actor when it switches "Target" on a Wonder Seed, else 0.
uint64_t seed_actor(uint64_t node) {
    if (!g_rigid_body_vtable || ld<uint64_t>(node) != g_main + g_rigid_body_vtable) return 0;
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

float current_step_ratio() {
    if (!g_step_info_ptr) return 1.0f;
    const uint64_t step_info = ld<uint64_t>(g_main + g_step_info_ptr);
    if (!step_info) return 1.0f;
    return ld<float>(step_info + 0x8);
}

void anm_driven_vel(CPU& c) {
    const uint64_t node = c.x[0];
    g_anm_driven_vel.game(c);
    const float ratio = current_step_ratio();
    if (!(ratio > 0.01f && ratio < 0.99f)) return;
    const uint64_t dst = ld<uint64_t>(node + 0x30) & ~3ULL;
    if (!dst) return;
    const float inv = 1.0f / ratio;
    const float vx = ld<float>(dst + 0x0) * inv;
    const float vy = ld<float>(dst + 0x4) * inv;
    const float vz = ld<float>(dst + 0x8) * inv;
    st<float>(dst + 0x0, vx);
    st<float>(dst + 0x4, vy);
    st<float>(dst + 0x8, vz);
    static int s_anm_log = 0;
    if (vy > 10.0f && s_anm_log < 10) {
        s_anm_log++;
        log("wonder: anm_driven_vel vy=%.2f ratio=%.3f", vy, ratio);
    }
}

void anm_delta_vel_y(CPU& c) {
    const uint64_t node = c.x[0];
    const uint32_t prev_bits = ld<uint32_t>(node + 0x48);
    constexpr uint32_t kSentinel = 0x7fc01234u;
    st<uint32_t>(node + 0x48, kSentinel);
    g_anm_delta_vel_y.game(c);
    if (ld<uint32_t>(node + 0x48) == kSentinel) {
        st<uint32_t>(node + 0x48, prev_bits);
        return;
    }
    const float ratio = current_step_ratio();
    if (!(ratio > 0.01f && ratio < 0.99f)) return;
    const float vy = ld<float>(node + 0x48) / ratio;
    st<float>(node + 0x48, vy);
    static int s_delta_log = 0;
    if (vy > 10.0f && s_delta_log < 10) {
        s_delta_log++;
        log("wonder: anm_delta_vel_y vy=%.2f ratio=%.3f", vy, ratio);
    }
}

void cam_scroll_update(CPU& c) {
    const uint64_t cam = c.x[0];
    const float ratio = current_step_ratio();
    if (!(ratio > 0.01f && ratio < 0.99f)) {
        g_cam_scroll_update.game(c);
        return;
    }
    uint64_t pcam = 0;
    uint64_t actor_impl = 0;
    if (uint64_t ai = ld<uint64_t>(cam + 8)) {
        if (uint64_t actor = ld<uint64_t>(ai + 8)) {
            actor_impl = ld<uint64_t>(actor + 0x1f8);
            if (actor_impl) {
                uint64_t comps = ld<uint64_t>(actor_impl + 0x208);
                uint32_t max_c = ld<uint32_t>(actor_impl + 0x200);
                if (comps && max_c > 0x42) pcam = ld<uint64_t>(comps + 0x210);
            }
        }
    }
    const float pcam_y_before = pcam ? ld<float>(pcam + 0x2c) : 0.0f;
    const float e4_before = ld<float>(cam + 0xe4);
    g_cam_scroll_update.game(c);
    const float e4_after = ld<float>(cam + 0xe4);
    const float delta = e4_after - e4_before;
    if (std::fabs(delta) > 1e-6f) {
        const float k = 1.0f - std::pow(0.75f, ratio);
        const float target_scaled = e4_before + delta / k;
        const float target_60 = target_scaled / ratio;
        float step = k * (target_60 - e4_before);
        if (step > 0.1f) step = 0.1f;
        if (step < -0.1f) step = -0.1f;
        st<float>(cam + 0xe4, e4_before + step);
    }
    // GameCourseCamera::calc clamps the vertical deadzone overshoot [cam+0xa4] to +-1.5 * step_ratio,
    // then multiplies it by (1 - 0.5^step_ratio) (~0.159 at 240 FPS vs 0.5 at 60 FPS), capping vertical
    // camera speed at ~14.3 u/s instead of 45 u/s. Restore the 60 Hz max vertical tracking speed,
    // and keep actor_impl+0x298 synchronized with pcam->pos.y to avoid per-frame snap-back oscillation.
    if (pcam) {
        const float a4 = ld<float>(cam + 0xa4);
        if (std::fabs(a4) >= 1.5f * ratio - 1e-3f) {
            const float pcam_y_after = ld<float>(pcam + 0x2c);
            const float step_y = pcam_y_after - pcam_y_before;
            const float k_y = 1.0f - std::pow(0.5f, ratio);
            const float min_capped_step = 0.5f * k_y * 1.5f * ratio;
            if (step_y * a4 > 0.0f && std::fabs(step_y) >= min_capped_step) {
                const float scale = 0.5f / k_y;
                float fixed_step = step_y * scale;
                const float max_step = 0.75f * ratio;
                if (fixed_step > max_step) fixed_step = max_step;
                if (fixed_step < -max_step) fixed_step = -max_step;
                const float extra_y = fixed_step - step_y;
                if (std::fabs(extra_y) > 1e-6f) {
                    st<float>(pcam + 0x2c, pcam_y_before + fixed_step);
                    st<float>(pcam + 0x38, ld<float>(pcam + 0x38) + extra_y);
                    if (actor_impl) st<float>(actor_impl + 0x298, ld<float>(actor_impl + 0x298) + extra_y);
                }
            }
        }
    }
}

void grav_scale_update(CPU& c) {
    const uint64_t node = c.x[0];
    const uint8_t node_active = (ld<uint8_t>(node + 0x18) >> 1) & 1;
    const float ratio = current_step_ratio();
    static uint8_t s_last_flag = 255;

    const uint64_t ai = ld<uint64_t>(node + 8);
    const uint64_t actor = ai ? ld<uint64_t>(ai + 8) : 0;
    const uint64_t actor_impl = actor ? ld<uint64_t>(actor + 0x1f8) : 0;
    const uint64_t bb = actor ? ld<uint64_t>(actor + 0x200) : 0;
    const uint64_t comps = actor_impl ? ld<uint64_t>(actor_impl + 0x208) : 0;
    const uint32_t max_c = actor_impl ? ld<uint32_t>(actor_impl + 0x200) : 0;
    const uint64_t player_comp = (comps && max_c > 0x1c) ? ld<uint64_t>(comps + 0xe0) : 0;

    auto read_state = [&](int32_t& grav_idx, float& vel_y, uint8_t& keep_val, uint8_t& flag_2a8, uint8_t& v2ac, int32_t& v300) -> bool {
        if (!actor_impl || !bb || !player_comp) return false;
        const uint32_t grav_idx_key = ld<uint32_t>(node + 0x64);
        grav_idx = (grav_idx_key < ld<uint32_t>(bb + 0xa8))
                       ? ld<int32_t>(ld<uint64_t>(bb + 0xb0) + 4ULL * grav_idx_key)
                       : -1;
        vel_y = ld<float>(actor_impl + 0x304);
        const uint32_t keep_key = ld<uint32_t>(player_comp + 0x23c);
        keep_val = (keep_key < ld<uint32_t>(bb + 0x198))
                       ? ld<uint8_t>(ld<uint64_t>(bb + 0x1a0) + keep_key)
                       : 0;
        const uint32_t flag_2a8_key = ld<uint32_t>(player_comp + 0x2a8);
        flag_2a8 = (flag_2a8_key < ld<uint32_t>(bb + 0x198))
                       ? ld<uint8_t>(ld<uint64_t>(bb + 0x1a0) + flag_2a8_key)
                       : 0;
        const uint32_t key_2ac = ld<uint32_t>(player_comp + 0x2ac);
        v2ac = (key_2ac < ld<uint32_t>(bb + 0x198))
                   ? ld<uint8_t>(ld<uint64_t>(bb + 0x1a0) + key_2ac)
                   : 0;
        const uint32_t key_300 = ld<uint32_t>(player_comp + 0x300);
        v300 = (key_300 < ld<uint32_t>(bb + 0xa8))
                   ? ld<int32_t>(ld<uint64_t>(bb + 0xb0) + 4ULL * key_300)
                   : -1;
        return true;
    };

    int32_t grav_idx = -1, v300 = -1;
    float pre_vel_y = 0.0f;
    uint8_t keep_val = 0, flag_2a8 = 0, v2ac = 0;
    const bool have_state = read_state(grav_idx, pre_vel_y, keep_val, flag_2a8, v2ac, v300);

    // Above 60 FPS, a frame-counted jump state expires early (~1.33s at 240 FPS vs ~5.33s at 60 FPS)
    // and drops vel_y below 39.0f while still inside AreaKeepJumpSpeed. Keep the float active
    // until the player leaves the area (keep == 0), changes gravity table, or hits a ceiling.
    if (have_state && ratio > 0.01f && ratio < 0.99f && node_active &&
        s_last_flag == 1 && keep_val == 1 && grav_idx == 6 && v2ac == 1 && v300 == 1 &&
        pre_vel_y > 0.0f && pre_vel_y < 39.1f) {
        st<float>(actor_impl + 0x304, 39.1f);
        const uint32_t flag_2a8_key = ld<uint32_t>(player_comp + 0x2a8);
        if (flag_2a8_key < ld<uint32_t>(bb + 0x198))
            st<uint8_t>(ld<uint64_t>(bb + 0x1a0) + flag_2a8_key, 1);
    }

    g_grav_scale_update.game(c);

    if (!have_state) return;
    float vel_y = 0.0f;
    read_state(grav_idx, vel_y, keep_val, flag_2a8, v2ac, v300);
    s_last_flag = flag_2a8;
}

void player_comp_calc(CPU& c) {
    const uint64_t pc = c.x[0];
    const uint64_t step_info = c.x[1];
    const float ratio = step_info ? ld<float>(step_info + 4) : current_step_ratio();
    if (!(ratio > 0.01f && ratio < 0.99f)) {
        g_player_comp_calc.game(c);
        return;
    }
    const int32_t c6cc_before = ld<int32_t>(pc + 0x6cc);
    const int32_t c6d0_before = ld<int32_t>(pc + 0x6d0);
    g_player_comp_calc.game(c);

    // 0x2895c8 (1.2.1) truncates step_ratio (< 1.0 above 60 FPS) with fcvtzs to 0,
    // freezing [player_comp+0x6cc] and [+0x6d0] at 0 (breaking Dolphin Kick AttackSensor
    // StartFrame/EndFrame checks and other frame-counted player state checks).
    const int32_t c6cc_after = ld<int32_t>(pc + 0x6cc);
    const int32_t c6d0_after = ld<int32_t>(pc + 0x6d0);
    struct Acc { uint64_t pc; float acc; };
    static Acc s_accs[4]{};
    float* acc = &s_accs[0].acc;
    for (auto& slot : s_accs) {
        if (slot.pc == pc) { acc = &slot.acc; break; }
        if (slot.pc == 0) { slot.pc = pc; slot.acc = 0.0f; acc = &slot.acc; break; }
    }
    *acc += ratio;
    const int32_t inc = static_cast<int32_t>(*acc);
    *acc -= static_cast<float>(inc);
    if (inc > 0) {
        if (c6cc_after == c6cc_before) st<int32_t>(pc + 0x6cc, c6cc_after + inc);
        if (c6d0_after == c6d0_before) st<int32_t>(pc + 0x6d0, c6d0_after + inc);
    }
}

void cluster_trail_update(CPU& c) {
    const uint64_t comp = c.x[0];
    const uint64_t actor = comp ? ld<uint64_t>(comp + 0x10) : 0;
    const float ratio = comp ? ld<float>(comp + 0xb8) : 1.0f;
    const bool active = comp && actor &&
        ((ld<uint8_t>(actor + 0x3dc) & 0x62u) != 0 || ld<uint8_t>(comp + 0x251) != 0);
    if (!active || !(ratio > 0.01f && ratio < 0.99f)) {
        g_cluster_trail_update.game(c);
        return;
    }
    const int32_t idx_before = ld<int32_t>(comp + 0xa0);
    g_cluster_trail_update.game(c);
    const int32_t idx_after = ld<int32_t>(comp + 0xa0);
    if (idx_after != idx_before) return;

    // 0x198e134 (1.2.1) / 0x18681c0 (1.0.0) truncates step_ratio (< 1.0 above 60 FPS) with fcvtzs to 0,
    // freezing the 300-entry trail ring-buffer write index [comp+0xa0]. Advance at 60 Hz (acc += ratio)
    // so the 300-slot buffer keeps 5.0 s of history (matching cluster_pose_update's 60 Hz lag).
    struct Acc { uint64_t comp; float acc; };
    static Acc s_accs[32]{};
    static unsigned s_next_slot = 0;
    float* acc = nullptr;
    for (auto& slot : s_accs) {
        if (slot.comp == comp) { acc = &slot.acc; break; }
    }
    if (!acc) {
        Acc& slot = s_accs[s_next_slot++ % 32];
        slot.comp = comp;
        slot.acc = 0.0f;
        acc = &slot.acc;
    }
    *acc += ratio;
    const int32_t inc = static_cast<int32_t>(*acc);
    *acc -= static_cast<float>(inc);
    if (inc > 0) {
        int32_t idx = idx_after + inc;
        if (idx >= 300) {
            idx %= 300;
            st<uint8_t>(comp + 0xa4, 1);
        }
        st<int32_t>(comp + 0xa0, idx);
    }
}

void cluster_pose_update(CPU& c) {
    const uint64_t comp = c.x[0];
    const float ratio = comp ? ld<float>(comp + 0xb8) : 1.0f;
    if (!comp || !(ratio > 0.01f && ratio < 0.99f)) {
        g_cluster_pose_update.game(c);
        return;
    }
    const uint32_t cnt = ld<uint32_t>(comp + 0x2c8);
    const uint64_t arr = ld<uint64_t>(comp + 0x2d0);
    if (!arr || cnt == 0 || cnt > 64) {
        g_cluster_pose_update.game(c);
        return;
    }
    // 0x198e99c (1.2.1) / 0x1868a28 (1.0.0) divides follower spacing by (60 / step_ratio) Hz, which
    // inflates lag by 1/step_ratio and wraps the 300-slot buffer negative on trailing Bulrushes.
    // Temporarily scale spacing by 1/step_ratio so lag is computed in 60 Hz slots matching updateTrail.
    float saved[64];
    const float inv = 1.0f / ratio;
    for (uint32_t i = 0; i < cnt; ++i) {
        const float v = ld<float>(arr + 4ULL * i);
        saved[i] = v;
        st<float>(arr + 4ULL * i, v * inv);
    }
    g_cluster_pose_update.game(c);
    for (uint32_t i = 0; i < cnt; ++i) st<float>(arr + 4ULL * i, saved[i]);
}

bool matches(uint64_t offset, const Hook& h) {
    const ModuleInfo* main = game_lib().main;
    if (!main || offset + sizeof(uint32_t) * h.code_words > main->text_size || (offset & 3u) != 0) return false;
    const uint64_t at = g_main + offset;
    for (int k = 0; k < h.code_words; k++)
        if (ld<uint32_t>(at + 4 * k) != h.code[k]) return false;
    return true;
}

bool install(uint64_t offset, Hook& h) {
    const LoadedModule* m = module_at(g_main);
    if (!m || !matches(offset, h)) return false;
    h.game = m->table[offset / 4];
    if (!h.game) return false;
    m->table[offset / 4] = h.replacement;
    return true;
}

}  // namespace

void seed_guard_init() {
    const char* e = std::getenv("NX_SEED_GUARD");
    const ModuleInfo* main = game_lib().main;
    if (!main) return;
    g_main = main->base;

    for (const VersionLayout& v : kVersions) {
        if (!matches(v.anm_driven_vel, g_anm_driven_vel)) continue;
        g_step_info_ptr = v.step_info_ptr;
        bool vel_ok = install(v.anm_driven_vel, g_anm_driven_vel);
        if (matches(v.anm_delta_vel_y, g_anm_delta_vel_y))
            vel_ok = install(v.anm_delta_vel_y, g_anm_delta_vel_y) && vel_ok;
        if (vel_ok)
            log("wonder: high-FPS anim-driven velocity fix on (%s)", v.version);
        if (v.cam_scroll_update && install(v.cam_scroll_update, g_cam_scroll_update)) {
            log("wonder: high-FPS CourseCameraScroll::update_ fix on (%s)", v.version);
        }
        if (v.grav_scale_update && install(v.grav_scale_update, g_grav_scale_update))
            log("wonder: high-FPS PlayerSetGravityScale fix on (%s)", v.version);
        if (v.player_comp_calc && install(v.player_comp_calc, g_player_comp_calc))
            log("wonder: high-FPS PlayerComponent::calc counter fix on (%s)", v.version);
        if (v.cluster_trail_update && install(v.cluster_trail_update, g_cluster_trail_update))
            log("wonder: high-FPS ClusterModelDrawComponent trail counter fix on (%s)", v.version);
        if (v.cluster_pose_update && install(v.cluster_pose_update, g_cluster_pose_update))
            log("wonder: high-FPS ClusterModelDrawComponent pose lag fix on (%s)", v.version);
        break;
    }

    if (e && e[0] == '0') {
        log("wonder: Wonder Seed guard off (NX_SEED_GUARD=0)");
        return;
    }
    g_hold_mode = e && std::strcmp(e, "60") == 0;
    for (const VersionLayout& v : kVersions) {
        if (!matches(v.off, g_off) || !matches(v.on, g_on)) continue;
        if (!g_hold_mode && (!matches(v.hit_enter, g_hit_enter) || !matches(v.hit_update, g_hit_update))) continue;
        const bool ok = install(v.off, g_off) && install(v.on, g_on) &&
                        (g_hold_mode || (install(v.hit_enter, g_hit_enter) && install(v.hit_update, g_hit_update)));
        if (ok) {
            g_rigid_body_vtable = v.rigid_body_vtable;
            log("wonder: Wonder Seed guard on (%s, %s while a seed comes out of a pipe)", v.version,
                g_hold_mode ? "60 FPS" : "its landing kept");
        }
        return;
    }
    log("wonder: unsupported game version for Wonder Seed guard (expected 1.0.0 or 1.2.1)");
}

// Each present (game.cpp): ends a hold that has lasted too long (the seed taken by something else, or gone).
void seed_guard_tick(uint64_t now_ns) {
    const uint64_t since = g_since_ns.load();
    if (since && now_ns - since > kHoldMaxNs) release("not seen landing (3 s)");
}

}  // namespace nx::wonder
