// Texture uploads (nvn_pipeline.h): guest pool memory into the host image, untiled, widened where the host stores a
// format wider than NVN does, and checked against what the copies read (docs/nvn/DEBUG.md #50).
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

#include "nvn_astc.h"
#include "nvn_pipeline.h"
#include "nvn_tiling.h"
#include "platform.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {

// R11G11B10F (NVN 0x3f) is stored as RGBA16F (nvn_format.cpp). Its channels are unsigned floats with half's 5-bit
// exponent and bias 15, and 6 (R, G) or 5 (B) mantissa bits: each is a half with the low mantissa bits zero and no
// sign, so shifting it into place is exact (Inf and NaN included). Alpha is 1. Copying the 4-byte texels as they are
// read twice the data: rows side by side, bits reinterpreted, the bottom half from other uploads (the garbage band).
void widen_r11g11b10(const uint8_t* src, uint8_t* dst, uint64_t texels) {
    for (uint64_t k = 0; k < texels; k++) {
        uint32_t v;
        std::memcpy(&v, src + k * 4, 4);
        uint16_t h[4] = {uint16_t((v & 0x7FF) << 4), uint16_t(((v >> 11) & 0x7FF) << 4),
                         uint16_t(((v >> 22) & 0x3FF) << 5), 0x3C00};
        std::memcpy(dst + k * 8, h, 8);
    }
}

// Bytes from `base` to the end of what vkCmdCopyBufferToImage reads for `r`, at `bytes` per bw x bh block.
uint64_t copy_end(const VkBufferImageCopy& r, uint64_t base, uint32_t bw, uint32_t bh, uint32_t bytes) {
    uint64_t row_blocks = ((r.bufferRowLength ? r.bufferRowLength : r.imageExtent.width) + bw - 1) / bw;
    uint64_t image_rows = ((r.bufferImageHeight ? r.bufferImageHeight : r.imageExtent.height) + bh - 1) / bh;
    uint64_t w = (r.imageExtent.width + bw - 1) / bw, h = (r.imageExtent.height + bh - 1) / bh;
    uint64_t slices = uint64_t(r.imageExtent.depth) * r.imageSubresource.layerCount;
    if (!w || !h || !slices) return r.bufferOffset - base;
    return r.bufferOffset - base + ((slices - 1) * image_rows + (h - 1)) * row_blocks * bytes + w * bytes;
}

// The copies read at most `staged` bytes from `base`, counted at the host format's size. A copy past it reads other
// uploads of the submit (or past the buffer): garbage, so the image is cleared instead. NX_NO_UPLOAD_GUARD=1: the old
// way (bisecting).
bool copies_fit(const Texture& t, const VkBufferImageCopy* r, uint32_t n, uint64_t base, uint64_t staged, uint32_t bw,
                uint32_t bh, uint32_t bytes) {
    static const bool off = std::getenv("NX_NO_UPLOAD_GUARD") != nullptr;
    uint64_t need = 0;
    for (uint32_t k = 0; k < n; k++) need = std::max(need, copy_end(r[k], base, bw, bh, bytes));
    if (off || need <= staged) return true;
    static std::set<uint32_t> logged;  // once per format
    if (logged.insert(t.format).second)
        log("nvn/vk: upload short: texture %ux%u format %#x reads %llu bytes, %llu staged: cleared instead", t.width,
            t.height, t.format, (unsigned long long)need, (unsigned long long)staged);
    return false;
}

// The RGBA8 layout of decoded ASTC levels: each level's layers back to back, 16-byte aligned. `total`: its size.
std::vector<LinearLevel> rgba_levels(const std::vector<LinearLevel>& levels, uint32_t layers, uint64_t& total) {
    std::vector<LinearLevel> out = levels;
    total = 0;
    for (LinearLevel& l : out) {
        l.offset = total;
        l.layer_size = uint64_t(l.width) * l.height * 4;
        l.width_blocks = l.width, l.height_blocks = l.height;
        total += (l.layer_size * layers + 15) & ~15ull;
    }
    return out;
}

