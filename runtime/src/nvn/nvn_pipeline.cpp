// Pipelines and sampled resources (nvn_pipeline.h). Everything is cached for the life of the process: layouts by
// their bindings, pipelines by their full state, samplers by their state, views by (image, type).
#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>

#include "nvn_pipeline.h"
#include "nvn_pipeline_async.h"
#include "platform.h"
#include "shadow_coord_debug_spv.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {

// Pipeline recipes are portable within this runtime version and GPU family. They contain shader hashes and the
// pointer-free part of PipelineKey; Vulkan's own pipeline cache separately stores the driver's compiled data.
constexpr char kPipelineKeysMagic[8] = {'W', 'O', 'N', 'P', 'I', 'P', '0', '1'};
constexpr uint32_t kPipelineKeysVersion = 1;
constexpr uint64_t kMaxPipelineRecords = 100000;
struct PipelineKeysHeader {
    char magic[8]{};
    uint32_t version = kPipelineKeysVersion;
    uint32_t record_size = 0;
    uint64_t count = 0;
    uint32_t vendor = 0, device = 0;
};
struct PipelineKeysRecord {
    char vs_hash[16]{}, fs_hash[16]{};  // fragment hash all zero: depth-only pipeline
    uint32_t vs_shared = 0, fs_shared = 0;
    uint32_t vs_local[3] = {1, 1, 1}, fs_local[3] = {1, 1, 1};
    PipelineKey key{};  // vs/fs pointers are always null on disk
};
static_assert(std::is_trivially_copyable_v<PipelineKeysHeader> && std::is_trivially_copyable_v<PipelineKeysRecord>);

std::vector<PipelineKeysRecord> g_pipeline_key_records;
std::set<std::string> g_pipeline_key_ids;
std::map<std::string, std::unique_ptr<ShaderInfo>> g_pipeline_warm_shaders;
std::string g_pipeline_keys_path;
uint32_t g_pipeline_vendor = 0, g_pipeline_device = 0;
bool g_pipeline_keys_loaded = false, g_pipeline_keys_dirty = false;

std::string pipeline_record_id(const PipelineKeysRecord& rec) {
    std::string id(rec.vs_hash, sizeof rec.vs_hash);
    id.append(rec.fs_hash, sizeof rec.fs_hash);
    id.append(reinterpret_cast<const char*>(&rec.key), sizeof rec.key);
    return id;
}

bool valid_pipeline_hash(const char hash[16]) {
    for (int i = 0; i < 16; ++i)
        if (!((hash[i] >= '0' && hash[i] <= '9') || (hash[i] >= 'a' && hash[i] <= 'f'))) return false;
    return true;
}

bool open_pipeline_keys_path() {
    if (!g_pipeline_keys_path.empty()) return true;
    VkPhysicalDeviceProperties props{};
    p_vkGetPhysicalDeviceProperties(vk_physical(), &props);
    g_pipeline_vendor = props.vendorID, g_pipeline_device = props.deviceID;
    char filename[80];
    // GPL dynamic mode keys leave out the dynamic state (normalized_key): another file, so neither mode prewarms
    // the other's pipelines.
    std::snprintf(filename, sizeof filename, "/pipeline_keys_%s%04x_%04x.bin", gpl_dynamic_state() ? "gpl_" : "",
                  props.vendorID, props.deviceID);
    g_pipeline_keys_path = plat::data_root() + filename;
    return true;
}

void load_pipeline_keys() {
    if (g_pipeline_keys_loaded) return;
    g_pipeline_keys_loaded = true;
    open_pipeline_keys_path();
    FILE* f = plat::fopen_utf8(g_pipeline_keys_path, "rb");
    if (!f) return;
    plat::fseek64(f, 0, SEEK_END);
    const int64_t length = plat::ftell64(f);
    plat::fseek64(f, 0, SEEK_SET);
    PipelineKeysHeader h{};
    bool valid = length >= int64_t(sizeof h) && length <= (128ll << 20) &&
                 std::fread(&h, 1, sizeof h, f) == sizeof h && std::memcmp(h.magic, kPipelineKeysMagic, 8) == 0 &&
                 h.version == kPipelineKeysVersion && h.record_size == sizeof(PipelineKeysRecord) &&
                 h.vendor == g_pipeline_vendor && h.device == g_pipeline_device && h.count <= kMaxPipelineRecords &&
                 uint64_t(length) == sizeof h + h.count * sizeof(PipelineKeysRecord);
    if (valid) {
        g_pipeline_key_records.resize(size_t(h.count));
        valid = h.count == 0 || std::fread(g_pipeline_key_records.data(), sizeof(PipelineKeysRecord), size_t(h.count), f) == h.count;
    }
    std::fclose(f);
    if (!valid) {
        g_pipeline_key_records.clear();
        log("nvn/vk: recorded pipeline keys are invalid or from another runtime version; starting fresh");
        return;
    }
    for (auto& rec : g_pipeline_key_records) {
        rec.key.vs = rec.key.fs = nullptr;  // never trust pointer bytes from disk, even in a versioned local cache
        g_pipeline_key_ids.insert(pipeline_record_id(rec));
    }
    log("nvn/vk: loaded %llu recorded pipeline recipes from %s", (unsigned long long)g_pipeline_key_records.size(),
        g_pipeline_keys_path.c_str());
}

ShaderInfo* load_pipeline_shader(const char hash[16], uint32_t shared, const uint32_t local[3]) {
    if (!valid_pipeline_hash(hash)) return nullptr;
    const std::string h(hash, 16);
    if (h == std::string(16, '\0')) return nullptr;
    std::string identity = h;
    identity.append(reinterpret_cast<const char*>(&shared), sizeof shared);
    identity.append(reinterpret_cast<const char*>(local), sizeof(uint32_t) * 3);
    if (auto it = g_pipeline_warm_shaders.find(identity); it != g_pipeline_warm_shaders.end()) return it->second.get();
    const std::string path = plat::data_root() + "/spirv/" + h + ".spv";
    FILE* f = plat::fopen_utf8(path, "rb");
    if (!f) return nullptr;
    plat::fseek64(f, 0, SEEK_END);
    const int64_t bytes = plat::ftell64(f);
    plat::fseek64(f, 0, SEEK_SET);
    if (bytes < 20 || bytes > (64ll << 20) || (bytes & 3)) { std::fclose(f); return nullptr; }
    auto info = std::make_unique<ShaderInfo>();
    info->hash = h;
    info->spirv.resize(size_t(bytes) / 4);
    const bool read = std::fread(info->spirv.data(), 1, size_t(bytes), f) == size_t(bytes);
    std::fclose(f);
    if (!read || !reflect_spirv(info->spirv.data(), info->spirv.size(), info->refl)) return nullptr;
    info->stage = info->refl.stage;
    info->control.valid = true;
    info->control.shared_size = shared;
    for (int i = 0; i < 3; ++i) info->control.local_size[i] = info->local_size[i] = local[i] ? local[i] : 1;
    ShaderInfo* result = info.get();
    g_pipeline_warm_shaders.emplace(std::move(identity), std::move(info));
    return result;
}

}  // namespace

bool shadow_coord_debug_on() {
    static const bool enabled = [] {
        const char* e = std::getenv("NX_SHADOW_COORD_DEBUG");
        return e && e[0] == '1';
    }();
    return enabled;
}

// NVN views carry a format: the game renders into RGBA8 textures through sRGB views (the GPU encodes on write and
// blends in linear) and samples them the same way. Ignoring it lost one conversion per draw: far too much contrast
// and saturation (docs/nvn/DEBUG.md #29).
VkFormat view_format(const GpuImage& img, uint32_t nvn) {
    if (VkFormat f; bc_view_format(img, nvn, f)) return f;  // decoded BC (nvn_bc.cpp)
    if (!nvn || nvn == img.nvn_format || img.alpha_from_r || img.aspect != VK_IMAGE_ASPECT_COLOR_BIT) return img.format;
    VkFormat f = vk_format(nvn);
    uint32_t bw = 1, bh = 1, bytes = 0, iw = 1, ih = 1, ibytes = 0;
    format_block(nvn, bw, bh, bytes);
    format_block(img.nvn_format, iw, ih, ibytes);
    // The sizes of the host formats: R11G11B10F is 4 bytes in NVN but RGBA16F here, and a 4-byte view (RGBA8, R32F)
    // of an 8-byte image is no valid view (G4, docs/nvn/DEBUG.md #50). NX_VIEW_NVN_SIZES=1: the NVN sizes (bisecting).
    static const bool nvn_sizes = std::getenv("NX_VIEW_NVN_SIZES") != nullptr;
    if (!nvn_sizes && vk_block_bytes(f) && vk_block_bytes(img.format))
        bytes = vk_block_bytes(f), ibytes = vk_block_bytes(img.format);
    bool ok = f != VK_FORMAT_UNDEFINED && format_aspect(nvn) == 0 && bw == iw && bh == ih && bytes == ibytes;
    if (ok && f != img.format && !img.mutable_format) {  // ubwc_flags: an image made without MUTABLE_FORMAT
        static std::set<std::pair<uint32_t, uint32_t>> not_mutable;
        if (not_mutable.insert({img.nvn_format, nvn}).second)
            log("nvn/vk: texture format %#x viewed as %#x, but the image is not mutable: its own format", img.nvn_format,
                nvn);
        return img.format;
    }
    static std::set<std::pair<uint32_t, uint32_t>> logged;
    if (logged.insert({img.nvn_format, nvn}).second && logged.size() <= 40)
        log("nvn/vk: texture format %#x viewed as %#x%s", img.nvn_format, nvn, ok ? "" : " (not compatible: ignored)");
    return ok ? f : img.format;
}

