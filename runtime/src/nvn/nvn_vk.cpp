// NVN's GPU side on Vulkan (docs/NVN.md, N3): the host device, a VkImage per NVN texture the GPU touches, the
// replay of recorded command lists, and presenting a window texture on the host swapchain.
//
// First version: every submit is recorded into one Vulkan command buffer and waited for (NX_PIPELINED, NX_ASYNC_GPU:
// into a slot's, not waited for, nvn_frames.cpp); image layouts are tracked per image, in submission order. The image
// behind a texture is nvn_image.cpp. On macOS this is MoltenVK: the instance enumerates portability drivers and the
// device enables VK_KHR_portability_subset.
#if !defined(_WIN32)
#include <unistd.h>  // _exit (stdlib.h on Windows)
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <atomic>
#include <tuple>
#include <vector>

#include "nvn_vk.h"
#include "overlay/overlay.h"
#include "nvn_pipeline.h"
#include "nvn_shader_prepare.h"
#include "platform.h"

#include <nx/game.h>

namespace nx::nvn {

using namespace nx::vk;


namespace {

std::recursive_mutex g_gpu;  // the whole GPU side: one submitting thread at a time
bool g_ready = false, g_failed = false;
bool g_shader_prepare_checked = false;
VkInstance g_instance = VK_NULL_HANDLE;
VkPhysicalDevice g_physical = VK_NULL_HANDLE;
VkDevice g_device = VK_NULL_HANDLE;
VkQueue g_queue = VK_NULL_HANDLE;
uint32_t g_family = 0;
VkPhysicalDeviceMemoryProperties g_memory{};
VkCommandPool g_pool = VK_NULL_HANDLE;
VkCommandBuffer g_cb = VK_NULL_HANDLE;
VkFence g_fence = VK_NULL_HANDLE;

VkSurfaceKHR g_surface = VK_NULL_HANDLE;
VkSwapchainKHR g_swapchain = VK_NULL_HANDLE;
std::vector<VkImage> g_sc_images;
VkExtent2D g_sc_extent{};
VkExtent2D g_sc_current{};  // the surface's currentExtent when the swapchain was made (SUBOPTIMAL: did it change?)
VkSurfaceTransformFlagBitsKHR g_sc_transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
VkFormat g_sc_format = VK_FORMAT_B8G8R8A8_UNORM;
VkImageUsageFlags g_sc_usage = 0;  // the swapchain images' usage (make_swapchain)
bool dump_presented() {
    static const bool on = std::getenv("NX_DUMP_PRESENTED") != nullptr;
    return on;
}
VkSemaphore g_acquired = VK_NULL_HANDLE, g_rendered = VK_NULL_HANDLE;
VkBool32 g_depth_clip_control = VK_FALSE;
bool g_gpl = false, g_cache_control = false;  // vk_gpl(), vk_pipeline_cache_control()
VkPhysicalDeviceFeatures g_features{};  // the core features enabled (vk_features())
VkDescriptorPool g_descriptors = VK_NULL_HANDLE;

// The per-submit upload buffer (nvn_vk.h): one host-visible buffer, bump-allocated, reset at every submit.
VkBuffer g_upload = VK_NULL_HANDLE;
VkDeviceMemory g_upload_memory = VK_NULL_HANDLE;
uint8_t* g_upload_ptr = nullptr;
uint64_t g_upload_size = 0, g_upload_used = 0;
void make_upload_buffer(uint64_t size);

// Commands the Hades bridge intercepts are not in the generated table: ours are loaded here.
PFN_vkEnumerateInstanceExtensionProperties p_enum_instance_ext = nullptr;
PFN_vkCreateInstance p_create_instance = nullptr;
PFN_vkEnumeratePhysicalDevices p_enum_physical = nullptr;
PFN_vkCreateDevice p_create_device = nullptr;

}  // namespace

// A lost device means the GPU hung (a shader looping for ever): the process ends at once. Submitting more work, or
// waiting, keeps the GPU stuck, and on macOS that froze WindowServer and restarted the machine.
[[noreturn]] void gpu_hung(const char* why) {
    log("nvn/vk: GPU hang (%s): stopping", why);
    std::fprintf(stderr, "nvn/vk: GPU hang (%s): stopping\n", why);
    _exit(4);
}

namespace {

bool check(VkResult r, const char* what) {
    if (r >= 0) return true;
    log("nvn/vk: %s failed (%d)", what, int(r));
    if (r == VK_ERROR_DEVICE_LOST) gpu_hung(what);
    return false;
}

bool has_extension(const std::vector<VkExtensionProperties>& list, const char* name) {
    for (auto& e : list)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

void* g_gipa = nullptr;  // vkGetInstanceProcAddr, for surfaces made later (make_surface)
bool make_surface();

bool init() {
    if (g_ready || g_failed) return g_ready;
    g_failed = true;  // until everything below succeeds
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(plat::vulkan_get_instance_proc_addr());
    if (!gipa) {
        log("nvn/vk: no Vulkan loader");
        return false;
    }
    for (size_t k = 0; k < g_entry_count; k++) *g_entries[k].pfn = gipa(nullptr, g_entries[k].name);
    p_enum_instance_ext = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
        gipa(nullptr, "vkEnumerateInstanceExtensionProperties"));
    p_create_instance = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));

