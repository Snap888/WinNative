/* Stand-in for framegen_engine.c when the compositor is built without the Winlator tree. */
#include "framegen_engine.h"

const void *fge_probe(PFN_vkGetInstanceProcAddr gipa, VkInstance inst, VkPhysicalDevice pd,
                      VkFormat ring_fmt) {
    (void)gipa; (void)inst; (void)pd; (void)ring_fmt; return NULL;
}
void fge_device_ready(PFN_vkGetInstanceProcAddr gipa, VkInstance inst, VkPhysicalDevice pd,
                      VkDevice dev, VkQueue queue, uint32_t qfam, int features_enabled) {
    (void)gipa; (void)inst; (void)pd; (void)dev; (void)queue; (void)qfam; (void)features_enabled;
}
int fge_caps_ok(int kind) { (void)kind; return 0; }
const char *fge_caps_reason(void) { return "frame-generation engines not built into this compositor"; }
const char *fge_caps_reason_for(int kind) { (void)kind; return fge_caps_reason(); }
void fge_device_lost(void) {}
void fge_reset_history(void) {}
void fge_set_candidate_reconstruction(int mode) { (void)mode; }
void fge_set_ai_mode(int mode) { (void)mode; }
void fge_set_debug_flow(int enabled) { (void)enabled; }
void fge_set_debug_mode(int mode) { (void)mode; }
void fge_debug_into(VkCommandBuffer cmd, VkImage output, VkImageView output_view,
                    uint32_t width, uint32_t height) {
    (void)cmd; (void)output; (void)output_view; (void)width; (void)height;
}
int fge_format_ok(VkFormat fmt) { (void)fmt; return 0; }
int fge_start(int kind, const char *cache_path) { (void)kind; (void)cache_path; return -1; }
void fge_stop(void) {}
int fge_unavailable(void) { return 1; }
void fge_configure(uint32_t multiplier, float flow_scale, float refresh_hz, int flow_min_side,
                   int target_fps) {
    (void)multiplier; (void)flow_scale; (void)refresh_hz; (void)flow_min_side; (void)target_fps;
}
uint32_t fge_effective_multiplier(void) { return 2u; }
void fge_set_wsi_feedback(uint32_t requested_multiplier, uint32_t current_multiplier,
                          float delivery_ratio, float acquire_pressure,
                          float submit_pressure, float present_pressure) {
    (void)requested_multiplier; (void)current_multiplier; (void)delivery_ratio;
    (void)acquire_pressure; (void)submit_pressure; (void)present_pressure;
}
int fge_prepare(uint32_t w, uint32_t h, VkFormat fmt) { (void)w; (void)h; (void)fmt; return 0; }
uint32_t fge_plan(uint32_t capacity, uint64_t source_frames, float presented_rate) {
    (void)capacity; (void)source_frames; (void)presented_rate; return 0;
}
uint32_t fge_process(VkCommandBuffer cmd, VkImage source, VkImageView source_view,
                     uint32_t w, uint32_t h, VkFormat format, uint32_t gens,
                     const SnapFrameVulkanOutputSlot *outputs) {
    (void)cmd; (void)source; (void)source_view; (void)w; (void)h;
    (void)format; (void)gens; (void)outputs; return 0u;
}
void fge_generate(VkCommandBuffer cmd, uint32_t g, uint32_t count, VkImage img, VkImageView view,
                  uint32_t w, uint32_t h) {
    (void)cmd; (void)g; (void)count; (void)img; (void)view; (void)w; (void)h;
}
void fge_forget_targets(void) {}
void fge_telemetry(float *accepted, float *source_rate, float *thermal) {
    *accepted = 0.0f; *source_rate = 0.0f; *thermal = -1.0f;
}
int fge_flow_metrics(float metrics[4]) { (void)metrics; return 0; }
const char *fge_engine_name(int kind) { return kind == 1 ? "WFG Native" : kind == 2 ? "SnapFrame Vulkan" : "LSFG Native"; }
const char *fge_build_info(void) { return "stub"; }
