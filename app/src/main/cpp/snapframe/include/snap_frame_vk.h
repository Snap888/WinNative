#ifndef SNAP_FRAME_VK_H
#define SNAP_FRAME_VK_H
#include <stdint.h>
#include <vulkan/vulkan.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct SnapFrameVk SnapFrameVk;
SnapFrameVk *snap_frame_vk_create(VkDevice device, VkPhysicalDevice physical_device);
void snap_frame_vk_destroy(SnapFrameVk *engine);
/* Device-loss path: releases host memory only; must not call Vulkan on a lost device. */
void snap_frame_vk_abandon(SnapFrameVk *engine);
int snap_frame_vk_prepare(SnapFrameVk *engine, uint32_t width, uint32_t height, VkFormat format, uint32_t flow_scale_percent);
int snap_frame_vk_has_history(const SnapFrameVk *engine);
void snap_frame_vk_reset_history(SnapFrameVk *engine);
/* Non-blocking timestamps from the last completed query slot; values are milliseconds. */
int snap_frame_vk_get_telemetry(const SnapFrameVk *engine, float stages_ms[5], float *total_ms);
int snap_frame_vk_process(SnapFrameVk *engine, VkCommandBuffer command_buffer, VkImage source, VkImageView source_view, uint32_t generation_count);
void snap_frame_vk_generate(SnapFrameVk *engine, VkCommandBuffer command_buffer, uint32_t phase_index, uint32_t phase_count, VkImage output, VkImageView output_view);
#ifdef __cplusplus
}
#endif
#endif
