# Draw Line 2 Points source-state audit

Source revision: `b69eca232217360cf1502ef0223523d818606652`.
Archive: `/home/declan/Documents/GitHub/atomic-game-engine-hidden-docs/files/pixel-composer-com/official-source-b69eca232217360cf1502ef0223523d818606652.tar.gz`.
Archive member prefix: `Pixel-Composer-b69eca232217360cf1502ef0223523d818606652/`.
Local script root: `/tmp/pixel-composer-source-b69eca232217360cf1502ef0223523d818606652/`.
The shader was read directly from archive member `shaders/sh_draw_line_width/sh_draw_line_width.fsh`. This audit establishes source control flow, not licensed pixel parity.

## Established source facts

- `scripts/node_line_2points/node_line_2points.gml:133-134` selects `sh_draw_line_width` for thick mode, then draws Background Surface immediately. Current UV Position/Scale and cap flags are uploaded only at lines 137-141. Current per-line thickness and length are uploaded at lines 163-164. Thus an RGBA background can execute the line shader before this invocation uploads those uniforms.
- `scripts/surface_functions/surface_functions.gml:41-55` unwraps SurfaceAtlas, delegates dynaSurf drawing, ignores arrays/missing surfaces, then calls `__channel_pre`, draws the surface, and calls `__channel_pos`.
- `scripts/surface_draw_functions/surface_draw_functions.gml:5-7` selects red-channel shaders for R8 unorm, R16 float and R32 float surfaces. Lines 15-18 reset the shader afterward. This helper does not restore `sh_draw_line_width`. Subsequent thick-line code contains no shader reselection. Do not silently restore it in a source-faithful executor.
- `scripts/shader_functions/shader_functions.gml:279-306` sets targets, clears by default, chooses blend state, and selects the requested shader. It contains no sampler or line-uniform initialization. Lines 309-319 reset interpolation to zero, normal blending, target and filtering, then reset the shader when applicable. They do not reset line caps, thickness, length, UV transforms, sampleMode or useUvMap.
- `scripts/shader_functions/shader_functions.gml:244-253` makes its texture-filter override a no-op on Linux/macOS and calls the underlying setter on Windows. A source platform profile must account for this difference.
- The reviewed script tree contains only one reference binding `sh_draw_line_width`: this node. There is no `shader_set_interpolation` or preset call in its update. Shader `sampleTexture` still reads useUvMap, interpolation, sampleDimension and sampleMode. In-range UVs enter texture2Dintp; out-of-range UVs consume sampleMode.
- `scripts/node_line_2points/node_line_2points.gml:159-160` evaluates a random base-gradient position and random width per line, including 1px mode. Source seed resets at line 106. Native draw observations must preserve this call order; equal-width random_range draw consumption requires the existing capture contract, not an invented seed stream.
- `scripts/draw_line_width2/draw_line_width2.gml:34-60` emits two tapered triangles with endpoint colors and local UVs 0..1. Its cap argument is unused. Shader main clips caps using lineThickness and lineLength, while each segment resets the local UV coordinates. The node supports three point modes and cyclic pairing of depth-two point arrays, lines 110-129.

## Complete bounded executor seam

Implement all 22 physical inputs, three point modes, point-array pairing, segmentation, 1px and thick geometry, host width curves and gradient evaluation, texture transforms, background drawing, caps, three blends and seven typed output formats. Prepare recipes for every selected row before output allocation or observers. Admit point/segment storage and conservative geometry/raster/curve/gradient work across the entire batch. Refusal preserves prior public results.

Use existing `SourceBuiltinRandomCapture` validation for actual ordered draws. Add a bounded invocation-state receipt for shader and sampler state at entry, including prior UV/cap/thickness/length uniforms, sampler flags/dimensions and filtering. Identify receipts by node, selected row, time and resolved controls, while recording actual shared shader state. A per-node fabricated history is insufficient because nodes use the same shader object.

Replay the background stage before current uniform uploads and preserve the single-channel background shader reset. Reuse existing triangle cross/top-left and bounded bounding-box code where compatible, but extend vertex-color interpolation and source blend equations rather than substituting the VFX blend modes. Declare the native 1px and triangle raster convention explicitly. Missing required state receipts get named diagnostics; this is a coverage boundary, not permission to implement only the no-background branch.

## Missing observations and comparison gate

Local source establishes no linked-program initial uniform values or initial texture filtering/binding state. Required observations include first-use shader initialization, previous invocation uniform state, no-texture surface_get_texture binding behavior, current shader after single-channel background and effects of subsequent uniform uploads, platform filtering, and actual primitive coverage/vertex interpolation/storage rounding. Capture source RNG draw sequences independently as well.

A deterministic fresh-state CPU profile could implement all controls with explicit raster and sampling conventions, but those choices must be described as native conventions. They cannot be presented as proven source initial state. Exact source renderer parity requires a licensed build and matched captures. This audit does not recommend a partial executor or declare the catalogue complete.

This audit did not execute the licensed renderer.
