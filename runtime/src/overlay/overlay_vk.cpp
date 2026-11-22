// Draws the overlay into the swapchain image with Dear ImGui's Vulkan backend: one render pass that loads the image
// as presented (PRESENT_SRC in and out), on the command buffer that already holds the frame's blit or present pass.
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "vk_bridge.h"  // first: the platform's Vulkan defines (Win32 surface types) before vk_gen.h

#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"

#include "nvn/nvn_vk.h"
#include "overlay/overlay_internal.h"

namespace nx::overlay {

using namespace nx::vk;

namespace {

bool g_ready = false;  // the ImGui Vulkan backend is initialised for g_format
VkFormat g_format = VK_FORMAT_UNDEFINED;
VkRenderPass g_pass = VK_NULL_HANDLE;
std::vector<VkImage> g_images;
std::vector<VkImageView> g_views;
std::vector<VkFramebuffer> g_framebuffers;
VkExtent2D g_extent{};

struct IconTexture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSet ds = VK_NULL_HANDLE;
};
std::vector<IconTexture> g_icon_textures;

void drop_icon_textures() {
    VkDevice dev = nvn::vk_device();
    for (IconTexture& t : g_icon_textures) {
        if (t.ds) ImGui_ImplVulkan_RemoveTexture(t.ds);
        if (t.sampler) p_vkDestroySampler(dev, t.sampler, nullptr);
        if (t.view) p_vkDestroyImageView(dev, t.view, nullptr);
        if (t.image) p_vkDestroyImage(dev, t.image, nullptr);
        if (t.memory) p_vkFreeMemory(dev, t.memory, nullptr);
    }
    g_icon_textures.clear();
}

PFN_vkVoidFunction load(const char* name, void*) {
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(nvn::vk_gipa());
    return gipa ? gipa(nvn::vk_instance(), name) : nullptr;
}

void check(VkResult r) {
    if (r != VK_SUCCESS) log("overlay: Vulkan error %d", int(r));
}

void drop_framebuffers() {
    for (VkFramebuffer f : g_framebuffers) p_vkDestroyFramebuffer(nvn::vk_device(), f, nullptr);
    for (VkImageView v : g_views) p_vkDestroyImageView(nvn::vk_device(), v, nullptr);
    g_framebuffers.clear(), g_views.clear(), g_images.clear();
}

bool make_pass(VkFormat format) {
    VkAttachmentDescription a{};
    a.format = format;
    a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    a.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    // The blit (transfer) or the present pass (colour) wrote the image just before.
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = 1;
    ci.pAttachments = &a;
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    ci.dependencyCount = 2;
    ci.pDependencies = deps;
    return p_vkCreateRenderPass(nvn::vk_device(), &ci, nullptr, &g_pass) == VK_SUCCESS;
}

bool ensure_backend(VkFormat format, uint32_t image_count) {
    if (g_ready && format == g_format) return true;
    if (g_ready) {
        p_vkDeviceWaitIdle(nvn::vk_device());
        drop_icon_textures();
        ImGui_ImplVulkan_Shutdown();
        drop_framebuffers();
        p_vkDestroyRenderPass(nvn::vk_device(), g_pass, nullptr);
        g_pass = VK_NULL_HANDLE, g_ready = false;
    }
    static bool loaded = false;
    if (!loaded && !(loaded = ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, load))) {
        log("overlay: Vulkan functions not found");
        return false;
    }
    if (!make_pass(format)) return false;
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_3;
    info.Instance = nvn::vk_instance();
    info.PhysicalDevice = nvn::vk_physical();
    info.Device = nvn::vk_device();
    info.QueueFamily = nvn::vk_queue_family();
    info.Queue = nvn::vk_queue();
    info.DescriptorPoolSize = 16;
    info.RenderPass = g_pass;
    info.MinImageCount = image_count < 2 ? 2 : image_count;
    info.ImageCount = info.MinImageCount;
    info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.CheckVkResultFn = check;
    if (!ImGui_ImplVulkan_Init(&info)) return false;
    g_format = format, g_ready = true;
    log("overlay: Dear ImGui %s on Vulkan, swapchain format %d", IMGUI_VERSION, int(format));
    return true;
}

bool ensure_framebuffers(const std::vector<VkImage>& images, VkExtent2D extent) {
    if (images == g_images && extent.width == g_extent.width && extent.height == g_extent.height) return true;
    drop_framebuffers();
    for (VkImage image : images) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = g_format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        if (p_vkCreateImageView(nvn::vk_device(), &vi, nullptr, &view) != VK_SUCCESS) return false;
        g_views.push_back(view);
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = g_pass;
        fi.attachmentCount = 1;
        fi.pAttachments = &view;
        fi.width = extent.width, fi.height = extent.height, fi.layers = 1;
        VkFramebuffer fb = VK_NULL_HANDLE;
        if (p_vkCreateFramebuffer(nvn::vk_device(), &fi, nullptr, &fb) != VK_SUCCESS) return false;
        g_framebuffers.push_back(fb);
    }
    g_images = images, g_extent = extent;
    return true;
}

}  // namespace

// NX_OVERLAY_TIMING=1: the overlay's CPU time per drawn frame (ImGui frame + recording), logged every 300 frames.
struct Timer {
    uint64_t t0 = plat::monotonic_ns();
    ~Timer() {
        static const bool on = std::getenv("NX_OVERLAY_TIMING") != nullptr;
        if (!on) return;
        static uint64_t n = 0, sum = 0, most = 0;
        const uint64_t d = plat::monotonic_ns() - t0;
        n++, sum += d, most = std::max(most, d);
        if (n % 300 == 0) log("overlay: %.3f ms a frame on average, %.3f ms at most (300 frames)", double(sum) / 300e6,
                              double(most) / 1e6), sum = 0, most = 0;
    }
};

