// Integer textures sampled through float declarations (nvn_pipeline.h, "Integer textures").
//
// The offline translator answers every texture-format query with RGBA8 unorm (tools/maxwell/spirv_harness,
// StaticGpuAccessor), so every texture binding is a float sampler. On Maxwell a texture of an integer format returns
// the integer's bits in the register, and the code reinterprets them: 20 of Wonder's vertex shaders read an R16_UI
// texture as `float(floatBitsToUint(texelFetch(vp_t_tcb_A, ...).x))`. In Vulkan a float sampler on a UINT or SINT view
// is undefined (VUID-vkCmdDrawIndexed-format-07753): NVIDIA returned the bits, AMD did not.
//
// The draws record the bindings sampled with integer views (gather_textures, nvn_sets.cpp). Their modules declare those
// bindings with an integer sampled type, and every sample, fetch or gather of them returns an integer vector that is
// bitcast back to the float vector the rest of the code expects: the Maxwell semantics exactly. Pipelines of a shader
// are rebuilt when it gains a record (int_texture_generation in their cache key). NX_INT_TEXTURES=0: off (bisecting).
#include <algorithm>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "nvn_pipeline.h"

namespace nx::nvn {

namespace {

enum : uint32_t {
    OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypeImage = 25, OpTypeSampledImage = 27,
    OpTypePointer = 32, OpFunction = 54, OpVariable = 59, OpLoad = 61, OpDecorate = 71, OpSampledImage = 86,
    OpImageSampleImplicitLod = 87, OpImageSampleExplicitLod = 88, OpImageSampleProjImplicitLod = 91,
    OpImageSampleProjExplicitLod = 92, OpImageFetch = 95, OpImageGather = 96, OpImage = 100, OpBitcast = 124,
    DecorationBinding = 33, DecorationDescriptorSet = 34, StorageUniformConstant = 0,
};

struct Records {
    std::mutex lock;
    std::map<const ShaderInfo*, std::map<uint32_t, uint32_t>> kinds;  // shader -> binding -> kind
    std::map<const ShaderInfo*, uint32_t> generation;
};
Records& records() {
    static Records r;
    return r;
}

bool enabled() {
    static const bool on = [] {
        const char* e = std::getenv("NX_INT_TEXTURES");
        return !(e && e[0] == '0');
    }();
    return on;
}

}  // namespace

uint32_t vk_int_kind(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R16_UINT:
        case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R32_UINT:
        case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32A32_UINT:
        case VK_FORMAT_A2B10G10R10_UINT_PACK32: case VK_FORMAT_S8_UINT:
            return 1;
        case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R16_SINT:
        case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32_SINT:
        case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32_SINT: case VK_FORMAT_R32G32B32A32_SINT:
            return 2;
        default:
            return 0;
    }
}

bool note_int_texture(const ShaderInfo* s, uint32_t binding, uint32_t kind) {
    if (!s || !kind || !enabled()) return false;
    Records& r = records();
    std::lock_guard lk(r.lock);
    uint32_t& k = r.kinds[s][binding];
    if (k == kind) return false;
    k = kind;
    r.generation[s]++;
    __atomic_add_fetch(&s->int_tex_gen, 1u, __ATOMIC_RELEASE);
    static int logged = 0;
    if (logged++ < 40)
        log("nvn/vk: shader %s binding %u samples %s integer texture: declared as such", s->hash.c_str(), binding,
            kind == 2 ? "a signed" : "an unsigned");
    return true;
}

uint32_t int_texture_kind(const ShaderInfo* s, uint32_t binding) {
    Records& r = records();
    std::lock_guard lk(r.lock);
    auto it = r.kinds.find(s);
    if (it == r.kinds.end()) return 0;
    auto b = it->second.find(binding);
    return b == it->second.end() ? 0 : b->second;
}

uint32_t int_texture_generation(const ShaderInfo* s) {
    return s ? __atomic_load_n(&s->int_tex_gen, __ATOMIC_ACQUIRE) : 0;
}

std::string int_texture_key(const ShaderInfo* s) {
    Records& r = records();
    std::lock_guard lk(r.lock);
    auto it = r.kinds.find(s);
    if (it == r.kinds.end()) return {};
    std::string key;
    for (auto [binding, kind] : it->second) key += std::to_string(binding) + ":" + std::to_string(kind) + ",";
    return key;
}

std::map<uint32_t, uint32_t> int_texture_kinds(const ShaderInfo* s) {
    Records& r = records();
    std::lock_guard lk(r.lock);
    auto it = r.kinds.find(s);
    return it == r.kinds.end() ? std::map<uint32_t, uint32_t>{} : it->second;
}