    uint32_t n = 0;
    p_enum_instance_ext(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> iext(n);
    p_enum_instance_ext(nullptr, &n, iext.data());
    std::vector<const char*> exts = {VK_KHR_SURFACE_EXTENSION_NAME, plat::vulkan_surface_extension()};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    if (has_extension(iext, "VK_KHR_portability_enumeration")) {
        exts.push_back("VK_KHR_portability_enumeration");
        ici.flags |= 0x00000001;  // VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = nx::game::kInfo.title;
    app.apiVersion = VK_API_VERSION_1_3;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = uint32_t(exts.size());
    ici.ppEnabledExtensionNames = exts.data();
    if (!check(p_create_instance(&ici, nullptr, &g_instance), "vkCreateInstance")) return false;
    for (size_t k = 0; k < g_entry_count; k++)
        if (auto p = gipa(g_instance, g_entries[k].name)) *g_entries[k].pfn = p;
    p_enum_physical = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(g_instance, "vkEnumeratePhysicalDevices"));
    p_create_device = reinterpret_cast<PFN_vkCreateDevice>(gipa(g_instance, "vkCreateDevice"));

    n = 0;
    p_enum_physical(g_instance, &n, nullptr);
    std::vector<VkPhysicalDevice> gpus(n);
    p_enum_physical(g_instance, &n, gpus.data());
    gpus.resize(n);
    g_physical = pick_physical_device(gpus);  // NX_GPU, default: the first discrete GPU (nvn_gpu_pick.cpp)
    if (!g_physical) {
        log("nvn/vk: no GPU");
        return false;
    }
    VkPhysicalDeviceProperties props{};
    p_vkGetPhysicalDeviceProperties(g_physical, &props);
    p_vkGetPhysicalDeviceMemoryProperties(g_physical, &g_memory);

    g_gipa = reinterpret_cast<void*>(gipa);
    if (!make_surface()) return false;

    p_vkGetPhysicalDeviceQueueFamilyProperties(g_physical, &n, nullptr);
    std::vector<VkQueueFamilyProperties> families(n);
    p_vkGetPhysicalDeviceQueueFamilyProperties(g_physical, &n, families.data());
    g_family = UINT32_MAX;
    for (uint32_t k = 0; k < n; k++) {
        VkBool32 present = VK_FALSE;
        p_vkGetPhysicalDeviceSurfaceSupportKHR(g_physical, k, g_surface, &present);
        if ((families[k].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
            g_family = k;
            break;
        }
    }
    if (g_family == UINT32_MAX) {
        log("nvn/vk: no graphics + present queue");
        return false;
    }

    p_vkEnumerateDeviceExtensionProperties(g_physical, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> dext(n);
    p_vkEnumerateDeviceExtensionProperties(g_physical, nullptr, &n, dext.data());
    std::vector<const char*> dexts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    // Features: everything core the GPU has, dynamic rendering (1.3), 8-bit indices (NVN has them), clip-space
    // depth -1..1 (NVN's default depth mode, what Wonder uses), and on MoltenVK the portability subset's own
    // features, which must be enabled to be used.
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceIndexTypeUint8FeaturesEXT fu8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT};
    VkPhysicalDeviceDepthClipControlFeaturesEXT fclip{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
    struct {  // VkPhysicalDevicePortabilitySubsetFeaturesKHR, without its header (beta extensions)
        VkStructureType sType = VkStructureType(1000163000);
        void* pNext = nullptr;
        VkBool32 fields[15] = {};
    } fport;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    features.pNext = &f13;
    f13.pNext = &f12;
    f12.pNext = &f11;
    void** tail = &f11.pNext;
    if (has_extension(dext, "VK_EXT_index_type_uint8")) {
        dexts.push_back("VK_EXT_index_type_uint8");
        *tail = &fu8, tail = &fu8.pNext;
    }
    if (has_extension(dext, "VK_EXT_depth_clip_control")) {
        dexts.push_back("VK_EXT_depth_clip_control");
        *tail = &fclip, tail = &fclip.pNext;
    }
    if (has_extension(dext, "VK_KHR_portability_subset")) {
        dexts.push_back("VK_KHR_portability_subset");
        *tail = &fport, tail = &fport.pNext;
    }
    // VK_EXT_graphics_pipeline_library (nvn_pipeline.cpp): last in the chain, so it can be unlinked again when unused.
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT fgpl{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
    const bool gpl_ext = has_extension(dext, "VK_EXT_graphics_pipeline_library") &&
                         has_extension(dext, "VK_KHR_pipeline_library");
    void** gpl_link = tail;
    if (gpl_ext) *tail = &fgpl, tail = &fgpl.pNext;
    p_vkGetPhysicalDeviceFeatures2(g_physical, &features);
    {
        // NX_GPL=0: off; NX_GPL=1: on (even without fast linking); default: on with graphicsPipelineLibrary and
        // fast linking (without it a link may cost a compile, and the old path is no worse), except on AMD's
        // proprietary driver. There, running GPL and non-GPL builds from the same exe breaks whichever runs second:
        // GPL after non-GPL draws the lit scene as black silhouettes, non-GPL after GPL hangs the GPU at boot. The
        // driver keeps it per exe path across runs and builds (its own shader cache, not ours: it survived our
        // cache files moved aside, a separate pipeline cache, a specialization constant, a renamed entry point,
        // distinct static state and a distinct VkApplicationInfo engine). An in-place update of an existing install
        // would hit it, so GPL is opt-in there until the cause is known. GPL alone on an exe path renders right.
        // On Linux's AMD drivers (RADV, RX 9070 XT, 0.4.0) GPL draws dark patches on the course ground and fences
        // until the camera passes them, and the picture flashes; NX_GPL=0 is clean at 60 and 120 fps. So GPL is
        // opt-in on every AMD GPU (vendor 0x1002), whatever the driver.
        const char* e = std::getenv("NX_GPL");
        VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT pgpl{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT};
        VkPhysicalDeviceDriverProperties pdrv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, &pgpl};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &pdrv};
        if (!(gpl_ext && fgpl.graphicsPipelineLibrary)) pdrv.pNext = nullptr;
        p_vkGetPhysicalDeviceProperties2(g_physical, &p2);
        const bool amd = p2.properties.vendorID == 0x1002;
        const bool wanted = e ? e[0] != '0' : !amd;
        const bool forced = e && e[0] == '1';
        g_gpl = wanted && gpl_ext && fgpl.graphicsPipelineLibrary && f13.dynamicRendering &&
                (pgpl.graphicsPipelineLibraryFastLinking || forced);
        g_cache_control = f13.pipelineCreationCacheControl;
        log("nvn/vk: graphics pipeline library: extension %u, feature %u, fast linking %u, independent "
            "interpolation %u: %s; pipelineCreationCacheControl %u",
            gpl_ext ? 1u : 0u, fgpl.graphicsPipelineLibrary, pgpl.graphicsPipelineLibraryFastLinking,
            pgpl.graphicsPipelineLibraryIndependentInterpolationDecoration,
            g_gpl ? "used" : !wanted ? (e ? "off (NX_GPL=0)" : "off by default on AMD GPUs (NX_GPL=1: on)") : "not used",
            g_cache_control ? 1u : 0u);
        if (g_gpl) {
            dexts.push_back("VK_KHR_pipeline_library");
            dexts.push_back("VK_EXT_graphics_pipeline_library");
        } else if (gpl_ext) {
            *gpl_link = nullptr;  // not enabled: its feature struct must not reach vkCreateDevice
            tail = gpl_link;
        }
    }
    // Of 1.1 and 1.2, only what the translated shaders and samplers use (they are valid only with it enabled):
    // shaderDrawParameters, for the BaseInstance/BaseVertex the shaders take the instance and vertex ids relative to;
    // samplerMirrorClampToEdge, for NVN's mirror-clamp wrap modes (nvn_sampler.cpp).
    VkBool32 mirror_clamp = f12.samplerMirrorClampToEdge, draw_params = f11.shaderDrawParameters;
    f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, f12.pNext};
    f12.samplerMirrorClampToEdge = mirror_clamp;
    f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, f11.pNext};
    f11.shaderDrawParameters = draw_params;
    log("nvn/vk: samplerMirrorClampToEdge %u, shaderDrawParameters %u", mirror_clamp, draw_params);
    g_depth_clip_control = fclip.depthClipControl;
    g_features = features.features;
    // NX_ASTC_DECODE=1|linear|srgb: the CPU decoder even where the GPU has ASTC (to compare the two on one GPU).
    set_astc_native(g_features.textureCompressionASTC_LDR, g_features.textureCompressionBC);
    log("nvn/vk: ASTC %s", astc_native() ? "native" : "decoded to RGBA8 at upload (no textureCompressionASTC_LDR)");
    log("nvn/vk: depthClamp %u, depthBiasClamp %u, depthBounds %u, depth clip control %u (extension %u)",
        g_features.depthClamp, g_features.depthBiasClamp, g_features.depthBounds, g_depth_clip_control,
        has_extension(dext, "VK_EXT_depth_clip_control") ? 1u : 0u);
    log("nvn/vk: portability: format reinterpretation %u, format swizzle %u", fport.fields[2], fport.fields[3]);
    log_device_caps(g_physical, dext);  // nvn_caps.cpp
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = g_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(dexts.size());
    dci.ppEnabledExtensionNames = dexts.data();
    dci.pNext = &features;
    if (!check(p_create_device(g_physical, &dci, nullptr, &g_device), "vkCreateDevice")) return false;
    p_vkGetDeviceQueue(g_device, g_family, 0, &g_queue);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = g_family;
    p_vkCreateCommandPool(g_device, &pci, nullptr, &g_pool);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = g_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    p_vkAllocateCommandBuffers(g_device, &cai, &g_cb);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    p_vkCreateFence(g_device, &fci, nullptr, &g_fence);
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    p_vkCreateSemaphore(g_device, &sci, nullptr, &g_acquired);
    p_vkCreateSemaphore(g_device, &sci, nullptr, &g_rendered);

