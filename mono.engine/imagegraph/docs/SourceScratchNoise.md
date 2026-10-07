# Source Scratch Noise

`pc.noise_scratch` follows the pinned Pixel Composer constructor and shader at
commit `b69eca232217360cf1502ef0223523d818606652`. Numeric fixtures use
independent binary32 references and do not claim parity with a particular GPU
or runtime.

## Inputs and shader behavior

The 18 physical inputs are `dimension`, `uv_map`, `uv_mix`, `mask`, `seed`,
`thickness`, `wavyness`, `softness`, `octaves`, `octave_scale`, `position`,
`rotation`, `scale`, `octave_shift`, `octave_rotation`, `thickness_map`,
`wavyness_map`, and `softness_map`, in source order. The three noise controls
can each use a surface map. Mapped controls use the mean of map RGB; map alpha
does not affect their values.

The shader samples UV-map alpha even when UV Mix is zero. It flips the sampled
Y coordinate before blending the sampled UV with the original UV, then
subtracts normalized Position, rotates, and divides by Scale. UV and control
maps use nearest sampling. Each octave evaluates a hash-based scratch pattern
and keeps the maximum. Before the loop, `fwidth` determines the smoothstep
width. The CPU model evaluates an aligned 2-by-2 quad, including helper lanes
outside the output canvas, so edge derivatives use those lane positions. Each
iteration rotates and shifts the coordinate, then scales that width for the
next octave.

## Bounded CPU evaluation

Covered pixels with positive Octaves require the mapped flags for Thickness,
Wavyness, and Softness because the generic source upload leaves their
two-component shader uniforms undefined when those flags are off. A consumed
Seed must be finite. Octaves uses source half-even rounding and a signed
32-bit bound. Zero Scale and equal smoothstep edges are refused rather than
assigned fallback pixels. A nonpositive octave count skips unused noise
controls and the Seed; UV-map alpha is still retained.

The evaluator preserves the requested output format and uses the shared mask,
channel, and mix processing. Raw Atlas sampler bindings are refused. The
shader passes descending edges to `smoothstep`. The CPU model keeps that
source expression, but GLSL does not specify a portable result for the edge
order. Hash and trigonometric results, `fwidth`, texture sampling, and raster
coverage can also vary by backend. The fixtures test the bounded CPU source
model, not device parity.

For a batch with covered pixels, admission quotes
`512 + 4096 + 8192 * max(octaves, 0)` work units for every allocated pixel.
A fully uncovered batch uses 512 units per allocated pixel. The cumulative
limit is 64 million units. The selected batch is admitted before outputs are
created or observers are called.
