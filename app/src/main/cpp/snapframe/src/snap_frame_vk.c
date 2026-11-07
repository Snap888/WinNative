#include "snap_frame_vk.h"
#include "snap_frame_vk_contract.h"
#include "snap_ai/snap_ai.h"
#include "vk_loader.h"
#include "snap_luma_downsample_comp.spv.h"
#include "snap_pyramid_downsample_comp.spv.h"
#include "snap_block_match_flow_comp.spv.h"
#include "snap_flow_consistency_comp.spv.h"
#include "snap_midpoint_warp_rgba8_comp.spv.h"
#include "snap_midpoint_warp_comp.spv.h"
#include "snap_flow_debug_comp.spv.h"
#include "snap_flow_metrics_comp.spv.h"
#include "snap_candidate_warp_comp.spv.h"
#include "snap_candidate_score_comp.spv.h"
#include "snap_candidate_reconstruct_rgba8_comp.spv.h"
#include "snap_candidate_reconstruct_comp.spv.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <math.h>
#include <android/log.h>

#define SF_LOG_TAG "SnapFrame"
#define SF_LOGI(...) __android_log_print(ANDROID_LOG_INFO, SF_LOG_TAG, __VA_ARGS__)

#define SF_LEVELS 3u
#define SF_PHASES 3u
#define SF_BINDINGS 8u

struct sf_image { VkImage image; VkDeviceMemory memory; VkImageView view; uint32_t w,h; int fresh; };
struct sf_pipeline { VkDescriptorSetLayout set_layout; VkPipelineLayout layout; VkPipeline pipeline; VkDescriptorSet sets[4]; };
struct sf_buffer { VkBuffer buffer; VkDeviceMemory memory; };
struct SnapFrameVk {
    VkDevice device; VkPhysicalDevice physical; VkPhysicalDeviceMemoryProperties memprops;
    VkSampler sampler; VkDescriptorPool pool;
    struct sf_pipeline luma,pyramid,flow,consistency,warp8,warp16,debug,metrics;
    struct sf_pipeline candidate_warp,candidate_score,candidate_reconstruct8,candidate_reconstruct16;
    struct sf_buffer metrics_readback[4]; struct sf_image metrics_output;
    int metrics_pipeline_ready,metrics_enabled; uint8_t metrics_pending[4];
    struct sf_image history,prev[SF_LEVELS],curr[SF_LEVELS],forward,backward,mask;
    struct sf_image candidates[SF_PHASES][4],candidate_weights[SF_PHASES];
    uint32_t candidate_reconstruction_mode; int candidate_reconstruction_enabled,ai_quality_mode;
    uint32_t width,height,flow_percent,manual_flow_percent,active_flow_w,active_flow_h,debug_mode; float search_radius_scale; VkFormat format;
    VkImage pending_source; VkImageView pending_source_view;
    VkQueryPool query_pool; float timestamp_period_ms;
    uint64_t frame_serial,query_frame_id[4]; uint8_t query_pending[4]; uint32_t query_slot;
    float stage_gpu_ms[5],last_gpu_ms,flow_metrics[4]; int telemetry_valid,flow_metrics_valid;
    pthread_mutex_t telemetry_mutex; int telemetry_mutex_ready;
    SnapAIHandle *ai;
    SnapFrameVkLifecycle lifecycle;
    int prepared,history_valid;
};

