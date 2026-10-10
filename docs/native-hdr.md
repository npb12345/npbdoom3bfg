# Native HDR10 output

Settings > System > **Native HDR** enables HDR10 on the DX12 renderer.
Enable Windows HDR first and restart the game after changing this setting.
The game queries the output containing its window. If Windows HDR or the
HDR10 swap-chain color space is unavailable, it renders SDR instead.
HDR is off by default for existing installations.

Deploy the executable **and the matching `base/renderprogs2` shaders** together.
Copying only the executable leaves the old SDR output shader running against
an HDR10 swap chain, producing incorrect brightness and oversaturated colors.
HDR changes `builtin/blit.ps.bin`, `builtin/rect.vs.bin`, and
`builtin/post/{exposure.cs,histogram.cs,tonemapping.ps}.bin` in both the DXIL
and SPIR-V directories. Fully restart the game after replacing shaders.

## Brightness controls

- **HDR Peak Brightness:** Auto reads the display's reported peak luminance.
  A manual setting overrides it (400–4000 nits). Windows reported 603 nits
  peak and 276 nits full-frame on the development display. This is the
  current display report, not a claim about every LG 45-inch model.
- **HDR Paper White:** 80–400 nits; default 200. Controls normal whites and
  HUD/menu brightness. Both brightness controls apply immediately.
- The existing Brightness control still adjusts scene exposure.

Console equivalents: `r_hdrOutput 1`, `r_hdrPeakNits 0` (Auto),
`r_hdrPaperWhiteNits 200`. `gfxInfo` reports the active output and brightness.
Restart after moving to a different display or changing Windows HDR.

## Rendering path

The scene remains FP16 through DLAA/DLSS and tone mapping. The HDR curve
retains the existing SDR color balance (the color LUT when provided, otherwise
the ACES-style curve) and adds a bounded luminance shoulder using scene values
that SDR would clip. A single gain is applied to all channels so warm lighting
does not acquire an exaggerated red cast. Peak limiting also uses a single
gain rather than clipping channels independently.

The scene contrast curve pivots around 18% gray with an exponent of 1.18,
deepening shadows without a hard black cutoff. The highlight shoulder begins
at 0.6 in the exposed scene signal and rises toward the configured peak.
These adjustments happen before HUD composition, so they do not raise menu
or HUD white above the paper-white setting.

Tone-mapped scene, SMAA intermediates and display composition use FP16.
Composition retains the legacy extended gamma-2.2 working space so existing
GUI materials and blending keep their established behavior. This is not
linear-light UI blending. Final output decodes that working space, scales
by paper white, converts Rec.709 primaries to BT.2020, then encodes absolute
luminance with ST 2084 PQ into an R10G10B10A2 swap chain. DXGI explicitly
uses `DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020`.

DLSS Frame Generation receives a separately captured HUD-less image in that
same RGB10/PQ encoding. The final real frame is encoded after HUD composition.
This is native scene HDR, rather than expansion of an already clipped SDR image.

SDR filmic and CRT filters are bypassed during native HDR. Retro render modes
retain their SDR palettes, displayed at paper-white brightness. Authored SDR
videos and menus also stay within paper white. Legacy special-effect capture
textures can still clip highlights during effects such as screen wipes.
The original SDR rendering path remains available by turning Native HDR off.

The first implementation targets Windows DX12. Vulkan remains SDR. HDR
screenshots/export and dynamic display/HDR-mode switching are not implemented.

## Validation on the development PC

- Release executable and DXIL/SPIR-V shaders build successfully.
- Native RGB10/PQ output confirmed at 3440x1440 with DLAA and DLSS Quality
  and Performance, alongside 2x Frame Generation.
- NVRHI validation enabled during the HDR regression run; no reported
  validation errors. Windowed 1920x1080 and return to 3440x1440 work.
- Peak/paper-white changes apply live and return to Auto/200 nits.
- Turning HDR off restores SDR output; DLAA and 2x Frame Generation still
  initialize and the SDR gameplay run exits normally.
- A queued-view size guard prevents DLSS evaluating stale dimensions during
  resolution/quality changes. The final regression has no Streamline errors.
- PQ encoding matches reference points at 0, 100, 1000 and 10000 nits;
  the highlight curve is monotonic and bounded across the control range.
- Visual brightness and highlight appearance still need user assessment
  on the HDR display; ordinary SDR screenshots cannot validate HDR luminance.

An earlier `listImages` diagnostic stalled and exited abnormally while
printing thousands of textures. It did confirm the LDR-named scene target
uses RGBA16F in HDR. The gameplay/resize regression without that diagnostic
completed normally. Test logs are in the ignored `build/dlss-test` directory.

## References

- [Microsoft HDR display output guidance](https://learn.microsoft.com/en-us/windows/win32/direct3darticles/high-dynamic-range)
- [Microsoft D3D12 HDR sample](https://github.com/microsoft/DirectX-Graphics-Samples/tree/master/Samples/Desktop/D3D12HDR)
- NVIDIA Streamline SDK 2.14.1, `docs/ProgrammingGuideDLSS_G.md`, HDR section.