bool patch_int_textures(const std::vector<uint32_t>& in, const std::map<uint32_t, uint32_t>& kinds,
                        std::vector<uint32_t>& out) {
    if (in.size() < 5 || kinds.empty()) return false;
    struct Inst {
        size_t at;
        uint32_t op, len;
    };
    std::vector<Inst> insts;
    for (size_t i = 5; i < in.size();) {
        uint32_t len = in[i] >> 16;
        if (!len || i + len > in.size()) return false;
        insts.push_back({i, in[i] & 0xffff, len});
        i += len;
    }
    // Declarations: bindings, sets, and every type a new declaration may reuse (SPIR-V forbids duplicates).
    std::map<uint32_t, uint32_t> binding, set, defined_at;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> ints, vecs;                          // (width|sign) / (comp, n)
    std::map<std::vector<uint32_t>, uint32_t> images;                                      // operands -> id
    std::map<uint32_t, std::vector<uint32_t>> image_ops;                                   // id -> operands
    std::map<uint32_t, uint32_t> sampled_of, sampled_image_type;                           // SI -> image, image -> SI
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> ptr;                                 // id -> (storage, type)
    uint32_t f32 = 0;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op == OpFunction) break;
        if (n.op == OpDecorate && n.len >= 4 && w[2] == DecorationBinding) binding[w[1]] = w[3];
        else if (n.op == OpDecorate && n.len >= 4 && w[2] == DecorationDescriptorSet) set[w[1]] = w[3];
        else if (n.op == OpTypeInt && n.len == 4) ints[{w[2], w[3]}] = w[1], defined_at[w[1]] = n.at;
        else if (n.op == OpTypeFloat && n.len == 3 && w[2] == 32) f32 = w[1];
        else if (n.op == OpTypeVector && n.len == 4) vecs[{w[2], w[3]}] = w[1], defined_at[w[1]] = n.at;
        else if (n.op == OpTypeImage && n.len >= 9) {
            std::vector<uint32_t> ops(w + 2, w + n.len);
            images[ops] = w[1], image_ops[w[1]] = ops, defined_at[w[1]] = n.at;
        } else if (n.op == OpTypeSampledImage && n.len == 3) {
            sampled_of[w[1]] = w[2], sampled_image_type[w[2]] = w[1], defined_at[w[1]] = n.at;
        } else if (n.op == OpTypePointer && n.len == 4) {
            ptr[w[1]] = {w[2], w[3]};
        }
    }
    // The variables to patch: set 2 bindings in `kinds` whose type is a sampled float image.
    std::map<uint32_t, uint32_t> var_kind;  // variable -> kind
    size_t insert_at = in.size();
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op == OpFunction) break;
        if (n.op != OpVariable || n.len < 4 || w[3] != StorageUniformConstant) continue;
        auto b = binding.find(w[2]);
        auto s = set.find(w[2]);
        if (b == binding.end() || s == set.end() || s->second != 2) continue;
        auto k = kinds.find(b->second);
        if (k == kinds.end() || !k->second) continue;
        auto p = ptr.find(w[1]);
        if (p == ptr.end() || !sampled_of.count(p->second.second)) continue;
        auto img = image_ops.find(sampled_of[p->second.second]);
        if (img == image_ops.end() || img->second[0] != f32) continue;  // only float declarations
        var_kind[w[2]] = k->second;
        insert_at = std::min(insert_at, n.at);
    }
    if (var_kind.empty()) return false;

    uint32_t bound = in[3];
    std::vector<uint32_t> types;  // declarations inserted before the first patched variable
    auto reuse_ok = [&](uint32_t id) { return !defined_at.count(id) || defined_at[id] < insert_at; };
    bool ok = true;
    auto int_type = [&](uint32_t sign) {
        auto it = ints.find({32, sign});
        if (it != ints.end()) return ok &= reuse_ok(it->second), it->second;
        uint32_t id = bound++;
        types.insert(types.end(), {(4u << 16) | OpTypeInt, id, 32, sign});
        return ints[{32, sign}] = id;
    };
    auto vec4_type = [&](uint32_t comp) {
        auto it = vecs.find({comp, 4});
        if (it != vecs.end()) return ok &= reuse_ok(it->second), it->second;
        uint32_t id = bound++;
        types.insert(types.end(), {(4u << 16) | OpTypeVector, id, comp, 4});
        return vecs[{comp, 4}] = id;
    };
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> new_image, new_si;  // (float image, kind) -> int image / SI
    auto int_image = [&](uint32_t float_image, uint32_t kind) {
        auto key = std::make_pair(float_image, kind);
        if (auto it = new_image.find(key); it != new_image.end()) return it->second;
        std::vector<uint32_t> ops = image_ops[float_image];
        ops[0] = int_type(kind == 2 ? 1 : 0);
        uint32_t id;
        if (auto it = images.find(ops); it != images.end()) {
            id = it->second, ok &= reuse_ok(id);
        } else {
            id = bound++;
            types.push_back(uint32_t(2 + ops.size()) << 16 | OpTypeImage);
            types.push_back(id);
            types.insert(types.end(), ops.begin(), ops.end());
            images[ops] = id, image_ops[id] = ops;
        }
        return new_image[key] = id;
    };
    auto int_si = [&](uint32_t float_image, uint32_t kind) {
        auto key = std::make_pair(float_image, kind);
        if (auto it = new_si.find(key); it != new_si.end()) return it->second;
        uint32_t image = int_image(float_image, kind), id;
        if (auto it = sampled_image_type.find(image); it != sampled_image_type.end()) {
            id = it->second, ok &= reuse_ok(id);
        } else {
            id = bound++;
            types.insert(types.end(), {(3u << 16) | OpTypeSampledImage, id, image});
            sampled_image_type[image] = id;
        }
        return new_si[key] = id;
    };
    // Values derived from a patched variable: id -> (kind, float image type it came from).
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> sampled_img_val, image_value;
    std::map<uint32_t, uint32_t> var_ptr;  // patched variable -> its new pointer type
    for (auto [var, kind] : var_kind) {
        for (const Inst& n : insts)
            if (n.op == OpVariable && in[n.at + 2] == var) {
                uint32_t float_image = sampled_of[ptr[in[n.at + 1]].second];
                uint32_t si = int_si(float_image, kind);  // may declare types: before the pointer that uses it
                uint32_t p = bound++;
                types.insert(types.end(), {(4u << 16) | OpTypePointer, p, StorageUniformConstant, si});
                var_ptr[var] = p;
                sampled_img_val[var] = {kind, float_image};  // the variable itself, as the pointer loads come from
                break;
            }
    }
    for (auto [var, kind] : var_kind) vec4_type(int_type(kind == 2 ? 1 : 0));  // the results' types, declared now
    if (!ok) {
        log("nvn/vk: integer texture patch skipped: a type it would reuse is declared after the variables");
        return false;
    }
    std::set<uint32_t> loaded_from_var;
    out.clear();
    out.reserve(in.size() + types.size() + 64);
    out.insert(out.end(), in.begin(), in.begin() + 5);
    for (const Inst& n : insts) {
        if (n.at == insert_at) out.insert(out.end(), types.begin(), types.end());
        std::vector<uint32_t> w(in.begin() + n.at, in.begin() + n.at + n.len);
        if (n.op == OpVariable && var_ptr.count(w[2])) {
            w[1] = var_ptr[w[2]];
        } else if (n.op == OpLoad && n.len >= 4 && var_ptr.count(w[3])) {
            auto [kind, fi] = sampled_img_val[w[3]];
            w[1] = int_si(fi, kind);
            sampled_img_val[w[2]] = {kind, fi};
        } else if (n.op == OpImage && n.len >= 4 && sampled_img_val.count(w[3])) {
            auto [kind, fi] = sampled_img_val[w[3]];
            w[1] = int_image(fi, kind);
            image_value[w[2]] = {kind, fi};
        } else if (n.op == OpSampledImage && n.len >= 5 && image_value.count(w[3])) {
            auto [kind, fi] = image_value[w[3]];
            w[1] = int_si(fi, kind);
            sampled_img_val[w[2]] = {kind, fi};
        } else if ((n.op == OpImageSampleImplicitLod || n.op == OpImageSampleExplicitLod ||
                    n.op == OpImageSampleProjImplicitLod || n.op == OpImageSampleProjExplicitLod ||
                    n.op == OpImageGather) && n.len >= 5 && sampled_img_val.count(w[3])) {
            uint32_t kind = sampled_img_val[w[3]].first, result = w[2], float_type = w[1];
            w[1] = vec4_type(int_type(kind == 2 ? 1 : 0)), w[2] = bound++;
            out.insert(out.end(), w.begin(), w.end());
            out.insert(out.end(), {(4u << 16) | OpBitcast, float_type, result, w[2]});
            continue;
        } else if (n.op == OpImageFetch && n.len >= 5 && image_value.count(w[3])) {
            uint32_t kind = image_value[w[3]].first, result = w[2], float_type = w[1];
            w[1] = vec4_type(int_type(kind == 2 ? 1 : 0)), w[2] = bound++;
            out.insert(out.end(), w.begin(), w.end());
            out.insert(out.end(), {(4u << 16) | OpBitcast, float_type, result, w[2]});
            continue;
        }
        out.insert(out.end(), w.begin(), w.end());
    }
    if (!ok) return false;  // a vector type created late would follow its use: never happens (made above)
    out[3] = bound;
    return true;
}

}  // namespace nx::nvn
