# Planned enhancements

- Bodycam render mode: deferred at the user's request. Start with lens distortion, vignette, edge softness, restrained grading and grain, plus an intensity control. Preserve HUD readability and HDR/DLSS compatibility. Camera motion would be a separate optional follow-up.

- Closed-door light leak: shelved at the user's request. Certain rotating lights shine through some closed doors; reportedly this previously affected more doors. Before changing the renderer, identify a reproducible level/door and distinguish actual illumination on nearby surfaces from a visible glow/flare. Investigate light shadow flags, closed-door area/portal culling, and shadow-map occlusion. Cause remains unconfirmed.
