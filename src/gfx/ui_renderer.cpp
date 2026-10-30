// SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx/ui_renderer.h"

#include <cstring>

#include "gfx/camera.h"
#include "ui/font.h"

namespace ember::gfx {
namespace {

const std::uint32_t kVertSpirv[] =
#include "ui.vert.inc"
    ;
const std::uint32_t kFragSpirv[] =
#include "ui.frag.inc"
    ;

constexpr VkDeviceSize kBufferBytes = ui::UiBatch::kMaxQuads * 6 * sizeof(ui::UiVertex);

bool Fail(std::string* error, const char* what, VkResult r) {
    if (error) *error = std::string(what) + " failed (" + std::to_string(static_cast<int>(r)) + ")";
    return false;
}

} // namespace

bool UiRenderer::Initialize(Renderer& renderer, std::string* error) {
    VkDevice device = renderer.Device();
    if (device == VK_NULL_HANDLE) {
        if (error) *error = "no device yet";
        return false;
    }
    const ui::FontAtlas& atlas = ui::Font();

    // Atlas image + staging copy.
    const VkDeviceSize atlas_bytes = atlas.rgba.size();
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    const auto host_buffer = [&](VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer,
                                 VkDeviceMemory& memory, void** mapped) {
        const VkBufferCreateInfo info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                                      .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        VkResult r = vkCreateBuffer(device, &info, nullptr, &buffer);
        if (r != VK_SUCCESS) return Fail(error, "vkCreateBuffer", r);
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, buffer, &req);
        const std::uint32_t type = renderer.FindMemoryType(
            req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (type == UINT32_MAX) {
            if (error) *error = "no host-visible memory";
            return false;
        }
        const VkMemoryAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                         .allocationSize = req.size, .memoryTypeIndex = type};
        r = vkAllocateMemory(device, &alloc, nullptr, &memory);
        if (r == VK_SUCCESS) r = vkBindBufferMemory(device, buffer, memory, 0);
        if (r == VK_SUCCESS) r = vkMapMemory(device, memory, 0, size, 0, mapped);
        return r == VK_SUCCESS ? true : Fail(error, "buffer memory", r);
    };
    void* staging_ptr = nullptr;
    if (!host_buffer(atlas_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging, staging_memory, &staging_ptr)) {
        return false;
    }
    std::memcpy(staging_ptr, atlas.rgba.data(), atlas.rgba.size());

