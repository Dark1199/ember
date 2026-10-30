// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <functional>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace ember::gfx {

/// Handed to the per-frame recording callback. The render pass is already begun and the viewport
/// and scissor are set to the full swapchain extent; the callback only records draw commands.
struct FrameContext {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkExtent2D extent{};
    uint32_t frame_slot = 0;
};

/// Vulkan 1.1 renderer sized for a mobile tiler (Adreno 612 is Vulkan 1.1 only).
///
/// Design choices for tile-based GPUs:
///  - one render pass, one subpass, colour CLEAR/STORE and depth CLEAR/DONT_CARE so the depth
///    buffer never has to leave tile memory;
///  - the depth image is a transient attachment backed by lazily allocated memory when the
///    driver offers it;
///  - swapchain pre-transform follows the display's current transform so the compositor does
///    not need an extra rotation pass.
///
/// The instance outlives the window; the surface, swapchain and framebuffers are recreated when
/// Android destroys and recreates the window.
class Renderer {
public:
    Renderer() = default;
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    /// Creates the Vulkan instance with the platform's surface extensions.
    bool Initialize(const std::vector<const char*>& instance_extensions, std::string* error);

    [[nodiscard]] VkInstance Instance() const noexcept {
        return instance_;
    }

    /// Takes ownership of `surface`. The first call also selects the GPU and creates the device.
    bool AttachSurface(VkSurfaceKHR surface, std::string* error);

    /// Destroys the swapchain and surface (window lost); the device stays alive.
    void DetachSurface();

    [[nodiscard]] bool HasSurface() const noexcept {
        return surface_ != VK_NULL_HANDLE && swapchain_ != VK_NULL_HANDLE;
    }

    void SetClearColor(float r, float g, float b, float a = 1.0f) {
        clear_color_ = {r, g, b, a};
    }

    /// Renders one frame. Returns false only on an unrecoverable error.
    bool DrawFrame(const std::function<void(const FrameContext&)>& record);

    void Shutdown();

    // Access for resource classes (MeshRenderer). The device and render pass outlive the window.
    [[nodiscard]] VkDevice Device() const noexcept { return device_; }
    [[nodiscard]] VkRenderPass RenderPass() const noexcept { return render_pass_; }
    [[nodiscard]] VkQueue Queue() const noexcept { return queue_; }
    [[nodiscard]] uint32_t QueueFamily() const noexcept { return queue_family_; }
    [[nodiscard]] uint32_t FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags required) const;
    /// Quarter turns clockwise (0..3) the swapchain pre-transform asks the app to apply to clip
    /// space, and whether width/height are swapped relative to what the user sees.
    [[nodiscard]] int PreRotationQuarterTurns() const noexcept { return pre_rotation_; }
    [[nodiscard]] VkExtent2D Extent() const noexcept { return extent_; }

private:
    static constexpr uint32_t kFramesInFlight = 2;

    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore image_available = VK_NULL_HANDLE;
    };

    bool PickPhysicalDevice(std::string* error);
    bool CreateDevice(std::string* error);
    bool CreateSwapchain(std::string* error);
    bool CreateRenderPass(std::string* error);
    bool CreateDepthResources(std::string* error);
    bool CreateFramebuffers(std::string* error);
    bool CreateFrames(std::string* error);
    void DestroySwapchainResources();
    bool RecreateSwapchain();

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;

    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat color_format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    int pre_rotation_ = 0;
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    std::vector<VkFramebuffer> framebuffers_;
    std::vector<VkSemaphore> render_finished_; // one per swapchain image

    VkFormat depth_format_ = VK_FORMAT_UNDEFINED;
    VkImage depth_image_ = VK_NULL_HANDLE;
    VkDeviceMemory depth_memory_ = VK_NULL_HANDLE;
    VkImageView depth_view_ = VK_NULL_HANDLE;

    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    std::array<Frame, kFramesInFlight> frames_{};
    uint32_t frame_slot_ = 0;
    std::array<float, 4> clear_color_{0.02f, 0.02f, 0.04f, 1.0f};
};

} // namespace ember::gfx
