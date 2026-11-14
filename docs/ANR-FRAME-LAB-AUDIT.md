# ANR Frame Lab — implementation audit

Audit target: `anr-frame-lab`, PR #9.

## Verified current state

The branch contains a native C recording core, a three-slot bounded queue, a writer thread, a raw frame stream, a fixed-width index, an opaque parameter stream, a replay-file iterator, basic RGBA8 metrics, a host smoke test, and a Standard debug APK workflow.

The core is linked into both `libwinlator` and `libwnwayland`. This is infrastructure only: the code and PR description explicitly state that live Vulkan image capture and user recording controls are not wired yet.

## Blocking gaps against the requested deliverable

1. **Vulkan staging/readback is absent.** There are no per-slot staging buffers, buffer memory mapping/invalidation, transfer barriers, per-readback fences, or completion worker. The compositor currently has one presentation fence and waits for it in its presentation path. Reusing that fence for capture would couple recording to presentation and is not acceptable.
2. **Capture points are declarations, not live hooks.** The six stage enum values exist, but no verified X11 or Wayland call sites submit Source, Pre-effect, Effect output, Generator input/output, or Final output.
3. **Settings snapshots are not connected to settings owners.** `anr_fl_config_v1` is a useful versioned container, but the smoke test fills it with constants. It does not prove that the actual X11/Wayland effect settings or generator controls are sampled atomically for the same frame.
4. **Replay is file iteration, not generator replay.** `anr_fl_replay_next` only decodes the recorded record. There is no adapter that feeds captured frame pairs and saved settings into the real LSFG, WFG, or SnapFrame runtime.
5. **No Frame Lab UI exists in this change.** There are no recording controls, session browser, export action, replay controls, or A/B comparison screen.
6. **Metrics are preliminary.** The current SSIM calculation is a single global statistic, not windowed SSIM. Temporal flicker is raw consecutive-frame difference and is not motion-compensated, so camera motion can dominate the score.
7. **Writer/replay hardening is incomplete.** The writer uses `ftell`/native `long` offsets, records zero when offset lookup fails, and continues draining after a write failure. The index is a native C struct (host endianness/layout), and replay does not validate stream file bounds or confirm the payload size against dimensions/format. The submit API also allocates and copies the entire image on the caller thread; a 4K RGBA frame is a substantial render-thread copy.
8. **CI coverage is narrow.** The workflow runs a host writer smoke test and builds only `assembleStandardDebug`; it does not exercise Vulkan capture, X11/Wayland runtime paths, replay through a generator, or the four-flavor APK matrix.

## Required implementation order

1. Add a bounded Vulkan readback ring with one staging allocation and one dedicated fence per slot. Record image-to-buffer copies and barriers on the compositor's existing command stream; do not wait on the render thread. A worker polls/waits the slot fence, invalidates non-coherent mapped memory, and transfers ownership to the writer without a second full-frame copy.
2. Integrate the ring at actual X11 and Wayland render graph boundaries. Keep image layout/queue ownership transitions correct and skip unsupported formats explicitly rather than silently recording corrupt pixels.
3. Snapshot the real settings under each subsystem's existing synchronization at the source-frame boundary; attach the same immutable snapshot and frame ID to every stage of that frame.
4. Add generator-specific replay adapters. Replay must call the real backend entry points, preserve pair ordering/timestamps and settings, and report unsupported backend/config combinations instead of silently substituting file playback.
5. Add a Frame Lab Android screen and JNI/API surface for record/stop, sessions, stage selection, replay backend, export, A/B, and metric display.
6. Harden the on-disk format and writer failure handling; add deterministic tests for truncation, partial writes, overflow, queue saturation, stop/drain, and schema/version mismatch.
7. Expand CI to compile native code, run host tests, build the requested APK variants, and upload installable artifacts.

## Acceptance criteria

- Recording does not call `vkWaitForFences` on the render/present thread.
- Each in-flight readback has an independent fence and bounded memory usage.
- X11 and Wayland produce verified stage records from real rendered frames.
- Every stage record carries the real frame's configuration snapshot.
- Replay invokes the selected real generator and records its output.
- UI can start/stop, browse, export, replay, and compare sessions.
- CI artifacts are installable APKs; runtime claims remain marked pending until device validation.

This audit intentionally does not label the existing file reader as generator replay or the existing writer as Vulkan capture.
