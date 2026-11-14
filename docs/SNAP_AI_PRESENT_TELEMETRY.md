# Snap AI presentation telemetry in WinNative

The portable recorder is built into the `snap_ai` static target and is available
to native host adapters through `snap_ai/frame_telemetry.h`.

The recorder is deliberately passive: it does not hook Vulkan, call Present,
wait for a queue, change present mode, or pace frames. The host must feed it
events from the outermost presentation boundary and provide a monotonic
nanosecond timestamp, a stable logical present ID, stream type (source or
generated), and swapchain generation.

## Adapter-specific wiring still required

- X11 embedded renderer: record each successful generated-image present and
  successful source-image present at the existing `vkQueuePresentKHR` sites in
  `winlator/vk/vk_renderer.c`. Keep the existing mirror/recording swapchain
  out of output FPS accounting.
- Wayland compositor: instrument the compositor's existing present completion
  boundary; do not infer actual presentation from a frame-generation callback
  or command-buffer submission.
- Increment the swapchain generation on recreation and reset the recorder.
  Do not count failed/out-of-date presents as displayed frames.
- Ensure one logical present ID covers any nested/wrapped API callback. In
  DXVK environments, instrument only the outermost Present/Present1 boundary
  unless both callbacks share a correlation ID.

The recorder is not yet connected to these presenter call sites and must not
be represented as live FPS telemetry until that wiring is completed. This
keeps the current presentation, semaphore, and pacing behavior unchanged.
