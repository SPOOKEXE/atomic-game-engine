# Surface and PXCS buffer conversion

`pc.surface_to_buffer` reads `surface` and publishes `buffer`.
`pc.surface_from_buffer` reads the actual unnamed source input `input_0` and
publishes `surface`. Both inherit Node_Processor. Their only evaluator controls
are `attribute_process` and the four `attribute_array_process` schedules. They
have no precision selector, header option, byte-order control or interpolation
control. Disabling processing uses the source scalar update path; it still
converts a scalar and refuses a whole array operand.

## Pinned source

Pixel Composer revision `b69eca232217360cf1502ef0223523d818606652`:

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| `scripts/node_surface_to_buffer/node_surface_to_buffer.gml` | 382 | `ad816f7ee340f1f3aeaa502027a6d2e3572d626f75b38fc094f37debeca2a0aa` |
| `scripts/node_surface_from_buffer/node_surface_from_buffer.gml` | 420 | `32c526c24731f2e4ad466fc39687a6f9c2583c43c2288fd25516284748595166` |
| `scripts/buffer_functions/buffer_functions.gml` | 5615 | `f72271252e759125ead67b59e38ff0f5e8735ffd74a53ac04b299a138db0ca4f` |
| `scripts/surface_functions/surface_functions.gml` | 37782 | `d27420ab408d66568062005e91b79dfbfb2a2e2f044441417959803b2ea1bfb2` |

The encoder calls `buffer_from_surface` with default header enabled, fixed
buffer kind and alignment one. The decoder calls `surface_from_buffer`.
The 24-byte source header is four ASCII bytes `PXCS`, unsigned little-endian
16-bit width, unsigned little-endian 16-bit height and one raw surface-format
byte. Bytes 9 through 23 are reserved, and pixels start at offset 24.
The source never writes the reserved bytes. It never validates their contents
or bytes after the expected pixel payload.

## Bounded native byte profile

The raw IDs below come from the inspected official GameMaker HTML5 revision
`60e51be51ce7f3d52025ef18106cf172b8e22a00`, not the Pixel Composer precision-menu
indices. [Texture constants](https://github.com/YoYoGames/GameMaker-HTML5/blob/60e51be51ce7f3d52025ef18106cf172b8e22a00/scripts/Globals.js#L806)
and [format names](https://github.com/YoYoGames/GameMaker-HTML5/blob/60e51be51ce7f3d52025ef18106cf172b8e22a00/scripts/libWebGL/libWebGL.js#L1309)
identify the seven existing native layouts:

| Raw ID | Native format | Pixel bytes |
| ---: | --- | ---: |
| 6 | RGBA8Unorm | 4 |
| 9 | R16Float | 2 |
| 10 | R32Float | 4 |
| 11 | RGBA4Unorm | 2 |
| 12 | R8Unorm | 1 |
| 14 | RGBA16Float | 8 |
| 15 | RGBA32Float | 16 |

The native encoder zeroes reserved bytes, writes header integers in little
endian and copies the existing native Image byte payload without conversion,
resampling or alpha changes. The decoder ignores reserved and trailing bytes,
requires a complete payload, copies it into separately owned native Image
storage and computes its native surface hash. Thus all seven formats roundtrip
exact native bytes, including signed finite floating samples.

The official [buffer implementation](https://github.com/YoYoGames/GameMaker-HTML5/blob/60e51be51ce7f3d52025ef18106cf172b8e22a00/scripts/yyBuffer.js)
uses a zero-initialized ArrayBuffer, little-endian u16 writes and no terminating
zero for buffer_text. This supports the chosen canonical header profile; it
does not establish initialization behavior for every native GameMaker runner.

## Limits and failures

Buffers retain the existing 4 MiB carrier cap. Encoded pixels must fit that cap
with the 24-byte header. Dimensions must be positive, fit u16, respect the
request dimension cap and yield a checked pixel layout. Floating samples must
be finite. Before first output allocation, the executor surveys borrowed
original value arrays or complete image arrays, including expensive later
rows. Bounded traversal admits at most 4,096 source array elements and the
existing maximum depth. A 64 MiB byte-work cap admits the surveyed source bytes
plus three times the largest row bytes times the complete processor row count,
using division checks before multiplication. The three row passes conservatively
cover sample validation, allocation initialization and copy. Source schedules
remain owned by the common processor, which admits output arrays and commits
candidate results only on success. Images and BufferValues use the shared
allocation ledger before growth. Meaningful profile scopes are
`imagegraph.surface_to_buffer` and `imagegraph.surface_from_buffer`.

Invalid magic, short header, zero dimensions, absent handles and unsupported
source formats have no represented source surface or buffer result, so they
produce named UnsupportedExecution diagnostics. Truncated transfers and
nonfinite samples also refuse explicitly. Size, dimension, work and allocation
limits produce LimitExceeded. Failure leaves caller outputs unchanged. RG8,
depth, compressed and unknown format IDs gain no invented mapping.

Graphs, links, controls and upstream animation roundtrip through the native
document codec. Runtime buffers and surfaces remain owned observations and are
not inserted as authored document literals. Tests supply arbitrary incoming
buffers through the existing exact recorded Byte File In host route, without
filesystem access, and test conversion into actual downstream buffer text.

The current native Array scalar collector refuses Image/Scalar and
Buffer/Scalar mixtures before conversion. A linked heterogeneous JSON array
selects its existing general source-array collector instead. Tests independently
verify that this path publishes Any.Items with the original image or buffer
and nested invalid leaves. The public image resolver refuses the generic
image/value array at the named encode input before invoking its kernel; the
buffer decoder reaches its own invalid-leaf preflight. An additional borrowed
encoder context uses that actual producer payload plus its selected first image
to verify the kernel's bounded invalid-leaf preflight and empty output storage.
The original direct scalar constructions separately check upstream Array policy
diagnostics and unchanged caller output.

## Reference gates

This is a device-independent native byte profile. GameMaker transfer packing,
row orientation, RGBA4 nibble order and floating texture byte layouts can vary
by runner or graphics backend. The official
[buffer_get_surface documentation](https://manual.gamemaker.io/monthly/en/GameMaker_Language/GML_Reference/Buffers/buffer_get_surface.htm)
and [buffer_set_surface documentation](https://manual.gamemaker.io/monthly/en/GameMaker_Language/GML_Reference/Buffers/buffer_set_surface.htm)
warn about format/platform differences. The inspected old HTML5 transfer
implementation also retains a five-argument API and does not apply its offset
as the pinned wrapper expects. It is not a transfer-byte oracle for this
three-argument PXCS source contract.

Licensed runner reserved-byte initialization and GPU transfer bytes remain
unverified. The source decoder can create a surface then attempt a truncated
transfer, leaving pixels outside this native finite profile. Source mutable
buffer cursors and handle aliases are also unrepresented: native decoding
reads immutable bytes and does not expose the source buffer_seek cursor change.
No executable, cross-platform GPU or mutable-handle parity claim is made.

Joined release66 CPU checks pass all 20 conversion cases after fixture repairs that distinguish upstream Array policy, public image resolution and native kernel preflight. This proves the bounded native byte profile; the reference gates above remain open.
