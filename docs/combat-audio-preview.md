# Combat audio first pass

Uses the installed `base/sound/weapons.sndshd` as its baseline, preserving the
custom machine-gun firing samples and unrelated sounds. No executable change.

- Machine gun: replace the duplicate gunshot on the mechanical channel with
  the four packaged BFG `_reissue` mechanism samples at 0 dB. Preserve the
  custom firing layer and its gain. Enable `no_dups` on both layers.
- Shotgun: pump rises from -6 to -2 dB; firing samples and gain stay unchanged.
- Bullet metal impacts: -10 to -5 dB; stone: 0 to 1 dB; flesh: 0 to 2 dB.
- Shotgun metal impacts: -5 to -2 dB; flesh: 0 to 2 dB.
- Enable `no_dups` on the adjusted multi-sample impact shaders.

Shared bullet impact definitions affect any weapon referencing them. No new
EQ, bass processing, reverb, room decay or audio assets are added in this pass.
The mechanical sample paths were verified in `_sound_pc.resources`.

Restart and compare short machine-gun bursts, sustained fire, shotgun shots
and pump, and metal/stone/flesh hits. Listen for impact balance and excessive
mechanical clicking. In-game listening is left to the user.

Installation keeps a timestamped backup under `build/combat-audio-backup-*`.
Restore its `weapons.sndshd` to the game's `base/sound` to revert this pass.