    // A busy submit reached 67 MB (uniforms padded to 4 KB, storage to 64 KB). Android: 128 MB, half, for an 8 GB
    // phone (docs/nvn/android-perf/MEMORY.md: the phone never filled it; big uploads have staging buffers, #72).
#if defined(__ANDROID__)
    make_upload_buffer(128ull << 20);
#else
    make_upload_buffer(256ull << 20);
#endif
    if (async_gpu())
        frame_init(g_pool, g_family, g_upload, g_upload_ptr, g_upload_size);  // the upload buffer: the slots' ring
    else
        g_descriptors = make_descriptor_pool();
    log("nvn/vk: %s, Vulkan %u.%u, queue family %u", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
        VK_API_VERSION_MINOR(props.apiVersion), g_family);
    g_failed = false;
    g_ready = true;
    return true;
}

// The window's surface, and the swapchain format from what the surface offers: B8G8R8A8_UNORM (the Mac), else
// R8G8B8A8_UNORM (Android surfaces offer no BGRA: a swapchain made with it showed nothing on a Galaxy S23), else the
// first. A new Android window (the app back from home or the lock screen) needs a new surface (gpu_present).
void* g_window = nullptr;
uint64_t g_window_gen = 0;
std::atomic<int> g_vsync_wanted{1};  // set_vsync; the swapchain is re-made when it differs from g_vsync_made
int g_vsync_made = -1;

