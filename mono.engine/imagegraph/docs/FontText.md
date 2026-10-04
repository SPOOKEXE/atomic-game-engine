# Font Data, Bitmap Font and Text

These CPU routes use owned `FontValue` payloads. They never search installed
fonts, resolve process-global font identifiers, or retain a host callback in a
snapshot or Pixel Builder recipe. Runtime fonts cannot be authored as document
literals. Native documents containing font literals fail serialization rather
than silently losing frames or converting handles into strings.

## Inputs and observations

Font Data resolves observed aliases and `%DIR%/`, `%APP%/`, project-relative
paths. Owned fonts forward directly. With processing disabled, root arrays
resolve string leaves and preserve other bounded leaves and nested lists. Raw
numeric GameMaker font handles have no represented native identity and refuse.
The source's editor preview SDF side effect is not represented by this pure
output route. Bitmap Font consumes a complete surface list, makes
owned frames, uses UTF-16 string-map positions, keeps duplicate-last mappings,
and preserves the source separation advance for missing frames. Heterogeneous
frames use the first frame's canvas with nearest pixel-center CPU resampling.
An empty valid-frame list refuses: the source deletes its previous resources
before returning, so retaining a live native font would invent handle lifetime.

Text accepts observed file paths or an owned font. File observations are keyed
by the authored node, selected processor row, signed clock, path, size,
antialiasing, SDF flag, requested glyphs and exact source context. A matching
record suppresses provider work. Observed absent files retain the previous
file font; unavailable file evidence is a refusal. Observed playback skips
file-font regeneration. Source default-font, path prefixes and playback are
explicit request facts. A bitmap font does not overwrite the retained file
font. Fallback selection follows the source's whole-text missing-glyph test.

ASCII case conversion is native. The default RecordedOnly profile requires a
bounded immutable original/transformed UTF-8 record for non-ASCII conversion.
The explicit UnicodeDefault profile supplies full Unicode 16.0.0 default
upper/lower mappings, including contextual final sigma. Title case follows the
source's literal-space word starts and uppercases the initial character without
lowercasing the rest. Locale-specific Lithuanian/Turkish/Azeri rules are excluded. The mapping inputs
are [UnicodeData](https://www.unicode.org/Public/16.0.0/ucd/UnicodeData.txt),
[SpecialCasing](https://www.unicode.org/Public/16.0.0/ucd/SpecialCasing.txt) and
[DerivedCoreProperties](https://www.unicode.org/Public/16.0.0/ucd/DerivedCoreProperties.txt).
This profile applies no normalization.
Both recorded texts are validated, duplicate records refuse, and an exact record
always overrides native conversion. Glyph requests and layout consume the same
transformed bytes. Native Unicode defaults are a documented profile, not proof
of platform-specific GameMaker casing parity.

## Rendering and state

Text retains source delimiter-preserving trim, wrap, tracking, line gap,
monospacing, alignment, padding, fixed/dynamic canvas, scale-to-fit, path shift,
path rotation and wave branches. Callback indices start at one. Palette
ping-pong duplicates the upper endpoint. Palette multiplication truncates
integer channel products divided by 255. Random branches require ordered
captured source draws; native rendering does not consume an ambient stream.
`round_position`, `blend_mode` and `character_range` are read but do not affect
the pinned active source drawing branch.

Glyph drawing uses explicit CPU pixel-center coverage and source alpha
multiply blending. The background uses the source Normal blend sequence; Atlas
glyphs use source alpha-add and own both glyph and original output pixels.
Empty text preserves the prior row's format and Atlas while producing a
transparent one-pixel surface. Shared primary/fallback fonts and per-row
format/Atlas state use owned DataReplay snapshots.

Ordinary native file-font texture modulation samples local glyph UV and real
coverage. Raw atlas debug output requires an observed atlas and rectangles.
Bitmap texture modulation requires those observations in SourceObserved mode.
The explicit NativeFrameUv mode instead samples the provided texture in local
glyph-frame UV. Exact observed texture/rectangles override that native profile.
Native file SDF uses actual FreeType signed-distance bytes, white RGB and alpha
with a 128 contour, spread 2..32, retained padded bearings and true advances.
Text derives coverage from distance and the selected scale/antialias setting;
it never relabels gray glyph coverage as SDF. Source derivative, spread and
licensed raster parity remain observation gates.
Pixel, bilinear, bicubic and Lanczos sampling are CPU profiles. Clean Edge is
unrepresented. `RequireSourceGpuRasterCoverage` refuses active glyph drawing
when exact source device coverage is required.

## Bounds and atomicity

Fonts have at most 4,096 glyphs/frames/measurements and four MiB of owned payload.
Text stages every selected processor row in source order, admits old state,
new fonts, immutable layout/draw plans, Atlas storage, temporary background
storage and all output coexistence before allocating output pixels. Providers
and captured random draws run once during preparation. The complete batch has
a 16-million-work-unit limit; work includes sampling taps, glyph writes and
background composition. Public outputs and replay state publish only after
successful execution.

Live font receipts are retained only when a Pixel Builder recipe needs them.
Recipe snapshots own the exact context and observations and retain no provider
pointer. Request, receipt and recipe capacities participate in byte admission.

## Reference boundaries

The pinned source is Pixel Composer commit
`b69eca232217360cf1502ef0223523d818606652`. The native FreeType profile uses
actual decoded glyph coverage, advances, bearings and line metrics. It does
not establish licensed GameMaker glyph rasterization, hinting, shaping,
kerning, heterogeneous sprite-frame behavior, source font-cache identity or
ambient atlas-state parity. Those require observations from the source runtime.
Out-of-range trim behavior, invalid/freed font handles, and unsupported source
sampling have named refusals rather than guessed results.

## Host and product ownership

`GraphFontInputs` owns an immutable configuration, exact read grants and one
process-local file provider. It cannot move or copy while requests borrow it.
Replacing its configuration validates and admits prior/candidate/provider
coexistence before publication, then advances its revision. Binding copies a
held-frame context and reports owner residency through
`SourceFontHostResidentBytes`; the core checked-admits that scalar before work.
The context and receipts are independently charged when independently retained.
A failed bind preserves the previous context and request.

Studio binds held preview playback and freezes the export configuration. A
fresh playing export seeds per-node primary/fallback fonts only from a matching
prepared preview revision, input revision and clock. Existing replay state wins
over seeds; an explicitly empty seed suppresses global InitialFont fallback.
The CLI `--font-inputs` artifact and client owner use configured playback rather
than interpreting animation ranges or rigid-body clocks as Composer playback.
Client configuration revisions invalidate derived graph caches. Inspector,
audio, snapshot, recipe, cook and export requests forward the same owned facts.
The durable font artifact is separate from native document literals and uses a
bounded native reader for receipt-authored nodes.

The generated Unicode tables are pinned by SHA-256 in
`tools/source-font-case-data.py`. Regenerate from explicitly supplied official
Unicode 16.0.0 data files, then apply the repository clang-format style. The
Unicode license is included in `docs/licenses/UnicodeCase.txt`.
