// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "formats/dds.h"
#include "gfx/renderer.h"

namespace ember::gfx {

/// One draw: a slice of the index buffer and the material it belongs to.
struct MeshRange {
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
    std::uint32_t material_index = 0;
};

/// Draws one indexed triangle mesh (positions + UVs) with a textured, flat-lit pipeline. Buffers
/// are host-visible and written once (the meshes are small and the target GPUs share memory with
/// the CPU); textures go through a staging buffer into optimal-tiled images. Every material starts
/// on a 1x1 default texture and is switched with BindMaterialTexture. All calls must come from the
/// render thread.
class MeshRenderer {
public:
    MeshRenderer() = default;
    ~MeshRenderer() = default;
    MeshRenderer(const MeshRenderer&) = delete;
    MeshRenderer& operator=(const MeshRenderer&) = delete;

    /// Creates the pipeline, sampler and descriptor pool. Needs the device (surface attached once).
    bool Initialize(Renderer& renderer, std::string* error);

    /// Replaces the mesh. `positions` holds xyz triples, `uvs` uv pairs (same vertex count).
    bool Upload(Renderer& renderer, const std::vector<float>& positions, const std::vector<float>& uvs,
                const std::vector<std::uint32_t>& indices, const std::vector<MeshRange>& ranges,
                std::string* error);

    /// Uploads a decoded mip chain; returns its slot (>= 1) or -1 on failure.
    int AddTexture(Renderer& renderer, const std::vector<formats::RgbaImage>& levels, std::string* error);

    /// All ranges of `material_index` sample from `slot` from now on.
    void BindMaterialTexture(std::uint32_t material_index, int slot);

    [[nodiscard]] bool Ready() const noexcept {
        return pipeline_ != VK_NULL_HANDLE && index_count_ > 0;
    }

    void Draw(const FrameContext& ctx, const float mvp[16], float min_y, float max_y) const;

    /// Destroys everything; call before the renderer shuts down.
    void Release(Renderer& renderer);

private:
    struct Buffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
    };
    struct Texture {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };
    bool CreateBuffer(Renderer& renderer, VkDeviceSize size, VkBufferUsageFlags usage,
                      const void* data, Buffer& out, std::string* error);
    void DestroyBuffer(VkDevice device, Buffer& buffer);
    bool CreateTexture(Renderer& renderer, const std::vector<formats::RgbaImage>& levels, Texture& out,
                       std::string* error);
    void DestroyTexture(VkDevice device, Texture& texture);

    static constexpr std::uint32_t kMaxTextures = 96;

    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    Buffer vertices_;
    Buffer uvs_;
    Buffer indices_;
    std::uint32_t index_count_ = 0;
    std::vector<Texture> textures_; ///< slot 0 is the default texture
    std::vector<MeshRange> ranges_;
    std::vector<int> range_slot_;   ///< parallel to ranges_
};

} // namespace ember::gfx
