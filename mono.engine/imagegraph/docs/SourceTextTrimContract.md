# Native Text trim boundary

`pc.text` keeps the original cased string for full-text sizing. Trimming only
changes the string used by its draw layout. The native implementation accepts
finite signed endpoints, including endpoints outside `[0,1]`, rather than
requiring a source recording for every such value.

The pinned Text constructor reads Range directly from the slider-range getter.
The getter does not clamp it. Text multiplies each endpoint by the character,
word or line count and uses the existing half-even rounding profile.

Character trimming follows the inspected HTML5 `string_copy` boundary rules:
clamp its one-based start, then clamp and swap substring endpoints. A negative
start therefore does not reduce the requested length. Native character indexing
uses valid Unicode scalars. The inspected HTML5 function's surrogate adjustment
loops and licensed native runner behavior remain platform evidence gates.

Word and line trimming follow `string_splice(...,keep=true)` and
`string_concat_ext`. Space/LF delimiters stay attached to the preceding token;
word mode recognizes both, line mode only LF. Trailing empty tokens remain.
Offsets clamp to the token range, negative offsets count from its end, and a
negative length concatenates tokens in reverse order. Output retains original
UTF8 bytes within each token. No case conversion is repeated here.

Signed source arguments outside int32 representation refuse with LimitExceeded
before conversion. Token table capacity is admitted and reconciled before use.
All output remains a private candidate until the existing Text batch commits.
The normal scheduled preflight quotes eight linear trim passes per cased UTF8
byte across every row before any font provider runs, bounded at 16 Mi work.
This quote does not change selected-row scheduling or consume random draws.

`imagegraph.font.trim` attributes the work to the existing profiler. Counters
report input/output bytes, retained token workspace and character/token counts.
`just imagegraphfont-wrapped-text-bench 3` checks and profiles this scope through
the granted font host. It also measures 128 wrapped lines, a 256-space scan and
64 distinct fractional-width requests using real decoded BDF advances. The
optimized `bench` preset retains heap hooks; output stays on stdout. Native
measurement request bytes and request counts are measured at the production
boundary. The work-unit counter is the conservative admission quote, rather
than an instrumented count of operations executed.

## Source evidence

Pixel Composer commit `b69eca232217360cf1502ef0223523d818606652`:

- `node_text.gml`: `34640857e6878bb84c4778649dedfac455a062d2a3be35cbb0b191f3cb8cc9ca`
- `string_splice.gml`: `6c0dcf2382680ed719254ad21dc0585a2c1deb9b4324be2f4de36fd2e60af1e8`
- `node_value_slider_range.gml`: `e4919e26d214fb036a727fd2e154eb20d8a74dbb041d8db6b0bce708fe06ce63`

GameMaker HTML5 commit `60e51be51ce7f3d52025ef18106cf172b8e22a00`:

- [Function_String.js](https://raw.githubusercontent.com/YoYoGames/GameMaker-HTML5/60e51be51ce7f3d52025ef18106cf172b8e22a00/scripts/functions/Function_String.js), retained bytes SHA256 `8fea0cea0d55e9e6b1ba07aae1502d9f4e75aa4b6ccaffc66d047a8d5d2f9fd3`
- `yyVariable.js` `computeIterationValues`, retained bytes SHA256 `da5d6a2fba34a237e563de92df55a3db96999f34b762d59dbae969b6bd08deb0`

## Remaining observation boundaries

File loading uses the existing exact read grant, ContentPolicy, real FreeType
coverage or signed-distance decoder and bounded owned artifact. This supplement
adds no font discovery, implicit read grant or new renderer. Source font/cache
identity, platform kerning, exact Unicode casing recordings, raw atlas debug
coordinates and source GPU coverage continue to use their explicit observation
profiles. Native SDF derivatives/spread and native advance-based wrapping remain
named native profiles. A CPU test or benchmark establishes none of that licensed
or device evidence.

Text's ordinary source interpolation selector exposes only 1 through 4.
Its actual `sh_node_text_render` shader has no CleanEdge algorithm: a raw
interpolation value 6 falls through to a nearest texture read. The unrelated
`sh_sample_clean` shader is not substituted into Text. No new authored choice
is exposed by this supplement.
