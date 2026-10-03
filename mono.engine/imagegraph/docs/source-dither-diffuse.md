# Error Diffuse Dither source CPU profile

`pc.dither_diffuse` follows pinned `Node_Dither_Diffuse`, source revision
`b69eca232217360cf1502ef0223523d818606652`. The GML SHA-256 is
`44e20dc65b1288f4a59f2e38f2c1182b27f8e794ae53a4b0c88ac1f82b74e5ed`.
The supporting `buffer_functions.gml` SHA-256 is
`f72271252e759125ead67b59e38ff0f5e8735ffd74a53ac04b299a138db0ca4f`.

The native profile accepts RGBA8 readback bytes. The source reads four bytes per
pixel from its unconverted surface buffer. Greyscale reads the red byte and
skips the other three. Each channel becomes 255 only when its accumulated signed
16-bit value is strictly greater than 128. Greyscale duplicates the resulting
red byte into RGB and forces alpha to 255. Colour mode diffuses alpha independently.
Traversal is left to right, then top to bottom. Each individual neighbor write
truncates toward zero and retains its low signed 16 bits.

Types use source indices 0 Floyd-Steinberg, 1 Jarvis, Judice, and Ninke,
2 Atkinson, and 3 Linear. Linear transports all error to the next pixel in the
same row. Atkinson uses six weights of 1/8. The source Jarvis kernel is asymmetric:
its first lower row has weights 3, 1, 7, 5, 3 over 48, and its second lower row has
weights 1, 3, 5, 3, 1 over 48. The implementation retains these exact coefficients
instead of replacing them with a textbook kernel. Out-of-image neighbors are
skipped. Seed is read by the source but never used.

Active false copies the input unchanged. Mask, invert mask, feather, mix, and
channel selection apply after diffusion through the common source processor
pipeline. Output storage is RGBA8 because this source node declares no surface
depth attribute. Complete processor-row work, including mask feathering, is
limited to 64 million operations before copying a disabled first row or allocating scratch. Row zero reads the original processor value arrays and image arrays to bound later dimensions, diffusion types, Colour/Greyscale mode, masks, and feathering. Independent extrema form a conservative whole-shape bound. Source Active rejects arrays; public processor rows retain scalar Active. Preflight also protects a supplied private context before copying a disabled selected row. Scratch is one
signed 16-bit value per pixel in Greyscale or four per pixel in Colour mode.
The reservation is resized to the vector's actual retained capacity before filling scratch. The shared evaluation ledger also accounts for output storage and retained
results before allocations; failed public evaluation preserves the prior result.

The public GameMaker HTML5 runner writes `buffer_s16` through JavaScript
`DataView.setInt16`. This establishes the truncating signed-16 conversion used
by this CPU profile. Supporting source was inspected at
[yyBuffer.js](https://github.com/YoYoGames/GameMaker-HTML5/blob/master/scripts/yyBuffer.js)
and its inspected bytes have SHA-256
`8e9ba50edbb1e2e3062e333d9e0365385a260fc1d1a6a9511dbbfc31bf6734b9`.
The [buffer_write manual](https://manual.gamemaker.io/beta/en/GameMaker_Language/GML_Reference/Buffers/buffer_write.htm)
defines the signed 16-bit storage range. Neither establishes a licensed desktop
runner oracle. Desktop readback, non-RGBA8 raw buffer layouts, and GPU raster
parity remain unverified; non-RGBA8 inputs receive `UnsupportedExecution` rather
than a fabricated conversion.
