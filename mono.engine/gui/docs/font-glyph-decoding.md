# Provided-byte font glyph decoding

`DecodeFontGlyphs` consumes exact caller-provided bytes and unique Unicode scalar
requests. It opens face zero, selects the Unicode charmap and applies an integer
pixel size of 1..512. Results preserve request order and own tightly packed
coverage. There is no file access, font discovery, shaping, kerning, fallback,
SDF conversion or imagegraph dependency.

Advances come from the loaded FreeType glyph, in pixels. Bitmap offsets are the
left bearing and negative top bearing relative to the baseline, using downward
GUI Y. Ascent, descent and line height come from the selected face size. Missing
characters have `Present=false`, index zero, zero metrics and empty coverage.
Present spaces retain their real advance and may have empty coverage. The caller
must explicitly request W, space or any other layout character it needs.

Scalable outlines use native hinting and normal or mono rendering selected by
Antialias. The auto-hinter is disabled, avoiding a second shaping allocator. Bitmap
BDF/PCF strikes retain their real mono/gray coverage; Antialias does not invent
smoothing for a fixed strike. Gray samples normalize their declared level count
to bytes. Mono bits unpack most significant bit first. Signed pitch and padding
are converted to tightly packed top-down rows. Unsupported coverage modes and
invalid row layouts refuse the operation. These contracts follow the
[FreeType glyph retrieval API](https://freetype.org/freetype2/docs/reference/ft2-glyph_retrieval.html)
and [bitmap fields](https://freetype.org/freetype2/docs/reference/ft2-basic_types.html#ft_bitmap).

Each request creates a private FreeType library. Its custom memory callbacks
account live allocations, aligned block headers and both blocks during growth.
The same operation ledger admits supplied font/request bytes, bounded validation
scratch and all candidate vector capacities. Font bytes are borrowed for the
call; copied glyphs remain valid after the input bytes are released. At most
4096 characters and 16 MiB font bytes are admitted. Owned glyph storage is
limited to 4 MiB. The caller can lower the operation limit from its 64 MiB cap.

A first glyph pass counts all copied coverage before output allocation. The
second pass checks that count while copying. Actual vector capacity above the
requested size is charged before publication. `RetainedBytes` reports owned
capacity, while `PeakOperationBytes` reports the maximum admitted operation
storage. These are logical payload counts. They exclude allocator implementation
overhead, call stacks and the caller's prior output, which the caller already
owns. The allocation protocol uses
[FreeType's custom memory interface](https://freetype.org/freetype2/docs/reference/ft2-system_interface.html#ft_memory).

The pinned FreeType WOFF2 path calls Brotli without the custom allocator, so
WOFF2 magic is refused before face creation. Embedded PNG/color bitmap decoding
also uses an external allocator. Scalable fonts therefore use `FT_LOAD_NO_BITMAP`;
non-scalable faces are admitted only as BDF/PCF. Ordinary supplied TrueType and
OpenType outlines remain supported. Color-only bitmap fonts require another
explicitly bounded decoder or source observations, rather than fabricated gray
glyphs.

FreeType releases its borrowed input before replacing output, including when
input storage belongs to the prior output. Every refusal, including allocation
failure, preserves the caller's prior batch.
A successful missing-glyph result is distinct from a font or decoder failure.
This is a native FreeType CPU coverage profile. It does not establish GameMaker
font_add, source SDF, whole-string measurement or licensed texture parity.
