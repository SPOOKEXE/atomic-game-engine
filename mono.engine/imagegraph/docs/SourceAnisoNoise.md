# anisotropic noise

Native CPU execution follows `node_noise_aniso.gml` and `sh_ani_noise.fsh` from Pixel Composer commit `b69eca232217360cf1502ef0223523d818606652`.

Shader arithmetic uses binary32 constants and operations. CPU sine is the platform float library sine; no claim of cross-driver sine identity is made. Nearest texture sampling uses native typed surface channels. UV mapping flips the sampled green coordinate and retains sampled alpha. Mapped controls sample original texture coordinates, before UV mapping.

The shader scales vertical UV by raw height / raw width, subtracts position / raw dimension, and rotates. Row coordinates wrap their signed absolute value modulo 289.653. The row seed shifts the horizontal coordinate. Tile wraps horizontal coordinates into [0,2). Level In and Level Out remap horizontal progress without clamping. Blend interpolates the two colour-seed random endpoints; Waterfall emits remapped progress. Unmapped Y Amount uses the second uniform endpoint, as the source shader does.

The output first stores in its selected typed depth. A mask reads that stored surface, changes alpha by mask RGB mean times mask alpha, writes an RGBA8 temporary, and copies back into the selected depth. This retains the source mask quantization stage.

All selected processor rows are prepared and quoted before publication. The fixed 64 million scalar-work budget quotes 512 base units and 512 covered shader units per pixel. Target bytes, request dimensions, RGBA8 mask workspace, raw Atlas sampler bindings, shader float range, equal Level In endpoints, undefined render modes and half-float storage overflow are checked with named ports. Relevant seeds must be resolved explicitly.

Tests use an independent transcription of the pinned random equations and hand-derived zero-X level extrapolation. No engine output was used as a reference image.