    const VkImageCreateInfo image_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {static_cast<std::uint32_t>(atlas.width), static_cast<std::uint32_t>(atlas.height), 1},
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkResult r = vkCreateImage(device, &image_info, nullptr, &image_);
    if (r != VK_SUCCESS) return Fail(error, "vkCreateImage", r);
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, image_, &req);
    std::uint32_t type = renderer.FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) type = renderer.FindMemoryType(req.memoryTypeBits, 0);
    const VkMemoryAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                     .allocationSize = req.size, .memoryTypeIndex = type};
    r = vkAllocateMemory(device, &alloc, nullptr, &image_memory_);
    if (r == VK_SUCCESS) r = vkBindImageMemory(device, image_, image_memory_, 0);
    if (r != VK_SUCCESS) return Fail(error, "atlas memory", r);
    const VkImageViewCreateInfo view_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image_, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    r = vkCreateImageView(device, &view_info, nullptr, &view_);
    if (r != VK_SUCCESS) return Fail(error, "vkCreateImageView", r);

    const VkCommandPoolCreateInfo pool_info{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                            .queueFamilyIndex = renderer.QueueFamily()};
    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    r = vkCreateCommandPool(device, &pool_info, nullptr, &cmd_pool);
    if (r != VK_SUCCESS) return Fail(error, "vkCreateCommandPool", r);
    const VkCommandBufferAllocateInfo cmd_info{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                               .commandPool = cmd_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                               .commandBufferCount = 1};
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    r = vkAllocateCommandBuffers(device, &cmd_info, &cmd);
    if (r == VK_SUCCESS) {
        const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                              .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        vkBeginCommandBuffer(cmd, &begin);
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image_;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {static_cast<std::uint32_t>(atlas.width), static_cast<std::uint32_t>(atlas.height), 1};
        vkCmdCopyBufferToImage(cmd, staging, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
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
        if (r == VK_SUCCESS) vkQueueWaitIdle(renderer.Queue());
    }
    vkDestroyCommandPool(device, cmd_pool, nullptr);
    vkDestroyBuffer(device, staging, nullptr);
    vkFreeMemory(device, staging_memory, nullptr);
    if (r != VK_SUCCESS) return Fail(error, "atlas upload", r);

    for (std::uint32_t i = 0; i < kSlots; ++i) {
        if (!host_buffer(kBufferBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, buffers_[i], memories_[i], &mapped_[i])) {
            return false;
        }
    }

    const VkSamplerCreateInfo sampler_info{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    r = vkCreateSampler(device, &sampler_info, nullptr, &sampler_);
    if (r != VK_SUCCESS) return Fail(error, "vkCreateSampler", r);
    const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                               VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    const VkDescriptorSetLayoutCreateInfo set_info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                   .bindingCount = 1, .pBindings = &binding};
    r = vkCreateDescriptorSetLayout(device, &set_info, nullptr, &set_layout_);
    if (r != VK_SUCCESS) return Fail(error, "vkCreateDescriptorSetLayout", r);
    const VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    const VkDescriptorPoolCreateInfo dpool{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                           .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &pool_size};
    r = vkCreateDescriptorPool(device, &dpool, nullptr, &pool_);
    if (r != VK_SUCCESS) return Fail(error, "vkCreateDescriptorPool", r);
    const VkDescriptorSetAllocateInfo set_alloc{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                .descriptorPool = pool_, .descriptorSetCount = 1,
                                                .pSetLayouts = &set_layout_};
    r = vkAllocateDescriptorSets(device, &set_alloc, &set_);
    if (r != VK_SUCCESS) return Fail(error, "vkAllocateDescriptorSets", r);
    const VkDescriptorImageInfo image_desc{sampler_, view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set_,
                                     .descriptorCount = 1,
                                     .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                     .pImageInfo = &image_desc};
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    const VkShaderModuleCreateInfo vs{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                      .codeSize = sizeof(kVertSpirv), .pCode = kVertSpirv};
    const VkShaderModuleCreateInfo fs{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                      .codeSize = sizeof(kFragSpirv), .pCode = kFragSpirv};
    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
    r = vkCreateShaderModule(device, &vs, nullptr, &vert);
    if (r == VK_SUCCESS) r = vkCreateShaderModule(device, &fs, nullptr, &frag);
    if (r != VK_SUCCESS) {
        if (vert) vkDestroyShaderModule(device, vert, nullptr);
        return Fail(error, "vkCreateShaderModule", r);
    }
    const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, 6 * sizeof(float)};
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
         .module = frag, .pName = "main"}};
    const VkVertexInputBindingDescription vb{0, sizeof(ui::UiVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attrs[3] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(ui::UiVertex, x)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(ui::UiVertex, u)},
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(ui::UiVertex, color)}};
    const VkPipelineVertexInputStateCreateInfo vertex_input{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &vb, .vertexAttributeDescriptionCount = 3, .pVertexAttributeDescriptions = attrs};
    const VkPipelineInputAssemblyStateCreateInfo assembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    const VkPipelineViewportStateCreateInfo viewport{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    const VkPipelineRasterizationStateCreateInfo raster{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f};
    const VkPipelineMultisampleStateCreateInfo multisample{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    const VkPipelineDepthStencilStateCreateInfo depth{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    const VkPipelineColorBlendAttachmentState blend_attachment{
        .blendEnable = VK_TRUE, .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
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

void UiRenderer::Draw(const FrameContext& ctx, const ui::UiBatch& batch, float layout_width,
                      float layout_height, int pre_rotation_quarter_turns) {
    const auto& vertices = batch.Vertices();
    if (!Ready() || vertices.empty() || ctx.frame_slot >= kSlots) return;
    std::memcpy(mapped_[ctx.frame_slot], vertices.data(), vertices.size() * sizeof(ui::UiVertex));
    const Mat4 rot = PreRotation(pre_rotation_quarter_turns);
    const float push[6] = {rot[0], rot[1], rot[4], rot[5], layout_width, layout_height};
    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdPushConstants(ctx.cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), push);
    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set_, 0, nullptr);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(ctx.cmd, 0, 1, &buffers_[ctx.frame_slot], &offset);
    vkCmdDraw(ctx.cmd, static_cast<std::uint32_t>(vertices.size()), 1, 0, 0);
}

void UiRenderer::Release(Renderer& renderer) {
    VkDevice device = renderer.Device();
    if (device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(device);
    for (std::uint32_t i = 0; i < kSlots; ++i) {
        if (mapped_[i]) vkUnmapMemory(device, memories_[i]);
        if (buffers_[i]) vkDestroyBuffer(device, buffers_[i], nullptr);
        if (memories_[i]) vkFreeMemory(device, memories_[i], nullptr);
        buffers_[i] = VK_NULL_HANDLE;
        memories_[i] = VK_NULL_HANDLE;
        mapped_[i] = nullptr;
    }
    if (view_) vkDestroyImageView(device, view_, nullptr);
    if (image_) vkDestroyImage(device, image_, nullptr);
    if (image_memory_) vkFreeMemory(device, image_memory_, nullptr);
    if (pool_) vkDestroyDescriptorPool(device, pool_, nullptr);
    if (sampler_) vkDestroySampler(device, sampler_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(device, set_layout_, nullptr);
    if (pipeline_) vkDestroyPipeline(device, pipeline_, nullptr);
    if (layout_) vkDestroyPipelineLayout(device, layout_, nullptr);
    view_ = VK_NULL_HANDLE; image_ = VK_NULL_HANDLE; image_memory_ = VK_NULL_HANDLE; pool_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE; set_layout_ = VK_NULL_HANDLE; pipeline_ = VK_NULL_HANDLE; layout_ = VK_NULL_HANDLE;
}

} // namespace ember::gfx
