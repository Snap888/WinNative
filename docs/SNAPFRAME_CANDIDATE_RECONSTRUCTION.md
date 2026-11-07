# SnapFrame four-candidate reconstruction (experimental)

## Status

This is an optional Vulkan compute path layered on top of the existing SnapFrame flow pipeline.
The original midpoint warp remains available as the fallback and is used unless the feature is
forced on or Snap AI selects its quality mode.

No shader source or FP16 model weights from the external `gsll` archive are copied. The
implementation uses the archive only as an algorithmic reference: multiple candidate warps,
confidence-aware scoring, and normalized candidate blending.

## Pipeline

For each generated phase:

1. `candidate_warp.comp` reads forward flow, backward flow, and the forward/backward
   consistency mask. It produces four candidate displacement records at the flow resolution:
   - primary previous-frame warp;
   - primary current-frame warp;
   - previous-frame warp using the best edge-compatible neighbour flow;
   - current-frame warp using the best edge-compatible neighbour flow.

   Each RGBA16F record stores `offset.xy, confidence, in_bounds`. Offsets are relative to
   the full-resolution output pixel, not absolute cell-center coordinates. The neighbour
   search is limited to the center and four cardinal neighbours; candidates whose motion
   differs by more than 3.5 flow units are rejected to reduce foreground/background leakage.

2. `candidate_score.comp` samples the two source frames at the flow-cell center plus each
   candidate offset. It combines each candidate's flow confidence and in-bounds validity with
   the photometric difference of its forward/backward pair. Four scores are softmax-normalized
   into an RGBA16F weight image. If all candidates are unreliable, it writes zero weights.

3. `candidate_reconstruct.comp` (RGBA16F) or
   `candidate_reconstruct_rgba8.comp` (RGBA8) applies those weights at full output resolution.
   When the weight sum is zero, it samples the current real frame as a sharp fallback.

Candidate metadata and weights are stored at flow resolution. Four candidate images plus one
weight image are allocated per generated phase; no full-resolution intermediate RGB candidate
surfaces are allocated.

## AI and manual control

The native control is exposed on both display paths:
- Wayland: `WaylandCompositor.nativeSetSnapFrameCandidateReconstruction(mode)`
- X11: `VulkanRenderer.setSnapFrameCandidateReconstruction(mode)` → JNI → `snap_frame_vk_set_candidate_reconstruction()`:

| Mode | Value | Behaviour |
|---|---:|---|
| AI automatic | 0 | Enable only when Snap AI selects `SNAP_AI_MODE_QUALITY`, motion and flow confidence exceed conservative minimums, artifact risk is below 0.20, and normalized GPU pressure is below 0.98; disable above 1.15 |
| Forced off | 1 | Always use the original midpoint warp |
| Forced on | 2 | Always use four-candidate reconstruction |

AI automatic is the default. The GPU budget used by the policy is 7 ms; the 0.98/1.15 enable/disable thresholds provide hysteresis so a brief candidate-stage cost increase does not immediately switch the path off. Motion, complexity, confidence and artifact-risk gates prevent enabling candidate reconstruction on static, uncertain or heavily occluded frames. Snap AI also controls the block matcher search window: the default ±8 coarse-luma-texel search can expand up to ±12 (plus the local refinement window) when motion uncertainty rises, helping fast camera turns avoid search-boundary vectors. The session drawer exposes AI Auto, Off, and On whenever the SnapFrame Vulkan backend is available, on both X11 and Wayland.

## Validation requirements

- Compile all four shaders with the project's `glslc --target-env=vulkan1.1 -O` step.
- Run SnapFrame contract and WSI ownership tests.
- Build the Android native target for all CI APK flavours.
- Test both RGBA8 and RGBA16F paths on the target Android Vulkan driver.
- Compare forced-off, forced-on, and AI-auto using the same captured frame sequence.
- Record generated-frame GPU time, candidate-stage GPU time (to be split out in a later
  telemetry update), ghosting, disocclusion artifacts, and fallback ratio.

## Known limitations

- The four candidates are deterministic flow-derived alternatives, not four independent learned
  optical-flow predictions.
- The neighbour alternative is deliberately conservative and may collapse to the center flow.
- The initial scorer uses photometric pair error and existing flow confidence/consistency. It
  does not yet estimate semantic occlusion or HUD regions.
- The current timestamp query groups candidate processing into the existing warp timing bucket.
  Separate per-stage GPU timings should be added before claiming a performance win.
- AI quality mode is the first control policy. A later policy should consider artifact risk,
  reconstruction cost, and thermal/GPU budget together.

The original midpoint warp remains the safe rollback path at all times.

## Flow Viewer stability

On X11, the diagnostic shader writes across the full composite target, which is then blitted to the full swapchain extent. Its display is deliberately independent of whether an extra generated swapchain image was acquired: acquisition can time out under FIFO pacing even though the latest completed flow field remains valid. When no generated image can be acquired, X11 submits a flow-only SnapFrame pass (`UINT32_MAX` sentinel): it computes the newest flow field and advances history without generating an intermediate frame. The viewer therefore stays full-target and continuously refreshed instead of blinking or freezing with `gen_count`.