int vsync_mode() {
    static const int forced = [] {
        const char* e = std::getenv("NX_VSYNC");
        if (!e) return std::getenv("NX_FIFO") ? 0 : -1;  // NX_FIFO=1: FIFO, as before
        const std::string v = e;
        return v == "on" ? 0 : v == "fast" ? 1 : v == "adaptive" ? 2 : v == "off" ? 3 : -1;
    }();
    return forced >= 0 ? forced : g_vsync_wanted.load(std::memory_order_relaxed);
}
bool make_surface() {
    if (g_surface) p_vkDestroySurfaceKHR(g_instance, g_surface, nullptr);
    if (g_window) plat::release_window(g_window);
    g_surface = VK_NULL_HANDLE;
    g_window = plat::create_window();  // waits for a window (Android); the generation is read once it is there
    g_window_gen = plat::window_generation();
    uint64_t surface = 0;
    if (!check(VkResult(plat::vulkan_create_surface(g_instance, g_window, &surface, g_gipa)), "surface")) return false;
    g_surface = reinterpret_cast<VkSurfaceKHR>(surface);
    uint32_t n = 0;
    p_vkGetPhysicalDeviceSurfaceFormatsKHR(g_physical, g_surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    p_vkGetPhysicalDeviceSurfaceFormatsKHR(g_physical, g_surface, &n, formats.data());
    auto offered = [&](VkFormat f) {
        for (const VkSurfaceFormatKHR& x : formats)
            if (x.format == f) return true;
        return false;
    };
    g_sc_format = offered(VK_FORMAT_B8G8R8A8_UNORM)   ? VK_FORMAT_B8G8R8A8_UNORM
                  : offered(VK_FORMAT_R8G8B8A8_UNORM) ? VK_FORMAT_R8G8B8A8_UNORM
                  : n                                  ? formats[0].format
                                                       : VK_FORMAT_B8G8R8A8_UNORM;
    log("nvn/vk: surface: %u formats offered, swapchain format %d", n, int(g_sc_format));
    return true;
}

bool make_swapchain() {
    if (g_swapchain) {
        if (async_gpu()) frame_wait_all();
        p_vkDeviceWaitIdle(g_device);
        present_pass_release();
        overlay::release();
        fsr_release();
        fxaa_release();
        p_vkDestroySwapchainKHR(g_device, g_swapchain, nullptr);
        g_swapchain = VK_NULL_HANDLE;
    }
    VkSurfaceCapabilitiesKHR caps{};
    p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_physical, g_surface, &caps);
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        uint32_t w = 0, h = 0;
        plat::window_size(w, h);
        extent = {w ? w : 1280, h ? h : 720};
    }
    if (!extent.width || !extent.height) return false;  // minimized
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = g_surface;
    ci.minImageCount = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    ci.imageFormat = g_sc_format;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    // NX_DUMP_PRESENTED=1 (tests): frame dumps read the swapchain image itself, the overlay included
    if (dump_presented() && (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    // The present pass (nvn_present.cpp) draws the picture pre-rotated: the images are in the display's own
    // orientation (portrait on a phone held sideways). Without it the images stay upright (the Mac: identity).
    bool sideways = caps.currentTransform & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR);
    bool pass = present_pass_wanted() && !present_pass_failed();
    g_sc_current = caps.currentExtent;
    if (pass && sideways && extent.width > extent.height) std::swap(extent.width, extent.height);
    if (!pass && (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR))
        ci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    // Some drivers report maxImageExtent in the other orientation: the larger side bounds both.
    uint32_t most = std::max(caps.maxImageExtent.width, caps.maxImageExtent.height);
    if (most) extent.width = std::min(extent.width, most), extent.height = std::min(extent.height, most);
    ci.imageExtent = extent;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & ci.compositeAlpha))
        for (VkCompositeAlphaFlagBitsKHR a : {VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                                              VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR})
            if (caps.supportedCompositeAlpha & a) {
                ci.compositeAlpha = a;
                break;
            }
    log("nvn/vk: surface %ux%u (current %ux%u), transform %u of %#x, composite alpha %#x of %#x, usage %#x",
        extent.width, extent.height, caps.currentExtent.width, caps.currentExtent.height, unsigned(caps.currentTransform),
        caps.supportedTransforms, unsigned(ci.compositeAlpha), caps.supportedCompositeAlpha,
        caps.supportedUsageFlags);
    // The game paces itself on its vsync event (hle_vi.cpp, 60 Hz): a FIFO present also waited for the display's own
    // refresh, two clocks drifting against each other (stutter near 60 fps, docs/nvn/DEBUG.md #33). MAILBOX never
    // blocks and shows the newest frame at each refresh; else IMMEDIATE (MoltenVK: no display sync in the Metal
    // layer, the macOS compositor still shows whole frames at its refresh), FIFO last. NX_FIFO=1: FIFO (DEBUG.md #62).
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;  // always offered
    uint32_t modes_n = 0;
    p_vkGetPhysicalDeviceSurfacePresentModesKHR(g_physical, g_surface, &modes_n, nullptr);
    std::vector<VkPresentModeKHR> modes(modes_n);
    p_vkGetPhysicalDeviceSurfacePresentModesKHR(g_physical, g_surface, &modes_n, modes.data());
    auto has = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
    // The player's VSync (set_vsync): On FIFO; Fast MAILBOX, else IMMEDIATE (the default, as before); Adaptive
    // FIFO_RELAXED, else FIFO; Off IMMEDIATE, else MAILBOX. Whatever is missing falls back to FIFO.
    const int vsync = vsync_mode();
    if (vsync == 1) {
        if (has(VK_PRESENT_MODE_MAILBOX_KHR))
            ci.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
        else if (has(VK_PRESENT_MODE_IMMEDIATE_KHR))
            ci.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    } else if (vsync == 2) {
        if (has(VK_PRESENT_MODE_FIFO_RELAXED_KHR)) ci.presentMode = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    } else if (vsync == 3) {
        if (has(VK_PRESENT_MODE_IMMEDIATE_KHR))
            ci.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        else if (has(VK_PRESENT_MODE_MAILBOX_KHR))
            ci.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
    }
    g_vsync_made = vsync;
    static const char* const mode_names[] = {"IMMEDIATE", "MAILBOX", "FIFO", "FIFO_RELAXED"};
    log("nvn/vk: present mode %s (%u modes offered)", ci.presentMode <= 3 ? mode_names[ci.presentMode] : "?", modes_n);
    ci.clipped = VK_TRUE;
    if (!check(p_vkCreateSwapchainKHR(g_device, &ci, nullptr, &g_swapchain), "vkCreateSwapchainKHR")) return false;
    g_sc_usage = ci.imageUsage;
    uint32_t n = 0;
    p_vkGetSwapchainImagesKHR(g_device, g_swapchain, &n, nullptr);
    g_sc_images.resize(n);
    p_vkGetSwapchainImagesKHR(g_device, g_swapchain, &n, g_sc_images.data());
    g_sc_extent = extent;
    g_sc_transform = ci.preTransform;
    if (n > 8) log("nvn/vk: %u swapchain images: more than the 8 present semaphores (nvn_frames.cpp)", n);
    bool drawn = pass && present_pass_targets(g_sc_format, g_sc_images, extent, ci.preTransform);
    if (pass && !drawn && present_pass_failed()) {  // the pass is out: an upright swapchain for the blit
        log("nvn/vk: present pass failed: swapchain made again for the blit");
        return make_swapchain();
    }
    log("nvn/vk: swapchain %ux%u, %u images, transform %u, %s", extent.width, extent.height, n,
        unsigned(ci.preTransform), drawn ? "drawn by the present pass" : "blitted");
    return true;
}

}  // namespace