// Whether the untiled blocks hold what the decoder reads of level `l`, which can exceed layer_size when the level's
// block counts disagree with it. Logged when not: the level is left black.
bool astc_level_fits(const LinearLevel& l, uint32_t layers, uint32_t bw, uint32_t bh, uint64_t untiled) {
    const uint64_t need = uint64_t((l.width + bw - 1) / bw) * ((l.height + bh - 1) / bh) * 16 * layers;
    if (l.offset + std::max(need, l.layer_size * layers) <= untiled) return true;
    log("nvn/vk: ASTC level %ux%u x%u: %llu bytes needed, %llu untiled: left black", l.width, l.height, layers,
        (unsigned long long)need, (unsigned long long)(untiled - l.offset));
    return false;
}

// ASTC on a GPU without it (nvn_format.cpp), on the CPU (NX_ASTC_CPU=1, or without the compute pipeline): the
// untiled blocks of every level become RGBA8 texels, laid out by rgba_levels. Logged every 200 textures: count,
// bytes and CPU time, the cost of the decoder at load.
void decode_astc(uint32_t format, std::vector<uint8_t>& linear, std::vector<LinearLevel>& levels, uint32_t layers,
                 uint32_t bw, uint32_t bh) {
    static uint64_t textures = 0, bytes = 0, ns = 0, invalid = 0;
    const uint64_t t0 = plat::monotonic_ns();
    const bool srgb = format >= 0x87;
    uint64_t at = 0;
    std::vector<LinearLevel> out = rgba_levels(levels, layers, at);
    std::vector<uint8_t> rgba(at);
    for (size_t k = 0; k < levels.size(); k++)
        if (astc_level_fits(levels[k], layers, bw, bh, linear.size()))
            invalid += astc_decode_image(linear.data() + levels[k].offset, levels[k].width, levels[k].height, layers,
                                         bw, bh, srgb, rgba.data() + out[k].offset);
    linear.swap(rgba);
    levels.swap(out);
    const uint64_t spent = plat::monotonic_ns() - t0;
    note_astc_decoded(spent);  // per timing window (nvn_pipeline_cache.cpp)
    bytes += at, ns += spent;
    if (++textures % 200 == 1)
        log("nvn/vk: ASTC decoded on the CPU: %llu textures, %.1f MB, %.0f ms, %llu invalid blocks",
            (unsigned long long)textures, double(bytes) / 1e6, double(ns) / 1e6, (unsigned long long)invalid);
}

