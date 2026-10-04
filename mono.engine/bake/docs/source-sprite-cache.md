# Source sprite-cache byte profile

Pinned source revision: b69eca232217360cf1502ef0223523d818606652.
`sprite_add_functions.gml` serializes a sprite as a width/height/buffer object
and a sprite list as an array of those objects. Buffer text is base64 of a
zlib stream. The cache codec accepts one sprite or a flat ordered list.
Nested sprite trees retain their source text and receive an unsupported shape
diagnostic instead of an invented flattening.

The normal source imported surface format has four bytes per pixel. Channel
order and row orientation require an explicit observation. The official
[buffer_get_surface documentation](https://manual.gamemaker.io/monthly/en/GameMaker_Language/GML_Reference/Buffers/buffer_get_surface.htm)
states that the same surface format can have different platform storage.
Supported names are rgba8-top-down, bgra8-top-down, rgba8-bottom-up and
bgra8-bottom-up. Native encoding chooses rgba8-top-down. This profile does not
claim universal source renderer byte parity or filtering parity.

The primitive frame DTO contains only dimensions and owned RGBA bytes. Caps
are 256 frames, dimensions at most 16384, 16 million total pixels and 1 MiB of
encoded text. Decoding checks the exact expanded byte count, zlib checksum,
base64 syntax and trailing data. Failure preserves the prior output. A fixed
BLAKE3 digest identifies exact bounded text, including its spelling.

Native capacities, prior output and a fixed codec-workspace allowance are
admitted inside maximumBytes. Borrowed input, vendor JSON/codec allocations
and allocator bookkeeping are separate from this logical owned-payload quota.
The text/depth/frame/pixel limits bound those parser and codec operations; the
API does not claim a full-process heap cap. Per-frame scratch admission is
cumulative and conservative: capacity reserved for a completed frame is not
credited back when its temporary buffers are released. This bounds the full
operation's admitted reservations rather than promising maximumBytes equals
peak live scratch. A long frame list may therefore refuse even when its
individual frames would fit separately. Codec scopes report processed frame
and byte counts at their actual boundary.

Independent literal zlib/base64 vectors verify channel order, alpha, row order
and mixed dimensions. These are native headless acceptance fixtures, with
joined runtime validation reported separately from strict compilation.