void set_vsync(int mode) { g_vsync_wanted.store(mode < 0 || mode > 3 ? 1 : mode); }

void flush_pipeline_cache() {
    std::lock_guard<std::recursive_mutex> lock(g_gpu);
    if (!g_device) return;
    // The GPU lock prevents concurrent pipeline creation while Vulkan serializes the cache.
    pipeline_cache_save(true);
    save_pipeline_keys(true);
}

std::recursive_mutex& gpu_lock() { return g_gpu; }
VkDevice vk_device() { return g_device; }
VkInstance vk_instance() { return g_instance; }
void* vk_gipa() { return g_gipa; }
VkPhysicalDevice vk_physical() { return g_physical; }
VkQueue vk_queue() { return g_queue; }
uint32_t vk_queue_family() { return g_family; }
bool vk_depth_clip_control() { return g_depth_clip_control; }
bool vk_gpl() { return g_gpl; }
bool vk_pipeline_cache_control() { return g_cache_control; }
const VkPhysicalDeviceFeatures& vk_features() { return g_features; }

uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t k = 0; k < g_memory.memoryTypeCount; k++)
        if ((bits >> k) & 1 && (g_memory.memoryTypes[k].propertyFlags & want) == want) return k;
    return 0;
}

Upload upload_alloc(uint64_t size, uint64_t align) {
    if (async_gpu()) return frame_upload_alloc(size, align);
    uint64_t at = (g_upload_used + align - 1) & ~(align - 1);
    if (at + size > g_upload_size) {
        log("nvn/vk: upload buffer full (%llu + %llu bytes)", (unsigned long long)at, (unsigned long long)size);
        at = 0;  // wraps: this submit's earlier data may be overwritten (grow the buffer when this shows)
        replay::dedup_reset();  // shared copies may be among them
    }
    g_upload_used = at + size;
    return {g_upload, at, g_upload_ptr + at};
}

Upload upload(const void* data, uint64_t size, uint64_t align) {
    Upload u = upload_alloc(size, align);
    if (data && size) std::memcpy(u.ptr, data, size);
    return u;
}

VkDescriptorSet descriptor_set(VkDescriptorSetLayout layout) {
    if (async_gpu()) return frame_descriptor_set(layout);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = g_descriptors;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (p_vkAllocateDescriptorSets(g_device, &ai, &set) != VK_SUCCESS) log("nvn/vk: descriptor pool exhausted");
    return set;
}

VkDescriptorPool make_descriptor_pool() {
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 65536}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16384},
                                    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 65536}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4096},
                                    {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1024}, {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1024},
                                    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 65536}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 32768;
    dpi.poolSizeCount = uint32_t(sizeof sizes / sizeof sizes[0]);
    dpi.pPoolSizes = sizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    p_vkCreateDescriptorPool(g_device, &dpi, nullptr, &pool);
    return pool;
}

