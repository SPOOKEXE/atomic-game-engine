# Source Displace

`pc.displace` follows `Node_Displace` and `sh_displace` at Pixel Composer commit `b69eca232217360cf1502ef0223523d818606652`. grug check source equations and native graphs. licensed runtime pixels remain unverified.

Linear uses alpha-weighted luma minus Mid Value along Position in pixels. Vector uses red and green directly, or two alpha-weighted luma maps with Separate Axis. Angle uses red as a full-turn angle and green as distance, or separate luma maps. Gradient uses neighboring mean RGB, main-surface texel steps and row-vector Angle Offset rotation. Radial rotates around Mid Point; Zoom scales around it. Mode index 4 is a source separator and refuses explicitly.

Position and Mid Point Reference units scale authored values in host double before float shader upload. all selected rows share main surface row zero as reference, as the source vector getter does. linked vectors already carry pixels. native transient reference state restores after success or refusal.

Strength and Mid Value retain selected mapped endpoints, scalar replication, range defaults and low endpoint fallback without a map. auxiliary stages are nearest, while displacement and Strength map reads still run `texture2Dintp` using main-surface dimensions. Mid Value uses plain nearest reads. Strength curves use the source GLSL layout and float evaluator. curves apply to Linear, separated Vector, Angle, Radial and Zoom; unseparated Vector and Gradient ignore curves.

UV remaps inside every displacement step, including Reposition steps. flipped green supplies Y; UV alpha is ignored. Oversample attribute controls main sampling. the separate Oversample Mode input is unused by source. the standard sampler region handles inherited CleanEdge selector 6 as a plain nearest texture read rather than the extended CleanEdge shader.

Iterate advances strength from one divided step through full strength. Reposition uses the last coordinate; otherwise each step starts at the original UV. Stop Empty stops before blending an empty sample, or returns transparent black immediately for an empty original. Fade Distance scales RGB by remaining distance. Overwrite, luma Min, luma Max and Mix blend every iterative sample, then blend once more against the original pixel. nonpositive Iteration runs no steps. Repeat clamps to one or more passes.

both source scratch surfaces default to RGBA8. input pixels clamp and quantize before the first pass; each repeat clamps and quantizes again. cleared scratch targets preserve shader RGB without premultiplication under the source alpha blend. final scratch pixels convert to requested output depth, then Mask, feather, Mix and Channel apply against the original typed image. missing required displacement maps copy the original into requested depth and bypass processor finish. inactive copies retain original depth and bytes.

main Atlas uses the source safe draw and unwraps backing pixels. raw auxiliary Atlas bindings refuse with named ports. source GPU filtering and format conversions remain unverified against licensed captures.

## bounded execution

`PrepareDisplace` validates selected surfaces, switches, units, mapped endpoints, float controls, format and curve layout. `DisplaceShift` computes one source coordinate step, including UV remap, and refuses nonfinite results. `QuoteDisplace` sums conservative sampler, curve, iteration, repeat and feather work over the complete selected batch, capped at 64 million units; it checks the full coordinate path before output allocation, conservatively independent of later empty-pixel stops.

`DisplaceShade` samples one scratch pixel, runs its iterative steps and final blend. `DrawSourceDisplace` charges both RGBA8 scratch images to the evaluation ledger, writes each pass into the opposite image, converts the final pixels into the charged output and applies processor finish. no files, clock, GPU state or host pointers enter durable graph data. `ENGINE_PROFILE` marks the whole executor; verification timings are not a performance claim.
