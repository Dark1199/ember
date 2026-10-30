// SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx/mesh_renderer.h"

#include <cstring>

namespace ember::gfx {
namespace {

// SPIR-V produced at build time from src/shaders (glslc -mfmt=c writes a brace-enclosed list).
const std::uint32_t kVertSpirv[] =
#include "mesh.vert.inc"
    ;
const std::uint32_t kFragSpirv[] =
#include "mesh.frag.inc"
    ;

constexpr std::uint32_t kPushBytes = 64 + 16;

bool Fail(std::string* error, const char* what, VkResult result) {
    if (error) *error = std::string(what) + " failed (" + std::to_string(static_cast<int>(result)) + ")";
    return false;
}

} // namespace

bool MeshRenderer::Initialize(Renderer& renderer, std::string* error) {
    VkDevice device = renderer.Device();
    if (device == VK_NULL_HANDLE) {
        if (error) *error = "no device yet";
        return false;
    }
    const VkShaderModuleCreateInfo vs{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                      .codeSize = sizeof(kVertSpirv), .pCode = kVertSpirv};
    const VkShaderModuleCreateInfo fs{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                      .codeSize = sizeof(kFragSpirv), .pCode = kFragSpirv};
    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
    VkResult r = vkCreateShaderModule(device, &vs, nullptr, &vert);
    if (r != VK_SUCCESS) return Fail(error, "vkCreateShaderModule(vert)", r);
    r = vkCreateShaderModule(device, &fs, nullptr, &frag);
    if (r != VK_SUCCESS) {
        vkDestroyShaderModule(device, vert, nullptr);
        return Fail(error, "vkCreateShaderModule(frag)", r);
    }

    const VkDescriptorSetLayoutBinding sampler_binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                                       VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    const VkDescriptorSetLayoutCreateInfo set_info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                   .bindingCount = 1, .pBindings = &sampler_binding};
    r = vkCreateDescriptorSetLayout(device, &set_info, nullptr, &set_layout_);
    if (r != VK_SUCCESS) return Fail(error, "vkCreateDescriptorSetLayout", r);
    const VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures};
    const VkDescriptorPoolCreateInfo pool_info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                               .maxSets = kMaxTextures, .poolSizeCount = 1, .pPoolSizes = &pool_size};
    r = vkCreateDescriptorPool(device, &pool_info, nullptr, &pool_);
    if (r != VK_SUCCESS) return Fail(error, "vkCreateDescriptorPool", r);
    const VkSamplerCreateInfo sampler_info{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT, .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT, .maxLod = VK_LOD_CLAMP_NONE};
    r = vkCreateSampler(device, &sampler_info, nullptr, &sampler_);
    if (r != VK_SUCCESS) return Fail(error, "vkCreateSampler", r);

    const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, kPushBytes};
    const VkPipelineLayoutCreateInfo layout_info{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                 .setLayoutCount = 1, .pSetLayouts = &set_layout_,
                                                 .pushConstantRangeCount = 1, .pPushConstantRanges = &push};
    r = vkCreatePipelineLayout(device, &layout_info, nullptr, &layout_);
    if (r != VK_SUCCESS) {
        vkDestroyShaderModule(device, vert, nullptr);
        vkDestroyShaderModule(device, frag, nullptr);
        return Fail(error, "vkCreatePipelineLayout", r);
    }

    const VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vert, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = frag, .pName = "main"},
    };
    const VkVertexInputBindingDescription bindings[2] = {{0, 12, VK_VERTEX_INPUT_RATE_VERTEX},
                                                         {1, 8, VK_VERTEX_INPUT_RATE_VERTEX}};
    const VkVertexInputAttributeDescription attributes[2] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
                                                             {1, 1, VK_FORMAT_R32G32_SFLOAT, 0}};
    const VkPipelineVertexInputStateCreateInfo vertex_input{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 2, .pVertexBindingDescriptions = bindings,
        .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = attributes};
    const VkPipelineInputAssemblyStateCreateInfo assembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    const VkPipelineViewportStateCreateInfo viewport{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    // No culling: winding and handedness of the source data are not established yet.
    const VkPipelineRasterizationStateCreateInfo raster{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f};
    const VkPipelineMultisampleStateCreateInfo multisample{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    const VkPipelineDepthStencilStateCreateInfo depth{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE, .depthCompareOp = VK_COMPARE_OP_LESS};
    const VkPipelineColorBlendAttachmentState blend_attachment{
        .blendEnable = VK_FALSE,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                          VK_COLOR_COMPONENT_A_BIT};
    const VkPipelineColorBlendStateCreateInfo blend{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
        .pAttachments = &blend_attachment};
    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dynamic{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2,
        .pDynamicStates = dynamic_states};
    const VkGraphicsPipelineCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages,
        .pVertexInputState = &vertex_input, .pInputAssemblyState = &assembly, .pViewportState = &viewport,
        .pRasterizationState = &raster, .pMultisampleState = &multisample, .pDepthStencilState = &depth,
        .pColorBlendState = &blend, .pDynamicState = &dynamic, .layout = layout_,
        .renderPass = renderer.RenderPass(), .subpass = 0};
    r = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline_);
    vkDestroyShaderModule(device, vert, nullptr);
    vkDestroyShaderModule(device, frag, nullptr);
    if (r != VK_SUCCESS) {
        pipeline_ = VK_NULL_HANDLE;
        return Fail(error, "vkCreateGraphicsPipelines", r);
    }
    return true;
}

