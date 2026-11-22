#include "nvn_ultrawide.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "nvn.h"
#include "nvn_pipeline.h"
#include <nx/module.h>
#include <nx/runtime.h>

#include "kernel.h"
#include "platform.h"
#include "svc_am.h"

namespace nx::nvn {

namespace {

constexpr double k169 = 16.0 / 9.0;

float init_factor() {
    const char* e = std::getenv("NX_ULTRAWIDE");
    if (!e || !*e || e[0] == '0') return 1.0f;
    double aspect = 0;
    unsigned w = 0, h = 0;
    if (std::sscanf(e, "%ux%u", &w, &h) == 2 && h) {
        aspect = double(w) / h;
    } else if (std::sscanf(e, "%u:%u", &w, &h) == 2 && h) {
        aspect = double(w) / h;
    } else {  // "1" / "auto": the window's shape
        uint32_t ww = 0, wh = 0;
        plat::window_size(ww, wh);
        if (wh) aspect = double(ww) / wh;
    }
    float k = float(aspect / k169);
    if (k < 1.02f) k = 1.0f;  // 16:9 and narrower: nothing to do
    if (k > 3.0f) k = 3.0f;
    log("nvn: ultrawide: display aspect %.4f, render targets widened by %.4f", aspect, double(k));
    return k;
}

}  // namespace

float uw_factor() {
    static const float k = init_factor();
    return k;
}

uint32_t uw_widen(uint32_t w) {
    uint32_t v = uint32_t(std::lround(double(w) * uw_factor()));
    return v & ~1u;
}

bool uw_is_target(const Texture& t) {
    if (!uw_on() || t.target != 1 || (t.flags & 4) || t.width < 16 || t.height < 9) return false;
    return std::fabs(double(t.width) / double(t.height) - k169) / k169 <= 0.03;
}

uint32_t uw_host_width(const Texture& t, float render_scale) {
    if (!uw_is_target(t)) return std::max(1u, uint32_t(std::lround(double(t.width) * double(render_scale))));
    uint32_t w = std::max(2u, uint32_t(std::lround(double(t.width) * double(render_scale) * double(uw_factor()))));
    w &= ~1u;
    if (internal_resolution_preset() == 6 && t.width >= 1360) {
        uint32_t dw = 0, dh = 0;
        native_display_size(dw, dh);
        if (dw >= 640 && std::abs(int64_t(dw) - int64_t(w)) <= 2) w = dw;
    }
    return w;
}

bool uw_widen_texture(Texture& t) {
    // Render targets (flags 8), the window textures (display, 1) and the scene colour (flags 0); not packaged assets (4).
    if (!uw_is_target(t)) return false;
    static std::map<std::string, int> seen;  // each distinct widened shape once, for the study log
    char key[64];
    std::snprintf(key, sizeof key, "%ux%u f%x fmt%x", t.width, t.height, t.flags, t.format);
    if (seen[key]++ == 0) log("nvn: ultrawide: widening %s", key);
    t.width = uw_widen(t.width);
    t.uw = true;
    return true;
}

bool uw_crop(int32_t crop[4]) {
    if (!uw_on() || crop[0] != 0 || crop[1] != 0 || crop[2] <= 0 || crop[3] <= 0) return false;
    double r = double(crop[2]) / double(crop[3]);
    if (std::fabs(r - k169) / k169 > 0.03) return false;
    crop[2] = int32_t(uw_widen(uint32_t(crop[2])));
    return true;
}


// NX_UW_SCAN=<present>: once, at that present, lists where guest memory (heap, main's data and bss) holds the numbers
// of the 16:9 picture (aspect 16/9 and 9/16 floats, 1920x1080 and 1280x720 as int and float pairs), with context.
void uw_scan(uint64_t present) {
    static const char* e = std::getenv("NX_UW_SCAN");
    static const uint64_t at = e ? std::strtoull(e, nullptr, 10) : 0;
    static bool done = false;
    if (!e || done || present < at) return;
    done = true;
    auto f32 = [](float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; };
    struct Pat { const char* name; uint32_t a, b; bool pair; };
    const Pat pats[] = {{"f32 16/9", f32(16.f / 9.f), 0, false}, {"f32 9/16", f32(9.f / 16.f), 0, false},
                        {"u32 1920,1080", 1920, 1080, true}, {"f32 1920,1080", f32(1920.f), f32(1080.f), true},
                        {"u32 1280,720", 1280, 720, true}, {"f32 1280,720", f32(1280.f), f32(720.f), true},
                        {"u32 1080,1920", 1080, 1920, true}, {"f32 960,540", f32(960.f), f32(540.f), true}};
    struct Range { uint64_t lo, hi; const char* name; };
    std::vector<Range> ranges{{kHeapBase, kHeapBase + heap_size(), "heap"}};
    if (const LoadedModule* m = module_at(kModuleBase)) {
        const ModuleInfo* mi = m->info;
        ranges.push_back({mi->base + mi->data, mi->base + mi->bss + mi->bss_size, "main data/bss"});
    }
    for (const Range& r : ranges)
        for (const Pat& p : pats) {
            int hits = 0;
            for (uint64_t a = r.lo; a + 8 <= r.hi; a += 4) {
                const uint32_t* w = reinterpret_cast<const uint32_t*>(a);
                if (w[0] != p.a || (p.pair && w[1] != p.b)) continue;
                if (hits++ >= 40) break;
                std::string ctx;
                for (int k = -4; k < 8; k++) {
                    uint64_t q = a + 4 * k;
                    if (q < r.lo || q + 4 > r.hi) continue;
                    uint32_t u = *reinterpret_cast<const uint32_t*>(q);
                    float f;
                    std::memcpy(&f, &u, 4);
                    char b[40];
                    std::snprintf(b, sizeof b, " %s%g/%x", k == 0 ? "[" : "", double(f), u);
                    ctx += b;
                }
                log("uwscan %s %s at %llx (main+%llx):%s", r.name, p.name, (unsigned long long)a,
                    (unsigned long long)(a - kModuleBase), ctx.c_str());
            }
            log("uwscan %s %s: %d hits%s", r.name, p.name, hits, hits > 40 ? "+" : "");
        }
}

// ---- camera aspect -----------------------------------------------------------------------------------------------
// The game keeps its cameras as objects holding {.., far 1000.0, fovy 0.785398, aspect 16/9, ..} (measured with
// NX_UW_SCAN: 40+ of them in a pool). The aspect is a plain float the game turns into the projection, so setting it
// to k * 16/9 gives the Hor+ camera (same vertical field of view, wider horizontal). Found by scanning the heap for
// the three floats in a row (rescanned every 2 s, a worker thread), rewritten at every present.
namespace {
std::mutex g_cam_lock;
std::vector<uint64_t> g_cams;  // addresses of the aspect floats
std::atomic<bool> g_cam_rescan_needed{true};

void cam_scan(bool throttled) {
    const uint32_t far_bits = 0x447a0000u, fov_bits = 0x3f490fdbu, asp_bits = 0x3fe38e39u;
    float wide = float(16.0 / 9.0) * uw_factor();
    uint32_t wide_bits = 0;
    std::memcpy(&wide_bits, &wide, 4);
    uint64_t lo = kHeapBase, hi = kHeapBase + heap_size();
    std::vector<uint64_t> found;
    constexpr uint64_t kChunkBytes = 64ull << 20;
    for (uint64_t base = lo; base + 12 <= hi; base += kChunkBytes) {
        const uint64_t chunk_hi = std::min(hi, base + kChunkBytes);
        for (uint64_t a = base; a + 12 <= chunk_hi; a += 4) {
            const uint32_t* w = reinterpret_cast<const uint32_t*>(a);
            if (w[0] != far_bits) continue;
            if (w[1] == fov_bits && (w[2] == asp_bits || w[2] == wide_bits)) found.push_back(a + 8);
        }
        if (throttled && chunk_hi + 12 <= hi) plat::sleep_ns(2'000'000ll);
    }
    std::lock_guard lk(g_cam_lock);
    g_cams.clear();
    for (uint64_t a : found)
        if (g_cams.size() < 8192) g_cams.push_back(a);
    g_cam_rescan_needed.store(g_cams.size() < 16, std::memory_order_release);
}
}  // namespace

void uw_cameras(uint64_t present) {
    if (!uw_on()) return;
    static const bool off = std::getenv("NX_UW_CAMERA") && std::getenv("NX_UW_CAMERA")[0] == '0';
    if (off) return;
    static std::once_flag once;
    std::call_once(once, [] {
        std::thread([] {
            plat::set_thread_name("uw-camscan");
            for (;;) {
                size_t known = 0;
                {
                    std::lock_guard lk(g_cam_lock);
                    known = g_cams.size();
                }
                const bool urgent = known < 16 || g_cam_rescan_needed.load(std::memory_order_acquire);
                if (urgent) {
                    uint64_t t0 = plat::monotonic_ns();
                    cam_scan(known >= 16);
                    static int logged = 0;
                    if (logged++ < 5) {
                        std::lock_guard lk(g_cam_lock);
                        log("nvn: ultrawide: camera scan %.0f ms, %zu cameras", double(plat::monotonic_ns() - t0) / 1e6, g_cams.size());
                    }
                }
                plat::sleep_ns(2'000'000'000ll);
            }
        }).detach();
    });
    const uint32_t asp_bits = 0x3fe38e39u;
    float wide = float(16.0 / 9.0) * uw_factor();
    uint32_t wide_bits;
    std::memcpy(&wide_bits, &wide, 4);
    std::lock_guard lk(g_cam_lock);
    size_t valid = 0;
    for (uint64_t a : g_cams) {
        const uint64_t heap_end = kHeapBase + heap_size();
        if (a < kHeapBase + 8 || a + 4 > heap_end) continue;
        auto* p = reinterpret_cast<volatile uint32_t*>(a);
        if (p[-2] == 0x447a0000u && p[-1] == 0x3f490fdbu) {
            ++valid;
            if (*p == asp_bits) *p = wide_bits;
        }
    }
    if (valid < g_cams.size() || valid < 16) g_cam_rescan_needed.store(true, std::memory_order_release);
}

bool g_uw_wide = false;

void uw_span(int32_t& x, int32_t& w, uint32_t extent_w) {
    int64_t x0 = std::llround(double(x) * uw_factor());
    int64_t ww = int64_t(uw_widen(uint32_t(std::max(0, w))));
    if (std::llabs(int64_t(extent_w) - ww) <= 2 && x == 0) ww = extent_w;  // the whole target
    if (x0 + ww > int64_t(extent_w)) ww = std::max<int64_t>(0, int64_t(extent_w) - x0);
    x = int32_t(x0), w = int32_t(ww);
}

void uw_ui_vertex_shader(const ShaderInfo* vs) {
    if (!vs || __atomic_load_n(&vs->uw_ui_vs, __ATOMIC_RELAXED)) return;
    if (__atomic_exchange_n(&vs->uw_ui_vs, 1, __ATOMIC_ACQ_REL) == 0)
        log("nvn: ultrawide: UI vertex shader %s", vs->hash.c_str());
}
bool uw_is_ui_vertex_shader(const ShaderInfo* vs) {
    return vs && __atomic_load_n(&vs->uw_ui_vs, __ATOMIC_RELAXED) != 0;
}

bool uw_vs_scaled(const ShaderInfo* vs) {
    if (!vs) return false;
    if (uint8_t cached = __atomic_load_n(&vs->uw_scaled_cache, __ATOMIC_RELAXED)) return cached == 2;
    static const std::string off = std::getenv("NX_UW_NOSCALE") ? std::getenv("NX_UW_NOSCALE") : "";
    static const std::string only = std::getenv("NX_UW_SCALE") ? std::getenv("NX_UW_SCALE") : "";
    bool scaled = false;
    for (const DescriptorBinding& b : vs->refl.bindings)
        if (b.type == DescriptorType::UniformBuffer && b.name != "support_buffer") scaled = true;
    if (!off.empty() && off.find(vs->hash) != std::string::npos) scaled = false;
    if (!only.empty()) scaled = only.find(vs->hash) != std::string::npos;
    __atomic_store_n(&vs->uw_scaled_cache, uint8_t(scaled ? 2 : 1), __ATOMIC_RELAXED);
    return scaled;
}

bool uw_fragcoord_x_on() {
    if (!uw_on()) return false;
    static const bool off = std::getenv("NX_UW_FRAGCOORD_X") && std::getenv("NX_UW_FRAGCOORD_X")[0] == '0';
    return !off;
}


// ---- gl_Position.x * spec constant (modelled on patch_clip_z, nvn_spirv_clip.cpp) -----------------------------------

namespace {
enum : uint32_t {
    OpEntryPoint = 15, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypePointer = 32, OpSpecConstant = 50,
    OpFunction = 54, OpFunctionEnd = 56, OpVariable = 59, OpLoad = 61, OpStore = 62, OpAccessChain = 65,
    OpDecorate = 71, OpMemberDecorate = 72, OpCompositeExtract = 81, OpCompositeInsert = 82, OpConstant = 43,
    OpFAdd = 129, OpFMul = 133, OpFDiv = 136, OpReturn = 253, OpStructType = 30,
    DecorationBuiltIn = 11, DecorationSpecId = 1, DecorationBinding = 33, DecorationDescriptorSet = 34,
    BuiltInPosition = 0, BuiltInFragCoord = 15, StorageUniform = 2, StorageOutput = 3,
    ModelVertex = 0, ModelFragment = 4,
};
bool is_header_op(uint32_t op) {  // before the types: capabilities .. annotations
    switch (op) {
        case 17: case 10: case 11: case 14: case 15: case 16: case 330: case 3: case 4: case 5: case 6: case 7: case 8:
        case 71: case 72: case 73: case 74: case 75: return true;
        default: return false;
    }
}
}  // namespace

bool patch_x_scale(const std::vector<uint32_t>& in, std::vector<uint32_t>& out, float scale, float offset) {
    if (in.size() < 5) return false;
    struct Inst { size_t at; uint32_t op, len; };
    std::vector<Inst> insts;
    for (size_t i = 5; i < in.size();) {
        uint32_t len = in[i] >> 16;
        if (!len || i + len > in.size()) return false;
        insts.push_back({i, in[i] & 0xffff, len});
        i += len;
    }
    uint32_t bound = in[3], entry = 0, f32 = 0, u32 = 0, v4f = 0, ptr_v4f = 0, zero = 0;
    uint32_t block = 0, member = 0, position_var = 0;
    std::map<uint32_t, uint32_t> struct_ptr;
    size_t first_function = in.size(), first_type = in.size();
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (first_type == in.size() && !is_header_op(n.op)) first_type = n.at;
        if (n.op == OpEntryPoint && n.len >= 3 && w[1] == ModelVertex && !entry) entry = w[2];
        else if (n.op == OpMemberDecorate && n.len >= 5 && w[3] == DecorationBuiltIn && w[4] == BuiltInPosition)
            block = w[1], member = w[2];
        else if (n.op == OpDecorate && n.len >= 4 && w[2] == DecorationBuiltIn && w[3] == BuiltInPosition)
            position_var = w[1];
        else if (n.op == OpTypeFloat && n.len == 3 && w[2] == 32) f32 = w[1];
        else if (n.op == OpTypeInt && n.len == 4 && w[2] == 32 && w[3] == 0) u32 = w[1];
        else if (n.op == OpTypeVector && n.len == 4 && w[2] == f32 && w[3] == 4) v4f = w[1];
        else if (n.op == OpTypePointer && n.len == 4 && w[2] == StorageOutput) {
            if (w[3] == v4f && v4f) ptr_v4f = w[1];
            struct_ptr[w[1]] = w[3];
        } else if (n.op == OpConstant && n.len == 4 && u32 && w[1] == u32 && w[3] == member) {
            zero = w[2];
        } else if (n.op == OpFunction && first_function == in.size()) {
            first_function = n.at;
        }
    }
    if (!entry || !f32 || !v4f || first_type == in.size()) return false;
    uint32_t var = 0;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op != OpVariable || n.len < 4 || w[3] != StorageOutput) continue;
        auto p = struct_ptr.find(w[1]);
        if (p == struct_ptr.end()) continue;
        if (block && p->second == block) var = w[2];
        if (!block && w[2] == position_var && p->second == v4f) var = w[2];
    }
    if (!var) return false;
    uint32_t scale_constant = bound++, offset_constant = bound++;
    uint32_t scale_bits = 0, offset_bits = 0;
    std::memcpy(&scale_bits, &scale, sizeof scale_bits);
    std::memcpy(&offset_bits, &offset, sizeof offset_bits);
    std::vector<uint32_t> globals;
    if (!ptr_v4f) ptr_v4f = bound++, globals.insert(globals.end(), {(4u << 16) | OpTypePointer, ptr_v4f, StorageOutput, v4f});
    if (block) {
        if (!u32) u32 = bound++, globals.insert(globals.end(), {(4u << 16) | OpTypeInt, u32, 32, 0});
        if (!zero) zero = bound++, globals.insert(globals.end(), {(4u << 16) | OpConstant, u32, zero, member});
    }
    globals.insert(globals.end(), {(4u << 16) | OpConstant, f32, scale_constant, scale_bits,
                                   (4u << 16) | OpConstant, f32, offset_constant, offset_bits});

    out.assign(in.begin(), in.begin() + 5);
    uint32_t function = 0;
    int returns = 0;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.at == first_function) out.insert(out.end(), globals.begin(), globals.end());
        if (n.op == OpFunction) function = w[2];
        if (n.op == OpReturn && function == entry) {
            uint32_t p = var;
            if (block) {
                p = bound++;
                out.insert(out.end(), {(5u << 16) | OpAccessChain, ptr_v4f, p, var, zero});
            }
            uint32_t v = bound++, x = bound++, m = bound++, scaled = bound++, r = bound++;
            out.insert(out.end(), {(4u << 16) | OpLoad, v4f, v, p});
            out.insert(out.end(), {(5u << 16) | OpCompositeExtract, f32, x, v, 0});
            out.insert(out.end(), {(5u << 16) | OpFMul, f32, m, x, scale_constant,
                                   (5u << 16) | OpFAdd, f32, scaled, m, offset_constant,
                                   (6u << 16) | OpCompositeInsert, v4f, r, scaled, v, 0});
            out.insert(out.end(), {(3u << 16) | OpStore, p, r});
            returns++;
        }
        if (n.op == OpFunctionEnd) function = 0;
        out.insert(out.end(), w, w + n.len);
    }
    if (!returns) return false;
    out[3] = bound;
    return true;
}

