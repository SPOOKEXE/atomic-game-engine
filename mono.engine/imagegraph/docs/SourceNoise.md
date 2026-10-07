# source noise

grug native `pc.noise` follows `scripts/node_noise/node_noise.gml` and `shaders/sh_noise/sh_noise.fsh` at Pixel Composer revision `b69eca232217360cf1502ef0223523d818606652`. licensed runtime pixel parity remains unverified. platform sine implementations can produce different low bits in this hash.

## function logic

prepare resolves the authored source seed, selected output dimensions and depth, color mode, level ranges, and raw UV and mask surfaces. zero or negative raw dimensions use the source allocation clamp to one. equal binary32 input level endpoints refuse execution because the shader divides by their difference.

shade uses binary32 operations for the source sine hash. fractional seeds interpolate adjacent seed hashes before the unclamped input/output level remap. RGB and HSV sample three coordinate offsets and apply their component ranges. Greyscale samples one hash and repeats it across RGB. HSV uses the shader's fractional hue conversion.

UV mapping samples nearest pixels, flips the mapped Y coordinate, and mixes coordinates with UV Mix. mapped alpha applies even when UV Mix is zero. raw UV and mask Atlas bindings refuse execution because the source sampler helper does not unwrap them.

without a mask, output stores directly in the requested source depth. with a mask, the source mask-empty shader multiplies alpha by mean mask RGB times mask alpha, writes default RGBA8 scratch, then copies back. this scratch clips and quantizes HDR even when the selected output depth is floating point. the synthetic Mask Alpha Only setting does not change this source shader.

quote checks all selected rows against a cumulative 64 million scalar work-unit limit before output allocation. each pixel quotes 512 units covering sample validation, execution sampling, hashing, color conversion, and optional masking. a conservative whole-batch byte quote covers maximum output dimensions, selected rows, output metadata, optional mask scratch, and requested raster field copies. execution charges actual scratch payload while output storage remains live. numeric refusals identify the control or sampler responsible.

existing image noise field export owns the resulting scalar 2D raster. this source generator does not use the native value-noise recipe algorithm.
