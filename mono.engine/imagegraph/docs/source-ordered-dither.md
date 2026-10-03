# Ordered Dither source CPU profile

`pc.dither` implements the pinned `Node_Dither` colour shader, distinct from
`pc.dither_diffuse`. Source revision is
`b69eca232217360cf1502ef0223523d818606652`.

| Source file | SHA-256 |
| --- | --- |
| `scripts/node_dither/node_dither.gml` | `a640402fe04bb3903fb35e93f27245fac4a3a3bfcad81b117a506857d46f217e` |
| `shaders/sh_dither/sh_dither.fsh` | `14346d350b4c8908e78933a44858fb808c7a8a4d72d134408c050858f24962f4` |
| `scripts/shader_functions/shader_functions.gml` | `3fc33618476756eec19830b3e1ef62b521637a443cefe366000bb890372e1cc7` |
| `scripts/color_function/color_function.gml` | `cbe9a62a8b56257756bd3fecf1c7eb9797eda9695d34b0b7b390d0bda0120177` |

Pattern indices are 0 Bayer 2x2, 1 Bayer 4x4, 2 Bayer 8x8, 3 White Noise,
4 Custom, and 5 Matrix. Colour Type indices are 0 Greyscale, 1 Palette, 2 RGB,
and 3 HSV. Each quantizer constructs two colours and selects them using their
source-normalized Lab distances. Greyscale quantizes each RGB channel using
one Steps value, rather than converting the image to a single luminance value.
RGB and HSV use separate R/G/B steps. Palette mode retains source scan order,
nearest-two tie behavior, and the last exact-match RGB replacement.

Positional patterns use snapped pixel coordinates and divide Bayer/matrix ranks
by the cell count minus one. Linear patterns index by the contrast-adjusted
colour ratio and use raw ranks with no normalization. Invert changes Bayer,
Matrix, and Custom thresholds; the White Noise branch ignores it. Noise uses
source `sin`, dot coefficients, seed modulo, and fractional-part arithmetic.
Custom reads weighted RGB luminance from a repeating nearest-sampled map; its
alpha does not affect the threshold. Scale snaps both source colour sampling
and positional pattern coordinates to the same grid.

Exact colours bypass pattern sampling. Shader alpha always multiplies source
alpha again, so a non-palette image's alpha is squared. Selected palette alpha
instead multiplies source alpha. The red-only safe-draw shim replaces the
selected shader and expands red to opaque RGB; this route remains usable even
when Alpha or an incomplete Custom pattern was selected.

Contrast mapped mode preserves the source two-component uniform. A linked
numeric pair remains one mapped range through input getters and processor row
selection. A linked scalar repeats into both endpoints. A missing mapped surface
uses range X, matching `shader_set_f_map`'s uniform write followed by disabling
`contrastUseSurf`. With a map, contrast interpolates the endpoints using mean RGB
at the output coordinate. Unmapped mode uses the scalar control.

The native profile samples ordinary textures with nearest clamp and uses double
precision for the shader arithmetic. `useConMap` has no writer in the pinned
scripts, so this profile preserves its GLSL initial zero. The
[OpenGL specification](https://registry.khronos.org/OpenGL/specs/gl/glspec21.pdf)
defines zero initialization and retention of program uniforms. This source audit
is not evidence of licensed desktop raster, texture-filter state, floating-point
or noise-bit parity. GPU/HLSL execution and the HLSL-only 1024-colour uniform
profile remain separate gates; this profile admits 256 palette and 64 matrix
slots.

Source branches requiring unobserved renderer history remain explicit:

- Alpha mode skips writes to colour, contrast, palette, steps, scale and invert
  uniforms while the shader still uses them. A colour-surface invocation requires
  that prior uniform state; the CPU profile returns `UnsupportedExecution`.
- Missing Custom maps leave pattern uniforms unwritten. Custom Linear reads a
  dither array that this branch never uploads. Exact colours still complete
  because those shader branches are not reached; affected pixels refuse.
- Linear indices outside the uploaded array, empty palettes, zero scale/steps,
  and positional single-cell matrices have unverified or undefined source behavior and
  receive diagnostics instead of invented pixels or retained uniforms.

Active false copies the input. Source Active explicitly rejects arrays, so public
array-work fixtures retain scalar Active. Mix, masks, feathering, invert mask,
channel selection and surface depth use the shared processor pipeline. Before
any inactive copy or output allocation, row-zero admission checks original value
and image arrays, conservatively combining the largest source/mask dimensions,
palette scan size and feather radius with the entire processor row count. Total
work is capped at 64 million. Inputs and palettes remain borrowed for this bounded
call; no renderer pointer, shader state, recipe or palette buffer is retained.
Public failure preserves the previous result through the common evaluator ledger.
