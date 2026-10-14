# Combat firing audio v2

Ten new mono PCM16 44.1 kHz WAV files in `base/sound/npb/combat_v2`.
The two player firing sound shaders reference these unique paths, so packed
generated samples for the original names cannot take precedence.

Machine gun: derives from the installed five custom Dane recordings. Adds
mid/high attack emphasis, low-frequency body, a short synthesized bass
transient and soft saturation. Original duration is retained.

Shotgun: derives from the five BFG `_reissue` firing samples extracted from
the local `_sound_pc.resources`. Decodes mono Microsoft ADPCM, mixes the
original attack with an 83% speed body, adds low-frequency impact and soft
saturation. Duration increases about 20%.

Both: remove subsonic energy, apply short edge fades and normalize peak to
-3 dBFS. Shader gain is 0 dB, with no unclamped boost. Spatial ranges are
unchanged. Mechanical layers and first-pass impact tuning remain in place.

Validation: all ten outputs reopen as mono PCM16/44100, contain finite samples,
and have zero clipped samples. Hashes, duration and RMS are recorded in
`combat-audio-v2-report.json`. This does not verify the in-game mix by ear;
the user performs that test. No executable rebuild required.

Reproduction: run `tools/audio/extract-shotgun-audio.py`, followed by
`tools/audio/make-combat-audio.py` from the repository root with NumPy available.
Source locations are the local game install and `build/audio-source`.
These are derivatives of the existing game/custom sounds, not newly licensed
standalone assets; source rights still apply to any future distribution.

To revert, restore `weapons.sndshd` from the timestamped
`build/combat-audio-v2-backup-*` directory to the game's `base/sound` folder.
The extra WAV files can remain unused.
