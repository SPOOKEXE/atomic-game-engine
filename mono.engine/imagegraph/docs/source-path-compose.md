# Source path composition

Three planar path routes implement source formulas from Pixel Composer commit
`b69eca232217360cf1502ef0223523d818606652`. They produce owned lazy path values,
not sampled approximations. Native values and replay state preserve their
composition through document serialization.

| Route | Source behavior preserved |
| --- | --- |
| `pc.path_offset` | Adds Offset to sample ratio and applies positive fractional wrapping or Clamp. Length and segment count delegate to the child; accumulated lengths are reversed, including that source quirk. Distance sampling divides by line zero length. |
| `pc.path_blend` | Lerp, Add, Subtract, and Multiply modes preserve position and weight formulas. Multiply displaces along Path 1's normal using Path 2's Y coordinate. Lerp length is interpolated; other modes sum lengths. Accumulated entries interpolate in Lerp and use maximum in other modes. |
| `pc.path_join` | Dynamic Path/Reverse pairs concatenate path distances and align later paths using `.999` endpoint samples. Selection advances only when distance is strictly greater than a child's length. Line counts sum, while length and segment count ignore requested line index. Accumulated arrays concatenate without offsets. |

Source default provenance distinguishes noone from an owned empty path. The
runtime does not infer that distinction from anchors or link presence. Planar
input profile is explicit; 3D path transport requires its dimensional adapter.
Join accepts flat path arrays and refuses malformed or nested non-path arrays.
Normal array processing uses the evaluator's bounded processor modes. Disabling
array processing with outer arrays retains the existing named execution refusal.

Blend has a source cache transition: when either input becomes noone, its
length and accumulated arrays are not rewritten. Native Blend retains that
metadata in a latest-value DataReplay entry for the node and processor row.
Fresh one-input Blend has zero length and forwards the present path's samples.
Both-input Blend replaces the cache. Evaluation owns candidate output and
replay copies and publishes them together. Failed byte or work admission leaves
the previous output and replay untouched. Supplied replay metadata also survives
seeking; clearing the owner provides the explicit native reset. Executable
source reset behavior across authored edits is not claimed by that native
ownership policy.

Bounding boxes preserve source quirks. Blend interpolates child boxes regardless
of mode. Join unions untranslated child boxes and begins coordinates at noone
(`-4`), because source addBBOX uses raw minimum and maximum rather than its
addPoint initialization rule.

The profile evaluates the deterministic formulas at requested ratios. Source
Blend memoizes six-decimal ratio keys, so nearby requests can alias according
to which sample populated a shared object first. Native immutable path values
and per-consumer runtimes do not represent that shared mutable cache identity.
Isolated numeric agreement does not prove parity of those cache side effects;
that requires an executable source observation or an explicit ordered cache
recording. No ambient cache or RNG state is fabricated.

Before cloning, the implementation admits the full output tree, runtime
storage, cached arrays, and replay copy. Tree depth, path counts, anchor counts,
weights, and cached numeric slots have fixed limits. Whole processor work uses
a 2^24 cap with division before multiplication. Cached-array derivation is
admitted before allocation. Nonfinite metadata or samples refuse execution.

| Pinned source | SHA-256 |
| --- | --- |
| `scripts/node_path_offset/node_path_offset.gml` | `bc6907149f7825617c38a9c6df20eed5e54d146e44f0d5e10dd84a2355c9c0a0` |
| `scripts/node_path_blend/node_path_blend.gml` | `8d6a4bc2c66c5d0938664c692160c12f70b4c1aed4b5855a01f6a2b749f4f58a` |
| `scripts/node_path_join/node_path_join.gml` | `2a178f1a12216b3353d1d531a6cf617b469f43a3649704d7cc76a85c3284aaca` |

Eleven actual graph tests cover defaults, wrapping/clamping, four Blend modes,
weights, length and accumulated metadata, Join translations and reversals,
multiple lines, processor arrays, tree and aggregate work limits, instance
refresh to noone, retained cache on seeking, atomic budget retry, and valid and
malformed native roundtrip payloads. Runtime acceptance awaits the joined gate.
