# Wet metal preview

Load the corridor save in Mars City Underground. The recent log places the camera near (934, -1451, 65), east of the Convergence Chamber 2 door.

Use `r_wetMaterialPreview 1` for the wet treatment, `0` for original, or `0.5` for a milder blend. This non-archived preview defaults to off on restart. It does not edit map entities, saves, materials or texture mods.

Targets found in nearby map geometry:
- `textures/base_wall/lfwall16a`
- `textures/enpro/enwall12`
- `textures/base_floor/a_insettrim_d01a`

The preview affects every use of these three material names while enabled, not a spatially clipped patch. Existing specular texture luminance masks the treatment, keeping darker wear relatively dry. Full strength reduces roughness toward 40% of the original (minimum 0.12), and increases specular reflectance by up to 35%. Existing normal detail and diffuse color remain intact. Both legacy direct-light PBR conversion and environment-light PBR conversion receive the adjustment. This does not add a new reflection method or change the G-buffer roughness representation.

Compare with a flashlight sweep and sideways movement along the panels and floor trim. Check for overly sharp sparkle or plastic-looking surfaces. Build and shader compilation passed for DX12 and Vulkan; visual validation is left to the user.
