# SnapFrame integration (Snap_AI_Engine)

Target branch: `Snap888-patch-1`

## Current state

WinNative's embedded Wayland compositor already owns a Vulkan device, command buffers, swapchain and present pacing. Its frame-generation bridge currently hosts the existing LSFG and DIS engines. SnapFrame is a separate engine and must not be aliased to DIS/WFG or LSFG.

The portable SnapFrame API is maintained in [Snap_AI_Engine](https://github.com/Snap888/Snap_AI_Engine), under `framegen/`. The API now accepts multipliers 1..4; this is a core contract capability only. It does not mean that a WinNative Vulkan implementation or ×3/×4 GPU passes are present.

## Correct integration boundary

The compositor adapter belongs beside:

- `app/src/main/cpp/waylandcomp/src/framegen_bridge.c`
- `app/src/main/cpp/waylandcomp/src/framegen_engine.c`
- `app/src/main/cpp/waylandcomp/src/vk_present.c`

The adapter must implement `SnapFrameBackendV1` and consume host-owned Vulkan resources. It must not create a second VkDevice, queue, command pool or independent present scheduler.

## Required adapter behavior

1. Initialize against the compositor's existing VkPhysicalDevice/VkDevice/VkQueue and resolve capabilities for the actual scene/ring formats.
2. Capture/retain consecutive scene images through host-owned references and synchronization. Never retain a swapchain image beyond its valid host lifetime.
3. Record the SnapFrame passes: luma/downsample pyramid, bidirectional motion estimation, flow consistency, occlusion rejection, multi-phase warp and composite.
4. Emit 1, 2 or 3 generated images for ×2, ×3 or ×4 respectively, in chronological temporal-phase order. Do not duplicate one midpoint to simulate multiple phases.
5. Return images to the compositor's existing present loop. Preserve per-present acquire/render semaphores, FIFO pacing and generated-before-source ordering.
6. Reset temporal history on resize, format change, scene cut, enable/disable, surface recreation and device loss.
7. Keep the adapter disabled when required Vulkan formats/features or shader pipelines are unavailable. Failure must fall back to source-frame presentation, never black frames.

## AI/control integration

Snap AI should consume low-overhead telemetry from the adapter: source/presented FPS, per-stage GPU time, queue latency, dropped/failed generated frames, flow scale, resolution, thermal state when available. Adapt flow scale and quality with hysteresis and bounded changes; never change Vulkan resources from a non-render thread. The host applies pending config at a safe render-thread boundary and resets history when a change invalidates it.

## Validation gate before exposing the UI option

- Android NDK arm64 build with adapter enabled and disabled.
- Vulkan capability probe and graceful fallback on unsupported devices.
- Stable source-only fallback on initialization/chain failure.
- Resize, HDR/format switch, scene-cut and device-loss tests.
- Semaphore ownership audit and FIFO pacing verification.
- Long-run stress test with no black frames, stale images or unbounded allocations.
- Verify ×2, ×3 and ×4 generated phase ordering and counters independently.

## Status

This document records the integration contract and sequencing. The WinNative SnapFrame Vulkan adapter is **not yet implemented**. Until the adapter and validation gate above are complete, the UI must continue to expose only engines that are actually present in the build.
