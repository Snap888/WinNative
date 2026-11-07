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

The native control is exposed as
`WaylandCompositor.nativeSetSnapFrameCandidateReconstruction(mode)`:

| Mode | Value | Behaviour |
|---|---:|---|
| AI automatic | 0 | Enable when Snap AI selects `SNAP_AI_MODE_QUALITY` and GPU pressure is below 0.75; disable above 0.90 |
| Forced off | 1 | Always use the original midpoint warp |
| Forced on | 2 | Always use four-candidate reconstruction |

AI automatic is the default. The Wayland SnapFrame settings pane exposes AI Auto, Off, and On for repeatable A/B testing. The feature is therefore not silently enabled for existing users.

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
