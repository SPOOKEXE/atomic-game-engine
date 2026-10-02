# Non-Uniform Blur and Blend CPU source profiles

The source pin is `b69eca232217360cf1502ef0223523d818606652`.
The draft's `source-provenance.json` records the scripts and fragment shaders,
including every Blend mode. The existing Pixel Composer MIT notice covers these
source-derived formulas. These are native CPU formula profiles. Licensed desktop
built-in behavior, shader float precision and GPU raster coverage remain external
verification gates. No GPU or desktop oracle was used.

## Non-Uniform Blur

`pc.blur_simple` preserves the literal Manhattan tap weight and positive-diagonal
suppression. Its center contributes before gamma conversion; only neighbors are
raised to 2.2, while the final RGB is raised to 1/2.2. Gamma therefore changes
zero-size pixels too. Alpha is averaged rather than used to normalize RGB.
The blur mask multiplies average RGB by alpha. Override Color replaces RGB and
multiplies alpha; the gradient's brightness measure precedes this replacement.
Nearest and bilinear hardware sampling, oversampling, gradient-map sampling,
processor mask/mix/channel finish and concrete source color depths are preserved.
The obsolete oversample-mode control is inert in the pinned shader.

The source allocates UV Map at physical input 17, then Gradient.addShift replaces
that slot with Shift. Both names survive in its input-name map. The shader's UV
binding receives Shift. A nonpositive numeric Shift fails the source's `s > 0`
surface predicate and executes the pure kernel. Positive Shift needs an observed
source surface-handle alias; it is diagnosed rather than assigned an invented
texture. A separate linked UV Map cannot be treated as a second physical slot.
Single-channel safe drawing replaces the shader before these checks.

Fourteen cases cover fractional diagonal weights, gamma center behavior, mask
alpha, override and gradient order, gradient maps, sampling, common processor
controls, slot collision, safe single-channel drawing, actual compiled graphs,
processor arrays and atomic byte/work refusal. For uniform RGB64, gamma with
size0 yields136; size1 yields96 under this native profile.

## Blend

`pc.blend` implements all25 executable source modes at their original indices,
including separators between groups. Literal burn/dodge/divide/light and alpha
quirks are retained. Undefined selected divisions produce explicit diagnostics.
There is no second generic processor-mask pass: the blend shader consumes its
mask exactly once. Background/Foreground/Mask/Maximum/Constant dimensions,
Swap, opacity, preserve alpha, None/Stretch/Tile and position units are handled.
Horizontal and Vertical Align are read by the source but do not affect drawing.

Both temporary surfaces use the selected output format. Writes quantize before
blend sampling. None uses an explicit CPU pixel-center placement profile with
integer truncation of the draw offset; native desktop subpixel footprint remains
an external gate. Single-channel safe drawing replaces the active shader: a
single-channel background can bypass the blend equation, and a single-channel
foreground bypasses the Stretch sampling shader. SurfaceAtlas output owns its
new pixels and retains transform/original-surface metadata, resetting position
only for Background dimensions. Generic Atlas is not a source SurfaceAtlas.

Mask modification creates an RGBA8 surface. RGB masks use average RGB times
alpha; alpha-only inversion changes alpha. Gaussian feather uses the source
normalized coefficients, two quantized passes, empty outside sampling and its
0.00001 accumulators. Fractional extents which select a contributing weight
beyond the uploaded coefficient array diagnose an unobserved source uniform.
SurfaceAtlas combinations calling raw surface_get_width(struct), and dynamic
draws needing source shader/handle identity, remain named diagnostics.

Seventeen cases cover all25 arithmetic goldens, all dimension policies, fill and
clipping, mask-once behavior, alpha-only inversion, both single-channel paths,
missing foreground, Swap, inactive behavior, selected division errors, actual
graph repeat/atomic refusal, processor rows, whole-canvas work refusal,
SurfaceAtlas metadata and selected/downstream image agreement, HDR replacement and unobserved Gaussian weights.
The arithmetic goldens reuse the module's existing independent fixed byte
fixtures in PixelOpsBlend.cpp; they are not licensed desktop capture claims.

Both nodes admit scratch/output storage before allocation and bound the whole
processor batch to64million work units. The SurfaceAtlas preview projection requires the narrow existing-document
patch listed in the integration manifest. Draft compiler checks passed. Joined
Compile/Evaluate runtime verification is pending integration by the root agent.
