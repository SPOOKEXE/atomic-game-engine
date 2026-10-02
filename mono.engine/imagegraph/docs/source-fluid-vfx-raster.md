# Source fluid and base-particle CPU raster

Pinned Pixel Composer source: `b69eca232217360cf1502ef0223523d818606652`.
GameMaker HTML5 geometry and random reference:
`60e51be51ce7f3d52025ef18106cf172b8e22a00`.

These routes define a bounded CPU reference. GPU fragment coverage and licensed
GameMaker desktop parity remain unverified. `RequireSourceGpuRasterCoverage`
refuses these routes explicitly before publishing a result.
FLIP random sampling uses `Request.Seed` as the native reset seed. The source
Render constructor's ambient seed is not recovered by this reference.

FLIP Render consumes owned solver readback and previous-frame history. Line mode
retains zero-slot gap bridging, lifetime clipping and the source velocity-gradient
calculation that compares an adjusted endpoint with a raw endpoint. Line alpha
ignores the Alpha input. Its geometry follows the HTML5 float32 line quad and
its offscreen Y offset. Fractional history indices receive an explicit refusal.

Particle mode accepts one Surface or a flat Surface array. Surface frames follow
particle-index modulo frame count. Every frame uses the first frame's dimensions
for its origin, including differently sized later frames. Plain sprite draws
retain the HTML5 signed32 vertex-alpha packing. Droplet shader alpha remains a
separate source operation. Captured-frame refresh rebuilds pixels from the exact
retained solver snapshot without stepping or changing its history.

VFX Renderer consumes ordered dynamic blend-and-particle groups. Each input is
one flat pool or a flat array of pools. The current owned carrier represents
source `__particleObject`, including FLIP-to-VFX output. Its draw method ignores
Round Position and render type; the renderer itself filters inactive particles
except in Line mode. A particle without a Surface draws a point and ignores its
Alpha. The CPU point profile uses draw-global alpha one and floor coverage after
the HTML5 float32 position and offscreen Y offset. Surface particles use centered
rotated float32 quads, signed scales, the selected sampler and packed vertex alpha.
Normal, Alpha and Additive retain the source blend equations.

The nearest VFX Inline collection supplies output dimensions. The renderer's
Output Dimension control is ignored by the source update. Renderer-only member
collections support Loop because source reset excludes Renderer. Other looping
VFX members require owned pre-render simulation replay and receive an explicit
refusal until that state contract exists.

Advanced source `__part` spawners, effectors, sprite animation and trails require
additional owned state and captured constructor randomness. This base carrier
does not claim those methods or invent their seed, lifetime or history fields.

Particle traversal is bounded by `MaximumArrayElements`. Raster work is capped
at 16,777,216 samples, and output and temporary selection storage are admitted
against the evaluation byte budget before allocation. Published pixels retain
no pointers to particle pools or source surfaces.
