# Native DLSS (DX12)

The Windows DX12 renderer supports native DLAA and DLSS Super Resolution through
NVIDIA Streamline 2.14.1. Select a mode under Settings > System > Anti-Aliasing.
The existing menu saves the selection and requests a restart when needed.

| Console setting | Mode |
| --- | --- |
| `r_antiAliasing 2` | Existing TAA |
| `r_antiAliasing 3` | DLAA |
| `r_antiAliasing 4` | DLSS Quality |
| `r_antiAliasing 5` | DLSS Balanced |
| `r_antiAliasing 6` | DLSS Performance |

These values apply to the default `ID_MSAA=0` build. Render sizes are queried from
the SDK for the actual output resolution; DLAA uses native resolution. The pass
uses HDR scene color, depth, motion vectors, and jitter before tone mapping and
the screen HUD. DLSS modes take precedence over the legacy resolution scaler.
The scene copy used by heat haze expands the active render rectangle to fill
the sampled texture, keeping steam/refraction coordinates aligned when upscaling.

Missing runtime files, unsupported GPUs/drivers, and Vulkan use TAA. The menu
labels unavailable modes with `TAA fallback`. Initialization and evaluation
failures are reported in the console rather than terminating the game.

## Build

Download and extract the official [Streamline 2.14.1 SDK](https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.14.1), then configure:

```powershell
cmake -S neo -B build -DUSE_STREAMLINE=ON -DSTREAMLINE_SDK_DIR=C:/path/to/streamline-sdk
cmake --build build --config Release --target NPBDoom3BFG --parallel 8
```

`USE_STREAMLINE` defaults to OFF. No SDK is downloaded automatically and no NVIDIA
import library is statically linked. With it enabled, the build copies these
signed release runtime files beside the executable:

- `sl.interposer.dll`
- `sl.common.dll`
- `sl.dlss.dll`
- `nvngx_dlss.dll`
- `sl.dlss_g.dll`
- `nvngx_dlssg.dll`
- `sl.reflex.dll`
- `sl.pcl.dll`

Deploy the rebuilt `base/renderprogs2` shaders along with the executable and these
eight DLLs. Preserve existing game files and configs when testing a new build.
The Streamline interposer's NVIDIA signature is checked before loading it.
Keep the SDK's license and third-party notices with a distribution. Public
release still requires review of NVIDIA's binary redistribution terms alongside
this project's GPL licensing; optional dynamic loading alone does not resolve
that question.

## RHI / Neural Rendering

This is native DLSS Super Resolution/DLAA, not a native DLSS 5 Neural Rendering
implementation. RHI's Neural Rendering consumer can potentially hook the native
DLSS evaluation instead of the feeder. Test the consumer without the feeder in a
separate game copy, including its supported DLL/driver combination. Native DLSS
should be verified alone first. Do not include third-party injectors or modified
NVIDIA binaries in the port's release package.

## Frame Generation preview

Settings > System > DLSS Frame Generation offers Off / On (2x). It defaults to
Off; changing it requests a restart. Select DLAA or a DLSS quality mode first.
The console equivalent is `r_dlssFrameGeneration 1` (restart required).
The local launcher respects saved anti-aliasing and frame-generation choices.

The integration loads Streamline's production DLSS-G, Reflex, and PCL plugins,
enables Reflex Low Latency, and requests one generated frame per rendered frame.
It checks runtime hardware/driver support; hardware-accelerated GPU scheduling
must be enabled in Windows. Unsupported configurations retain normal DLSS/DLAA.
The SDK reference is NVIDIA's [DLSS-G integration guide](https://github.com/NVIDIA-RTX/Streamline/blob/v2.14.1/docs/ProgrammingGuideDLSS_G.md).

Simulation, render-submit, and present markers share the command buffer's frame
ID, including with parallel game/render execution. The swap chain is created
through Streamline's factory proxy so FG controls its back buffers. Frame
generation suspends during loading, pause, the console, menus, and frames without
valid DLSS inputs. It is disabled before resize and shutdown. A restart with the
setting Off unloads the FG plugin before swap-chain creation, avoiding its extra
presentation path during normal rendering.

Depth and motion vectors are tagged through Present. A separate copy of the
final scene before the first HUD draw is tagged as HUD-less color. A separate UI
alpha layer is not supplied yet; the inherited camera-only motion-vector limit
below also applies to generated frames. Test HUD edges, fast movement, effects,
and moving characters before treating this as release quality. RHI coexistence
has not been validated. Engine FPS counters still count rendered frames; they
do not include generated frames.

The initial RTX 4070 Ti Super smoke test at 3440x1440 verified
`2 presented frames per rendered frame`, all four DLSS modes, pause/resume, and
fallback to TAA. The process exited with code 0. The logs are saved as
`build/dlss-test/fg-first-run.log` and `fg-first-streamline.log`.

Serial rendering (`com_smp 0`) also generated frames before and after a
3440x1440-to-1920x1080 resize. The transition waits two real frames before
resuming interpolation. The SDK still logged a transient
`NvAPI_D3D12_SetAsyncFrameMarker` error while switching display modes, then
recovered and reported two presented frames again. This remains a preview issue
to investigate; restarting at the desired resolution avoids that transition.
See `build/dlss-test/fg-resize.log` and `fg-resize-streamline.log`.

The Off and missing-FG-runtime smoke tests both exited with code 0 and verified
native DLAA evaluation at 3440x1440. Their logs are `fg-off.log` and
`fg-missing-runtime.log` under `build/dlss-test`.

## Validation and current limits

The local preview launcher is `build/Launch Native DLSS.cmd`. It starts the DX12
build in borderless mode, defaults to DLAA, reads the installed game assets, and
uses `build/dlss-test` for its configuration and rebuilt shaders. It does not load
the installed game's RHI DLLs. The existing game executable is not replaced.

On the development RTX 4070 Ti Super, all four modes completed native evaluation
at 1280x720, ultrawide windowed resolution, and 3440x1440 borderless, including
switching back to TAA. At 3440x1440, Quality rendered at 2293x960, Balanced at
1995x835, and Performance at 1720x720; DLAA used native resolution. The borderless
test exited with code 0 and no Streamline errors.
A separate executable without the NVIDIA DLLs loaded the map with DLSS Quality
selected and exited normally through the TAA fallback. Build and smoke-test
logs are in `build/dlss-final-build.log` and `build/dlss-test`.

Look for `Native DLSS available (DX12)` and `DLSS: native evaluation succeeded` in
`qconsole.log`. `sl.log` beside the executable contains the SDK diagnostics.

Before release, test all four modes at 16:9 and ultrawide resolutions, menu/HUD
clarity, resize/fullscreen changes, load/save and camera cuts, missing-DLL
fallback, and injector coexistence. Compare GPU timings on the same saved scene.

The inherited motion-vector pass supplies camera reprojection and excludes the
weapon from camera motion. It does not yet supply previous-frame skeletal
deformation or full independent object motion. This first integration therefore
needs motion-quality testing and a full object-velocity pass before being treated
as release quality; moving enemies, weapon animations, and particles can ghost.
