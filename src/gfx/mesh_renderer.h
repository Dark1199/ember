// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "gfx/renderer.h"

namespace ember::gfx {

/// Draws one indexed triangle mesh (positions only) with a flat-shaded pipeline. Buffers are
/// host-visible and written once: the meshes are small and the target GPUs share memory with the
/// CPU, so no staging copy is needed. All calls must come from the render thread.
class MeshRenderer {
public:
    MeshRenderer() = default;
    ~MeshRenderer() = default;
    MeshRenderer(const MeshRenderer&) = delete;
    MeshRenderer& operator=(const MeshRenderer&) = delete;

    /// Creates the pipeline. Needs the renderer's device to exist (a surface attached once).
    bool Initialize(Renderer& renderer, std::string* error);

    /// Replaces the mesh. `positions` holds xyz triples. Waits for the GPU to be idle first.
    bool Upload(Renderer& renderer, const std::vector<float>& positions,
                const std::vector<std::uint32_t>& indices, std::string* error);

    [[nodiscard]] bool Ready() const noexcept {
        return pipeline_ != VK_NULL_HANDLE && index_count_ > 0;
    }

    /// Records the draw. `mvp` is column-major; `min_y`/`max_y` drive the height tint.
    void Draw(const FrameContext& ctx, const float mvp[16], float min_y, float max_y) const;

    /// Destroys everything; call before the renderer shuts down.
    void Release(Renderer& renderer);

private:
    struct Buffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
    };
    bool CreateBuffer(Renderer& renderer, VkDeviceSize size, VkBufferUsageFlags usage,
                      const void* data, Buffer& out, std::string* error);
    void DestroyBuffer(VkDevice device, Buffer& buffer);

    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    Buffer vertices_;
    Buffer indices_;
    std::uint32_t index_count_ = 0;
};

} // namespace ember::gfx
