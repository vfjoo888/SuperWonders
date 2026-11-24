// Depth-compare twins: a D32 copy of a colour texture that the game samples with a depth-compare (shadow) sampler.
//
// Maxwell compares against the first channel of any format, and Wonder relies on it: where a lit layer has no shadow
// map it binds small colour textures of its own instead (4x4 RGBA8 and R32F, "nothing is in shadow"), read through
// the same compare samplers (fp_t_tcb_1A, cp_t_tcb_20). Vulkan allows a compare only on formats with
// SAMPLED_IMAGE_DEPTH_COMPARISON: the validation layer reported both on every course frame (VUID-...-06479).
// NVIDIA's driver compares colour formats anyway; on AMD the result is undefined. Vulkan cannot view a colour format
// as depth, so the twin is a separate D32_SFLOAT image of the same size, refreshed by copy whenever the source was
// rendered again since the last copy (GpuImage::rendered_at):
//   R32F            -> buffer -> D32 (the same bits; valid on every driver);
//   other colour    -> blit to an R32F image (converts the first channel to float) -> buffer -> D32.
// Integer and block-compressed formats are left as they were. NX_COMPARE_TWIN=0: off, the colour view as before.
#include <algorithm>
#include <cstdlib>
#include <map>
#include <vector>

#include "nvn_pipeline.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {

struct Twin {
    GpuImage image;
    GpuImage r32f;  // the blit's target when the source is not R32F
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory buffer_memory = VK_NULL_HANDLE;
    uint64_t copied_serial = ~0ull;  // the source's rendered_at at the last copy
    std::map<VkImageViewType, VkImageView> views;
};

std::map<VkImage, Twin>& twins() {
    static std::map<VkImage, Twin> t;
    return t;
}

bool enabled() {
    static const bool on = [] {
        const char* e = std::getenv("NX_COMPARE_TWIN");
        return !(e && e[0] == '0');
    }();
    return on;
}

bool compares(VkFormat f) {
    static std::map<VkFormat, bool> cache;
    auto it = cache.find(f);
    if (it != cache.end()) return it->second;
    VkFormatProperties fp{};
    p_vkGetPhysicalDeviceFormatProperties(vk_physical(), f, &fp);
    // VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT has no VkFormatFeatureFlags (1) bit: a depth format
    // compares, a colour one is taken as unable to.
    bool depth = f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D32_SFLOAT_S8_UINT ||
                 f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_X8_D24_UNORM_PACK32;
    return cache[f] = depth && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
}

// Every write moves it (note_written): rendered_at moved at pass ends only, and an image filled by a clear, a copy or
// an upload kept the twin of its first contents (Wonder's "nothing in shadow" stand-in read as all in shadow on AMD:
// dark grass and blotched characters on the world map, vfjoo888/SuperWonders#123).
uint64_t serial_of(const GpuImage& img) { return img.written; }

