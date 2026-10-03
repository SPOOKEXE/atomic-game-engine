# Provided-byte font glyph decoding

`DecodeFontGlyphs` consumes exact caller-provided bytes and unique Unicode scalar
requests. It opens face zero, selects the Unicode charmap and applies an integer
pixel size of 1..512. Results preserve request order and own tightly packed
coverage. There is no file access, font discovery, shaping, kerning, fallback,
imagegraph dependency. The default raster profile produces coverage; the explicit
native distance profile below produces real SDF bytes.

Advances come from the loaded FreeType glyph, in pixels. Bitmap offsets are the
left bearing and negative top bearing relative to the baseline, using downward
GUI Y. Ascent, descent and line height come from the selected face size. Missing
characters have `Present=false`, index zero, zero metrics and empty coverage.
Present spaces retain their real advance and may have empty coverage. The caller
must explicitly request W, space or any other layout character it needs.

In the default Coverage profile, scalable outlines use native hinting and normal
or mono rendering selected by Antialias. The auto-hinter is disabled, avoiding a
second shaping allocator. Bitmap
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
overhead and call stacks. The default Coverage profile excludes the caller's
prior output; explicit SignedDistance includes its backing storage until
replacement. The allocation protocol uses
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
The default mode is a native FreeType CPU coverage profile. It does not establish
GameMaker
font_add, source SDF, whole-string measurement or licensed texture parity.

## Explicit native signed distance profile

`FontGlyphRequest::Raster = SignedDistance` calls the pinned FreeType
`FT_RENDER_MODE_SDF` renderer. Scalable outlines use `FT_LOAD_NO_HINTING`,
`FT_LOAD_NO_AUTOHINT` and `FT_LOAD_NO_BITMAP`. BDF/PCF strikes use the real
`bsdf` converter. Borrowed strike bitmaps are copied into slot-owned storage
through the same tracked allocator before conversion. `Antialias` affects only
the default Coverage profile.
`DistanceSpread` must be 2 through 32 pixels; its default is 8. Both SDF
renderer properties are explicitly assigned, without ambient font properties.

The existing `Coverage` vector carries raw unsigned distance bytes in this
profile. Value 128 is the contour, larger values are inside, and smaller values
are outside. Distance in pixels is `(byte - 128) * spread / 128`. These bytes
are copied without grayscale normalization, including saturated 255. The batch
records its raster mode and spread; default Coverage batches have spread zero.
Nonempty distance glyphs report the actual renderer's spread-sized border and
bitmap bearings. Empty/missing glyphs have padding zero. Advances and font line
metrics remain the actual FreeType values. An upper image adapter can store
white RGB with distance alpha and apply an explicit distance-to-coverage rule.
This module neither samples that image nor claims a source desktop SDF profile.

The custom FreeType allocator charges outline/bitmap SDF workspace before
allocation, including old plus new blocks during reallocation. SDF operation
admission additionally includes the caller's existing batch backing storage
until successful replacement. Coverage retains its existing independently
bounded result semantics. Both modes preserve the prior batch on failure.
`RetainedBytes` still describes only the replacement batch's vector capacities;
`PeakOperationBytes` describes all charged operation storage, including a prior
SDF destination. Allocator implementation overhead and call stacks are excluded.

Before distance rendering, each glyph admits its padded pixel extent. Outline
coordinates are bounded to plus/minus 4096 pixels, with at most 2048 points and
256 contours. Conservative pixel/segment visits across both render passes are
limited to 256 Mi visits per batch. The estimate uses the pinned conic
subdivision bound derived from the control box and the cubic subdivision bound
of 32. Bitmap distance transforms are admitted at 64 visits per padded pixel.
These are conservative workload units, not instruction counts or a time bound;
font parsing and interpreter internals remain non-preemptive vendor work.
Large or unusually complex glyphs can therefore be refused even when their
copied pixels would fit the 4 MiB output ceiling.

FreeType's outline SDF has documented limitations for self-intersections and
small features. Renderer failures are atomic decode failures, without a hidden
coverage fallback. This native profile does not reconstruct licensed source
spread, derivative smoothing, atlas packing or desktop runtime behavior.
HTML5 `font_enable_sdf` being a no-op does not establish those semantics.
The upstream [FreeType SDF property documentation](https://freetype.org/freetype2/docs/reference/ft2-properties.html#spread)
describes the distance encoding and distinct outline/bitmap renderers.