bool record(VkCommandBuffer cb, uint32_t index, const std::vector<VkImage>& images, VkFormat format, VkExtent2D extent) {
    if (!enabled() || images.empty() || index >= images.size() || !frame_wanted()) return false;
    Timer timer;
    ensure_context();
    if (!ensure_backend(format, uint32_t(images.size())) || !ensure_framebuffers(images, extent)) return false;
    ImGui_ImplVulkan_NewFrame();  // uploads the font atlas the first time (before ImGui::NewFrame)
    ImDrawData* data = build_frame(extent.width, extent.height);
    if (!data || data->CmdListsCount == 0) return false;
    VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = g_pass;
    bi.framebuffer = g_framebuffers[index];
    bi.renderArea.extent = extent;
    p_vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(data, cb);
    p_vkCmdEndRenderPass(cb);
    return true;
}

void release() {
    if (!g_ready) return;
    p_vkDeviceWaitIdle(nvn::vk_device());
    drop_icon_textures();
    drop_framebuffers();
}

uint64_t vk_upload_rgba_texture(const uint8_t* rgba, int w, int h) {
    if (!g_ready || !rgba || w <= 0 || h <= 0) return 0;
    VkDevice dev = nvn::vk_device();
    const VkDeviceSize size = VkDeviceSize(w) * VkDeviceSize(h) * 4;

    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory buf_mem = VK_NULL_HANDLE;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (p_vkCreateBuffer(dev, &bci, nullptr, &buf) != VK_SUCCESS) return 0;
    VkMemoryRequirements breq{};
    p_vkGetBufferMemoryRequirements(dev, buf, &breq);
    VkMemoryAllocateInfo bai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    bai.allocationSize = breq.size;
    bai.memoryTypeIndex = nvn::memory_type(breq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (bai.memoryTypeIndex == UINT32_MAX || p_vkAllocateMemory(dev, &bai, nullptr, &buf_mem) != VK_SUCCESS) {
        p_vkDestroyBuffer(dev, buf, nullptr);
        return 0;
    }
    p_vkBindBufferMemory(dev, buf, buf_mem, 0);
    void* mapped = nullptr;
    if (p_vkMapMemory(dev, buf_mem, 0, size, 0, &mapped) != VK_SUCCESS) {
        p_vkDestroyBuffer(dev, buf, nullptr);
        p_vkFreeMemory(dev, buf_mem, nullptr);
        return 0;
    }
    std::memcpy(mapped, rgba, size_t(size));
    p_vkUnmapMemory(dev, buf_mem);

    IconTexture tex{};
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {uint32_t(w), uint32_t(h), 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (p_vkCreateImage(dev, &ici, nullptr, &tex.image) != VK_SUCCESS) {
        p_vkDestroyBuffer(dev, buf, nullptr);
        p_vkFreeMemory(dev, buf_mem, nullptr);
        return 0;
    }
    VkMemoryRequirements ireq{};
    p_vkGetImageMemoryRequirements(dev, tex.image, &ireq);
    VkMemoryAllocateInfo iai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    iai.allocationSize = ireq.size;
    iai.memoryTypeIndex = nvn::memory_type(ireq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (iai.memoryTypeIndex == UINT32_MAX || p_vkAllocateMemory(dev, &iai, nullptr, &tex.memory) != VK_SUCCESS) {
        p_vkDestroyImage(dev, tex.image, nullptr);
        p_vkDestroyBuffer(dev, buf, nullptr);
        p_vkFreeMemory(dev, buf_mem, nullptr);
        return 0;
    }
    p_vkBindImageMemory(dev, tex.image, tex.memory, 0);

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = nvn::vk_queue_family();
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    p_vkCreateCommandPool(dev, &pci, nullptr, &pool);
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    p_vkAllocateCommandBuffers(dev, &cbai, &cb);
    VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    p_vkBeginCommandBuffer(cb, &cbi);

    auto barrier = [&](VkImageLayout old_l, VkImageLayout new_l, VkAccessFlags src_a, VkAccessFlags dst_a,
                       VkPipelineStageFlags src_s, VkPipelineStageFlags dst_s) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = old_l;
        b.newLayout = new_l;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = tex.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = src_a;
        b.dstAccessMask = dst_a;
        p_vkCmdPipelineBarrier(cb, src_s, dst_s, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {uint32_t(w), uint32_t(h), 1};
    p_vkCmdCopyBufferToImage(cb, buf, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    p_vkEndCommandBuffer(cb);

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    p_vkQueueSubmit(nvn::vk_queue(), 1, &si, VK_NULL_HANDLE);
    p_vkQueueWaitIdle(nvn::vk_queue());
    p_vkDestroyCommandPool(dev, pool, nullptr);
    p_vkDestroyBuffer(dev, buf, nullptr);
    p_vkFreeMemory(dev, buf_mem, nullptr);

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = tex.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    p_vkCreateImageView(dev, &vci, nullptr, &tex.view);

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    p_vkCreateSampler(dev, &sci, nullptr, &tex.sampler);

    tex.ds = ImGui_ImplVulkan_AddTexture(tex.sampler, tex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    g_icon_textures.push_back(tex);
    return uint64_t(reinterpret_cast<uintptr_t>(tex.ds));
}

}  // namespace nx::overlay
