// Headless rendering test: draws a triangle with dynamic rendering into an
// offscreen image, reads it back and checks real pixel values. Exit code 0 only
// if the pixels match and the validation layer reported no errors.
#include "vk_device.h"
#include "vk_frame.h"
#include "vk_log.h"

#include "triangle_frag.h"
#include "triangle_vert.h"

#include <cstdio>
#include <cstring>

using namespace bf::vk;

namespace {

constexpr uint32_t kSize = 64;
constexpr VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;

VkShaderModule MakeModule(VkDevice d, const uint32_t* code, size_t bytes) {
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = bytes;
  ci.pCode = code;
  VkShaderModule m = VK_NULL_HANDLE;
  vkCreateShaderModule(d, &ci, nullptr, &m);
  return m;
}

void Barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to,
             VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
             VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
  VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b.srcStageMask = src_stage;
  b.srcAccessMask = src_access;
  b.dstStageMask = dst_stage;
  b.dstAccessMask = dst_access;
  b.oldLayout = from;
  b.newLayout = to;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = img;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dep.imageMemoryBarrierCount = 1;
  dep.pImageMemoryBarriers = &b;
  vkCmdPipelineBarrier2(cmd, &dep);
}

bool Near(const uint8_t* px, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  auto n = [](int x, int y) { return x - y <= 1 && y - x <= 1; };
  return n(px[0], r) && n(px[1], g) && n(px[2], b) && n(px[3], a);
}

}  // namespace

int main(int argc, char** argv) {
  DeviceOptions opts;
  opts.headless = true;
  opts.validation = true;
  opts.pipeline_cache_path = "vk_headless_test.pipelinecache";
  for (int i = 1; i < argc; ++i)
    if (!std::strcmp(argv[i], "--no-validation")) opts.validation = false;

  Device dev;
  if (!dev.Init(opts)) return 2;
  VkDevice d = dev.device();
  FrameRing frames;
  if (!frames.Init(&dev)) return 2;

  // Render target.
  VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = kFormat;
  ici.extent = {kSize, kSize, 1};
  ici.mipLevels = ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  VmaAllocationCreateInfo aci{};
  aci.usage = VMA_MEMORY_USAGE_AUTO;
  VkImage image;
  VmaAllocation image_mem;
  if (vmaCreateImage(dev.allocator(), &ici, &aci, &image, &image_mem, nullptr) != VK_SUCCESS)
    return 2;
  VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.image = image;
  vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vci.format = kFormat;
  vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkImageView view;
  vkCreateImageView(d, &vci, nullptr, &view);

  // Readback buffer.
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = kSize * kSize * 4;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VmaAllocationCreateInfo baci{};
  baci.usage = VMA_MEMORY_USAGE_AUTO;
  baci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
  VkBuffer readback;
  VmaAllocation readback_mem;
  VmaAllocationInfo readback_info;
  if (vmaCreateBuffer(dev.allocator(), &bci, &baci, &readback, &readback_mem, &readback_info) !=
      VK_SUCCESS)
    return 2;

  // Pipeline (dynamic rendering, no render pass objects).
  VkShaderModule vs = MakeModule(d, triangle_vert, sizeof(triangle_vert));
  VkShaderModule fs = MakeModule(d, triangle_frag, sizeof(triangle_frag));
  VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
  VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.pushConstantRangeCount = 1;
  plci.pPushConstantRanges = &pcr;
  VkPipelineLayout layout;
  vkCreatePipelineLayout(d, &plci, nullptr, &layout);

  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vs;
  stages[0].pName = "main";
  stages[1] = stages[0];
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fs;
  VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState cba{};
  cba.colorWriteMask = 0xF;
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1;
  cb.pAttachments = &cba;
  VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  ds.dynamicStateCount = 2;
  ds.pDynamicStates = dyn;
  VkPipelineRenderingCreateInfo prci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  prci.colorAttachmentCount = 1;
  prci.pColorAttachmentFormats = &kFormat;
  VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gpci.pNext = &prci;
  gpci.stageCount = 2;
  gpci.pStages = stages;
  gpci.pVertexInputState = &vin;
  gpci.pInputAssemblyState = &ia;
  gpci.pViewportState = &vp;
  gpci.pRasterizationState = &rs;
  gpci.pMultisampleState = &ms;
  gpci.pColorBlendState = &cb;
  gpci.pDynamicState = &ds;
  gpci.layout = layout;
  VkPipeline pipeline;
  if (vkCreateGraphicsPipelines(d, dev.pipeline_cache(), 1, &gpci, nullptr, &pipeline) !=
      VK_SUCCESS)
    return 2;

  // Record: clear to blue, draw red triangle, copy to buffer.
  VkCommandBuffer cmd = frames.Begin();
  Barrier(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
  VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  att.imageView = view;
  att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  att.clearValue.color = {{0.0f, 0.0f, 1.0f, 1.0f}};
  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea = {{0, 0}, {kSize, kSize}};
  ri.layerCount = 1;
  ri.colorAttachmentCount = 1;
  ri.pColorAttachments = &att;
  vkCmdBeginRendering(cmd, &ri);
  VkViewport viewport{0, 0, float(kSize), float(kSize), 0, 1};
  VkRect2D scissor{{0, 0}, {kSize, kSize}};
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  vkCmdSetScissor(cmd, 0, 1, &scissor);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  float red[4] = {1, 0, 0, 1};
  vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, red);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  vkCmdEndRendering(cmd);
  Barrier(cmd, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
          VK_ACCESS_2_TRANSFER_READ_BIT);
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {kSize, kSize, 1};
  vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &region);
  VkMemoryBarrier2 hb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  hb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
  hb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  hb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
  hb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
  VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dep.memoryBarrierCount = 1;
  dep.pMemoryBarriers = &hb;
  vkCmdPipelineBarrier2(cmd, &dep);
  bool ok = frames.Submit(VK_NULL_HANDLE, VK_PIPELINE_STAGE_2_NONE) && frames.WaitIdle();

  vmaInvalidateAllocation(dev.allocator(), readback_mem, 0, VK_WHOLE_SIZE);
  const uint8_t* px = static_cast<const uint8_t*>(readback_info.pMappedData);
  const uint8_t* center = px + ((kSize / 2) * kSize + kSize / 2) * 4;
  const uint8_t* corner = px;
  bool center_ok = Near(center, 255, 0, 0, 255);
  bool corner_ok = Near(corner, 0, 0, 255, 255);
  std::printf("center = %u,%u,%u,%u (want 255,0,0,255) %s\n", center[0], center[1], center[2],
              center[3], center_ok ? "OK" : "FAIL");
  std::printf("corner = %u,%u,%u,%u (want 0,0,255,255) %s\n", corner[0], corner[1], corner[2],
              corner[3], corner_ok ? "OK" : "FAIL");
  ok = ok && center_ok && corner_ok;

  vkDestroyPipeline(d, pipeline, nullptr);
  vkDestroyPipelineLayout(d, layout, nullptr);
  vkDestroyShaderModule(d, vs, nullptr);
  vkDestroyShaderModule(d, fs, nullptr);
  vkDestroyImageView(d, view, nullptr);
  vmaDestroyImage(dev.allocator(), image, image_mem);
  vmaDestroyBuffer(dev.allocator(), readback, readback_mem);
  frames.Shutdown();
  uint32_t errors = dev.validation_errors();
  dev.Shutdown();
  std::printf("validation errors: %u\n", errors);
  ok = ok && errors == 0;
  std::printf("%s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
