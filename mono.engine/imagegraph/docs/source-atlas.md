# Source Atlas CPU routes

Pinned source: Pixel Composer `b69eca232217360cf1502ef0223523d818606652`.

These routes implement bounded CPU evaluation and typed Atlas transport. GPU and
licensed executable parity have not been verified. Random influence uses the
verified GameMaker HTML5 seed profile; desktop seed parity remains unverified.

- `pc.atlas` is Pixel Expand, with radial, scan and linear kernels. Its scratch
  surfaces use RGBA8 before the selected output format and normal filter controls.
- Atlas Get and Struct preserve pixel ownership and expose position, rotation,
  scale, blend and alpha. Per-row Get outputs use their actual scalar/vector types.
- Atlas Set requires the source SurfaceAtlas methods. A base Atlas lacks those
  methods, so it receives an explicit refusal. Replacement order preserves the
  old dimensions used by subsequent source transform calculations.
- Draw Atlas preserves ordered layers, source class filtering, and padding in
  right/top/left/bottom order. Combined arrays remain one operation; individual
  mode uses processor rows. Missing original surfaces use source dimensions 1x1.
- Affector preserves five influence modes, axis selection, pivot quirks, target
  interpolation and two random draws per uniform influence. Its preview accepts
  both Atlas classes. Undefined array holes remain ordered.

All work guards include the processor row count before output/scratch allocation.
The 64 million work limit charges Pixel Expand sample loops and Atlas layer pixels,
including the worst supported Lanczos reads. Affector also admits bounded curve
and influence work before copying output Atlas payloads. Retained source pixels
remain charged even when only metadata outputs are selected. Refusals retain the
last caller-owned output. Nonfinite or undefined source operations receive named
diagnostics instead of invented pixels.

The six `[source_atlas_nodes]` cases cover transforms, class boundaries, arrays,
persistence, source pixel priorities and seeded influence goldens. The two-row
Pixel Expand fixture crosses the total work limit while each row fits separately. The joined
development and release core suites each passed 1,343 cases and 1,812,127
assertions, including all six Atlas cases.

## Source SHA-256

Paths below are relative to the pinned source tree.

| Path | SHA-256 |
| --- | --- |
| `scripts/node_atlas/node_atlas.gml` | `ef991510f13280844f0982389f02d20d46b78cbc7d72286ab9da685c17025da7` |
| `scripts/node_atlas_get/node_atlas_get.gml` | `f3b210dbc1f72f13df0fc8f2f2c277f6eacc3e4a6d492cfbe7733dc711ec55f8` |
| `scripts/node_atlas_set/node_atlas_set.gml` | `762e1cee5a6f5e5d57ec5a8f855cf19d9e4f8ebaa03e4a2683b5ab22f438ba2f` |
| `scripts/node_atlas_to_struct/node_atlas_to_struct.gml` | `1861f8fa02f60a55dcf5cb2b72e103d6214beae82efb4cf1162e9afacefc3b9b` |
| `scripts/node_atlas_draw/node_atlas_draw.gml` | `6bf09c824cb4472d5f866468b39a478e4d455a79ee639702500316e07db74df5` |
| `scripts/node_atlas_affector/node_atlas_affector.gml` | `2ae42e48ec21ba9f535395d4476c0b385caf7642720770e1c9791e93a56051a4` |
| `scripts/__surface/__surface.gml` | `7c6459408774b60db2c33bd706df390cc66f9e3d857642a913028f23cc24a54e` |
| `scripts/surface_sampler/surface_sampler.gml` | `f94f25a55c36b584535c6b8bc92d2b3c02bf2b32687e4a2444ce4e4471fa894a` |
| `scripts/color_function/color_function.gml` | `cbe9a62a8b56257756bd3fecf1c7eb9797eda9695d34b0b7b390d0bda0120177` |
| `scripts/point_rotate/point_rotate.gml` | `648a19ea3c3768d70a84f956d48367fdc309f45309661d84b65c9820d50ddab6` |
| `scripts/lerp_float/lerp_float.gml` | `dd2a30392c4a1a8a5a09843b6b43ab45d7254c87878b065daaab7d48f00e129b` |
| `shaders/sh_atlas/sh_atlas.fsh` | `49ccd63cb270c45c3322724dfd64a903e0e2eaaab234755fe38da02b8ee83e40` |
| `shaders/sh_atlas_scan/sh_atlas_scan.fsh` | `bb9f073551bd4fae7c09b6f72d29a3647971ab345d64ce2b7ad29970696b6036` |
| `shaders/sh_atlas_linear/sh_atlas_linear.fsh` | `c9385fa1987cd7176eb08febd583f19f9d76a34f6fb250480ba5e518449f04b6` |
