# Blend Depth

grug implement `pc.blend_depth` from pinned `Node_Blend_Depth` and `sh_blend_depth`. both outputs use first surface dimensions and RGBA8. source has no colour-depth attribute.

colour coordinates subtract anchor, rotate clockwise in UV space, divide scale, add anchor, then subtract position. reference position uses first dimensions for both surfaces. pixel position divides by those dimensions. independent anchors, signed scales and rotations apply to each colour and depth pair.

plain depth reads use nearest clamp, ignore Oversample, and average RGB times alpha. missing depth uses transformed Y without clamping. ranges interpolate before comparison. red surfaces keep sampled red-only RGB, so red depth divides by three.

the four depth comparisons select foreground and background. source multiplies every foreground channel by foreground alpha, including alpha itself. output depth always takes the smaller depth, including Greater modes. a missing or transparent second colour keeps the first colour and first depth.

`shader_set_surface` forces nearest texture-stage filtering after `shader_set_interpolation`. native sampler preserves this override. Bicubic, Lanczos and CleanEdge kernels share the first surface dimensions even when second dimensions differ. inherited interpolation and oversampling use the normal processor controls.

grug admit the whole original batch before allocation. conservative work includes both colour kernels, depth reads and blending under 64 million work units. outputs also use normal node and evaluation byte limits. malformed surfaces, nonfinite controls and undefined coordinates report named diagnostics. zero scale reports unsupported source behaviour. Atlas texture binding is unsupported by the pinned helper and refused by name.

missing first surface returns retained output in source. pure native evaluation has no retained node output and reports `surface_1` invalid instead. exact licensed raster parity remains unverified.

source pin: [b69eca232217360cf1502ef0223523d818606652](https://github.com/Ttanasart-pt/Pixel-Composer/tree/b69eca232217360cf1502ef0223523d818606652). inspected node script, shader fragment, shader binding helper and default depth allocation. native semantic checks live in `tests/SourceBlendDepth.cpp`.
