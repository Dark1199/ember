// SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx/renderer.h"

#include <algorithm>
#include <cstring>

#include "core/log.h"

namespace ember::gfx {

namespace {

bool Check(VkResult result, const char* what, std::string* error) {
    if (result == VK_SUCCESS) {
        return true;
    }
    if (error) {
        *error = std::string(what) + " failed (VkResult " + std::to_string(result) + ")";
    }
    EMBER_ERROR("%s failed (VkResult %d)", what, static_cast<int>(result));
    return false;
}

#define EMBER_VK(expr, error) \
    do {                      \
        if (!Check((expr), #expr, (error))) return false; \
    } while (0)

} // namespace

Renderer::~Renderer() {
    Shutdown();
}

bool Renderer::Initialize(const std::vector<const char*>& instance_extensions,
                          std::string* error) {
    const VkApplicationInfo app{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "ember",
        .applicationVersion = VK_MAKE_VERSION(0, 0, 1),
        .pEngineName = "ember",
        .engineVersion = VK_MAKE_VERSION(0, 0, 1),
        .apiVersion = VK_API_VERSION_1_1,
    };
    const VkInstanceCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
        .enabledExtensionCount = static_cast<uint32_t>(instance_extensions.size()),
        .ppEnabledExtensionNames = instance_extensions.data(),
    };
    EMBER_VK(vkCreateInstance(&info, nullptr, &instance_), error);
    return true;
}

bool Renderer::PickPhysicalDevice(std::string* error) {
    uint32_t count = 0;
    EMBER_VK(vkEnumeratePhysicalDevices(instance_, &count, nullptr), error);
    std::vector<VkPhysicalDevice> devices(count);
    EMBER_VK(vkEnumeratePhysicalDevices(instance_, &count, devices.data()), error);

    for (const VkPhysicalDevice candidate : devices) {
        uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
        for (uint32_t i = 0; i < family_count; ++i) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface_, &present);
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                physical_ = candidate;
                queue_family_ = i;
                VkPhysicalDeviceProperties props;
                vkGetPhysicalDeviceProperties(candidate, &props);
                EMBER_LOG("GPU: %s | API %u.%u.%u | driver 0x%x", props.deviceName,
                          VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
                          VK_VERSION_PATCH(props.apiVersion), props.driverVersion);
                return true;
            }
        }
    }
    if (error) {
        *error = "no GPU with a graphics+present queue";
    }
    return false;
}

bool Renderer::CreateDevice(std::string* error) {
    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queue_info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = queue_family_,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    const VkDeviceCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = extensions,
    };
    EMBER_VK(vkCreateDevice(physical_, &info, nullptr, &device_), error);
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    return true;
}

uint32_t Renderer::FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags required) const {
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(physical_, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & required) == required) {
            return i;
        }
    }
    return UINT32_MAX;
}

bool Renderer::AttachSurface(VkSurfaceKHR surface, std::string* error) {
    surface_ = surface;
    if (device_ == VK_NULL_HANDLE) {
        if (!PickPhysicalDevice(error) || !CreateDevice(error)) {
            return false;
        }
    } else {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(physical_, queue_family_, surface_, &present);
        if (!present) {
            if (error) {
                *error = "new surface is not presentable from the existing queue family";
            }
            return false;
        }
    }
    if (!CreateSwapchain(error) || !CreateRenderPass(error) || !CreateDepthResources(error) ||
        !CreateFramebuffers(error)) {
        return false;
    }
    if (frames_[0].pool == VK_NULL_HANDLE && !CreateFrames(error)) {
        return false;
    }
    return true;
}

