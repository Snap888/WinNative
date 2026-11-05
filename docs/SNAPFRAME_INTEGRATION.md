# SnapFrame integration

Target branch: [`Snap888-patch-1`](https://github.com/Snap888/WinNative/tree/Snap888-patch-1).

## Current implementation

SnapFrame is now a separate Wayland compositor engine, not an alias for LSFG, DIS or WFG. The
branch vendors the portable SnapFrame C API and Snap AI policy library, and registers a real
`SnapFrameBackendV1` callback adapter over the compositor's existing Vulkan device and queue.

Relevant code:

- `app/src/main/cpp/thirdparty/snapframe` — portable core and Vulkan interop ABI.
- `app/src/main/cpp/snapai` — Snap AI policy controller.
- `app/src/main/cpp/snapframe/src/snap_frame_vk.c` — Vulkan compute backend.
- `app/src/main/cpp/waylandcomp/src/framegen_engine.c` — portable-core lifecycle and callback bridge.
- `app/src/main/cpp/waylandcomp/src/framegen_bridge.c` — output ring and generated-frame routing.
- `app/src/main/cpp/waylandcomp/src/vk_present.c` / `effects_chain.c` — scene/effects sampled views and FIFO present path.

## GPU pipeline

1. Half/quarter/eighth-resolution luma pyramid.
2. Bidirectional block-matching optical flow.
3. Flow consistency and occlusion mask.
4. Distinct temporal warp phases for ×2, ×3 and ×4.
5. Generated images written to compositor-owned ring slots.
6. Existing FIFO present loop owns submission, acquire/render semaphores and presentation.

Flow Scale adjusts the active flow grid through push constants while maximum-size flow images remain allocated. This avoids reallocating GPU resources for every slider or AI adjustment.

## Snap AI and telemetry

Timestamp queries measure five GPU stage groups: luma, pyramid, flow, consistency and warp. Completed query slots are collected non-blockingly several frames later. Snap AI compares measured GPU time against a 5 ms budget and returns a bounded ratio multiplied by the user's Flow Scale baseline. AI can reduce the active scale under GPU pressure but cannot silently exceed the selected baseline.

The current policy receives measured GPU timing plus neutral motion/confidence priors. GPU reductions for actual flow confidence, motion complexity and artifact risk are not yet implemented.

## Vulkan and WSI ownership

The backend borrows the compositor's `VkDevice`, queue, command buffer, source view and output slots. It does not create another device/queue, submit the host command buffer, wait for GPU completion, perform CPU readback, own WSI semaphores or present images itself. Resource replacement happens at the compositor's serialized frame boundary after its per-frame fence has completed.

The host supplies output slots in GENERAL layout. SnapFrame writes them and leaves them for the compositor's existing transfer/present path. LSFG and DIS remain independent.

## Settings

- Per-game Game Settings: choose LSFG or SnapFrame, enable generation, select multiplier and set Flow Scale.
- In-game overlay: select OFF, LSFG, DIS or SnapFrame; change multiplier and Flow Scale.
- SnapFrame is shown as available only when the current Wayland Vulkan device reports the required storage/sample formats and compute timestamp-query support.

## Tests and remaining validation

Standalone contract tests cover ×2/×3/×4 phase fractions, flow-grid bounds, RGBA8/RGBA16F format cases, resize, format/HDR change, scene-cut reset, arm toggle and device-generation change. A static WSI ownership test rejects queue submission, present calls, semaphore creation and GPU waits from the SnapFrame backend.

The branch CI builds the APK flavor matrix and runs these tests. Physical Turnip/Adreno runtime validation is still required. Scene discontinuity reset currently uses explicit core events and a frame gap greater than 500 ms; pixel-based scene-cut detection is not yet implemented. Long-run testing must still confirm no black/stale frames and correct phase ordering.