static uint16_t pack_565(uint8_t r, uint8_t g, uint8_t b) {
    return uint16_t((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
}

// A compact, deterministic BC3 encoder used only by the opt-in low-VRAM profile. It chooses block endpoint
// extrema and nearest palette entries; quality is lower than a high-quality offline encoder but storage is 1 B/px.
static void encode_bc3_block(const uint8_t* rgba, uint32_t stride, uint32_t width, uint32_t height, uint32_t x0,
                             uint32_t y0, uint8_t* out) {
    uint8_t pixels[16][4];
    for (uint32_t y = 0; y < 4; ++y) for (uint32_t x = 0; x < 4; ++x) {
        const uint32_t sx = std::min(x0 + x, width - 1), sy = std::min(y0 + y, height - 1);
        std::memcpy(pixels[y * 4 + x], rgba + uint64_t(sy) * stride + uint64_t(sx) * 4, 4);
    }
    uint8_t amin = 255, amax = 0;
    for (const auto& p : pixels) amin = std::min(amin, p[3]), amax = std::max(amax, p[3]);
    out[0] = amax; out[1] = amin;
    uint8_t ap[8] = {amax, amin};
    for (int i = 1; i <= 6; ++i) ap[i + 1] = uint8_t(((6 - i) * amax + (i - 1) * amin) / 5);
    uint64_t ai = 0;
    for (int i = 0; i < 16; ++i) {
        int best = 0, error = 1 << 30;
        for (int j = 0; j < 8; ++j) { int d = int(pixels[i][3]) - ap[j]; if (d * d < error) error = d * d, best = j; }
        ai |= uint64_t(best) << (3 * i);
    }
    for (int i = 0; i < 6; ++i) out[2 + i] = uint8_t(ai >> (8 * i));

    int lo = 0, hi = 0;
    auto lum = [&](int i) { return 299 * int(pixels[i][0]) + 587 * int(pixels[i][1]) + 114 * int(pixels[i][2]); };
    for (int i = 1; i < 16; ++i) { if (lum(i) < lum(lo)) lo = i; if (lum(i) > lum(hi)) hi = i; }
    uint16_t c0 = pack_565(pixels[hi][0], pixels[hi][1], pixels[hi][2]);
    uint16_t c1 = pack_565(pixels[lo][0], pixels[lo][1], pixels[lo][2]);
    if (c0 <= c1) { if (c0 < 0xffff) c0 = c1 + 1; else c1 = c0 - 1; }
    std::memcpy(out + 8, &c0, 2); std::memcpy(out + 10, &c1, 2);
    auto color = [](uint16_t c) {
        return std::array<int, 3>{int((c >> 11) & 31) * 255 / 31, int((c >> 5) & 63) * 255 / 63, int(c & 31) * 255 / 31};
    };
    const auto a = color(c0), b = color(c1);
    int palette[4][3];
    for (int ch = 0; ch < 3; ++ch) palette[0][ch] = a[ch], palette[1][ch] = b[ch],
        palette[2][ch] = (2 * a[ch] + b[ch]) / 3, palette[3][ch] = (a[ch] + 2 * b[ch]) / 3;
    uint32_t ci = 0;
    for (int i = 0; i < 16; ++i) {
        int best = 0, error = 1 << 30;
        for (int j = 0; j < 4; ++j) { int d = 0; for (int ch = 0; ch < 3; ++ch) { int q = int(pixels[i][ch]) - palette[j][ch]; d += q * q; }
            if (d < error) error = d, best = j; }
        ci |= uint32_t(best) << (2 * i);
    }
    std::memcpy(out + 12, &ci, 4);
}

static void transcode_astc_to_bc3(std::vector<uint8_t>& rgba, std::vector<LinearLevel>& levels, uint32_t layers) {
    std::vector<LinearLevel> out = levels;
    uint64_t total = 0;
    for (LinearLevel& l : out) {
        l.width_blocks = (l.width + 3) / 4; l.height_blocks = (l.height + 3) / 4;
        l.layer_size = uint64_t(l.width_blocks) * l.height_blocks * 16;
        l.offset = total; total += (l.layer_size * layers + 15) & ~15ull;
    }
    std::vector<uint8_t> compressed(total);
    for (size_t level = 0; level < levels.size(); ++level) {
        const auto& src = levels[level]; auto& dst = out[level];
        for (uint32_t layer = 0; layer < layers; ++layer) {
            const uint8_t* image = rgba.data() + src.offset + uint64_t(layer) * src.layer_size;
            uint8_t* blocks = compressed.data() + dst.offset + uint64_t(layer) * dst.layer_size;
            for (uint32_t y = 0; y < dst.height_blocks; ++y) for (uint32_t x = 0; x < dst.width_blocks; ++x)
                encode_bc3_block(image, src.width * 4, src.width, src.height, x * 4, y * 4,
                                 blocks + (uint64_t(y) * dst.width_blocks + x) * 16);
        }
    }
    rgba.swap(compressed); levels.swap(out);
}

// Whether a 1 MB+ upload gets a staging buffer of its own (PHASE4.md U1). In steady state (a course frame stages ~30
// such textures, ~40 MB per second) the ring takes them: a fresh buffer each was an allocation (a kgsl ioctl that
// zero-fills), a page fault per 4 KB of the copy and a free at retire, on the replay thread. A scene load, which
// stages hundreds of MB in one submit, still gets its own buffers: past a quarter of the ring in this submit, or one
// upload over an eighth of it. NX_STAGING_MIN=0: a staging buffer for every 1 MB+ upload, as before.
bool staging_wanted(uint64_t size) {
    static const bool always = [] {
        const char* e = std::getenv("NX_STAGING_MIN");
        return e && e[0] == '0';
    }();
    uint64_t ring = frame_ring_size();
    return always || !ring || frame_upload_used() + size > ring / 4 || size > ring / 8;
}

// Staging memory for `size` bytes of a texture: 1 MB and up a buffer of its own (staging_wanted), so a scene load
// cannot wrap the ring over itself (DEBUG-2 #72); NX_STAGING_RING=1: through the ring, as before (bisecting).
Upload texture_staging(uint64_t size) {
    static const bool ring = std::getenv("NX_STAGING_RING") != nullptr;
    if (size >= (1u << 20) && !ring && staging_wanted(size)) return staging_alloc(size);
    return upload_alloc(size, 16);
}

// glslc of runtime/shaders/astc_decode.comp, compiled by the build (CMakeLists.txt).
const uint32_t kAstcDecode[] = {
#include "astc_decode.comp.inc"
};

struct AstcPush {
    uint32_t width, height, slices, bw, bh, srgb, in_word, out_word;
};

struct AstcPipeline {
    VkDescriptorSetLayout set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
};

// Made on first use; null if the device refused it (ASTC is then decoded on the CPU).
const AstcPipeline* astc_pipeline() {
    static AstcPipeline p;
    static bool made = false;
    if (made) return p.pipeline ? &p : nullptr;
    made = true;
    VkDevice d = vk_device();
    VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                         {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo sci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sci.bindingCount = 2, sci.pBindings = b;
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(AstcPush)};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount = 1, lci.pSetLayouts = &p.set, lci.pushConstantRangeCount = 1, lci.pPushConstantRanges = &push;
    VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mci.codeSize = sizeof kAstcDecode, mci.pCode = kAstcDecode;
    VkShaderModule module = VK_NULL_HANDLE;
    if (p_vkCreateDescriptorSetLayout(d, &sci, nullptr, &p.set) != VK_SUCCESS ||
        p_vkCreatePipelineLayout(d, &lci, nullptr, &p.layout) != VK_SUCCESS ||
        p_vkCreateShaderModule(d, &mci, nullptr, &module) != VK_SUCCESS) {
        log("nvn/vk: ASTC decode shader rejected: decoded on the CPU");
        return nullptr;
    }
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main"};
    ci.layout = p.layout;
    if (p_vkCreateComputePipelines(d, pipeline_cache(), 1, &ci, nullptr, &p.pipeline) != VK_SUCCESS) {
        log("nvn/vk: ASTC decode pipeline failed: decoded on the CPU");
        p.pipeline = VK_NULL_HANDLE;
    }
    p_vkDestroyShaderModule(d, module, nullptr);
    return p.pipeline ? &p : nullptr;
}

// decode_astc on the GPU: the blocks are staged as they are and a dispatch per level (a thread per texel) writes the
// RGBA8 texels into `u` (`size` bytes, laid out by rgba_levels), which the copies then read as they read the CPU's.
// Recorded on cb, outside render passes. False when there is no pipeline or NX_ASTC_CPU=1 (decode_astc then).
bool decode_astc_gpu(VkCommandBuffer cb, uint32_t format, const std::vector<uint8_t>& linear,
                     std::vector<LinearLevel>& levels, uint32_t layers, uint32_t bw, uint32_t bh, Upload& u,
                     uint64_t& size) {
    static const bool cpu = [] {  // NX_ASTC_CPU=1: CPU decoder; 0: GPU; unset: CPU on NVIDIA Pascal
        if (const char* e = std::getenv("NX_ASTC_CPU"); e && *e) return e[0] != '0';
        VkPhysicalDeviceProperties props{};
        if (vk_physical()) p_vkGetPhysicalDeviceProperties(vk_physical(), &props);
        const bool nvidia_pascal = props.vendorID == 0x10de &&
            ((props.deviceID >= 0x1b00 && props.deviceID <= 0x1dff) ||
             (props.deviceID >= 0x15f0 && props.deviceID <= 0x15ff));
        if (nvidia_pascal)
            log("nvn/vk: ASTC decoded on the CPU by default for NVIDIA Pascal (NX_ASTC_CPU=0 to override)");
        return nvidia_pascal;
    }();
    static uint64_t textures = 0, bytes = 0, ns = 0;
    const AstcPipeline* p = cpu ? nullptr : astc_pipeline();
    if (!p) return false;
    const uint64_t t0 = plat::monotonic_ns();
    std::vector<LinearLevel> out = rgba_levels(levels, layers, size);
    Upload in = texture_staging(linear.size());
    std::memcpy(in.ptr, linear.data(), linear.size());
    u = texture_staging(size);
    // Bound from an offset the device accepts (minStorageBufferOffsetAlignment is at most 256); the rest goes in the
    // push constants, in words (every offset here is a multiple of 16).
    const uint64_t in_base = in.offset & ~255ull, out_base = u.offset & ~255ull;
    VkDescriptorSet set = descriptor_set(p->set);
    VkDescriptorBufferInfo bi[2] = {{in.buffer, in_base, in.offset - in_base + linear.size()},
                                    {u.buffer, out_base, u.offset - out_base + size}};
    VkWriteDescriptorSet w[2] = {};
    for (uint32_t k = 0; k < 2; k++) {
        w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[k].dstSet = set, w[k].dstBinding = k, w[k].descriptorCount = 1;
        w[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, w[k].pBufferInfo = &bi[k];
    }
    p_vkUpdateDescriptorSets(vk_device(), 2, w, 0, nullptr);
    p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipeline);
    p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p->layout, 0, 1, &set, 0, nullptr);
    for (size_t k = 0; k < levels.size(); k++) {
        if (!astc_level_fits(levels[k], layers, bw, bh, linear.size())) {
            std::memset(u.ptr + out[k].offset, 0, out[k].layer_size * layers);
            continue;
        }
        AstcPush pc{levels[k].width, levels[k].height, layers, bw, bh, format >= 0x87,
                    uint32_t((in.offset - in_base + levels[k].offset) / 4), uint32_t((u.offset - out_base + out[k].offset) / 4)};
        p_vkCmdPushConstants(cb, p->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
        p_vkCmdDispatch(cb, (levels[k].width + 7) / 8, (levels[k].height + 7) / 8, layers);
    }
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0,
                           nullptr, 0, nullptr);
    levels.swap(out);
    const uint64_t spent = plat::monotonic_ns() - t0;
    note_astc_decoded(spent);  // per timing window (nvn_pipeline_cache.cpp): now only the staging and recording
    bytes += size, ns += spent;
    if (++textures % 200 == 1)
        log("nvn/vk: ASTC decoded on the GPU: %llu textures, %.1f MB, %.1f ms to stage and record",
            (unsigned long long)textures, double(bytes) / 1e6, double(ns) / 1e6);
    return true;
}

}  // namespace

