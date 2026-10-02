# Source colour filter CPU profile

The four colour filters follow the pinned node controls and the shaders each node
actually submits. Their processor inputs use the ordinary source row scheduler,
precision inheritance, graph input resolution, copied document persistence and
shared byte ledger. Output admission precedes pixel or palette allocation.

`pc.color_adjust` implements brightness, contrast, exposure, hue, saturation,
value, blend, alpha and all eight mapped controls. Map textures are nearest
sampled into the source RGBA8 scratch profile, including normal blending and
single-channel safe-draw expansion. Surface contrast clamps before brightness;
palette contrast and brightness clamp together. A mapped flag alone retains the
stored scalar in palette mode; imported endpoint arrays project into an explicit
map-range field and contribute their first endpoint. Map textures are ignored in
palette mode. Surface blend amount includes
blend alpha; palette blending interpolates packed RGBA. Palette Alpha and Blend
mode are ignored by the source. Channel is ignored in both branches. The shader
implements numeric blend modes 0 through 12; later menu choices use its default
normal colour branch. Menu labels are not substituted for those shader branches.
Masks retain their RGB components when the source mask modifier takes its early
return, including when Alpha Only is enabled. Inversion/feather modifiers use the
source grayscale or alpha construction, then the existing bounded Gaussian CPU
profile. The palette result remains available when an absent surface output is
refused. An inactive node copies its surface and refuses the uncaptured retained
palette output.

`pc.color_replace` implements Order and Closest Color in HSV, threshold matching,
soft replacement, alpha multiplication during comparison, replacement of unmatched
colours, mask/mix and selected output channels. HSV distances do not wrap hue.
The source shader uses strict comparisons, preserving the first equal-distance
candidate. Closest randomization evaluates the explicit shader hash from an
explicit Seed control. No process RNG is read. Random mode depends on GameMaker's
opaque `array_shuffle` permutation: it replays only an exact host output receipt.
Empty palettes, a one-element Order source palette and missing observable Seed
likewise require a receipt, instead of inventing undefined GPU results.

`pc.colors_replace` compares RGB distance, preserves the first tie, and multiplies
source alpha by target alpha. Its Threshold control is present in the node but
unused in the submitted shader. The GLSL profile uploads the first 256 palette
entries. Missing target slots and empty source palettes retain source uniforms
and require an exact receipt.

`pc.color_separate` returns an image array. All Colors reads unique nontransparent
RGBA8 packed pixels and orders them by descending byte HSV. Explicit colours
support exact RGBA matching or normalized Lab distance. Lab ties select the last
palette colour, and matching compares the entire selected colour including alpha.
Frames have the source default RGBA8 format. The GLSL palette has 128 slots;
extraction beyond that limit and non-RGBA8 packed readback require observations.
No source surface returns an empty array. Frame pixels, owned frame/item metadata
and the whole aggregate result are admitted before frame construction.

This is a native CPU formula profile. GPU bitwise output, ambient draw state,
GameMaker packed-HSV rounding and equal-key builtin sort order have not been
measured. Packed HSV uses half-even byte rounding and stable scan order for equal
native sort keys; shader calculations use native floating point. These choices
are explicit, and are not source-runner parity claims. Host receipts bind exact
authored node, signed frame, resolved controls and input image hashes, and use the
existing bounded host validation and publication route. They grant no ambient
filesystem, process or random access.

The official [GameMaker HSV constructor documentation](https://manual.gamemaker.io/lts/en/GameMaker_Language/GML_Reference/Drawing/Colour_And_Alpha/make_colour_hsv.htm)
describes the 0 through 255 HSV component scale; it does not establish the opaque
builtin byte-rounding behavior used by these native profiles.

Sources are pinned to `b69eca232217360cf1502ef0223523d818606652`:

| Source | SHA-256 |
| --- | --- |
| `scripts/node_color_adjust/node_color_adjust.gml` | `aa202919b88702e1ed57ffe20a2f6272261cd617dfc000f179aa384b7a5b4e95` |
| `scripts/node_color_replace/node_color_replace.gml` | `6ab2c79f0e9491aca75270071db0b1a849350103fd222cc0d0e8777937deb94f` |
| `scripts/node_colors_replace/node_colors_replace.gml` | `c98df893c49e67f325f8ed51024f27168cd87f138919c12c41de76a6e6fd37d2` |
| `scripts/node_separate_color/node_separate_color.gml` | `55f8a2f86e44bdde36c62a77ae675d1914ccf98ca2694974e3c0efccafe204ab` |
| `scripts/color_function/color_function.gml` | `cbe9a62a8b56257756bd3fecf1c7eb9797eda9695d34b0b7b390d0bda0120177` |
| `scripts/mask_function/mask_function.gml` | `94c22c8b1d3085c8ada6c673e5836326a8c866579b35ebb4487c343fb4f94023` |
| `shaders/sh_color_adjust/sh_color_adjust.fsh` | `36ab6069f85fbdbb3699136194cbec73a892932056f2008b1f2ac45275b7d7a7` |
| `shaders/sh_palette_replace/sh_palette_replace.fsh` | `b0eb969a59600f6584641b2e1bec1169fcb0956db5139a5dd03e3137df036e1d` |
| `shaders/sh_colours_replace/sh_colours_replace.fsh` | `a73f12166e5c5d678028dcfa1b89849ef92b2523e32afc225607b1f0f2c0f65d` |
| `shaders/sh_separate_color/sh_separate_color.fsh` | `6fe0ebf4accc51bf3d22737b356df2c859dd0f0f3a4313cdfff0a5e9613f95b3` |
| `shaders/sh_mask_invert/sh_mask_invert.fsh` | `88238a63dcbf0fd137b8575886b827c0cef422a2aa7501a8e59b765acc9d5b0b` |

`SourceColourFilters.cpp` exercises compiled graphs, all blend indices, mapped
and colored masks, source precision formats, palette persistence, ignored controls,
Order/Closest/soft matching, RGB alpha composition, unique separation and Lab ties,
linked processor rows, exact captured permutation replay, stale-input rejection,
and aggregate budget failure with unchanged caller output.
