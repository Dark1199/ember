// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "gfx/renderer.h"
#include "ui/ui_batch.h"

namespace ember::gfx {

/// Draws a UiBatch on top of the scene: alpha-blended quads sampled from the built-in font atlas
/// (nearest filtering, so pixels stay sharp at any scale). One host-visible vertex buffer per
/// frame in flight, written every frame. Render thread only.
class UiRenderer {
public:
    bool Initialize(Renderer& renderer, std::string* error);
    [[nodiscard]] bool Ready() const noexcept { return pipeline_ != VK_NULL_HANDLE; }

    /// Records the draw. `layout_width` x `layout_height` are the layout units the batch uses;
    /// `pre_rotation_quarter_turns` is the renderer's value.
    void Draw(const FrameContext& ctx, const ui::UiBatch& batch, float layout_width, float layout_height,
              int pre_rotation_quarter_turns);

    void Release(Renderer& renderer);

private:
    static constexpr std::uint32_t kSlots = 2;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory image_memory_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkBuffer buffers_[kSlots] = {};
    VkDeviceMemory memories_[kSlots] = {};
    void* mapped_[kSlots] = {};
};

} // namespace ember::gfx
