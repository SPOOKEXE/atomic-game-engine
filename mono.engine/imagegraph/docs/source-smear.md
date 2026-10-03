# Smear CPU source profile

Pinned Pixel Composer revision b69eca232217360cf1502ef0223523d818606652.

- `scripts/node_smear/node_smear.gml`: c39cf8fcdae7c59a1f14efde56706716d686666fb8545625c2a9a7c51fac35a8.
- `shaders/sh_smear/sh_smear.fsh`: 2dc4f9a3d8a7e8397abb6e334fd059c4b1078a04cf5cae78ed0b10dd8eea4586.

The native CPU draft preserves the two shader outputs, normal and inverted selection, independent greyscale/alpha modulation, four inverted rendering modes, mapped strength/direction ranges, source curve evaluation, fractional spread angles, maximum/additive blending, side color and final texture. The authored Side Texture control is preserved but unused by the pinned shader.

Main texture interpolation and oversampling use the native source sampler profile. UV and numeric map reads use point filtering. Final Texture uses a point-filtered texture stage while retaining the main input dimensions used by the shader's bicubic/Lanczos region. Mask, Mix and Channel affect only color output. All output storage follows the native selected surface format.

The shader leaves `out depth` and `out basePosition` unwritten when a sweep finds no qualifying sample. GLSL out parameters do not inherit caller initializers. Native color remains usable when independently defined; undefined depth or final-texture position gets an explicit per-output diagnostic. See the [GLSL specification](https://registry.khronos.org/OpenGL/specs/gl/GLSLangSpec.4.60.html). Red-only safe draw replaces the Smear shader and never writes its second target.

The current stateless inactive route copies source color and refuses Depth Pass because the source retains a previous output. The adjacent caller-owned state proposal specifies the completion seam. GPU shader precision, MRT behavior and licensed output comparisons remain separate acceptance work. CPU intermediate math uses doubles except the source loop parameter; this profile does not claim GPU float parity.

The active whole processor batch admits worst original dimensions, resolution and spread before the first output allocation. Shader work is bounded to64million conservative sample-operation units, including validation of defined outputs before publication and combined mask-feather work. The inactive copy bypasses shader work; its retained-output admission is part of the pending state tranche. Curves use the source GLSL64-float profile, at most9 anchors. Nonprogressing resolution, zero spread with an enabled spread curve, and nonfinite shader math refuse explicitly. Finite authored controls can overflow derived coordinates; the new Smear boundary checks both pre-remap coordinates and derived interpolation tap coordinates before a sampler cast. Reproduced NaN-to-unsigned diagnostics are retained with the focused probe. These guards do not modify the shared sampler. Refusal preserves prior publication.