bool patch_fragcoord_x_scale(const std::vector<uint32_t>& in, std::vector<uint32_t>& out) {
    if (in.size() < 5) return false;
    struct Inst { size_t at; uint32_t op, len; };
    std::vector<Inst> insts;
    insts.reserve(in.size() / 3);
    for (size_t i = 5; i < in.size();) {
        const uint32_t len = in[i] >> 16;
        if (!len || i + len > in.size()) return false;
        insts.push_back({i, in[i] & 0xffff, len});
        i += len;
    }

    uint32_t bound = in[3], fc_var = 0, f32 = 0, i32 = 0, u32 = 0, v4f = 0, ptr_uniform_f32 = 0;
    bool is_fragment = false;
    std::unordered_map<uint32_t, uint32_t> dec_set, dec_bind, int_const, i32_const;
    std::unordered_map<uint32_t, std::vector<uint32_t>> struct_members;
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> ptr_pointee;
    size_t first_function = in.size();

    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op == OpEntryPoint && n.len >= 3 && w[1] == ModelFragment) {
            is_fragment = true;
        } else if (n.op == OpDecorate && n.len >= 4) {
            if (w[2] == DecorationBuiltIn && w[3] == BuiltInFragCoord) fc_var = w[1];
            else if (w[2] == DecorationDescriptorSet) dec_set[w[1]] = w[3];
            else if (w[2] == DecorationBinding) dec_bind[w[1]] = w[3];
        } else if (n.op == OpTypeFloat && n.len == 3 && w[2] == 32) {
            f32 = w[1];
        } else if (n.op == OpTypeInt && n.len == 4 && w[2] == 32) {
            if (w[3] == 1) i32 = w[1];
            else if (w[3] == 0) u32 = w[1];
        } else if (n.op == OpTypeVector && n.len == 4 && f32 && w[2] == f32 && w[3] == 4) {
            v4f = w[1];
        } else if (n.op == OpStructType && n.len >= 2) {
            struct_members.emplace(w[1], std::vector<uint32_t>(w + 2, w + n.len));
        } else if (n.op == OpTypePointer && n.len == 4) {
            ptr_pointee[w[1]] = {w[2], w[3]};
            if (w[2] == StorageUniform && f32 && w[3] == f32) ptr_uniform_f32 = w[1];
        } else if (n.op == OpConstant && n.len == 4 && ((i32 && w[1] == i32) || (u32 && w[1] == u32))) {
            int_const[w[2]] = w[3];
            if (i32 && w[1] == i32) i32_const[w[3]] = w[2];
        } else if (n.op == OpFunction && first_function == in.size()) {
            first_function = n.at;
        }
    }
    if (!is_fragment || !fc_var || !f32 || !v4f || first_function == in.size()) return false;

    uint32_t sb_var = 0;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op != OpVariable || n.len < 4 || w[3] != StorageUniform) continue;
        auto b = dec_bind.find(w[2]);
        if (b == dec_bind.end() || b->second != 0) continue;
        auto s = dec_set.find(w[2]);
        if (s != dec_set.end() && s->second != 0) continue;
        auto pt = ptr_pointee.find(w[1]);
        if (pt == ptr_pointee.end() || pt->second.first != StorageUniform) continue;
        auto sm = struct_members.find(pt->second.second);
        if (sm != struct_members.end() && sm->second.size() > 3 && sm->second[3] == v4f) {
            sb_var = w[2];
            break;
        }
    }
    if (!sb_var) return false;

    struct AccessChain {
        uint32_t base = 0;
        uint32_t count = 0;
        uint32_t idx[2] = {};
    };
    std::unordered_map<uint32_t, AccessChain> ac;
    std::unordered_map<uint32_t, uint32_t> ld;
    std::unordered_set<uint32_t> fc_x_vals;
    std::unordered_set<size_t> target_fdivs;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op == OpAccessChain && (n.len == 5 || n.len == 6)) {
            auto i0 = int_const.find(w[4]);
            if (i0 == int_const.end()) continue;
            AccessChain chain{w[3], n.len - 4, {i0->second, 0}};
            if (n.len == 6) {
                auto i1 = int_const.find(w[5]);
                if (i1 == int_const.end()) continue;
                chain.idx[1] = i1->second;
            }
            ac[w[2]] = chain;
        } else if (n.op == OpLoad && n.len >= 4) {
            ld[w[2]] = w[3];
            auto a = ac.find(w[3]);
            if (a != ac.end() && a->second.base == fc_var && a->second.count == 1 && a->second.idx[0] == 0)
                fc_x_vals.insert(w[2]);
        } else if (n.op == OpFDiv && n.len == 5 && w[1] == f32 && fc_x_vals.count(w[3])) {
            auto l = ld.find(w[4]);
            if (l == ld.end()) continue;
            auto a = ac.find(l->second);
            if (a != ac.end() && a->second.base == sb_var && a->second.count == 2 && a->second.idx[0] == 5 &&
                a->second.idx[1] == 0) {
                target_fdivs.insert(n.at);
            }
        }
    }
    if (target_fdivs.empty()) return false;

    std::vector<uint32_t> globals;
    if (!i32) {
        i32 = bound++;
        globals.insert(globals.end(), {(4u << 16) | OpTypeInt, i32, 32, 1});
    }
    if (!ptr_uniform_f32) {
        ptr_uniform_f32 = bound++;
        globals.insert(globals.end(), {(4u << 16) | OpTypePointer, ptr_uniform_f32, StorageUniform, f32});
    }
    auto find_or_add_i32 = [&](uint32_t val) {
        if (auto it = i32_const.find(val); it != i32_const.end()) return it->second;
        const uint32_t id = bound++;
        globals.insert(globals.end(), {(4u << 16) | OpConstant, i32, id, val});
        return id;
    };
    const uint32_t c3 = find_or_add_i32(3);
    const uint32_t c2 = find_or_add_i32(2);

    out.clear();
    out.reserve(in.size() + globals.size() + target_fdivs.size() * 10);
    out.insert(out.end(), in.begin(), in.begin() + 5);
    for (const Inst& n : insts) {
        if (n.at == first_function && !globals.empty()) out.insert(out.end(), globals.begin(), globals.end());
        const uint32_t* w = &in[n.at];
        if (target_fdivs.count(n.at)) {
            const uint32_t ptr_id = bound++;
            const uint32_t scale_id = bound++;
            out.insert(out.end(), {(6u << 16) | OpAccessChain, ptr_uniform_f32, ptr_id, sb_var, c3, c2,
                                   (4u << 16) | OpLoad, f32, scale_id, ptr_id,
                                   w[0], w[1], w[2], w[3], scale_id});
        } else {
            out.insert(out.end(), w, w + n.len);
        }
    }
    out[3] = bound;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed))
        log("nvn: ultrawide: fragment gl_FragCoord.x scaling enabled (NX_UW_FRAGCOORD_X=0 to disable)");
    return true;
}

}  // namespace nx::nvn
