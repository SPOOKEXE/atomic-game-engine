# Source conversion profile

`pc.interpret_number` consumes each processor row at source array depth one,
recursively spreads its numeric leaves, and produces a one-row surface. Scalars,
integer and Boolean numbers, enums, packed colours and native tuple carriers
retain their source numeric meaning. Uniform values and range arithmetic use
single precision. Scalar mode choices clamp to the three source EButton choices.
Array-selected choices bypass that clamp; an unmatched shader mode produces
transparent zero. The executor walks the input twice without copying its tree:
one bounded sizing pass, then one rendering pass. Output admission precedes pixel
allocation. Processor selection, source precision inheritance and document
persistence use the ordinary evaluator.

Greyscale computes `(value-low)/(high-low)`. Palette mode truncates a nonnegative
shader integer and takes its remainder against the uploaded palette length.
The native GLSL profile uploads the first 256 colours. Gradient mode wraps
normalized progress plus shift into `[0,1)` and shares Interpret Matrix's gradient
key and mapped-gradient evaluation. The pinned `shader_set_gradient` explicitly
enables filtered texture sampling, so mapped gradients use bilinear clamp.
Gradient interpolation uses the existing CPU shader formula profile; no GPU
bitwise parity is claimed by these tests.

Empty top-level source arrays return the previous surface handle without drawing.
A fresh native evaluation has no such retained handle and reports an explicit
refused output. An empty palette preserves prior GPU uniforms, also requiring a
source observation. Negative palette indices, singular ranges and nonfinite
uniform values are diagnosed rather than reading undefined GPU memory or
publishing an invented colour.

`pc.color_to_oklch` follows `rgb2srgbLinear`, `convertLrgbToOklab` and
`oklab2oklch`. Alpha is ignored. Integral packed colour transport is accepted for
Boolean, integer, scalar and enum values in the 32-bit colour range. Fractional
packed conversion is unverified and refused. The source returns NaN hue when
both opponent channels have magnitude below 0.0002. Native finite scalars cannot
carry NaN: hue is refused, while lightness and chroma remain available. A refused
processor hue row quarantines that entire output while preserving sibling rows.

Sources are pinned to `b69eca232217360cf1502ef0223523d818606652`:

| Source | SHA-256 |
| --- | --- |
| `scripts/node_interpret_number/node_interpret_number.gml` | `5bb47d37f8d79c7be7d849e25139a550671e60b0824a2567320e5392d3d499d7` |
| `shaders/sh_interpret_number/sh_interpret_number.fsh` | `a38a31290cb5a7d409e94118164bca7f9a5d71afd396026068218321b48ecf3e` |
| `scripts/node_color_to_oklch/node_color_to_oklch.gml` | `3cbdc8e0598612d4680c03e6f57053fef1f6775cec58645a7ec77a1c931f53de` |
| `scripts/okhsl_function/okhsl_function.gml` | `1006ad7ff497fc8c45d44e8d92cf7b308fe185ed7fe0623d5c61da0e4ed638b6` |

`SourceConversion.cpp` checks compiled graphs, persistence, multi-batch numeric
inputs, nested values, processor image and colour rows, palette truncation,
gradient wrapping and maps, all source surface formats, neutral hue diagnostics,
packed colours, and output budget failure atomicity. Oklch golden values were
calculated independently from the pinned source coefficients, not from the
executor helper.
