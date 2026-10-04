# Blobify native source profile

`pc.blobify` ports pinned `Node_Blobify` and `sh_blobify` from revision
`b69eca232217360cf1502ef0223523d818606652`. Source-derived portions retain the
[MIT notice](../../../docs/pixel-composer-m0/PixelComposer-LICENSE.txt).
This is a bounded binary64 CPU source-equation profile. Licensed/device float
loop behavior, mathematical intrinsics and pixel parity require observations;
no GPU or licensed runtime was executed.

| Source slots | Controls | Behavior |
| --- | --- | --- |
| 0,1 | Surface In, Active | Required surface; inactive source copy |
| 2,4 | Radius, Radius Map | Minimum0 half-even integer endpoints; mapped pair |
| 3 | Threshold | Step threshold or smoothstep midpoint |
| 5,6,7,8,9 | Mask, Mix, Invert mask, Feather, Channel | Common final processor |
| 10 | Smoothness | Smoothstep edge separation; zero selects step |
| 11 | Shape | Circle, Diamond, Square source indices0,1,2 |
| 12 | Distance | Source radius-distance weights |
| 13 | Keep Alpha | Restore original base alpha before inversion |
| 14 | Inverted | Invert base/samples and final output, including alpha |

The literal shader and UI disagree: index1 labelled Diamond uses a square
max-norm neighborhood; index2 labelled Square uses a Manhattan diamond.
Changing this correspondence would change existing authored graphs.
Circle iterates radius rings from0 with unit increments strictly below the
maximum uploaded Radius endpoint, and angles from0 strictly below literal
`TAU=6.28318530718` with step `TAU/64`. This CPU arithmetic executes64 angular
visits; preflight conservatively admits65. Native shader binary32 loop behavior
is a separate observation gate. Square/diamond candidate loops begin at negative
maximum endpoint and preserve fractional origins and inclusive end tests.

Mapped Radius endpoints preserve physical authored/link precedence and undergo
minimum0 then half-even rounding for ordinary numeric getter inputs. Surface
getter dimension rows bypass that numeric processing; their provenance remains
attached through row selection. Missing Radius maps use the first endpoint;
map mean RGB ignores alpha and produces a continuous per-pixel radius without
another rounding pass. Radius Map Range is the native projection of physical
Int slot2, default pair0,3.

Shader sampler_simple uses nearest for Pixel/CleanEdge and base texture filtering
for the other inherited/selected modes. Bicubic/Lanczos names do not execute
extended kernels here. Radius maps are nearest independently. Common oversample
rules apply to each base sample. Alpha participates in sampled mean brightness,
while Distance weights apply to all four components. Red-format safe draws
replace the Blobify shader and replicate red into RGB before final masking.

Circle maximum uploaded endpoint0 has an empty accumulator. Distance radius0 consumes0/0 in the
other shapes; empty/zero/nonfinite weights are undefined source arithmetic.
These configurations return named `UnsupportedExecution` diagnostics, preserving
previous public outputs. No zero-weight stabilizer or replacement morphology is
added. Negative Smoothness reverses smoothstep edges and is likewise refused
when its shader branch is consumed. Valid configurations retain literal step,
smoothstep, inversion and Keep Alpha ordering.

Original batch admission precedes inactive copying and staging. Original shape,
radius/pair and image rows are inspected. Physical linked/authored Radius takes
precedence; the synthetic pair contributes only when that source-default
projection is selected, and unmapped pair metadata remains inert. Circle quote is65 times ceil(radius);
polygon quote is the full `(2*ceil(radius)+1)^2` candidate square. Only shapes
present in the original rows contribute their corresponding bound. Per-cell
quote128 for nearest covers texel64 plus coordinate/trigonometric/weight/RGBA
accumulation64; filtering quote384 covers four texels256, bilinear64 and cell64.
Fixed512 covers map, base, brightness, threshold/inversion and final processing.
Mask work is separate `(64+128*ceil(feather))*largestMaskPixels*rows`. The complete
sum must fit64M units. Units count source expressions/reads, not measured cycles
or a machine instruction bound. Worst output depth16 bytes/pixel plus row
metadata, retained staging, mask and feather storage are admitted before copies.

Independent Python equations generate literal pixels for all three shapes,
Distance, inversion and fractional mapped Radius. Compiled-graph fixtures cover
numeric ties, Surface getter rows, maps, masks, channels, formats, animation,
round-trip persistence, heterogeneous batches and work/byte atomicity. A source
PXCX inverse fixture verifies Int slot2 and Surface slot4 while retaining unknown
archive data. The joined optimized CPU run passes all 74 Kuwahara/Blobify cases, all 2745 core
cases and all 250 IO cases, with dependent host suites. Authored static-attribute
arrays fail compilation; linked arrays fail at the named scalar reader before
publication, preserving the prior output. Measured performance remains pending.
