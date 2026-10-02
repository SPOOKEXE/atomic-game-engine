# CSV and tilemap text profiles

Source contracts use Pixel Composer commit
`b69eca232217360cf1502ef0223523d818606652`, scripts `node_csv_file_write` and
`node_tiler_export`. String conversion was checked against the official
GameMaker HTML5 runtime tree `60e51be51ce7f3d52025ef18106cf172b8e22a00`.

GameMaker documents that
[string_join_ext](https://manual.gamemaker.io/monthly/en/GameMaker_Language/GML_Reference/Strings/string_join_ext.htm)
implicitly invokes string(), whose
[real conversion](https://manual.gamemaker.io/lts/en/GameMaker_Language/GML_Reference/Strings/string.htm)
emits two places for fractional values. The native CSV formatter follows the
executed HTML5 `yyGetString` implementation. Exact significand arithmetic avoids
rounding through a binary64 multiplication: `1.125` becomes `1.13`, while `2.675`
becomes `2.67`. Signed zero converts to `0`; tiny negative fractions can become
`-0.00`. HTML5's signed32 coercion makes larger integral reals use two places
until magnitude reaches `1e21`, then shortest scientific notation. Owned native
int64 values retain exact integer text separately. JSON numeric output does not
use this formatter.

The compiled tile formatter matched the executed official conversion for all
63,488 finite binary16 patterns. The CSV formatter matched 99,957 finite
binary64 boundary and seeded random samples under UBSan. These probes ran
outside the repository using verbatim primary functions and primitive-only
global stubs. The unmodified JavaScript-compatible source CSV loop also matched
flat and mixed fixture text, including its leading separators after nested rows.
This is evidence for the native HTML5 profile, not an execution of proprietary
GameMaker desktop file I/O.

SHA256 of the primary runtime files used by those probes:

| File | SHA256 |
| --- | --- |
| yyTypes.js | `10afe81c766f2cf932a37b3745f67f130acc919b67d60b096133fb8e99f153a1` |
| Function_String.js | `8fea0cea0d55e9e6b1ba07aae1502d9f4e75aa4b6ccaffc66d047a8d5d2f9fd3` |
| Function_Variable.js | `da5d6a2fba34a237e563de92df55a3db96999f34b762d59dbae969b6bd08deb0` |

Native CSV emits LF and writes cell commas, quotes and embedded newlines
literally, matching the source loop without inventing CSV escaping. Nested
array-valued cells retain the source bracket representation. Opaque runtime
objects and their custom methods require source observations before conversion.

Tilemap export admits at most 65,536 cells and the supplied operation budget.
Room templates have a conservative byte admission and bounded depth, events and
strings. The supplied source parent, first layer, tilesetId and tiles records must
be objects; the adapter updates only the source-assigned fields. Square maps
retain row-major order. Both rectangular orientations hit source invalid array
indexing and are explicitly refused. Native room JSON uses two-space indentation.
Proprietary desktop rounding, platform file_text_writeln bytes and GameMaker's
JSON pretty-print bytes remain observation gates.