VkImageViewType view_type(const ImageShape& s) {  // SPIR-V Dim: 0 1D, 1 2D, 2 3D, 3 Cube
    switch (s.dim) {
        case 0: return s.arrayed ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
        case 2: return VK_IMAGE_VIEW_TYPE_3D;
        case 3: return s.arrayed ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
        default: return s.arrayed ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    }
}

namespace {

VkShaderStageFlags stage_bit(ShaderStage s) {
    return s == ShaderStage::Vertex     ? VK_SHADER_STAGE_VERTEX_BIT
           : s == ShaderStage::Fragment ? VK_SHADER_STAGE_FRAGMENT_BIT
                                        : VK_SHADER_STAGE_COMPUTE_BIT;
}

// `kinds`: per vertex input location, 0 float or the integer kind of its attribute format (vertex_int_kind).
// `unwritten`: fragment input locations the vertex stage does not write (default_unwritten_inputs).
std::map<std::tuple<const ShaderInfo*, std::string, uint32_t, std::string>, VkShaderModule> g_modules;  // module_of

VkShaderModule module_of(const ShaderInfo* s, const uint8_t* kinds = nullptr, uint32_t unwritten = 0) {
    auto& cache = g_modules;
    // The integer-vertex-input patch (skinned models' bone indices; without it Metal refuses every character's
    // pipeline, DEBUG.md #25/#26). NX_INT_ATTRIBS=0 turns it off.
    static const char* int_env = std::getenv("NX_INT_ATTRIBS");
    static const bool off = int_env && int_env[0] == '0';
    std::string k = kinds && !off ? std::string(reinterpret_cast<const char*>(kinds), 16) : std::string(16, '\0');
    auto& m = cache[{s, k, unwritten, int_texture_key(s)}];  // integer textures (nvn_spirv_int_textures.cpp)
    if (!m) {
        bool is_patched = false;
        const uint8_t* int_kinds = k != std::string(16, '\0') ? reinterpret_cast<const uint8_t*>(k.data()) : nullptr;
        std::vector<uint32_t> code;
        if (shadow_coord_debug_on() && s->stage == ShaderStage::Fragment && s->hash == "1872caa47df3cd7a") {
            code.assign(std::begin(kShadowCoordDebugSpirv), std::end(kShadowCoordDebugSpirv));
            log("nvn/vk: NX_SHADOW_COORD_DEBUG replaces fragment shader %s with projected-coordinate view",
                s->hash.c_str());
        } else {
            code = patched_spirv(s, int_kinds, unwritten, &is_patched);
        }
        if (s->stage == ShaderStage::Vertex) clip_z_module(s, code);  // NX_CLIP_Z_PATCH (nvn_spirv_clip.cpp)
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = code.size() * 4;
        ci.pCode = code.data();
        if (p_vkCreateShaderModule(vk_device(), &ci, nullptr, &m) != VK_SUCCESS) log("nvn/vk: shader %s rejected", s->hash.c_str());
        static int logged = 0;
        if (is_patched && logged++ < 20) log("nvn/vk: shader %s: integer vertex inputs", s->hash.c_str());
    }
    return m;
}

VkDescriptorSetLayout set_layout(const std::vector<VkDescriptorSetLayoutBinding>& bindings) {
    static std::map<std::string, VkDescriptorSetLayout> cache;
    std::string key(reinterpret_cast<const char*>(bindings.data()), bindings.size() * sizeof(bindings[0]));
    auto& l = cache[key];
    if (!l) {
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = uint32_t(bindings.size());
        ci.pBindings = bindings.data();
        p_vkCreateDescriptorSetLayout(vk_device(), &ci, nullptr, &l);
    }
    return l;
}

// ---- images ---------------------------------------------------------------------------------------------------------

// Views made for sampling and for rendering, by image; forget_image_views() drops an image's before it is freed.
using SampledKey = std::tuple<VkImage, VkImageViewType, uint32_t, uint64_t, uint64_t>;
std::map<SampledKey, VkImageView> g_sampled_views;
std::map<std::tuple<VkImage, uint32_t, uint32_t, VkFormat>, VkImageView> g_attachment_views;
// In front of g_sampled_views (a tree walk per sampled texture of every draw): copies of entries it holds, by a hash
// of the key (view null: an empty slot). forget_image_views, the only place entries go, empties the image's slots.
struct ViewHit {
    SampledKey key{};
    VkImageView view = VK_NULL_HANDLE;
};
ViewHit g_view_hits[256];

bool identity(const VkComponentMapping& m) {
    auto id = [](VkComponentSwizzle s, VkComponentSwizzle own) { return s == VK_COMPONENT_SWIZZLE_IDENTITY || s == own; };
    return id(m.r, VK_COMPONENT_SWIZZLE_R) && id(m.g, VK_COMPONENT_SWIZZLE_G) && id(m.b, VK_COMPONENT_SWIZZLE_B) &&
           id(m.a, VK_COMPONENT_SWIZZLE_A);
}


// Views Metal accepts of an image: cubes only of cube images with 6 layers per cube, 3D only of 3D images (a 2D
// view of a 3D image is for rendering a slice), 1D of 1D (only with NX_1D_IMAGES: NVN 1D textures are 2D images of
// height 1, since the translator samples them as 2D, G8). A shader declaring another shape than the texture it gets
// (a samplerCube given a 2D texture) reads the dummy instead: Metal aborts on the view.
// A view in another format than its image inherits all the image's usage, storage included, which an sRGB (or other
// non-storage) view format cannot have (VUID-VkImageViewCreateInfo-usage-02275). Such views get only the usage they
// are used for. NX_VIEW_FULL_USAGE=1: inherit as before.
bool format_storage(VkFormat f) {
    static std::map<VkFormat, bool> known;
    auto it = known.find(f);
    if (it != known.end()) return it->second;
    VkFormatProperties fp{};
    p_vkGetPhysicalDeviceFormatProperties(vk_physical(), f, &fp);
    return known[f] = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
}
bool narrow_view_usage(const GpuImage& img, VkFormat format, VkImageUsageFlags usage, VkImageViewCreateInfo& ci,
                       VkImageViewUsageCreateInfo& u) {
    static const bool off = std::getenv("NX_VIEW_FULL_USAGE") != nullptr;
    if (off || format == img.format || !img.storage || format_storage(format)) return false;
    u = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    u.usage = usage;
    ci.pNext = &u;
    return true;
}

bool view_compatible(const GpuImage& img, VkImageViewType type, uint32_t layers) {
    bool cube_img = img.view_type == VK_IMAGE_VIEW_TYPE_CUBE || img.view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    bool img3d = img.view_type == VK_IMAGE_VIEW_TYPE_3D;
    bool img1d = img.view_type == VK_IMAGE_VIEW_TYPE_1D || img.view_type == VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    switch (type) {
        case VK_IMAGE_VIEW_TYPE_CUBE:
        case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY: return cube_img && layers >= 6;
        case VK_IMAGE_VIEW_TYPE_3D: return img3d;
        case VK_IMAGE_VIEW_TYPE_1D:
        case VK_IMAGE_VIEW_TYPE_1D_ARRAY: return img1d;
        default: return !img3d && !img1d;
    }
}

// A view for sampling: the image's own when it fits, else one of the view type with the texture's swizzle.
VkImageView view_of(GpuImage& img, VkImageViewType type, const ViewInfo& view) {
    uint32_t swizzle = img.alpha_from_r ? 0 : view.swizzle;  // materialized at upload
    VkComponentMapping m = img.swizzle;
    if (swizzle) {
        constexpr VkComponentSwizzle kSwizzle[] = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE,
                                                   VK_COMPONENT_SWIZZLE_R,    VK_COMPONENT_SWIZZLE_G,
                                                   VK_COMPONENT_SWIZZLE_B,    VK_COMPONENT_SWIZZLE_A};
        m = {kSwizzle[swizzle & 0xFF], kSwizzle[(swizzle >> 8) & 0xFF], kSwizzle[(swizzle >> 16) & 0xFF],
             kSwizzle[swizzle >> 24]};
    }
    uint32_t base = std::min(view.base_level, img.levels - 1);
    uint32_t levels = view.levels ? std::min(view.levels, img.levels - base) : img.levels - base;
    uint32_t min_layer = std::min(view.min_layer, img.layers - 1);
    bool whole = base == 0 && levels == img.levels && min_layer == 0 && (!view.layers || view.layers == img.layers);
    VkFormat format = view_format(img, view.format);
    if (type == img.view_type && identity(m) && whole && format == img.format) return img.view;
    auto& cache = g_sampled_views;
    uint64_t range = uint64_t(base) | uint64_t(levels) << 16 | uint64_t(min_layer) << 32 | uint64_t(view.layers) << 48;
    uint32_t mapping = uint32_t(m.r) | uint32_t(m.g) << 8 | uint32_t(m.b) << 16 | uint32_t(m.a) << 24;
    SampledKey key{img.image, type, mapping, range, uint64_t(format)};
    uint64_t h = uintptr_t(img.image) >> 4 ^ range * 31 ^ mapping ^ uint64_t(type) << 5 ^ uint64_t(format);
    ViewHit& hit = g_view_hits[(h ^ h >> 8) & 255];
    if (hit.view && hit.key == key) return hit.view;
    auto& v = cache[key];
    if (!v) {
        VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ci.image = img.image;
        ci.viewType = type;
        ci.format = format;
        ci.components = m;
        uint32_t layers = std::min(view.layers ? view.layers : img.layers - min_layer, img.layers - min_layer);
        if (type == VK_IMAGE_VIEW_TYPE_2D || type == VK_IMAGE_VIEW_TYPE_1D) layers = 1;
        if (type == VK_IMAGE_VIEW_TYPE_CUBE) layers = 6;
        if (type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) layers -= layers % 6;
        if (!view_compatible(img, type, layers)) return VK_NULL_HANDLE;
        ci.subresourceRange = {img.aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                                   ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT)
                                   : img.aspect,
                               base, levels, min_layer, layers};
        VkImageViewUsageCreateInfo usage{};
        narrow_view_usage(img, format, VK_IMAGE_USAGE_SAMPLED_BIT, ci, usage);
        if (p_vkCreateImageView(vk_device(), &ci, nullptr, &v) != VK_SUCCESS) v = img.view;
    }
    if (v) hit = {key, v};  // never a null one: those are tried again
    return v;
}

