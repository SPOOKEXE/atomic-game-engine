# source JPEG

grug native `pc.jpeg` follows the node and two shaders at Pixel Composer source revision `b69eca232217360cf1502ef0223523d818606652`. this executor is a block transform effect, not a JPEG file encoder. licensed runtime captures remain unverified.

## defined paths

Cosine, Zigzag, and SmoothZigzag use the pinned shader bases and phase. phase converts authored degrees to radians before float upload; shader PI remains `3.1415972`. sample reads use nearest filtering with edge clamping, including blocks that extend beyond the source.

the first pass transforms each block into RGBA16Float coefficients. optional compression uses the source rounding rule: fractional parts greater than one half round up; ties round down, including negative ties. reconstruction reads those half-float coefficients and writes another RGBA16Float surface before copying into the requested output format. each pass forces alpha to one.

`deconstruct_only` skips the first pass and reconstructs directly from the original surface. despite the source label, it does not publish the DCT coefficients. `reconstruct_all` chooses Patch Size as the reconstruction count; otherwise Reconstruction may exceed Patch Size and sample adjoining blocks. Patch Size clamps to at least one and Reconstruction to at least zero, matching ordinary source value validators.

Step reads uninitialized basis variables in both pinned shaders. native execution reports `UnsupportedExecution` at `transformation` rather than inventing stable pixels for that undefined path.

## processor controls and limits

main SurfaceAtlas input unwraps to its backing surface. the raw source mask binding cannot bind an Atlas; native execution refuses it at `mask`. mask feather, inversion, alpha-only sampling, mix, channel selection, output depth, arrays, and animated controls use the shared processor paths. inactive processing copies the source unchanged.

shader counters must fit signed 32-bit uniforms and numeric controls must fit finite float uploads. the native path refuses unrepresentable intermediate half-float samples with a named diagnostic. this bound is deliberate; no claim is made about source-driver infinity handling.

all selected rows are admitted against a cumulative 64 million work-unit limit before the first row executes. both transform loops and mask feather contribute to that quote. actual scratch payload is charged to the evaluation workspace; skipped passes allocate no scratch. these guards bound native CPU execution without changing the mathematical loop count for admitted inputs.

## function logic

prepare validates the main surface, authored controls, raw mask, and source counter minima. quote counts both selected transform loops and shared mask work without allocating output. draw samples the admitted inputs in shader loop order, quantizes each completed pass to half, copies into the requested surface format, and applies shared processor finish against the original source. whole-batch admission repeats prepare and quote for every selected row before invoking the executor or its observer.

source paths: `scripts/node_jpeg/node_jpeg.gml`, `shaders/sh_jpeg_dct/sh_jpeg_dct.fsh`, and `shaders/sh_jpeg_recons/sh_jpeg_recons.fsh`. counter behavior also depends on `node_value_int`, `node_value_validators`, and `node_value_output`; mask binding depends on the shared shader and mask helpers.
