# source area warp

grug native `pc.wrap_area` draws an ordinary surface into its typed Area rectangle. pixel units use the rectangle directly; Reference units scale its first four fields by input width and height.

grug normalize authored padding and two-point modes before scaling. linked Areas retain producer normalization. shape does not affect this source node.

grug clear outside the rectangle and retain mirrored extents. inactive images copy without drawing. all seven numeric surface formats work.

grug red-only drawing replicates red into RGB with opaque alpha before destination encoding. ordinary RGBA drawing uses shared source sampling, including CleanEdge and inherited group settings.

grug admit the largest original image and greatest original sampler cost across all processor rows before allocation. the complete batch cap is 64 million work units. malformed Areas, nonfinite rectangles and out-of-range samples give named diagnostics.

grug homogeneous Area and Enum controls retain flat and nested payloads through version 9 persistence. existing leaf and byte checks still apply.

grug active SurfaceAtlas uses Area top-left plus Atlas position, scale, rotation, RGB tint and alpha. source ignores Area width and height when drawing this wrapper. actual backing dimensions control output; stored dimensions and original surface do not.

grug inactive SurfaceAtlas unwraps and copies backing pixels without its draw fields. base Atlas refuses because source `is_surface` excludes it. array row conversion is enabled for Area Warp's verified receiver only.

grug nonzero-mode linked Area bypass getters still require originating surface context. sampler arrays refuse unresolved source array depth; linked scalar sampler controls work. these are explicit native limits.

grug read pinned [Area Warp](https://github.com/Ttanasart-pt/Pixel-Composer/blob/b69eca232217360cf1502ef0223523d818606652/scripts/node_wrap_area/node_wrap_area.gml), Area getter and unit rules, surface draw helpers and `sh_sample`. source came from the local pinned archive. [MIT notice](../../../docs/pixel-composer-m0/PixelComposer-LICENSE.txt) covers source-derived work.

Atlas receipt: [native area warp Atlas](../../../docs/pixel-composer-m0/native-area-warp-atlas-validation-2026-10-07.json). ordinary-surface receipt: [native area warp](../../../docs/pixel-composer-m0/native-area-warp-validation-2026-10-07.json).

grug check actual FrameGraph scope; no benchmark or speed claim. licensed captures, exact GPU edge parity and headed Studio remain unverified.
