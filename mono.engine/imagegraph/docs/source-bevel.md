# Source bevel processors

`pc.bevel` and `pc.pb_fx_bevel` implement bounded CPU reference calculations from
Pixel Composer commit `b69eca232217360cf1502ef0223523d818606652`. Registration
establishes CPU routing capability. Licensed execution and GPU visual parity
have not been verified.

Bevel retains the source 65-direction quarter-step sweep and the high resolution
513-direction eighth-step sweep. Low resolution distances round to pixels.
Mapped height samples mean RGB without alpha. The shared source getter rounds
flat integer endpoints before processor row selection; nested inputs retain
fractions, matching the source getter's depth-two bypass. The kernel reads the
selected physical range, including when mapping is enabled without a map image.
Shift units, scale, slope, GLSL slope curves, surface format, oversampling and
processor mask controls follow the existing bounded source helpers. The visible
`oversample_mode` input is unused by the pinned processor, which submits its
`oversample` attribute instead.

Pixel Bevel retains three RGBA8 scratch surfaces, ordered edge and angle passes,
four cardinal sweep directions, two gradient evaluations and the separate inner
area output. Both outputs use RGBA8. Its third pass restores `bm_normal` after
the preceding shader reset: over transparent scratch, RGB multiplies by alpha
and alpha squares. Fixed-point attachment values clamp before blending.
Directional and all-side highlights operate on the staged inner area. Gradients
support source modes 0 through 4 and at most 128 keys. Plain texture reads use
the nearest clamp reference profile.

Both processors admit at most 64 million aggregate sample visits across selected
processor rows before allocation. Scratch storage is charged. Nonfinite shader
math, zero bevel scale, oversized curves or gradients, and excessive work return
named diagnostics without replacing caller results. The bevel curve profile
matches the source GLSL 64-float upload, allowing at most nine anchors.

The fixtures exercise compiled graphs, persistence, high resolution sweep
values, inactive and mix controls, flat and nested mapped endpoints, separate
Pixel Bevel outputs, gradient/highlight topology, alpha staging and atomic
refusal across multiple processor rows. GPU, licensed source and visual checks
remain separate verification gates.

The source staging interpretation also follows the official [GameMaker blend
mode guide](https://manual.gamemaker.io/lts/en/Additional_Information/Guide_To_Using_Blendmodes.htm)
and the [Khronos fixed-point blending rules](https://wikis.khronos.org/opengl/Draw_Buffer_Blend).

## Pinned source hashes

SHA-256 values identify the exact processor and shader files used for the port.

```text
bfa14a6fc20ad0ca9a3cad533683658180f8e246274f6cbe9a0487786875da27 scripts/node_bevel/node_bevel.gml
5afa940d7ec72aa67ce6335b52520bdb6790deeb7a0f5c0c45817ecfe8651770 scripts/node_pb_filter_bevel/node_pb_filter_bevel.gml
3fc33618476756eec19830b3e1ef62b521637a443cefe366000bb890372e1cc7 scripts/shader_functions/shader_functions.gml
94c22c8b1d3085c8ada6c673e5836326a8c866579b35ebb4487c343fb4f94023 scripts/mask_function/mask_function.gml
9024edcebffbfc924faebe5ccbc98f80637b53f4e311ef40d18f0a89ec352f96 shaders/sh_bevel/sh_bevel.fsh
f741708340aae255e0ff0f74079977203d18d13b2f2ee465a9151945486557a2 shaders/sh_bevel_highp/sh_bevel_highp.fsh
2549708a001d7132f80e8ce8d3fd795f8cf80e8cd86157d03146b58679dee6c2 shaders/sh_pb_fx_bevel_edge/sh_pb_fx_bevel_edge.fsh
60b503a73f13b9cae946cb0201a55536441dc02e0e256f737a57c12139d146de shaders/sh_pb_fx_bevel_angle/sh_pb_fx_bevel_angle.fsh
38b8b32220f6dc2708b4c778c343dcb7e75815fabc471f733a6d602313bec0ca shaders/sh_pb_fx_bevel/sh_pb_fx_bevel.fsh
898e164dd94895c91db4526045b0a9d4a37044832fb022b52d15be765a8278f8 shaders/sh_pb_fx_bevel_apply/sh_pb_fx_bevel_apply.fsh
f4702dd2337ca20635d699456b6ef27c11ff1aa890f91610629b4d91b94eb51c scripts/_draw_defines/_draw_defines.gml
6ca786dca5c9bb7a9116faaee9e8d24e0e5c86aea71ac376230fbccb491e17dd scripts/node_value/node_value.gml
75ae2d6a3df5c6d7f62e50adf0bfc1f1467c51e6519bb80011f6164536dff8d2 scripts/node_value_int/node_value_int.gml
de682b47fff4d6ebef720aadfbd486af25266a32a009ff113c6158e0f9969b75 scripts/scrollBox/scrollBox.gml
```
