# SnapFrame integration

Target branch: [`Snap888-patch-1`](https://github.com/Snap888/WinNative/tree/Snap888-patch-1).

## Current implementation

SnapFrame is a separate Vulkan frame-generation engine, not an alias for LSFG, DIS or WFG. The
branch vendors the portable SnapFrame C API and Snap AI policy library. Both the Wayland compositor
and the embedded X11 `VulkanRenderer` use the same backend over their already-owned Vulkan device;
there is no second device or queue created for frame generation.

Relevant code:

- `app/src/main/cpp/thirdparty/snapframe` — portable core and Vulkan interop ABI.
- `app/src/main/cpp/snapai` — Snap AI policy controller.
- `app/src/main/cpp/snapframe/src/snap_frame_vk.c` — Vulkan compute backend.
- `app/src/main/cpp/waylandcomp/src/framegen_engine.c` — portable-core lifecycle and callback bridge.
- `app/src/main/cpp/waylandcomp/src/framegen_bridge.c` — output ring and generated-frame routing.
- `app/src/main/cpp/waylandcomp/src/vk_present.c` / `effects_chain.c` — Wayland scene/effects sampled views and FIFO present path.
- `app/src/main/cpp/winlator/vk/vk_renderer.c` — embedded X11 render/composite/present path; SnapFrame phases are recorded into the renderer's command buffer and compositor-owned composite targets.
- `app/src/main/cpp/waylandcomp/src/vk_loader.c` — non-owning dispatch binding for the existing X11 Vulkan instance/device.

## GPU pipeline

1. Half/quarter/eighth-resolution luma pyramid.
2. Bidirectional block-matching optical flow.
3. Flow consistency and occlusion mask.
4. Distinct temporal warp phases for ×2, ×3 and ×4.
5. Generated images written to compositor-owned ring slots.
6. The host's existing present loop owns submission, acquire/render semaphores and presentation (FIFO for the X11 generated-frame cadence).

Flow Scale adjusts the active flow grid through push constants while maximum-size flow images remain allocated. This avoids reallocating GPU resources for every slider or AI adjustment.

## Snap AI and telemetry

Timestamp queries measure five GPU stage groups: luma, pyramid, flow, consistency and warp. Completed query slots are collected non-blockingly several frames later. Snap AI compares measured GPU time against a 5 ms budget and returns a bounded ratio multiplied by the user's Flow Scale baseline. AI can reduce the active scale under GPU pressure but cannot silently exceed the selected baseline.

The current policy receives measured GPU timing plus neutral motion/confidence priors. GPU reductions for actual flow confidence, motion complexity and artifact risk are not yet implemented.

## Vulkan and WSI ownership

The backend borrows the host's `VkDevice`, command buffer, source view and output slots. It does not create another device/queue, submit the host command buffer, wait for GPU completion, perform CPU readback, own WSI semaphores or present images itself. On X11, the renderer records SnapFrame work in its existing serialized graphics command buffer and owns all swapchain acquire/present operations. Resource replacement happens at the compositor's serialized frame boundary after its per-frame fence has completed.

The host supplies output slots in GENERAL layout. SnapFrame writes them and leaves them for the compositor's existing transfer/present path. LSFG and DIS remain independent.

## Settings

- In-game overlay: select OFF, LSFG, DIS or SnapFrame; change multiplier and Flow Scale.
- On X11, SnapFrame availability is probed against the active swapchain: RGBA8 UNORM storage/sample support, required R16/RGBA16F compute formats, timestamp queries and at least three swapchain images are required.
- The SnapFrame panel exposes **Debug Flow**. When enabled, the current output is replaced with a live GPU visualization of SnapFrame's own bidirectional vectors and consistency/occlusion mask (hue = direction, brightness = magnitude, dark = invalid/occluded). Generated frames continue through the normal X11 present path.
- Wayland retains its separate compositor capability probe; X11 uses `VulkanRenderer.nativeIsSnapFrameGenerationAvailable`.

## Tests and remaining validation

Standalone contract tests cover ×2/×3/×4 phase fractions, flow-grid bounds, RGBA8/RGBA16F format cases, resize, format/HDR change, scene-cut reset, arm toggle and device-generation change. A static WSI ownership test rejects queue submission, present calls, semaphore creation and GPU waits from the SnapFrame backend.

The branch CI builds the APK flavor matrix and runs these tests. Physical Turnip/Adreno runtime validation is still required, including checking the X11 debug-flow view, generated/presented frame counters, resize handling and sustained FIFO pacing. Scene discontinuity reset currently uses explicit core events and a frame gap greater than 500 ms; pixel-based scene-cut detection is not yet implemented. Long-run testing must still confirm no black/stale frames and correct phase ordering.
