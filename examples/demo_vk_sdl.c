#include "vg.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef VG_DEMO_HAS_POST_SHADERS
#define VG_DEMO_HAS_POST_SHADERS 0
#endif

#if VG_DEMO_HAS_POST_SHADERS
#include "demo_bloom_frag_spv.h"
#include "demo_composite_frag_spv.h"
#include "demo_fullscreen_vert_spv.h"
#endif

#define APP_WIDTH 1280
#define APP_HEIGHT 720
#define APP_MAX_SWAPCHAIN_IMAGES 8

typedef struct post_pc {
    float texel[2];
    float bloom_strength;
    float bloom_radius_px;
    float vignette_strength;
    float barrel_distortion;
    float scanline_strength;
    float noise_strength;
    float time_s;
} post_pc;

typedef enum frame_result {
    FRAME_OK = 0,
    FRAME_RECREATE = 1,
    FRAME_FAIL = 2
} frame_result;

typedef struct app {
    SDL_Window* window;

    VkInstance instance;
    VkSurfaceKHR surface;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue graphics_queue;
    VkQueue present_queue;

    uint32_t graphics_queue_family;
    uint32_t present_queue_family;

    VkSwapchainKHR swapchain;
    VkFormat swapchain_format;
    VkExtent2D swapchain_extent;
    uint32_t swapchain_image_count;
    VkImage swapchain_images[APP_MAX_SWAPCHAIN_IMAGES];
    VkImageView swapchain_image_views[APP_MAX_SWAPCHAIN_IMAGES];

    VkRenderPass present_render_pass;
    VkFramebuffer present_framebuffers[APP_MAX_SWAPCHAIN_IMAGES];

    VkImage scene_image;
    VkDeviceMemory scene_memory;
    VkImageView scene_view;
    VkFramebuffer scene_fb;
    VkRenderPass scene_render_pass;
    int scene_initialized;

    VkImage bloom_image;
    VkDeviceMemory bloom_memory;
    VkImageView bloom_view;
    VkFramebuffer bloom_fb;
    VkRenderPass bloom_render_pass;

    VkSampler post_sampler;
    VkDescriptorSetLayout post_desc_layout;
    VkDescriptorPool post_desc_pool;
    VkDescriptorSet post_desc_set;
    VkPipelineLayout post_layout;
    VkPipeline bloom_pipeline;
    VkPipeline composite_pipeline;

    VkCommandPool command_pool;
    VkCommandBuffer command_buffers[APP_MAX_SWAPCHAIN_IMAGES];

    VkSemaphore image_available;
    VkSemaphore render_finished;
    VkFence in_flight;

    vg_context* vg;
    vg_path* wave_path;

    int show_ui;
    int selected_param;
    float main_line_width;
    float fps_smoothed;
    int prev_adjust_dir;
    int prev_nav_dir;
    float adjust_repeat_timer;
    float nav_repeat_timer;
} app;

enum {
    UI_PARAM_BLOOM_STRENGTH = 0,
    UI_PARAM_BLOOM_RADIUS = 1,
    UI_PARAM_PERSISTENCE = 2,
    UI_PARAM_JITTER = 3,
    UI_PARAM_FLICKER = 4,
    UI_PARAM_BEAM_CORE = 5,
    UI_PARAM_BEAM_HALO = 6,
    UI_PARAM_BEAM_INTENSITY = 7,
    UI_PARAM_VIGNETTE = 8,
    UI_PARAM_BARREL = 9,
    UI_PARAM_SCANLINE = 10,
    UI_PARAM_NOISE = 11,
    UI_PARAM_LINE_WIDTH = 12,
    UI_PARAM_COUNT = 13
};

static int check_vk(VkResult res, const char* what) {
    if (res != VK_SUCCESS) {
        fprintf(stderr, "%s failed (VkResult=%d)\n", what, (int)res);
        return 0;
    }
    return 1;
}

static uint32_t hash_u32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static float rand_signed(uint32_t seed) {
    uint32_t h = hash_u32(seed);
    float t = (float)(h & 0x00ffffffu) / 8388607.5f;
    return t - 1.0f;
}

static float clampf(float v, float lo, float hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static float norm_range(float v, float lo, float hi) {
    if (hi <= lo) {
        return 0.0f;
    }
    return clampf((v - lo) / (hi - lo), 0.0f, 1.0f);
}

static uint32_t find_memory_type(app* a, uint32_t type_bits, VkMemoryPropertyFlags required) {
    VkPhysicalDeviceMemoryProperties props = {0};
    vkGetPhysicalDeviceMemoryProperties(a->physical_device, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) && ((props.memoryTypes[i].propertyFlags & required) == required)) {
            return i;
        }
    }
    return UINT32_MAX;
}

static int create_image_2d(
    app* a,
    uint32_t w,
    uint32_t h,
    VkFormat format,
    VkImageUsageFlags usage,
    VkImage* out_image,
    VkDeviceMemory* out_mem,
    VkImageView* out_view
) {
    VkImageCreateInfo img = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {w, h, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };
    if (!check_vk(vkCreateImage(a->device, &img, NULL, out_image), "vkCreateImage")) {
        return 0;
    }

    VkMemoryRequirements req = {0};
    vkGetImageMemoryRequirements(a->device, *out_image, &req);

    uint32_t mem_type = find_memory_type(a, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mem_type == UINT32_MAX) {
        fprintf(stderr, "No device local memory type for image\n");
        return 0;
    }

    VkMemoryAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = mem_type
    };
    if (!check_vk(vkAllocateMemory(a->device, &alloc, NULL, out_mem), "vkAllocateMemory(image)")) {
        return 0;
    }

    if (!check_vk(vkBindImageMemory(a->device, *out_image, *out_mem, 0), "vkBindImageMemory")) {
        return 0;
    }

    VkImageViewCreateInfo view = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = *out_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1
        }
    };
    if (!check_vk(vkCreateImageView(a->device, &view, NULL, out_view), "vkCreateImageView(offscreen)")) {
        return 0;
    }

    return 1;
}

static int create_instance(app* a) {
    unsigned int ext_count = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(a->window, &ext_count, NULL)) {
        fprintf(stderr, "SDL_Vulkan_GetInstanceExtensions(count) failed: %s\n", SDL_GetError());
        return 0;
    }

    const char** exts = (const char**)calloc(ext_count, sizeof(*exts));
    if (!exts) {
        return 0;
    }

    int ok = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(a->window, &ext_count, exts)) {
        fprintf(stderr, "SDL_Vulkan_GetInstanceExtensions(list) failed: %s\n", SDL_GetError());
        goto cleanup;
    }

    VkApplicationInfo app_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "vectorgfx Vulkan SDL demo",
        .applicationVersion = VK_MAKE_VERSION(0, 1, 0),
        .pEngineName = "vectorgfx",
        .engineVersion = VK_MAKE_VERSION(0, 1, 0),
        .apiVersion = VK_API_VERSION_1_1
    };

    VkInstanceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info,
        .enabledExtensionCount = ext_count,
        .ppEnabledExtensionNames = exts
    };

    ok = check_vk(vkCreateInstance(&create_info, NULL, &a->instance), "vkCreateInstance");

cleanup:
    free(exts);
    return ok;
}

