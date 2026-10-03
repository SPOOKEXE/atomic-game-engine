# Switch CPU routing contract

The private family implements `pc.switch` and `pc.threshold_switch` from pinned
Pixel Composer commit `b69eca232217360cf1502ef0223523d818606652`.

| Source | SHA-256 |
| --- | --- |
| `scripts/node_switch/node_switch.gml` | `0088c238b35068b4bdf6dcb9413f45a8b2ea0350ff5328f0ddd38e692392ebfe` |
| `scripts/node_threshold_switch/node_threshold_switch.gml` | `92436e09dc75b8b2d30b395078eb1b276f5ac2022bb0f3f732234a434e7f4e16` |

## Source inputs and selection

Both constructors inherit plain `Node`. They do not run FX processor batches.
Switch has fixed Text `index` and Any `default_value`, then pairs of Text Case
and Any Value. Exact generated pair IDs are `case_0` / `value_0`, and subsequent
numeric source slot groups. Threshold Switch adds EButton `type` (Number=0,
Frame=1); its pair labels are both Value, so duplicate-name extraction produces
Float `value_0` / Any `value_2_0`. The metadata correction is owned separately in
`/tmp/pc-switch64`; no runtime template is fabricated by the executor.

Switch skips empty string Case values and keeps the LAST equal case in source
slot order. Strings compare byte-exactly with case preserved. The Text getter
returns raw linked values, so numbers are not formatted as text. Threshold
Switch likewise skips an actual raw empty string, does not sort thresholds,
and keeps the LAST inclusive qualifying threshold. Frame uses signed
`frame + 1`, including subframes; it ignores the numeric Index for selection.
Empty lists and missing Any input values use the actual constructor numeric 0.

Dynamic declaration order does not replace source slot order. Each declaration
must resolve to its extracted pair template; members must be complete and
unique. The current native limit remains 64 declarations, or 32 pairs. No
public quota or carrier is widened.

## Native comparison profile

Scalar comparison uses epsilon `1e-5`, grounded in `yyCompareVal`, `yyfequal`,
and `yyfgreaterequal` in the immutable official HTML5 runtime:
https://raw.githubusercontent.com/YoYoGames/GameMaker-HTML5/60e51be51ce7f3d52025ef18106cf172b8e22a00/scripts/functions/Function_Maths.js
The local inspected runtime file hashes to
`673b03e5f2cab870d145141911c1ec2dc767dbb59252d41d01fd8ef3f6eee005`.
Numeric/string comparisons follow its bounded leading numeric-prefix grammar,
including trimmed whitespace, signs, at most 30 digits per mantissa component,
and the restricted one/two-digit exponent. Two strings compare as strings,
so `"01"` and `"1"` differ. Undefined equality and non-equality with scalar
values are represented. Two raw text threshold operands compare UTF-16 code units, including surrogate
pairs for astral code points. Native malformed UTF-8 has no represented source
string and refuses explicitly; it is not replaced or ordered as bytes. Two Undefined operands qualify;
threshold comparisons with only one undefined operand do not qualify;
non-numeric raw Any text is an explicit unsupported operator diagnostic.

Native `int64` versus `int64` comparisons preserve all bits and use exact safe
integer ordering. The pinned HTML5 implementation subtracts two Long objects,
which wraps at opposite signed extremes. That runner-specific overflow result
remains a reference gate; this native profile deliberately performs no signed
C++ overflow. Tests cover both adjacent integers above 2^53 and opposite signed
extremes. Ambient changes to GameMaker's comparison epsilon and licensed runner
execution have not been observed, so no licensed executable parity is claimed.

The Text and Float getters override base `NodeValue.getValue`. Their apparent
Index `rejectArray()` flag does not itself execute the base array rejection.
Raw arrays and structs compare by source identity when both key operands are
reference values. Native owned copies have no such identity observations, so
that branch explicitly refuses. Mixed scalar/reference equality is unequal in
the inspected runtime. Threshold array, surface-dimension, or unrepresented
struct getter/operator routes refuse explicitly. Surface handles used as key
numbers also need actual source handle observations.

## Ownership, bounds and failure

Selection forwards a complete payload, including nested heterogeneous arrays,
Struct, Undefined and existing typed carriers. Real image inputs retain pixel
format, dimensions, bytes and hash. An Atlas value remains an Atlas value;
its embedded surface is not substituted. Image arrays retain the complete
owned shape and images without flattening or borrowing producer buffers.
Native copies do not claim source mutable handle alias identity.

Pair processing has a fixed 32-entry table. String comparison/conversion work
has a conservative 16 MiB byte admission (UTF-16 ordering charges both complete
UTF-8 validation and comparison scans) before output allocation; skipped
empty cases and ignored Frame Index do not charge comparison work. Selected
Value trees use existing recursive validation and clone byte admission.
Selected images use the existing checked layout/request budget. Image-array
metadata, image count, nested depth, indices and all owned pixels are checked
before the complete clone. Every candidate allocation is admitted through the
shared evaluator ledger before growth. Failure publishes no replacement to the
caller; no replay state, ambient RNG or external callback is introduced.

## Validation status

The outside draft contains real Compile/Evaluate fixtures for defaults,
duplicates, source slot ordering, string and raw numeric equality, unsorted
inclusive thresholds, signed subframes, downstream nested-array access,
Struct/Undefined, owned typed images and nested image arrays, identity/operator
refusals, quota and comparison bounds, and atomic clone refusal. Strict syntax
uses current CMake C++20 flags. Joined engine runtime tests, sanitizers,
profiling and licensed-runner checks have not run for this draft. Registration
or syntax is not runtime acceptance.