// NX_1D_IMAGES=1: NVN 1D textures as Vulkan 1D images again (they are 2D of height 1: Ryujinx samples 1D as 2D, G8).
bool vk_1d_images() {
    static const bool on = std::getenv("NX_1D_IMAGES") != nullptr;
    return on;
}

void clear_image(VkCommandBuffer cb, GpuImage& img, bool magenta) {
    note_written(img);
    transition(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageSubresourceRange r{img.aspect, 0, img.levels, 0, img.layers};
    if (img.aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
        VkClearColorValue v{};
        if (magenta) v = {{1.0f, 0.0f, 1.0f, 1.0f}};
        p_vkCmdClearColorImage(cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &r);
    } else {
        VkClearDepthStencilValue v{1.0f, 0};  // far: the course passes test LESS
        p_vkCmdClearDepthStencilImage(cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &r);
    }
}

void upload_texture(VkCommandBuffer cb, const Texture& t, GpuImage& img) {
    img.uploaded = true;
    note_written(img);
    if (gpu_profile_on()) gpu_profile_mark(cb, "texture uploads");
    if (!t.memory || !t.storage_size) return;
    if (std::getenv("NX_TRACE_UPLOADS") && t.storage_size >= (1u << 20)) {
        const uint8_t* m = reinterpret_cast<const uint8_t*>(t.memory);
        size_t nz = 0;
        for (uint64_t k = 0; k < t.storage_size; k += 64) nz += m[k] != 0;
        log("nvn: upload %ux%u fmt %#x stride %u: %zu of %llu sampled bytes non-zero", t.width, t.height, t.format,
            t.stride, nz, (unsigned long long)(t.storage_size / 64));
    }
    uint32_t bw = 1, bh = 1, bytes = 4;
    format_block(t.format, bw, bh, bytes);
    // What the copies read per block: the host format's size (RGBA16F for R11G11B10F), else the NVN one.
    uint32_t host_bytes = vk_block_bytes(img.format) ? vk_block_bytes(img.format) : bytes;
    // NX_R11G11B10_NARROW=1: R11G11B10F copied at 4 bytes per texel, as before (the guard then clears it unless
    // NX_NO_UPLOAD_GUARD=1 too; bisecting).
    static const bool narrow = std::getenv("NX_R11G11B10_NARROW") != nullptr;
    bool widen = !narrow && t.format == 0x3f && img.format == VK_FORMAT_R16G16B16A16_SFLOAT && !img.alpha_from_r;
    std::vector<uint8_t> linear;
    std::vector<VkBufferImageCopy> copies;
    uint32_t layers = img.view_type == VK_IMAGE_VIEW_TYPE_3D ? std::max(1u, t.depth) : img.layers;
    if (img.alpha_from_r && t.stride) {  // R8 pitch-linear -> RGBA8 (255, 255, 255, R), level 0
        const uint8_t* src = reinterpret_cast<const uint8_t*>(t.memory);
        uint64_t rows = uint64_t(img.height) * layers;
        if (uint64_t(t.stride) * rows > t.storage_size) rows = t.storage_size / t.stride;
        Upload u = upload_alloc(rows * img.width * 4, 16);
        for (uint64_t y = 0; y < rows; y++) {
            const uint8_t* s = src + y * t.stride;
            uint32_t* d = reinterpret_cast<uint32_t*>(u.ptr + y * img.width * 4);
            for (uint32_t x = 0; x < img.width; x++) d[x] = 0x00FFFFFFu | (uint32_t(s[x]) << 24);
        }
        VkBufferImageCopy r{};
        r.bufferOffset = u.offset;
        r.imageSubresource = {img.aspect, 0, 0, img.layers};
        r.imageExtent = {img.width, uint32_t(std::min<uint64_t>(rows, img.height)), 1};
        transition(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        p_vkCmdCopyBufferToImage(cb, u.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        return;
    }
    bool decode = bc_host_decode(t.format);  // nvn_bc.cpp: no BC on this GPU
    if (decode && t.stride) {
        static bool logged = false;
        if (!logged) log("nvn/vk: pitch-linear BC texture (format %#x): not decoded, cleared", t.format), logged = true;
        return clear_image(cb, img);
    }
    if (t.stride && astc_decoded(t.format)) {  // never seen: pitch-linear ASTC would need the decoder too
        log("nvn/vk: pitch-linear ASTC texture %ux%u format %#x: cleared", img.width, img.height, t.format);
        return clear_image(cb, img);
    }
    if (t.stride) {  // pitch-linear: rows as they are
        uint64_t rows = (img.height + bh - 1) / bh;
        VkBufferImageCopy r{};
        r.imageSubresource = {img.aspect, 0, 0, img.layers};
        r.imageExtent = {img.width, img.height, 1};
        Upload u;
        uint64_t staged = 0;
        if (widen) {  // tightly packed RGBA16F rows; rows past the storage stay zero
            const uint8_t* src = reinterpret_cast<const uint8_t*>(t.memory);
            uint64_t n = rows * layers, have = std::min<uint64_t>(n, t.storage_size / t.stride);
            uint64_t texels = std::min<uint64_t>(img.width, t.stride / 4);
            staged = n * img.width * 8;
            u = upload_alloc(staged, 16);
            std::memset(u.ptr, 0, staged);
            for (uint64_t y = 0; y < have; y++) widen_r11g11b10(src + y * t.stride, u.ptr + y * img.width * 8, texels);
        } else {
            staged = uint64_t(t.stride) * rows * layers;
            u = upload(reinterpret_cast<const void*>(t.memory), staged, 16);
            r.bufferRowLength = t.stride / bytes * bw;
        }
        r.bufferOffset = u.offset;
        if (!copies_fit(t, &r, 1, u.offset, staged, bw, bh, host_bytes)) return clear_image(cb, img);
        transition(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        p_vkCmdCopyBufferToImage(cb, u.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        return;
    }
    auto levels = deswizzle_texture(reinterpret_cast<const uint8_t*>(t.memory), t.storage_size, img.width,
                                    img.height, layers, img.levels, bw, bh, bytes, linear, t.block_height_gobs);
    if (levels.empty()) {
        log("nvn/vk: texture %ux%u format %#x: could not untile (%llu bytes)", img.width, img.height, t.format,
            (unsigned long long)t.storage_size);
        return;
    }
    if (decode) {  // every level's blocks -> texels, tightly packed, then copied as an uncompressed image
        uint32_t tb = bc_decoded_bytes(t.format), slices = img.view_type == VK_IMAGE_VIEW_TYPE_3D ? layers : img.layers;
        uint64_t total = 0;
        for (const auto& l : levels) total += uint64_t(l.width) * l.height * slices * tb;
        Upload u = upload_alloc(total, 16);
        uint64_t at = 0;
        for (uint32_t k = 0; k < levels.size(); k++) {
            bc_decode(t.format, linear.data() + levels[k].offset, levels[k].width, levels[k].height, slices, u.ptr + at);
            VkBufferImageCopy r{};
            r.bufferOffset = u.offset + at;
            r.imageSubresource = {img.aspect, k, 0, img.view_type == VK_IMAGE_VIEW_TYPE_3D ? 1u : img.layers};
            r.imageExtent = {levels[k].width, levels[k].height, img.view_type == VK_IMAGE_VIEW_TYPE_3D ? layers : 1u};
            copies.push_back(r);
            at += uint64_t(levels[k].width) * levels[k].height * slices * tb;
        }
        transition(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        p_vkCmdCopyBufferToImage(cb, u.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(copies.size()),
                                 copies.data());
        return;
    }
    Upload u;
    uint64_t staged = 0;
    bool gpu_decoded = false;  // the texels are in u already
    if (astc_decoded(t.format)) {  // RGBA8 texels from here on
        if (img.format == VK_FORMAT_BC3_UNORM_BLOCK || img.format == VK_FORMAT_BC3_SRGB_BLOCK) {
            // Low-VRAM profile: BC3 is encoded from the CPU decode (the GPU decoder writes RGBA8 only).
            decode_astc(t.format, linear, levels, layers, bw, bh);
            transcode_astc_to_bc3(linear, levels, layers);
            bw = bh = 4, bytes = host_bytes = 16;
            static uint64_t bc3_textures = 0;
            if (++bc3_textures == 1 || bc3_textures % 500 == 0)
                log("nvn/vk: ASTC textures transcoded to BC3 for low-VRAM profile: %llu", (unsigned long long)bc3_textures);
        } else {
            gpu_decoded = decode_astc_gpu(cb, t.format, linear, levels, layers, bw, bh, u, staged);
            if (!gpu_decoded) decode_astc(t.format, linear, levels, layers, bw, bh);
            bw = bh = 1, bytes = host_bytes = 4;
        }
    }
    // Widened, every offset in the linear buffer doubles (level offsets are multiples of the 4-byte texel).
    uint64_t scale = widen ? 2 : 1;
    if (gpu_decoded) {
    } else if (widen) {
        u = upload_alloc(linear.size() * 2, 16);
        widen_r11g11b10(linear.data(), u.ptr, linear.size() / 4);
        staged = linear.size();
    } else {
        u = texture_staging(linear.size());
        std::memcpy(u.ptr, linear.data(), linear.size());
        staged = linear.size();
    }
    for (uint32_t k = 0; k < levels.size(); k++) {
        VkBufferImageCopy r{};
        r.bufferOffset = u.offset + levels[k].offset * scale;
        r.imageSubresource = {img.aspect, k, 0, img.view_type == VK_IMAGE_VIEW_TYPE_3D ? 1u : img.layers};
        r.imageExtent = {levels[k].width, levels[k].height, img.view_type == VK_IMAGE_VIEW_TYPE_3D ? layers : 1u};
        copies.push_back(r);
    }
    if (!copies_fit(t, copies.data(), uint32_t(copies.size()), u.offset, staged * scale, bw, bh, host_bytes))
        return clear_image(cb, img);
    transition(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    p_vkCmdCopyBufferToImage(cb, u.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(copies.size()),
                             copies.data());
}

}  // namespace nx::nvn
