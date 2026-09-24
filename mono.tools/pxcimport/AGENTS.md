# pxcimport invariants

- This shared, headless tool opens a bounded PXCX file and calls `Engine::imagegraphio` for its semantic projection. It does not implement foreign nodes or evaluate images.
- The `.graph` result is a native projection. It is not a Pixel Composer save file and cannot restore opaque source data. Print diagnostics for every opaque node and projected output.
- `--output-id` selects one projected output by its durable text ID. An absent selection keeps all outputs. Reject an unknown selection before writing.
- `--reference-rgba` writes the saved 256x256 straight RGBA8 thumbnail bytes with an explicit dimensions and hash report. It is a source reference, not an evaluated native output.
- `--extract-node` writes only the mapped upstream closure of one image node after native compile validation. It reports every omitted opaque dependency edge and does not alter the lossless import or imply full-project output parity.
- Do not embed or copy private PXC projects into this module. Synthetic fixture tests are part of the normal suite; external acceptance remains opt-in through the existing environment variable.
