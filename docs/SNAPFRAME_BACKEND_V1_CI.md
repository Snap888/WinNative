# SnapFrameBackendV1 CI integration (WinNative)

This branch contains a CI gate that fetches the pinned portable SnapFrame core, runs its host unit tests and cross-compiles the Android arm64 static library. WinNative currently owns a separate Wayland compositor frame-generation path (`framegen_bridge` / `framegen_engine`) and does not yet implement the SnapFrame Vulkan backend. The correct next integration point is the compositor's scene-frame hook, not DXVK's private hook ABI. This workflow validates the portable dependency only, not frame generation in the WinNative runtime.
