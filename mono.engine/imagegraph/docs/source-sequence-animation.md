# Source Sequence Animation CPU profile

`pc.sequence_anim` implements the pinned ordinary-array `Node_Sequence_Anim.update`
branch. The source commit and exact file hashes are recorded in
`native-sequence-animation-policy.json`. This is a CPU execution contract, without
an Inspector or timeline thumbnail drawing claim.

The current signed frame times Speed is floored. Hold clamps the sequence
position, Loop wraps it, Ping Pong reflects a period of `2 * length - 2`, and
Empty returns source `noone` only at or beyond the positive sequence end.
Negative Empty positions still wrap through the source array helper. An empty
Sequence selects frames in their original order. A nonarray Surface In is
forwarded before frame/index arithmetic.

Source `noone` is integer `-4`. An invalid frame index returns numeric zero,
matching `array_safe_get_fast`'s default argument. These are separate typed
results, not transparent images. A selected nested row keeps its shape.
Source real-index bounds are checked before the existing native array-index
profile truncates a finite nonnegative in-range fractional index. Licensed
desktop fractional index coercion is an open reference gate. Single-frame Ping
Pong performs modulo zero in the source and receives an explicit unsupported
execution diagnostic until that runtime behavior is established. Ordinary
native arrays do not claim the source helper's ArrayObject extension behavior.

This node has no persistent evaluator state. Direct seeks and signed fractional
requests select the current input deterministically. The client recognizes its
source `update_on_frame` contract even without authored keys, so keyless
sequences are sampled as animated graphs. Unrelated static graphs retain their
existing cadence.

The evaluator admits bounded source-tree temporary storage before copying, then
admits selected output storage while the temporary remains live. Output storage
is independently owned. Invalid arithmetic, unsupported source runtime behavior
and byte-limit refusal preserve the caller's prior result. Existing array depth,
count and payload byte limits remain in force.

The compiled graph suites cover all four modes, authored permutations, signed
fractional clocks, negative speed, scalar passthrough, empty arrays, both
sentinels, direct seeks, reset, nested rows and failure atomicity. A measured
executor admission-edge test verifies the complete temporary/output overlap
with unrelated retained bytes. The CPU client suite loads an actual keyless
sequence file and checks changed frame pixels, repeat seeks and static cadence.
Runtime acceptance is pending a fresh joined build; syntax checks are not
runtime evidence.