bool make(Twin& t, const GpuImage& src) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_D32_SFLOAT;
    ci.extent = {src.width, src.height, 1};
    ci.mipLevels = src.levels;
    ci.arrayLayers = src.layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (p_vkCreateImage(vk_device(), &ci, nullptr, &t.image.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    p_vkGetImageMemoryRequirements(vk_device(), t.image.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (p_vkAllocateMemory(vk_device(), &ai, nullptr, &t.image.memory) != VK_SUCCESS) return false;
    p_vkBindImageMemory(vk_device(), t.image.image, t.image.memory, 0);
    t.image.format = ci.format;
    t.image.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    t.image.width = src.width, t.image.height = src.height, t.image.levels = src.levels, t.image.layers = src.layers;
    if (src.format != VK_FORMAT_R32_SFLOAT) {
        ci.format = VK_FORMAT_R32_SFLOAT;
        ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (p_vkCreateImage(vk_device(), &ci, nullptr, &t.r32f.image) != VK_SUCCESS) return false;
        p_vkGetImageMemoryRequirements(vk_device(), t.r32f.image, &req);
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (p_vkAllocateMemory(vk_device(), &ai, nullptr, &t.r32f.memory) != VK_SUCCESS) return false;
        p_vkBindImageMemory(vk_device(), t.r32f.image, t.r32f.memory, 0);
        t.r32f.format = ci.format;
    }

    uint64_t bytes = 0;
    for (uint32_t l = 0; l < src.levels; l++)
        bytes += uint64_t(std::max(src.width >> l, 1u)) * std::max(src.height >> l, 1u) * 4 * src.layers;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (p_vkCreateBuffer(vk_device(), &bi, nullptr, &t.buffer) != VK_SUCCESS) return false;
    p_vkGetBufferMemoryRequirements(vk_device(), t.buffer, &req);
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (p_vkAllocateMemory(vk_device(), &ai, nullptr, &t.buffer_memory) != VK_SUCCESS) return false;
    p_vkBindBufferMemory(vk_device(), t.buffer, t.buffer_memory, 0);
    log("nvn/vk: depth-compare twin for a %ux%u x%u colour image, Vulkan format %d (no depth comparison on it)",
        src.width, src.height, src.layers, int(src.format));
    return true;
}

void destroy(Twin& t) {
    for (auto& [type, v] : t.views) p_vkDestroyImageView(vk_device(), v, nullptr);
    if (t.image.image) p_vkDestroyImage(vk_device(), t.image.image, nullptr);
    if (t.image.memory) p_vkFreeMemory(vk_device(), t.image.memory, nullptr);
    if (t.buffer) p_vkDestroyBuffer(vk_device(), t.buffer, nullptr);
    if (t.buffer_memory) p_vkFreeMemory(vk_device(), t.buffer_memory, nullptr);
    if (t.r32f.image) p_vkDestroyImage(vk_device(), t.r32f.image, nullptr);
    if (t.r32f.memory) p_vkFreeMemory(vk_device(), t.r32f.memory, nullptr);
    t = Twin{};
}

void copy(VkCommandBuffer cb, Twin& t, GpuImage& src) {
    // Everything that wrote the source is done before the copy reads it (it may already be in GENERAL: transition
    // would add no barrier then).
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = src.layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_GENERAL : src.layout;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = src.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    VkImageMemoryBarrier tb = b;  // the twin: whatever it held is replaced
    tb.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    tb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    tb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    tb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    tb.image = t.image.image;
    tb.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    VkImageMemoryBarrier both[2] = {b, tb};
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                           nullptr, 2, both);
    src.layout = VK_IMAGE_LAYOUT_GENERAL;

    // Not R32F: blit it into the R32F image first (each level, every layer), then copy from there.
    VkImage from = src.image;
    VkImageLayout from_layout = VK_IMAGE_LAYOUT_GENERAL;
    if (t.r32f.image) {
        VkImageMemoryBarrier rb = b;
        rb.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        rb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        rb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        rb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        rb.image = t.r32f.image;
        p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                               nullptr, 1, &rb);
        std::vector<VkImageBlit> blits;
        for (uint32_t l = 0; l < src.levels; l++) {
            int32_t w = int32_t(std::max(src.width >> l, 1u)), h = int32_t(std::max(src.height >> l, 1u));
            VkImageBlit r{};
            r.srcSubresource = r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, src.layers};
            r.srcOffsets[1] = r.dstOffsets[1] = {w, h, 1};
            blits.push_back(r);
        }
        p_vkCmdBlitImage(cb, src.image, VK_IMAGE_LAYOUT_GENERAL, t.r32f.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         uint32_t(blits.size()), blits.data(), VK_FILTER_NEAREST);
        rb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        rb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        rb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        rb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                               nullptr, 1, &rb);
        from = t.r32f.image;
        from_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }

    std::vector<VkBufferImageCopy> regions;
    VkDeviceSize offset = 0;
    for (uint32_t l = 0; l < src.levels; l++) {
        uint32_t w = std::max(src.width >> l, 1u), h = std::max(src.height >> l, 1u);
        VkBufferImageCopy r{};
        r.bufferOffset = offset;
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, src.layers};
        r.imageExtent = {w, h, 1};
        regions.push_back(r);
        offset += VkDeviceSize(w) * h * 4 * src.layers;
    }
    p_vkCmdCopyImageToBuffer(cb, from, from_layout, t.buffer, uint32_t(regions.size()), regions.data());
    VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.buffer = t.buffer;
    bb.size = VK_WHOLE_SIZE;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &bb, 0,
                           nullptr);
    for (VkBufferImageCopy& r : regions) r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    p_vkCmdCopyBufferToImage(cb, t.buffer, t.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(regions.size()),
                             regions.data());
    tb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    tb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    tb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    tb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &tb);
    t.image.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    t.copied_serial = serial_of(src);
}

}  // namespace

void note_written(GpuImage& img) {
    static uint64_t serial = 0;  // under the GPU lock, as every caller
    img.written = ++serial;
}

static bool blittable(VkFormat f) {
    static std::map<VkFormat, bool> cache;
    auto it = cache.find(f);
    if (it != cache.end()) return it->second;
    VkFormatProperties fp{}, r32{};
    p_vkGetPhysicalDeviceFormatProperties(vk_physical(), f, &fp);
    p_vkGetPhysicalDeviceFormatProperties(vk_physical(), VK_FORMAT_R32_SFLOAT, &r32);
    // Blits need float, unorm or snorm on both sides (no integer formats) and no block compression.
    bool integer = false;
    switch (f) {
        case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT:
        case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT:
        case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16A16_UINT:
        case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_UINT:
        case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT:
        case VK_FORMAT_A2B10G10R10_UINT_PACK32: integer = true; break;
        default: break;
    }
    return cache[f] = !integer && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) &&
                      (r32.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT);
}

bool compare_twin_wanted(const GpuImage& img) {
    if (!enabled() || img.aspect != VK_IMAGE_ASPECT_COLOR_BIT || img.depth > 1 || compares(img.format)) return false;
    return img.format == VK_FORMAT_R32_SFLOAT || blittable(img.format);
}

bool compare_twin_ready(const GpuImage& img) {
    auto it = twins().find(img.image);
    return it != twins().end() && it->second.image.image && it->second.image.width == img.width &&
           it->second.image.height == img.height && it->second.image.levels == img.levels &&
           it->second.image.layers == img.layers && it->second.copied_serial == serial_of(img);
}

VkImageView compare_twin_view(VkCommandBuffer cb, GpuImage& img, VkImageViewType type) {
    Twin& t = twins()[img.image];
    if (t.image.image && (t.image.width != img.width || t.image.height != img.height || t.image.levels != img.levels ||
                          t.image.layers != img.layers))
        destroy(t);  // the handle was reused for another image
    if (!t.image.image && !make(t, img)) {
        destroy(t);
        return VK_NULL_HANDLE;
    }
    if (t.copied_serial != serial_of(img)) copy(cb, t, img);
    VkImageView& v = t.views[type];
    if (!v) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = t.image.image;
        vi.viewType = type;
        vi.format = VK_FORMAT_D32_SFLOAT;
        uint32_t layers = type == VK_IMAGE_VIEW_TYPE_2D ? 1 : img.layers;
        vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, img.levels, 0, layers};
        if (p_vkCreateImageView(vk_device(), &vi, nullptr, &v) != VK_SUCCESS) v = VK_NULL_HANDLE;
    }
    return v;
}

}  // namespace nx::nvn