bool Renderer::CreateSwapchain(std::string* error) {
    VkSurfaceCapabilitiesKHR caps;
    EMBER_VK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps), error);

    uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &format_count, formats.data());
    if (formats.empty()) {
        if (error) *error = "surface reports no formats";
        return false;
    }
    // Plain UNORM: shading happens in whatever space the engine chooses, no implicit sRGB step.
    VkSurfaceFormatKHR chosen = formats[0];
    for (const auto& f : formats) {
        if ((f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    }
    color_format_ = chosen.format;

    // Android reports the window size in currentExtent; 0xFFFFFFFF means "you decide".
    extent_ = caps.currentExtent;
    if (extent_.width == UINT32_MAX) {
        extent_ = {std::clamp(1280u, caps.minImageExtent.width, caps.maxImageExtent.width),
                   std::clamp(720u, caps.minImageExtent.height, caps.maxImageExtent.height)};
    }
    if (extent_.width == 0 || extent_.height == 0) {
        if (error) *error = "surface has zero size";
        return false;
    }

    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount != 0) {
        image_count = std::min(image_count, caps.maxImageCount);
    }

    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) {
        for (const auto bit : {VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
                               VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                               VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR}) {
            if (caps.supportedCompositeAlpha & bit) {
                alpha = bit;
                break;
            }
        }
    }

    const VkSwapchainCreateInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface_,
        .minImageCount = image_count,
        .imageFormat = chosen.format,
        .imageColorSpace = chosen.colorSpace,
        .imageExtent = extent_,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        // Follow the display rotation so the compositor never has to rotate our output.
        .preTransform = caps.currentTransform,
        .compositeAlpha = alpha,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR, // always available
        .clipped = VK_TRUE,
        .oldSwapchain = VK_NULL_HANDLE,
    };
    EMBER_VK(vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain_), error);

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr);
    images_.resize(count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &count, images_.data());

    views_.assign(count, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < count; ++i) {
        const VkImageViewCreateInfo view{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = images_[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = color_format_,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        EMBER_VK(vkCreateImageView(device_, &view, nullptr, &views_[i]), error);
    }

    render_finished_.assign(count, VK_NULL_HANDLE);
    const VkSemaphoreCreateInfo sem{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < count; ++i) {
        EMBER_VK(vkCreateSemaphore(device_, &sem, nullptr, &render_finished_[i]), error);
    }
    EMBER_LOG("Swapchain: %ux%u, %u images, format %d", extent_.width, extent_.height, count,
              static_cast<int>(color_format_));
    return true;
}

bool Renderer::CreateRenderPass(std::string* error) {
    if (render_pass_ != VK_NULL_HANDLE) {
        return true; // formats do not change across window recreation on a given device
    }
    // Pick a depth format the GPU can use as an attachment.
    for (const VkFormat candidate : {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT,
                                     VK_FORMAT_D16_UNORM_S8_UINT}) {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(physical_, candidate, &props);
        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            depth_format_ = candidate;
            break;
        }
    }
    if (depth_format_ == VK_FORMAT_UNDEFINED) {
        if (error) *error = "no usable depth/stencil format";
        return false;
    }

    const std::array<VkAttachmentDescription, 2> attachments{{
        {// colour: cleared on tile load, written back once, presented
         .format = color_format_,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR},
        {// depth/stencil: cleared, never stored, so it can stay in tile memory
         .format = depth_format_,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL},
    }};
    const VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    const VkSubpassDescription subpass{
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_ref,
        .pDepthStencilAttachment = &depth_ref,
    };
    // Orders this frame's attachment writes after the previous frame's (the depth image is
    // shared by the frames in flight) and after the swapchain image becomes available.
    constexpr VkPipelineStageFlags kStages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                             VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const VkSubpassDependency dependency{
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = kStages,
        .dstStageMask = kStages,
        .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
    };
    const VkRenderPassCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = static_cast<uint32_t>(attachments.size()),
        .pAttachments = attachments.data(),
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency,
    };
    EMBER_VK(vkCreateRenderPass(device_, &info, nullptr, &render_pass_), error);
    return true;
}

bool Renderer::CreateDepthResources(std::string* error) {
    const VkImageCreateInfo image_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = depth_format_,
        .extent = {extent_.width, extent_.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        // Transient: contents never leave the tile buffer (storeOp is DONT_CARE).
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    EMBER_VK(vkCreateImage(device_, &image_info, nullptr, &depth_image_), error);

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, depth_image_, &req);
    uint32_t type = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                                                           VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT);
    if (type == UINT32_MAX) {
        type = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }
    if (type == UINT32_MAX) {
        type = FindMemoryType(req.memoryTypeBits, 0);
    }
    if (type == UINT32_MAX) {
        if (error) *error = "no memory type for the depth image";
        return false;
    }
    const VkMemoryAllocateInfo alloc{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = type,
    };
    EMBER_VK(vkAllocateMemory(device_, &alloc, nullptr, &depth_memory_), error);
    EMBER_VK(vkBindImageMemory(device_, depth_image_, depth_memory_, 0), error);

    const VkImageViewCreateInfo view{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = depth_image_,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = depth_format_,
        .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1},
    };
    EMBER_VK(vkCreateImageView(device_, &view, nullptr, &depth_view_), error);
    return true;
}

bool Renderer::CreateFramebuffers(std::string* error) {
    framebuffers_.assign(views_.size(), VK_NULL_HANDLE);
    for (size_t i = 0; i < views_.size(); ++i) {
        const VkImageView attachments[] = {views_[i], depth_view_};
        const VkFramebufferCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = render_pass_,
            .attachmentCount = 2,
            .pAttachments = attachments,
            .width = extent_.width,
            .height = extent_.height,
            .layers = 1,
        };
        EMBER_VK(vkCreateFramebuffer(device_, &info, nullptr, &framebuffers_[i]), error);
    }
    return true;
}

