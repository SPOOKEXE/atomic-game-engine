# source particle 2d

source pin: `b69eca232217360cf1502ef0223523d818606652`.

grug followed `scripts/node_particle/node_particle.gml`, `scripts/__VFX/__VFX.gml`,
`scripts/angle_functions/angle_functions.gml`, `scripts/area_function/area_function.gml`,
`scripts/random_function/random_function.gml`, `scripts/curve_bezier_function/curve_bezier_function.gml`
and `shaders/sh_sample_points/sh_sample_points.fsh`.

the native emitter owns its bounded pool through one versioned `DataReplay`
receipt per processor row. the receipt carries slot transforms, velocities,
lifespans, draw state, histories, copied sprite pixels, gradients, paths, curve
maps, wiggle maps, circular pool counters and the last rendered surface. no
private persistent cache owns a second copy. decoding rejects missing, repeated,
extra or malformed fields before publishing state.

spawn supports stream, burst and trigger schedules. each event resets the
html5 random profile from the retained seed, then adds 1000 to that seed.
newborn particles move before first rendering. source lifespan postdecrement
is retained: the slot survives the step starting with zero life. empty sprite
arrays suppress spawning. scalar noone sprites use the native circle fallback.
sprite arrays retain source random, order, animation and scale selectors.
surface atlas rectangles affect the spawn location while their owned surface
pixels remain available for drawing.

the first loop prerenders the selected tail of the timeline without drawing,
then restores the spawn seed and steps frame zero. subsequent owned frames
must be consecutive. curve maps use the total timeline frame count as their
precision. fast curve lookup selects a discrete map entry. native source
requests preserve the distinction between raw position/rotation and the
wiggled, path-adjusted draw transform.

native raster follows transformed sprite corners, position rounding, animation
end behavior, lifespan tint and alpha, blend selectors, wraps, y sorting and
retained trail histories. equal-y sorting uses source slot identity as its
native tie rule. distribution-map sampling uses a bounded float translation
of the pinned eight-attempt shader with byte-quantized output. these are native
profiles, not proof of source gpu coverage. strict source gpu coverage requests
require separate observations.

poisson uses a bounded deterministic native random profile. the pinned linux
extension uses libc random state and a different angle convention from its
gml fallback. the native profile does not claim either platform's exact point
cloud. its grid and trials are bounded before allocation; output truncation
follows the full admitted search.

`boundary_data`, `atlas` and `reset_seed` inputs are declared but never read by
the pinned emitter. they are preserved without invented behavior. dynamic
sprite custom parameters need an owned supported drawing payload; unsupported
objects must produce an explicit diagnostic.

verification is pending the joined build and runtime batch. this document does
not claim source gpu parity or a completed image composer.