static int create_surface(app* a) {
    if (!SDL_Vulkan_CreateSurface(a->window, a->instance, &a->surface)) {
        fprintf(stderr, "SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
        return 0;
    }
    return 1;
}

static int pick_physical_device(app* a) {
    uint32_t count = 0;
    if (!check_vk(vkEnumeratePhysicalDevices(a->instance, &count, NULL), "vkEnumeratePhysicalDevices(count)")) {
        return 0;
    }
    if (count == 0) {
        fprintf(stderr, "No Vulkan physical devices found\n");
        return 0;
    }

    VkPhysicalDevice* devices = (VkPhysicalDevice*)calloc(count, sizeof(*devices));
    if (!devices) {
        return 0;
    }

    int ok = 0;
    if (!check_vk(vkEnumeratePhysicalDevices(a->instance, &count, devices), "vkEnumeratePhysicalDevices(list)")) {
        goto cleanup;
    }

    for (uint32_t d = 0; d < count && !ok; ++d) {
        VkPhysicalDevice dev = devices[d];

        uint32_t qcount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qcount, NULL);
        if (qcount == 0) {
            continue;
        }

        VkQueueFamilyProperties* qprops = (VkQueueFamilyProperties*)calloc(qcount, sizeof(*qprops));
        if (!qprops) {
            continue;
        }
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qcount, qprops);

        int have_graphics = 0;
        int have_present = 0;
        uint32_t gq = 0;
        uint32_t pq = 0;

        for (uint32_t i = 0; i < qcount; ++i) {
            if ((qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && !have_graphics) {
                gq = i;
                have_graphics = 1;
            }
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, a->surface, &present);
            if (present && !have_present) {
                pq = i;
                have_present = 1;
            }
        }

        free(qprops);

        if (have_graphics && have_present) {
            a->physical_device = dev;
            a->graphics_queue_family = gq;
            a->present_queue_family = pq;
            ok = 1;
        }
    }

cleanup:
    free(devices);
    if (!ok) {
        fprintf(stderr, "Failed to find suitable physical device\n");
    }
    return ok;
}

static int create_device(app* a) {
    float priority = 1.0f;

    VkDeviceQueueCreateInfo queue_infos[2] = {0};
    uint32_t queue_info_count = 0;

    queue_infos[queue_info_count].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_infos[queue_info_count].queueFamilyIndex = a->graphics_queue_family;
    queue_infos[queue_info_count].queueCount = 1;
    queue_infos[queue_info_count].pQueuePriorities = &priority;
    queue_info_count++;

    if (a->present_queue_family != a->graphics_queue_family) {
        queue_infos[queue_info_count].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_infos[queue_info_count].queueFamilyIndex = a->present_queue_family;
        queue_infos[queue_info_count].queueCount = 1;
        queue_infos[queue_info_count].pQueuePriorities = &priority;
        queue_info_count++;
    }

    const char* dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    VkDeviceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = queue_info_count,
        .pQueueCreateInfos = queue_infos,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = dev_exts
    };

    if (!check_vk(vkCreateDevice(a->physical_device, &create_info, NULL, &a->device), "vkCreateDevice")) {
        return 0;
    }

    vkGetDeviceQueue(a->device, a->graphics_queue_family, 0, &a->graphics_queue);
    vkGetDeviceQueue(a->device, a->present_queue_family, 0, &a->present_queue);
    return 1;
}

static VkSurfaceFormatKHR choose_surface_format(const VkSurfaceFormatKHR* formats, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM && formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return formats[i];
        }
    }
    return formats[0];
}

static VkPresentModeKHR choose_present_mode(const VkPresentModeKHR* modes, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) {
            return modes[i];
        }
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

static VkExtent2D choose_extent(const VkSurfaceCapabilitiesKHR* caps) {
    if (caps->currentExtent.width != UINT32_MAX) {
        return caps->currentExtent;
    }
    VkExtent2D out = {APP_WIDTH, APP_HEIGHT};
    if (out.width < caps->minImageExtent.width) {
        out.width = caps->minImageExtent.width;
    }
    if (out.height < caps->minImageExtent.height) {
        out.height = caps->minImageExtent.height;
    }
    if (out.width > caps->maxImageExtent.width) {
        out.width = caps->maxImageExtent.width;
    }
    if (out.height > caps->maxImageExtent.height) {
        out.height = caps->maxImageExtent.height;
    }
    return out;
}

static int create_swapchain(app* a) {
    VkSurfaceCapabilitiesKHR caps = {0};
    if (!check_vk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(a->physical_device, a->surface, &caps), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")) {
        return 0;
    }

    uint32_t format_count = 0;
    if (!check_vk(vkGetPhysicalDeviceSurfaceFormatsKHR(a->physical_device, a->surface, &format_count, NULL), "vkGetPhysicalDeviceSurfaceFormatsKHR(count)")) {
        return 0;
    }
    if (format_count == 0) {
        return 0;
    }
    VkSurfaceFormatKHR* formats = (VkSurfaceFormatKHR*)calloc(format_count, sizeof(*formats));
    if (!formats) {
        return 0;
    }
    if (!check_vk(vkGetPhysicalDeviceSurfaceFormatsKHR(a->physical_device, a->surface, &format_count, formats), "vkGetPhysicalDeviceSurfaceFormatsKHR(list)")) {
        free(formats);
        return 0;
    }

    uint32_t mode_count = 0;
    if (!check_vk(vkGetPhysicalDeviceSurfacePresentModesKHR(a->physical_device, a->surface, &mode_count, NULL), "vkGetPhysicalDeviceSurfacePresentModesKHR(count)")) {
        free(formats);
        return 0;
    }
    VkPresentModeKHR* modes = (VkPresentModeKHR*)calloc(mode_count > 0 ? mode_count : 1u, sizeof(*modes));
    if (!modes) {
        free(formats);
        return 0;
    }
    if (mode_count > 0 && !check_vk(vkGetPhysicalDeviceSurfacePresentModesKHR(a->physical_device, a->surface, &mode_count, modes), "vkGetPhysicalDeviceSurfacePresentModesKHR(list)")) {
        free(modes);
        free(formats);
        return 0;
    }

    VkSurfaceFormatKHR fmt = choose_surface_format(formats, format_count);
    VkPresentModeKHR mode = choose_present_mode(modes, mode_count);
    VkExtent2D extent = choose_extent(&caps);

    free(modes);
    free(formats);

    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) {
        image_count = caps.maxImageCount;
    }
    if (image_count > APP_MAX_SWAPCHAIN_IMAGES) {
        image_count = APP_MAX_SWAPCHAIN_IMAGES;
    }

    uint32_t queue_indices[2] = {a->graphics_queue_family, a->present_queue_family};

    VkSwapchainCreateInfoKHR create_info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = a->surface,
        .minImageCount = image_count,
        .imageFormat = fmt.format,
        .imageColorSpace = fmt.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = mode,
        .clipped = VK_TRUE,
        .oldSwapchain = VK_NULL_HANDLE
    };

    if (a->graphics_queue_family != a->present_queue_family) {
        create_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        create_info.queueFamilyIndexCount = 2;
        create_info.pQueueFamilyIndices = queue_indices;
    } else {
        create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    if (!check_vk(vkCreateSwapchainKHR(a->device, &create_info, NULL, &a->swapchain), "vkCreateSwapchainKHR")) {
        return 0;
    }

    a->swapchain_format = fmt.format;
    a->swapchain_extent = extent;

    if (!check_vk(vkGetSwapchainImagesKHR(a->device, a->swapchain, &a->swapchain_image_count, NULL), "vkGetSwapchainImagesKHR(count)")) {
        return 0;
    }
    if (a->swapchain_image_count > APP_MAX_SWAPCHAIN_IMAGES) {
        fprintf(stderr, "swapchain images exceed APP_MAX_SWAPCHAIN_IMAGES\n");
        return 0;
    }
    if (!check_vk(vkGetSwapchainImagesKHR(a->device, a->swapchain, &a->swapchain_image_count, a->swapchain_images), "vkGetSwapchainImagesKHR(list)")) {
        return 0;
    }

    return 1;
}

