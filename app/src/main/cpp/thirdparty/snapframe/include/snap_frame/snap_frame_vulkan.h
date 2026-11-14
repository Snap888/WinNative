#ifndef SNAP_FRAME_VULKAN_INTEROP_H
#define SNAP_FRAME_VULKAN_INTEROP_H

/*
 * SnapFrame Vulkan host interop, API v1.
 *
 * This is the native_frame payload used by a Vulkan SnapFrameBackendV1 adapter.
 * The host owns every Vulkan object in this structure. The backend may record
 * commands into command_buffer and write only to output slots explicitly
 * supplied by the host. It must not create a second VkDevice/queue, submit the
 * host command buffer, wait for GPU completion, or destroy host objects.
 *
 * All handles are borrowed for the duration of process_frame only.
 */
#include <stdint.h>
#include <vulkan/vulkan.h>
#include "snap_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SNAP_FRAME_VULKAN_INTEROP_VERSION 1u

typedef struct SnapFrameVulkanOutputSlot {
  VkImage image;
  VkImageView view;
  VkImageLayout layout;       /* expected initial/final layout for this callback */
  VkImageLayout final_layout; /* backend must transition to this before returning */
} SnapFrameVulkanOutputSlot;

typedef struct SnapFrameVulkanFrame {
  uint32_t struct_size;
  uint32_t interop_version;

  VkPhysicalDevice physical_device;
  VkDevice device;
  VkQueue queue;              /* host queue; backend must not submit it */
  uint32_t queue_family_index;
  VkCommandBuffer command_buffer; /* recording and owned by host */

  VkImage source_image;
  VkImageView source_view;
  VkImageLayout source_layout;
  VkFormat source_format;
  VkExtent2D extent;

  uint32_t output_capacity;  /* must be <= SNAP_FRAME_MAX_MULTIPLIER - 1 */
  SnapFrameVulkanOutputSlot outputs[SNAP_FRAME_MAX_MULTIPLIER - 1];

  uint64_t frame_id;
  uint64_t timestamp_ns;
  uint32_t scene_cut;
  uint32_t flags;
} SnapFrameVulkanFrame;

/* Validate a native_frame payload before interpreting it as
 * SnapFrameVulkanFrame. The struct_size must cover the complete v1 structure. */
static inline int snap_frame_vulkan_frame_valid(const SnapFrameFrameInput* input) {
  if (!input || input->struct_size < sizeof(SnapFrameFrameInput) ||
      input->native_frame == (uintptr_t)0) return 0;
  const SnapFrameVulkanFrame* frame =
      (const SnapFrameVulkanFrame*)input->native_frame;
  return frame->struct_size >= sizeof(SnapFrameVulkanFrame) &&
      frame->interop_version == SNAP_FRAME_VULKAN_INTEROP_VERSION &&
      frame->device != VK_NULL_HANDLE &&
      frame->command_buffer != VK_NULL_HANDLE &&
      frame->source_image != VK_NULL_HANDLE &&
      frame->source_view != VK_NULL_HANDLE &&
      frame->extent.width != 0 && frame->extent.height != 0 &&
      frame->output_capacity <= SNAP_FRAME_MAX_MULTIPLIER - 1;
}

#ifdef __cplusplus
}
#endif
#endif
