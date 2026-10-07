# Noise fields

`value.noise_field` owns a smooth scalar value-noise recipe. its dimension dropdown chooses one, two, or three coordinate axes. dimension does not change the scalar returned by a sample. seed, frequency, octaves, and gain accept numeric controls; integer controls reject fractional values.

`value.sample_noise` takes a field and a scalar, Vector2, or Vector3 coordinate. coordinate count must match the field dimension. missing coordinates sample the origin. computed output is an ordinary scalar, so existing number consumers use it directly. field and coordinate sockets use explicit accepted-type unions. mismatches produce node and port diagnostics.

existing image noise generators keep their image output and expose an additional 2D field. this field owns the generated raster. sampling uses normalized UV coordinates, clamped nearest pixels, and the red component. image output remains unchanged. field storage is allocated only when the field is requested or linked. a source processor array produces an array of fields; the scalar sampler requires a single field.

recipes allow 1 to 16 octaves, frequency greater than zero through 8192, and gain from zero through one. coordinate bounds apply after frequency and octave scaling. raster storage and graph results use the existing live byte budget. sampling checks layout and the selected pixel; full raster finiteness validation happens at payload validation.

native recipes are value noise. grug makes no claim they reproduce licensed source Simplex pixels. source image fields sample the existing image algorithm's output.
