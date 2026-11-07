/* Runs the X11 renderer's frame-generation engines on the compositor's Turnip device. */
#include "vk_dispatch.h"

#include "framegen_engine.h"
#include "framegen_bridge.h"
#include "snap_frame_vk.h"
#include "snap_frame/snap_frame.h"
#include "snap_frame/snap_frame_vulkan.h"
#include "vkr_dis.h"
#include "lsfg/vkr_lsfg.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#define DIS_FLOW_MIN_SIDE_DEFAULT 180u

static VkPhysicalDevice g_pd;
static VkDevice g_dev;
static VkrLsfg *g_lsfg;
static VkrDis *g_dis;
static SnapFrameVk *g_snap;
static SnapFrameEngine *g_snap_core;
typedef struct SnapFrameHost { SnapFrameVk *vk; uint32_t multiplier; } SnapFrameHost;
static SnapFrameHost g_snap_host;
static uint64_t g_snap_frame_id;
static VkQueue g_queue;
static uint32_t g_qfam;
static pthread_mutex_t g_snap_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_kind = -1;
static int g_chain_ok;
static int g_chain_failed;

static VkImage g_source;
static uint64_t g_last_plan_ns;

static uint32_t g_multiplier = 2;
static float g_flow_scale = 0.7f;
static float g_refresh_hz;
static uint32_t g_dis_min_side = DIS_FLOW_MIN_SIDE_DEFAULT;
static uint32_t g_target_fps;

static int g_caps_lsfg, g_caps_dis, g_caps_snapframe;
static const char *g_reason = "not probed";

