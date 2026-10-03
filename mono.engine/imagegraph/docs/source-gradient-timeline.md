# Persisted Gradient animation

This native CPU profile follows Pixel Composer commit
`b69eca232217360cf1502ef0223523d818606652`, specifically
`gradients_function.gml` `lerpTo`/`eval`, `node_value_gradient.gml`
`lerpAnimKeys`, `node_keyframe.gml` `getValue`, and `color_function.gml`
`merge_color_ext`/`merge_color_rgba`.

Persisted Gradient keys deserialize their packed colours to int64. The native
Colour carrier follows that representation, including RGBA interpolation in the
single-key branch. Runtime Gradient keys stored as GML real values may take the
RGB-only `merge_color_ext` branch and need an explicit observation; no licensed
runtime parity is claimed for that separate representation.

The result keeps the left Gradient mode. Its key count is the ceiling of the
interpolated input counts. A nonpositive count produces an empty Gradient. A
single key is sampled at zero; otherwise the source pairs keys by truncating
normalized array indices, interpolates their times, samples both input Gradients
at that time, and merges their RGBA bytes. The shared sampler retains all seven
source modes, byte rounding and the pinned Oklab/CMYK alpha behavior.

The native cap is 128 keys. Counts and timeline workspace are admitted before
allocation. Nonfinite count/time/colour calculations refuse atomically.
Multi-key results with an empty input refuse because the source indexes that
empty array. Finite overshoot is retained when its result fits these limits.

Source cuts select their raw key before invoking a driver. Bounce, elastic and
curve drivers return a raw key when there is no next interval. Active numeric
struct operations in drivers remain unsupported for Gradient values. Matrix
objects inherit numeric struct interpolation rather than a Matrix lerp method;
interior source Matrix intervals remain unsupported while raw endpoints work.
