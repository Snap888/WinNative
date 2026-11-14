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