static uint32_t div_up(uint32_t x,uint32_t y){return (x+y-1u)/y;}
static void image_destroy(SnapFrameVk*e,struct sf_image*i){
    if(i->view)g_vk.DestroyImageView(e->device,i->view,NULL);
    if(i->image)g_vk.DestroyImage(e->device,i->image,NULL);
    if(i->memory)g_vk.FreeMemory(e->device,i->memory,NULL);
    memset(i,0,sizeof(*i));
}
static int image_create(SnapFrameVk*e,struct sf_image*i,uint32_t w,uint32_t h,VkFormat fmt){
    VkImageCreateInfo ci={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,
      .format=fmt,.extent={w,h,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,
      .tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
    if(g_vk.CreateImage(e->device,&ci,NULL,&i->image)!=VK_SUCCESS)return 0;
    VkMemoryRequirements req;g_vk.GetImageMemoryRequirements(e->device,i->image,&req);
    int mt=-1;
    for(uint32_t n=0;n<e->memprops.memoryTypeCount;n++)
      if((req.memoryTypeBits&(1u<<n))&&(e->memprops.memoryTypes[n].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)){mt=(int)n;break;}
    if(mt<0)return 0;
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=(uint32_t)mt};
    if(g_vk.AllocateMemory(e->device,&ai,NULL,&i->memory)!=VK_SUCCESS)return 0;
    if(g_vk.BindImageMemory(e->device,i->image,i->memory,0)!=VK_SUCCESS)return 0;
    VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=i->image,
      .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=fmt,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    if(g_vk.CreateImageView(e->device,&vi,NULL,&i->view)!=VK_SUCCESS)return 0;
    i->w=w;i->h=h;i->fresh=1;return 1;
}
static void buffer_destroy(SnapFrameVk*e,struct sf_buffer*b){
    if(b->buffer)g_vk.DestroyBuffer(e->device,b->buffer,NULL);
    if(b->memory)g_vk.FreeMemory(e->device,b->memory,NULL);
    memset(b,0,sizeof(*b));
}
static int buffer_create_readback(SnapFrameVk*e,struct sf_buffer*b){
    VkBufferCreateInfo ci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=sizeof(float)*4u,
        .usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    if(g_vk.CreateBuffer(e->device,&ci,NULL,&b->buffer)!=VK_SUCCESS)return 0;
    VkMemoryRequirements req;g_vk.GetBufferMemoryRequirements(e->device,b->buffer,&req);
    int mt=-1;
    for(uint32_t i=0;i<e->memprops.memoryTypeCount;i++){
        VkMemoryPropertyFlags flags=e->memprops.memoryTypes[i].propertyFlags;
        if((req.memoryTypeBits&(1u<<i))&&
           (flags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){mt=(int)i;break;}
    }
    if(mt<0){buffer_destroy(e,b);return 0;}
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize=req.size,.memoryTypeIndex=(uint32_t)mt};
    if(g_vk.AllocateMemory(e->device,&ai,NULL,&b->memory)!=VK_SUCCESS){buffer_destroy(e,b);return 0;}
    if(g_vk.BindBufferMemory(e->device,b->buffer,b->memory,0)!=VK_SUCCESS){buffer_destroy(e,b);return 0;}
    return 1;
}
static void pipeline_destroy(SnapFrameVk*e,struct sf_pipeline*p){
    if(p->pipeline)g_vk.DestroyPipeline(e->device,p->pipeline,NULL);
    if(p->layout)g_vk.DestroyPipelineLayout(e->device,p->layout,NULL);
    if(p->set_layout)g_vk.DestroyDescriptorSetLayout(e->device,p->set_layout,NULL);
    memset(p,0,sizeof(*p));
}
static int pipeline_create(SnapFrameVk*e,struct sf_pipeline*p,const uint32_t*code,size_t bytes,
                           uint32_t count,uint32_t sampled,uint32_t push_bytes){
    VkDescriptorSetLayoutBinding b[SF_BINDINGS];memset(b,0,sizeof(b));
    for(uint32_t i=0;i<count;i++){b[i].binding=i;b[i].descriptorType=(sampled&(1u<<i))?
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;b[i].descriptorCount=1;b[i].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;}
    VkDescriptorSetLayoutCreateInfo si={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=count,.pBindings=b};
    if(g_vk.CreateDescriptorSetLayout(e->device,&si,NULL,&p->set_layout)!=VK_SUCCESS)return 0;
    VkPushConstantRange pc={.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT,.offset=0,.size=push_bytes};
    VkPipelineLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&p->set_layout,
      .pushConstantRangeCount=push_bytes?1u:0u,.pPushConstantRanges=push_bytes?&pc:NULL};
    if(g_vk.CreatePipelineLayout(e->device,&li,NULL,&p->layout)!=VK_SUCCESS)return 0;
    VkShaderModuleCreateInfo mi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=bytes,.pCode=code};
    VkShaderModule module=VK_NULL_HANDLE;if(g_vk.CreateShaderModule(e->device,&mi,NULL,&module)!=VK_SUCCESS)return 0;
    VkComputePipelineCreateInfo ci={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=module,.pName="main"},.layout=p->layout};
    VkResult r=g_vk.CreateComputePipelines(e->device,VK_NULL_HANDLE,1,&ci,NULL,&p->pipeline);
    g_vk.DestroyShaderModule(e->device,module,NULL);return r==VK_SUCCESS;
}
static int alloc_sets(SnapFrameVk*e,struct sf_pipeline*p,uint32_t n){
    VkDescriptorSetLayout ls[4];if(n>4)return 0;for(uint32_t i=0;i<n;i++)ls[i]=p->set_layout;
    VkDescriptorSetAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=e->pool,.descriptorSetCount=n,.pSetLayouts=ls};
    return g_vk.AllocateDescriptorSets(e->device,&ai,p->sets)==VK_SUCCESS;
}
static void update_set(SnapFrameVk*e,struct sf_pipeline*p,uint32_t set,uint32_t n,uint32_t sampled,VkImageView*views){
    VkDescriptorImageInfo infos[SF_BINDINGS];VkWriteDescriptorSet writes[SF_BINDINGS];memset(infos,0,sizeof(infos));memset(writes,0,sizeof(writes));
    for(uint32_t i=0;i<n;i++){infos[i].imageView=views[i];infos[i].imageLayout=VK_IMAGE_LAYOUT_GENERAL;if(sampled&(1u<<i))infos[i].sampler=e->sampler;
      writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[i].dstSet=p->sets[set];writes[i].dstBinding=i;writes[i].descriptorCount=1;
      writes[i].descriptorType=(sampled&(1u<<i))?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;writes[i].pImageInfo=&infos[i];}
    g_vk.UpdateDescriptorSets(e->device,n,writes,0,NULL);
}
static void dispatch(VkCommandBuffer cmd,struct sf_pipeline*p,uint32_t set,uint32_t w,uint32_t h){
    g_vk.CmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,p->pipeline);
    g_vk.CmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,p->layout,0,1,&p->sets[set],0,NULL);
    g_vk.CmdDispatch(cmd,div_up(w,8),div_up(h,8),1);
}
static void barrier_compute(VkCommandBuffer cmd){
    VkMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,
      .dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT};
    g_vk.CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&b,0,NULL,0,NULL);
}
static void barrier_compute_reuse(VkCommandBuffer cmd){
    /* Candidate maps are read by reconstruction and overwritten on the next source frame. */
    VkMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,
      .dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT};
    g_vk.CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&b,0,NULL,0,NULL);
}
static void transition_fresh(SnapFrameVk*e,VkCommandBuffer cmd){
    VkImageMemoryBarrier b[32];uint32_t n=0;
#define FRESH(i) do{if((i).fresh){b[n++]=(VkImageMemoryBarrier){.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=0,\
 .dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_GENERAL,\
 .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=(i).image,\
 .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};(i).fresh=0;}}while(0)
    FRESH(e->history);for(uint32_t i=0;i<SF_LEVELS;i++){FRESH(e->prev[i]);FRESH(e->curr[i]);}
    FRESH(e->forward);FRESH(e->backward);FRESH(e->mask);FRESH(e->metrics_output);
    for(uint32_t phase=0;phase<SF_PHASES;phase++){for(uint32_t c=0;c<4;c++)FRESH(e->candidates[phase][c]);FRESH(e->candidate_weights[phase]);}