namespace {

void make_upload_buffer(uint64_t size) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = kUploadUsage;
    p_vkCreateBuffer(g_device, &bi, nullptr, &g_upload);
    VkMemoryRequirements req{};
    p_vkGetBufferMemoryRequirements(g_device, g_upload, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = upload_memory_type(req.memoryTypeBits);  // nvn_caps.cpp (NX_RING_CACHED)
    p_vkAllocateMemory(g_device, &ai, nullptr, &g_upload_memory);
    p_vkBindBufferMemory(g_device, g_upload, g_upload_memory, 0);
    void* p = nullptr;
    p_vkMapMemory(g_device, g_upload_memory, 0, VK_WHOLE_SIZE, 0, &p);
    g_upload_ptr = static_cast<uint8_t*>(p);
    g_upload_size = size;
}

bool begin(bool replay = false) {
    if (async_gpu()) return (g_cb = frame_begin(replay)) != VK_NULL_HANDLE;
    p_vkResetCommandBuffer(g_cb, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return check(p_vkBeginCommandBuffer(g_cb, &bi), "vkBeginCommandBuffer");
}

// Where the submitting thread's time goes, logged with the draw stats, per presented frame: the replay (recording
// into Vulkan), vkQueueSubmit (Metal encoding happens inside it), fences waited for, the rest of gpu_submit (object
// drain, image frees, write-backs), and gpu_present as a whole; bytes through the upload buffer.
struct Timing {
    uint64_t submits = 0, replay_ns = 0, encode_ns = 0, fence_ns = 0, submit_ns = 0, present_ns = 0, upload_bytes = 0;
} g_timing;

void end_and_wait(VkSemaphore wait, VkSemaphore signal) {
    uint64_t t0 = plat::monotonic_ns();
    p_vkEndCommandBuffer(g_cb);
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_cb;
    if (wait) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &wait;
        si.pWaitDstStageMask = &stage;
    }
    if (signal) {
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &signal;
    }
    p_vkResetFences(g_device, 1, &g_fence);
    bool queued = check(p_vkQueueSubmit(g_queue, 1, &si, g_fence), "vkQueueSubmit");
    uint64_t t1 = plat::monotonic_ns();
    // 2 s at most: a frame's work takes milliseconds; longer is a shader that does not end.
    if (queued) {
        VkResult w = p_vkWaitForFences(g_device, 1, &g_fence, VK_TRUE, 2'000'000'000ull);
        if (w == VK_TIMEOUT) gpu_hung("a submit did not finish in 2 s");
        check(w, "vkWaitForFences");
    }
    frame_times().encode_ns += t1 - t0, frame_times().fence_ns += plat::monotonic_ns() - t1;
}

void log_timing(uint64_t frames) {
    auto ms = [&](uint64_t ns) { return double(ns) / 1e6 / double(frames); };
    static uint64_t last = 0;  // wall clock: the real frame rate
    uint64_t now = plat::monotonic_ns();
    double fps = last ? double(frames) * 1e9 / double(now - last) : 0.0;
    last = now;
    const Timing& t = g_timing;
    uint64_t known = t.replay_ns + t.encode_ns + t.fence_ns;
    log("nvn/vk: per frame over %llu: %.1f fps, %.1f submits, replay %.2f ms, submit (encode) %.2f ms, fence wait "
        "%.2f ms, other %.2f ms, present %.2f ms, upload %.2f MB",
        (unsigned long long)frames, fps, double(t.submits) / double(frames), ms(t.replay_ns), ms(t.encode_ns),
        ms(t.fence_ns), ms(t.submit_ns > known ? t.submit_ns - known : 0), ms(t.present_ns),
        double(t.upload_bytes) / 1e6 / double(frames));
    frame_log(frames);
    creation_log(frames);  // pipelines and ASTC decodes of the window; saves the pipeline cache now and then
    g_timing = {};
}

// gpu_present's time, whatever way it returns.
struct PresentTimer {
    uint64_t t0 = plat::monotonic_ns();
    ~PresentTimer() { g_timing.present_ns += plat::monotonic_ns() - t0; }
};

}  // namespace

uint64_t draw_take_dump_request();          // nvn_draw.cpp

namespace {
std::shared_ptr<const DeferredValues> g_deferred;  // the submit being replayed (replay thread only)
}
const DeferredValues* current_deferred() { return g_deferred.get(); }

void gpu_submit(const std::vector<std::shared_ptr<CmdList>>& lists, std::shared_ptr<const DeferredValues> deferred) {
    g_deferred = std::move(deferred);
    std::lock_guard lk(g_gpu);
    uint64_t t_in = plat::monotonic_ns();
    const FrameTimes f_in = frame_times();
    uint64_t lists_epoch = 0;
    for (auto& l : lists) lists_epoch = std::max(lists_epoch, l->epoch);
    ReplayScope replay(lists_epoch);  // the texture mark (#55, #57), then drain_objects (under the GPU lock, #50)
    if (!init()) return;
    if (!g_shader_prepare_checked) {
        g_shader_prepare_checked = true;
        prepare_shader_catalog();
        prepare_recorded_pipelines();
    }
    bool async = async_gpu();
    if (!async) {  // the previous submit was waited for: all of it is free
        free_retired_images(UINT64_MAX);
        p_vkResetDescriptorPool(g_device, g_descriptors, 0);
        g_upload_used = 0;
    } else if (pipelined()) {  // what the retired submits were the last to use (they free nothing themselves)
        free_retired_images(frame_completed());
    }
    if (!begin(true)) return;
    draw_begin_submit();
    gpu_profile_begin(g_cb);
    zero_draws_begin_submit(g_cb);
    // Pipelined: the part up to the last dispatch is queued as soon as it is recorded (nvn_frames.cpp). Not with the
    // profile or the zero-draw queries: they span the submit, which is then waited for whole.
    size_t split_after = gpu_profile_on() || zero_draws_on() ? SIZE_MAX : frame_plan_split(lists);
    uint64_t t0 = plat::monotonic_ns();
    const FrameTimes f0 = frame_times();
    Targets st;
    for (size_t k = 0; k < lists.size(); k++) {
        replay_list(g_cb, st, *lists[k], 0);
        // After the last list with a dispatch, unless nothing follows it (replay_list may have split inside a list).
        g_cb = k == split_after && k + 1 < lists.size() ? frame_split(g_cb) : frame_cb(g_cb);
    }
    draw_end_rendering(g_cb);
    const FrameTimes f1 = frame_times();  // the first part's submit and ring waits are not replay time
    g_timing.submits++;
    g_timing.replay_ns += plat::monotonic_ns() - t0 - (f1.encode_ns - f0.encode_ns) - (f1.fence_ns - f0.fence_ns);
    g_timing.upload_bytes += async ? frame_upload_used() : g_upload_used;
    gpu_profile_mark(g_cb, "end");
    if (async)  // the profile, zero-draw queries and snapshots read this submit's results right below: waited for
        frame_submit(VK_NULL_HANDLE, VK_NULL_HANDLE, draw_take_write_backs(),
                     gpu_profile_on() || zero_draws_on() || debug_snapshots_pending());
    else
        end_and_wait(VK_NULL_HANDLE, VK_NULL_HANDLE);
    draw_end_submit();  // not waited for: snapshots only (the write-backs went with the submit)
    gpu_profile_collect(), zero_draws_collect();
    debug_dump_requested(draw_take_dump_request());  // reads back in a submit of its own, which it waits for
    const FrameTimes f2 = frame_times();
    g_timing.encode_ns += f2.encode_ns - f_in.encode_ns, g_timing.fence_ns += f2.fence_ns - f_in.fence_ns;
    g_timing.submit_ns += plat::monotonic_ns() - t_in;
}

// Headless: the presented texture is only counted, logged and dumped (no swapchain). Nothing is submitted unless a
// frame dump is due: the empty submit was a Metal command buffer and a fence wait per frame.
void present_headless(uint64_t texture) {
    GpuImage* src = image_of(texture);  // made here if new, as before (the targets rendered into it share it, #44)
    static uint64_t presents = 0;
    presents++;
    if (presents % (presents < 300 ? 60 : 300) == 0) draw_log_stats();
    if (presents % 60 == 0) log_timing(60), gpu_profile_log(60);
    if (presents % 300 == 0) zero_draws_log();
    if (!frame_dump_due(presents)) return;
    if (src && src->layout != VK_IMAGE_LAYOUT_UNDEFINED && begin()) {
        GpuImage* shown = src;  // NX_PRESENT_TEST: what the present pass would put on a display (nvn_present.cpp)
        if (GpuImage* test = present_pass_test_target(); test && present_pass_record(g_cb, *src, nullptr, 0))
            test->layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, shown = test;
        transition(g_cb, *shown, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        bool dump = frame_dump_record(g_cb, *shown);
        if (async_gpu())
            frame_submit(VK_NULL_HANDLE, VK_NULL_HANDLE, {}, dump);  // a dumped frame is waited for
        else
            end_and_wait(VK_NULL_HANDLE, VK_NULL_HANDLE);
        if (dump) frame_dump_write(*shown, presents);
    }
    debug_dump_textures(presents);
}

// The startup shader preparation (nvn_shader_prepare.cpp, nvn_pipeline.cpp) runs inside the first gpu_submit or
// gpu_present, under the GPU lock, before the game has drawn anything. Its progress goes to the platform (the Win32
// window's painted screen when the ImGui overlay is off) and to the overlay's compile screen, which is presented here
// on its own: a cleared swapchain image with the overlay on top, at most 30 times a second.
// A frame of the overlay alone: a cleared swapchain image with the overlay on top (the compile screen, the launcher).
// The caller holds the GPU lock.
static void present_overlay_frame() {
    static const bool headless_present = std::getenv("NX_HEADLESS_PRESENT") != nullptr;
    if ((plat::headless() && !headless_present) || plat::window_minimized()) return;
    if (!g_swapchain && !make_swapchain()) return;
    uint32_t index = 0;
    VkSemaphore acquired = async_gpu() ? frame_acquire_semaphore() : g_acquired;
    VkResult r = p_vkAcquireNextImageKHR(g_device, g_swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        make_swapchain();
        return;
    }
    if (!check(r, "vkAcquireNextImageKHR (compile screen)") || !begin()) return;
    GpuImage dst;
    dst.image = g_sc_images[index];
    dst.format = g_sc_format;
    dst.width = g_sc_extent.width, dst.height = g_sc_extent.height;
    transition(g_cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue v{{0.035f, 0.024f, 0.07f, 1.0f}};
    VkImageSubresourceRange rr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    p_vkCmdClearColorImage(g_cb, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &rr);
    transition(g_cb, dst, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    overlay::record(g_cb, index, g_sc_images, g_sc_format, g_sc_extent);
    // NX_DUMP_PRESENTED with NX_DUMP_PREP=<n>: every nth compile-screen frame to frames/9NNNNN.ppm (tests)
    static const uint64_t dump_every = [] {
        const char* e = std::getenv("NX_DUMP_PREP");
        return e ? std::strtoull(e, nullptr, 10) : 0ull;
    }();
    static uint64_t shown = 0;
    shown++;
    const bool dump = dump_every && dump_presented() && (g_sc_usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) &&
                      shown % dump_every == 0;
    if (dump) {
        dst.layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        transition(g_cb, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        frame_dump_record(g_cb, dst);
        transition(g_cb, dst, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }
    static const bool shared_sem = std::getenv("NX_SHARED_PRESENT_SEMAPHORE") != nullptr;
    VkSemaphore rendered = async_gpu() || !shared_sem ? frame_rendered_semaphore(index) : g_rendered;
    if (async_gpu())
        frame_submit(acquired, rendered, {}, dump);
    else
        end_and_wait(acquired, rendered);
    if (dump) frame_dump_write(dst, 900000 + shown);
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &rendered;
    pi.swapchainCount = 1;
    pi.pSwapchains = &g_swapchain;
    pi.pImageIndices = &index;
    r = p_vkQueuePresentKHR(g_queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) make_swapchain();
}

void prep_progress(uint64_t done, uint64_t total, bool visible, int kind) {
    plat::shader_compile_progress(done, total, visible);
    overlay::shader_progress(done, total, visible, kind);
    if (!visible || !overlay::enabled() || !g_device) return;
    static uint64_t last = 0;
    const uint64_t now = plat::monotonic_ns();
    if (last && now - last < 33'000'000ull) return;
    last = now;
    present_overlay_frame();
}

bool present_ui_frame() {
    std::lock_guard lk(g_gpu);
    drain_objects();
    if (!overlay::enabled() || !init()) return false;
    present_overlay_frame();
    return true;
}

void gpu_present(uint64_t texture, const int32_t* crop) {
    plat::perf_hint_thread();  // the thread that presents takes part in the frame (perf_hint.cpp)
    std::lock_guard lk(g_gpu);
    PresentTimer timer;
    drain_objects();  // under the GPU lock, as in gpu_submit
    if (!init()) return;
    if (!g_shader_prepare_checked) {
        g_shader_prepare_checked = true;
        prepare_shader_catalog();
        prepare_recorded_pipelines();
    }
    if (!async_gpu() || pipelined()) free_retired_images(async_gpu() ? frame_completed() : UINT64_MAX);
    if (plat::headless()) return present_headless(texture);
    if (plat::window_minimized()) return;
    if (plat::window_generation() != g_window_gen) {  // Android: a new window after home or the lock screen
        if (async_gpu()) frame_wait_all();
        p_vkDeviceWaitIdle(g_device);
        present_pass_release();
        overlay::release();
        fsr_release();
        fxaa_release();
        if (g_swapchain) p_vkDestroySwapchainKHR(g_device, g_swapchain, nullptr);
        g_swapchain = VK_NULL_HANDLE;
        log("nvn/vk: new window: surface and swapchain re-made");
        if (!make_surface()) return;
    }
    if (g_swapchain && vsync_mode() != g_vsync_made) {  // the player changed VSync: a swapchain with the new mode
        if (async_gpu()) frame_wait_all();
        p_vkDeviceWaitIdle(g_device);
        present_pass_release();
        overlay::release();
        fsr_release();
        fxaa_release();
        p_vkDestroySwapchainKHR(g_device, g_swapchain, nullptr);
        g_swapchain = VK_NULL_HANDLE;
        log("nvn/vk: VSync changed: swapchain re-made");
    }
    if (!g_swapchain && !make_swapchain()) return;
    uint32_t index = 0;
    VkSemaphore acquired = async_gpu() ? frame_acquire_semaphore() : g_acquired;
    VkResult r = p_vkAcquireNextImageKHR(g_device, g_swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        make_swapchain();
        return;
    }
    if (!check(r, "vkAcquireNextImageKHR") || !begin()) return;
    GpuImage* src = image_of(texture);
    GpuImage dst;
    dst.image = g_sc_images[index];
    dst.format = g_sc_format;
    if (src) {
        if (src->layout == VK_IMAGE_LAYOUT_UNDEFINED) {  // never rendered: dark grey, to tell it from a failure
            transition(g_cb, *src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkClearColorValue v{{0.08f, 0.08f, 0.1f, 1.0f}};
            VkImageSubresourceRange rr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            p_vkCmdClearColorImage(g_cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &rr);
        }
    }
    bool drawn = src && present_pass_record(g_cb, *src, crop, index);  // nvn_present.cpp: leaves dst PRESENT_SRC
    // FSR 1 when the image is smaller than the window (nvn_fsr.cpp): leaves dst TRANSFER_DST, as the blit does.
    // FXAA first when on (nvn_fxaa.cpp): its output stands in for the game's image from here.
    GpuImage* in = src && !drawn ? fxaa_record(g_cb, *src) : nullptr;
    if (!in) in = src;
    const bool upscaled = in && !drawn && fsr_record(g_cb, *in, dst, g_sc_extent, crop);
    if (in && !drawn && !upscaled) transition(g_cb, *in, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    if (!drawn) transition(g_cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (in && !drawn && !upscaled) {
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {int32_t(in->width), int32_t(in->height), 1};
        // nvnWindowSetCrop is in guest pixels; convert its rectangle to the upscaled Vulkan image.
        const uint32_t logical_width = in->logical_width ? in->logical_width : in->width;
        const uint32_t logical_height = in->logical_height ? in->logical_height : in->height;
        if (crop && crop[2] > 0 && crop[3] > 0 && crop[0] + crop[2] <= int32_t(logical_width) &&
            crop[1] + crop[3] <= int32_t(logical_height)) {
            const float scale_x = float(in->width) / float(logical_width);
            const float scale_y = float(in->height) / float(logical_height);
            const int32_t x0 = int32_t(std::lround(crop[0] * scale_x));
            const int32_t y0 = int32_t(std::lround(crop[1] * scale_y));
            const int32_t x1 = int32_t(std::lround((crop[0] + crop[2]) * scale_x));
            const int32_t y1 = int32_t(std::lround((crop[1] + crop[3]) * scale_y));
            blit.srcOffsets[0] = {x0, y0, 0}, blit.srcOffsets[1] = {x1, y1, 1};
        }
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[1] = {int32_t(g_sc_extent.width), int32_t(g_sc_extent.height), 1};
        p_vkCmdBlitImage(g_cb, in->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    }
    if (!drawn) transition(g_cb, dst, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    overlay::record(g_cb, index, g_sc_images, g_sc_format, g_sc_extent);  // the F1 menu and metrics (overlay/)
    static uint64_t presents = 0;
    presents++;
    if (presents % (presents < 300 ? 60 : 300) == 0) draw_log_stats();
    if (presents % 60 == 0) log_timing(60), gpu_profile_log(60);
    if (presents % 300 == 0) zero_draws_log();
    frame_sig_flush(presents);
    const bool due = frame_dump_due(presents);
    GpuImage* shown = src;  // NX_DUMP_FSR: the image as presented (after FXAA and FSR 1)
    if (std::getenv("NX_DUMP_FSR")) shown = upscaled && fsr_output() ? fsr_output() : in;
    // NX_DUMP_PRESENTED: the swapchain image as it goes to the display (after the overlay), back to PRESENT_SRC after
    // the copy. Needs the TRANSFER_SRC usage make_swapchain asked for.
    GpuImage presented = dst;
    if (dump_presented() && (g_sc_usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        presented.width = g_sc_extent.width, presented.height = g_sc_extent.height;
        presented.layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        shown = &presented;
    }
    bool dump = shown && due;
    if (dump) transition(g_cb, *shown, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);  // the present pass left it sampled
    dump = dump && frame_dump_record(g_cb, *shown);
    if (shown == &presented && due) transition(g_cb, presented, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    // One per swapchain image on both paths: a single semaphore could be signaled again while an earlier present
    // still waited on it (VUID-vkQueueSubmit-pSignalSemaphores-00067). NX_SHARED_PRESENT_SEMAPHORE=1: as before.
    static const bool shared_sem = std::getenv("NX_SHARED_PRESENT_SEMAPHORE") != nullptr;
    VkSemaphore rendered = async_gpu() || !shared_sem ? frame_rendered_semaphore(index) : g_rendered;
    if (async_gpu())  // the blit's fence is not waited for (vkQueuePresentKHR waits on `rendered`); a dumped frame is
        frame_submit(acquired, rendered, {}, dump);
    else
        end_and_wait(acquired, rendered);
    if (dump) frame_dump_write(*shown, presents);
    if (due) debug_dump_textures(presents);
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &rendered;
    pi.swapchainCount = 1;
    pi.pSwapchains = &g_swapchain;
    pi.pImageIndices = &index;
    r = p_vkQueuePresentKHR(g_queue, &pi);
    plat::frame_presented();  // cores, CPU time, present intervals (frame_stats.cpp)
    // SUBOPTIMAL re-makes the swapchain only when the surface size changed: Android returns it for every present when
    // the swapchain's transform is not the display's (a Galaxy S23 did for Hades), a rebuild per frame.
    bool remake = r == VK_ERROR_OUT_OF_DATE_KHR;
    if (r == VK_SUBOPTIMAL_KHR) {  // compared with what the swapchain was made from (its extent may be turned)
        VkSurfaceCapabilitiesKHR caps{};
        p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_physical, g_surface, &caps);
        remake = caps.currentExtent.width != g_sc_current.width || caps.currentExtent.height != g_sc_current.height ||
                 (present_pass_wanted() && !present_pass_failed() && caps.currentTransform != g_sc_transform);
    }
    if (remake) make_swapchain();
}

}  // namespace nx::nvn
