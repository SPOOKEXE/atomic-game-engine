# Kuwahara native source profile

`pc.kuwahara` implements `Node_Kuwahara` from pinned revision
`b69eca232217360cf1502ef0223523d818606652`. Source-derived portions retain the
[MIT notice](../../../docs/pixel-composer-m0/PixelComposer-LICENSE.txt).
This is a bounded binary64 CPU shader-equation profile. Joined native, IO and
dependent CPU suites pass; no licensed runtime or GPU was executed.

| Slot | Control | Source behavior |
| --- | --- | --- |
| 0, 1 | Surface In, Active | Required original input; inactive copies it |
| 2, 16 | Radius, Radius Map | Integer getter, minimum 1, half-even round; mapped pair |
| 3, 4 | Mask, Mix | Common final processor composition |
| 5, 6 | Unused, Channel | Unused remains inert; channel selects final changes |
| 7, 8 | Invert Mask, Mask Feather | Common mask modifiers |
| 9 | Types | Basic, Anisotropics, Generalized |
| 10 | Alpha | Anisotropic ellipse only |
| 11 | Zero crossing | Anisotropic/generalized polynomial only |
| 12, 13 | Hardness, Sharpness | Anisotropic/generalized sector variance only |
| 14, 15 | UV Map, UV Mix | Distance-dependent sample remapping |

The native Radius Map Range field projects the pair held by Radius slot 2.
A source mapped toggle starts from `[0,current]`; numeric endpoints undergo
minimum validation and half-even rounding before upload. Physical authored or
linked Radius values precede synthetic static endpoints. Missing optional maps
use the low endpoint. Radius maps use unfiltered mean RGB, ignoring alpha.
Surface-linked numeric getters return dimension rows before integer validation;
this exact input origin remains attached through processor row selection.

Basic executes the literal non-HLSL `MAX_RAD=16` shader loop. Authored radius
remains unchanged for coordinate normalization and weights, even above 16. The
HLSL64 loop profile and licensed/device pixel parity remain outstanding gates.
Basic uses eight polynomial sectors with eta=0, fixed q=18 and a `.0001` weight
sum stabilizer. It is not the conventional four-quadrant Kuwahara algorithm.

Anisotropics executes the source Sobel tensor, horizontal Gaussian radius 5,
vertical Gaussian/eigen transform and elliptical sector filter. All three
intermediate surfaces are RGBA8, because source `surface_verify` omits depth.
Signed tensor/eigen channels therefore clamp during staging. Substituting a
floating tensor would change the algorithm. Generalized executes its original
floating loops from negative maximum uploaded Radius endpoint, with unit steps
and per-pixel range exclusion. Its eta denominator is `sin(crossing)*crossing`;
the anisotropic denominator is `sin(crossing)*sin(crossing)`.

Base filtering follows `shader_set_interpolation`: Pixel is nearest; the other
listed interpolation modes enable the base texture filter. The sampler_simple
shader does not execute the extended bicubic/Lanczos kernels. UV, Radius Map
and tensor samplers bind nearest separately. UV amount is the literal distance
term, including signed Gaussian offsets in tensor passes. No map means that
an otherwise unused undefined UV amount is not consumed. Red source safe draws
replicate red and bypass the active filter shader, as in source, before final
mask composition. Ordinary filter output alpha is opaque; sampled alpha does
not weight the sector means.

Zero polynomial sums, zero ellipse/Alpha denominators, zero sector weights,
nonfinite arithmetic and invalid source variance powers produce named
`UnsupportedExecution` diagnostics. The default Generalized radius 2/crossing .58
has sampled corners with zero polynomial sum; the native profile refuses that
arithmetic rather than inventing a stabilized filter. Crossing 2.5 provides a
finite source-defined Generalized configuration covered by independent literals.
[Khronos GLSL 1.20](https://registry.khronos.org/OpenGL/specs/gl/GLSLangSpec.1.20.pdf)
leaves powers with negative base, or zero base and nonpositive exponent,
undefined. Device handling of divisions, NaNs, texture state, binary32 arithmetic
and raster coverage needs actual observations; no such parity is claimed.

Whole original batch work is admitted before inactive copying or staging.
The bound is `(fixed + 1024*(2*radius+1)^2)*largestPixels*rows`, plus separate
mask work `largestMaskPixels*rows*(64+128*ceil(feather))`, within 64M units.
The fixed quote is 32768 if any original row selects Anisotropics, otherwise 8192.
It covers all tensor/blur/eigen/variance and common finishing arithmetic.
For exclusively Basic rows only the loop extent is capped at 16 in this quote;
normalization still uses the actual radius. Original type/radius/map-range rows
and image arrays are inspected, so a later dynamic mode or large image cannot
escape the bound. Byte admission includes16 bytes per output pixel per row,
row/image/array metadata, 64 bytes per largest main pixel, 32 bytes per largest
mask pixel and feather kernel storage. Actual scratch/output ownership keeps
its allocator reservations; public refusals preserve previous output.

Independent Python equations generate literal pixels for all three modes,
partial Radius mapping, distance-dependent UV sampling and Basic radius 32.
The authored graph fixtures also cover integer ties, Surface origins, processor
rows, modes, common controls, depth/safe draw, animation, persistence and atomic
budget/source arithmetic refusals. A real source PXCX inverse case checks Int
slot 2/Surface slot 16 with unknown data retention. The joined optimized CPU run passes all 74 Kuwahara/Blobify cases, all 2745 core
cases and all 250 IO cases, with dependent host suites. Authored static-attribute
arrays fail compilation; linked arrays fail at the named scalar reader before
publication, preserving the prior output. Measured performance remains pending.
