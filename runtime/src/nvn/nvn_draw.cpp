// Draws (docs/NVN.md, N6): replays the bind, state and draw commands of nvn_state.cpp into Vulkan. Everything a draw
// reads from guest memory (vertices, indices, uniform and storage buffers, textures) is copied at replay time into
// the submit's upload buffer: the replay runs right when the game submits and is waited for, so the memory holds
// what the GPU would have read. Render targets become a dynamic-rendering pass, begun at the first draw and ended
// before any transfer.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <cstring>
#include <set>

#include "nvn_draw_state.h"
#include "nvn_ultrawide.h"
#include "platform.h"

namespace nx::nvn {

using namespace nx::vk;

// The replay state shared with nvn_draw_trace.cpp (nvn_draw_state.h).
namespace replay {

State g_state;
Stats g_stats;

const uint8_t* guest_bytes(uint64_t gpu) {
    uint64_t off = 0;
    MemoryPool* p = pool_at_gpu(gpu, &off);
    uint64_t host = p ? pool_host(*p, off) : 0;
    return host ? reinterpret_cast<const uint8_t*>(host) : nullptr;
}

}  // namespace replay

using namespace replay;

namespace {

bool g_rendering = false;
bool g_render_pass = false;
bool g_pass_placeholder_color = false;  // Intel depth-only compatibility target, if created for this pass
bool g_vertex_pending = false;  // a narrow pass-end barrier since the last one that covers vertex stages (G4)
uint64_t g_pass_serial = 0, g_vertex_visible_at = 0;  // pass ends so far; the serial a full barrier covered
uint64_t g_vertex_barriers = 0;

bool intel_placeholder_color_on() {
    static const bool on = [] {
        const char* e = std::getenv("NX_INTEL_PLACEHOLDER_COLOR");
        VkPhysicalDeviceProperties props{};
        p_vkGetPhysicalDeviceProperties(vk_physical(), &props);
        const bool enabled = e ? e[0] != '0' : props.vendorID == 0x8086;
        if (enabled)
            log("nvn/vk: Intel depth-only pass workaround enabled%s",
                e ? " (NX_INTEL_PLACEHOLDER_COLOR)" : " (Intel GPU default)");
        return enabled;
    }();
    return on;
}

struct IntelPlaceholderColor {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    uint64_t allocation_size = 0;
};

VkImageView intel_placeholder_color_view(VkCommandBuffer cb, uint32_t width, uint32_t height) {
    static IntelPlaceholderColor target;
    if (target.view && target.width >= width && target.height >= height) return target.view;
    width = std::max(width, target.width);
    height = std::max(height, target.height);

    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8_UNORM;
    ci.extent = {width, height, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image = VK_NULL_HANDLE;
    if (p_vkCreateImage(vk_device(), &ci, nullptr, &image) != VK_SUCCESS) {
        log("nvn/vk: Intel placeholder color image creation failed");
        return VK_NULL_HANDLE;
    }

    VkMemoryRequirements req{};
    p_vkGetImageMemoryRequirements(vk_device(), image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (p_vkAllocateMemory(vk_device(), &ai, nullptr, &memory) != VK_SUCCESS ||
        p_vkBindImageMemory(vk_device(), image, memory, 0) != VK_SUCCESS) {
        if (memory) p_vkFreeMemory(vk_device(), memory, nullptr);
        p_vkDestroyImage(vk_device(), image, nullptr);
        log("nvn/vk: Intel placeholder color memory allocation failed");
        return VK_NULL_HANDLE;
    }
    gpu_image_memory_allocated(req.size);

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    if (p_vkCreateImageView(vk_device(), &vi, nullptr, &view) != VK_SUCCESS) {
        p_vkFreeMemory(vk_device(), memory, nullptr);
        gpu_image_memory_freed(req.size);
        p_vkDestroyImage(vk_device(), image, nullptr);
        log("nvn/vk: Intel placeholder color view creation failed");
        return VK_NULL_HANDLE;
    }

    // A larger target replaces the previous one only after its earlier submissions and cached framebuffers retire.
    if (target.image) {
        p_vkDeviceWaitIdle(vk_device());
        forget_framebuffers(target.view);
        p_vkDestroyImageView(vk_device(), target.view, nullptr);
        p_vkDestroyImage(vk_device(), target.image, nullptr);
        p_vkFreeMemory(vk_device(), target.memory, nullptr);
        gpu_image_memory_freed(target.allocation_size);
    }
    target = {image, memory, view, width, height, req.size};

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                           0, nullptr, 0, nullptr, 1, &barrier);
    log("nvn/vk: Intel placeholder color target ready (%ux%u, R8_UNORM)", width, height);
    return view;
}

// Off by default (NX_PASS_BARRIER_NARROW=1 turns it on): the first S23 run with it showed a black band over the course
// background in 2 of 28 frames (phone_n6def, 15000 and 16200), a timing-dependent hazard of the DEBUG-2 #71 class.
bool narrow_pass_barrier() {
    static const bool on = std::getenv("NX_PASS_BARRIER_NARROW") != nullptr;
    return on;
}

// A vertex shader sampling a rendered image after a narrow pass-end barrier: the pass ends (it cannot take a barrier
// inside) and the writes so far are made visible to the vertex stages.
void vertex_read_barrier(VkCommandBuffer cb, const ShaderInfo* vs);  // the open pass is a render pass object (nvn_renderpass.cpp), not dynamic rendering
Targets g_pass;           // what the open pass renders to
GpuImage* g_pass_images[9];  // their images (8: depth), resolved once per pass for gather_textures
uint32_t g_pass_written = 0;  // targets the open pass's draws wrote: bit c colour c, bit 8 depth
uint64_t g_pass_ends[8];  // passes ended, by draw_end_rendering reason
uint64_t g_submit_ns = 0;  // when the submit being replayed started
VkExtent2D g_pass_extent{};

// What this submit's command buffer already has bound and set. Vulkan keeps the pipeline, vertex and index buffers
// and dynamic state of a command buffer across its dynamic-rendering passes, and most draws repeated what the draw
// before them set (the vertex streams and the viewport of a whole pass): only what differs is recorded. Forgotten
// at each submit, on another command buffer, and when something else binds a pipeline there (the masked clear's
// fullscreen draw, nvn_mask_clear.cpp). NX_DRAW_SLOW: forgotten before every draw.
struct Bound {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    uint32_t vertex_set = 0;  // bit s: binding s holds vertex[s]
    Upload vertex[16];
    bool has_index = false, has_dynamic = false;
    Upload index;
    VkIndexType index_type = VK_INDEX_TYPE_UINT16;
    VkViewport viewport{};
    VkRect2D scissor{};
    float blend[4] = {};
    uint32_t raster[10] = {};  // raster_dynamic's inputs: stencil reference and masks per face, the depth bias
    bool has_raster_depth = false;
    RasterDepthState raster_depth{};  // GPL dynamic mode (nvn_pipeline.h): the cull/depth/stencil state set
} g_bound;
uint64_t g_state_set = 0, g_state_kept = 0;  // state commands recorded and left out, for the log

template <typename T>
bool same_bytes(const T& a, const T& b) { return std::memcmp(&a, &b, sizeof(T)) == 0; }

void bind_vertex(VkCommandBuffer cb, uint32_t binding, const Upload& u) {
    Upload& now = g_bound.vertex[binding];
    if ((g_bound.vertex_set >> binding & 1) && now.buffer == u.buffer && now.offset == u.offset) {
        g_state_kept++;
        return;
    }
    VkDeviceSize off = u.offset;
    p_vkCmdBindVertexBuffers(cb, binding, 1, &u.buffer, &off);
    now = u, g_bound.vertex_set |= 1u << binding, g_state_set++;
}

// Viewport, scissor, blend constants and the rasterizer's dynamic state (every pipeline declares them dynamic).
void set_dynamic(VkCommandBuffer cb, const VkViewport& vp, const VkRect2D& sc) {
    Bound& b = g_bound;
    const State& s = g_state;
    uint32_t raster[10] = {s.stencil_ref[0],        s.stencil_ref[1],        s.stencil_compare_mask[0],
                          s.stencil_compare_mask[1], s.stencil_write_mask[0], s.stencil_write_mask[1]};
    std::memcpy(raster + 6, &s.bias_factor, 4), std::memcpy(raster + 7, &s.bias_units, 4);
    std::memcpy(raster + 8, &s.bias_clamp, 4);
    bool slack = s.depth.depth_test && equal_slack_pass(s.depth);
    raster[9] = slack;
    bool all = !b.has_dynamic;
    b.has_dynamic = true;
    auto differs = [&](bool d) { return (all || d) ? (g_state_set++, true) : (g_state_kept++, false); };
    if (differs(!same_bytes(b.viewport, vp))) p_vkCmdSetViewport(cb, 0, 1, &vp), b.viewport = vp;
    if (differs(!same_bytes(b.scissor, sc))) p_vkCmdSetScissor(cb, 0, 1, &sc), b.scissor = sc;
    if (differs(std::memcmp(b.blend, s.blend_color, sizeof b.blend) != 0))
        p_vkCmdSetBlendConstants(cb, s.blend_color), std::memcpy(b.blend, s.blend_color, sizeof b.blend);
    if (differs(std::memcmp(b.raster, raster, sizeof raster) != 0))  // stencil reference and masks, depth bias
        raster_dynamic(cb, slack), std::memcpy(b.raster, raster, sizeof raster);  // (nvn_raster.cpp)
}

// NX_TRACE_RESOLUTION=<first frame>: log each distinct colour/depth target and viewport size, up to 128 combinations.
// This shows whether Wonder changes its 3D scene resolution while the game runs without logging every draw.
void trace_render_resolution(const Targets& t) {
    static const char* env = std::getenv("NX_TRACE_RESOLUTION");
    static const uint64_t from = env ? std::strtoull(env, nullptr, 10) : 0;
    static std::set<std::array<int64_t, 10>> seen;
    static unsigned logged = 0;
    if (!env || g_frame.load(std::memory_order_relaxed) < from || logged >= 128) return;
    GpuImage* color = t.color_count && t.colors[0] ? image_of(t.colors[0]) : nullptr;
    GpuImage* depth = t.depth ? image_of(t.depth) : nullptr;
    uint32_t cl = color ? std::min(uint32_t(t.views.level[0]), color->levels - 1) : 0;
    uint32_t dl = depth ? std::min(uint32_t(t.views.level[8]), depth->levels - 1) : 0;
    uint32_t cw = color ? std::max(1u, color->width >> cl) : 0;
    uint32_t ch = color ? std::max(1u, color->height >> cl) : 0;
    uint32_t gw = color ? std::max(1u, color->logical_width >> cl) : 0;
    uint32_t gh = color ? std::max(1u, color->logical_height >> cl) : 0;
    uint32_t dw = depth ? std::max(1u, depth->width >> dl) : 0;
    uint32_t dh = depth ? std::max(1u, depth->height >> dl) : 0;
    std::array<int64_t, 10> key{cw, ch, dw, dh, g_state.viewport[0], g_state.viewport[1],
                               g_state.viewport[2], g_state.viewport[3], g_pass_extent.width, g_pass_extent.height};
    if (!seen.insert(key).second) return;
    logged++;
    log("resolution probe: frame %llu colour0=%llx guest %ux%u host %ux%u scale %.3f depth=%llx %ux%u pass %ux%u viewport %d,%d %dx%d",
        (unsigned long long)g_frame.load(std::memory_order_relaxed), (unsigned long long)(t.color_count ? t.colors[0] : 0), gw, gh, cw, ch,
        color ? color->render_scale : 1.0f, (unsigned long long)t.depth, dw, dh,
        g_pass_extent.width, g_pass_extent.height, g_state.viewport[0], g_state.viewport[1],
        g_state.viewport[2], g_state.viewport[3]);
}
void begin_rendering(VkCommandBuffer cb, const Targets& t) {
    if (clears_pending_other(t)) clears_flush(cb);  // other targets' clears first, in order
    // An image never written (layout UNDEFINED) has nothing to load: loadOp CLEAR to zero, not a LOAD of whatever
    // the memory held (G3, docs/nvn/DEBUG.md #50); a pending clear still wins (clears_apply). Env: bisecting.
    static const bool load_undefined = std::getenv("NX_UNDEFINED_LOAD") != nullptr;
    uint32_t fresh = 0;  // bit c colour c, bit 8 depth: begun cleared to 0 (depth 0, stencil 0)
    VkRenderingAttachmentInfo colors[8]{};
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkExtent2D extent{UINT32_MAX, UINT32_MAX};
    VkFormat formats[8]{};  // the views' (render pass objects)
    g_pass_placeholder_color = false;
    for (uint32_t k = 0; k < t.color_count; k++) {
        colors[k].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        GpuImage* img = t.colors[k] ? image_of(t.colors[k]) : nullptr;
        g_pass_images[k] = img;
        if (!img) continue;
        if (img->layout == VK_IMAGE_LAYOUT_UNDEFINED && !load_undefined) fresh |= 1u << k;
        transition(cb, *img, VK_IMAGE_LAYOUT_GENERAL);  // rendered images stay GENERAL (sampled while rendered)
        img->rendered = true;
        uint32_t level = std::min<uint32_t>(t.views.level[k], img->levels - 1);
        formats[k] = view_format(*img, t.views.format[k]);
        colors[k].imageView = attachment_view(*img, level, t.views.layer[k], formats[k]);
        // NX_TRACE_CUBES=<first frame>: writes into cube images from that frame on. With 400 from the start, the log
        // ran out at the first 3D scene (frame ~171) and never reached a course load.
        static const char* trace_cubes = std::getenv("NX_TRACE_CUBES");
        static const uint64_t cube_from = trace_cubes ? std::strtoull(trace_cubes, nullptr, 10) : 0;
        static int cube_logs = 0;
        if (trace_cubes && (img->view_type == VK_IMAGE_VIEW_TYPE_CUBE || img->view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) &&
            g_frame >= cube_from && cube_logs++ < 2000)
            log("cubes: render into %llx level %u layer %u (%ux%u, %u levels, %u layers) view %s",
                (unsigned long long)t.colors[k], level, t.views.layer[k], img->width, img->height, img->levels,
                img->layers, colors[k].imageView == img->view ? "WHOLE IMAGE (fallback)" : "ok");
        colors[k].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        colors[k].loadOp = fresh >> k & 1 ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        colors[k].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        extent = {std::min(extent.width, std::max(1u, img->width >> level)),
                  std::min(extent.height, std::max(1u, img->height >> level))};
    }
    GpuImage* dimg = t.depth ? image_of(t.depth) : nullptr;
    g_pass_images[8] = dimg;
    if (dimg && dimg->aspect != VK_IMAGE_ASPECT_COLOR_BIT) {
        if (dimg->layout == VK_IMAGE_LAYOUT_UNDEFINED && !load_undefined) fresh |= 1u << 8;
        transition(cb, *dimg, VK_IMAGE_LAYOUT_GENERAL);
        dimg->rendered = true;
        uint32_t level = std::min<uint32_t>(t.views.level[8], dimg->levels - 1);
        depth.imageView = attachment_view(*dimg, level, t.views.layer[8]);
        depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        depth.loadOp = fresh >> 8 & 1 ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.clearValue.depthStencil = {1.0f, 0};  // far (a fresh depth buffer; the course passes test LESS)
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        extent = {std::min(extent.width, std::max(1u, dimg->width >> level)),
                  std::min(extent.height, std::max(1u, dimg->height >> level))};
    } else {
        dimg = nullptr;
    }
    if (extent.width == UINT32_MAX) extent = {1, 1};
    uint32_t color_count = t.color_count;
    if (!color_count && dimg && intel_placeholder_color_on()) {
        VkImageView view = intel_placeholder_color_view(cb, extent.width, extent.height);
        if (view) {
            colors[0] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            colors[0].imageView = view;
            colors[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            colors[0].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            colors[0].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            formats[0] = VK_FORMAT_R8_UNORM;
            color_count = 1;
            g_pass_placeholder_color = true;
        }
    }
    VkRenderingAttachmentInfo stencil = depth;  // same view, its own loadOp
    uint32_t cleared = clears_apply(t, colors, depth, stencil);  // pending clears of these targets: loadOp CLEAR
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = color_count;
    ri.pColorAttachments = color_count ? colors : nullptr;
    if (dimg) {
        if (dimg->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ri.pDepthAttachment = &depth;
        if (dimg->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ri.pStencilAttachment = &stencil;
    }
    g_render_pass = render_pass_objects();
    if (g_render_pass)
        begin_render_pass(cb, color_count, formats, colors, dimg ? dimg->format : VK_FORMAT_UNDEFINED, depth, stencil,
                          extent);
    else
        p_vkCmdBeginRendering(cb, &ri);
    g_rendering = true;
    g_pass_written = cleared | fresh;  // as a clear inside the pass: sampling it needs it stored first
    trace_pass("begin: cleared, fresh", t, cleared, fresh);
    g_pass = t;
    g_pass_extent = extent;
}

uint64_t g_dump_draws = 0, g_dump_request = 0;

// DispatchCompute: the bound program's compute stage over (x, y, z) groups, outside render passes, then a barrier
// so what it wrote is visible to what follows.
void dispatch(VkCommandBuffer cb, const Cmd& c) {
    static const bool off = std::getenv("NX_NO_COMPUTE") != nullptr;  // debug: skip compute dispatches
    if (off) return;
    const auto* shaders = program_shaders(g_state.program);
    const ShaderInfo* cs = nullptr;
    if (shaders)
        for (const ShaderInfo* s : *shaders)
            if (s->stage == ShaderStage::Compute) cs = s;
    if (!cs) return;
    uint32_t x = uint32_t(c.i[0]), y = uint32_t(c.i[1]), z = uint32_t(c.i[2]);
    const uint8_t* args = c.n == 1 ? guest_bytes(c.a[0]) : nullptr;  // indirect: {x, y, z}
    if (c.n == 1 && !args) return;
    // Written by an earlier dispatch of this submit: only the GPU knows them (vkCmdDispatchIndirect, below).
    bool gpu_args = args && !written_separate() && written_overlaps(args, 12);
    if (args && !gpu_args) std::memcpy(&x, args, 4), std::memcpy(&y, args + 4, 4), std::memcpy(&z, args + 8, 4);
    if (!gpu_args && (!x || !y || !z)) return;
    trace_compute_once(cs, x, y, z);  // 0, 0, 0 for GPU arguments
    trace_nan_dispatch(cs);
    std::vector<const ShaderInfo*> stages{cs};
    const ProgramLayout& layout = program_layout(stages);
    VkPipeline pipeline = compute_pipeline(cs, layout);
    if (!pipeline) return;
    draw_end_rendering(cb, 2);
    gather_textures(stages);
    build_sets(cb, stages, layout);
    p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    bind_sets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout);
    if (gpu_profile_on()) gpu_profile_mark(cb, "compute " + cs->hash);
    if (!gpu_args || !written_indirect(cb, args)) p_vkCmdDispatch(cb, x, y, z);
    if (gpu_profile_on()) gpu_profile_mark(cb, "outside passes (clears, uploads, copies)");
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                           nullptr, 0, nullptr);
    g_stats.dispatches++;
}

void vertex_read_barrier(VkCommandBuffer cb, const ShaderInfo* vs) {
    if (!g_vertex_pending || !vs) return;
    bool reads = false;
    for (size_t k = 0; k < g_textures.bindings.size() && !reads; k++)
        if (g_textures.bindings[k].first == vs && k < g_textures.resolved.size() && g_textures.resolved[k].texture) {
            GpuImage* img = g_textures.resolved[k].image ? g_textures.resolved[k].image
                                                         : image_of(g_textures.resolved[k].texture);
            if (img) reads = img->rendered && img->rendered_at > g_vertex_visible_at;
        }
    if (!reads) return;
    draw_end_rendering(cb, 1);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                       VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 1, &mb, 0,
                           nullptr, 0, nullptr);
    g_vertex_pending = false, g_vertex_barriers++, g_vertex_visible_at = g_pass_serial;
}

bool draw(VkCommandBuffer cb, const Targets& t, uint32_t mode, bool indexed, const Cmd& c) {
    struct AspectFlags {
        uint64_t frame = UINT64_MAX;
        bool flower = false, card = false, align = false, detail = false, markers = false, settings = false, alt = false;
    };
    static AspectFlags flags;
    const uint64_t flag_frame = g_frame.load(std::memory_order_relaxed);
    if (flags.frame != flag_frame) {
        auto enabled = [](const char* name) {
            const char* value = std::getenv(name);
            return value && std::strcmp(value, "1") == 0;
        };
        flags.flower = enabled("NX_UW_FLOWER_TEST");
        flags.card = enabled("NX_UW_MAP_CARD_TEST");
        flags.align = enabled("NX_UW_MAP_ALIGN_TEST");
        flags.detail = enabled("NX_UW_MAP_DETAIL_TEST");
        flags.markers = enabled("NX_UW_MAP_MARKERS_TEST");
        flags.settings = enabled("NX_UW_SETTINGS_SCISSOR_TEST");
        flags.alt = enabled("NX_UW_FLOWER_ALT_TEST");
        flags.frame = flag_frame;
    }
    g_stats.total++;
    if (g_state.rasterizer_discard || !g_state.render_enable) return false;  // nothing reaches the targets
    const auto* shaders = program_shaders(g_state.program);
    if (!shaders || shaders->empty()) {
        g_stats.no_program++;
        return false;
    }
    const ShaderInfo* vs = nullptr;
    const ShaderInfo* fs = nullptr;
    for (const ShaderInfo* s : *shaders) {
        if (s->stage == ShaderStage::Vertex) vs = s;
        if (s->stage == ShaderStage::Fragment) fs = s;
    }
    if (!vs) {
        g_stats.no_vertex++;
        return false;
    }
    std::vector<const ShaderInfo*> swapped;  // NX_SWAP_FS / NX_SKIP_FS (nvn_draw_trace.cpp)
    if (!debug_shader_overrides(shaders, fs, swapped)) return false;
    const ProgramLayout& layout = program_layout(*shaders);

    // NX_SNAP_FS_BEFORE=<hash>[@<first draw>]: snapshot colour target 0 before draws with that fragment shader
    // (what a feedback pass like a tonemap reads), 8 at most.
    {
        static const std::string before = std::getenv("NX_SNAP_FS_BEFORE") ? std::getenv("NX_SNAP_FS_BEFORE") : "";
        static const uint64_t from = before.find('@') != std::string::npos
                                         ? std::strtoull(before.c_str() + before.find('@') + 1, nullptr, 10) : 0;
        static int taken = 0;
        bool match = (fs && before.compare(0, 16, fs->hash) == 0) || before.compare(0, 16, vs->hash) == 0;
        static const char* before_trigger = std::getenv("NX_SNAP_BEFORE_TRIGGER_FILE");
        static bool before_triggered = !before_trigger || !*before_trigger;
        if (!before_triggered && match)
            if (FILE* marker = std::fopen(before_trigger, "rb")) { std::fclose(marker); before_triggered = true; }
        static const int before_limit = [] { const char* e = std::getenv("NX_SNAP_BEFORE_LIMIT"); return e ? std::atoi(e) : 8; }();
        if (before_triggered && !before.empty() && match && g_stats.total >= from && taken < before_limit &&
            t.color_count && t.colors[0])
            if (GpuImage* img = image_of(t.colors[0])) {
                taken++;
                log("map before snapshot: total=%llu done=%llu", (unsigned long long)g_stats.total, (unsigned long long)g_stats.done);
                draw_end_rendering(cb, 6);
                debug_snapshot(cb, *img, 0xB0000000ull + g_stats.total);
            }
    }
    // Textures first: an upload or a layout change cannot happen inside the render pass.
    uint32_t gathered = gather_textures(*shaders);
    // A pass that neither tests nor writes depth but keeps its depth image bound while sampling that same image (the
    // world map's depth-to-colour linearization, fs 0aa56d1b): on Metal the image is then attached and read in one
    // encoder, which is undefined, and the copy came out 0 everywhere; the composite then took the player for
    // occluded (a flat silhouette, issue #50; DEBUG-2.md #67). The depth is unused: the pass runs without it.
    // NX_KEEP_SAMPLED_DEPTH=1: as before.
    Targets detached;
    const Targets& tt = [&]() -> const Targets& {
        static const bool keep = std::getenv("NX_KEEP_SAMPLED_DEPTH") != nullptr;
        const DepthStencilInfo& d = g_state.depth;
        if (keep || !t.depth || d.depth_test || d.depth_write || d.stencil_test) return t;
        GpuImage* dimg = image_of(t.depth);
        if (!dimg) return t;
        for (const ResolvedTexture& r : g_textures.resolved)
            if (r.texture && (r.image ? r.image : image_of(r.texture)) == dimg) {
                static uint64_t n = 0;
                if (n++ == 0) log("nvn: a pass samples its own unused depth image: drawn without it (#50)");
                detached = t, detached.depth = 0;
                return detached;
            }
        return t;
    }();
    GpuImage* primary_target = tt.color_count && tt.colors[0] ? image_of(tt.colors[0])
                                                             : (tt.depth ? image_of(tt.depth) : nullptr);
    g_uw_wide = primary_target && primary_target->uw;
    const float target_scale = primary_target ? primary_target->render_scale : 1.0f;
    vertex_read_barrier(cb, vs);  // G4
    bool need_break = gathered & kNeedBreak;
    if (gathered & kSampledPending) clears_flush(cb);  // cleared (and stored) before it is sampled
    static const bool bytes = std::getenv("NX_TARGETS_MEMCMP") != nullptr;  // bisecting
    bool targets_changed = !g_rendering || (bytes ? std::memcmp(&g_pass, &tt, sizeof tt) != 0 : !targets_same(g_pass, tt));
    if (g_rendering && (need_break || targets_changed)) {
        (targets_changed ? g_stats.breaks_targets : g_stats.breaks_textures)++;
        draw_end_rendering(cb, 1);
    }

    build_sets(cb, *shaders, layout, target_scale);
    // Vertex and index data before the pass begins: a range a dispatch of this submit wrote may need a GPU copy
    // (nvn_written.cpp), which cannot be recorded inside a pass.
    DrawInputs in;
    if (!draw_inputs_shared(cb, vs, indexed, c, in)) {
        g_stats.no_indices++;
        return false;
    }
    check_vertex_ranges(vs, fs, indexed, c, in);  // NX_CHECK_VERTS (nvn_written.cpp)
    if (!g_rendering) {
        if (gpu_profile_on()) {
            GpuImage* img = t.color_count && t.colors[0] ? image_of(t.colors[0]) : nullptr;
            char label[128];
            std::snprintf(label, sizeof label, "%ux%u L%u fs %s", img ? img->width : 0, img ? img->height : 0,
                          t.views.level[0], fs ? fs->hash.c_str() : "-");
            gpu_profile_mark(cb, label);
        }
        begin_rendering(cb, tt);
    }

    // Pipeline.
    PipelineKey key;
    std::memset(&key, 0, sizeof key);  // the key is compared byte for byte
    key.vs = vs, key.fs = fs;
    static const bool cam_patch = !(std::getenv("NX_UW_CAMERA") && std::getenv("NX_UW_CAMERA")[0] == '0');
    if (uw_on() && cam_patch && !g_uw_wide && t.color_count && t.colors[0]) {
        const Texture* layer = get<Texture>(t.colors[0]);
        if (layer && layer->format == 0x25 && (layer->flags & 8)) uw_ui_vertex_shader(vs);
    }
    static const bool ui_learn = std::getenv("NX_UW_UI") && std::string(std::getenv("NX_UW_UI")) == "learn";
    const bool ui_pick = !cam_patch || (ui_learn ? uw_is_ui_vertex_shader(vs) : !g_state.depth.depth_test);
    bool anchor_hud_left = false;
    float flower_offset = 0.0f;
    static uint64_t flower_frame = UINT64_MAX;
    static float flower_x = 0.0f, flower_y = 0.0f, flower_proj_x = 1.0f, flower_proj_y = 1.0f;
    static bool flower_seen = false;
    static float flower_direction = 1.0f;
    if (flags.flower && g_uw_wide && !g_state.depth.depth_test && vs && fs && t.color_count && t.colors[0]) {
        uint64_t frame = g_frame.load();
        if (frame != flower_frame) { flower_frame = frame; flower_seen = false; }
        GpuImage* target = primary_target;
        const Range& u = g_state.uniforms[0][0];
        const float* f = u.address && u.size >= 96 ? reinterpret_cast<const float*>(guest_bytes(u.address)) : nullptr;
        if (target && target->logical_width == 1920 && target->logical_height == 1080 && f) {
            static unsigned flower_logs = 0;
            const char* flower_trace = std::getenv("NX_UW_FLOWER_TRACE");
            FILE* marker = flower_trace && flower_logs < 120 ? std::fopen(flower_trace, "rb") : nullptr;
            if (marker) {
                std::fclose(marker);
                if ((vs->hash == "42ea61385aa6a70c" && fs->hash == "5bbd5392666b3406") ||
                    vs->hash == "2fc962d8d063c1c9" || vs->hash == "a106c763cf05f818") {
                    ++flower_logs;
                    std::string textures;
                    for (const ResolvedTexture& r : g_textures.resolved) {
                        GpuImage* img = r.image;
                        Texture* src = r.object;
                        if (!img || !src) continue;
                        char item[64]; std::snprintf(item, sizeof item, "%ux%u/f%x ", img->logical_width, img->logical_height, src->format);
                        textures += item;
                    }
                    log("flower trace: frame=%llu vs=%s fs=%s mat=%g,%g model=%g,%g ref=%u,%g,%g src=%s", (unsigned long long)frame,
                        vs->hash.c_str(), fs->hash.c_str(), f[3], f[7], f[19], f[23], unsigned(flower_seen), flower_x, flower_y, textures.c_str());
                }
            }
            bool bubble = false;
            if (vs->hash == "42ea61385aa6a70c" && fs->hash == "5bbd5392666b3406") {
                for (const ResolvedTexture& r : g_textures.resolved) {
                    GpuImage* img = r.image;
                    Texture* src = r.object;
                    if (img && src && ((src->format == 0x49 &&
                        ((img->logical_width == 48 && img->logical_height == 96) ||
                         (img->logical_width == 32 && img->logical_height == 64))) ||
                        (src->format == 0x87 && img->logical_width == 48 &&
                         (img->logical_height == 72 || img->logical_height == 104)))) bubble = true;
                }
                if (bubble) {
                    flower_x = f[19]; flower_y = f[23]; flower_proj_x = f[0]; flower_proj_y = f[5]; flower_seen = true;
                    flower_direction = flags.alt && std::fabs(f[16] - 0.95f) < 0.005f ? 2.0f : 1.0f;
                    flower_offset = (flower_x - (flower_direction == 2.0f ? 384.0f : 0.0f)) * (uw_factor() - 1.0f);
                }
                else if (flower_seen && std::fabs(f[19] - flower_x) < 160.0f && std::fabs(f[23] - flower_y) < 100.0f)
                    flower_offset = (flower_x - (flower_direction == 2.0f ? 384.0f : 0.0f)) * (uw_factor() - 1.0f);
            }
            if (flower_seen && (vs->hash == "2fc962d8d063c1c9" || vs->hash == "a106c763cf05f818") &&
                (fs->hash == "46841c209865a1c0" || fs->hash == "49add6de02fe8180") &&
                std::fabs(f[3] - flower_x * flower_proj_x) < 32.0f && std::fabs(f[7] - flower_y * flower_proj_y) < 100.0f)
                flower_offset = (flower_x - (flower_direction == 2.0f ? 384.0f : 0.0f)) * (uw_factor() - 1.0f);
            for (const ResolvedTexture& r : g_textures.resolved) {
                if (r.image && r.object && r.image->logical_width == 1920 && r.image->logical_height == 1080) {
                    flower_seen = false;
                    break;
                }
            }
        }
    }
    // World-map course cards use world-projected positions but are drawn in the
    // centered 16:9 UI layer. Track their distinctive frame texture, then move
    // nearby card components together by the same horizontal world offset.
    if (flags.card && g_uw_wide &&
        !g_state.depth.depth_test && vs && fs && t.color_count && t.colors[0]) {
        static uint64_t card_frame = UINT64_MAX;
        static float card_x = 0.0f, card_y = 0.0f;
        static bool card_seen = false;
        const uint64_t frame = g_frame.load();
        if (frame != card_frame) { card_frame = frame; card_seen = false; }
        GpuImage* target = primary_target;
        const Range& u = g_state.uniforms[0][0];
        const float* f = u.address && u.size >= 96 ? reinterpret_cast<const float*>(guest_bytes(u.address)) : nullptr;
        if (target && target->logical_width == 1920 && target->logical_height == 1080 && f) {
            bool card_frame_texture = false;
            for (const ResolvedTexture& r : g_textures.resolved) {
                GpuImage* img = r.image;
                Texture* src = r.object;
                if (img && src && img->logical_width == 88 && img->logical_height == 276 && src->format == 0x49)
                    card_frame_texture = true;
            }
            if (card_frame_texture && vs->hash == "5587b5f73977f07e" && fs->hash == "e4ff94e7165656b5") {
                card_x = f[19]; card_y = f[23]; card_seen = true;
            }
            if (card_seen && std::fabs(f[19] - card_x) < 8.0f && std::fabs(f[23] - card_y) < 100.0f)
                flower_offset = card_x * (uw_factor() - 1.0f);
            // Extra pieces of the expanded card use their own transforms. Keep
            // their local spacing and apply the same anchor offset as the card.
            if (flags.detail && card_seen) {
                bool detail_texture = false;
                for (const ResolvedTexture& r : g_textures.resolved) {
                    GpuImage* img = r.image;
                    Texture* src = r.object;
                    if (!img || !src) continue;
                    detail_texture |= (src->format == 0x49 && img->logical_width == 150 && img->logical_height == 150) ||
                                      (src->format == 0x87 && img->logical_width == 240 && img->logical_height == 240);
                }
                const bool detail_shape = vs->hash == "42ea61385aa6a70c" && fs->hash == "5bbd5392666b3406" &&
                    std::fabs(f[19] - card_x - 158.0f) < 5.0f && std::fabs(f[23] - card_y + 81.0f) < 10.0f;
                if ((detail_texture || detail_shape) && std::fabs(f[19] - card_x) < 240.0f &&
                    std::fabs(f[23] - card_y) < 120.0f)
                    flower_offset = card_x * (uw_factor() - 1.0f);
                if ((vs->hash == "2fc962d8d063c1c9" || vs->hash == "a106c763cf05f818") &&
                    (fs->hash == "46841c209865a1c0" || fs->hash == "49add6de02fe8180") &&
                    std::fabs(f[3] - card_x * 1.00833f) < 300.0f &&
                    std::fabs(f[7] - card_y * 1.79259f) < 250.0f)
                    flower_offset = card_x * (uw_factor() - 1.0f);
            }
            if (flags.markers) {
                static uint64_t msg_frame = UINT64_MAX;
                static float msg_x = 0.0f, msg_y = 0.0f, msg_proj_x = 1.0f;
                static bool msg_seen = false;
                if (frame != msg_frame) { msg_frame = frame; msg_seen = false; }
                bool marker_icon = false, message_frame = false, seed_counter = false;
                for (const ResolvedTexture& r : g_textures.resolved) {
                    GpuImage* img = r.image;
                    Texture* src = r.object;
                    if (!img || !src) continue;
                    marker_icon |= (src->format == 0x87 &&
                                   ((img->logical_width == 160 && img->logical_height == 160) ||
                                    (img->logical_width == 48 && img->logical_height == 48))) ||
                                   (src->format == 0x49 && img->logical_width == 256 && img->logical_height == 256);
                    message_frame |= src->format == 0x49 && img->logical_width == 48 && img->logical_height == 112;
                    seed_counter |= src->format == 0x25 && img->logical_width == 200 && img->logical_height == 130;
                }
                if (card_seen && marker_icon && std::fabs(f[19] - card_x) < 200.0f &&
                    std::fabs(f[23] - card_y) < 130.0f &&
                    (vs->hash == "42ea61385aa6a70c" || vs->hash == "6a4179388f4b1b2e"))
                    flower_offset = card_x * (uw_factor() - 1.0f);
                if (seed_counter && vs->hash == "42ea61385aa6a70c" && fs->hash == "5bbd5392666b3406")
                    flower_offset = f[19] * (uw_factor() - 1.0f);
                if (message_frame && std::fabs(f[16] - 0.8f) < 0.01f && vs->hash == "5587b5f73977f07e" && fs->hash == "e4ff94e7165656b5") {
                    msg_x = f[19]; msg_y = f[23]; msg_proj_x = f[0]; msg_seen = true;
                    flower_offset = msg_x * (uw_factor() - 1.0f);
                } else if (msg_seen && vs->hash == "9e68098b1e77c2fa" &&
                           std::fabs(f[23] - msg_y) < 100.0f && std::fabs(f[19] - msg_x) < 200.0f) {
                    flower_offset = msg_x * (uw_factor() - 1.0f);
                } else if (msg_seen && vs->hash == "2fc962d8d063c1c9" &&
                           std::fabs(f[3] - msg_x * msg_proj_x) < 24.0f) {
                    flower_offset = msg_x * (uw_factor() - 1.0f);
                } else if (msg_seen && vs->hash == "d0c055be04d18268" && fs->hash == "8886bf302ad2c2aa" &&
                           std::fabs(f[7] - msg_x * msg_proj_x) < 24.0f) {
                    flower_offset = msg_x * (uw_factor() - 1.0f);
                }
            }
            bool menu_blur = false;
            for (const ResolvedTexture& r : g_textures.resolved) {
                GpuImage* img = r.image;
                Texture* src = r.object;
                if (img && src && img->logical_width == 1920 && img->logical_height == 1080) menu_blur = true;
            }
            if (menu_blur) {
                card_seen = false;
                flower_seen = false;
            }
        }
    }
    if (g_uw_wide && !g_state.depth.depth_test && vs && fs && t.color_count && t.colors[0]) {
        GpuImage* target = primary_target;
        const Range& u = g_state.uniforms[0][0];
        const float* f = u.address && u.size >= 96 ? reinterpret_cast<const float*>(guest_bytes(u.address)) : nullptr;
        if (target && target->logical_width == 1920 && target->logical_height == 1080 && f) {
            // Online world-projected player nameplates (360x60/f25) and friend-race bubbles.
            bool online_nameplate = false, online_bubble_mask = false, online_bubble_body = false;
            for (const ResolvedTexture& r : g_textures.resolved) {
                GpuImage* img = r.image;
                Texture* src = r.object;
                if (!img || !src) continue;
                if (src->format == 0x25 && img->logical_width == 360 && img->logical_height == 60)
                    online_nameplate = true;
                if (src->format == 0x49 && img->logical_width == 150 && img->logical_height == 150) {
                    for (const ResolvedTexture& r2 : g_textures.resolved) {
                        if (!r2.image || !r2.object) continue;
                        if (r2.object->format == 0x87 && r2.image->logical_width == 1920 &&
                            r2.image->logical_height == 2)
                            online_bubble_mask = true;
                        if (r2.object->format == 0x87 && r2.image->logical_width == 320 &&
                            r2.image->logical_height == 320)
                            online_bubble_body = true;
                    }
                }
            }
            if ((online_nameplate &&
                 ((vs->hash == "5587b5f73977f07e" && fs->hash == "e4ff94e7165656b5") ||
                  (vs->hash == "42ea61385aa6a70c" && fs->hash == "5bbd5392666b3406"))) ||
                (online_bubble_mask &&
                 ((vs->hash == "1b11e4e08801f8aa" && fs->hash == "708acb7a19fccd21") ||
                  (vs->hash == "beb4ee08b0251d69" && fs->hash == "d37ad47acbe02764"))) ||
                (online_bubble_body &&
                 ((vs->hash == "6a4179388f4b1b2e" && fs->hash == "1642a92d1f3d654a") ||
                  (vs->hash == "e66384ab092a9d27" && fs->hash == "32686281e07a9149")))) {
                flower_offset = f[19] * (uw_factor() - 1.0f);
            }
        }
    }
    if (g_uw_wide && ui_pick && uw_vs_scaled(vs)) {
        bool samples_wide = false;
        bool samples_hud_layer = false;
        bool samples_powerup_candidate = false;
        bool samples_powerup_icon = false;
        bool samples_stock_shadow = false;
        const uint32_t count = indexed ? uint32_t(c.i[0]) : uint32_t(c.i[1]);
        if (count <= 6) for (const ResolvedTexture& r : g_textures.resolved) {
            GpuImage* sampled = r.image;
            samples_wide |= sampled && sampled->uw;
            // Wonder composites its HUD from this fixed-size layer. It is screen-anchored at 16:9
            // coordinates, so the usual centered safe-area squeeze moves it away from the real edge.
            Texture* source = r.object;
            samples_hud_layer |= sampled && source && sampled->logical_width == 430 && sampled->logical_height == 180 &&
                                 source->format == 0x25 && (source->flags & 0x8);
            samples_powerup_candidate |= sampled && source && sampled->logical_width == 112 &&
                                         sampled->logical_height == 112 && source->format == 0x49;
            samples_powerup_icon |= sampled && source && sampled->logical_width == 80 &&
                                    sampled->logical_height == 80 && source->format == 0x87;
            samples_stock_shadow |= sampled && source && sampled->logical_width == 29 &&
                                    sampled->logical_height == 29 && source->format == 0x01;
        }
        if (!samples_wide) {
            key.uw_x = 1.0f / uw_factor();
            static const bool powerup_test = std::getenv("NX_UW_POWERUP_TEST") &&
                                             std::string(std::getenv("NX_UW_POWERUP_TEST")) == "1";
            bool stock_content = false;
            if (powerup_test && vs && fs && t.color_count && t.colors[0]) {
                GpuImage* target = primary_target;
                const Range& u = g_state.uniforms[0][0];
                const float* f = u.address && u.size >= 96 ? reinterpret_cast<const float*>(guest_bytes(u.address)) : nullptr;
                if (target && target->logical_width == 1920 && target->logical_height == 1080 && f) {
                    // The stock icon and key label have distinct shaders; their transform positions identify the
                    // fixed stock slot without moving unrelated 80x80 pictures or text in other UI layouts.
                    if (samples_powerup_icon && vs->hash == "42ea61385aa6a70c" && fs->hash == "5bbd5392666b3406")
                        stock_content = std::fabs(f[19] + 864.0f) < 128.0f && std::fabs(f[23] - 223.0f) < 128.0f;
                    if ((vs->hash == "a106c763cf05f818" || vs->hash == "2fc962d8d063c1c9") &&
                        (fs->hash == "46841c209865a1c0" || fs->hash == "49add6de02fe8180"))
                        stock_content = std::fabs(f[3] + 836.412f) < 128.0f && std::fabs(f[7] - 413.192f) < 128.0f;
                }
            }
            if (samples_hud_layer || (powerup_test && vs && fs &&
                ((samples_powerup_candidate &&
                  ((vs->hash == "42ea61385aa6a70c" && fs->hash == "5bbd5392666b3406") ||
                   (vs->hash == "6a4179388f4b1b2e" && fs->hash == "846e8143e854b162"))) ||
                 (samples_stock_shadow && vs->hash == "d0c055be04d18268" && fs->hash == "8886bf302ad2c2aa"))) || stock_content) {
                anchor_hud_left = true;
            }
        }
    }
    static const bool uw_trace = std::getenv("NX_UW_TRACE") && std::string(std::getenv("NX_UW_TRACE")) == "1";
    static const bool stock_trace = std::getenv("NX_UW_STOCK_TRACE") && std::string(std::getenv("NX_UW_STOCK_TRACE")) == "1";
    if (stock_trace && g_uw_wide && !g_state.depth.depth_test && vs && fs && t.color_count && t.colors[0]) {
        const Range& u = g_state.uniforms[0][0];
        const float* f = u.address && u.size >= 96 ? reinterpret_cast<const float*>(guest_bytes(u.address)) : nullptr;
        GpuImage* target = image_of(t.colors[0]);
        if (f && target && target->logical_width == 1920 && target->logical_height == 1080 &&
            ((std::fabs(f[3] + 836.412f) < 180.0f && std::fabs(f[7] - 413.192f) < 180.0f) ||
             (std::fabs(f[19] + 864.0f) < 180.0f && std::fabs(f[23] - 223.0f) < 180.0f))) {
            std::string src;
            for (const ResolvedTexture& r : g_textures.resolved) {
                GpuImage* img = r.texture ? image_of(r.texture) : nullptr;
                Texture* tex = r.texture ? get<Texture>(r.texture) : nullptr;
                if (!img || !tex) continue;
                char b[64]; std::snprintf(b, sizeof b, "%ux%u/f%x ", img->logical_width, img->logical_height, tex->format);
                src += b;
            }
            const std::string sig = vs->hash + ":" + fs->hash + ":" + src;
            static std::set<std::string> seen_stock;
            if (seen_stock.size() < 300 && seen_stock.insert(sig).second)
                log("nvn: stock trace: frame=%llu vs=%s fs=%s pos=%g,%g model=%g,%g sources=%s",
                    (unsigned long long)g_frame.load(), vs->hash.c_str(), fs->hash.c_str(),
                    f[3], f[7], f[19], f[23], src.c_str());
        }
    }
    static const bool powerup_trace = std::getenv("NX_UW_POWERUP_TRACE") &&
                                      std::string(std::getenv("NX_UW_POWERUP_TRACE")) == "1";
    if (powerup_trace && g_uw_wide && !g_state.depth.depth_test && vs && fs && t.color_count && t.colors[0]) {
        GpuImage* target = image_of(t.colors[0]);
        if (target && target->logical_width == 1920 && target->logical_height == 1080) {
            std::string shapes;
            for (const ResolvedTexture& r : g_textures.resolved) {
                GpuImage* img = r.texture ? image_of(r.texture) : nullptr;
                Texture* tex = r.texture ? get<Texture>(r.texture) : nullptr;
                if (!img || !tex) continue;
                char b[64];
                std::snprintf(b, sizeof b, "%ux%u/f%x%s ", img->logical_width, img->logical_height,
                              tex->format, img->uw ? "W" : "");
                shapes += b;
            }
            const std::string signature = vs->hash + ":" + fs->hash + ":" + shapes;
            static std::set<std::string> seen_powerup;
            if (seen_powerup.size() < 2000 && seen_powerup.insert(signature).second)
                log("nvn: UW powerup trace: frame=%llu vs=%s fs=%s count=%u sources=%s",
                    (unsigned long long)g_frame.load(), vs->hash.c_str(), fs->hash.c_str(),
                    indexed ? unsigned(c.i[0]) : unsigned(c.i[1]), shapes.c_str());
        }
    }
    static const bool marker_trace = std::getenv("NX_UW_MARKER_TRACE") &&
                                       std::string(std::getenv("NX_UW_MARKER_TRACE")) == "1";
    if (marker_trace && g_uw_wide && !g_state.depth.depth_test && vs && fs && t.color_count && t.colors[0] &&
        flower_offset == 0.0f && !anchor_hud_left && g_frame.load() > 1500) {
        GpuImage* target = image_of(t.colors[0]);
        const Range& u = g_state.uniforms[0][0];
        const float* f = u.address && u.size >= 96 ? reinterpret_cast<const float*>(guest_bytes(u.address)) : nullptr;
        if (target && target->logical_width == 1920 && target->logical_height == 1080 && f &&
            (std::fabs(f[19]) > 20.0f || std::fabs(f[3]) > 20.0f)) {
            std::string src;
            for (const ResolvedTexture& r : g_textures.resolved) {
                GpuImage* img = r.image;
                Texture* tex = r.object;
                if (!img || !tex) continue;
                char b[64];
                std::snprintf(b, sizeof b, "%ux%u/f%x ", img->logical_width, img->logical_height, tex->format);
                src += b;
            }
            const std::string sig = vs->hash + ":" + fs->hash + ":" + src;
            static std::set<std::string> seen_marker;
            if (seen_marker.size() < 150 && seen_marker.insert(sig).second) {
                log("uw_marker_trace: frame=%llu vs=%s fs=%s mat=%g,%g model=%g,%g proj=%g,%g f16=%g src=%s",
                    (unsigned long long)g_frame.load(), vs->hash.c_str(), fs->hash.c_str(), f[3], f[7],
                    f[19], f[23], f[0], f[5], f[16], src.c_str());
            }
        }
    }
    if (uw_trace && t.color_count && t.colors[0]) {
        Texture* dst = get<Texture>(t.colors[0]);
        GpuImage* image = image_of(t.colors[0]);
        if (dst && image && image->logical_width >= 640 && image->logical_height >= 360) {
            char shape[96];
            std::snprintf(shape, sizeof shape, "%ux%u/f%x/flags%x/uw%u", image->logical_width,
                          image->logical_height, dst->format, dst->flags, unsigned(image->uw));
            std::string state = std::string(shape) + ":" + (vs ? vs->hash : "-") + ":" +
                                (fs ? fs->hash : "-") + ":d" + std::to_string(g_state.depth.depth_test) +
                                ":vp" + std::to_string(g_state.has_viewport) + "/" +
                                std::to_string(g_state.viewport[0]) + "," + std::to_string(g_state.viewport[1]) +
                                "," + std::to_string(g_state.viewport[2]) + "," + std::to_string(g_state.viewport[3]);
            static std::set<std::string> seen_uw_draws;
            static unsigned uw_draw_count = 0;
            if (seen_uw_draws.insert(state).second && uw_draw_count++ < 500) {
                std::string sources;
                for (const ResolvedTexture& r : g_textures.resolved) {
                    GpuImage* sampled = r.texture ? image_of(r.texture) : nullptr;
                    Texture* source = r.texture ? get<Texture>(r.texture) : nullptr;
                    if (!sampled || !source) continue;
                    char b[112];
                    std::snprintf(b, sizeof b, "%llx:%ux%u/f%x%s ", (unsigned long long)r.texture,
                                  sampled->logical_width, sampled->logical_height, source->format,
                                  sampled->uw ? "W" : "");
                    sources += b;
                    if (sources.size() >= 240) break;
                }
                // A single shader/viewport often draws many distinct UI elements in one frame. Include the sampled
                // texture handles in the deduplication key so the trace keeps those separate (e.g. HUD vs map art).
                state += ":src=" + sources;
                log("nvn: UW trace: draw frame=%llu target=%s wide-pass=%u vs=%s fs=%s depth=%u viewport=%u,%d,%d,%d,%d sources=%s",
                    (unsigned long long)g_frame.load(), shape, unsigned(g_uw_wide), vs ? vs->hash.c_str() : "-",
                    fs ? fs->hash.c_str() : "-", unsigned(g_state.depth.depth_test), unsigned(g_state.has_viewport),
                    g_state.viewport[0], g_state.viewport[1], g_state.viewport[2], g_state.viewport[3], sources.c_str());
            }

            // The broad trace can spend its 500 entries on world materials before UI shaders are learned. Keep a
            // separate, bounded trace for those shaders so HUD draws remain visible later in a long session.
            static const bool uw_trace_ui = std::getenv("NX_UW_TRACE_UI") &&
                                            std::string(std::getenv("NX_UW_TRACE_UI")) == "1";
            if (uw_trace_ui && g_uw_wide && vs && uw_is_ui_vertex_shader(vs)) {
                std::string ui_state = std::string(shape) + ":" + vs->hash + ":" +
                                       (fs ? fs->hash : "-") + ":d" + std::to_string(g_state.depth.depth_test) +
                                       ":vp" + std::to_string(g_state.has_viewport) + "/" +
                                       std::to_string(g_state.viewport[0]) + "," + std::to_string(g_state.viewport[1]) +
                                       "," + std::to_string(g_state.viewport[2]) + "," + std::to_string(g_state.viewport[3]);
                std::string ui_sources;
                for (const ResolvedTexture& r : g_textures.resolved) {
                    GpuImage* sampled = r.texture ? image_of(r.texture) : nullptr;
                    Texture* source = r.texture ? get<Texture>(r.texture) : nullptr;
                    if (!sampled || !source) continue;
                    char b[112];
                    std::snprintf(b, sizeof b, "%llx:%ux%u/f%x%s ", (unsigned long long)r.texture,
                                  sampled->logical_width, sampled->logical_height, source->format,
                                  sampled->uw ? "W" : "");
                    ui_sources += b;
                }
                ui_state += ":src=" + ui_sources;
                static std::set<std::string> seen_uw_ui_draws;
                static unsigned uw_ui_draw_count = 0;
                if (seen_uw_ui_draws.insert(ui_state).second && uw_ui_draw_count++ < 1000) {
                    log("nvn: UW UI trace: frame=%llu target=%s vs=%s fs=%s depth=%u viewport=%u,%d,%d,%d,%d sources=%s",
                        (unsigned long long)g_frame.load(), shape, vs->hash.c_str(), fs ? fs->hash.c_str() : "-",
                        unsigned(g_state.depth.depth_test), unsigned(g_state.has_viewport), g_state.viewport[0],
                        g_state.viewport[1], g_state.viewport[2], g_state.viewport[3], ui_sources.c_str());
                }
            }
        }
    }
    if (flags.align && g_uw_wide && vs && fs &&
        vs->hash == "af561088cd1b1905" && fs->hash == "0bebd775cd1a4238") {
        key.uw_x = 1.0f;
        static bool logged = false;
        if (!logged) { logged = true; log("ultrawide map align test: corrected composite geometry x scale %g", double(key.uw_x)); }
    }
    key.topology = mode;
    key.attrib_count = uint32_t(std::min<size_t>(g_state.attribs.size(), 16));
    std::copy_n(g_state.attribs.begin(), key.attrib_count, key.attribs);
    key.stream_count = uint32_t(std::min<size_t>(g_state.streams.size(), 16));
    std::copy_n(g_state.streams.begin(), key.stream_count, key.streams);
    std::copy_n(g_state.blend, 8, key.blend);
    key.color = g_state.color, key.mask = g_state.mask, key.depth = g_state.depth, key.polygon = g_state.polygon;
    // NX_FREE_FS=<hash>:<d|c|dc>: draws with that fragment shader without the depth test (d) or culling (c).
    static const std::string free_fs = std::getenv("NX_FREE_FS") ? std::getenv("NX_FREE_FS") : "";
    if (fs && free_fs.size() > 17 && free_fs.compare(0, 16, fs->hash) == 0) {
        if (free_fs.find('d', 17) != std::string::npos) key.depth.depth_test = 0;
        if (free_fs.find('c', 17) != std::string::npos) key.polygon.cull = 0;
    }
    key.color_count = t.color_count, key.depth_clamp = g_state.depth_clamp;
    key.alpha_to_coverage = g_state.alpha_to_coverage;
    // NX_SHADOW_COORD_DEBUG substitutes the receiver FS; make its false-colour output visible regardless of the
    // game's material blend and channel-mask state so the diagnostic cannot silently look like the normal frame.
    if (shadow_coord_debug_on() && fs && fs->hash == "1872caa47df3cd7a") {
        std::memset(key.color.blend_enable, 0, sizeof key.color.blend_enable);
        std::memset(key.mask.mask, 0x0F, sizeof key.mask.mask);
        key.alpha_to_coverage = 0;
    }
    for (uint32_t k = 0; k < t.color_count; k++) {
        GpuImage* img = g_pass_images[k];
        key.color_formats[k] = img ? view_format(*img, t.views.format[k]) : unused_slot_format();
        if (img && (img->nvn_format == 0x3f || t.views.format[k] == 0x3f)) key.ufloat_colors |= 1u << k;  // G4
    }
    GpuImage* dimg = tt.depth ? g_pass_images[8] : nullptr;
    key.depth_format = dimg && dimg->aspect != VK_IMAGE_ASPECT_COLOR_BIT ? dimg->format : VK_FORMAT_UNDEFINED;
    if (g_pass_placeholder_color) {
        key.color_count = 1;
        key.color_formats[0] = VK_FORMAT_R8_UNORM;
    }
    // A pipeline not compiled yet may be compiled on a worker while this draw is skipped, when the next frame draws it
    // again: every target of the pass was also rendered in the previous frame (or this one). A target rendered for
    // the first time, or once now and then (a light probe, a capture), waits for its pipeline (nvn_pipeline_async.h).
    bool may_skip = t.color_count || dimg;
    const uint64_t frame = g_frame.load(std::memory_order_relaxed) + 1;
    for (uint32_t k = 0; k < t.color_count && may_skip; k++) {
        GpuImage* img = g_pass_images[k];
        may_skip = img && img->rendered_frame && img->rendered_frame + 1 >= frame;
    }
    if (may_skip && dimg) may_skip = dimg->rendered_frame && dimg->rendered_frame + 1 >= frame;
    VkPipeline pipeline = graphics_pipeline(key, layout, may_skip);
    if (!pipeline) {
        g_stats.no_pipeline++;
        return false;
    }

    if (g_bound.cb != cb || draw_slow(kSlowState)) g_bound = Bound{}, g_bound.cb = cb;
    if (g_bound.pipeline != pipeline) {
        p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        g_bound.pipeline = pipeline, g_state_set++;
    } else {
        g_state_kept++;
    }
    bind_sets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout);
    // GPL dynamic mode: the rasterizer and depth/stencil state the pipeline leaves to the command buffer.
    if (gpl_dynamic_state()) {
        const RasterDepthState rd = raster_depth_state(key);
        if (!g_bound.has_raster_depth || std::memcmp(&rd, &g_bound.raster_depth, sizeof rd) != 0) {
            set_raster_depth_state(cb, rd);
            g_bound.raster_depth = rd, g_bound.has_raster_depth = true, g_state_set++;
        } else {
            g_state_kept++;
        }
    }

    // The vertex buffers resolved above, plus the zero stream (binding 15).
    for (uint32_t s = 0; s < 15; s++)
        if (in.used >> s & 1) bind_vertex(cb, s, in.vertex[s]);
    bind_vertex(cb, 15, zero_stream());

    GpuImage* viewport_image = tt.color_count && tt.colors[0] ? g_pass_images[0] : g_pass_images[8];
    const float scale = viewport_image ? viewport_image->render_scale : 1.0f;
    VkViewport vp{0, 0, float(g_pass_extent.width), float(g_pass_extent.height), 0, 1};
    if (g_state.has_viewport)
        vp = {float(g_state.viewport[0]) * scale, float(g_state.viewport[1]) * scale,
              float(g_state.viewport[2]) * scale, float(g_state.viewport[3]) * scale, g_state.depth_near,
              g_state.depth_far};
    if (viewport_image && viewport_image->uw && g_state.has_viewport) {
        vp.x *= uw_factor();
        vp.width *= uw_factor();
        vp.x += flower_offset * scale;
        if (anchor_hud_left)
            vp.x -= (uw_factor() - 1.0f) * float(g_state.viewport[2]) * scale * 0.5f;
    }
    static const std::string offset_trace = [] { const char* e = std::getenv("NX_UW_OFFSET_TRACE"); return e ? e : ""; }();
    if (!offset_trace.empty()) {
        static unsigned offset_logs = 0;
        if (offset_logs < 400 && vs && fs && g_uw_wide &&
            (vs->hash == "a106c763cf05f818" || vs->hash == "2fc962d8d063c1c9" ||
             vs->hash == "5587b5f73977f07e")) {
            if (FILE* marker = std::fopen(offset_trace.c_str(), "rb")) {
                std::fclose(marker);
                const Range& u = g_state.uniforms[0][0];
                const float* f = u.address && u.size >= 96 ? reinterpret_cast<const float*>(guest_bytes(u.address)) : nullptr;
                if (f && t.color_count && t.colors[0]) {
                    GpuImage* target = image_of(t.colors[0]);
                    if (target && target->logical_width == 1920 && target->logical_height == 1080) {
                        ++offset_logs;
                        log("UW offset trace: frame=%llu vs=%s fs=%s text=%g,%g model=%g,%g offset=%g hud=%u flower=%u,%g,%g",
                            (unsigned long long)g_frame.load(), vs->hash.c_str(), fs->hash.c_str(), f[3], f[7], f[19], f[23],
                            flower_offset, unsigned(anchor_hud_left), unsigned(flower_seen), flower_x, flower_y);
                    }
                }
            }
        }
    }
    // NVN's upper-left window origin (what Wonder sets) maps NDC +Y to the top of the render target, like D3D;
    // Vulkan maps it to the bottom. A negative-height viewport gives NVN's mapping, and with it NVN's front faces.
    // (Before this, the front face was inverted instead: triangles were culled right, but every render target was
    // upside down, which the post chain undid in some scenes and not in others: the first course showed a
    // mirrored sky.)
    vp.y += vp.height, vp.height = -vp.height;
    VkRect2D sc{{0, 0}, g_pass_extent};
    if (g_state.has_scissor) {
        const bool uw_vp = viewport_image && viewport_image->uw;
        const bool settings_text = uw_vp && flags.settings &&
            vs && vs->hash == "a106c763cf05f818" && fs && fs->hash == "46841c209865a1c0" &&
            g_state.scissor[0] >= 1100 && g_state.scissor[0] <= 1200 &&
            g_state.scissor[2] >= 500 && g_state.scissor[2] <= 530 &&
            g_state.scissor[3] >= 30 && g_state.scissor[3] <= 65;
        const float sx = (uw_vp && !settings_text) ? (scale * uw_factor()) : scale;
        int32_t x = int32_t(std::lround(double(g_state.scissor[0]) * double(sx)));
        int32_t x1 = int32_t(std::lround(double(g_state.scissor[0] + g_state.scissor[2]) * double(sx)));
        int32_t y = int32_t(std::lround(double(g_state.scissor[1]) * double(scale)));
        int32_t y1 = int32_t(std::lround(double(g_state.scissor[1] + g_state.scissor[3]) * double(scale)));
        if (settings_text) {
            const int32_t off = int32_t(std::lround((uw_factor() - 1.0f) * 960.0f * scale));
            x += off;
            x1 += off;
        }
        uint32_t w = uint32_t(std::max<int32_t>(0, x1 - x));
        uint32_t h = uint32_t(std::max<int32_t>(0, y1 - y));
        sc = {{std::max(0, x), std::max(0, y)}, {w, h}};
    }
    set_dynamic(cb, vp, sc);

    trace_render_resolution(t);
    trace_skin(indexed, c, vs, fs);
    if (indexed) {
        uint32_t count = uint32_t(c.i[0]);
        if (g_bound.has_index && g_bound.index.buffer == in.index.buffer && g_bound.index.offset == in.index.offset &&
            g_bound.index_type == in.index_type) {
            g_state_kept++;
        } else {
            p_vkCmdBindIndexBuffer(cb, in.index.buffer, in.index.offset, in.index_type);
            g_bound.has_index = true, g_bound.index = in.index, g_bound.index_type = in.index_type, g_state_set++;
        }
        bool q = zero_draws_on() && zero_draws_begin(cb, vs->hash + " " + (fs ? fs->hash : std::string("-")));
        p_vkCmdDrawIndexed(cb, count, uint32_t(std::max(1, c.i[3])), 0, c.i[1], uint32_t(c.i[2]));
        if (q) zero_draws_end(cb);
    } else {
        bool q = zero_draws_on() && zero_draws_begin(cb, vs->hash + " " + (fs ? fs->hash : std::string("-")));
        p_vkCmdDraw(cb, uint32_t(c.i[1]), uint32_t(std::max(1, c.i[3])), uint32_t(c.i[0]), uint32_t(c.i[2]));
        if (q) zero_draws_end(cb);
    }
    g_stats.done++;
    g_pass_written |= ((1u << t.color_count) - 1) | (g_state.depth.depth_write || g_state.depth.stencil_test ? 1u << 8 : 0);
    // NX_SNAP_FS=<hash>,<hash>[@<first draw>]: also snapshot after draws with those fragment shaders (8 at most, or
    // NX_SNAP_FS_MAX).
    static const std::string snap_fs = std::getenv("NX_SNAP_FS") ? std::getenv("NX_SNAP_FS") : "";
    static const uint64_t snap_from = snap_fs.find('@') != std::string::npos
                                          ? std::strtoull(snap_fs.c_str() + snap_fs.find('@') + 1, nullptr, 10) : 0;
    static int snap_count = 0;
    static const int snap_max = std::getenv("NX_SNAP_FS_MAX") ? std::atoi(std::getenv("NX_SNAP_FS_MAX")) : 8;
    bool snap_by_fs = !snap_fs.empty() && g_stats.done >= snap_from && snap_count < snap_max &&
                      ((fs && snap_fs.find(fs->hash) != std::string::npos) ||
                       snap_fs.find(vs->hash) != std::string::npos) &&
                      ++snap_count;  // the vertex shader's hash matches too
    if ((snap_by_fs || snap_follow(t, vs, fs, g_textures.handles) || debug_snap_wanted(g_stats.done)) && t.color_count && t.colors[0])
        if (GpuImage* snap = image_of(t.colors[0])) draw_end_rendering(cb, 6), debug_snapshot(cb, *snap, g_stats.done);
    // NX_DUMP_ON_TARGET=<hex NVNtexture*>: every NX_DUMP_FRAMES-th draw into it asks for NX_DUMP_TEXTURES to be read
    // back right after this submit (before later frames change them).
    static const char* dump_env = std::getenv("NX_DUMP_ON_TARGET");
    static const uint64_t dump_target = dump_env ? std::strtoull(dump_env, nullptr, 16) : 0;
    if (dump_target && t.colors[0] == dump_target) g_dump_request = ++g_dump_draws;
    // NX_DUMP_ON_FS=<fs hash>[,...]: a draw with one of these fragment shaders dumps its target 0 and what it samples.
    static const std::string dump_fs = [] { const char* e = std::getenv("NX_DUMP_ON_FS"); return std::string(e ? e : ""); }();
    static const uint64_t dump_from = [] { const char* e = std::getenv("NX_DUMP_FROM"); return e ? std::strtoull(e, nullptr, 10) : 0; }();
    static const char* dump_trigger = std::getenv("NX_DUMP_TRIGGER_FILE");
    static bool dump_triggered = !dump_trigger || !*dump_trigger;
    if (!dump_triggered && fs && !dump_fs.empty() && dump_fs.find(fs->hash) != std::string::npos)
        if (FILE* marker = std::fopen(dump_trigger, "rb")) { std::fclose(marker); dump_triggered = true; log("map dump: trigger received"); }
    static const uint64_t dump_limit = [] { const char* e = std::getenv("NX_DUMP_LIMIT"); return e ? std::strtoull(e, nullptr, 10) : 400ull; }();
    if (dump_triggered && !dump_fs.empty() && fs && g_frame >= dump_from && dump_fs.find(fs->hash) != std::string::npos && g_dump_draws < dump_limit &&
        t.colors[0] && image_of(t.colors[0]) && (std::getenv("NX_DUMP_ANY") || image_of(t.colors[0])->view_type == VK_IMAGE_VIEW_TYPE_CUBE ||
                                                           image_of(t.colors[0])->view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY)) {
        debug_dump_add(t.colors[0]);
        log("map dump: shader=%s target=%llx", fs->hash.c_str(), (unsigned long long)t.colors[0]);
        for (const ResolvedTexture& r : g_textures.resolved) { log("map dump: sample=%llx", (unsigned long long)r.texture); debug_dump_add(r.texture); }
        g_dump_request = ++g_dump_draws;
    }
    trace_draw(t, mode, indexed, c, vs, fs, g_textures.bindings, g_textures.handles);
    return true;
}

}  // namespace

void draw_end_submit() {  // after the submit's fence: compute results to guest memory, debug snapshots
    debug_snapshots_write();
    for (const WriteBack& w : draw_take_write_backs()) {
        trace_nan_write_back(w.from, w.to, w.size);
        apply_write_back(w);
    }
}

std::vector<WriteBack> draw_take_write_backs() {  // an asynchronous submit copies them when it is retired
    std::vector<WriteBack> v = written_take();  // one per written range (disjoint), then NX_COMPUTE_SEPARATE's
    v.insert(v.end(), separate_write_backs().begin(), separate_write_backs().end());
    separate_write_backs().clear();
    return v;
}

void draw_begin_submit() {
    g_rendering = false;
    g_vertex_pending = false, g_vertex_visible_at = g_pass_serial;  // each command buffer begins with a full barrier
    g_bound = Bound{};  // a new command buffer
    submit_lists_reset();
    sets_begin_submit();
    clears_begin_submit();
    g_submit_ns = plat::monotonic_ns();
    dedup_reset();
    written_reset();
    pipeline_begin_submit();
}

uint64_t recorded_draws();

void draw_log_stats() {
    if (g_vertex_barriers) log("nvn passes: %llu vertex-read barriers (G4)", (unsigned long long)g_vertex_barriers);
    dedup_log();
    render_pass_log();
    written_log();
    sampled_ready_log();
    sets_log();
    if (g_state_set + g_state_kept)
        log("nvn: draw state: %llu binds and sets recorded, %llu already in place", (unsigned long long)g_state_set,
            (unsigned long long)g_state_kept);
    g_state_set = g_state_kept = 0;
    clears_log();
    log("nvn passes ended: %llu submit end, %llu draw, %llu dispatch, %llu clear of other targets, %llu clear larger than "
        "the pass, %llu copy, %llu snapshot, %llu SetRenderTargets", (unsigned long long)g_pass_ends[0],
        (unsigned long long)g_pass_ends[1], (unsigned long long)g_pass_ends[2], (unsigned long long)g_pass_ends[3],
        (unsigned long long)g_pass_ends[4], (unsigned long long)g_pass_ends[5], (unsigned long long)g_pass_ends[6],
        (unsigned long long)g_pass_ends[7]);
    Stats& t = g_stats;
    log("nvn draws: %llu recorded, %llu replayed:", (unsigned long long)recorded_draws(), (unsigned long long)t.total);
    log("nvn draws: %llu total, %llu done, %llu no program, %llu no vertex shader, %llu no pipeline, %llu no indices",
        (unsigned long long)t.total, (unsigned long long)t.done, (unsigned long long)t.no_program,
        (unsigned long long)t.no_vertex, (unsigned long long)t.no_pipeline, (unsigned long long)t.no_indices);
    log("nvn passes: %llu ended by a draw's texture uploads or layout changes, %llu by new targets, %llu dispatches",
        (unsigned long long)t.breaks_textures, (unsigned long long)t.breaks_targets, (unsigned long long)t.dispatches);
}

void pass_end_only(VkCommandBuffer cb, int reason) {
    if (!g_rendering) return;
    g_pass_ends[reason & 7]++, trace_pass("end: reason", g_pass, uint32_t(reason), 0);
    if (g_render_pass)
        p_vkCmdEndRenderPass(cb);
    else
        p_vkCmdEndRendering(cb);
    g_rendering = false;
    // What the pass wrote is read by what follows (sampled, copied, loaded by the next pass), and what it sampled may
    // be rendered over next. The images stay GENERAL, so transition() records no barrier. Metal (MoltenVK) and the
    // Intel driver ordered passes anyway; NVIDIA overlaps them, and later passes read half-written tiles: the course
    // HUD in blocks (DEBUG-2 #71). NX_NO_PASS_BARRIER=1: none, as before (bisecting).
    // Narrow (PHASE4.md G4; NX_PASS_BARRIER_NARROW=1): the later stages only, so the next pass's vertex work and
    // binning may start while this one finishes. A draw whose vertex shader samples a rendered image gets a barrier
    // of its own first (vertex_read_barrier). By default ALL_COMMANDS, as before.
    static const bool no_barrier = std::getenv("NX_NO_PASS_BARRIER") != nullptr;
    if (!no_barrier) {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        VkPipelineStageFlags dst = narrow_pass_barrier()
                                       ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                             VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
                                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                                       : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, dst, 0, 1, &mb, 0, nullptr, 0, nullptr);
        g_vertex_pending |= narrow_pass_barrier();
    }
    g_pass_serial++;
    for (uint32_t k = 0; k < 9; k++)  // this pass's targets only (the other slots may name freed images)
        if ((k < g_pass.color_count || (k == 8 && g_pass.depth)) && g_pass_images[k]) {
            g_pass_images[k]->rendered_at = g_pass_serial;
            note_written(*g_pass_images[k]);
            g_pass_images[k]->rendered_frame = g_frame.load(std::memory_order_relaxed) + 1;
        }
    gpu_profile_mark(cb, "outside passes (clears, uploads, copies)");
}
void draw_end_rendering(VkCommandBuffer cb, int reason) {  // and the pending clears (nvn_clears.cpp)
    pass_end_only(cb, reason);
    clears_flush(cb);
}
bool pass_open() { return g_rendering; }
bool pass_open_on(const Targets& t) { return g_rendering && targets_same(g_pass, t); }
VkExtent2D pass_extent() { return g_pass_extent; }
void pass_begin(VkCommandBuffer cb, const Targets& t) { begin_rendering(cb, t); }
// After a clear inside the pass (nvn_clears.cpp). A masked one is a fullscreen draw with its own pipeline, viewport
// and scissor (nvn_mask_clear.cpp): the next draw sets everything again.
void pass_mark_written(uint32_t bits) { g_pass_written |= bits, g_bound = Bound{}; }

bool replay::pass_wrote(const GpuImage* img) {
    if (!g_rendering || !img) return false;
    uint32_t hit = g_pass.depth && g_pass_images[8] == img ? 1u << 8 : 0;
    for (uint32_t c = 0; c < g_pass.color_count; c++)
        if (g_pass.colors[c] && g_pass_images[c] == img) hit |= 1u << c;
    return (hit & g_pass_written) != 0;
}

uint64_t submit_start_ns() { return g_submit_ns; }

// Entry points of the draws and dispatches for draw_command (nvn_draw_state.cpp).
bool draw_call(VkCommandBuffer cb, const Targets& t, uint32_t mode, bool indexed, const Cmd& c) {
    return draw(cb, t, mode, indexed, c);
}
void dispatch_call(VkCommandBuffer cb, const Cmd& c) { dispatch(cb, c); }

uint64_t draw_take_dump_request() {
    uint64_t r = g_dump_request;
    g_dump_request = 0;
    return r;
}

}  // namespace nx::nvn