static int create_swapchain_image_views(app* a) {
    for (uint32_t i = 0; i < a->swapchain_image_count; ++i) {
        VkImageViewCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = a->swapchain_images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = a->swapchain_format,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1
            }
        };
        if (!check_vk(vkCreateImageView(a->device, &info, NULL, &a->swapchain_image_views[i]), "vkCreateImageView(swapchain)")) {
            return 0;
        }
    }
    return 1;
}

static int create_render_passes(app* a) {
    VkAttachmentDescription scene_att = {
        .format = a->swapchain_format,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    };
    VkAttachmentReference scene_ref = {.attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription scene_sub = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &scene_ref
    };
    VkRenderPassCreateInfo scene_rp = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &scene_att,
        .subpassCount = 1,
        .pSubpasses = &scene_sub
    };
    if (!check_vk(vkCreateRenderPass(a->device, &scene_rp, NULL, &a->scene_render_pass), "vkCreateRenderPass(scene)")) {
        return 0;
    }

    VkAttachmentDescription bloom_att = {
        .format = a->swapchain_format,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    };
    VkAttachmentReference bloom_ref = {.attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription bloom_sub = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &bloom_ref
    };
    VkRenderPassCreateInfo bloom_rp = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &bloom_att,
        .subpassCount = 1,
        .pSubpasses = &bloom_sub
    };
    if (!check_vk(vkCreateRenderPass(a->device, &bloom_rp, NULL, &a->bloom_render_pass), "vkCreateRenderPass(bloom)")) {
        return 0;
    }

    VkAttachmentDescription present_att = {
        .format = a->swapchain_format,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
    };
    VkAttachmentReference present_ref = {.attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription present_sub = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &present_ref
    };
    VkRenderPassCreateInfo present_rp = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &present_att,
        .subpassCount = 1,
        .pSubpasses = &present_sub
    };
    if (!check_vk(vkCreateRenderPass(a->device, &present_rp, NULL, &a->present_render_pass), "vkCreateRenderPass(present)")) {
        return 0;
    }

    return 1;
}

static int create_offscreen_targets(app* a) {
    uint32_t w = a->swapchain_extent.width;
    uint32_t h = a->swapchain_extent.height;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    if (!create_image_2d(a, w, h, a->swapchain_format, usage, &a->scene_image, &a->scene_memory, &a->scene_view)) {
        return 0;
    }
    if (!create_image_2d(a, w, h, a->swapchain_format, usage, &a->bloom_image, &a->bloom_memory, &a->bloom_view)) {
        return 0;
    }

    VkImageView scene_att[] = {a->scene_view};
    VkFramebufferCreateInfo scene_fb = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = a->scene_render_pass,
        .attachmentCount = 1,
        .pAttachments = scene_att,
        .width = w,
        .height = h,
        .layers = 1
    };
    if (!check_vk(vkCreateFramebuffer(a->device, &scene_fb, NULL, &a->scene_fb), "vkCreateFramebuffer(scene)")) {
        return 0;
    }

    VkImageView bloom_att[] = {a->bloom_view};
    VkFramebufferCreateInfo bloom_fb = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = a->bloom_render_pass,
        .attachmentCount = 1,
        .pAttachments = bloom_att,
        .width = w,
        .height = h,
        .layers = 1
    };
    if (!check_vk(vkCreateFramebuffer(a->device, &bloom_fb, NULL, &a->bloom_fb), "vkCreateFramebuffer(bloom)")) {
        return 0;
    }

    return 1;
}

static int create_present_framebuffers(app* a) {
    for (uint32_t i = 0; i < a->swapchain_image_count; ++i) {
        VkImageView att[] = {a->swapchain_image_views[i]};
        VkFramebufferCreateInfo fb = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = a->present_render_pass,
            .attachmentCount = 1,
            .pAttachments = att,
            .width = a->swapchain_extent.width,
            .height = a->swapchain_extent.height,
            .layers = 1
        };
        if (!check_vk(vkCreateFramebuffer(a->device, &fb, NULL, &a->present_framebuffers[i]), "vkCreateFramebuffer(present)")) {
            return 0;
        }
    }
    return 1;
}

static int create_command_pool_and_buffers(app* a) {
    VkCommandPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = a->graphics_queue_family
    };
    if (!check_vk(vkCreateCommandPool(a->device, &pool, NULL, &a->command_pool), "vkCreateCommandPool")) {
        return 0;
    }

    VkCommandBufferAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = a->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = a->swapchain_image_count
    };
    return check_vk(vkAllocateCommandBuffers(a->device, &alloc, a->command_buffers), "vkAllocateCommandBuffers");
}

static int create_sync(app* a) {
    VkSemaphoreCreateInfo sem = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};

    if (!check_vk(vkCreateSemaphore(a->device, &sem, NULL, &a->image_available), "vkCreateSemaphore(image_available)")) {
        return 0;
    }
    if (!check_vk(vkCreateSemaphore(a->device, &sem, NULL, &a->render_finished), "vkCreateSemaphore(render_finished)")) {
        return 0;
    }
    return check_vk(vkCreateFence(a->device, &fence, NULL, &a->in_flight), "vkCreateFence");
}