// R11G11B10F (0x3f) has no alpha, stored here as RGBA16F: sampling reads alpha 1 and blending takes destination
// alpha as 1, as Maxwell does (G4, docs/nvn/DEBUG.md #50). NX_R11G11B10_RAW=1: the stored alpha (bisecting).
bool ufloat_raw() {
    static const bool raw = std::getenv("NX_R11G11B10_RAW") != nullptr;
    return raw;
}

}  // namespace

void record_pipeline_key(const PipelineKey& key) {
    if (!key.vs || key.vs->hash.size() != 16 || (key.fs && key.fs->hash.size() != 16)) return;
    load_pipeline_keys();
    if (g_pipeline_key_records.size() >= kMaxPipelineRecords) return;
    PipelineKeysRecord rec{};
    std::memcpy(rec.vs_hash, key.vs->hash.data(), 16);
    if (key.fs) std::memcpy(rec.fs_hash, key.fs->hash.data(), 16);
    rec.vs_shared = key.vs->control.shared_size;
    rec.fs_shared = key.fs ? key.fs->control.shared_size : 0;
    std::copy(std::begin(key.vs->local_size), std::end(key.vs->local_size), rec.vs_local);
    if (key.fs) std::copy(std::begin(key.fs->local_size), std::end(key.fs->local_size), rec.fs_local);
    std::memcpy(&rec.key, &key, sizeof key);
    rec.key.vs = rec.key.fs = nullptr;
    const std::string id = pipeline_record_id(rec);
    if (g_pipeline_key_ids.insert(id).second) {
        g_pipeline_key_records.push_back(rec);
        g_pipeline_keys_dirty = true;
    }
}

namespace {

// The file write of save_pipeline_keys: on the background writer, or on the caller at shutdown.
void write_pipeline_keys(const std::string& path, const PipelineKeysHeader& h,
                         const std::vector<PipelineKeysRecord>& records, bool force) {
    const std::string tmp = path + ".tmp";
    FILE* f = plat::fopen_utf8(tmp, "wb");
    if (!f) return;
    bool ok = std::fwrite(&h, 1, sizeof h, f) == sizeof h;
    if (ok && !records.empty())
        ok = std::fwrite(records.data(), sizeof(PipelineKeysRecord), records.size(), f) == records.size();
    ok = std::fclose(f) == 0 && ok;
    std::remove(path.c_str());
    if (ok && std::rename(tmp.c_str(), path.c_str()) == 0)
        log("nvn/vk: saved %llu pipeline recipes%s", (unsigned long long)records.size(), force ? " at shutdown" : "");
    else
        log("nvn/vk: saving pipeline recipes to %s failed", path.c_str());
}

}  // namespace

void save_pipeline_keys(bool force) {
    if (!g_pipeline_keys_dirty && !force) return;
    open_pipeline_keys_path();
    PipelineKeysHeader h{};
    std::memcpy(h.magic, kPipelineKeysMagic, sizeof h.magic);
    h.record_size = sizeof(PipelineKeysRecord);
    h.count = g_pipeline_key_records.size();
    h.vendor = g_pipeline_vendor, h.device = g_pipeline_device;
    g_pipeline_keys_dirty = false;
    if (force) {
        wait_background_writes();  // an older periodic save must not land after this one
        write_pipeline_keys(g_pipeline_keys_path, h, g_pipeline_key_records, true);
        return;
    }
    // A copy of the records (~9 MB, about a millisecond) so the replay can keep adding while the file is written.
    background_write([path = g_pipeline_keys_path, h, records = g_pipeline_key_records] {
        write_pipeline_keys(path, h, records, false);
    });
}

void prepare_recorded_pipelines() {
    load_pipeline_keys();
    const uint64_t total = g_pipeline_key_records.size();
    if (!total) return;
    const uint64_t start = plat::monotonic_ns();
    uint64_t ready = 0, skipped = 0;
    prep_progress(0, total, true, 1);
    log("nvn/vk: prewarming %llu previously seen graphics pipelines", (unsigned long long)total);
    for (uint64_t i = 0; i < total; ++i) {
        const PipelineKeysRecord& rec = g_pipeline_key_records[size_t(i)];
        ShaderInfo* vs = load_pipeline_shader(rec.vs_hash, rec.vs_shared, rec.vs_local);
        ShaderInfo* fs = load_pipeline_shader(rec.fs_hash, rec.fs_shared, rec.fs_local);
        const bool has_fragment = std::any_of(std::begin(rec.fs_hash), std::end(rec.fs_hash), [](char c) { return c != 0; });
        if (!vs || vs->stage != ShaderStage::Vertex || (has_fragment && (!fs || fs->stage != ShaderStage::Fragment))) {
            ++skipped;
        } else {
            PipelineKey key = rec.key;
            key.vs = vs, key.fs = fs;
            std::vector<const ShaderInfo*> stages{vs};
            if (fs) stages.push_back(fs);
            if (graphics_pipeline(key, program_layout(stages))) ++ready; else ++skipped;
        }
        if ((i & 15u) == 15u || i + 1 == total) prep_progress(i + 1, total, true, 1);
    }
    prep_progress(total, total, false, 1);
    log("nvn/vk: pipeline prewarm finished: %llu/%llu ready, %llu skipped in %.1f ms",
        (unsigned long long)ready, (unsigned long long)total, (unsigned long long)skipped,
        double(plat::monotonic_ns() - start) / 1e6);
}

// ---- layouts ----------------------------------------------------------------------------------------------------------

const ProgramLayout& program_layout(const std::vector<const ShaderInfo*>& stages) {
    static std::map<std::vector<const ShaderInfo*>, ProgramLayout> cache;
    static const std::pair<const std::vector<const ShaderInfo*>, ProgramLayout>* last = nullptr;  // entries stay
    if (last && last->first == stages) return last->second;  // runs of draws use one program
    auto it = cache.find(stages);
    if (it != cache.end()) return (last = &*it)->second;
    std::map<uint32_t, VkDescriptorSetLayoutBinding> sets[4];
    for (const ShaderInfo* s : stages)
        for (const DescriptorBinding& b : s->refl.bindings) {
            if (b.set >= 4) continue;
            auto& vb = sets[b.set][b.binding];
            vb.binding = b.binding;
            vb.descriptorType = VkDescriptorType(b.type);
            vb.descriptorCount = std::max(1u, b.count);
            vb.stageFlags |= stage_bit(s->stage);
        }
    ProgramLayout pl;
    // On by default on Android only (NX_DYNAMIC_UBO=0/1 overrides): on the Mac (MoltenVK) every frame came out black
    // and the GPU hung a few thousand frames in (2026-10-01), cause not found yet.
    static const uint32_t max_dynamic = [] {
        const char* e = std::getenv("NX_DYNAMIC_UBO");
#if defined(__ANDROID__)
        if (e && e[0] == '0') return 0u;
#else
        if (!e || e[0] != '1') return 0u;
#endif
        VkPhysicalDeviceProperties p{};
        p_vkGetPhysicalDeviceProperties(vk_physical(), &p);
        log("nvn/vk: uniform buffers bound with dynamic offsets (up to %u per layout)",
            p.limits.maxDescriptorSetUniformBuffersDynamic);
        return p.limits.maxDescriptorSetUniformBuffersDynamic;
    }();
    uint64_t dynamic = 0;
    for (auto& [n, b] : sets[0])
        if (b.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b.descriptorCount == 1 && n < 64) dynamic |= 1ull << n;
    if (dynamic && uint32_t(__builtin_popcountll(dynamic)) <= max_dynamic) {
        for (auto& [n, b] : sets[0])
            if (n < 64 && dynamic >> n & 1) b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        pl.dynamic = dynamic;
    }
    for (int k = 0; k < 4; k++) {
        std::vector<VkDescriptorSetLayoutBinding> v;
        for (auto& [n, b] : sets[k]) v.push_back(b);
        pl.sets[k] = set_layout(v);
    }
    VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    ci.setLayoutCount = 4;
    ci.pSetLayouts = pl.sets;
    p_vkCreatePipelineLayout(vk_device(), &ci, nullptr, &pl.layout);
    return cache[stages] = pl;
}

// ---- graphics pipelines ---------------------------------------------------------------------------------------------

bool gpl_dynamic_state() {
    static const bool on = [] {
        const char* e = std::getenv("NX_GPL_DYNAMIC");
        const bool v = vk_gpl() && !(e && e[0] == '0');
        if (vk_gpl())
            log("nvn/vk: GPL: cull, front face, depth bias enable, depth and stencil tests %s",
                v ? "dynamic" : "static (NX_GPL_DYNAMIC=0)");
        return v;
    }();
    return on;
}