#undef FRESH
    if(n)g_vk.CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,NULL,0,NULL,n,b);
}
static void copy_history(SnapFrameVk*e,VkCommandBuffer cmd,VkImage src){
    VkImageMemoryBarrier before[2]={
      {.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,
       .oldLayout=VK_IMAGE_LAYOUT_GENERAL,.newLayout=VK_IMAGE_LAYOUT_GENERAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=src,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}},
      {.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
       .oldLayout=VK_IMAGE_LAYOUT_GENERAL,.newLayout=VK_IMAGE_LAYOUT_GENERAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=e->history.image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}}};
    g_vk.CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,2,before);
    VkImageCopy cp={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.extent={e->width,e->height,1}};
    g_vk.CmdCopyImage(cmd,src,VK_IMAGE_LAYOUT_GENERAL,e->history.image,VK_IMAGE_LAYOUT_GENERAL,1,&cp);
    VkImageMemoryBarrier after={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT,
      .oldLayout=VK_IMAGE_LAYOUT_GENERAL,.newLayout=VK_IMAGE_LAYOUT_GENERAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
      .image=e->history.image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    g_vk.CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,NULL,0,NULL,1,&after);
    e->history_valid=1;e->lifecycle.history_valid=1;
}
static void resources_destroy(SnapFrameVk*e){
    image_destroy(e,&e->history);for(uint32_t i=0;i<SF_LEVELS;i++){image_destroy(e,&e->prev[i]);image_destroy(e,&e->curr[i]);}
    image_destroy(e,&e->forward);image_destroy(e,&e->backward);image_destroy(e,&e->mask);image_destroy(e,&e->metrics_output);e->metrics_enabled=0;
    for(uint32_t phase=0;phase<SF_PHASES;phase++){for(uint32_t c=0;c<4;c++)image_destroy(e,&e->candidates[phase][c]);image_destroy(e,&e->candidate_weights[phase]);}
    e->width=e->height=0;e->prepared=0;e->history_valid=0;e->pending_source=VK_NULL_HANDLE;e->pending_source_view=VK_NULL_HANDLE;
}
SnapFrameVk*snap_frame_vk_create(VkDevice d,VkPhysicalDevice pd){
    if(!d||!pd||!g_vk.CreateComputePipelines||!g_vk.CmdDispatch||!g_vk.CmdCopyImage||
       !g_vk.CreateQueryPool||!g_vk.GetQueryPoolResults||!g_vk.CmdResetQueryPool||!g_vk.CmdWriteTimestamp)return NULL;
    SnapFrameVk*e=(SnapFrameVk*)calloc(1,sizeof(*e));if(!e)return NULL;e->device=d;e->physical=pd;
    if(pthread_mutex_init(&e->telemetry_mutex,NULL)!=0)goto fail;e->telemetry_mutex_ready=1;
    g_vk.GetPhysicalDeviceMemoryProperties(pd,&e->memprops);
    VkPhysicalDeviceProperties props; memset(&props,0,sizeof(props));
    g_vk.GetPhysicalDeviceProperties(pd,&props);
    if(!props.limits.timestampComputeAndGraphics || props.limits.timestampPeriod<=0.0f)goto fail;
    e->timestamp_period_ms=props.limits.timestampPeriod/1000000.0f;
    SnapAIConfig ai_cfg; snap_ai_default_config(&ai_cfg);
    ai_cfg.min_flow_scale=0.25f; ai_cfg.max_flow_scale=1.0f;
    ai_cfg.target_gpu_budget_ms=7.0f; ai_cfg.hysteresis=0.08f; ai_cfg.warmup_frames=8; e->search_radius_scale=1.0f;
    e->ai=snap_ai_create(&ai_cfg); if(!e->ai)goto fail;
    VkQueryPoolCreateInfo qi={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,.queryType=VK_QUERY_TYPE_TIMESTAMP,.queryCount=40};
    if(g_vk.CreateQueryPool(d,&qi,NULL,&e->query_pool)!=VK_SUCCESS)goto fail;
    VkSamplerCreateInfo si={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=VK_FILTER_LINEAR,.minFilter=VK_FILTER_LINEAR,
      .mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.maxLod=0};
    if(g_vk.CreateSampler(d,&si,NULL,&e->sampler)!=VK_SUCCESS)goto fail;
    VkDescriptorPoolSize sizes[2]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,128},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,64}};
    VkDescriptorPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,.maxSets=32,.poolSizeCount=2,.pPoolSizes=sizes};
    if(g_vk.CreateDescriptorPool(d,&pi,NULL,&e->pool)!=VK_SUCCESS)goto fail;
    if(!pipeline_create(e,&e->luma,snap_luma_downsample_comp,snap_luma_downsample_comp_size,2,1,0)||
       !pipeline_create(e,&e->pyramid,snap_pyramid_downsample_comp,snap_pyramid_downsample_comp_size,2,1,0)||
       !pipeline_create(e,&e->flow,snap_block_match_flow_comp,snap_block_match_flow_comp_size,3,3,12)||
       !pipeline_create(e,&e->consistency,snap_flow_consistency_comp,snap_flow_consistency_comp_size,4,11,8)||
       !pipeline_create(e,&e->warp8,snap_midpoint_warp_rgba8_comp,snap_midpoint_warp_rgba8_comp_size,6,31,16)||
       !pipeline_create(e,&e->warp16,snap_midpoint_warp_comp,snap_midpoint_warp_comp_size,6,31,16)||
       !pipeline_create(e,&e->debug,snap_flow_debug_comp,snap_flow_debug_comp_size,3,3,24)||
       !pipeline_create(e,&e->candidate_warp,snap_candidate_warp_comp,snap_candidate_warp_comp_size,7,7,24)||
       !pipeline_create(e,&e->candidate_score,snap_candidate_score_comp,snap_candidate_score_comp_size,7,63,8)||
       !pipeline_create(e,&e->candidate_reconstruct8,snap_candidate_reconstruct_rgba8_comp,snap_candidate_reconstruct_rgba8_comp_size,8,127,16)||
       !pipeline_create(e,&e->candidate_reconstruct16,snap_candidate_reconstruct_comp,snap_candidate_reconstruct_comp_size,8,127,16))goto fail;
    if(!alloc_sets(e,&e->luma,2)||!alloc_sets(e,&e->pyramid,4)||!alloc_sets(e,&e->flow,2)||
       !alloc_sets(e,&e->consistency,1)||!alloc_sets(e,&e->warp8,3)||!alloc_sets(e,&e->warp16,3)||
       !alloc_sets(e,&e->debug,1)||!alloc_sets(e,&e->candidate_warp,SF_PHASES)||
       !alloc_sets(e,&e->candidate_score,SF_PHASES)||!alloc_sets(e,&e->candidate_reconstruct8,SF_PHASES)||
       !alloc_sets(e,&e->candidate_reconstruct16,SF_PHASES))goto fail;
    if(g_vk.CreateBuffer&&g_vk.GetBufferMemoryRequirements&&g_vk.BindBufferMemory&&g_vk.DestroyBuffer&&g_vk.CmdCopyImageToBuffer&&g_vk.MapMemory&&g_vk.UnmapMemory&&
       pipeline_create(e,&e->metrics,snap_flow_metrics_comp,snap_flow_metrics_comp_size,3,3,0)&&alloc_sets(e,&e->metrics,1)){
        e->metrics_pipeline_ready=1;
        for(uint32_t i=0;i<4;i++)if(!buffer_create_readback(e,&e->metrics_readback[i])){
            for(uint32_t j=0;j<4;j++)buffer_destroy(e,&e->metrics_readback[j]);
            pipeline_destroy(e,&e->metrics);e->metrics_pipeline_ready=0;break;
        }
    }
    return e;
