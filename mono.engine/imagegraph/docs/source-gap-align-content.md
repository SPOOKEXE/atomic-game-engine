# Gap Contract and Align Content CPU contracts

Pinned source: Pixel Composer `b69eca232217360cf1502ef0223523d818606652`.
The upstream MIT notice is distributed in `assets/licenses/PixelComposer.txt`.
The integration manifest records exact source paths, sizes and SHA-256 hashes.

## Gap Contract

`pc.gap_contract` accepts the source Surface In, Active, Mask, Mix, Invert Mask,
Mask Feather, signed integer Max Width, Invert and Keep Alpha controls. Concrete
surface depth, processor arrays and Oversample use the existing source helpers.
Source Int conversion rounds half to even before selecting processor rows.

Two RGBA8 staging buffers preserve the source default temporary depth. Invert
changes RGB before the binary passes and preserves alpha. Single-channel safe
drawing replaces the inversion shader with grayscale RGB and alpha one.

Each pass thresholds mean RGB multiplied by alpha at 0.5, counts eight-neighbor
occupancy and clockwise zero-to-one transitions, then applies the literal
phase-zero or phase-one triplet predicates. Positive Max Width removes selected
foreground pixels. Negative Max Width adds selected background pixels. The
phase alternates once per pass; this is not a generic dilation operation.
Interpolate is inert because this shader uses ordinary texture reads. Oversample
still selects the source outside-image rule. Every binary pass writes alpha one.
Keep Alpha restores only the original alpha in the final pass. Mask modifiers
and Mix run once through the common processor path.

For the five-by-five pattern `...../.###./.###./.###./.....`, counts 1, 2 and -1
produce these independent scalar-transcription goldens:

| Count | Rows |
| --- | --- |
| 1 | `...../..#../.##../...../.....` |
| 2 | `...../...../..#../...../.....` |
| -1 | `.###./####./####./####./.....` |

## Align Content

`pc.align_content` accepts Surface In, Active, Background, Align Anchor and
integer Pad Content. The output canvas keeps the input width and height. Exact
four-channel equality to Background clears the RGBA8 staging pixel. The pinned
bounding-box extension scans its quantized alpha bytes, including every nonzero
byte. A fully empty buffer has zero extent. Single-channel safe drawing replaces
the color-removal shader, so a matching background remains present on that route.

Pad Content is ordered right, top, left, bottom. A scalar repeats four times;
short numeric arrays are zero-padded, longer arrays are truncated, and nested
rows retain source processor selection. Pixel units pass through; Reference
units multiply alternating width and height before half-even integer rounding.
Units apply to linked values as well as authored values.

Translation is `-min + anchor * (canvas - content - opposite padding) + leading
padding` independently on each axis. Negative padding may clip content. Final
plain drawing uses source RGB blend `src + dst * (1 - srcAlpha)` and alpha-add,
not a normalized alpha-over formula. For source `(64,0,0,128)` over background
`(32,0,0,64)`, the RGBA8 output is `(80,0,0,192)`.

## Bounds and verification

Work admission covers the complete processor batch before kernel execution:
64 million work units, including Gap Contract passes and positive mask feather
work. Two Gap buffers or one Align buffer are admitted before allocation while
inputs remain live. Common output allocation admission includes scratch and
retained previous results. Failed evaluation preserves caller-owned results.

The outside native probe links stable dev53 archives against an owned
committed53 header/private-source snapshot, with new unoptimized constructor/test
objects and a replacement source registry. It
exercises actual Compile, Evaluate, EvaluateArray and Read/Write routes. The fresh
joined release55 core suite passed all 1,515 cases, including the 24 content-filter
cases. Its retained log is `.cache/build/dev/evidence/pixel-composer-2026-10-02/core-release-55-initial.log`.

This is a documented CPU source-formula profile. Floating-point shader precision,
licensed desktop builtin behavior and GPU texture/raster coverage are not
verified by these tests. Align's plain draw explicitly truncates translation to
an integer in this CPU profile. Fractional GPU draw footprints remain an external
parity gate. No renderer or GPU execution occurred.