bool Renderer::CreateFrames(std::string* error) {
    for (Frame& frame : frames_) {
        const VkCommandPoolCreateInfo pool{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
            .queueFamilyIndex = queue_family_,
        };
        EMBER_VK(vkCreateCommandPool(device_, &pool, nullptr, &frame.pool), error);
        const VkCommandBufferAllocateInfo alloc{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = frame.pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        EMBER_VK(vkAllocateCommandBuffers(device_, &alloc, &frame.cmd), error);
        const VkFenceCreateInfo fence{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                      .flags = VK_FENCE_CREATE_SIGNALED_BIT};
        EMBER_VK(vkCreateFence(device_, &fence, nullptr, &frame.fence), error);
        const VkSemaphoreCreateInfo sem{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        EMBER_VK(vkCreateSemaphore(device_, &sem, nullptr, &frame.image_available), error);
    }
    return true;
}

void Renderer::DestroySwapchainResources() {
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    vkDeviceWaitIdle(device_);
    for (const VkFramebuffer fb : framebuffers_) vkDestroyFramebuffer(device_, fb, nullptr);
    framebuffers_.clear();
    if (depth_view_) vkDestroyImageView(device_, depth_view_, nullptr);
    if (depth_image_) vkDestroyImage(device_, depth_image_, nullptr);
    if (depth_memory_) vkFreeMemory(device_, depth_memory_, nullptr);
    depth_view_ = VK_NULL_HANDLE;
    depth_image_ = VK_NULL_HANDLE;
    depth_memory_ = VK_NULL_HANDLE;
    for (const VkSemaphore s : render_finished_) vkDestroySemaphore(device_, s, nullptr);
    render_finished_.clear();
    for (const VkImageView v : views_) vkDestroyImageView(device_, v, nullptr);
    views_.clear();
    images_.clear();
    if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

void Renderer::DetachSurface() {
    DestroySwapchainResources();
    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
}

bool Renderer::RecreateSwapchain() {
    DestroySwapchainResources();
    std::string error;
    if (!CreateSwapchain(&error) || !CreateDepthResources(&error) ||
        !CreateFramebuffers(&error)) {
        EMBER_WARN("Swapchain recreation failed: %s", error.c_str());
        // The surface may be gone; the platform layer will attach a new one.
        DestroySwapchainResources();
        return false;
    }
    return true;
}

bool Renderer::DrawFrame(const std::function<void(const FrameContext&)>& record) {
    if (!HasSurface()) {
        return true; // nothing to draw into (window not available)
    }
    Frame& frame = frames_[frame_slot_];
    EMBER_VK(vkWaitForFences(device_, 1, &frame.fence, VK_TRUE, UINT64_MAX), nullptr);

    uint32_t image_index = 0;
    VkResult acquired = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                              frame.image_available, VK_NULL_HANDLE, &image_index);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        RecreateSwapchain();
        return true;
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
        return Check(acquired, "vkAcquireNextImageKHR", nullptr);
    }
    EMBER_VK(vkResetFences(device_, 1, &frame.fence), nullptr);
    EMBER_VK(vkResetCommandPool(device_, frame.pool, 0), nullptr);

    const VkCommandBufferBeginInfo begin{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    EMBER_VK(vkBeginCommandBuffer(frame.cmd, &begin), nullptr);

    VkClearValue clears[2]{};
    std::memcpy(clears[0].color.float32, clear_color_.data(), sizeof(float) * 4);
    clears[1].depthStencil = {1.0f, 0};
    const VkRenderPassBeginInfo pass{
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = render_pass_,
        .framebuffer = framebuffers_[image_index],
        .renderArea = {{0, 0}, extent_},
        .clearValueCount = 2,
        .pClearValues = clears,
    };
    vkCmdBeginRenderPass(frame.cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport{0, 0, static_cast<float>(extent_.width),
                              static_cast<float>(extent_.height), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, extent_};
    vkCmdSetViewport(frame.cmd, 0, 1, &viewport);
    vkCmdSetScissor(frame.cmd, 0, 1, &scissor);
    if (record) {
        record(FrameContext{frame.cmd, render_pass_, extent_, frame_slot_});
    }
    vkCmdEndRenderPass(frame.cmd);
    EMBER_VK(vkEndCommandBuffer(frame.cmd), nullptr);

    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkSubmitInfo submit{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &frame.image_available,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &frame.cmd,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &render_finished_[image_index],
    };
    EMBER_VK(vkQueueSubmit(queue_, 1, &submit, frame.fence), nullptr);

    const VkPresentInfoKHR present{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &render_finished_[image_index],
        .swapchainCount = 1,
        .pSwapchains = &swapchain_,
        .pImageIndices = &image_index,
    };
    const VkResult presented = vkQueuePresentKHR(queue_, &present);
    frame_slot_ = (frame_slot_ + 1) % kFramesInFlight;
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR ||
        acquired == VK_SUBOPTIMAL_KHR) {
        RecreateSwapchain();
        return true;
    }
    return Check(presented, "vkQueuePresentKHR", nullptr);
}

void Renderer::Shutdown() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }
    DetachSurface();
    if (device_ != VK_NULL_HANDLE) {
        for (Frame& frame : frames_) {
            if (frame.image_available) vkDestroySemaphore(device_, frame.image_available, nullptr);
            if (frame.fence) vkDestroyFence(device_, frame.fence, nullptr);
            if (frame.pool) vkDestroyCommandPool(device_, frame.pool, nullptr);
            frame = {};
        }
        if (render_pass_) vkDestroyRenderPass(device_, render_pass_, nullptr);
        render_pass_ = VK_NULL_HANDLE;
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
}

} // namespace ember::gfx
