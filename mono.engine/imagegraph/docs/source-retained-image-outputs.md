# Crop Content and retained processor outputs

Pinned Pixel Composer revision: `b69eca232217360cf1502ef0223523d818606652`.

This is a bounded CPU reference profile. It creates owned native images and typed Atlas metadata. It does not claim source GPU readback or ambient GPU sampler parity.

## Observable behavior

Crop Content processes the complete flat input list itself, rather than applying the generic processor row loop. An empty flat Atlas list produces empty arrays. Atlas list shape is validated before element iteration, so an empty Elements vector cannot conceal nested or general items. Active processing supports independent bounds or a shared union. Padding is right, top, left, bottom and uses half-even integer getter rounding. Background comparison uses the original full RGBA sample; bounding readback tests the rounded RGBA8 alpha. Background affects bounds, while pixels inside the resulting crop remain intact.

Inputs larger than64 pixels pass through the source's sixteen-tap RGBA8 downsample shortcut. This profile uses point sampling, clamp addressing and pixel-center quad coverage. Every stage sums taps without averaging and rounds into RGBA8. An empty final stage keeps the initialized1x1 extent; a small empty image has0x0 bounds. Allocation clamps either result to at least1 pixel. A following cell's positive taps can still reach an odd-size source edge even when the final destination quad cell is uncovered.

Active output depth uses the explicit source Color Depth attribute, including Input and Inherited. SurfaceAtlas placement adds the bounds origin to an incoming SurfaceAtlas position. Base Atlas position does not participate in that source class branch. Atlas outputs own their pixels, dimensions, original dimensions and source class. Selecting a declared Atlas batch output preserves that typed value; image consumers may still project its surface.

Inactive Crop updates only the image output, using default RGBA8 conversion. It retains the complete previous Crop Distance and Atlas values, including previous array shape and count. A fresh inactive node uses the source constructor's zero distance and empty Atlas list. Inactive Smear clones its current color input and retains its whole previous depth image or image-array shape before generic row selection. An active undefined Smear depth stores an Undefined marker, invalidating older defined depth. It remains a named output refusal and never becomes fabricated depth pixels.

## Ownership and admission

The existing caller-owned DataReplay ledger contains one latest Struct record per authored node, row0. Crop stores named `crop_distance` and `atlas` fields. Smear stores `depth_pass`. No borrowed pixel pointer survives evaluation. Ordinary stateless evaluation does not create a retained owner; use EvaluateStateful, EvaluateStatefulOutputs or CapturedFeedbackHost to publish the journal alongside outputs.

Stateful evaluation already charges existing publication, caller prior journals and their copied candidate together. The producer additionally admits its staging buffers, current image and Atlas outputs, transient metadata copies, complete new journal entry, field strings and backing pixels before their corresponding allocations. A replacement cannot silently discard old residency from its byte cap. Refusal leaves the published output and all prior journals unchanged.

The existing typed carrier policy also bounds each complete retained Struct record to4MiB and4096 logical nodes, including field metadata and owned pixels. The producer checks this aggregate before cloning the record. No image or list is truncated to fit. Output surfaces retain the existing4096-axis and64MiB per-surface bounds; whole evaluation residency retains the128MiB ceiling. These are different limits.

Crop admits at most64million combined pixel/tap visits for the complete input list before staging or image output allocation. Union sizing uses the largest original width and height across the list. All final sizes and depth layouts are checked before allocating the first output. Smear's approved active preflight and coordinate guards are unchanged from its separately frozen producer.

## Time and malformed state

The temporal cone treats both nodes as Data processors requiring frame-zero prefix replay. CapturedFeedbackHost rebuilds nonnegative fixed-frame seeks, resets on authored/input revision changes, and preserves its atomic frame-start checkpoint. Tests compare seek12, backseek0 and cleared-owner seek12. The retained record validates its exact tick, signed fractional clock, field count and output shape; a future record or valid-but-wrong typed field is refused.

The host's reconstruction uses its existing fixed-tick prefix and final requested subframe. It does not reproduce an arbitrary historical sequence of fractional UI observations inside one tick. Direct caller-owned state supports chronological fractional observations and rejects future fractional or signed prior state. Source GPU filter inheritance and historical fractional observation parity remain explicit acceptance limits.

## Evidence

Source files and SHA256:

- `scripts/node_crop_content/node_crop_content.gml`: `de92c3e2d42a9f0051647916ca7f8348f1fe7026e4ff92b801a3e2ec2e1ce37e`
- `scripts/surface_functions/surface_functions.gml`: `d27420ab408d66568062005e91b79dfbfb2a2e2f044441417959803b2ea1bfb2`
- `shaders/sh_crop_conent_downsample/sh_crop_conent_downsample.fsh`: `d6d104659a59e6b2044e042481db190dd663d7d64fdaf8191769393930f792fe`
- `shaders/sh_crop_content_replace_color/sh_crop_content_replace_color.fsh`: `930602f0dfe51d00879776d440b342fb744be689e6e6f6a312997ed4b80926b6`

Additional read source: `scripts/__surface/__surface.gml`, `scripts/node_value_padding/node_value_padding.gml`, `scripts/paddingBox/paddingBox.gml`, `scripts/shader_functions/shader_functions.gml`, and `scripts/_draw_defines/_draw_defines.gml`. Smear processor skip provenance remains documented in the separately frozen source-smear profile.