static int create_post_resources(app* a) {
#if !VG_DEMO_HAS_POST_SHADERS
    fprintf(stderr, "Demo post shaders were not generated.\n");
    return 0;
#else
    VkSamplerCreateInfo sampler = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = 1.0f
    };
    if (!check_vk(vkCreateSampler(a->device, &sampler, NULL, &a->post_sampler), "vkCreateSampler")) {
        return 0;
    }

    VkDescriptorSetLayoutBinding bindings[2] = {
        {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT
        },
        {
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT
        }
    };
    VkDescriptorSetLayoutCreateInfo dsl = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings = bindings
    };
    if (!check_vk(vkCreateDescriptorSetLayout(a->device, &dsl, NULL, &a->post_desc_layout), "vkCreateDescriptorSetLayout")) {
        return 0;
    }

    VkDescriptorPoolSize pool_size = {
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 2
    };
    VkDescriptorPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
        .maxSets = 1
    };
    if (!check_vk(vkCreateDescriptorPool(a->device, &pool, NULL, &a->post_desc_pool), "vkCreateDescriptorPool")) {
        return 0;
    }

    VkDescriptorSetAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = a->post_desc_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &a->post_desc_layout
    };
    if (!check_vk(vkAllocateDescriptorSets(a->device, &alloc, &a->post_desc_set), "vkAllocateDescriptorSets")) {
        return 0;
    }

    VkDescriptorImageInfo scene_info = {
        .sampler = a->post_sampler,
        .imageView = a->scene_view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    };
    VkDescriptorImageInfo bloom_info = {
        .sampler = a->post_sampler,
        .imageView = a->bloom_view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    };

    VkWriteDescriptorSet writes[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = a->post_desc_set,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &scene_info
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = a->post_desc_set,
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &bloom_info
        }
    };
    vkUpdateDescriptorSets(a->device, 2, writes, 0, NULL);

    VkPushConstantRange pc = {
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(post_pc)
    };
    VkPipelineLayoutCreateInfo pli = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &a->post_desc_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pc
    };
    if (!check_vk(vkCreatePipelineLayout(a->device, &pli, NULL, &a->post_layout), "vkCreatePipelineLayout(post)")) {
        return 0;
    }

    VkShaderModuleCreateInfo vs_ci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = demo_fullscreen_vert_spv_len,
        .pCode = (const uint32_t*)demo_fullscreen_vert_spv
    };
    VkShaderModuleCreateInfo bloom_ci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = demo_bloom_frag_spv_len,
        .pCode = (const uint32_t*)demo_bloom_frag_spv
    };
    VkShaderModuleCreateInfo comp_ci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = demo_composite_frag_spv_len,
        .pCode = (const uint32_t*)demo_composite_frag_spv
    };

    VkShaderModule vs = VK_NULL_HANDLE;
    VkShaderModule fs_bloom = VK_NULL_HANDLE;
    VkShaderModule fs_comp = VK_NULL_HANDLE;
    if (!check_vk(vkCreateShaderModule(a->device, &vs_ci, NULL, &vs), "vkCreateShaderModule(vs)")) {
        return 0;
    }
    if (!check_vk(vkCreateShaderModule(a->device, &bloom_ci, NULL, &fs_bloom), "vkCreateShaderModule(fs bloom)")) {
        vkDestroyShaderModule(a->device, vs, NULL);
        return 0;
    }
    if (!check_vk(vkCreateShaderModule(a->device, &comp_ci, NULL, &fs_comp), "vkCreateShaderModule(fs comp)")) {
        vkDestroyShaderModule(a->device, fs_bloom, NULL);
        vkDestroyShaderModule(a->device, vs, NULL);
        return 0;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vs,
            .pName = "main"
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = fs_bloom,
            .pName = "main"
        }
    };

    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
    };
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1
    };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .lineWidth = 1.0f,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE
    };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT
    };
    VkPipelineColorBlendAttachmentState cb_att = {
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT
    };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &cb_att
    };
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dyn
    };

    VkGraphicsPipelineCreateInfo gp = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vi,
        .pInputAssemblyState = &ia,
        .pViewportState = &vp,
        .pRasterizationState = &rs,
        .pMultisampleState = &ms,
        .pColorBlendState = &cb,
        .pDynamicState = &ds,
        .layout = a->post_layout,
        .renderPass = a->bloom_render_pass,
        .subpass = 0
    };
    if (!check_vk(vkCreateGraphicsPipelines(a->device, VK_NULL_HANDLE, 1, &gp, NULL, &a->bloom_pipeline), "vkCreateGraphicsPipelines(bloom)")) {
        vkDestroyShaderModule(a->device, fs_comp, NULL);
        vkDestroyShaderModule(a->device, fs_bloom, NULL);
        vkDestroyShaderModule(a->device, vs, NULL);
        return 0;
    }

    stages[1].module = fs_comp;
    cb_att.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    cb_att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cb_att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cb_att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    gp.renderPass = a->present_render_pass;
    if (!check_vk(vkCreateGraphicsPipelines(a->device, VK_NULL_HANDLE, 1, &gp, NULL, &a->composite_pipeline), "vkCreateGraphicsPipelines(composite)")) {
        vkDestroyShaderModule(a->device, fs_comp, NULL);
        vkDestroyShaderModule(a->device, fs_bloom, NULL);
        vkDestroyShaderModule(a->device, vs, NULL);
        return 0;
    }

    vkDestroyShaderModule(a->device, fs_comp, NULL);
    vkDestroyShaderModule(a->device, fs_bloom, NULL);
    vkDestroyShaderModule(a->device, vs, NULL);
    return 1;
#endif
}

