# Source Generator Dimension Contract

The seven current source generators routed through `source2d::RunGenerator` and
with an authored Mask input are Checker, Quasicrystal, Wave Interference,
Zigzag, Box Pattern, Fold Noise, and Anisotropic Noise. Their pinned source
constructors create or inherit a `Dimension` value and apply the mask after
sizing the generated surface.

For `Dimension Unit = Mask`, Pixel Composer converts each authored dimension
component into mask pixels: width is multiplied by mask width and height by
mask height. A linked surface supplied to Dimension already has physical pixel
size, so the consumer's unit setting does not scale it and does not require a
Mask input.

Before generating a processor batch, the native route checks every original
Dimension and unit value against the largest available Mask extent. It uses
independent component minima and maxima and image maxima to bound every
selected row, checking both multiplication signs before any row output. This is
conservative: crossed extrema can refuse a batch whose individually selected
Dimension and Mask pairs would each fit. Each selected row still uses its exact
Dimension and Mask values for the output size.

The source mask operation preserves the generated surface's extent. Masking
changes samples after generation and does not replace the requested output
size with the Mask's physical extent.

The native evaluator also accepts a general `ArrayValue.Items` profile whose
Dimension rows can mix scalar and vector leaves. Scalars read through the native
`Vec2` accessor expand to equal components, so preflight bounds both axes for
that native payload shape. This does not establish that Pixel Composer's GML
Dimension getter emits the same heterogeneous batch shape.