RasterDepthState raster_depth_state(const PipelineKey& key) {
    RasterDepthState r;
    std::memset(&r, 0, sizeof r);  // compared byte for byte (nvn_draw.cpp)
    r.cull = vk_cull(key.polygon.cull);
    // NVNfrontFace CW 0 / CCW 1, taken as is: the draws use a negative-height viewport (nvn_draw.cpp) that gives NVN's
    // upper-left-origin mapping, so windings match NVN's.
    r.front = key.polygon.front == 0 ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    // Polygon offset: NVNpolygonOffsetEnable has a bit per polygon mode (POINT 1, LINE 2, FILL 4; modes are POINT 0,
    // LINE 1, FILL 2), and the bit of the mode drawn with decides. Factor, units and clamp are dynamic (SetDepthBias
    // at each draw, nvn_raster.cpp): the game sets them on the command buffer, not in the state object.
    uint32_t offset_bit = key.polygon.mode <= 2 ? 1u << key.polygon.mode : 4u;
    r.bias = key.polygon.offset_enables & offset_bit || equal_slack_pass(key.depth) ? VK_TRUE : VK_FALSE;
    bool has_depth = key.depth_format != VK_FORMAT_UNDEFINED;
    r.depth_test = has_depth && key.depth.depth_test;
    r.depth_write = has_depth && key.depth.depth_write;
    r.depth_compare = vk_compare(key.depth.depth_func);
    // EQUAL after a depth pre-pass (the colour pass does not write depth) needs both passes to compute bit-identical
    // depth; two translated shaders compiled separately by Metal do not, and Wonder's whole course colour pass was
    // discarded (only the sky showed). LESS_OR_EQUAL keeps what EQUAL keeps for opaque geometry after a pre-pass.
    if (key.depth.depth_func == 3 && !key.depth.depth_write) r.depth_compare = VK_COMPARE_OP_LESS_OR_EQUAL;
    r.stencil_test = has_depth && key.depth.stencil_test;
    for (int f = 0; f < 2; f++) {
        r.faces[f].failOp = vk_stencil_op(key.depth.stencil_fail[f]);
        r.faces[f].depthFailOp = vk_stencil_op(key.depth.depth_fail[f]);
        r.faces[f].passOp = vk_stencil_op(key.depth.depth_pass[f]);
        r.faces[f].compareOp = vk_compare(key.depth.stencil_func[f]);
        r.faces[f].compareMask = r.faces[f].writeMask = 0xFF;  // dynamic: SetStencilValueMask / SetStencilMask
    }
    return r;
}

void set_raster_depth_state(VkCommandBuffer cb, const RasterDepthState& s) {
    p_vkCmdSetCullMode(cb, s.cull);
    p_vkCmdSetFrontFace(cb, s.front);
    p_vkCmdSetDepthBiasEnable(cb, s.bias);
    p_vkCmdSetDepthTestEnable(cb, s.depth_test);
    p_vkCmdSetDepthWriteEnable(cb, s.depth_write);
    p_vkCmdSetDepthCompareOp(cb, s.depth_compare);
    p_vkCmdSetStencilTestEnable(cb, s.stencil_test);
    const VkStencilFaceFlags face[2] = {VK_STENCIL_FACE_FRONT_BIT, VK_STENCIL_FACE_BACK_BIT};
    for (int f = 0; f < 2; f++)
        p_vkCmdSetStencilOp(cb, face[f], s.faces[f].failOp, s.faces[f].passOp, s.faces[f].depthFailOp,
                            s.faces[f].compareOp);
}