bool MeshRenderer::CreateBuffer(Renderer& renderer, VkDeviceSize size, VkBufferUsageFlags usage,
                                const void* data, Buffer& out, std::string* error) {
    VkDevice device = renderer.Device();
    const VkBufferCreateInfo info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                                  .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkResult r = vkCreateBuffer(device, &info, nullptr, &out.buffer);
    if (r != VK_SUCCESS) return Fail(error, "vkCreateBuffer", r);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, out.buffer, &req);
    const std::uint32_t type = renderer.FindMemoryType(
        req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) {
        if (error) *error = "no host-visible coherent memory type";
        return false;
    }
    const VkMemoryAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                     .allocationSize = req.size, .memoryTypeIndex = type};
    r = vkAllocateMemory(device, &alloc, nullptr, &out.memory);
    if (r != VK_SUCCESS) return Fail(error, "vkAllocateMemory", r);
    r = vkBindBufferMemory(device, out.buffer, out.memory, 0);
    if (r != VK_SUCCESS) return Fail(error, "vkBindBufferMemory", r);
    void* mapped = nullptr;
    r = vkMapMemory(device, out.memory, 0, size, 0, &mapped);
    if (r != VK_SUCCESS) return Fail(error, "vkMapMemory", r);
    std::memcpy(mapped, data, static_cast<std::size_t>(size));
    vkUnmapMemory(device, out.memory);
    return true;
}

void MeshRenderer::DestroyBuffer(VkDevice device, Buffer& buffer) {
    if (buffer.buffer) vkDestroyBuffer(device, buffer.buffer, nullptr);
    if (buffer.memory) vkFreeMemory(device, buffer.memory, nullptr);
    buffer = {};
}