static int init_scene_image_layout(app* a) {
    VkCommandBufferAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = a->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (!check_vk(vkAllocateCommandBuffers(a->device, &alloc, &cmd), "vkAllocateCommandBuffers(init)")) {
        return 0;
    }

    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if (!check_vk(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer(init)")) {
        return 0;
    }

    VkImageMemoryBarrier to_transfer = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = a->scene_image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1
        },
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT
    };
    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        0,
        NULL,
        0,
        NULL,
        1,
        &to_transfer
    );

    VkClearColorValue clear = {{0.0f, 0.0f, 0.0f, 1.0f}};
    VkImageSubresourceRange range = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1
    };
    vkCmdClearColorImage(cmd, a->scene_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);

    VkImageMemoryBarrier to_sample = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = a->scene_image,
        .subresourceRange = range,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT
    };
    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0,
        0,
        NULL,
        0,
        NULL,
        1,
        &to_sample
    );

    if (!check_vk(vkEndCommandBuffer(cmd), "vkEndCommandBuffer(init)")) {
        return 0;
    }

    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd
    };
    if (!check_vk(vkQueueSubmit(a->graphics_queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit(init)")) {
        return 0;
    }
    if (!check_vk(vkQueueWaitIdle(a->graphics_queue), "vkQueueWaitIdle(init)")) {
        return 0;
    }

    vkFreeCommandBuffers(a->device, a->command_pool, 1, &cmd);
    a->scene_initialized = 1;
    return 1;
}

static int create_vg_context(app* a) {
    vg_context_desc desc;
    memset(&desc, 0, sizeof(desc));

    desc.backend = VG_BACKEND_VULKAN;
    desc.api.vulkan.instance = (void*)a->instance;
    desc.api.vulkan.physical_device = (void*)a->physical_device;
    desc.api.vulkan.device = (void*)a->device;
    desc.api.vulkan.graphics_queue = (void*)a->graphics_queue;
    desc.api.vulkan.graphics_queue_family = a->graphics_queue_family;
    desc.api.vulkan.render_pass = (void*)a->scene_render_pass;
    desc.api.vulkan.vertex_binding = 0;
    desc.api.vulkan.max_frames_in_flight = 2;

    vg_result r = vg_context_create(&desc, &a->vg);
    if (r != VG_OK) {
        fprintf(stderr, "vg_context_create failed: %s\n", vg_result_string(r));
        return 0;
    }

    r = vg_path_create(a->vg, &a->wave_path);
    if (r != VG_OK) {
        fprintf(stderr, "vg_path_create failed: %s\n", vg_result_string(r));
        return 0;
    }

    vg_crt_profile crt = {0};
    vg_make_crt_profile(VG_CRT_PRESET_WOPR, &crt);
    crt.bloom_strength = 0.75f;
    crt.bloom_radius_px = 4.0f;
    crt.persistence_decay = 0.92f;
    crt.jitter_amount = 0.15f;
    crt.flicker_amount = 0.1f;
    vg_set_crt_profile(a->vg, &crt);
    return 1;
}

static void destroy_vg_context(app* a) {
    if (a->wave_path) {
        vg_path_destroy(a->wave_path);
        a->wave_path = NULL;
    }
    if (a->vg) {
        vg_context_destroy(a->vg);
        a->vg = NULL;
    }
}

static void destroy_swapchain_resources(app* a) {
    destroy_vg_context(a);

    if (a->bloom_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(a->device, a->bloom_pipeline, NULL);
        a->bloom_pipeline = VK_NULL_HANDLE;
    }
    if (a->composite_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(a->device, a->composite_pipeline, NULL);
        a->composite_pipeline = VK_NULL_HANDLE;
    }
    if (a->post_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(a->device, a->post_layout, NULL);
        a->post_layout = VK_NULL_HANDLE;
    }
    if (a->post_desc_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(a->device, a->post_desc_pool, NULL);
        a->post_desc_pool = VK_NULL_HANDLE;
    }
    if (a->post_desc_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(a->device, a->post_desc_layout, NULL);
        a->post_desc_layout = VK_NULL_HANDLE;
    }
    if (a->post_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(a->device, a->post_sampler, NULL);
        a->post_sampler = VK_NULL_HANDLE;
    }

    if (a->scene_fb != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(a->device, a->scene_fb, NULL);
        a->scene_fb = VK_NULL_HANDLE;
    }
    if (a->bloom_fb != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(a->device, a->bloom_fb, NULL);
        a->bloom_fb = VK_NULL_HANDLE;
    }
    if (a->scene_render_pass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(a->device, a->scene_render_pass, NULL);
        a->scene_render_pass = VK_NULL_HANDLE;
    }
    if (a->bloom_render_pass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(a->device, a->bloom_render_pass, NULL);
        a->bloom_render_pass = VK_NULL_HANDLE;
    }

    if (a->scene_view != VK_NULL_HANDLE) {
        vkDestroyImageView(a->device, a->scene_view, NULL);
        a->scene_view = VK_NULL_HANDLE;
    }
    if (a->bloom_view != VK_NULL_HANDLE) {
        vkDestroyImageView(a->device, a->bloom_view, NULL);
        a->bloom_view = VK_NULL_HANDLE;
    }
    if (a->scene_image != VK_NULL_HANDLE) {
        vkDestroyImage(a->device, a->scene_image, NULL);
        a->scene_image = VK_NULL_HANDLE;
    }
    if (a->bloom_image != VK_NULL_HANDLE) {
        vkDestroyImage(a->device, a->bloom_image, NULL);
        a->bloom_image = VK_NULL_HANDLE;
    }
    if (a->scene_memory != VK_NULL_HANDLE) {
        vkFreeMemory(a->device, a->scene_memory, NULL);
        a->scene_memory = VK_NULL_HANDLE;
    }
    if (a->bloom_memory != VK_NULL_HANDLE) {
        vkFreeMemory(a->device, a->bloom_memory, NULL);
        a->bloom_memory = VK_NULL_HANDLE;
    }

    if (a->command_pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(a->device, a->command_pool, NULL);
        a->command_pool = VK_NULL_HANDLE;
    }

    for (uint32_t i = 0; i < a->swapchain_image_count; ++i) {
        if (a->present_framebuffers[i] != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(a->device, a->present_framebuffers[i], NULL);
            a->present_framebuffers[i] = VK_NULL_HANDLE;
        }
    }
    if (a->present_render_pass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(a->device, a->present_render_pass, NULL);
        a->present_render_pass = VK_NULL_HANDLE;
    }

    for (uint32_t i = 0; i < a->swapchain_image_count; ++i) {
        if (a->swapchain_image_views[i] != VK_NULL_HANDLE) {
            vkDestroyImageView(a->device, a->swapchain_image_views[i], NULL);
            a->swapchain_image_views[i] = VK_NULL_HANDLE;
        }
    }
    if (a->swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(a->device, a->swapchain, NULL);
        a->swapchain = VK_NULL_HANDLE;
    }

    a->swapchain_image_count = 0;
    a->scene_initialized = 0;
}

static int create_swapchain_resources(app* a) {
    if (!create_swapchain(a) ||
        !create_swapchain_image_views(a) ||
        !create_render_passes(a) ||
        !create_offscreen_targets(a) ||
        !create_present_framebuffers(a) ||
        !create_command_pool_and_buffers(a) ||
        !create_post_resources(a) ||
        !init_scene_image_layout(a) ||
        !create_vg_context(a)) {
        return 0;
    }
    return 1;
}

static int recreate_swapchain_resources(app* a) {
    int w = 0;
    int h = 0;
    SDL_Vulkan_GetDrawableSize(a->window, &w, &h);
    if (w == 0 || h == 0) {
        return 1;
    }

    if (!check_vk(vkDeviceWaitIdle(a->device), "vkDeviceWaitIdle(recreate)")) {
        return 0;
    }
    destroy_swapchain_resources(a);
    return create_swapchain_resources(a);
}

static void set_viewport_scissor(VkCommandBuffer cmd, uint32_t w, uint32_t h) {
    VkViewport vp = {.x = 0.0f, .y = 0.0f, .width = (float)w, .height = (float)h, .minDepth = 0.0f, .maxDepth = 1.0f};
    VkRect2D sc = {.offset = {0, 0}, .extent = {w, h}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
}

static void apply_selected_tweak(app* a, int dir) {
    vg_crt_profile crt;
    vg_get_crt_profile(a->vg, &crt);

    switch (a->selected_param) {
        case UI_PARAM_BLOOM_STRENGTH:
            crt.bloom_strength = clampf(crt.bloom_strength + 0.05f * (float)dir, 0.0f, 3.0f);
            break;
        case UI_PARAM_BLOOM_RADIUS:
            crt.bloom_radius_px = clampf(crt.bloom_radius_px + 0.35f * (float)dir, 0.0f, 14.0f);
            break;
        case UI_PARAM_PERSISTENCE:
            crt.persistence_decay = clampf(crt.persistence_decay + 0.005f * (float)dir, 0.70f, 0.985f);
            break;
        case UI_PARAM_JITTER:
            crt.jitter_amount = clampf(crt.jitter_amount + 0.02f * (float)dir, 0.0f, 1.5f);
            break;
        case UI_PARAM_FLICKER:
            crt.flicker_amount = clampf(crt.flicker_amount + 0.02f * (float)dir, 0.0f, 1.0f);
            break;
        case UI_PARAM_BEAM_CORE:
            crt.beam_core_width_px = clampf(crt.beam_core_width_px + 0.05f * (float)dir, 0.5f, 3.5f);
            break;
        case UI_PARAM_BEAM_HALO:
            crt.beam_halo_width_px = clampf(crt.beam_halo_width_px + 0.12f * (float)dir, 0.0f, 10.0f);
            break;
        case UI_PARAM_BEAM_INTENSITY:
            crt.beam_intensity = clampf(crt.beam_intensity + 0.05f * (float)dir, 0.2f, 3.0f);
            break;
        case UI_PARAM_VIGNETTE:
            crt.vignette_strength = clampf(crt.vignette_strength + 0.02f * (float)dir, 0.0f, 1.0f);
            break;
        case UI_PARAM_BARREL:
            crt.barrel_distortion = clampf(crt.barrel_distortion + 0.01f * (float)dir, 0.0f, 0.30f);
            break;
        case UI_PARAM_SCANLINE:
            crt.scanline_strength = clampf(crt.scanline_strength + 0.02f * (float)dir, 0.0f, 1.0f);
            break;
        case UI_PARAM_NOISE:
            crt.noise_strength = clampf(crt.noise_strength + 0.01f * (float)dir, 0.0f, 0.30f);
            break;
        case UI_PARAM_LINE_WIDTH:
            a->main_line_width = clampf(a->main_line_width + 0.25f * (float)dir, 1.0f, 16.0f);
            break;
        default:
            break;
    }
    vg_set_crt_profile(a->vg, &crt);
}

static void step_selected_param(app* a, int dir) {
    if (dir > 0) {
        a->selected_param = (a->selected_param + 1) % UI_PARAM_COUNT;
    } else if (dir < 0) {
        a->selected_param = (a->selected_param + UI_PARAM_COUNT - 1) % UI_PARAM_COUNT;
    }
}

static void handle_ui_hold(app* a, float dt) {
    const Uint8* ks = SDL_GetKeyboardState(NULL);
    int adjust_dir = (ks[SDL_SCANCODE_RIGHT] ? 1 : 0) - (ks[SDL_SCANCODE_LEFT] ? 1 : 0);
    int nav_dir = (ks[SDL_SCANCODE_UP] ? 1 : 0) - (ks[SDL_SCANCODE_DOWN] ? 1 : 0);

    if (adjust_dir != 0) {
        if (adjust_dir != a->prev_adjust_dir) {
            apply_selected_tweak(a, adjust_dir);
            a->adjust_repeat_timer = 0.24f;
        } else {
            a->adjust_repeat_timer -= dt;
            while (a->adjust_repeat_timer <= 0.0f) {
                apply_selected_tweak(a, adjust_dir);
                a->adjust_repeat_timer += 0.06f;
            }
        }
    } else {
        a->adjust_repeat_timer = 0.0f;
    }
    a->prev_adjust_dir = adjust_dir;

    if (nav_dir != 0) {
        if (nav_dir != a->prev_nav_dir) {
            step_selected_param(a, nav_dir);
            a->nav_repeat_timer = 0.24f;
        } else {
            a->nav_repeat_timer -= dt;
            while (a->nav_repeat_timer <= 0.0f) {
                step_selected_param(a, nav_dir);
                a->nav_repeat_timer += 0.09f;
            }
        }
    } else {
        a->nav_repeat_timer = 0.0f;
    }
    a->prev_nav_dir = nav_dir;
}

static vg_result draw_debug_ui(app* a, const vg_crt_profile* crt, float fps) {
    const float ui_x = 24.0f;
    const float ui_y = 24.0f;
    const float ui_w = 560.0f;
    const float row_step = 40.0f;
    const float rows_top = 70.0f;
    const float footer_h = 56.0f;
    const float ui_h = rows_top + (float)UI_PARAM_COUNT * row_step + footer_h;

    vg_stroke_style panel = {
        .width_px = 2.0f,
        .intensity = 0.65f,
        .color = {0.15f, 0.95f, 0.35f, 0.80f},
        .cap = VG_LINE_CAP_BUTT,
        .join = VG_LINE_JOIN_BEVEL,
        .miter_limit = 2.0f,
        .blend = VG_BLEND_ALPHA
    };
    vg_stroke_style text = panel;
    text.width_px = 1.6f;
    text.intensity = 0.95f;
    text.cap = VG_LINE_CAP_ROUND;
    text.join = VG_LINE_JOIN_ROUND;
    text.blend = VG_BLEND_ALPHA;

    vg_rect ui_rect = {ui_x, ui_y, ui_w, ui_h};
    vg_result r = vg_draw_rect(a->vg, ui_rect, &panel);
    if (r != VG_OK) {
        return r;
    }

    char line[96];
    r = vg_draw_text(
        a->vg,
        "TAB UI  UP DOWN SELECT  LEFT RIGHT ADJUST",
        (vg_vec2){ui_x + 16.0f, ui_y + 14.0f},
        11.0f,
        0.8f,
        &text,
        NULL
    );
    if (r != VG_OK) {
        return r;
    }

    static const char* labels[UI_PARAM_COUNT] = {
        "BLOOM STR",
        "BLOOM RAD",
        "PERSISTENCE",
        "JITTER",
        "FLICKER",
        "BEAM CORE",
        "BEAM HALO",
        "BEAM INTENSITY",
        "VIGNETTE",
        "BARREL DISTORT",
        "SCANLINE",
        "NOISE",
        "LINE WIDTH PX"
    };
    float values[UI_PARAM_COUNT] = {
        crt->bloom_strength,
        crt->bloom_radius_px,
        crt->persistence_decay,
        crt->jitter_amount,
        crt->flicker_amount,
        crt->beam_core_width_px,
        crt->beam_halo_width_px,
        crt->beam_intensity,
        crt->vignette_strength,
        crt->barrel_distortion,
        crt->scanline_strength,
        crt->noise_strength,
        a->main_line_width
    };
    float values_norm[UI_PARAM_COUNT] = {
        norm_range(crt->bloom_strength, 0.0f, 3.0f),
        norm_range(crt->bloom_radius_px, 0.0f, 14.0f),
        norm_range(crt->persistence_decay, 0.70f, 0.985f),
        norm_range(crt->jitter_amount, 0.0f, 1.5f),
        norm_range(crt->flicker_amount, 0.0f, 1.0f),
        norm_range(crt->beam_core_width_px, 0.5f, 3.5f),
        norm_range(crt->beam_halo_width_px, 0.0f, 10.0f),
        norm_range(crt->beam_intensity, 0.2f, 3.0f),
        norm_range(crt->vignette_strength, 0.0f, 1.0f),
        norm_range(crt->barrel_distortion, 0.0f, 0.30f),
        norm_range(crt->scanline_strength, 0.0f, 1.0f),
        norm_range(crt->noise_strength, 0.0f, 0.30f),
        norm_range(a->main_line_width, 1.0f, 16.0f)
    };

    for (int i = 0; i < UI_PARAM_COUNT; ++i) {
        float row_y = ui_y + rows_top + (float)i * row_step;
        vg_rect button = {ui_x + 16.0f, row_y, 222.0f, 30.0f};
        r = vg_draw_button(a->vg, button, labels[i], 13.0f, &panel, &text, i == a->selected_param);
        if (r != VG_OK) {
            return r;
        }

        vg_rect slider = {ui_x + 254.0f, row_y + 2.0f, 230.0f, 26.0f};
        r = vg_draw_slider(a->vg, slider, values_norm[i], &panel, &text, &text);
        if (r != VG_OK) {
            return r;
        }

        snprintf(line, sizeof(line), "%.3f", values[i]);
        r = vg_draw_text(a->vg, line, (vg_vec2){ui_x + 492.0f, row_y + 8.0f}, 11.5f, 0.8f, &text, NULL);
        if (r != VG_OK) {
            return r;
        }
    }

    snprintf(line, sizeof(line), "FPS %.1f", fps);
    return vg_draw_text(a->vg, line, (vg_vec2){ui_x + 16.0f, ui_y + ui_h - 26.0f}, 18.0f, 1.0f, &text, NULL);
}

static frame_result record_and_submit(app* a, uint32_t image_index, float t, float dt, float fps) {
    VkCommandBuffer cmd = a->command_buffers[image_index];

    if (!check_vk(vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer")) {
        return FRAME_FAIL;
    }
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if (!check_vk(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer")) {
        return FRAME_FAIL;
    }

    VkClearValue scene_clear = {.color = {{0.0f, 0.0f, 0.0f, 1.0f}}};
    VkRenderPassBeginInfo scene_rp = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = a->scene_render_pass,
        .framebuffer = a->scene_fb,
        .renderArea = {.offset = {0, 0}, .extent = a->swapchain_extent},
        .clearValueCount = 1,
        .pClearValues = &scene_clear
    };
    vkCmdBeginRenderPass(cmd, &scene_rp, VK_SUBPASS_CONTENTS_INLINE);

    vg_frame_desc frame = {
        .width = a->swapchain_extent.width,
        .height = a->swapchain_extent.height,
        .delta_time_s = dt,
        .command_buffer = (void*)cmd
    };
    vg_result vr = vg_begin_frame(a->vg, &frame);
    if (vr != VG_OK) {
        fprintf(stderr, "vg_begin_frame failed: %s\n", vg_result_string(vr));
        return FRAME_FAIL;
    }

    vg_crt_profile crt;
    vg_get_crt_profile(a->vg, &crt);
    float persistence = crt.persistence_decay;
    if (persistence < 0.0f) {
        persistence = 0.0f;
    }
    if (persistence > 1.0f) {
        persistence = 1.0f;
    }
    float frame_decay = powf(persistence, dt * 95.0f);
    float fade_alpha = 1.0f - frame_decay;
    if (fade_alpha < 0.025f) {
        fade_alpha = 0.025f;
    }

    float flicker_n = rand_signed((uint32_t)(t * 1000.0f));
    float intensity_scale = 1.0f + crt.flicker_amount * flicker_n;
    if (intensity_scale < 0.0f) {
        intensity_scale = 0.0f;
    }
    float jx = crt.jitter_amount * 2.0f * rand_signed((uint32_t)(t * 1300.0f));
    float jy = crt.jitter_amount * 2.0f * rand_signed((uint32_t)(t * 1700.0f));

    vg_stroke_style fade = {
        .width_px = (float)a->swapchain_extent.height * 2.5f,
        .intensity = 1.0f,
        .color = {0.0f, 0.0f, 0.0f, fade_alpha},
        .cap = VG_LINE_CAP_BUTT,
        .join = VG_LINE_JOIN_BEVEL,
        .miter_limit = 1.0f,
        .blend = VG_BLEND_ALPHA
    };
    vg_vec2 fade_line[2] = {
        {-(float)a->swapchain_extent.width + jx, (float)a->swapchain_extent.height * 0.5f + jy},
        {(float)a->swapchain_extent.width * 2.0f + jx, (float)a->swapchain_extent.height * 0.5f + jy}
    };
    vr = vg_draw_polyline(a->vg, fade_line, 2, &fade, 0);
    if (vr != VG_OK) {
        fprintf(stderr, "vg_draw_polyline(fade) failed: %s\n", vg_result_string(vr));
        return FRAME_FAIL;
    }

    float cx = (float)a->swapchain_extent.width * 0.5f;
    float cy = (float)a->swapchain_extent.height * 0.5f;

    vg_stroke_style halo_s = {
        .width_px = a->main_line_width * crt.beam_core_width_px + crt.beam_halo_width_px,
        .intensity = 0.42f * crt.beam_intensity * intensity_scale,
        .color = {0.2f, 1.0f, 0.35f, 0.45f},
        .cap = VG_LINE_CAP_ROUND,
        .join = VG_LINE_JOIN_ROUND,
        .miter_limit = 4.0f,
        .blend = VG_BLEND_ADDITIVE
    };
    vg_stroke_style main_s = {
        .width_px = a->main_line_width * crt.beam_core_width_px,
        .intensity = 1.2f * crt.beam_intensity * intensity_scale,
        .color = {0.2f, 1.0f, 0.35f, 1.0f},
        .cap = VG_LINE_CAP_ROUND,
        .join = VG_LINE_JOIN_ROUND,
        .miter_limit = 4.0f,
        .blend = VG_BLEND_ADDITIVE
    };

    vg_vec2 tri[4] = {
        {cx + cosf(t) * 120.0f + jx, cy - 140.0f + jy},
        {cx + 140.0f + jx, cy + 100.0f + jy},
        {cx - 140.0f + jx, cy + 100.0f + jy},
        {cx + cosf(t) * 120.0f + jx, cy - 140.0f + jy}
    };
    vr = vg_draw_polyline(a->vg, tri, 4, &halo_s, 0);
    if (vr != VG_OK) {
        fprintf(stderr, "vg_draw_polyline(tri halo) failed: %s\n", vg_result_string(vr));
        return FRAME_FAIL;
    }
    vr = vg_draw_polyline(a->vg, tri, 4, &main_s, 0);
    if (vr != VG_OK) {
        fprintf(stderr, "vg_draw_polyline(tri) failed: %s\n", vg_result_string(vr));
        return FRAME_FAIL;
    }

    vg_path_clear(a->wave_path);
    vg_path_move_to(a->wave_path, (vg_vec2){120.0f + jx, cy + 220.0f + jy});
    vg_path_cubic_to(a->wave_path, (vg_vec2){280.0f + jx, cy + 80.0f + sinf(t) * 50.0f + jy}, (vg_vec2){420.0f + jx, cy + 360.0f + jy}, (vg_vec2){580.0f + jx, cy + 220.0f + jy});
    vg_path_cubic_to(a->wave_path, (vg_vec2){760.0f + jx, cy + 70.0f + jy}, (vg_vec2){920.0f + jx, cy + 370.0f + cosf(t * 1.2f) * 60.0f + jy}, (vg_vec2){1120.0f + jx, cy + 220.0f + jy});
    vr = vg_draw_path_stroke(a->vg, a->wave_path, &halo_s);
    if (vr != VG_OK) {
        fprintf(stderr, "vg_draw_path_stroke(wave halo) failed: %s\n", vg_result_string(vr));
        return FRAME_FAIL;
    }
    vr = vg_draw_path_stroke(a->vg, a->wave_path, &main_s);
    if (vr != VG_OK) {
        fprintf(stderr, "vg_draw_path_stroke(wave) failed: %s\n", vg_result_string(vr));
        return FRAME_FAIL;
    }

    if (a->show_ui) {
        vr = draw_debug_ui(a, &crt, fps);
        if (vr != VG_OK) {
            fprintf(stderr, "draw_debug_ui failed: %s\n", vg_result_string(vr));
            return FRAME_FAIL;
        }
    }

    vr = vg_end_frame(a->vg);
    if (vr != VG_OK) {
        fprintf(stderr, "vg_end_frame failed: %s\n", vg_result_string(vr));
        return FRAME_FAIL;
    }

    vkCmdEndRenderPass(cmd);

    VkClearValue bloom_clear = {.color = {{0.0f, 0.0f, 0.0f, 1.0f}}};
    VkRenderPassBeginInfo bloom_rp = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = a->bloom_render_pass,
        .framebuffer = a->bloom_fb,
        .renderArea = {.offset = {0, 0}, .extent = a->swapchain_extent},
        .clearValueCount = 1,
        .pClearValues = &bloom_clear
    };
    vkCmdBeginRenderPass(cmd, &bloom_rp, VK_SUBPASS_CONTENTS_INLINE);
    set_viewport_scissor(cmd, a->swapchain_extent.width, a->swapchain_extent.height);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, a->bloom_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, a->post_layout, 0, 1, &a->post_desc_set, 0, NULL);

    post_pc pc = {0};
    pc.texel[0] = 1.0f / (float)a->swapchain_extent.width;
    pc.texel[1] = 1.0f / (float)a->swapchain_extent.height;
    pc.bloom_strength = crt.bloom_strength;
    pc.bloom_radius_px = crt.bloom_radius_px;
    pc.vignette_strength = crt.vignette_strength;
    pc.barrel_distortion = crt.barrel_distortion;
    pc.scanline_strength = crt.scanline_strength;
    pc.noise_strength = crt.noise_strength;
    pc.time_s = t;
    vkCmdPushConstants(cmd, a->post_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);

    VkClearValue present_clear = {.color = {{0.0f, 0.0f, 0.0f, 1.0f}}};
    VkRenderPassBeginInfo present_rp = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = a->present_render_pass,
        .framebuffer = a->present_framebuffers[image_index],
        .renderArea = {.offset = {0, 0}, .extent = a->swapchain_extent},
        .clearValueCount = 1,
        .pClearValues = &present_clear
    };
    vkCmdBeginRenderPass(cmd, &present_rp, VK_SUBPASS_CONTENTS_INLINE);
    set_viewport_scissor(cmd, a->swapchain_extent.width, a->swapchain_extent.height);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, a->composite_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, a->post_layout, 0, 1, &a->post_desc_set, 0, NULL);
    vkCmdPushConstants(cmd, a->post_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);

    if (!check_vk(vkEndCommandBuffer(cmd), "vkEndCommandBuffer")) {
        return FRAME_FAIL;
    }

    if (!check_vk(vkResetFences(a->device, 1, &a->in_flight), "vkResetFences")) {
        return FRAME_FAIL;
    }

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &a->image_available,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &a->render_finished
    };

    if (!check_vk(vkQueueSubmit(a->graphics_queue, 1, &submit, a->in_flight), "vkQueueSubmit")) {
        return FRAME_FAIL;
    }

    VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &a->render_finished,
        .swapchainCount = 1,
        .pSwapchains = &a->swapchain,
        .pImageIndices = &image_index
    };
    VkResult pr = vkQueuePresentKHR(a->present_queue, &present);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
        return FRAME_RECREATE;
    }
    if (!check_vk(pr, "vkQueuePresentKHR")) {
        return FRAME_FAIL;
    }
    return FRAME_OK;
}

