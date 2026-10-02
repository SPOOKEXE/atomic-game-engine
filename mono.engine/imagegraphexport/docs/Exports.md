# Shared graph exports

The module owns bounded filesystem publication, pure still and animation bytes,
and explicit external codec execution. Tools and Studio supply host grants.

`attribute_clear_directory=false` retains the exclusively created
`.graph-export-*` directory beside the granted output. Returned directory paths
identify the retained PNG frames. Existing directories are never reused.

The native GIF profile uses deterministic quality 0..3 channel quantization,
weighted palette reduction and binary alpha. Exact GameMaker palette, alpha,
loop and fractional-delay equality requires the named native encoder parity gate.

The live overload borrows an immutable Document, Plan and EvaluationRequest for
one synchronous call. The Input pathname supplies template context and is not
read. Missing preview bindings are appended to a bounded private document copy.
Single exports retain the exact signed fractional evaluation clock.

The native GIF delay profile floors centiseconds and clamps its minimum to one.
This reproducible profile does not establish GameMaker fractional narrowing.

Host providers are shared engine adapters. `GraphFileHost` binds each read or
write to an exact node, path and operation. GameMaker room dependency grants
also bind their resource key. `ExecuteGraphHostNode` resolves a live node's
controls using the supplied plan and request; its capture can then be retained
for replay. The host owns grant storage throughout these synchronous calls.

`GraphVideoHost` supplies bounded decoded frames through an explicit codec
executable. `GraphCommandHost` accepts literal process arguments and working
directories, exact HTTP requests, or explicit clock-node grants. `GraphDeviceHost`
uses recorded MIDI, Spout and calendar observations without opening devices.

Authored batches stage every output beside its destination before publishing.
The batch admits at most 512 MiB of candidate files and retained frames. Existing
files are backed up during publication and restored if a later publication
fails. Failed rendering leaves every destination unchanged. Failed batches
remove newly created empty parent directories and preserve the caller's retained
frame list. Sequence exports evaluate each planned frame; single exports retain
the live request's exact clock.

Still, image-array and animated-image reads admit actual PNG, BMP and baseline
JPEG dimensions and GIF frame ledgers before invoking the decoder. Each array pathname requires its
own exact named resource grant, including repeated paths in the ordered inputs.
The source `IPadding` getter rounds every component to even after unit conversion,
matching the [documented GameMaker round function](https://manual.gamemaker.io/lts/en/GameMaker_Language/GML_Reference/Maths_And_Numbers/Number_Functions/round.htm).
The CPU adapter does not depend on the process floating-point rounding mode.

The pinned image-array implementation uses plain sprite draws without setting
shader interpolation. The native CPU profile samples destination pixel centres
with nearest texels, supports scale-to-fit and fractional centered placement,
and leaves uncovered pixels transparent. This is not a recorded comparison
against GameMaker GPU edge coverage or texture-page sampling.

Animated-image selection follows the pinned `node_image_animated` update: the
exact request clock, `floor(clock / period) - (start - 1)`, project frame count
plus one for stretch, source signed remainder for Loop/Ping pong, and the Hold
and Hide bounds. Single-frame Ping pong uses source `safe_mod`'s zero-divisor
result. Negative source remainders do not wrap to the last frame. Its CPU sprite
profile is nearest sampling; GameMaker shader/GPU parity remains unverified.
Zero un-stretched speed is refused pending a division-by-zero source observation.
Missing or malformed granted images fail without replacing the prior capture.
File watcher refresh, embedded sprite-cache restoration and the timeline match
button remain host workflow parity gates. The source audit uses commit
`b69eca232217360cf1502ef0223523d818606652`, scripts `node_value_padding`,
`node_image_sequence`, `node_image_animated`, `safe_operation` and `shader_functions`.

GIF still input preflight accepts only GIF87a/GIF89a, scans bounded colour tables,
extension and image subblocks, canvas-contained descriptors, the frame count and
final trailer before decoding. It admits the full composited frame ledger and
fixed decoder scratch space against the caller's byte cap. `pc.image`, image
arrays and animated-image file rows publish only subimage zero, matching the
pinned sprite draw index; they do not turn GIF file subframes into extra rows.
Content policy checks both the supplied filename and the actual encoded image
format before decoder dispatch, including GIF bytes presented under a PNG name.
Still input normalizes only graphic-control delay fields in its owned encoded
copy, avoiding the sequence codec's float-clock ceiling without changing pixels,
disposal or the original file. Native GIF decoding uses the shared bake compositor. Exact equality with
GameMaker's GIF import and background handling remains an observation gate.

## Directory observations

`pc.directory_search` uses an exact absolute canonical root grant to perform a
bounded native directory observation. An authored path never grants access.
Assetc accepts repeated `--graph-directory-read NODE=ROOT` grants; selected
images also require `--graph-file-resource NODE:PATH=PATH` grants. Studio exposes
Grant directory beside its exact per-image read grants, with Read / refresh and
Revoke reads. Revoking the root also prevents reuse of a prior capture.

The shared provider records each directory's entry order once and retains it
across frames for the same node, root and recursion control. Its explicit
`Refresh` operation discards that node's recorded order. Assetc uses this provider
for its complete render range. Studio keeps
the captured ordered paths and decoded images until refresh or changed controls
or grants. Explicit `GraphDirectoryObservation` records can also be projected
without enumerating again. The observation is limited to 64 directories and
256 entries, with paths, traversal storage, captures and decoder work admitted
against the operation byte cap before growth. Symbolic links and non-regular
entries are refused.

Projection preserves the pinned source's stack traversal: non-recursive mode
still includes immediate child directories. Extension filter tokens retain
case, while observed filename extensions use ASCII lowercase. Image mode emits
PNG/JPG/JPEG/GIF surfaces and their ordered paths. Text mode preserves the
source's empty output arrays because its publication guard tests sprite existence.
This mode does not read otherwise unused text file contents. The authored mode
selects the compiled output type; changing it requires recompilation.

The recorded `std::filesystem` order is a native host profile. It does not prove
GameMaker `file_find` or `struct_get_names` ordering, hidden/system attributes,
or symbolic-link behavior. Admitted images that the
native decoder rejects are skipped with their paths, matching the source
`sprite_exists` publication guard. Grant, policy, dimension, byte-cap and native
codec-coverage failures reject the complete read and preserve the prior capture.
Each successful image keeps its own source dimensions. Ambient `file_checker`
polling is represented by explicit host refresh. Those source parity gates remain explicit.

Directory observation, capture, session and refresh use the existing Engine
profile scopes. Metrics distinguish actual enumeration operations and observed
entries from order replays. Raster counters report real stream bytes read and
actual decode attempts once at those boundaries. Directory counters report
skipped invalid images and emitted pixel payloads. The last-session gauge counts
owned logical observation bytes; it does not claim allocator heap residency.