bool MeshRenderer::CreateTexture(Renderer& renderer, const std::vector<formats::RgbaImage>& levels,
                                 Texture& out, std::string* error) {
    VkDevice device = renderer.Device();
    if (levels.empty() || textures_.size() >= kMaxTextures) {
        if (error) *error = "no levels or too many textures";
        return false;
    }
    std::vector<std::uint8_t> staging_bytes;
    std::vector<VkBufferImageCopy> copies;
    for (std::size_t i = 0; i < levels.size(); ++i) {
        const formats::RgbaImage& level = levels[i];
        if (level.rgba.size() != 4ull * level.width * level.height) {
            if (error) *error = "level size mismatch";
            return false;
        }
        VkBufferImageCopy copy{};
        copy.bufferOffset = staging_bytes.size();
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, static_cast<std::uint32_t>(i), 0, 1};
        copy.imageExtent = {level.width, level.height, 1};
        copies.push_back(copy);
        staging_bytes.insert(staging_bytes.end(), level.rgba.begin(), level.rgba.end());
    }
    Buffer staging;
    if (!CreateBuffer(renderer, staging_bytes.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging_bytes.data(),
                      staging, error)) {
        DestroyBuffer(device, staging);
        return false;
    }
    const VkImageCreateInfo image_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {levels[0].width, levels[0].height, 1},
        .mipLevels = static_cast<std::uint32_t>(levels.size()), .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkResult r = vkCreateImage(device, &image_info, nullptr, &out.image);
    if (r != VK_SUCCESS) { DestroyBuffer(device, staging); return Fail(error, "vkCreateImage", r); }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, out.image, &req);
    std::uint32_t type = renderer.FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) type = renderer.FindMemoryType(req.memoryTypeBits, 0);
    const VkMemoryAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                     .allocationSize = req.size, .memoryTypeIndex = type};
    r = vkAllocateMemory(device, &alloc, nullptr, &out.memory);
    if (r == VK_SUCCESS) r = vkBindImageMemory(device, out.image, out.memory, 0);
    if (r != VK_SUCCESS) { DestroyBuffer(device, staging); return Fail(error, "image memory", r); }
    const VkImageViewCreateInfo view_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = out.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, static_cast<std::uint32_t>(levels.size()), 0, 1}};
    r = vkCreateImageView(device, &view_info, nullptr, &out.view);
    if (r != VK_SUCCESS) { DestroyBuffer(device, staging); return Fail(error, "vkCreateImageView", r); }

    // One-off command buffer: undefined -> transfer destination -> copy -> shader read.
    const VkCommandPoolCreateInfo pool_info{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                            .queueFamilyIndex = renderer.QueueFamily()};
    VkCommandPool pool = VK_NULL_HANDLE;
    r = vkCreateCommandPool(device, &pool_info, nullptr, &pool);
    if (r != VK_SUCCESS) { DestroyBuffer(device, staging); return Fail(error, "vkCreateCommandPool", r); }
    const VkCommandBufferAllocateInfo cmd_info{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                               .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                               .commandBufferCount = 1};
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    r = vkAllocateCommandBuffers(device, &cmd_info, &cmd);
    bool ok = r == VK_SUCCESS;
    if (ok) {
        const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                             .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        vkBeginCommandBuffer(cmd, &begin);
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = out.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, static_cast<std::uint32_t>(levels.size()), 0, 1};
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);
        vkCmdCopyBufferToImage(cmd, staging.buffer, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<std::uint32_t>(copies.size()), copies.data());
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);
        vkEndCommandBuffer(cmd);
        const VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
                                  .pCommandBuffers = &cmd};
        r = vkQueueSubmit(renderer.Queue(), 1, &submit, VK_NULL_HANDLE);
        ok = r == VK_SUCCESS;
        if (ok) vkQueueWaitIdle(renderer.Queue());
    }
    vkDestroyCommandPool(device, pool, nullptr);
    DestroyBuffer(device, staging);
    if (!ok) return Fail(error, "texture upload submit", r);

    const VkDescriptorSetAllocateInfo set_alloc{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                .descriptorPool = pool_, .descriptorSetCount = 1,
                                                .pSetLayouts = &set_layout_};
    r = vkAllocateDescriptorSets(device, &set_alloc, &out.set);
    if (r != VK_SUCCESS) return Fail(error, "vkAllocateDescriptorSets", r);
    const VkDescriptorImageInfo image_desc{sampler_, out.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = out.set,
                                     .descriptorCount = 1,
                                     .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                     .pImageInfo = &image_desc};
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    return true;
}

void MeshRenderer::DestroyTexture(VkDevice device, Texture& texture) {
    if (texture.view) vkDestroyImageView(device, texture.view, nullptr);
    if (texture.image) vkDestroyImage(device, texture.image, nullptr);
    if (texture.memory) vkFreeMemory(device, texture.memory, nullptr);
    texture = {}; // the descriptor set is returned when the pool is destroyed
}