static int format_supports(VkFormat fmt, VkFormatFeatureFlags want) {
    if (!g_pd || !vkd.GetPhysicalDeviceFormatProperties) return 0;
    VkFormatProperties props;
    memset(&props, 0, sizeof(props));
    vkd.GetPhysicalDeviceFormatProperties(g_pd, fmt, &props);
    return (props.optimalTilingFeatures & want) == want;
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

const void *fge_probe(PFN_vkGetInstanceProcAddr gipa, VkInstance inst, VkPhysicalDevice pd,
                      VkFormat ring_fmt) {
    g_pd = pd;
    if (!vkd_bind_proc(gipa, inst)) {
        g_caps_lsfg = g_caps_dis = 0;
        g_reason = "the compositor's Vulkan entry points could not be resolved";
        return NULL;
    }

    const VkFormatFeatureFlags ring_want =
            VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT
            | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if (!format_supports(ring_fmt, ring_want)) {
        g_caps_lsfg = g_caps_dis = 0;
        g_reason = "the driver cannot store to the generation ring format";
        return NULL;
    }

    /* LSFG keeps its motion pyramid in half-float; DIS needs only the ring. */
    g_caps_lsfg = format_supports(VK_FORMAT_R16G16B16A16_SFLOAT,
                                  VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT
                                  | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    g_caps_dis = 1;
    VkPhysicalDeviceProperties snap_props; memset(&snap_props,0,sizeof(snap_props));
    vkd.GetPhysicalDeviceProperties(pd,&snap_props);
    g_caps_snapframe = snap_props.limits.timestampComputeAndGraphics &&
        format_supports(VK_FORMAT_R16_SFLOAT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) &&
        format_supports(VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) &&
        format_supports(VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) &&
        format_supports(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    g_reason = g_caps_lsfg ? "supported" : "the driver has no storage half-float images";

    /* Neither engine needs a feature enabled at device creation. */
    return NULL;
}

void fge_device_ready(PFN_vkGetInstanceProcAddr gipa, VkInstance inst, VkPhysicalDevice pd,
                      VkDevice dev, VkQueue queue, uint32_t qfam, int features_enabled) {
    (void)features_enabled;
    g_pd = pd;
    g_dev = dev; g_queue = queue; g_qfam = qfam;
    if (!vkd.CreateInstance) vkd_bind_proc(gipa, inst);
}

int fge_caps_ok(int kind) {
    if (!g_dev) return 0;
    if (kind == VKP_FG_ENGINE_DIS) return g_caps_dis;
    if (kind == VKP_FG_ENGINE_SNAPFRAME) return g_caps_snapframe;
    return g_caps_lsfg;
}

const char *fge_caps_reason(void) { return g_reason; }
const char *fge_caps_reason_for(int kind) {
    if (kind == VKP_FG_ENGINE_SNAPFRAME) return g_caps_snapframe ? "SnapFrame Vulkan compute formats supported" : "Turnip lacks R16/RG16 flow storage formats, sampled RGBA formats or compute timestamp queries required by SnapFrame";
    if (kind == VKP_FG_ENGINE_DIS) return g_caps_dis ? "WFG/DIS supported" : "the compositor cannot create its generation ring";
    return g_reason;
}

int fge_format_ok(VkFormat fmt) {
    return format_supports(fmt, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT
                                | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
}

static SnapFrameStatus snap_backend_initialize(void *user,const SnapFrameEngineConfig *cfg){
    SnapFrameHost*h=(SnapFrameHost*)user;if(!h||!h->vk||!cfg)return SNAP_FRAME_ERROR_INVALID_ARGUMENT;
    h->multiplier=cfg->multiplier;return SNAP_FRAME_OK;
}
static void snap_backend_shutdown(void *user){(void)user;}
static SnapFrameStatus snap_backend_update(void *user,const SnapFrameEngineConfig *cfg){
    SnapFrameHost*h=(SnapFrameHost*)user;if(!h||!cfg)return SNAP_FRAME_ERROR_INVALID_ARGUMENT;
    h->multiplier=cfg->multiplier;return SNAP_FRAME_OK;
}
static void snap_backend_reset(void *user,uint32_t reason){
    (void)reason;SnapFrameHost*h=(SnapFrameHost*)user;if(h&&h->vk)snap_frame_vk_reset_history(h->vk);
}
static SnapFrameStatus snap_backend_process(void *user,const SnapFrameFrameInput *input,SnapFrameFrameOutput *output){
    SnapFrameHost*h=(SnapFrameHost*)user;
    if(!h||!h->vk||!output||!snap_frame_vulkan_frame_valid(input))return SNAP_FRAME_ERROR_INVALID_ARGUMENT;
    const SnapFrameVulkanFrame*f=(const SnapFrameVulkanFrame*)input->native_frame;
    for(uint32_t i=0;i<f->output_capacity;i++)if(!f->outputs[i].image||!f->outputs[i].view)return SNAP_FRAME_ERROR_INVALID_ARGUMENT;
    int ready=snap_frame_vk_process(h->vk,f->command_buffer,f->source_image,f->source_view,f->output_capacity);
    output->generated_frames=ready?f->output_capacity:0;
    float stages[5],total=0.0f;
    if(snap_frame_vk_get_telemetry(h->vk,stages,&total)){
        output->preprocess_gpu_ms=stages[0]+stages[1];output->flow_gpu_ms=stages[2];
        output->consistency_gpu_ms=stages[3];output->warp_gpu_ms=stages[4];
    }
    return SNAP_FRAME_OK;
}

static void apply_config(void) {
    if(g_snap_core){SnapFrameEngineConfig cfg;snap_frame_default_config(&cfg);cfg.enabled=1;cfg.multiplier=g_multiplier;cfg.flow_scale_percent=(uint32_t)(g_flow_scale*100.0f);cfg.quality=SNAP_FRAME_QUALITY_BALANCED;(void)snap_frame_set_config(g_snap_core,&cfg);}
    if (g_lsfg) {
        vkr_lsfg_configure(g_lsfg, g_multiplier, g_target_fps, g_flow_scale, g_refresh_hz, 0.0f);
    } else if (g_dis) {
        vkr_dis_configure(g_dis, g_dis_min_side, g_target_fps, g_refresh_hz);
    }
}

int fge_start(int kind, const char *cache_path) {
    fge_stop();
    if (!g_dev || !g_pd) return -1;

    if (kind == VKP_FG_ENGINE_DIS) {
        g_dis = vkr_dis_create(g_dev, g_pd);
        if (!g_dis) return -1;
    } else if (kind == VKP_FG_ENGINE_SNAPFRAME) {
        SnapFrameVk *created = snap_frame_vk_create(g_dev, g_pd);
        if (!created) return -1;
        pthread_mutex_lock(&g_snap_lock); g_snap = created; pthread_mutex_unlock(&g_snap_lock);
        g_snap_host.vk=created;g_snap_host.multiplier=g_multiplier;g_snap_frame_id=0;
        SnapFrameEngineConfig cfg;snap_frame_default_config(&cfg);cfg.enabled=1;cfg.multiplier=g_multiplier;
        cfg.flow_scale_percent=(uint32_t)(g_flow_scale*100.0f);cfg.quality=SNAP_FRAME_QUALITY_BALANCED;
        SnapFrameBackendV1 backend={0};backend.struct_size=sizeof(backend);backend.api_version=SNAP_FRAME_API_VERSION;
        backend.user_data=&g_snap_host;backend.initialize=snap_backend_initialize;backend.shutdown=snap_backend_shutdown;
        backend.update_config=snap_backend_update;backend.reset_history=snap_backend_reset;backend.process_frame=snap_backend_process;
        g_snap_core=snap_frame_create(&cfg,&backend);
        if(!g_snap_core){pthread_mutex_lock(&g_snap_lock);g_snap=NULL;pthread_mutex_unlock(&g_snap_lock);snap_frame_vk_destroy(created);g_snap_host.vk=NULL;return -1;}
    } else {
        if (!cache_path || !*cache_path) return -1;
        g_lsfg = vkr_lsfg_create(g_dev, g_pd, cache_path);
        if (!g_lsfg) return -1;
    }
    g_kind = kind;
    g_chain_ok = 0;
    g_chain_failed = 0;
    g_last_plan_ns = 0;
    apply_config();
    return 0;
}

void fge_stop(void) {
    if(g_snap_core){snap_frame_destroy(g_snap_core);g_snap_core=NULL;}
    g_snap_host.vk=NULL;
    if (g_lsfg) { vkr_lsfg_destroy(g_lsfg); g_lsfg = NULL; }
    if (g_dis) { vkr_dis_destroy(g_dis); g_dis = NULL; }
    pthread_mutex_lock(&g_snap_lock);
    if (g_snap) { snap_frame_vk_destroy(g_snap); g_snap = NULL; }
    pthread_mutex_unlock(&g_snap_lock);
    g_kind = -1;
    g_chain_ok = 0;
    g_chain_failed = 0;
    g_source = VK_NULL_HANDLE;
}

int fge_unavailable(void) { return (!g_lsfg && !g_dis && !g_snap) || g_chain_failed; }

void fge_configure(uint32_t multiplier, float flow_scale, float refresh_hz, int flow_min_side,
                   int target_fps) {
    g_multiplier = multiplier ? multiplier : 2u;
    g_flow_scale = flow_scale > 0.0f ? flow_scale : 0.7f;
    g_refresh_hz = refresh_hz;
    if (flow_min_side > 0) g_dis_min_side = (uint32_t)flow_min_side;
    g_target_fps = target_fps > 0 ? (uint32_t)target_fps : 0u;
    apply_config();
}

int fge_prepare(uint32_t w, uint32_t h, VkFormat fmt) {
    if (!w || !h) return 0;

    if (g_lsfg) {
        if (g_chain_ok && !vkr_lsfg_needs_rebuild(g_lsfg, w, h, fmt)) return 1;
        vkr_lsfg_forget_targets(g_lsfg);
        vkr_lsfg_set_guest_extent(g_lsfg, w, h);
        g_chain_ok = vkr_lsfg_prepare(g_lsfg, w, h, fmt) ? 1 : 0;
    } else if (g_snap) {
        g_chain_ok = snap_frame_vk_prepare(g_snap, w, h, fmt, (uint32_t)(g_flow_scale * 100.0f)) ? 1 : 0;
    } else if (g_dis) {
        const VkrDisContentRect content = {0, 0, w, h};
        if (g_chain_ok && !vkr_dis_needs_rebuild(g_dis, w, h, fmt, content)) return 1;
        vkr_dis_forget_targets(g_dis);
        g_chain_ok = vkr_dis_prepare(g_dis, w, h, fmt, content) ? 1 : 0;
    } else {
        return 0;
    }

    g_chain_failed = !g_chain_ok;
    return g_chain_ok;
}

uint32_t fge_plan(uint32_t capacity, uint64_t source_frames, float presented_rate) {
    (void)presented_rate;
    if (!g_chain_ok) return 0;
    if (g_lsfg) {
        vkr_lsfg_set_refresh_rate(g_lsfg, g_refresh_hz);
        return vkr_lsfg_plan(g_lsfg, capacity, source_frames);
    }
    if (g_snap) { uint32_t n=g_multiplier>1?g_multiplier-1:0; return snap_frame_vk_has_history(g_snap) ? (n<capacity?n:capacity) : 0; }
    if (g_dis) return vkr_dis_plan(g_dis, capacity, source_frames);
    return 0;
}

uint32_t fge_process(VkCommandBuffer cmd, VkImage source, VkImageView source_view, uint32_t w, uint32_t h, VkFormat fmt, uint32_t gens, const SnapFrameVulkanOutputSlot *outputs) {
    if (!g_chain_ok) return 0;
    g_source = source;
    if (g_snap && g_snap_core) {
        uint64_t now=now_ns();uint32_t scene_cut=(g_last_plan_ns&&now-g_last_plan_ns>500000000ull)?1u:0u;
        g_last_plan_ns=now;
        SnapFrameVulkanFrame native={0};native.struct_size=sizeof(native);native.interop_version=SNAP_FRAME_VULKAN_INTEROP_VERSION;
        native.physical_device=g_pd;native.device=g_dev;native.queue=g_queue;native.queue_family_index=g_qfam;
        native.command_buffer=cmd;native.source_image=source;native.source_view=source_view;
        native.source_layout=VK_IMAGE_LAYOUT_GENERAL;native.source_format=fmt;native.extent=(VkExtent2D){w,h};
        native.output_capacity=gens>SNAP_FRAME_MAX_MULTIPLIER-1?SNAP_FRAME_MAX_MULTIPLIER-1:gens;
        for(uint32_t i=0;i<native.output_capacity;i++)native.outputs[i]=outputs[i];
        native.frame_id=++g_snap_frame_id;native.timestamp_ns=now;native.scene_cut=scene_cut;
        SnapFrameFrameInput input={0};input.struct_size=sizeof(input);input.frame_id=native.frame_id;
        input.timestamp_ns=now;input.width=w;input.height=h;input.format=(uint32_t)fmt;input.scene_cut=scene_cut;
        input.native_frame=(uintptr_t)&native;
        SnapFrameFrameOutput output={0};output.struct_size=sizeof(output);
        SnapFrameStatus status=snap_frame_process(g_snap_core,&input,&output);
        return status==SNAP_FRAME_OK?output.generated_frames:0u;
    }
    if (g_lsfg) {
        vkr_lsfg_process(g_lsfg, cmd, source, w, h, gens);
        /* The pacer wants the source cadence; the compositor's own frame clock is the measure. */
        uint64_t now = now_ns();
        if (g_last_plan_ns) vkr_lsfg_note_frame(g_lsfg, now - g_last_plan_ns, gens);
        g_last_plan_ns = now;
    } else if (g_dis) {
        vkr_dis_process(g_dis, cmd, source, w, h, gens);
    }
    return gens;
}

void fge_generate(VkCommandBuffer cmd, uint32_t g, uint32_t count, VkImage img, VkImageView view,
                  uint32_t w, uint32_t h) {
    (void)count;
    if (!g_chain_ok) return;
    /* The ring slot is stable for the life of the chain, so it doubles as the descriptor key. */
    if (g_snap) {
        snap_frame_vk_generate(g_snap,cmd,g,count,img,view);
    } else if (g_lsfg) {
        vkr_lsfg_generate_into(g_lsfg, cmd, g, g, img, view, w, h);
    } else if (g_dis) {
        vkr_dis_generate_into(g_dis, cmd, g, g, img, view, w, h, g_source);
    }
}

void fge_forget_targets(void) {
    if (g_lsfg) vkr_lsfg_forget_targets(g_lsfg);
    if (g_dis) vkr_dis_forget_targets(g_dis);
    if (g_snap) snap_frame_vk_reset_history(g_snap);
}

void fge_telemetry(float *accepted, float *source_rate, float *thermal) {
    *accepted = 0.0f; *source_rate = 0.0f; *thermal = -1.0f;
    pthread_mutex_lock(&g_snap_lock);
    SnapFrameVk *snap = g_snap;
    if (snap) { float stages[5], total=0.0f; if (snap_frame_vk_get_telemetry(snap,stages,&total)) { *accepted=total; *source_rate=stages[2]; } }
    pthread_mutex_unlock(&g_snap_lock);
}

int fge_flow_metrics(float metrics[4]) {
    if (!metrics) return 0;
    pthread_mutex_lock(&g_snap_lock);
    int valid = g_snap ? snap_frame_vk_get_flow_metrics(g_snap, metrics) : 0;
    pthread_mutex_unlock(&g_snap_lock);
    return valid;
}

const char *fge_engine_name(int kind) {
    if (kind == VKP_FG_ENGINE_SNAPFRAME) return "SnapFrame Vulkan";
    return kind == VKP_FG_ENGINE_DIS ? "WFG Native" : "LSFG Native";
}

void fge_reset_history(void) { if (g_snap) snap_frame_vk_reset_history(g_snap); }

void fge_set_candidate_reconstruction(int enabled) {
    if (g_snap) snap_frame_vk_set_candidate_reconstruction(g_snap, enabled ? 1u : 0u);
}

void fge_device_lost(void) {
    /* A lost VkDevice must not be touched for cleanup. The driver reclaims its objects. */
    if(g_snap_core){snap_frame_destroy(g_snap_core);g_snap_core=NULL;}
    g_snap_host.vk=NULL;
    g_lsfg = NULL; g_dis = NULL;
    pthread_mutex_lock(&g_snap_lock);
    if (g_snap) snap_frame_vk_abandon(g_snap);
    g_snap = NULL;
    pthread_mutex_unlock(&g_snap_lock);
    g_dev = VK_NULL_HANDLE;g_queue=VK_NULL_HANDLE;g_qfam=0;
    g_kind = -1; g_chain_ok = 0; g_chain_failed = 0; g_source = VK_NULL_HANDLE;
}

const char *fge_build_info(void) { return "SnapFrame Vulkan V1 + lsfg-native/dis-native"; }
