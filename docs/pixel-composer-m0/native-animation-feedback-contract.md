# Native animation and feedback contracts

The native audio key driver and native captured-image feedback are explicit engine extensions. They are not implementations of an undocumented Pixel Composer audio key driver or its source Feedback collection. The pinned `node_keyframe_driver.gml` contains no audio driver.

## Captured audio key driver

`KeyframeAudioDriver` stores a capture source ID, channel, named metric (`rms`, `peak`, or `mean`), gain and bias. Native persistence writes the named `native_audio` driver record. The offset is `metric * gain + bias`, added to the interpolated numeric key components. Integer components retain the existing final half-even rounding. Silence and empty sample windows have metric zero.

The driver reads an explicit `AudioCaptureFrame` at the exact unsigned request tick. Fractional timeline sampling does not interpolate recorded capture windows. Missing sources, missing ticks, absent channels, duplicate frame identities, malformed samples or a nonfinite result are diagnosed. Capture sample rate and planar-channel validation remain the ordinary recorded-input contract. No device, wall clock or filesystem is opened by evaluation.

Native audio keys use the authored continuous fractional key schedule. This does not establish Pixel Composer's fractional `key_map` array coercion. Existing source-map uncertainty is retained in `fractional-key-map-evidence-2026-09-30.json`.

Studio exposes the captured-audio controls in the key driver popup. The runner can evaluate the same saved driver with its existing recorded audio capture input.

## Native image feedback replay

A feedback binding maps one native `image.captured` source ID to one declared graph output. Reset evaluates frame zero against explicit seed images. Each later fixed tick reads every bound source from the preceding generation. All bound outputs commit together only after every output succeeds. Missing outputs, duplicate source IDs, changed bindings, authoring revision changes, skipped ticks and fractional requests refuse atomically.

`SeekFeedbackReplay` rebuilds from seeds through the selected target, including every intervening tick. It accepts an explicit work bound of at most 4096 evaluated frames. A seek outside that bound is diagnosed. Callers can instead stream contiguous steps using `ReplayFeedbackFrame`. Numeric surface formats remain attached to retained images; precision is not silently reduced to RGBA8 during replay.

Replay admission includes the prior owner, seed payloads, destination owner, candidate images, copied binding metadata and evaluator workspace. The image evaluator's bounded overload shares its ordinary logical allocation ledger.

Studio, the client and the headless runner use the shared `CapturedFeedbackHost`. Bound feedback outputs and the selected output use one compiled union closure per tick, so shared simulation, random and cached processors execute once. Its byte cap includes retained outputs, replay owners and copied captures during replacement. Seek gaps reconstruct intervening ticks; ordinary consecutive playback retains the existing owners.

Studio persists a native binding through the captured node's source ID, `feedback:<output-id>`. Its output panel can set this through the ordinary document edit and undo path. Studio starts with transparent RGBA8 seeds at the authored project size and exposes a reset action. A changed document or recorded audio input rebuilds replay. Repeated previews of the same frame reuse that generation rather than feeding it into itself again.

## Source temporal processor replay

`EvaluateStateful`, `EvaluateStatefulOutputs` and the shared host return simulation, interlace-surface, random and source data replay owners with the selected outputs. A failed output, malformed prior owner or exhausted byte budget preserves the previous destination. Processor rows retain separate identities. Mesh consumers resolve the current constructor-owned simulation snapshot, preserving source mesh aliases across downstream affectors.

The source `pc.interlaced` cache retains its original input surface for each node, processor row and integer frame. It does not retain the masked or mixed output as the next input. Missing delayed frames sample transparent black. Axis, inversion and loop wrapping follow the pinned shader. The pinned node passes Mask feather metadata to its Size map shader setup; the native executor preserves that disabled-map behavior rather than correcting the source node silently.

Source interlace caches survive document edits and backward seeks because the pinned source uses `clearCacheOnChange = false`. Resetting the host explicitly discards them. Physics and random owners reconstruct from frame zero on seeks. Manual Cache Mesh control captures survive a seek or selection change while their authored node and authoring revision remain valid. Random-only closures accept fractional sampling and match recorded entropy by exact node, processor row, tick and subframe. Nondeterministic clock and reshuffle seeds require explicit captured entropy. Evaluation does not read a machine clock or manufacture a seed.

Source Differential and Boolean Trigger keep constructor-owned history per processor row. Differential uses the exact signed sampled frame: backward sampling computes a negative frame delta, while a zero frame delta returns zero and still stores the new value. These two families retain history across input and control edits. A closure containing only these data histories evaluates the requested sample directly, including fractions and signed rewinds; it does not invent intervening evaluations. Explicit host reset clears the histories.

Manual Cache Mesh actions identify durable node IDs and execute at the requested sample only. Their dependencies join the selected closure even when the cache branch is disconnected from its image output. Intermediate native feedback seeks do not repeat the action. A same-frame action reuses the current pose and does not apply physics or affectors twice. Renderer input snapshots and named feedback outputs share that transaction without running an unsupported CPU renderer kernel.

The source random helper is checked against the pinned official GameMaker HTML5 runtime. This establishes the inspected HTML5 behavior; equivalent desktop runtime output has not been verified.

## Remaining source parity gates

These native contracts do not complete source Feedback collection expansion, inline feedback node import, group feedback cache semantics or external executable comparison. Those need their own source-backed compiler, interchange and acceptance paths. GameMaker fractional bracket read/write and array-size coercion remain unverified for the pinned source runtime.

## Native GIF byte profile

`bake::WriteGif` accepts borrowed equal-size RGBA8 frames with explicit nonzero centisecond delays. It emits independent full frames, binary alpha (below 128 transparent), per-frame palettes, and a Netscape loop count (zero repeats indefinitely). Quality 0 through 3 retains 2, 4, 6, or 8 channel bits before deterministic weighted median palette reduction to 255 opaque colours. Quality 3 preserves every opaque colour when the frame has at most 255 distinct colours. Partial alpha cannot be preserved in GIF.

The encoder admits candidate output and peak workspace before allocation, bounds the sequence to 4096 frames and 16 million total pixels, and publishes bytes only on success. Literal LZW codes reset the dictionary every 200 pixels, keeping code width fixed and output bounded. This profile prioritizes deterministic bytes over compression ratio. The byte cap excludes borrowed source frames and the caller's old output.

This is an explicit native CPU profile. GameMaker's opaque quantizer, fractional delay narrowing, black canvas compositing, and loop behavior remain separate source parity gates. The source `gif_open` third argument is a clear colour, not a loop count. GIF block layout and disposal follow the [GIF89a specification](https://giflib.sourceforge.net/gifstandard/GIF89a.html).
