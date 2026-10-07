# Noise fields

`value.noise_field` uses one analytic smooth value-noise recipe for Generator and Computed modes. three authored dropdowns select its ports:

| choice | values | port effect |
| --- | --- | --- |
| mode | Generator, Computed | `field` output or ordinary `value` output |
| coordinate dimension | 1D, 2D, 3D | Scalar, Vector2, or Vector3 `position` input in Computed mode |
| output type | Scalar, Vector2, Vector3 | one, two, or three component samples |

a 2D generator returning Vector3 takes two coordinates and returns three components. coordinate count and component count stay distinct. dimension zero is an ordinary constant value node.

Generator owns a recipe without evaluating pixels. Computed samples that same recipe at its position input; absent positions sample the origin. `value.sample_noise` remains useful when several consumers need different coordinates and uses the same sampler.

Sample Noise has its own output type dropdown. its field input accepts the three coordinate dimensions with that exact result shape. its position accepts Scalar, Vector2, or Vector3; coordinate count must match the selected field at evaluation.

native vector noise uses stable channel-specific seed mixing. its first component equals the scalar result; extra components use distinct seeds. this is independent vector noise, not a gradient, normal, or curl field.

the field signature carries coordinate dimension and result shape through durable type names. compiler and Studio use the same instance port resolver. union ports list exact accepted signatures; scalar fields cannot silently satisfy vector-field inputs.

dropdowns are static authored settings, never linked or animated controls. changing a choice retains incompatible links with named diagnostics. dragging darkens incompatible sockets and lists compatible port ids using the canvas compatibility check.

Studio dimension changes resize an authored position, preserving retained axes and filling new axes with zero. linked coordinates and animation keys remain authored and report mismatches when their shape disagrees. saved recipes without the new settings keep Generator mode, 2D coordinates, and Scalar output.

existing image noise generators keep their Image output and a raster-backed 2D Field output. their output type dropdown selects Scalar from red, Vector2 from RG, or Vector3 from RGB. coordinates remain normalized 2D UVs, sampled at clamped nearest pixels. this dropdown changes the field result shape; Generator/Computed modes and 1D/2D/3D coordinate choices belong to the native analytic node.

raster fields own the generated pixels. old saves default to Scalar/red. source instances inherit the field output type until explicitly overridden, and Studio displays the effective choice. storage is allocated only when requested or linked; processor arrays carry the selected field signature, while Sample Noise requires a single field.

seed, frequency, octaves, and gain accept numeric controls; integer controls reject fractional values. recipes allow 1 to 16 octaves, frequency greater than zero through 8192, and gain from zero through one. coordinate bounds apply after frequency and octave scaling.

raster storage and graph results use the existing live byte budget. sampling checks layout and the selected pixel; full raster finiteness validation happens at payload validation. native recipes are value noise, with no claim of licensed source Simplex pixel parity.
