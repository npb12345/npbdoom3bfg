# Bodycam hip-fire weapon position

Inside a weapon's `entityDef` in its `.def` file, add:

```
"bodycamHipOffset" "0 0 -1"
```

The three values are forward, left, and up in game units. Negative values
move backward, right, and down. The example lowers the weapon one unit.

The offset applies only with bodycam active and smoothly fades to zero when
aiming. It does not replace `zoomOffset` or change the normal camera's weapon
position. Omitted values default to `0 0 0`. The machine-gun definition includes
a zero-valued entry ready for adjustment; other weapons can use the same key.

Restart the game after editing to reload the definition. Visual positioning
should be checked in game, including the transition into and out of aiming.