fail:snap_frame_vk_destroy(e);return NULL;
}
void snap_frame_vk_destroy(SnapFrameVk*e){
    if(!e)return;resources_destroy(e);pipeline_destroy(e,&e->luma);pipeline_destroy(e,&e->pyramid);
    pipeline_destroy(e,&e->flow);pipeline_destroy(e,&e->consistency);pipeline_destroy(e,&e->warp8);pipeline_destroy(e,&e->warp16);pipeline_destroy(e,&e->debug);
    pipeline_destroy(e,&e->candidate_warp);pipeline_destroy(e,&e->candidate_score);pipeline_destroy(e,&e->candidate_reconstruct8);pipeline_destroy(e,&e->candidate_reconstruct16);
    if(e->query_pool)g_vk.DestroyQueryPool(e->device,e->query_pool,NULL);
    if(e->pool)g_vk.DestroyDescriptorPool(e->device,e->pool,NULL);
    if(e->sampler)g_vk.DestroySampler(e->device,e->sampler,NULL);
    for(uint32_t i=0;i<4;i++)buffer_destroy(e,&e->metrics_readback[i]);
    pipeline_destroy(e,&e->metrics);
    if(e->ai)snap_ai_destroy(e->ai);
    if(e->telemetry_mutex_ready)pthread_mutex_destroy(&e->telemetry_mutex);
    free(e);
}
int snap_frame_vk_prepare(SnapFrameVk*e,uint32_t w,uint32_t h,VkFormat fmt,uint32_t scale){
    if(!e||!w||!h||!snap_frame_vk_format_supported(fmt))return 0;
    if(scale<25)scale=25;if(scale>100)scale=100;
    if(e->prepared&&e->width==w&&e->height==h&&e->format==fmt){
        if(e->manual_flow_percent!=scale){e->manual_flow_percent=scale;e->flow_percent=scale;}
        snap_frame_vk_flow_extent(e->forward.w,e->forward.h,e->flow_percent,&e->active_flow_w,&e->active_flow_h);
        return 1;
    }
    /* render_impl has completed the previous frame's fence before this callback; keep resize non-blocking. */
    resources_destroy(e);
    uint32_t lw[3],lh[3];lw[0]=w>1?w/2:1;lh[0]=h>1?h/2:1;
    for(uint32_t i=1;i<3;i++){lw[i]=lw[i-1]>1?lw[i-1]/2:1;lh[i]=lh[i-1]>1?lh[i-1]/2:1;}
    uint32_t fw=lw[2],fh=lh[2];
    if(!image_create(e,&e->history,w,h,fmt))goto fail;
    for(uint32_t i=0;i<3;i++)if(!image_create(e,&e->prev[i],lw[i],lh[i],VK_FORMAT_R16_SFLOAT)||
        !image_create(e,&e->curr[i],lw[i],lh[i],VK_FORMAT_R16_SFLOAT))goto fail;
    if(!image_create(e,&e->forward,fw,fh,VK_FORMAT_R16G16B16A16_SFLOAT)||
       !image_create(e,&e->backward,fw,fh,VK_FORMAT_R16G16B16A16_SFLOAT)||
       !image_create(e,&e->mask,fw,fh,VK_FORMAT_R16_SFLOAT))goto fail;
    for(uint32_t phase=0;phase<SF_PHASES;phase++){
        for(uint32_t c=0;c<4;c++)if(!image_create(e,&e->candidates[phase][c],fw,fh,VK_FORMAT_R16G16B16A16_SFLOAT))goto fail;
        if(!image_create(e,&e->candidate_weights[phase],fw,fh,VK_FORMAT_R16G16B16A16_SFLOAT))goto fail;
    }
    if(e->metrics_pipeline_ready&&image_create(e,&e->metrics_output,1,1,VK_FORMAT_R32G32B32A32_SFLOAT))e->metrics_enabled=1;
    e->width=w;e->height=h;e->format=fmt;e->flow_percent=scale;e->manual_flow_percent=scale;
    snap_frame_vk_flow_extent(fw,fh,scale,&e->active_flow_w,&e->active_flow_h);
    snap_frame_vk_lifecycle_update(&e->lifecycle,1,w,h,fmt,1,0);
    e->prepared=1;e->history_valid=0;e->lifecycle.history_valid=0;e->frame_serial=0;memset(e->query_pending,0,sizeof(e->query_pending));memset(e->metrics_pending,0,sizeof(e->metrics_pending));
    pthread_mutex_lock(&e->telemetry_mutex);e->telemetry_valid=0;e->flow_metrics_valid=0;e->last_gpu_ms=0.0f;memset(e->stage_gpu_ms,0,sizeof(e->stage_gpu_ms));memset(e->flow_metrics,0,sizeof(e->flow_metrics));pthread_mutex_unlock(&e->telemetry_mutex);
    snap_ai_set_enabled(e->ai,0);snap_ai_set_enabled(e->ai,1);
    SF_LOGI("prepared extent=%ux%u format=%u flow_scale=%u flow_extent=%ux%u candidate_mode=%u metrics=%d",
        w,h,(unsigned)fmt,e->flow_percent,e->active_flow_w,e->active_flow_h,
        e->candidate_reconstruction_mode,e->metrics_enabled);
    return 1;
fail:resources_destroy(e);SF_LOGI("prepare failed extent=%ux%u format=%u",w,h,(unsigned)fmt);return 0;
}
int snap_frame_vk_has_history(const SnapFrameVk*e){return e&&e->history_valid;}
void snap_frame_vk_set_candidate_reconstruction(SnapFrameVk*e,uint32_t mode){
    if(!e)return;
    /* 0=AI automatic, 1=forced off, 2=forced on. */
    mode=snap_frame_vk_candidate_mode(mode);
    if (e->candidate_reconstruction_mode != mode)
        SF_LOGI("candidate reconstruction mode=%u", mode);
    e->candidate_reconstruction_mode=mode;
    e->candidate_reconstruction_enabled=(mode==SNAP_FRAME_CANDIDATE_FORCE_ON)||
        (mode==SNAP_FRAME_CANDIDATE_AI_AUTO&&e->ai_quality_mode);
}
void snap_frame_vk_reset_history(SnapFrameVk*e){if(e){e->history_valid=0;e->lifecycle.history_valid=0;snap_ai_set_enabled(e->ai,0);snap_ai_set_enabled(e->ai,1);}}
static void collect_query_slot(SnapFrameVk*e,uint32_t slot){
    if(!e->query_pending[slot])return;
    uint64_t values[20];memset(values,0,sizeof(values));
    VkResult r=g_vk.GetQueryPoolResults(e->device,e->query_pool,slot*10u,10u,sizeof(values),values,
        sizeof(uint64_t)*2u,VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);    e->query_pending[slot]=0;
    /* Retire this slot's metrics on every path, including unavailable timestamps. */
    uint8_t metrics_ready=e->metrics_pending[slot];
    e->metrics_pending[slot]=0;
    if(r!=VK_SUCCESS)return;
    float total=0.0f,stage_values[5]={0};
    for(uint32_t i=0;i<5;i++){
        uint64_t begin=values[(i*2u)*2u],begin_ok=values[(i*2u)*2u+1u];
        uint64_t end=values[(i*2u+1u)*2u],end_ok=values[(i*2u+1u)*2u+1u];
        if(!begin_ok||!end_ok||end<begin)return;
        stage_values[i]=(float)(end-begin)*e->timestamp_period_ms;
        total+=stage_values[i];
    }
    int telemetry_valid=total>0.0f;
    pthread_mutex_lock(&e->telemetry_mutex);memcpy(e->stage_gpu_ms,stage_values,sizeof(stage_values));
    e->last_gpu_ms=total;e->telemetry_valid=telemetry_valid;pthread_mutex_unlock(&e->telemetry_mutex);
    /* Sparse diagnostic telemetry: one line per 60 completed source frames. No GPU wait/readback
       is added here; query results are already retired asynchronously by the existing ring. */
    if (telemetry_valid && (e->query_frame_id[slot] % 60u) == 0u) {
        SF_LOGI("frame=%llu gpu_ms=%.3f preprocess_ms=%.3f flow_ms=%.3f consistency_ms=%.3f warp_ms=%.3f flow_scale=%u active_flow=%ux%u candidate_mode=%u candidate_enabled=%d ai_quality=%d",
            (unsigned long long)e->query_frame_id[slot], total, stage_values[0], stage_values[2],
            stage_values[3], stage_values[4], e->flow_percent, e->active_flow_w, e->active_flow_h,
            e->candidate_reconstruction_mode, e->candidate_reconstruction_enabled, e->ai_quality_mode);
    }
    if(telemetry_valid&&e->ai&&e->metrics_enabled&&metrics_ready){
        float flow_metrics[4]={0};void*mapped=NULL;
        if(g_vk.MapMemory(e->device,e->metrics_readback[slot].memory,0,sizeof(flow_metrics),0,&mapped)==VK_SUCCESS&&mapped){
            memcpy(flow_metrics,mapped,sizeof(flow_metrics));g_vk.UnmapMemory(e->device,e->metrics_readback[slot].memory);
            int valid=1;for(uint32_t i=0;i<4;i++)if(!isfinite(flow_metrics[i])||flow_metrics[i]<0.0f||flow_metrics[i]>1.0f)valid=0;
            if(valid){
                pthread_mutex_lock(&e->telemetry_mutex);memcpy(e->flow_metrics,flow_metrics,sizeof(flow_metrics));e->flow_metrics_valid=1;pthread_mutex_unlock(&e->telemetry_mutex);
                SnapAIFrameMetrics metrics={0};metrics.struct_size=sizeof(metrics);
                metrics.frame_id=e->query_frame_id[slot];metrics.gpu_time_ms=total;
                metrics.motion_magnitude=flow_metrics[0];metrics.motion_complexity=flow_metrics[1];
                metrics.flow_confidence=flow_metrics[2];metrics.gpu_pressure=total/7.0f;
                metrics.artifact_risk=flow_metrics[3];metrics.telemetry_valid=1;
                SnapAIDecision decision={0};decision.struct_size=sizeof(decision);
                if(snap_ai_update(e->ai,&metrics,&decision)==SNAP_AI_OK){
                    if(e->candidate_reconstruction_mode==SNAP_FRAME_CANDIDATE_AI_AUTO){
                        /* Hysteresis prevents candidate compute from oscillating near the GPU budget. */
                        const int candidate_scene = metrics.motion_magnitude >= 0.12f &&
                            metrics.motion_complexity >= 0.18f &&
                            metrics.flow_confidence >= 0.18f &&
                            metrics.artifact_risk <= 0.10f;
                        if(decision.mode!=SNAP_AI_MODE_QUALITY || !candidate_scene ||
                           metrics.gpu_pressure>1.15f)
                            e->ai_quality_mode=0;
                        else if(metrics.gpu_pressure<0.98f)
                            e->ai_quality_mode=1;
                        e->candidate_reconstruction_enabled=e->ai_quality_mode;
                    }
                    if(decision.mode!=SNAP_AI_MODE_BYPASS){
                        e->search_radius_scale=fmaxf(0.75f,fminf(1.5f,decision.search_radius_scale));
                        uint32_t scale=(uint32_t)((float)e->manual_flow_percent*decision.flow_scale+0.5f);
                        if(scale<25)scale=25;if(scale>100)scale=100;e->flow_percent=scale;
                    } else {
                        /* BYPASS means no AI policy is active: restore the user's baseline,
                           otherwise an earlier QUALITY decision leaves flow permanently reduced. */
                        e->flow_percent=e->manual_flow_percent;
                        e->search_radius_scale=1.0f;
                        e->ai_quality_mode=0;
                        if(e->candidate_reconstruction_mode==SNAP_FRAME_CANDIDATE_AI_AUTO)
                            e->candidate_reconstruction_enabled=0;
                    }
                    if ((e->query_frame_id[slot] % 60u) == 0u) {
                        SF_LOGI("ai frame=%llu mode=%u flow_scale=%.3f gpu_pressure=%.3f motion=%.3f complexity=%.3f confidence=%.3f artifact_risk=%.3f",
                            (unsigned long long)e->query_frame_id[slot], (unsigned)decision.mode,
                            decision.flow_scale, metrics.gpu_pressure, metrics.motion_magnitude,
                            metrics.motion_complexity, metrics.flow_confidence, metrics.artifact_risk);
                    }
                }
            }
        }
    }
}
static void write_timestamp(SnapFrameVk*e,VkCommandBuffer cmd,uint32_t query){
    g_vk.CmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,e->query_pool,e->query_slot*10u+query);
}

