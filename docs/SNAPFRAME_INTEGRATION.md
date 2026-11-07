# SnapFrame integration

Target branch: [`Snap888-patch-1`](https://github.com/Snap888/WinNative/tree/Snap888-patch-1).

## Current implementation

SnapFrame is a separate Vulkan frame-generation engine, not an alias for LSFG, DIS or WFG. The branch vendors the portable SnapFrame C API and Snap AI policy library. The embedded X11 renderer and Wayland compositor both use the WinNative SnapFrame Vulkan compute implementation over their already-owned Vulkan device, but the host adapters differ:

- **Embedded X11:** the native `VulkanRenderer` path in `app/src/main/cpp/winlator/vk/vk_renderer.c` calls `snap_frame_vk_process/generate` directly in its serialized renderer command buffer. It routes generated images through its own swapchain/present path.
- **Wayland:** `app/src/main/cpp/waylandcomp/src/framegen_engine.c` wraps the portable `SnapFrameBackendV1` core and adapts compositor-owned Vulkan resources into `SnapFrameVulkanFrame`.

Relevant code:

- `app/src/main/cpp/thirdparty/snapframe` — portable core and Vulkan interop ABI.
- `app/src/main/cpp/snapai` — Snap AI policy controller.
- `app/src/main/cpp/snapframe/src/snap_frame_vk.c` — shared WinNative Vulkan compute backend and Snap AI policy application.
- `app/src/main/cpp/waylandcomp/src/framegen_engine.c` — Wayland portable-core lifecycle and callback bridge.
- `app/src/main/cpp/waylandcomp/src/framegen_bridge.c` — Wayland output ring and generated-frame routing.
- `app/src/main/cpp/waylandcomp/src/vk_present.c` / `effects_chain.c` — Wayland scene/effects sampled views and FIFO present path.
- `app/src/main/cpp/winlator/vk/vk_renderer.c` — embedded X11 render/composite/present path, capability probe, frame routing, Debug Flow and frame counters.
- `app/src/main/runtime/display/renderer/VulkanRenderer.java` — X11 Java/JNI settings bridge.

## GPU pipeline

1. Half/quarter/eighth-resolution luma pyramid.
2. Bidirectional block-matching optical flow.
3. Flow consistency and occlusion mask.
4. Distinct temporal warp phases for ×2, ×3 and ×4, subject to available output capacity.
5. Generated images written to host-owned output targets.
6. The host's existing present loop owns submission, acquire/render semaphores and presentation.

Flow Scale adjusts the active flow grid through push constants while maximum-size flow images remain allocated. This avoids reallocating GPU resources for every slider or AI adjustment.

## Snap AI and telemetry

Timestamp queries measure five GPU stage groups: luma, pyramid, flow, consistency and warp. Completed query slots are collected non-blockingly several frames later. Snap AI compares measured GPU time against a 5 ms budget and returns a bounded ratio multiplied by the user's Flow Scale baseline.

The X11 Vulkan backend now reduces the already-computed forward flow and consistency mask in one 256-lane compute workgroup. It writes four normalized values to a 1x1 RGBA32F image: mean motion magnitude, motion-magnitude deviation, confidence weighted by forward/backward consistency, and complementary artifact risk. The image is copied to a 16-byte host-visible/coherent staging buffer in a four-slot ring. CPU mapping happens only when the corresponding timestamp-query slot is already available several frames later; no queue/device wait and no full-resolution image readback are used.

Snap AI now receives these measured values together with GPU time. Motion magnitude contributes to the controller's smoothed scene-complexity input, so large uniform motion is not treated as a trivially static scene. Metrics are capability-gated: if buffer readback entry points, coherent host memory, or the RGBA32F storage image are unavailable, SnapFrame interpolation remains enabled and content metrics are simply skipped. The Wayland adapter still needs its own telemetry handoff before it can expose the same content-aware AI behavior.

## Vulkan and WSI ownership

The backend borrows the host's Vulkan device, command buffer, source view and output slots. It does not create another device/queue, submit the host command buffer, wait for GPU completion, read back source/generated images, own WSI semaphores or present images itself. The optional AI telemetry path copies only 16 bytes of aggregate metrics into a ring-buffer staging slot and maps it only after the host's normal frame/query completion proves that slot is finished. On X11, the renderer owns swapchain acquire/present operations. On Wayland, the compositor owns its FIFO present path. Resource replacement must occur at the host's serialized safe frame boundary.

## Settings and diagnostics

- In-game overlay: select OFF, LSFG, DIS or SnapFrame; change multiplier and Flow Scale.
- X11 availability is checked through `VulkanRenderer.nativeIsSnapFrameGenerationAvailable` against the active renderer/device/swapchain capabilities.
- The X11 SnapFrame panel exposes **Debug Flow**. It visualizes the forward motion field and consistency/occlusion mask in the live output (hue = direction, brightness = magnitude, dark = invalid/occluded); both forward and backward fields are used for interpolation.
- X11 logs delivered real/generated frame counters and GPU stage timings.
- Wayland retains its separate compositor capability probe and lifecycle bridge.

## Tests and remaining validation

The branch CI includes contract and static WSI ownership tests and builds the APK flavor matrix. The [Tag APK Artifacts run 42134152224](https://github.com/Snap888/WinNative/actions/runs/42134152224) passed all four APK variants on commit `43fdd2f6a3cb214e52bc41134d7e8b9a36f8504e`.

Physical Turnip/Adreno runtime validation is still required, including checking the X11 Debug Flow view, generated/presented frame counters, resize handling, ×2/×3/×4 output ordering and sustained FIFO pacing. Scene discontinuity reset currently uses explicit events and a frame gap greater than 500 ms; pixel-based scene-cut detection is not implemented. Long-run testing must still confirm no black/stale frames and correct phase ordering.

## Generic Winlator ports

This branch's embedded X11 and Wayland support must not be generalized to every Winlator-derived APK. Other ports need an adapter for their exact renderer/compositor and DXVK fork/version. DXVK integration is a separate host adapter; the legacy DXVK migration/reference code in Snap_AI_Engine is not a production-ready universal Winlator backend.


### Metrics portability and slot retirement

The flow-metrics reduction uses a 128-invocation workgroup, matching Vulkan's guaranteed minimum compute workgroup invocation count. The asynchronous four-slot readback now retires its metrics-pending flag as soon as the corresponding timestamp slot is collected, including unavailable/invalid timestamp paths. This prevents a recycled query slot from carrying stale metrics state into a later frame. Metrics remain optional: missing readback entry points or unsupported allocations disable telemetry without disabling interpolation.


### Flow metrics telemetry handoff

The Vulkan backend exposes the latest completed four-channel GPU reduction through
`snap_frame_vk_get_flow_metrics()`: normalized motion magnitude, magnitude deviation
(scene complexity proxy), confidence weighted by forward/backward consistency, and
complementary artifact risk. The X11 renderer logs these values in its existing 120-frame
telemetry interval alongside per-stage GPU timings. The Wayland frame-generation adapter
exposes the same completed sample through `fge_flow_metrics()` and logs it every 120 source
frames. The readback remains delayed by the existing four-slot asynchronous ring; these
logs report completed samples, never trigger a synchronous GPU wait.

The existing X11 Flow Viewer remains the visual flow/consistency view. The numeric metrics
are currently available in logs; selectable confidence/occlusion/photometric-residual viewer
layers remain future work and are not claimed as implemented.


### Selectable X11 Flow Viewer layers

The existing SnapFrame debug compute pass now has three selectable modes: motion
(direction hue and magnitude brightness), matcher confidence (the block matcher's
best/second-best SAD separation stored in flow.z), and forward/backward consistency
(the existing consistency mask, which also incorporates local matcher confidence).
The mode is a push constant in the same compute dispatch; no extra flow pass,
image allocation, CPU readback, or synchronous GPU wait is introduced. The X11
frame-generation drawer exposes the three modes while Debug Flow is enabled.
