# Source Shift path

This outside candidate adds `pc.path_shift` to the native value evaluator. It is a CPU source-algorithm profile. The pinned GML wrapper and the official HTML5 numeric functions are the reference; licensed desktop GameMaker parity has not been captured. Fresh joined release verification is still required before landing.

| Input | Source index | Native payload | Default |
| --- | --- | --- | --- |
| Path | 0 | Path2D | Missing source path |
| Distance | 1 | Scalar | 0 |
| Range | 2 | Vector2 | 0, 1 |
| Loop | 3 | Boolean | False |

Shift derives `Node`, rather than `Node_Processor`. It produces one owned lazy path. Authored scalar distance arrays are rejected by the existing catalogue contract. Owned Shift paths can travel through typed `ArrayValue<Path2D>`, the actual source array getter, native save/load, animation and fresh seeks.

The interval is inclusive and its endpoints are not reordered. Outside it, samples and weights pass through. Inside it, signed distance displaces the sample along the source tangent plus 90 degrees. Wrappers inheriting `Path` use its 0.001 tangent probes and their own loop field. Shift, Array/Combine, Join and Mesh-to-Path publish `Node` objects without that method, so they use Shift's 0.01 fallback and Loop control. The source `pfract` factor is 0.9999. Point direction follows the HTML5 six-decimal Delphi rounding, and lengthdir preserves its near-integer snap. Distance sampling divides by the default line-zero length, even when another line is selected. Bounds, lengths, segment counts and weights forward without displacement.

The source memo key is a six-decimal ratio followed by a comma and line index. Its first sample wins, including the original range decision and weight. Two linked consumers share that result; document order can therefore change which nearby ratio wins. Each evaluation creates a fresh bounded memo. Exact binary64 decimal-half handling preserves HTML5 `toFixed(6)` ties such as 0.0078125 becoming 0.007813. Values at magnitude 1e21 or larger use an equivalent unique binary64 key class instead of duplicating shortest decimal text.

The memo owns only sample slots and owner identity strings. Authored node, port and bounded element routes identify independent blobs. Trusted producer copies carry an evaluation-local stamp. Native persistence omits it, semantic equality ignores it, and public outputs, snapshots, host/builtin receipts, nested PixelBuilder recordings and DataReplay publication strip it. A public or retained authored stamp is never accepted as a producer identity.

Geometry and Redistribute's native finite-validation probes suppress memo reads and writes within an exception-safe nested scope. Their source constructors do not sample at that point. Skew's real source sample at zero remains observable. This avoids manufacturing a first cache sample during validation.

Slots, strings, vector growth overlap and input clones are admitted before allocation. Identity and sample tables each have at most 4096 slots, and lookup work is bounded at 2^24. Nonfinite controls, samples or keys and unsupported payloads fail explicitly. Failed evaluation leaves the caller's output, authored trees and prior replay intact. There is no ambient or persistent numeric cache identity.

## Evidence

The source extraction pin is `b69eca232217360cf1502ef0223523d818606652`. Exact file hashes are recorded in the outside manifest:

- `scripts/node_path_shift/node_path_shift.gml`: `55539a3ae8ea96ec199900632f7045a74e5427e587a65a521235e65c4e4aafab`.
- `scripts/__path/__path.gml`: `5a3243e1bac128dfd342e6f4488d9e82e46ce4cada72e15677cd9b3cf4c7945d`.
- `scripts/number_function/number_function.gml`: `ccbd5ec721288e000c865bf9003a7e378f6092df54591ce25039d107fa0d11ac`.

Official [HTML5 string functions](https://github.com/YoYoGames/GameMaker-HTML5/blob/develop/scripts/functions/Function_String.js) were read at SHA-256 `8fea0cea0d55e9e6b1ba07aae1502d9f4e75aa4b6ccaffc66d047a8d5d2f9fd3`; [math functions](https://github.com/YoYoGames/GameMaker-HTML5/blob/develop/scripts/functions/Function_Maths.js) at `673b03e5f2cab870d145141911c1ec2dc767dbb59252d41d01fd8ef3f6eee005`.

The immutable outside imagegraph archive contains all 187 baseline translation units plus the two new units, compiled with candidate headers and no post-build source/header drift. The diagnostic runtime uses configured release-tests module flags with `-O0 -g0`, no PCH; new units and tests also use `-Werror`. This is functional evidence, not a performance claim or joined release gate. The matched graph suite passed 206 assertions across 16 cases. Independent JavaScript `toFixed(6)` vectors matched all 11,814 generated keys.

The proposed stale-reference defect did not reproduce: animated Shift through Redistribute with prior DataReplay samples (5,-2) at tick zero and (5,2) at tick one, matching a fresh seek and the source. Redistribute/MapArea processData and Skew update refresh each frame; no extra durable producer refresh state was added.