bool MeshRenderer::Upload(Renderer& renderer, const std::vector<float>& positions,
                          const std::vector<float>& uvs, const std::vector<std::uint32_t>& indices,
                          const std::vector<MeshRange>& ranges, std::string* error) {
    VkDevice device = renderer.Device();
    if (pipeline_ == VK_NULL_HANDLE || positions.empty() || indices.empty() ||
        uvs.size() / 2 != positions.size() / 3 || ranges.empty()) {
        if (error) *error = "pipeline missing, empty mesh or mismatched UV count";
        return false;
    }
    vkDeviceWaitIdle(device);
    DestroyBuffer(device, vertices_);
    DestroyBuffer(device, uvs_);
    DestroyBuffer(device, indices_);
    index_count_ = 0;
    if (textures_.empty()) { // default texture: warm grey, used until a material gets its own
        Texture fallback;
        formats::RgbaImage px;
        px.width = px.height = 1;
        px.rgba = {190, 160, 130, 255};
        if (!CreateTexture(renderer, {px}, fallback, error)) {
            DestroyTexture(device, fallback);
            return false;
        }
        textures_.push_back(fallback);
    }
    if (!CreateBuffer(renderer, positions.size() * sizeof(float), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                      positions.data(), vertices_, error) ||
        !CreateBuffer(renderer, uvs.size() * sizeof(float), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, uvs.data(),
                      uvs_, error) ||
        !CreateBuffer(renderer, indices.size() * sizeof(std::uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                      indices.data(), indices_, error)) {
        DestroyBuffer(device, vertices_);
        DestroyBuffer(device, uvs_);
        DestroyBuffer(device, indices_);
        return false;
    }
    ranges_ = ranges;
    range_slot_.assign(ranges.size(), 0);
    index_count_ = static_cast<std::uint32_t>(indices.size());
    return true;
}

int MeshRenderer::AddTexture(Renderer& renderer, const std::vector<formats::RgbaImage>& levels,
                             std::string* error) {
    if (textures_.empty()) {
        if (error) *error = "upload the mesh first";
        return -1;
    }
    Texture texture;
    if (!CreateTexture(renderer, levels, texture, error)) {
        DestroyTexture(renderer.Device(), texture);
        return -1;
    }
    textures_.push_back(texture);
    return static_cast<int>(textures_.size()) - 1;
}

void MeshRenderer::BindMaterialTexture(std::uint32_t material_index, int slot) {
    if (slot < 0 || static_cast<std::size_t>(slot) >= textures_.size()) return;
    for (std::size_t i = 0; i < ranges_.size(); ++i) {
        if (ranges_[i].material_index == material_index) range_slot_[i] = slot;
    }
}

void MeshRenderer::Draw(const FrameContext& ctx, const float mvp[16], float min_y, float max_y) const {
    if (!Ready()) return;
    float push[20];
    std::memcpy(push, mvp, sizeof(float) * 16);
    push[16] = min_y;
    push[17] = max_y;
    push[18] = push[19] = 0.0f;
    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdPushConstants(ctx.cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       kPushBytes, push);
    const VkBuffer buffers[2] = {vertices_.buffer, uvs_.buffer};
    const VkDeviceSize offsets[2] = {0, 0};
    vkCmdBindVertexBuffers(ctx.cmd, 0, 2, buffers, offsets);
    vkCmdBindIndexBuffer(ctx.cmd, indices_.buffer, 0, VK_INDEX_TYPE_UINT32);
    int bound = -1;
    for (std::size_t i = 0; i < ranges_.size(); ++i) {
        if (ranges_[i].index_count == 0) continue;
        if (range_slot_[i] != bound) {
            bound = range_slot_[i];
            vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1,
                                    &textures_[static_cast<std::size_t>(bound)].set, 0, nullptr);
        }
        vkCmdDrawIndexed(ctx.cmd, ranges_[i].index_count, 1, ranges_[i].first_index, 0, 0);
    }
}

void MeshRenderer::Release(Renderer& renderer) {
    VkDevice device = renderer.Device();
    if (device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(device);
    DestroyBuffer(device, vertices_);
    DestroyBuffer(device, uvs_);
    DestroyBuffer(device, indices_);
    for (Texture& t : textures_) DestroyTexture(device, t);
    textures_.clear();
    if (pool_) vkDestroyDescriptorPool(device, pool_, nullptr);
    if (sampler_) vkDestroySampler(device, sampler_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(device, set_layout_, nullptr);
    if (pipeline_) vkDestroyPipeline(device, pipeline_, nullptr);
    if (layout_) vkDestroyPipelineLayout(device, layout_, nullptr);
    pool_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
    set_layout_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    index_count_ = 0;
}

} // namespace ember::gfx
