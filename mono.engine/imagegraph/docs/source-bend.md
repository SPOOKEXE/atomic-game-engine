# source bend

grug add native `pc.bend` from pinned `Node_Bend` and `sh_bend_draw`. Arc and Wave
use the source's capped grid, with two triangles per cell in column-first order.
this is a bounded CPU raster profile. licensed GPU coverage and float precision
remain unverified.

`PrepareBend` reads one selected row, builds Arc or Wave vertices, records raw
bounds, then applies source fit and centred scale. Arc keeps the source's
unscaled centre offsets and negative `lengthdir_y`. Wave uses the cross-axis
coordinate, frequency and shift. zero transform scale collapses triangles.

fixed dimensions keep their unrounded values for fitting and offsets. physical
surfaces use source round-to-even and minimum one, subject to native caps.
[source round](https://manual.gamemaker.io/lts/en/GameMaker_Language/GML_Reference/Maths_And_Numbers/Number_Functions/round.htm)
uses even ties. project units apply only to unlinked dimensions. dynamic dimensions use the
sampled vertex bounds. Keep ratio changes fixed-mode transform scale only.

`AdmitSourceBend` quotes vertex work, clearing and clipped triangle candidate
work for every selected row before drawing starts. the complete batch has a
64 million work-unit cap. byte reservations cover temporary vertices and output
surfaces. inactive rows quote their copy and ignore active-only controls.

`DrawBend` uses pixel centres and a top-left edge rule. barycentric UVs become
`uv * uv_scale - uv_shift`. shader sampling uses physical output dimensions as
its reference, including Bicubic, Lanczos and inherited CleanEdge. each fragment
adds source RGB over inverse-alpha destination RGB and adds both alphas, with
RGBA8 quantization after blending. this is the source `BLEND_ALPHA` rule.

active output is RGBA8 because source `surface_verify` omits its format argument.
inactive output preserves input pixels and format. raw active texture binding
rejects Atlas. inactive SurfaceAtlas copies remain available. missing surfaces,
empty source grids below two pixels, undefined source switches, nonfinite
geometry and native work or byte limits report named diagnostics.

`docs/pixel-composer-m0/native-bend-validation-2026-10-07.json` records source
hashes, joined checks, failed checks and remaining limits.
