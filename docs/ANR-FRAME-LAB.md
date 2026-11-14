# ANR Frame Lab (WinNative integration)

This is the Android-native capture core for a future ANR-style record/replay workflow. It is
intentionally separate from either compositor so X11 and Wayland can submit identical capture
records without depending on one another's Vulkan dispatch table.

## Current implementation

- A bounded three-entry queue; the render thread copies a submitted payload into an owned slot and
  returns immediately. A dedicated writer thread performs file I/O.
- Lossless raw payload storage in `frames.bin`, fixed-width little-endian records in `frames.idx`,
  opaque per-frame parameter blobs in `params.bin`, and a small `record.json` manifest.
- Explicit capture stages: source, pre-effect, effect output, frame-generation input,
  frame-generation output, final output.
- Queue saturation is visible as `EAGAIN` and increments the dropped-frame counter; no render
  thread waits for disk I/O.
- Payload size limits prevent unbounded allocations (256 MiB/frame, 1 MiB/parameter blob).

## Important scope boundary

The native library is linked into both X11 (`libwinlator`) and Wayland (`libwnwayland`). The
submit API is ready for Vulkan readback integration, but this commit does **not** claim to capture
GPU images automatically: a compositor must first copy the relevant VkImage to a host-visible
staging buffer, wait on a fence asynchronously, then call `anr_fl_submit`. That GPU readback path
is the next integration step. This avoids a synchronous image readback or queue stall on the
presentation thread.

## File layout

| File | Contents |
| --- | --- |
| `record.json` | Format and stream names |
| `frames.idx` | Stage, dimensions, format, frame/timestamp, byte offsets |
| `frames.bin` | Lossless image bytes, concatenated |
| `params.bin` | Opaque frame-generation/effect parameter snapshots |

`format=ANR-FL-1` is a WinNative-local format, not a claim of byte compatibility with upstream
ANR's private capture files. It deliberately preserves the same separation between metadata,
frame payloads and parameters to make later replay adapters possible.

## Capture contract

Use a consistent frame ID across all stages from one real frame. Record timestamps in monotonic
nanoseconds. Store actual motion/depth/HUD-less/UI inputs only when the engine exposes them; do not
synthesize them from the final composited image. The format field identifies the submitted pixel
layout and must be interpreted by the decoder.

## Metrics update

The current metrics implementation now uses non-overlapping local 8x8 SSIM windows (with
smaller edge windows) instead of one global luminance/variance score. Temporal comparison runs
a bounded global translation search (±8 pixels, coarse 4-pixel samples), aligns the frames, and
computes residual RGB flicker on the overlapping region. The estimated integer translation is
returned as `motion_x` / `motion_y`.

This is motion-compensated global translation, **not dense optical flow**. It handles simple
camera pans and fixed translations, but cannot compensate independently moving objects,
rotation, parallax, or non-rigid motion. Scores are therefore diagnostic metrics, not a
replacement for a full optical-flow temporal consistency benchmark.

## Integration status (explicit)

- The six stage identifiers and session file format exist.
- The X11 renderer has a stable scene snapshot under `scene_mutex`, real effect slots, and
  concrete effect/frame-generation command-recording boundaries. These are suitable integration
  points, but the Frame Lab submit path is not yet connected to those GPU images.
- The Wayland renderer has a scene image, a synchronized effects-chain boundary, and the real
  frame-generation bridge. Its own Vulkan dispatch table and queue lifetime are separate from
  X11's; capture resources must be created/destroyed per backend/device.
- Neither backend currently has a verified per-slot Vulkan readback ring with independent capture
  fences. The existing presentation fences must not be reused for capture.
- Generator replay adapters are not implemented. The current replay API reads stored records only.
- No Android Frame Lab screen is wired into app navigation yet.

Do not interpret these integration points as completed capture or replay functionality.
