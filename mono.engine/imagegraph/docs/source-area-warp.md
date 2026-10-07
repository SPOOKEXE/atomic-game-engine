# source area warp

grug native `pc.wrap_area` draws an ordinary input surface into its typed Area rectangle. pixel units use the rectangle directly; Reference units scale its first four fields by input width and height. authored padding and two-point modes normalize before scaling. linked Areas retain producer normalization; shape does not affect this source node.

grug clear outside the rectangle, retain mirrored extents, and copy inactive images without drawing. seven numeric surface formats work. red-only inputs use the source red-channel draw rule: replicate red into RGB with opaque alpha before destination encoding. ordinary RGBA inputs use shared source sampling, including CleanEdge and inherited group settings.

grug admit the largest original image and greatest original sampler cost across every processor row before allocation. the native cap is 64 million work units for the complete batch. malformed Areas, nonfinite rectangles and out-of-range samples give named diagnostics. homogeneous Area and Enum control arrays retain flat and nested payloads through version 9 document persistence, with existing leaf and byte checks.

grug still refuse Atlas input, including inactive input, because the processor must preserve its owned transform and bypass payload first. nonzero-mode linked Area bypass getters also require originating surface context. sampler arrays refuse unresolved source array depth; linked scalar sampler controls work. these are explicit native limits, not source parity claims.

grug source contract comes from pinned [Area Warp](https://github.com/Ttanasart-pt/Pixel-Composer/blob/b69eca232217360cf1502ef0223523d818606652/scripts/node_wrap_area/node_wrap_area.gml), Area getter and unit rules, surface draw helpers and `sh_sample`. reviewed local pinned archive, not a licensed executable. [MIT notice](../../../docs/pixel-composer-m0/PixelComposer-LICENSE.txt) covers source-derived work.

validation receipt: [native area warp](../../../docs/pixel-composer-m0/native-area-warp-validation-2026-10-07.json). grug check actual FrameGraph scope; no benchmark or speed claim. licensed captures, exact GPU edge parity and headed Studio remain unverified.
