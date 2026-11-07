# Frame generation

WinNative can interpolate extra frames between the ones your game actually renders. Interpolation
runs **on the Android side**, inside WinNative's Vulkan compositor rather than inside the Wine
container, so it works with any graphics API Wine can drive — DXVK, WineD3D or native Vulkan alike.

There are **three independent engines** in the **FG** tab of the session drawer. Pick one; the
compositor drives a single interpolator per frame. Each engine keeps its own settings.

| Engine | Needs | Character |
| --- | --- | --- |
| **Lossless Scaling (LSFG)** | Your own copy of Lossless Scaling on Steam | The LSFG shader chain ported to Vulkan |
| **WFG** | Nothing — ships with the APK | Dense Inverse Search optical flow, fully open source |
| **SnapFrame** *(experimental)* | Wayland compositor and a compatible Turnip Vulkan device | Native SnapFrame compute pipeline with bidirectional flow, occlusion handling, multi-phase warp and Snap AI GPU-budget control |

## Lossless Scaling (LSFG)

**You must own [Lossless Scaling](https://store.steampowered.com/) on Steam.** Its shaders are
not redistributable, so nothing ships with the APK. WinNative reads them out of your own copy of
`Lossless.dll`, translates them from DXBC to SPIR-V once, and caches the result in app storage.
The DLL is parsed as data and never executed.

**Setup:** sign in to Steam, install Lossless Scaling, then open **Container Settings → Frame
Generation**. WinNative finds the DLL automatically from your Steam library; if it can't, use
**Select Lossless.dll…** to point at it. The LSFG half of the **FG** tab stays disabled until the
shaders import successfully.

## WFG — WinNative/Winlator Frame Generator

**Nothing to buy, nothing to import.** WFG (WinNative/Winlator Frame Generator, MIT) is a
complete open-source Dense Inverse Search frame generator built into WinNative — compute shaders
that ship with the APK — so it works on a fresh install with no Steam account and no
`Lossless.dll`. Turn it on in the **FG** tab and it runs.

## SnapFrame — native Vulkan *(experimental branch)*

SnapFrame is a separate frame-generation engine built into the Wayland compositor. It uses the
compositor's existing Vulkan device and FIFO present loop; it does not depend on LSFG, DIS or WFG.
The runtime selector enables it only when the current Vulkan device supports the required storage
formats and compute timestamp queries.

The pipeline builds a three-level luma pyramid, estimates forward/backward optical flow, rejects
inconsistent flow with an occlusion mask, and reconstructs distinct temporal phases for 2×, 3× and
4× output. Snap AI consumes measured stage GPU times and scales the selected Flow Scale baseline
down when the GPU exceeds its budget.

SnapFrame settings are available in **per-game Game Settings** and the in-game **FG** overlay.
The branch is experimental: a successful APK build and physical Turnip/Adreno stress test are
still required before calling it hardware-validated. Current scene-cut resets are event/frame-gap
based rather than pixel-content detection.

## Controls

| Control | Engine | What it does |
| --- | --- | --- |
| Generate Frames | LSFG / WFG | Master toggle |
| Engine | SnapFrame | Select OFF / LSFG / DIS / SnapFrame in the overlay; per-game settings offer LSFG or SnapFrame |
| Multiplier | LSFG / SnapFrame | 2× / 3× / 4× generated phases |
| Adaptive Target | LSFG | Aim for a specific output rate (60/90/120/144/165) instead of a fixed multiplier |
| Flow Scale | LSFG / SnapFrame | 25–100%; SnapFrame treats this as the AI's upper baseline |
| Snap AI | SnapFrame | Automatically reduces Flow Scale when measured GPU time exceeds the target budget |
| Resolution Scale | WFG | Fast / Balance — 180 / 252 pixels on the frame's shorter side |
| Target FPS | WFG | Max refresh rate, or a specific one (60/90/120/144/165) |
| Show flow | WFG | Debug view: renders the estimated motion field instead of the frame |
| FPS Limiter | All | Caps the game's own frame rate, from 15 fps upward |

## What to expect

Frame generation costs **one extra frame of input latency** — interpolating between two frames
means holding the newer one back. It also needs spare display refresh: generated frames occupy
vblanks, so WinNative sizes the multiplier against your panel's refresh rate and the game's actual
frame rate, and will hand back generated frames rather than take real ones from the game. A game
already running near your panel's refresh rate has nothing to gain. Pairing a multiplier with an
FPS limiter that divides the refresh rate evenly (120 Hz with a 60 fps cap at 2×, or 40 at 3×)
gives the most even pacing.

## Credits

LSFG and WFG are other people's work. The LSFG chain derives from **Camille LaVey**'s Vulkan port
in the Eden Emulator Project, which derives from **PancakeTAS**'s lsfg-vk; the WFG engine was
contributed by **qwertypower** of DEVAR Entertainment LLC. SnapFrame is the independent engine
integrated from the [Snap_AI_Engine project](https://github.com/Snap888/Snap_AI_Engine). See
[CREDITS.md](../CREDITS.md) for the full attribution, including source-file origins.
