# source pixel math

grug add `pc.pixel_math` from pinned `Node_Pixel_Math` and `sh_pixel_math`.
all 26 menu slots retain numeric source indices. shader slots 18 through 21
compare values, even though their menu labels say Map, Log, Max and Min.
appended comparison labels at slots 22 through 25 leave shader pixels unchanged.
this preserves the pinned source, with licensed capture parity still unverified.

`PreparePixelMath` reads selected controls and backing surfaces. main
SurfaceAtlas input unwraps its backing image, like `draw_surface_safe`.
Operand Surface and mask use raw bindings and refuse Atlas. absent Operand
Surface reports a named refusal because the source leaves that texture binding
unset. active output uses Color Depth; inactive copies preserve input storage.

`PixelMathChannel` evaluates shader arithmetic in float. Round is the shader's
`floor(value + .5)`, including negative and HDR values. Modulo subtracts the
floored quotient; Snap also floors. Clamp uses Range RG for Vec4 and Color,
but sampled operand RG for Surface. comparisons force alpha to one.
Surface operands sample nearest at normalized output pixel centres.
source overwrite blending preserves raw RGB before mask, outer Mix and Channel.

`AdmitSourcePixelMath` quotes every selected row before execution. the 64 million
work-unit cap covers pixel math and both complete mask-feather passes, their
taps, copies and weight setup. normal evaluation byte reservations cover outputs
and feather scratch. source array scheduling and instance animation use the
existing processor and animator paths.

zero divisors, undefined GLSL powers, reversed Clamp bounds and nonfinite shader
values return named diagnostics. fractional shader-int controls remain a named
unverified conversion. native arithmetic tests cover every menu slot, formats,
operand types, masks, persistence, animation and arrays. no speed claim made.

source overview: [Pixel Math](https://docs.pixel-composer.com/nodes/filter/effect/pixel_operation/pixel_math.html).
joined evidence lives in `docs/pixel-composer-m0/native-pixel-math-validation-2026-10-07.json`.
