# Source Erode CPU profile

This implementation covers `pc.erode` from Pixel Composer commit
`b69eca232217360cf1502ef0223523d818606652`. The joined release CPU suites
validate this profile. GPU and licensed-reference comparisons remain open.

Source evidence:

| Source | SHA-256 | Behavior |
|---|---|---|
| `scripts/node_erode/node_erode.gml` | `8a050f7402210402deb9b8433efd675f9d8654fd1a0c10d5c36d7b9ea3d9dbbb` | Width slot 1, map slot 10, Pattern slot 11; processor mask/channel finish |
| `shaders/sh_erode/sh_erode.fsh` | `82fb06cdb1e3a145d2108e97dc1359eeb1c9e85c5b95a876eeb65b03c7e66064` | All four ordered neighborhood loops and signed alpha comparisons |
| `scripts/shader_functions/shader_functions.gml` | `3fc33618476756eec19830b3e1ef62b521637a443cefe366000bb890372e1cc7` | `shader_set_m` endpoint uniforms; unfiltered map texture; oversampling |
| `scripts/_draw_defines/_draw_defines.gml` | `f4702dd2337ca20635d699456b6ef27c11ff1aa890f91610629b4d91b94eb51c` | Cleared target and one/one alpha blend factors retain fragment RGB and alpha |
| `scripts/node_value/node_value.gml` | `6ca786dca5c9bb7a9116faaee9e8d24e0e5c86aea71ac376230fbccb491e17dd` | `setMappable` stores endpoint pair in the original numeric slot, with mapped depth 1 |

## Ordered shader behavior

Positive Width changes only the base pixel's alpha to the smallest visited
alpha. Nonpositive Width starts its comparison alpha at zero and copies RGB
and alpha from a strictly greater-alpha sample. Equal alpha keeps the first
winning sample. The original RGB remains when no sample wins.

Radial takes 65 source dyadic angles per integer ring, including repeated
angles. Box walks X then Y from `-maximum` in increments of one; Diamond
uses that same order and skips positions outside its Manhattan bound. Cross
visits positive X, negative X, positive Y, negative Y. Mapped Box/Diamond
starting offsets can be fractional because their maximum comes from both
range endpoints, even when the local mapped Width is smaller.

The shader's Preserve Border uniform is unused. Use Alpha changes `isSolid`,
but its result is assigned to an unused local and never affects color. These
controls remain authored and serialized without an invented visual effect.
Erode is unrelated to the existing Dilate center-pull UV distortion.

`width_map_range` is a native projection of source Width slot 1, not an
additional source socket. Flat numeric endpoints use the existing source Int
getter's ties-even rounding. Nested numeric rows retain source fractions.
Mapping without a surface keeps the first endpoint, exactly as `sizeUseSurf=0`.
A linked map mixes endpoints by mean RGB without clamping HDR map values.

## Native sampling and storage

The CPU profile evaluates shader loop and coordinate arithmetic as binary32,
with a white vertex color, nearest clamp texture reads and the source
sampler_simple oversampling
branches. The wrapper never calls `shader_set_interpolation`; synthetic native
Interpolate metadata has no effect on these reads. Native shader fragment
results use the existing processor surface format and mask, mix, invert,
feather, alpha-only and Channel finish. Inactive processors copy source bytes
and format unchanged.

This defines a device-free reference profile. It does not establish licensed
GameMaker GPU texture precision, filtering state, blend/raster coverage or
platform equivalence. `RequireSourceGpuRasterCoverage` refuses active Erode
with a named diagnostic. No GPU comparison or performance claim was made.

The first processor row admits a conservative whole-batch bound using all
original Width values/ranges, source image dimensions and `ProcessorCount`.
At most 64,000,000 texel-read/write work units are allowed. Every offset
uses one nearest tap: `SampleTextureSimple(..., false)` explicitly sets
Interpolation to Pixel, regardless of synthetic Interpolate metadata. Empty
and Black outside addresses read no texel; the bound conservatively counts
one. Every fragment additionally counts one base read, one output write,
and one map read when mapping can use a supplied surface. No unreachable
Bicubic/Lanczos multiplier is charged. The bound includes the
entire radial or bounding-box scan, before any output row is allocated. All
batch output payloads are checked together in their resolved surface format;
normal row/output ledger charges remain authoritative. Conservative admission
can refuse a batch whose individual selected rows would fit a tighter bound.
Controls exceeding finite shader math or work/storage bounds diagnose failure;
caller outputs remain unchanged. Processor postprocessing uses its existing
bounded scratch admission.

## Persistence and proof

Native graph save/load retains controls, source arrays and timeline keys.
Static local PXC mapped endpoint edits use source slot 1 and preserve unknown
input fields. Unsupported animated/linked inverse edits and nested endpoint
inverse writes diagnose refusal without replacing the caller's archive bytes.
The existing Bevel static mapped inverse remains covered by its original suite.

`reference.py` is an independent scalar transcription of the pinned shader,
using binary32 arithmetic and nearest/Empty sampling. Its source digest is
checked before generating eleven complete 5x5 image goldens. The C++ graph
suite covers all four patterns and both width signs, unused controls, first
winner RGB order, nested fractional mapped offsets, flat endpoint rounding,
arrays, animation, native save/load, signed floating surfaces, all ten
documented oversampling modes, mask/channel finish, inactive behavior,
whole-batch sample-work and mixed-format byte limits, plus GPU-coverage refusal. These are reference-profile
results, not captured GPU goldens.

Outside archive probes are diagnostic only. Final acceptance requires the
fresh joined release build after the shared metadata and inverse patches are
reviewed and landed together.