int snap_frame_vk_process(SnapFrameVk*e,VkCommandBuffer cmd,VkImage src,VkImageView src_view,uint32_t n){
    if(!e||!e->prepared||!cmd||!src||!src_view)return 0;e->pending_source=src;e->pending_source_view=src_view;transition_fresh(e,cmd);
    if(!e->history_valid){copy_history(e,cmd,src);return 0;}
    const int debug_only = n == UINT32_MAX;
    if(n==0){copy_history(e,cmd,src);return 0;}
    pthread_mutex_lock(&e->telemetry_mutex);e->telemetry_valid=0;pthread_mutex_unlock(&e->telemetry_mutex);
    e->query_slot=(uint32_t)(e->frame_serial%4u);collect_query_slot(e,e->query_slot);
    g_vk.CmdResetQueryPool(cmd,e->query_pool,e->query_slot*10u,10u);
    e->query_frame_id[e->query_slot]=++e->frame_serial;e->query_pending[e->query_slot]=1;
    snap_frame_vk_flow_extent(e->forward.w,e->forward.h,e->flow_percent,&e->active_flow_w,&e->active_flow_h);
    VkImageView v[6];
    write_timestamp(e,cmd,0);
    v[0]=e->history.view;v[1]=e->prev[0].view;update_set(e,&e->luma,0,2,1,v);
    v[0]=src_view;v[1]=e->curr[0].view;update_set(e,&e->luma,1,2,1,v);
    dispatch(cmd,&e->luma,0,e->prev[0].w,e->prev[0].h);dispatch(cmd,&e->luma,1,e->curr[0].w,e->curr[0].h);barrier_compute(cmd);write_timestamp(e,cmd,1);
    write_timestamp(e,cmd,2);
    for(uint32_t i=1;i<3;i++){
      v[0]=e->prev[i-1].view;v[1]=e->prev[i].view;update_set(e,&e->pyramid,i-1,2,1,v);
      v[0]=e->curr[i-1].view;v[1]=e->curr[i].view;update_set(e,&e->pyramid,i+1,2,1,v);
      dispatch(cmd,&e->pyramid,i-1,e->prev[i].w,e->prev[i].h);dispatch(cmd,&e->pyramid,i+1,e->curr[i].w,e->curr[i].h);barrier_compute(cmd);
    }
    write_timestamp(e,cmd,3);write_timestamp(e,cmd,4);
    v[0]=e->prev[2].view;v[1]=e->curr[2].view;v[2]=e->forward.view;update_set(e,&e->flow,0,3,3,v);
    v[0]=e->curr[2].view;v[1]=e->prev[2].view;v[2]=e->backward.view;update_set(e,&e->flow,1,3,3,v);
    struct {uint32_t width,height;float search_radius_scale;} flow_pc={
        e->active_flow_w,e->active_flow_h,e->search_radius_scale};
    g_vk.CmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,e->flow.pipeline);
    g_vk.CmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,e->flow.layout,0,1,&e->flow.sets[0],0,NULL);
    g_vk.CmdPushConstants(cmd,e->flow.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(flow_pc),&flow_pc);
    g_vk.CmdDispatch(cmd,div_up(e->active_flow_w,8),div_up(e->active_flow_h,8),1);
    g_vk.CmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,e->flow.layout,0,1,&e->flow.sets[1],0,NULL);
    g_vk.CmdPushConstants(cmd,e->flow.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(flow_pc),&flow_pc);
    g_vk.CmdDispatch(cmd,div_up(e->active_flow_w,8),div_up(e->active_flow_h,8),1);barrier_compute(cmd);
    write_timestamp(e,cmd,5);
    v[0]=e->forward.view;v[1]=e->backward.view;v[2]=e->mask.view;v[3]=e->curr[2].view;
    update_set(e,&e->consistency,0,4,11,v);
    g_vk.CmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,e->consistency.pipeline);
    g_vk.CmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,e->consistency.layout,0,1,&e->consistency.sets[0],0,NULL);
    g_vk.CmdPushConstants(cmd,e->consistency.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(flow_pc),&flow_pc);
    write_timestamp(e,cmd,6);
    g_vk.CmdDispatch(cmd,div_up(e->active_flow_w,8),div_up(e->active_flow_h,8),1);barrier_compute(cmd);
    if(e->metrics_enabled){
        VkImageView metric_views[3]={e->forward.view,e->mask.view,e->metrics_output.view};
        update_set(e,&e->metrics,0,3,3,metric_views);
        g_vk.CmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,e->metrics.pipeline);
        g_vk.CmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,e->metrics.layout,0,1,&e->metrics.sets[0],0,NULL);
        g_vk.CmdDispatch(cmd,1,1,1);
        VkImageMemoryBarrier image_to_copy={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout=VK_IMAGE_LAYOUT_GENERAL,.newLayout=VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .image=e->metrics_output.image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        VkBufferMemoryBarrier host_to_copy={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask=VK_ACCESS_HOST_READ_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .buffer=e->metrics_readback[e->query_slot].buffer,.offset=0,.size=VK_WHOLE_SIZE};
        g_vk.CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_HOST_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,1,&host_to_copy,1,&image_to_copy);
        VkBufferImageCopy copy={.bufferOffset=0,.bufferRowLength=0,.bufferImageHeight=0,
            .imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageOffset={0,0,0},.imageExtent={1,1,1}};
        g_vk.CmdCopyImageToBuffer(cmd,e->metrics_output.image,VK_IMAGE_LAYOUT_GENERAL,
            e->metrics_readback[e->query_slot].buffer,1,&copy);
        VkImageMemoryBarrier image_back={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT,.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT,
            .oldLayout=VK_IMAGE_LAYOUT_GENERAL,.newLayout=VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .image=e->metrics_output.image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        VkBufferMemoryBarrier buffer_to_host={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .buffer=e->metrics_readback[e->query_slot].buffer,.offset=0,.size=VK_WHOLE_SIZE};
        g_vk.CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_HOST_BIT,0,0,NULL,1,&buffer_to_host,1,&image_back);
        e->metrics_pending[e->query_slot]=1;
    }
    write_timestamp(e,cmd,7);
    /* Debug-only processing still advances history after calculating the latest flow field. */
    if(debug_only)copy_history(e,cmd,src);
    return 1;
}
void snap_frame_vk_generate(SnapFrameVk*e,VkCommandBuffer cmd,uint32_t phase,uint32_t count,VkImage out,VkImageView out_view){
    if(!e||!e->prepared||!e->history_valid||!e->pending_source||!e->pending_source_view||!out||!out_view||!count||count>SF_PHASES||phase>=count)return;
    if(phase==0)write_timestamp(e,cmd,8);
    if(e->candidate_reconstruction_enabled){
        struct sf_pipeline *reconstruct=e->format==VK_FORMAT_R8G8B8A8_UNORM?
            &e->candidate_reconstruct8:&e->candidate_reconstruct16;
        VkImageView warpViews[7]={e->forward.view,e->backward.view,e->mask.view,
            e->candidates[phase][0].view,e->candidates[phase][1].view,
            e->candidates[phase][2].view,e->candidates[phase][3].view};
        update_set(e,&e->candidate_warp,phase,7,7,warpViews);
        barrier_compute_reuse(cmd);
        struct {float phase,flow_to_pixels;uint32_t flow_w,flow_h,full_w,full_h;} warpPc={
            snap_frame_vk_phase_time(phase,count),8.0f,e->active_flow_w,e->active_flow_h,e->width,e->height};
        g_vk.CmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,e->candidate_warp.pipeline);
        g_vk.CmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,e->candidate_warp.layout,0,1,
                                   &e->candidate_warp.sets[phase],0,NULL);
        g_vk.CmdPushConstants(cmd,e->candidate_warp.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(warpPc),&warpPc);
        g_vk.CmdDispatch(cmd,div_up(e->active_flow_w,8),div_up(e->active_flow_h,8),1);
        barrier_compute(cmd);

        VkImageView scoreViews[7]={e->history.view,e->pending_source_view,
            e->candidates[phase][0].view,e->candidates[phase][1].view,
            e->candidates[phase][2].view,e->candidates[phase][3].view,
            e->candidate_weights[phase].view};
        update_set(e,&e->candidate_score,phase,7,63,scoreViews);
        struct {uint32_t flow_w,flow_h;} scorePc={e->active_flow_w,e->active_flow_h};
        g_vk.CmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,e->candidate_score.pipeline);
        g_vk.CmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,e->candidate_score.layout,0,1,
                                   &e->candidate_score.sets[phase],0,NULL);
        g_vk.CmdPushConstants(cmd,e->candidate_score.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,
                              sizeof(scorePc),&scorePc);
        g_vk.CmdDispatch(cmd,div_up(e->active_flow_w,8),div_up(e->active_flow_h,8),1);
        barrier_compute(cmd);

        VkImageView reconstructViews[8]={e->history.view,e->pending_source_view,
            e->candidates[phase][0].view,e->candidates[phase][1].view,
            e->candidates[phase][2].view,e->candidates[phase][3].view,
            e->candidate_weights[phase].view,out_view};
        update_set(e,reconstruct,phase,8,127,reconstructViews);
        struct {uint32_t flow_w,flow_h,full_w,full_h;} reconstructPc={
            e->active_flow_w,e->active_flow_h,e->width,e->height};
        g_vk.CmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,reconstruct->pipeline);
        g_vk.CmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,reconstruct->layout,0,1,
                                   &reconstruct->sets[phase],0,NULL);
        g_vk.CmdPushConstants(cmd,reconstruct->layout,VK_SHADER_STAGE_COMPUTE_BIT,0,
                              sizeof(reconstructPc),&reconstructPc);
        g_vk.CmdDispatch(cmd,div_up(e->width,8),div_up(e->height,8),1);
    }else{
        struct sf_pipeline*p=e->format==VK_FORMAT_R8G8B8A8_UNORM?&e->warp8:&e->warp16;
        VkImageView v[6]={e->history.view,e->pending_source_view,e->forward.view,e->backward.view,e->mask.view,out_view};
        update_set(e,p,phase,6,31,v);
        struct {float phase,flow_to_pixels;uint32_t flow_w,flow_h;} pc={snap_frame_vk_phase_time(phase,count),8.0f,e->active_flow_w,e->active_flow_h};
        g_vk.CmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,p->pipeline);
        g_vk.CmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,p->layout,0,1,&p->sets[phase],0,NULL);
        g_vk.CmdPushConstants(cmd,p->layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(pc),&pc);
        g_vk.CmdDispatch(cmd,div_up(e->width,8),div_up(e->height,8),1);
    }
    if(phase+1u==count){write_timestamp(e,cmd,9);copy_history(e,cmd,e->pending_source);}
}

