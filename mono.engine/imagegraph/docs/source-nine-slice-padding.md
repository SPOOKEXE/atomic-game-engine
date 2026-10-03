# Nine Slice and Padding native source profile

Pinned source b69eca232217360cf1502ef0223523d818606652:

| Source | SHA-256 |
| --- | --- |
| node_9slice.gml | e1bbbba7517cd7500cf4c752823db0c4d90760ae8bfdc9feb4c1e342fd424821 |
| node_padding.gml | 3e7005db946bab3392876649c85c8cca60cef066c7c64924d189a869e3920b35 |

Nine Slice preserves an owned original image. Dynamic width/height report original dimensions. The native dynamic draw uses its recorded CPU sampler profile. Licensed dynamic draws inherit ambient caller shader/filter state, which is not claimed captured. Each draw reconstructs fixed corners and scales or repeats the center. Repeated edge/center tiles clip their final extent. Splices use right, top, left, bottom order, Reference width/height conversion and half-even integer rounding. Central source spans use max(original minus splices,1); splice sums are never normalized to fit.

Internal Nine Slice staging is RGBA8 with override blending. Tint and alpha apply before staging quantization. The constructor retains original input sample dimensions during its outer sh_sample pass. The part wrapper ignores rotation; the final draw rotates once and changes blend to Normal. Normal applies source alpha to all four channels. PCX retains that blend for subsequent draws in the same execution. Pixel Builder draws the dynamic staging with the same Normal transition.

Padding Empty copies with override blending. Solid draws over fill colour using the native initial Normal blend profile. The Padding Vec2 getter returns linked surface dimensions before Reference conversion. Inactive dynamic input retains its owned recipe. Pad to size alignment uses requested floating dimensions before allocation rounding. Pad out dimensions at most one preserve the source's previous cached result, requiring an explicit observation in the stateless evaluator.

Pixel Expand uses sh_atlas without uploading resolution. Offline GLSL reflection confirms resolution is active. Fully opaque RGBA8 staging returns before reading it; transparent staged pixels require unobserved shader state and refuse execution. No replacement resolution or flood-fill algorithm is invented.

The CPU profile uses pixel-center rectangular coverage, truncation for translated Padding draws and the existing source texture sampler. Signed Nine Slice part geometry and requested exact GPU coverage require licensed renderer observations. Floating-point GPU/desktop differences remain an external parity gate.

Generated recipes are runtime values. Authored graph serialization stores source controls/links and evaluation regenerates recipes. Deep cloning and retained-capacity accounting include original image bytes; no provider or borrowed view survives. Allocation and whole-batch work admission preserve previous caller results on refusal.

Nine Slice tests raw surface existence before initialization. Struct-backed Atlas/dynamic inputs therefore require observed previous outputs on both ports. The native path refuses them rather than flattening them into replacement raw images.
