# Pixel Composer noise runtime evidence

Pinned source revision: `b69eca232217360cf1502ef0223523d818606652`.

Wavelet source at `shaders/sh_noise_wavelet/sh_noise_wavelet.fsh:74` evaluates
`smoothstep(.25, .0, dot(q, q))`. GLSL 4.60.8 §8.3 says `smoothstep` results
are undefined when `edge0 >= edge1`, which applies to these reversed edges.
[Khronos GLSL 4.60.8](https://registry.khronos.org/OpenGL/specs/gl/GLSLangSpec.4.60.html)

Scratch defaults Softness to `3` in
`scripts/node_noise_scratch/node_noise_scratch.gml:20`. Its shader computes
`w = length(fwidth(p)) * soft` at
`shaders/sh_noise_scratch/sh_noise_scratch.fsh:92` and passes `w` to
`scratch(p, w)`. The `scratch` function receives it as `f` and evaluates
`smoothstep(thick + f, thick - f, x)` at line 84. At the default softness,
`f` is nonnegative. The edges are reversed when `f` is positive and equal when
it is zero, both covered by GLSL's undefined case. GLSL defines `fwidth(p)` as
`abs(dFdx(p)) + abs(dFdy(p))` in §8.14.1. The same specification notes that
derivatives may be approximate and are undefined in non-uniform control flow.

These source observations need matched licensed-reference captures and backend evidence
before native parity is claimed. They do not establish that a complete native
implementation is impossible.
