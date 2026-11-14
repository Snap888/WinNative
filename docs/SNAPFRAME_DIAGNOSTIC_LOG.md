# SnapFrame diagnostic logging

The session drawer's Frame Generation → SnapFrame section has an opt-in **SnapFrame diagnostic log** switch. It is disabled by default.

When enabled, WinNative starts a filtered Android logcat capture and writes:

`Download/winnative/snapframe.log`

The capture does not clear the device-wide log buffer. It filters the `SnapFrame`, `VkRenderer`, and `SnapFrameLog` tags. Turning the switch off stops the capture; the activity also stops it during teardown. The setting is stored with the active shortcut/container.

## Logged telemetry

SnapFrame emits one compact diagnostic sample per 60 completed source frames:

- source frame sequence number;
- total GPU compute time and the existing preprocess, flow, consistency, and warp timing buckets;
- configured flow scale and active flow-grid extent;
- candidate reconstruction mode and whether candidate reconstruction was active;
- Snap AI decision mode and flow-scale multiplier;
- normalized motion, complexity, confidence, GPU-pressure, and artifact-risk metrics when the asynchronous metrics readback is valid.

The existing four-slot timestamp/readback ring is reused. Logging does not add a queue/device wait or a new per-frame GPU readback. Candidate reconstruction stages are still grouped into the warp timing bucket; do not interpret that bucket as a candidate-only measurement.

## Debugging procedure

1. Enable the switch before launching or while running a SnapFrame session.
2. Reproduce the stutter/artifact for at least 60 source frames so a telemetry sample is emitted.
3. Turn logging off and attach `Download/winnative/snapframe.log`.
4. Compare identical scenes with candidate mode Auto, Off, and On. Keep resolution, multiplier, and flow scale fixed.
5. Inspect GPU time, flow confidence/consistency proxies, AI mode/flow-scale changes, and whether candidate reconstruction was selected.

If Android denies write access to the public Download directory, the switch is reset and the failure is written to the app log. The application may need its existing all-files storage permission enabled.