static void cleanup(app* a) {
    if (a->device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(a->device);
        destroy_swapchain_resources(a);
    }

    if (a->in_flight != VK_NULL_HANDLE) {
        vkDestroyFence(a->device, a->in_flight, NULL);
    }
    if (a->render_finished != VK_NULL_HANDLE) {
        vkDestroySemaphore(a->device, a->render_finished, NULL);
    }
    if (a->image_available != VK_NULL_HANDLE) {
        vkDestroySemaphore(a->device, a->image_available, NULL);
    }

    if (a->device != VK_NULL_HANDLE) {
        vkDestroyDevice(a->device, NULL);
    }
    if (a->surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(a->instance, a->surface, NULL);
    }
    if (a->instance != VK_NULL_HANDLE) {
        vkDestroyInstance(a->instance, NULL);
    }

    if (a->window) {
        SDL_DestroyWindow(a->window);
    }
    SDL_Quit();
}

int main(void) {
    app a;
    memset(&a, 0, sizeof(a));
    a.show_ui = 1;
    a.selected_param = 0;
    a.main_line_width = 4.5f;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    a.window = SDL_CreateWindow(
        "vectorgfx Vulkan example",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        APP_WIDTH,
        APP_HEIGHT,
        SDL_WINDOW_VULKAN | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );
    if (!a.window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        cleanup(&a);
        return 1;
    }

    if (!create_instance(&a) ||
        !create_surface(&a) ||
        !pick_physical_device(&a) ||
        !create_device(&a) ||
        !create_sync(&a) ||
        !create_swapchain_resources(&a)) {
        cleanup(&a);
        return 1;
    }

    int running = 1;
    int need_recreate = 0;
    uint64_t last = SDL_GetPerformanceCounter();
    float freq = (float)SDL_GetPerformanceFrequency();

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                running = 0;
            } else if (ev.type == SDL_WINDOWEVENT) {
                if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || ev.window.event == SDL_WINDOWEVENT_RESIZED) {
                    need_recreate = 1;
                }
            } else if (ev.type == SDL_KEYDOWN && !ev.key.repeat) {
                if (ev.key.keysym.sym == SDLK_TAB) {
                    a.show_ui = !a.show_ui;
                }
            }
        }

        uint64_t now = SDL_GetPerformanceCounter();
        float dt = (float)(now - last) / freq;
        last = now;
        if (dt <= 0.0f) {
            dt = 1.0f / 60.0f;
        }
        if (a.show_ui) {
            handle_ui_hold(&a, dt);
        } else {
            a.prev_adjust_dir = 0;
            a.prev_nav_dir = 0;
            a.adjust_repeat_timer = 0.0f;
            a.nav_repeat_timer = 0.0f;
        }
        float fps_inst = 1.0f / dt;
        if (a.fps_smoothed <= 0.0f) {
            a.fps_smoothed = fps_inst;
        } else {
            a.fps_smoothed += (fps_inst - a.fps_smoothed) * 0.10f;
        }

        if (need_recreate) {
            if (!recreate_swapchain_resources(&a)) {
                break;
            }
            need_recreate = 0;
            continue;
        }

        if (!check_vk(vkWaitForFences(a.device, 1, &a.in_flight, VK_TRUE, UINT64_MAX), "vkWaitForFences")) {
            break;
        }

        uint32_t image_index = 0;
        VkResult ar = vkAcquireNextImageKHR(a.device, a.swapchain, UINT64_MAX, a.image_available, VK_NULL_HANDLE, &image_index);
        if (ar == VK_ERROR_OUT_OF_DATE_KHR) {
            need_recreate = 1;
            continue;
        }
        if (ar == VK_SUBOPTIMAL_KHR) {
            need_recreate = 1;
        }
        if (!check_vk(ar, "vkAcquireNextImageKHR")) {
            break;
        }

        float t = (float)SDL_GetTicks() * 0.001f;
        frame_result fr = record_and_submit(&a, image_index, t, dt, a.fps_smoothed);
        if (fr == FRAME_RECREATE) {
            need_recreate = 1;
            continue;
        }
        if (fr == FRAME_FAIL) {
            break;
        }
    }

    cleanup(&a);
    return 0;
}
