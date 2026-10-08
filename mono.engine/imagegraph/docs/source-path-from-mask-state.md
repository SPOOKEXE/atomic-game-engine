# Source path from mask state audit

This audit records observed behavior from pinned Pixel Composer artifacts. It does not establish native parity, native refusal rules, or executor behavior.

## Pinned inputs

- GML source: commit `b69eca232217360cf1502ef0223523d818606652`; `scripts/node_path_from_mask/node_path_from_mask.gml`, raw SHA-256 `15509e828c26b1a750f4c1c6a63ebcc5bb19440020d56fa34756dd50a9882fc8`
- FSH source: `shaders/sh_image_trace/sh_image_trace.fsh`, raw SHA-256 `e80f7f2f08bc7eebf057baeb3c6516de4042df1e782d23f507c437749872d0f2`
- Linux shared object: `extensions/inlineC/path_from_mask.so`, raw SHA-256 `511d0cf5f3e7ce845bf4e8f0828e0ea203f05965014f44160ac12bf3f42d1186`

## Proven behavior

- The compiled C++ extension stores x/y as 32-bit values at byte offsets 0 and 4, with an eight-byte stride. GML reads that buffer as two unsigned 16-bit halves, with a four-byte stride. These are producer and reader layouts, not two equivalent output paths.
- At the configured `M`-point cap, the C++ loop post-increments `pointCount` beyond the vectors it initialized. The non-smooth branch returns `pointCount + 1`, or `M + 1`, after writing the closure. The smooth branch instead consumes `pointCount` vectors, including an uninitialized slot at the cap, and returns `simplifiedSize`.
- Separately, a negative authored epsilon can recurse without progress when maximum distance is zero. The distance helper returns squared interior distance, but square-root endpoint distance and zero-chord distance; it compares the result directly with epsilon.
- In the shader, foreground is center alpha not equal to zero. A neighbor is background when it is outside the image or `(R + G + B) * A == 0`. If both left and right are background, or both up and down are background, the shader returns its initialized zero output. Otherwise, if any neighbor is background, it emits white multiplied by `v_vColour`.
- The node does not explicitly reset draw tint or blend state. Whether state persists between evaluations is unknown.
- GML clears anchors before returning for an invalid surface or `-1`, while retaining the old `lengthTotal`, `lengths`, `lengthAccs`, and `boundary` values.
- The catalogue contains a synthetic `mask_alpha_only` case that GML does not read.

## Unknowns and capture needed

- Capture exact inputs, pre-state, post-state, and output bytes for cap handling, invalid-surface and `-1` returns, and the shader's center and neighbor classifications.
- Capture the negative-epsilon, zero-maximum-distance case to establish recursion outcome and termination behavior.
- Capture repeated shader evaluations, including incoming draw state and output, to determine whether tint or blend state persists.
- Native refusal rules, publication semantics, and executor behavior are unknown. No new implementation or refusal behavior is claimed.

Do not infer native behavior or source recovery from these observations alone.
