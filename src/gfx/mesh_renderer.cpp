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

    const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, kPushBytes};
    const VkPipelineLayoutCreateInfo layout_info{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
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
    const VkVertexInputBindingDescription binding{0, 12, VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attribute{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
    const VkPipelineVertexInputStateCreateInfo vertex_input{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = 1, .pVertexAttributeDescriptions = &attribute};
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

bool MeshRenderer::Upload(Renderer& renderer, const std::vector<float>& positions,
                          const std::vector<std::uint32_t>& indices, std::string* error) {
    VkDevice device = renderer.Device();
    if (pipeline_ == VK_NULL_HANDLE || positions.empty() || indices.empty()) {
        if (error) *error = "pipeline missing or empty mesh";
        return false;
    }
    vkDeviceWaitIdle(device);
    DestroyBuffer(device, vertices_);
    DestroyBuffer(device, indices_);
    index_count_ = 0;
    if (!CreateBuffer(renderer, positions.size() * sizeof(float), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                      positions.data(), vertices_, error) ||
        !CreateBuffer(renderer, indices.size() * sizeof(std::uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                      indices.data(), indices_, error)) {
        DestroyBuffer(device, vertices_);
        DestroyBuffer(device, indices_);
        return false;
    }
    index_count_ = static_cast<std::uint32_t>(indices.size());
    return true;
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
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(ctx.cmd, 0, 1, &vertices_.buffer, &offset);
    vkCmdBindIndexBuffer(ctx.cmd, indices_.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(ctx.cmd, index_count_, 1, 0, 0, 0);
}

void MeshRenderer::Release(Renderer& renderer) {
    VkDevice device = renderer.Device();
    if (device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(device);
    DestroyBuffer(device, vertices_);
    DestroyBuffer(device, indices_);
    if (pipeline_) vkDestroyPipeline(device, pipeline_, nullptr);
    if (layout_) vkDestroyPipelineLayout(device, layout_, nullptr);
    pipeline_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    index_count_ = 0;
}

} // namespace ember::gfx
