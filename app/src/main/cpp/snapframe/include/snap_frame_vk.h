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
/* Candidate reconstruction mode: 0=AI automatic, 1=forced off, 2=forced on. */
void snap_frame_vk_set_candidate_reconstruction(SnapFrameVk *engine, uint32_t mode);
/* Non-blocking timestamps from the last completed query slot; values are milliseconds. */
int snap_frame_vk_get_telemetry(const SnapFrameVk *engine, float stages_ms[6], float *total_ms);
/* Feed the previous frame's actual WSI delivery to Snap AI; pressure values are normalized 0..1. */
void snap_frame_vk_set_wsi_feedback(SnapFrameVk *engine, uint32_t requested_multiplier,
                                    uint32_t current_multiplier, float delivery_ratio,
                                    float acquire_pressure, float submit_pressure,
                                    float present_pressure);
/* 0 until AI has a recommendation; otherwise the effective multiplier (2..4). */
uint32_t snap_frame_vk_get_recommended_multiplier(const SnapFrameVk *engine);
/* Last completed GPU-reduced metrics: motion, complexity, FB-weighted confidence,
 * and forward-confidence inconsistency risk (a diagnostic proxy, not ground truth). */
int snap_frame_vk_get_flow_metrics(const SnapFrameVk *engine, float metrics[4]);
int snap_frame_vk_process(SnapFrameVk *engine, VkCommandBuffer command_buffer, VkImage source, VkImageView source_view, uint32_t generation_count);
void snap_frame_vk_generate(SnapFrameVk *engine, VkCommandBuffer command_buffer, uint32_t phase_index, uint32_t phase_count, VkImage output, VkImageView output_view);
/* Select debug visualization: 0=motion, 1=matcher confidence, 2=FB consistency. */
void snap_frame_vk_set_debug_mode(SnapFrameVk *engine, uint32_t mode);
/* Render the engine's own flow/confidence/consistency into an RGBA8 debug target. */
void snap_frame_vk_debug_into(SnapFrameVk *engine, VkCommandBuffer command_buffer,
                              VkImage output, VkImageView output_view,
                              uint32_t width, uint32_t height);
#ifdef __cplusplus
}
#endif
#endif
