# Source Fold Noise

`pc.fold_noise` follows the pinned Pixel Composer constructor and shader at
commit `b69eca232217360cf1502ef0223523d818606652`. Numeric fixtures use
independent binary32 references. They do not claim parity with a particular
GPU or runtime.

## Inputs and shader behavior

The constructor assigns 14 physical slots: `dimension`, `position`, `scale`,
`iteration`, `stretch`, `amplitude`, `mode`, `rotation`, `mask`, `uv_map`,
`uv_mix`, `level_in`, `level_out`, and `detail`. Position uses simple units.
Dimension controls the raw sprite extent and output allocation uses the
rounded extent.

The shader flips the UV map's Y coordinate, blends its sample with the input
UV, normalizes Y by the image aspect ratio, subtracts normalized Position,
rotates, then applies Scale. Each iteration applies the cosine update using
the current coordinates, the sine update using the updated coordinates, then
Amplitude. Greyscale mode adds the folded vector length to all four channels;
Map mode adds the folded X and Y values to red and green. Level In and Level
Out transform RGB, and sampled UV alpha multiplies the resulting alpha even
when UV Mix is zero.

## Native evaluation limits

The evaluator keeps source iteration rounding and the two source modes. It
refuses consumed non-finite values, equal Level In endpoints, and output values
that cannot be represented in the selected surface format. Zero Scale is
defined by this shader and is accepted. Nonpositive iteration skips the
Stretch, Amplitude, and Detail loop inputs. An uncovered raw sprite extent is
cleared without evaluating fragment controls.

The requested output format is retained. Shared mask, channel, and mix
processing follows the source generator path. Its mask-alpha-only input is not
read by this generator call. The selected batch is admitted before output
images or observers are published. The quote charges 512 logical work units per allocated pixel, plus 4096 per
positive iteration for every allocated pixel in a row with any covered pixel.
Wholly uncovered rows charge only 512 per allocated pixel. This conservative
charge includes preflight, drawing, transcendental loops, and staging. It is
not a timing measurement. The cumulative cap is 64 million units across the
batch.

Shader transcendental results, texture sampling, raster coverage, and
unspecified arithmetic may differ by GPU and runtime. The fixtures check
source-derived CPU behavior, not licensed-runtime conformance.