void snap_frame_vk_debug_into(SnapFrameVk*e,VkCommandBuffer cmd,VkImage out,
                              VkImageView out_view,uint32_t w,uint32_t h){
    if(!e||!e->prepared||!e->history_valid||!e->forward.image||!e->mask.image
       ||!cmd||!out||!out_view||!w||!h||e->format!=VK_FORMAT_R8G8B8A8_UNORM)return;
    VkImageView views[3]={e->forward.view,e->mask.view,out_view};
    update_set(e,&e->debug,0,3,3,views);
    struct {uint32_t width,height,flow_width,flow_height;float magnitude_scale;uint32_t debug_mode;} pc={w,h,e->active_flow_w,e->active_flow_h,0.075f,e->debug_mode};
    g_vk.CmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,e->debug.pipeline);
    g_vk.CmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,e->debug.layout,0,1,
                               &e->debug.sets[0],0,NULL);
    g_vk.CmdPushConstants(cmd,e->debug.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(pc),&pc);
    g_vk.CmdDispatch(cmd,div_up(w,8),div_up(h,8),1);
    VkMemoryBarrier to_transfer={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT};
    g_vk.CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&to_transfer,0,NULL,0,NULL);
}


int snap_frame_vk_get_telemetry(const SnapFrameVk*e,float stages_ms[5],float*total_ms){if(!e||!stages_ms||!total_ms)return 0;SnapFrameVk*m=(SnapFrameVk*)e;pthread_mutex_lock(&m->telemetry_mutex);int valid=m->telemetry_valid;if(valid){memcpy(stages_ms,m->stage_gpu_ms,sizeof(m->stage_gpu_ms));*total_ms=m->last_gpu_ms;}pthread_mutex_unlock(&m->telemetry_mutex);return valid;}

int snap_frame_vk_get_flow_metrics(const SnapFrameVk*e,float metrics[4]){if(!e||!metrics)return 0;SnapFrameVk*m=(SnapFrameVk*)e;pthread_mutex_lock(&m->telemetry_mutex);int valid=m->flow_metrics_valid;if(valid)memcpy(metrics,m->flow_metrics,sizeof(m->flow_metrics));pthread_mutex_unlock(&m->telemetry_mutex);return valid;}

void snap_frame_vk_set_debug_mode(SnapFrameVk*e,uint32_t mode){if(e)e->debug_mode=snap_frame_vk_debug_mode(mode);}

void snap_frame_vk_abandon(SnapFrameVk*e){if(!e)return;if(e->ai)snap_ai_destroy(e->ai);if(e->telemetry_mutex_ready)pthread_mutex_destroy(&e->telemetry_mutex);free(e);}