namespace {

// The state a GPL-mode pipeline takes from the command buffer (gpl_dynamic_state) is left out of its cache key, so
// draws that differ only in it share one pipeline.
PipelineKey normalized_key(const PipelineKey& key) {
    if (!gpl_dynamic_state()) return key;
    PipelineKey k = key;
    k.polygon.cull = k.polygon.front = k.polygon.offset_enables = 0;
    std::memset(&k.depth, 0, sizeof k.depth);  // with its padding: keys are compared byte for byte
    return k;
}

// Everything vkCreateGraphicsPipelines reads for `key` (a normalized key in GPL dynamic mode).
std::shared_ptr<PipelineBuild> make_build(const PipelineKey& key, const ProgramLayout& layout) {
    auto build = std::make_shared<PipelineBuild>();
    build->key = key;
    auto& stages = build->stages;
    uint32_t nstages = 0;
    uint8_t int_kinds[16] = {};
    for (uint32_t a = 0; a < key.attrib_count && a < 16; a++) int_kinds[a] = uint8_t(vertex_int_kind(key.attribs[a].format));
    // NX_TRACE_PIPELINES=1: every new pipeline's shaders, and each vertex shader's inputs once.
    static std::set<const ShaderInfo*> logged_vs;
    static const bool trace = std::getenv("NX_TRACE_PIPELINES") != nullptr;
    if (trace) log("nvn/vk: new pipeline vs %s fs %s", key.vs ? key.vs->hash.c_str() : "-", key.fs ? key.fs->hash.c_str() : "-");
    if (trace && key.vs && logged_vs.insert(key.vs).second) {
        std::string line;
        for (const ShaderIo& in : key.vs->refl.inputs) {
            char b[64];
            const VertexAttrib* va = in.location < key.attrib_count ? &key.attribs[in.location] : nullptr;
            std::snprintf(b, sizeof b, " %u:%x/s%u+%u", in.location, va ? va->format : 0, va ? va->stream : 0,
                          va ? va->offset : 0);
            line += b;
        }
        log("nvn/vk: vs %s inputs (location:format/stream+offset)%s", key.vs->hash.c_str(), line.c_str());
    }
    uint32_t unwritten = unwritten_inputs(key.vs, key.fs);
    for (const ShaderInfo* s : {key.vs, key.fs}) {
        if (!s) continue;
        auto& st = stages[nstages++];
        st.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st.stage = VkShaderStageFlagBits(stage_bit(s->stage));
        st.module = module_of(s, s == key.vs ? int_kinds : nullptr, s == key.fs ? unwritten : 0);
        st.pName = "main";
        // The module's identity for the GPL library keys: the same SPIR-V and patches give the same key, whichever
        // ShaderInfo (the game's or the startup catalog's) it came from.
        std::string& id = s == key.vs ? build->vs_id : build->fs_id;
        id = s->hash;
        if (s == key.vs) id.append(reinterpret_cast<const char*>(int_kinds), 16);
        else id.append(reinterpret_cast<const char*>(&unwritten), sizeof unwritten);
        id += int_texture_key(s);
    }

    // Vertex input: each shader input location reads the attribute of the same index. Locations with no attribute
    // (or an unknown format) read a zero vec4 from binding 15.
    auto& attrs = build->attrs;
    auto& binds = build->binds;
    bool streams_used[16] = {}, zero_used = false;
    if (key.vs)
        for (const ShaderIo& in : key.vs->refl.inputs) {
            VkVertexInputAttributeDescription a{in.location, 15, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
            if (in.location < key.attrib_count) {
                const VertexAttrib& va = key.attribs[in.location];
                VkFormat f = vk_vertex_format(va.format);
                if (f != VK_FORMAT_UNDEFINED && va.stream < 15) {
                    a.binding = va.stream, a.format = f, a.offset = va.offset;
                    streams_used[va.stream] = true;
                }
            }
            if (a.binding == 15) zero_used = true;
            attrs.push_back(a);
        }
    for (uint32_t s = 0; s < 15; s++)
        if (streams_used[s]) {
            const VertexStream& vs = key.streams[s];
            binds.push_back({s, vs.stride, vs.divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX});
        }
    if (zero_used) binds.push_back({15, 0, VK_VERTEX_INPUT_RATE_INSTANCE});
    auto& vi = build->vi;
    vi.vertexBindingDescriptionCount = uint32_t(binds.size());
    vi.pVertexBindingDescriptions = binds.data();
    vi.vertexAttributeDescriptionCount = uint32_t(attrs.size());
    vi.pVertexAttributeDescriptions = attrs.data();

    auto& ia = build->ia;
    ia.topology = vk_topology(key.topology);

    auto& clip = build->clip;
    clip.negativeOneToOne = VK_TRUE;  // NVN depth mode 0 (Wonder): clip-space z in -1..1
    auto& vp = build->vp;
    vp.viewportCount = vp.scissorCount = 1;
    if (vk_depth_clip_control() && !depth_remap()) vp.pNext = &clip;

    const RasterDepthState rd = raster_depth_state(key);
    auto& rs = build->rs;
    rs.polygonMode = key.polygon.mode == 0 ? VK_POLYGON_MODE_POINT : key.polygon.mode == 1 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.cullMode = rd.cull;
    rs.frontFace = rd.front;
    rs.lineWidth = 1.0f;
    // SetDepthClamp: clamps instead of clipping at the near and far planes, like depthClampEnable (a device feature;
    // nvn_vk.cpp enables every core feature the GPU has, and MoltenVK has it through Metal's depth clip mode).
    rs.depthClampEnable = key.depth_clamp && vk_features().depthClamp ? VK_TRUE : VK_FALSE;
    rs.depthBiasEnable = rd.bias;

    auto& ms = build->ms;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    // BindMultisampleState's alpha to coverage (G7; single-sampled targets: alpha under 0.5 drops the fragment),
    // logged once per pipeline that has it. NX_NO_ALPHA_TO_COVERAGE=1: off, as when it was a stub.
    static const bool no_a2c = std::getenv("NX_NO_ALPHA_TO_COVERAGE") != nullptr;
    ms.alphaToCoverageEnable = key.alpha_to_coverage && !no_a2c ? VK_TRUE : VK_FALSE;
    if (key.alpha_to_coverage)
        log("nvn/vk: alpha to coverage%s: fs %s", no_a2c ? " (off)" : "", key.fs ? key.fs->hash.c_str() : "-");

    auto& ds = build->ds;
    ds.depthTestEnable = rd.depth_test;
    ds.depthWriteEnable = rd.depth_write;
    ds.depthCompareOp = rd.depth_compare;
    ds.stencilTestEnable = rd.stencil_test;
    ds.front = rd.faces[0], ds.back = rd.faces[1];

    auto& att = build->att;
    for (uint32_t k = 0; k < key.color_count; k++) {
        const BlendTarget& b = key.blend[k];
        // An R11G11B10F target's destination alpha is 1 on Maxwell; the RGBA16F image holds what the shader wrote.
        bool ufloat = (key.ufloat_colors >> k & 1) && !ufloat_raw();
        auto factor = [ufloat](uint32_t f) {
            VkBlendFactor v = vk_blend_factor(f);
            if (!ufloat) return v;
            return v == VK_BLEND_FACTOR_DST_ALPHA             ? VK_BLEND_FACTOR_ONE
                   : v == VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA ? VK_BLEND_FACTOR_ZERO
                   : v == VK_BLEND_FACTOR_SRC_ALPHA_SATURATE  ? VK_BLEND_FACTOR_ZERO  // min(As, 1 - 1)
                                                              : v;
        };
        att[k].blendEnable = key.color.blend_enable[k] ? VK_TRUE : VK_FALSE;
        att[k].srcColorBlendFactor = factor(b.src_rgb);
        att[k].dstColorBlendFactor = factor(b.dst_rgb);
        att[k].colorBlendOp = vk_blend_op(b.eq_rgb);
        att[k].srcAlphaBlendFactor = factor(b.src_a);
        att[k].dstAlphaBlendFactor = factor(b.dst_a);
        att[k].alphaBlendOp = vk_blend_op(b.eq_a);
        att[k].colorWriteMask = key.mask.mask[k] & 0xF;
    }
    auto& cb = build->cb;
    cb.attachmentCount = key.color_count;
    cb.pAttachments = att;

    // Stencil reference and masks and depth bias values come from command-buffer calls (nvn_raster.cpp), so they are
    // dynamic: one pipeline for every value the game uses. In GPL dynamic mode also the state raster_depth_state
    // gives (set per draw by nvn_draw.cpp), the same in the libraries and in the optimised pipeline.
    static const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                         VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                                         VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                                         VK_DYNAMIC_STATE_DEPTH_BIAS,
                                         // gpl_dynamic_state() only:
                                         VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
                                         VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
                                         VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
                                         VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP};
    auto& dy = build->dy;
    dy.dynamicStateCount = gpl_dynamic_state() ? uint32_t(sizeof dyn / sizeof dyn[0]) : 7u;
    dy.pDynamicStates = dyn;

    auto& ri = build->ri;
    ri.colorAttachmentCount = key.color_count;
    ri.pColorAttachmentFormats = build->key.color_formats;  // the build's copy: the caller's key may be gone
    bool stencil = key.depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT || key.depth_format == VK_FORMAT_S8_UINT;
    ri.depthAttachmentFormat = key.depth_format == VK_FORMAT_S8_UINT ? VK_FORMAT_UNDEFINED : key.depth_format;
    ri.stencilAttachmentFormat = stencil ? key.depth_format : VK_FORMAT_UNDEFINED;

    auto& ci = build->ci;
    ci.pNext = &ri;
    if (render_pass_objects())  // nvn_renderpass.cpp: compatible with every pass of these formats
        ci.pNext = nullptr, ci.renderPass = compatible_render_pass(key.color_count, key.color_formats, key.depth_format);
    ci.stageCount = nstages;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = layout.layout;
    return build;
}

// ---- graphics pipeline libraries (VK_EXT_graphics_pipeline_library) -------------------------------------------------
//
// A pipeline nobody has seen costs a full compile, ~5x slower on the AMD Windows driver than on RADV. With GPL a
// pipeline is linked from four parts, each cached by its own key: vertex input (formats, topology), pre-rasterization
// (vertex shader, rasterizer), fragment shader (fragment shader, depth/stencil) and fragment output (blending,
// formats). The shader parts are what cost; a program's are made once (at startup from the shader catalog, or the
// first time it is drawn) and every new state combination of it is a fast link (no link-time optimisation), used at
// once. The optimised, monolithic pipeline is compiled on the worker pool meanwhile (nvn_pipeline_async.h) and
// swapped in when ready. The TotK PC recomp's design (valentderah/totk-pc-recomp, docs/architecture.md, MIT).
// NX_GPL=0: none of this (nvn_vk.cpp); NX_GPL_DYNAMIC=0: GPL with the rasterizer and depth state in the libraries.

enum LibPart { kVertexInput, kPreRaster, kFragmentShader, kFragmentOutput, kParts };
constexpr VkGraphicsPipelineLibraryFlagsEXT kPartFlag[kParts] = {
    VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT,
    VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT,
    VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT,
    VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT};
const char* const kPartName[kParts] = {"vertex input", "pre-rasterization", "fragment shader", "fragment output"};

// Libraries by key; a null value is a library that failed (its pipelines are compiled whole). Made on the replay
// thread and on the worker pool, so under a lock; never destroyed.
struct LibCache {
    std::mutex lock;
    std::unordered_map<std::string, VkPipeline> map;
};
LibCache g_libs[kParts];

struct GplStats {  // per timing window (gpl_log), counted on any thread
    std::atomic<uint64_t> lib_made[kParts]{}, lib_ns[kParts]{};
    std::atomic<uint64_t> probes{0}, probe_hits{0}, probe_ns{0}, links{0}, link_ns{0}, swaps{0}, waits{0};
    std::atomic<uint64_t> lib_hits{0}, lib_misses{0};  // shader libraries tried cache-only: found / compile needed
} g_gpl;

template <typename T>
void add_bytes(std::string& s, const T& v) {
    s.append(reinterpret_cast<const char*>(&v), sizeof v);
}

// The keys of a build's four parts. Layouts by their set layouts (cached by their bindings): the startup catalog's
// ProgramLayout is another object with identical sets. Pointers inside the structs are left out.
void library_keys(const PipelineBuild& b, const ProgramLayout& layout, std::string keys[kParts]) {
    std::string program;
    for (VkDescriptorSetLayout s : layout.sets) add_bytes(program, s);
    add_bytes(program, layout.dynamic);
    add_bytes(program, b.ci.renderPass);
    add_bytes(program, b.dy.dynamicStateCount);
    // Vertex input.
    std::string& v = keys[kVertexInput];
    add_bytes(v, b.ia.topology);
    add_bytes(v, b.ia.primitiveRestartEnable);
    add_bytes(v, uint32_t(b.binds.size()));
    v.append(reinterpret_cast<const char*>(b.binds.data()), b.binds.size() * sizeof b.binds[0]);
    v.append(reinterpret_cast<const char*>(b.attrs.data()), b.attrs.size() * sizeof b.attrs[0]);
    add_bytes(v, b.dy.dynamicStateCount);
    // Pre-rasterization.
    std::string& p = keys[kPreRaster];
    p = program;
    add_bytes(p, uint32_t(b.vs_id.size()));
    p += b.vs_id;
    // Field by field: the create-info structs have padding after sType.
    for (uint32_t v : {uint32_t(b.rs.polygonMode), uint32_t(b.rs.cullMode), uint32_t(b.rs.frontFace),
                       b.rs.depthClampEnable, b.rs.depthBiasEnable, b.rs.rasterizerDiscardEnable})
        add_bytes(p, v);
    add_bytes(p, b.rs.lineWidth);
    add_bytes(p, uint32_t(b.vp.pNext != nullptr));
    // Fragment shader. The multisample state only with a render pass object (then it is part of this state).
    std::string& f = keys[kFragmentShader];
    f = program;
    add_bytes(f, uint32_t(b.fs_id.size()));
    f += b.fs_id;
    for (uint32_t v : {b.ds.depthTestEnable, b.ds.depthWriteEnable, uint32_t(b.ds.depthCompareOp),
                       b.ds.depthBoundsTestEnable, b.ds.stencilTestEnable})
        add_bytes(f, v);
    add_bytes(f, b.ds.front), add_bytes(f, b.ds.back);  // VkStencilOpState: uint32 fields only
    if (b.ci.renderPass) add_bytes(f, b.ms.alphaToCoverageEnable);
    // Fragment output.
    std::string& o = keys[kFragmentOutput];
    add_bytes(o, b.ci.renderPass);
    add_bytes(o, b.cb.attachmentCount);
    o.append(reinterpret_cast<const char*>(b.att), b.cb.attachmentCount * sizeof b.att[0]);
    add_bytes(o, b.ms.alphaToCoverageEnable);
    add_bytes(o, b.ms.rasterizationSamples);
    add_bytes(o, b.ri.colorAttachmentCount);
    o.append(reinterpret_cast<const char*>(b.key.color_formats), b.ri.colorAttachmentCount * sizeof(VkFormat));
    add_bytes(o, b.ri.depthAttachmentFormat);
    add_bytes(o, b.ri.stencilAttachmentFormat);
    add_bytes(o, b.dy.dynamicStateCount);
}

// Whether the cache has `key` (its library in *out, null if it failed).
bool find_library(LibPart part, const std::string& key, VkPipeline* out) {
    LibCache& c = g_libs[part];
    std::lock_guard l(c.lock);
    auto it = c.map.find(key);
    if (it == c.map.end()) return false;
    *out = it->second;
    return true;
}

// Creates one part of `b` as a library and caches it (any thread). Two threads making the same one: the first kept.
// `cache_only`: only when the pipeline cache (ours or the driver's) has it; *compiled false and nothing cached when a
// compile would be needed. `keep` false: made only to fill the caches (startup preparation), destroyed at once.
VkPipeline make_library(LibPart part, const std::string& key, const PipelineBuild& b, bool cache_only = false,
                        bool* compiled = nullptr, bool keep = true) {
    VkGraphicsPipelineLibraryCreateInfoEXT li{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT};
    li.flags = kPartFlag[part];
    // The shader parts take only the view mask from the rendering info (the formats are the output part's).
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    const bool output = part == kFragmentOutput;
    if (!b.ci.renderPass && part != kVertexInput)
        li.pNext = output ? const_cast<VkPipelineRenderingCreateInfo*>(&b.ri) : &ri;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &li;
    ci.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
    if (cache_only) ci.flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
    ci.pDynamicState = &b.dy;
    switch (part) {
        case kVertexInput:
            ci.pVertexInputState = &b.vi;
            ci.pInputAssemblyState = &b.ia;
            break;
        case kPreRaster:
            ci.stageCount = 1;
            ci.pStages = &b.stages[0];
            ci.pViewportState = &b.vp;
            ci.pRasterizationState = &b.rs;
            ci.layout = b.ci.layout;
            ci.renderPass = b.ci.renderPass;
            break;
        case kFragmentShader:
            ci.stageCount = b.key.fs ? 1 : 0;
            ci.pStages = b.key.fs ? &b.stages[1] : nullptr;
            ci.pDepthStencilState = &b.ds;
            if (b.ci.renderPass) ci.pMultisampleState = &b.ms;
            ci.layout = b.ci.layout;
            ci.renderPass = b.ci.renderPass;
            break;
        default:
            ci.pColorBlendState = &b.cb;
            ci.pMultisampleState = &b.ms;
            ci.renderPass = b.ci.renderPass;
            break;
    }
    const uint64_t t0 = plat::monotonic_ns();
    VkPipeline p = VK_NULL_HANDLE;
    const VkResult r = p_vkCreateGraphicsPipelines(vk_device(), pipeline_cache(), 1, &ci, nullptr, &p);
    if (compiled) *compiled = r != VK_PIPELINE_COMPILE_REQUIRED;
    if (r == VK_PIPELINE_COMPILE_REQUIRED) {
        g_gpl.lib_misses++, g_gpl.probe_ns += plat::monotonic_ns() - t0;
        return VK_NULL_HANDLE;
    }
    if (r != VK_SUCCESS) {
        p = VK_NULL_HANDLE;
        log("nvn/vk: GPL %s library for %s + %s failed (%d)", kPartName[part], b.key.vs ? b.key.vs->hash.c_str() : "-",
            b.key.fs ? b.key.fs->hash.c_str() : "-", int(r));
    } else if (!cache_only) {
        note_pipeline_cache_changed();
    }
    (cache_only ? g_gpl.lib_hits : g_gpl.lib_made[part])++;
    g_gpl.lib_ns[part] += plat::monotonic_ns() - t0;
    if (!keep) {
        if (p) p_vkDestroyPipeline(vk_device(), p, nullptr);
        return VK_NULL_HANDLE;
    }
    LibCache& c = g_libs[part];
    std::lock_guard l(c.lock);
    auto [it, fresh] = c.map.emplace(key, p);
    if (!fresh && p) p_vkDestroyPipeline(vk_device(), p, nullptr);
    return it->second;
}

VkPipeline get_library(LibPart part, const std::string& key, const PipelineBuild& b) {
    VkPipeline p = VK_NULL_HANDLE;
    return find_library(part, key, &p) ? p : make_library(part, key, b);
}

// The libraries' fast link: no link-time optimisation, so no compile; not put in the pipeline cache (the optimised
// pipeline is what later runs find there).
VkPipeline fast_link(const VkPipeline libs[kParts], const ProgramLayout& layout) {
    VkPipelineLibraryCreateInfoKHR lk{VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR};
    lk.libraryCount = kParts;
    lk.pLibraries = libs;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &lk;
    ci.layout = layout.layout;
    const uint64_t t0 = plat::monotonic_ns();
    VkPipeline p = VK_NULL_HANDLE;
    if (p_vkCreateGraphicsPipelines(vk_device(), VK_NULL_HANDLE, 1, &ci, nullptr, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    g_gpl.links++, g_gpl.link_ns += plat::monotonic_ns() - t0;
    return p;
}

// Fast-linked pipelines replaced by their optimised one: destroyed once no submit that may use them is in flight.
// Replay thread only.
std::deque<std::pair<uint64_t, VkPipeline>> g_retired;
void retire_pipeline(VkPipeline p) { g_retired.push_back({frame_recording_serial(), p}); }
void free_retired_pipelines() {
    const uint64_t completed = frame_completed();
    while (!g_retired.empty() && g_retired.front().first <= completed) {
        p_vkDestroyPipeline(vk_device(), g_retired.front().second, nullptr);
        g_retired.pop_front();
    }
}

}  // namespace

std::function<void()> gpl_program_job(const ShaderInfo* vs, const ShaderInfo* fs, const ProgramLayout& layout) {
    if (!vk_gpl() || !vs || render_pass_objects()) return {};
    // The state a draw of this program most likely has: a filled triangle list with no depth clamp. With
    // gpl_dynamic_state() nothing else of the shader parts' keys depends on the draw (NX_GPL_DYNAMIC=0: the culled,
    // depth-tested variants are made when first drawn).
    PipelineKey key;
    std::memset(&key, 0, sizeof key);
    key.vs = vs, key.fs = fs;
    key.polygon.mode = 2;
    std::shared_ptr<PipelineBuild> b = make_build(normalized_key(key), layout);  // the modules made here, on the caller
    auto keys = std::make_shared<std::array<std::string, kParts>>();
    library_keys(*b, layout, keys->data());
    // Made and destroyed: kept, the libraries of all ~12,500 programs held ~1.6 GB (and a course then passed the
    // 8 GB guard). What stays is the compiled code in the pipeline caches, from where a draw's library is made in
    // ~0.2 ms (graphics_pipeline).
    return [b, keys] {
        for (LibPart part : {kPreRaster, kFragmentShader}) make_library(part, (*keys)[part], *b, false, nullptr, false);
    };
}

void release_modules(const std::set<const ShaderInfo*>& shaders) {
    size_t n = 0;
    for (auto it = g_modules.begin(); it != g_modules.end();)
        if (shaders.count(std::get<0>(it->first))) {
            if (it->second) p_vkDestroyShaderModule(vk_device(), it->second, nullptr);
            it = g_modules.erase(it), n++;
        } else {
            ++it;
        }
    if (n) log("nvn/vk: released %zu shader modules of the startup preparation", n);
}

void gpl_log() {
    if (!vk_gpl()) return;
    free_retired_pipelines();
    uint64_t made = 0;
    for (auto& m : g_gpl.lib_made) made += m.load();
    if (!made && !g_gpl.probes && !g_gpl.links && !g_gpl.swaps && !g_gpl.lib_hits && !g_gpl.lib_misses) return;
    char parts[256];
    std::snprintf(parts, sizeof parts, "%llu+%llu+%llu+%llu (%.1f/%.1f/%.1f/%.1f ms)",
                  (unsigned long long)g_gpl.lib_made[0].exchange(0), (unsigned long long)g_gpl.lib_made[1].exchange(0),
                  (unsigned long long)g_gpl.lib_made[2].exchange(0), (unsigned long long)g_gpl.lib_made[3].exchange(0),
                  double(g_gpl.lib_ns[0].exchange(0)) / 1e6, double(g_gpl.lib_ns[1].exchange(0)) / 1e6,
                  double(g_gpl.lib_ns[2].exchange(0)) / 1e6, double(g_gpl.lib_ns[3].exchange(0)) / 1e6);
    log("nvn/vk: GPL: libraries vi+pre+fs+out %s, shader libraries from the cache %llu (%llu needed a compile), "
        "cache probes %llu (%llu hits, %.1f ms), fast links %llu (%.1f ms), optimised swapped in %llu, compiled in "
        "place %llu",
        parts, (unsigned long long)g_gpl.lib_hits.exchange(0), (unsigned long long)g_gpl.lib_misses.exchange(0),
        (unsigned long long)g_gpl.probes.exchange(0), (unsigned long long)g_gpl.probe_hits.exchange(0),
        double(g_gpl.probe_ns.exchange(0)) / 1e6, (unsigned long long)g_gpl.links.exchange(0),
        double(g_gpl.link_ns.exchange(0)) / 1e6, (unsigned long long)g_gpl.swaps.exchange(0),
        (unsigned long long)g_gpl.waits.exchange(0));
}

VkPipeline graphics_pipeline(const PipelineKey& draw_key, const ProgramLayout& layout, bool may_skip) {
    if (!draw_key.vs || draw_key.vs->hash.size() != 16 || (draw_key.fs && draw_key.fs->hash.size() != 16))
        return VK_NULL_HANDLE;
    // GPL dynamic mode: the state the draw sets on the command buffer is not part of the pipeline.
    const bool dynamic = gpl_dynamic_state();
    PipelineKey normalized;
    if (dynamic) normalized = normalized_key(draw_key);
    const PipelineKey& key = dynamic ? normalized : draw_key;
    // By the key's bytes, as before, but hashed (nvn_pipeline.h) instead of a ~700-byte std::string built and
    // compared down a tree per draw; and the last entry first: consecutive draws mostly share their state.
    using Key = ByteKey<sizeof(PipelineKey) + 40>;
    // A pipeline, and with GPL its optimised replacement while that compiles (swapped in by the first draw after).
    struct Entry {
        VkPipeline pipeline = VK_NULL_HANDLE;
        std::shared_ptr<AsyncPipeline> optimised;
    };
    static std::unordered_map<Key, Entry, ByteKeyHash> cache;
    static std::pair<const Key, Entry>* last = nullptr;  // entries are never erased
    auto current = [&](Entry& e) {
        if (e.optimised && e.optimised->finished.load(std::memory_order_acquire)) {
            AsyncPipeline& a = *e.optimised;
            note_pipeline_created(a.ns);
            if (a.result == VK_SUCCESS && a.pipeline) {
                retire_pipeline(e.pipeline);
                e.pipeline = a.pipeline;
                g_gpl.swaps++;
            } else {
                log("nvn/vk: optimised pipeline for %s + %s failed: the fast-linked one stays", key.vs->hash.c_str(),
                    key.fs ? key.fs->hash.c_str() : "-");
            }
            e.optimised.reset();
            free_retired_pipelines();
        }
        return e.pipeline;
    };
    Key k{};
    std::memcpy(k.b, key.vs->hash.data(), 16);
    if (key.fs) std::memcpy(k.b + 16, key.fs->hash.data(), 16);
    PipelineKey state_key{};
    std::memcpy(&state_key, &key, sizeof key);
    state_key.vs = state_key.fs = nullptr;
    std::memcpy(k.b + 32, &state_key, sizeof state_key);
    // A stage that gained an integer texture record has another module (nvn_spirv_int_textures.cpp).
    const uint32_t generations[2] = {int_texture_generation(key.vs), key.fs ? int_texture_generation(key.fs) : 0u};
    std::memcpy(k.b + 32 + sizeof state_key, generations, sizeof generations);
    if (last && std::memcmp(k.b, last->first.b, sizeof k.b) == 0) return current(last->second);
    if (auto it = cache.find(k); it != cache.end()) return current((last = &*it)->second);
    auto store = [&](VkPipeline p, std::shared_ptr<AsyncPipeline> optimised = nullptr) {
        auto it = cache.emplace(k, Entry{p, std::move(optimised)}).first;
        return (last = &*it)->second.pipeline;
    };
    // A compile already on a worker (nvn_pipeline_async.cpp): its pipeline when done, else skip or wait.
    static std::unordered_map<Key, std::shared_ptr<AsyncPipeline>, ByteKeyHash> pending;
    auto finish = [&](AsyncPipeline& a) {
        note_pipeline_created(a.ns);
        if (a.result != VK_SUCCESS)
            log("nvn/vk: pipeline for %s + %s failed", key.vs->hash.c_str(), key.fs ? key.fs->hash.c_str() : "-");
        else
            record_pipeline_key(key);
        return store(a.result == VK_SUCCESS ? a.pipeline : VK_NULL_HANDLE);
    };
    if (auto it = pending.find(k); it != pending.end()) {
        std::shared_ptr<AsyncPipeline> a = it->second;
        if (!a->ready()) {
            if (may_skip) return g_pipelines_skipped++, VK_NULL_HANDLE;
            a->wait();
        }
        pending.erase(it);
        return finish(*a);
    }

    std::shared_ptr<PipelineBuild> build = make_build(key, layout);
    VkGraphicsPipelineCreateInfo& ci = build->ci;

    if (vk_gpl()) {
        // 1. In the pipeline cache already (an earlier run compiled it): made from there, no compile.
        if (vk_pipeline_cache_control() && pipeline_cache()) {
            ci.flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
            VkPipeline p = VK_NULL_HANDLE;
            const uint64_t t0 = plat::monotonic_ns();
            const VkResult r = p_vkCreateGraphicsPipelines(vk_device(), pipeline_cache(), 1, &ci, nullptr, &p);
            ci.flags &= ~VkPipelineCreateFlags(VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT);
            g_gpl.probes++, g_gpl.probe_ns += plat::monotonic_ns() - t0;
            if (r == VK_SUCCESS && p) {
                g_gpl.probe_hits++;
                record_pipeline_key(key);
                return store(p);
            }
        }
        // 2. Linked from the libraries now, the optimised pipeline compiled on a worker meanwhile.
        auto keys = std::make_shared<std::array<std::string, kParts>>();
        library_keys(*build, layout, keys->data());
        VkPipeline libs[kParts] = {};
        // The shader parts: made already, or made now from the pipeline caches (no compile). A compile needed:
        // done here only when the draw cannot wait.
        bool have[kParts] = {};
        for (LibPart part : {kPreRaster, kFragmentShader}) {
            have[part] = find_library(part, (*keys)[part], &libs[part]);
            if (!have[part] && vk_pipeline_cache_control()) {
                bool compiled = true;
                libs[part] = make_library(part, (*keys)[part], *build, true, &compiled);
                have[part] = compiled;
            }
        }
        const bool have_pre = have[kPreRaster], have_fs = have[kFragmentShader];
        if ((!have_pre || !have_fs) && may_skip && async_pipelines()) {
            // A program not seen yet, whose draw can wait a frame: its shader libraries are made on a worker for the
            // next state combinations, and this one compiles whole on a worker, as without GPL.
            run_on_pool([build, keys] {
                for (LibPart part : {kPreRaster, kFragmentShader}) get_library(part, (*keys)[part], *build);
            });
            pending.emplace(k, compile_pipeline_async(build));
            return g_pipelines_skipped++, VK_NULL_HANDLE;
        }
        bool complete = true;
        for (int part = 0; part < kParts; part++) {
            if (!libs[part]) libs[part] = get_library(LibPart(part), (*keys)[part], *build);
            complete = complete && libs[part];
        }
        if (VkPipeline p = complete ? fast_link(libs, layout) : VK_NULL_HANDLE) {
            if (async_pipelines()) {
                record_pipeline_key(key);
                return store(p, compile_pipeline_async(build));
            }
            // NX_ASYNC_PIPELINES=0: the optimised one compiled here, below; the linked one is not used.
            g_gpl.waits++;
            p_vkDestroyPipeline(vk_device(), p, nullptr);
        }
        // A library or the link failed: compiled whole below.
    }

    // A draw that may skip a frame: compiled on a worker meanwhile (nvn_pipeline_async.h). But first, as GPL's step
    // 1: a pipeline an earlier run compiled is in the pipeline cache, and made from there in about 0.1 ms, so its
    // draw need not skip a frame. Without this every warm run still skipped each new pipeline's first draw, and
    // layers flashed off as they came on screen (Windows, AMD, GPL off: 6,620 draws in a session, cache loaded).
    if (may_skip && async_pipelines() && !vk_gpl() && vk_pipeline_cache_control() && pipeline_cache()) {
        ci.flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
        VkPipeline p = VK_NULL_HANDLE;
        const uint64_t t0 = plat::monotonic_ns();
        const VkResult r = p_vkCreateGraphicsPipelines(vk_device(), pipeline_cache(), 1, &ci, nullptr, &p);
        ci.flags &= ~VkPipelineCreateFlags(VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT);
        if (r == VK_SUCCESS && p) {
            note_pipeline_created(plat::monotonic_ns() - t0);
            record_pipeline_key(key);
            return store(p);
        }
    }
    if (may_skip && async_pipelines()) {
        pending.emplace(k, compile_pipeline_async(build));
        return g_pipelines_skipped++, VK_NULL_HANDLE;
    }
    VkPipeline p = VK_NULL_HANDLE;
    const uint64_t t0 = plat::monotonic_ns();
    VkResult made = p_vkCreateGraphicsPipelines(vk_device(), pipeline_cache(), 1, &ci, nullptr, &p);
    note_pipeline_created(plat::monotonic_ns() - t0);
    if (made != VK_SUCCESS)
        log("nvn/vk: pipeline for %s + %s failed", key.vs->hash.c_str(), key.fs ? key.fs->hash.c_str() : "-");
    else if (g_trace)
        log("nvn/vk: pipeline %zu: %s + %s", cache.size() + 1, key.vs->hash.c_str(), key.fs ? key.fs->hash.c_str() : "-");
    if (made == VK_SUCCESS) record_pipeline_key(key);
    return store(made == VK_SUCCESS ? p : VK_NULL_HANDLE);
}

// ---- sampled textures -------------------------------------------------------------------------------------------------

// Pending nvnTextureWriteTexels regions go to the image (a transfer: outside render passes).
void apply_texel_writes(VkCommandBuffer cb, Texture& t, GpuImage& img) {
    // Nothing queued (nearly always): no lock. Every sampled texture of every draw comes here, and taking the
    // registry lock alone also invalidated every thread's get<T> cache each time (docs/nvn/PERF-PLAN.md).
    if (!t.has_writes.load(std::memory_order_acquire)) return;
    std::vector<Texture::TexelWrite> writes;
    {
        std::lock_guard lk(objects_lock());
        writes.swap(t.writes);
        t.has_writes.store(false, std::memory_order_release);
    }
    if (writes.empty()) return;
    img.texels = true;
    VkImageLayout back = img.layout;
    transition(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    bool is3d = img.view_type == VK_IMAGE_VIEW_TYPE_3D;
    for (const auto& w : writes) {
        if (w.level >= img.levels) continue;
        Upload u = upload(w.data.data(), w.data.size(), 16);
        VkBufferImageCopy r{};
        r.bufferOffset = u.offset;
        r.imageSubresource = {img.aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                                  ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT)
                                  : img.aspect,
                              w.level, is3d ? 0 : std::min(w.z, img.layers - 1), is3d ? 1 : std::min(w.d, img.layers)};
        r.imageOffset = {int32_t(w.x), int32_t(w.y), is3d ? int32_t(w.z) : 0};
        r.imageExtent = {w.w, w.h, is3d ? w.d : 1};
        p_vkCmdCopyBufferToImage(cb, u.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    }
    if (back != VK_IMAGE_LAYOUT_UNDEFINED) transition(cb, img, back);
}

uint64_t g_submit_serial = 0;

void pipeline_begin_submit() { g_submit_serial++; }

uint64_t g_not_ready[6];  // why sampled_ready said no: no image, dirty, texel writes, rendered, not uploaded, layout

bool sampled_ready(uint64_t texture) {
    Texture* t = get<Texture>(texture);
    if (!t || !t->image) return g_not_ready[0]++, false;  // no image yet (missing textures are the caller's)
    check_linear_content(*t);
    if (t->dirty && !t->image->rendered) return g_not_ready[1]++, false;
    if (t->has_writes.load(std::memory_order_acquire)) return g_not_ready[2]++, false;  // no lock (nvn.h)
    const GpuImage& img = *t->image;
    bool ok = img.rendered ? img.layout == VK_IMAGE_LAYOUT_GENERAL
                           : img.uploaded && img.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (!ok) g_not_ready[img.rendered ? 3 : !img.uploaded ? 4 : 5]++;
    return ok;
}

void sampled_ready_log() {
    log("nvn passes: textures not ready: %llu no image, %llu dirty, %llu texel writes, %llu rendered not GENERAL, "
        "%llu not uploaded, %llu uploaded in another layout",
        (unsigned long long)g_not_ready[0], (unsigned long long)g_not_ready[1], (unsigned long long)g_not_ready[2],
        (unsigned long long)g_not_ready[3], (unsigned long long)g_not_ready[4], (unsigned long long)g_not_ready[5]);
}

VkImageView attachment_view(GpuImage& img, uint32_t level, uint32_t layer, VkFormat format) {
    if (format == VK_FORMAT_UNDEFINED) format = img.format;
    level = std::min(level, img.levels - 1);
    // A 3D image's slice is a layer of its 2D-array-compatible views, counted at this level.
    layer = std::min(layer, img.view_type == VK_IMAGE_VIEW_TYPE_3D ? std::max(1u, img.depth >> level) - 1 : img.layers - 1);
    if (level == 0 && layer == 0 && img.levels == 1 && img.layers == 1 && img.view_type == VK_IMAGE_VIEW_TYPE_2D &&
        format == img.format)
        return img.view;
    auto& cache = g_attachment_views;
    auto& v = cache[{img.image, level, layer, format}];
    if (!v) {
        VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ci.image = img.image;
        ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ci.format = format;
        ci.subresourceRange = {img.aspect, level, 1, layer, 1};
        VkImageViewUsageCreateInfo usage{};
        narrow_view_usage(img, format, img.aspect == VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                                                              : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                          ci, usage);
        if (p_vkCreateImageView(vk_device(), &ci, nullptr, &v) != VK_SUCCESS) v = img.view;
    }
    return v;
}

GpuImage* ensure_contents(VkCommandBuffer cb, uint64_t texture) {
    Texture* t = get<Texture>(texture);
    GpuImage* img = t ? image_of(texture) : nullptr;
    if (!img) return nullptr;
    bool dirty = t->dirty.exchange(false);
    static const bool reupload = std::getenv("NX_TEXELS_REUPLOAD") != nullptr;  // bisecting: the old way
    bool upload = !img->rendered && (!img->uploaded || (dirty && (!img->texels || reupload)));
    // A render target (COMPRESSIBLE, flag 0x8) the GPU never wrote through this image: its guest memory never holds
    // GPU output here (another target over the same memory left only stale data), so it starts cleared, not uploaded
    // (G3 cause 2, docs/nvn/DEBUG.md #50). NX_RT_UPLOAD=1: uploaded as before; NX_ALIAS_MAGENTA=1: cleared magenta.
    static const bool rt_upload = std::getenv("NX_RT_UPLOAD") != nullptr;
    static const bool magenta = std::getenv("NX_ALIAS_MAGENTA") != nullptr;
    if (upload && !rt_upload && (t->flags & 0x8) && !img->uploaded && !img->texels && !dirty) {
        if (!t->has_writes.load(std::memory_order_acquire)) {
            clear_image(cb, *img, magenta);
            img->uploaded = true;
            upload = false;
        }
    }
    if (upload) upload_texture(cb, *t, *img);
    apply_texel_writes(cb, *t, *img);
    if (img->layout == VK_IMAGE_LAYOUT_UNDEFINED) {  // never written: black, not garbage
        transition(cb, *img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageSubresourceRange r{img->aspect, 0, img->levels, 0, img->layers};
        if (img->aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
            VkClearColorValue v{};
            p_vkCmdClearColorImage(cb, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &r);
        }
    }
    return img;
}

VkImageView sampled_view(VkCommandBuffer cb, uint64_t texture, const ImageShape& shape, const ViewInfo* view,
                         VkImageLayout* layout) {
    *layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    GpuImage* original = image_of(texture);
    GpuImage* alias = latest_overlapping_shadow_image(texture, original);
    GpuImage* img = alias != original ? alias : ensure_contents(cb, texture);
    static uint64_t no_image = 0, incompatible = 0;
    if (!img) {
        if (++no_image % 100000 == 1) log("nvn/vk: dummy sampled: %llu with no image, %llu with an incompatible view",
                                          (unsigned long long)no_image, (unsigned long long)incompatible);
        return dummy_view(cb, shape);
    }
    // A depth-compare binding on a colour image the GPU cannot compare (Wonder's 4x4 "no shadow" stand-ins on AMD):
    // its D32 twin (nvn_compare_twin.cpp).
    if (shape.depth && compare_twin_wanted(*img))
        if (VkImageView twin = compare_twin_view(cb, *img, view_type(shape))) return twin;
    *layout = img->rendered ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    transition(cb, *img, *layout);
    // The swizzle belongs to the texture (its builder, or its pool view), not to the image: an image may be shared
    // with or inherited from another texture over the same memory (#36, #42), whose swizzle differs. With the
    // image's, the UI's single-channel masks lost their alpha and drew as rectangles (docs/nvn/DEBUG.md #44).
    // The slot's own view decides: pool_swizzle is whichever slot registered the texture last (the world map's player
    // mask read as G G G G: a flat silhouette, DEBUG-2.md #67). NX_POOL_SWIZZLE_ANY=1: as before.
    static const bool pool_any = std::getenv("NX_POOL_SWIZZLE_ANY") != nullptr;
    ViewInfo vi = view ? *view : ViewInfo{};
    if (!vi.swizzle && !img->alpha_from_r)
        if (Texture* t = get<Texture>(texture)) {
            uint32_t s = pool_any || !view ? t->pool_swizzle.load() : 0u;
            if (!s)
                for (int k = 0; k < 4; k++) s |= uint32_t(std::min<uint8_t>(t->swizzle[k], 5)) << (8 * k);
            vi.swizzle = s;
        }
    // R11G11B10F: a channel reading A (NVNtextureSwizzle 5) reads ONE (1), after the texture's swizzle (G4).
    if (!ufloat_raw() && !img->alpha_from_r && (img->nvn_format == 0x3f || vi.format == 0x3f)) {
        uint32_t s = vi.swizzle ? vi.swizzle : 0x05040302u;  // 0: identity (R G B A)
        for (int k = 0; k < 4; k++)
            if ((s >> (8 * k) & 0xFF) == 5) s = (s & ~(0xFFu << (8 * k))) | (1u << (8 * k));
        vi.swizzle = s;
    }
    VkImageView v = view_of(*img, view_type(shape), vi);
    if (!v) {
        *layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (++incompatible % 1000 == 1)
            log("nvn/vk: texture %llx (%ux%u x%u, view type %d) sampled as view type %d: dummy", (unsigned long long)texture,
                img->width, img->height, img->layers, int(img->view_type), int(view_type(shape)));
        return dummy_view(cb, shape);
    }
    return v;
}

VkPipeline compute_pipeline(const ShaderInfo* cs, const ProgramLayout& layout) {
    static std::map<std::pair<const ShaderInfo*, VkPipelineLayout>, VkPipeline> cache;
    auto& p = cache[{cs, layout.layout}];
    if (!p) {
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module_of(cs);
        ci.stage.pName = "main";
        ci.layout = layout.layout;
        if (p_vkCreateComputePipelines(vk_device(), pipeline_cache(), 1, &ci, nullptr, &p) != VK_SUCCESS) {
            log("nvn/vk: compute pipeline for %s failed", cs->hash.c_str());
            p = VK_NULL_HANDLE;
        } else note_pipeline_cache_changed();
    }
    return p;
}

VkImageView storage_view(VkCommandBuffer cb, uint64_t texture, const ImageShape& shape, const ViewInfo* view) {
    GpuImage* img = texture ? ensure_contents(cb, texture) : nullptr;
    if (img && img->storage) {
        transition(cb, *img, VK_IMAGE_LAYOUT_GENERAL);
        img->rendered = true;  // the shader may write it
        ViewInfo v = view ? *view : ViewInfo{};
        v.levels = 1, v.swizzle = 0;  // storage views: one level, identity
        if (VkImageView iv = view_of(*img, view_type(shape), v)) return iv;
    }
    static std::map<VkImageViewType, GpuImage> dummies;
    VkImageViewType type = view_type(shape);
    auto& d = dummies[type];
    if (!d.image) {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = type == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_R32_UINT;
        ci.extent = {1, 1, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.usage = VK_IMAGE_USAGE_STORAGE_BIT;
        p_vkCreateImage(vk_device(), &ci, nullptr, &d.image);
        VkMemoryRequirements req{};
        p_vkGetImageMemoryRequirements(vk_device(), d.image, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        p_vkAllocateMemory(vk_device(), &ai, nullptr, &d.memory);
        p_vkBindImageMemory(vk_device(), d.image, d.memory, 0);
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = d.image;
        vi.viewType = type == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D
                      : (type == VK_IMAGE_VIEW_TYPE_2D_ARRAY ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D);
        vi.format = ci.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        p_vkCreateImageView(vk_device(), &vi, nullptr, &d.view);
        d.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        d.format = ci.format;
    }
    transition(cb, d, VK_IMAGE_LAYOUT_GENERAL);
    return d.view;
}

void forget_image_views(const GpuImage& img) {
    // The caches are ordered by image first: its views are one run from the smallest key with that image (this was
    // a scan of every cached view per destroyed texture, a visible share of the replay time).
    auto drop = [&](auto& cache, auto lowest) {
        for (auto it = cache.lower_bound(lowest); it != cache.end() && std::get<0>(it->first) == img.image;) {
            forget_framebuffers(it->second);
            if (it->second && it->second != img.view) p_vkDestroyImageView(vk_device(), it->second, nullptr);
            it = cache.erase(it);
        }
    };
    forget_framebuffers(img.view);  // nvn_renderpass.cpp (attachments may use the whole-image view)
    drop(g_sampled_views, std::make_tuple(img.image, VkImageViewType(0), 0u, uint64_t(0), uint64_t(0)));
    for (ViewHit& h : g_view_hits)
        if (std::get<0>(h.key) == img.image) h = {};
    drop(g_attachment_views, std::make_tuple(img.image, 0u, 0u, VkFormat(0)));
}

}  // namespace nx::nvn
