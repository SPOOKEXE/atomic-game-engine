# Edge Detect and Lens Blur source contracts

The native `pc.edge_detect` and `pc.blur_bokeh` executors follow Pixel Composer
commit `b69eca232217360cf1502ef0223523d818606652`. They are bounded CPU references
for the pinned GML and GLSL formulas. Joined runtime tests passed. The
expected pixels below are arithmetic derivations, not captured GPU output.

| Pinned source file | SHA256 |
| --- | --- |
| `scripts/node_edge_detect/node_edge_detect.gml` | `5092abdab90fdcda76a840d5dbac36c2924f85fb87c56cafa188325981453456` |
| `shaders/sh_edge_detect/sh_edge_detect.fsh` | `3a0beb1cb300efc110063d18e0d2d4f6cb0a724d72d9f96acb26c29bc4b4c0c6` |
| `scripts/node_blur_bokeh/node_blur_bokeh.gml` | `9d0257eceb08ac7c8e7bd650724f4e88681ddbfdde6c79804e3e738c1322cf6e` |
| `shaders/sh_blur_bokeh/sh_blur_bokeh.fsh` | `0832059b89e4e0af69654ddc50f7a2731a39d5630ca7c20a6539b158a39d9ff6` |

## Edge Detect

Sobel and Prewitt compute `distance(horizontal / divisor, vertical / divisor)`
over all four RGBA components, with divisors four and three respectively.
Equal horizontal and vertical gradients cancel. Laplacian divides signed RGB
by two and preserves the original alpha. Neighbor mode takes the maximum
absolute RGB difference from selected neighbors and ignores the center.

`attribute_filter` contains exactly nine switches in row order, from top-left
to bottom-right. Its source default is `[1,1,0,1,0,0,0,0,0]`. Only entries equal
to one select a neighbor. Flat scalar, integer and boolean arrays, including
mixed numeric/boolean leaves, are supported. Nested or image leaves are refused.
Integer values must fit signed 32-bit storage. Fractional integer-uniform uploads
need a desktop source observation; the native executor diagnoses them rather
than guessing a conversion. The official
[integer-array uniform API](https://manual.gamemaker.io/beta/en/GameMaker_Language/GML_Reference/Asset_Management/Shaders/shader_set_uniform_i_array.htm)
specifies an integer array.

Level In/Out remap RGB. Greyscale uses luminance weights
`[0.2126,0.7152,0.0722]` multiplied by original alpha; black-and-white thresholds
that result at 0.5. Equal Level In endpoints are an undefined source division.
Every neighbor UV is clamped before sampling, so oversample controls are inert.
The source's simple sampler consumes hardware nearest/bilinear filtering.

## Lens Blur

The source integer getter rounds scalar and depth-one array iteration leaves
before shader submission. The native source profile uses round-half-even:
1.1 becomes one tap and 1.5 becomes two. Residual deeper arrays are refused.

The spiral starts with `rec = 1` and `hang = [0,strength*0.01/sqrt(iteration)]`.
Each tap applies `rec += 1/rec`, then the source row-vector rotation by
`2.39996323 * (1 + (rotation-1)*pi/180/100)`. Horizontal offset includes the
surface height/width aspect ratio. Signed strength is preserved. Strength maps
use average RGB and ignore alpha; an unbound mapped strength uses the range
minimum. UV influence is `tap/iteration`, with nearest map sampling and ignored
map alpha.

Per-channel weight is
`(smoothness + pow(sampleRGB*sampleAlpha,max(contrast_factor,1))*contrast) * curve^3`.
RGB divides its weighted premultiplied accumulation by each channel's weight.
Alpha divides its weighted accumulation by the average channel weight. Enabled
curves admit at most nine anchors and require a nonzero header scale.
Colorize and gradient intensity/scale/shift controls are shader-inert: the pinned
shader submits their uniforms but never uses them in the resulting accumulation.

Nonpositive iteration, zero accumulated divisors, negative power bases,
nonfinite arithmetic and unrepresentable curves produce explicit diagnostics.

## Shared bounds and verification

Inactive nodes copy the input. Single-channel R8/R16/R32 inputs use source-safe
greyscale drawing before shader-only checks. Normal execution preserves the
source common mask, mix, inversion, feather, channel selection and output depth.
Owned results and mask intermediates use the existing evaluation byte ledger.

Both kernels admit at most 64 million estimated work units across the entire
processor batch before allocating output. Edge Detect charges ten units per
pixel. Lens Blur charges each tap for texture filtering, bounded curve work and
its accumulation. Neither kernel allocates per-pixel or per-tap scratch storage.

| Arithmetic fixture | Expected RGBA8 |
| --- | --- |
| Edge vertical step `[0,0,0,0]` to `[64,0,0,192]`, center Sobel or Prewitt | `[202,202,202,192]` |
| Edge selected difference `[64,128,192]`, greyscale with alpha 128 | `[60,60,60,128]` |
| Edge equal diagonal gradients | `[0,0,0,255]` |
| Lens uniform input `[64,128,192,128]` | `[32,64,96,128]` |
| Lens opaque red/green taps, contrast 10, smoothness 2, factor 1 | `[219,219,0,255]` |

The suites contain 13 Edge Detect and 15 Lens Blur cases, including real graph
publication, byte refusal preserving a prior result, processor rows and work
refusal before output allocation. Strict compiler checks passed. The joined development and release core suites
each passed 1,343 cases and 1,812,127 assertions, including all 28 cases here.
No GPU or profiling run was performed for this chunk. Native double trig/pow and existing CPU texture sampling define this
profile. Exact desktop getter tie behavior, shader float arithmetic and licensed
GPU texture coverage remain external verification gates.
