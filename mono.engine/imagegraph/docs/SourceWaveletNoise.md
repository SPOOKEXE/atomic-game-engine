# Source Wavelet Noise

`pc.wavelet_noise` follows the pinned Pixel Composer constructor and shader at
commit `b69eca232217360cf1502ef0223523d818606652`. Its numeric fixtures use
independent binary32 references. They do not establish pixel parity with a
particular GPU or runtime.

## Inputs and shader behavior

The constructor has 15 physical inputs. They are `dimension`, `position`,
`scale`, `seed`, `progress`, `detail`, `scale_map`, `progress_map`,
`detail_map`, `rotation`, `mask`, `uv_map`, `uv_mix`, `level_in`, and
`level_out`, in that order. Position uses simple units; the other transform
controls are scalar or vector shader uniforms. Scale, Progress, and Detail
can each be driven by a surface map.

At each fragment the shader samples enabled control maps and averages their
RGB channels. Each average interpolates its corresponding two control values;
map alpha does not affect those controls. The UV map flips its sampled Y
coordinate before blending it with the original UV. Its sampled alpha is
preserved even when UV Mix is zero.

The shader scales Y by the image aspect ratio, subtracts normalized Position,
applies Rotation in degrees, then applies Scale divided by 16. It evaluates
four wavelet-noise octaves. Each octave hashes the floored cell coordinate with
Seed, adds a Progress-dependent rotation, applies a reversed
`smoothstep(0.25, 0.0, dot(q,q))`, and divides by the octave scale. Detail
multiplies that scale between octaves. The weighted sum is normalized by the
sum of reciprocal octave scales, remapped through Level In and Level Out, and
written as grayscale RGB. Final alpha is the UV-map alpha when a map is
enabled, otherwise one.

## Defined limits and native evaluation

The shader has fixed four-octave iteration. Detail zero makes later octave
scales zero, so the division is consumed and does not have a defined finite
result. Equal Level In endpoints also divide by zero. These cases are refused
by the bounded CPU evaluator rather than assigned a fallback value. The
reversed smoothstep edge order is retained as the literal source expression;
GLSL does not specify a portable result for that edge ordering, so no device
parity claim is made.

The evaluator preserves requested output format and applies the shared mask,
channel, and mix processing. It resolves Reference Position against the first
prepared Dimension before processor rows are selected. Raw sampler bindings
must be representable by the CPU evaluator; the fixtures cover the supported
surface-map paths and named refusals.

Covered pixels require all three mapped flags, Scale Mapped, Progress Mapped,
and Detail Mapped, because the source shader reads two-component uniforms that
the generic unmapped upload path does not define for this shader. A consumed
Seed must be finite. A finite negative Detail is retained unless an octave
scale or accumulated weight reaches a zero divisor. Missing maps use the
source range's low endpoint.

Admission quotes 512 base units for each allocated pixel and 8192 additional
units for each covered pixel, with a cumulative 64 million unit batch limit.
It checks the selected batch before producing images or invoking observers.
Raw Atlas values on active samplers are refused at the corresponding map port.

The shader's transcendental results, reversed smoothstep behavior, texture
sampling, raster coverage, and unspecified arithmetic can vary by GPU and
runtime. The independent fixtures describe the source equations in binary32,
not a licensed-runtime conformance claim.
