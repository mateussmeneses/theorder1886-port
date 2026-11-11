# The Order: 1886 PC Port - Windows v0.8.3

A one-fix release on top of v0.8.2 for the stutters that come back every time the game is
started again (#113). Tested on one PC (RTX, 32 GB) with the CUSA00035 01.02 dump on the
prologue route; no open issue is claimed resolved before the reporters' retest.

## Fix

**Far fewer shader and pipeline compiles after the first session (#113).** Four vertex
shaders of the game's billboard particles read up to 16 pairs of lighting textures, but only
the first few (7 in the prologue) are live; the rest of the table holds leftovers from earlier
frames. Every new combination of those leftovers became a new shader variant (up to 126 per
shader) and a new pipeline, each a ~160 ms compile in the driver, in every session. The port
already carried a fix adapted from TOport that clears the unused part of that table, but it never ran here:
it expected one more buffer than this port's shader compiler declares. It now runs.

Measured on the same route, three sessions in a row from an empty cache (one run per build):

| | v0.8.2 | v0.8.3 |
|---|---|---|
| 5-second windows with a frame over 100 ms (sessions 1 / 2 / 3) | 15 / 7 / 7 | 3 / 1 / 1 |
| Driver pipeline compiles after the first session | 38-39 | 0 |
| Shader variants stored | 821 | 472 |
| Pipelines stored | 638 | 341 |

The prologue renders normally in our captures, but the frames were not compared pixel for
pixel, and other chapters were not measured.
`THEORDER_PARTICLE_SRT_FIX=0` restores the old behaviour.

**Updating from an earlier version:** the extra variants already in your shader cache stay
there and are harmless; new sessions stop adding them. Other scenes and chapters were not
measured.

## Known open items

Unchanged from v0.8.2: flickering objects and decals, first-visit stutters in areas not
visited before, and missing grenade smoke in The Atrium.
