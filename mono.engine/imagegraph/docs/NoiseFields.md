# Noise fields

`value.noise_field` owns a smooth scalar value-noise recipe. its dimension dropdown chooses one, two, or three coordinate axes. dimension does not change the scalar returned by a sample. seed, frequency, octaves, and gain accept numeric controls; integer controls reject fractional values.

`value.sample_noise` takes a field and a scalar, Vector2, or Vector3 coordinate. coordinate count must match the field dimension. missing coordinates sample the origin. computed output is an ordinary scalar, so existing number consumers use it directly. field and coordinate sockets use explicit accepted-type unions. mismatches produce node and port diagnostics.

existing image noise generators keep their image output and expose an additional 2D field. this field owns the generated raster. sampling uses normalized UV coordinates, clamped nearest pixels, and the red component. image output remains unchanged. field storage is allocated only when the field is requested or linked. a source processor array produces an array of fields; the scalar sampler requires a single field.

recipes allow 1 to 16 octaves, frequency greater than zero through 8192, and gain from zero through one. coordinate bounds apply after frequency and octave scaling. raster storage and graph results use the existing live byte budget. sampling checks layout and the selected pixel; full raster finiteness validation happens at payload validation.

native recipes are value noise. grug makes no claim they reproduce licensed source Simplex pixels. source image fields sample the existing image algorithm's output.

## proposed generator and computed controls

grug keep three separate authored choices. this section is a design contract; vector fields and the combined mode dropdown are not implemented yet.

| choice | values | port effect |
| --- | --- | --- |
| mode | Generator, Computed | field output or ordinary value output |
| coordinate dimension | 1D, 2D, 3D | scalar, Vector2, or Vector3 position input |
| output type | Scalar, Vector2, Vector3 | scalar or independent component samples |

a 2D generator returning Vector3 is a field sampled with two coordinates and returning three components. coordinate count and component count stay distinct. dimension zero is an ordinary constant value node, so it needs no special noise field type.

Generator returns an owned recipe without evaluating pixels. Computed samples that same recipe at its position input. an explicit Sample Noise node remains useful when several consumers need different coordinates; both paths must use the same sampling function and return equal values for equal controls and coordinates.

native vector noise uses stable channel-specific seed mixing. its first component equals the scalar result; extra components use distinct seeds rather than copying the first component. this is independent vector noise, not a gradient, normal, or curl field; those need explicitly named operations with their own mathematical contracts.

the field signature carries coordinate dimension and result shape. compiler compatibility checks both; a scalar field cannot silently satisfy a vector-field input. extend the existing noise payload and port-signature data rather than adding a separate evaluator for each of the nine combinations. union ports list the exact field signatures or ordinary value types they accept; broad scalar/vector broadcasting requires an explicit conversion node.

these dropdowns are authored settings, not linked or animated controls. resolve the resulting ports once before compiling and use that same signature in Studio, validation, and evaluation. changing a choice retains incompatible links with a named diagnostic so the author can repair them; dragging darkens incompatible sockets and lists compatible port ids using the same compatibility check.

saved scalar recipes retain their current defaults and identifiers. existing image generators retain Image and scalar 2D Field outputs; vector recipes belong to the native generator until a source node has a defined vector contract. do not reinterpret source RGB as independent vector noise.

before shipping these controls, verify all coordinate/result combinations, channel determinism, Generator plus Sample versus Computed parity, incompatible links after dropdown changes, unions, serialization defaults, and byte/work budgets. the current scalar behavior above remains the implemented contract until those checks pass.
