# Path Blur source contract

`pc.blur_path` implements the GML and GLSL formulas from Pixel Composer commit
`b69eca232217360cf1502ef0223523d818606652` as a bounded CPU reference.

| Pinned file | SHA256 |
| --- | --- |
| `scripts/node_blur_path/node_blur_path.gml` | `51fbf4e9606ba3903ca92832162460a07c508d6e7b7c6ae2b8d13f77560f2498` |
| `shaders/sh_blur_path/sh_blur_path.fsh` | `2ffcb9bb2413f52ba7719724b5f1da5d024d96531adf9ead6091fc4a56afe228` |

## Default identity and producer routes

The source path getter defaults to `noone`. That default clears a fresh output
surface before shader controls are read. A linked or explicitly authored empty
path is an owned path object and executes the selected blur mode.

The evaluator marks only an input that actually selects a catalogue fallback.
This private provenance follows the existing effective instance owner, timeline,
replay and group resolution. Empty path contents do not identify a default.
Successful PCX input replacement clears the fallback mark. Processor rows retain
the same context; owned Pixel Builder recipes rebuild provenance from their
copied document and replay during each evaluation. No borrowed route survives in
a durable recipe.

Private `InputProducer` borrows the compile plan's effective links and checks the
consumer and its resolved input owner. It reports the actual producer node/port,
including an empty output. It does not infer producer identity from the first
output element or invent a producer row from the consumer row.

## Kernel controls

Source integer resolution is rounded with the native half-even getter profile,
then clamped to 2 through 128. Path origin selects the reference point. Range
ratios are clamped to `[0,0.99]`; selected path geometry uses the existing owned
path sampler. Inverted order reverses the tap indices. Tap progress is
`index/resolution`, including the source's final value below one.

Each sample subtracts the path offset, applies scale and rotation around Anchor,
and applies UV influence at tap progress. The source simple sampler uses hardware
nearest/bilinear filtering. Blur mode multiplies samples by Intensity and the
Intensity Curve, accumulates RGBA, divides RGB by accumulated weighted alpha and
alpha by resolution. The curve is consumed even when Intensity Curved is false,
as the source shader upload enables it independently.

Blend mode applies the selected gradient and composites taps in order. It ignores
Intensity and Intensity Curve. Transparent source samples are skipped. Scale
Curve is consumed in both modes. Curves admit at most nine anchors with a nonzero
header scale; the gradient admits at most 64 keys.

Inactive execution copies the input. After default identity is resolved,
single-channel R8/R16/R32 inputs use source-safe greyscale drawing. Normal execution
retains source masks, mix, inversion, feather, channel selection and output depth.
Zero scale, undefined accumulation divisors, nonfinite transforms and
unrepresentable shader uniforms produce explicit diagnostics.

## Bounds and verification

The executor admits at most 64 million estimated sample/curve work units across
the complete processor batch before allocation. Its 128 path offsets use fixed
stack storage. Path runtime scratch and owned output use the evaluation byte
ledger. Both new provenance vectors are admitted before reserving their bounded
input slots.

Uniform Blur input `[64,128,192,128]` gives `[128,255,255,128]`; Intensity 0.5 keeps
that RGB and gives alpha 64. Ordered half-alpha red/green gradient taps give
`[170,85,0,192]`, or `[213,42,0,192]` when inverted. These are source-formula
arithmetic expectations, not captured GPU output.

The compiler publishes inherited input links and junction defaults as instance
routes before constructing the DAG. Local links take priority, including links
on intermediate instances. Explicit instance overrides stop inheritance. Clones
are admitted before allocation; derived routes share the native 8192-link bound
and a 64-million lookup-work bound which includes override-list scans. Ordinary
producer ordering and consumer retention then cover inherited inputs.

Twenty-five cases cover kernel controls, graph publication, byte/work refusal,
instances, local and intermediate linked inputs, explicit overrides, junction
ownership, atomic derived-route refusal, timeline keys, group replay and Pixel
Builder rerender. The focused linked native repair probe passed 172 assertions
in all 25 cases. Strict compiler checks passed. Joined runtime tests remain pending.
No separate request parameter-override API is claimed.
Native double trig and CPU texture sampling define this profile; exact desktop
getter tie behavior, shader float arithmetic and licensed GPU coverage remain
external verification gates. No GPU or profiling run was performed for this chunk.
