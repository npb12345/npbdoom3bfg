# Mars City Underground lighting preview

Load the existing save near the locker and Convergence Chamber 2 door. Open the console and enter `npbHazePreview 1`.

- `npbHazePreview 0`: remove the preview and return to the original lighting.
- `npbHazePreview 1`: subtle cool haze with two restrained fixture accents.
- `npbHazePreview 2`: the same lighting with denser haze for comparison.

The command is limited to mc_underground and replaces its own named entities when repeated. No original map entities are edited. The fog volume ends before the closed door; this does not address the shelved light leak. Added fixture lights cast shadows.

Existing renderer support consists of fog-light volumes, blend lights and particle effects. No active shadowed volumetric scattering pass was found. This prototype uses the existing fog-light renderer: haze is not dynamically lit or shadowed, and is not a true volumetric shaft. Its material disables portal-fog culling. It runs in the existing scene pipeline, without shader or DLSS changes.

Compare depth down the corridor, fixture readability and black levels with the flashlight off, then with it on, in normal and bodycam views. Gameplay verification is left to the user. If fog lights have manually been disabled, `r_skipFogLights 0` is required.

Preview entities are ordinary runtime lights and can be included in a new save. Use `npbHazePreview 0` before saving to keep the save free of them; loading the original save also resets the test.
