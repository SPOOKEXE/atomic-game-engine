# Source point data

The CPU reference profile implements three Pixel Composer routes from source
commit `b69eca232217360cf1502ef0223523d818606652`.

| Route | Source kernel | Native output |
| --- | --- | --- |
| `pc.scatter_point_fibonacci` | Sequential length and angle accumulation; signed step, independent X/Y scale, rotation and project-relative center | Flat Vector2 points, processor rows when controls are arrays |
| `pc.scatter_point_lattice` | Rounded subdivisions plus one, inclusive endpoints and row-major traversal; area, padding and two-point coordinates converted before units | Flat Vector2 points, processor rows when controls are arrays |
| `pc.segment_filter` | Strict angle difference less than spread, optional opposite direction, contiguous accepted runs across one or multiple paths | Nested Vector2 paths |

Fibonacci and Lattice call `random_set_seed` in their source kernels but consume
no random draws. Their native points are deterministic and do not mutate an
ambient random stream. Agreement of these points does not establish parity of
later consumers of the source shared RNG. Shared-stream side effects require
executable source evidence or an owned generator-state observation. An empty
random draw receipt does not prove that reseeding happened.

Fibonacci retains sequential additions of step and angle, including accumulated
floating-point rounding. Lattice uses ties-to-even rounding for integer vector
subdivisions. Negative subdivisions producing zero points are valid. Negative
Fibonacci array sizes have no defined native source profile and produce a named
refusal. Nonfinite generated coordinates and excess output/work/byte budgets
refuse before publication.

Segment Filter performs no output write when the input or its first path is
empty. Its fresh output is `[[]]`; a later empty input returns the latest owned
output. Valid singleton paths update the output to `[]`. Native evaluation
stores only the latest value in the existing DataReplay owner, with an exact
node and processor-row identity. The caller commits the output and replay
candidate together. A failed evaluation preserves both, permitting a retry.
Clearing that replay owner provides a native reset. Source cache behavior across
authored edits and host reset events has not been verified; this document does
not claim those transitions have source executable parity.

Each generated point array is bounded by MaximumArrayElements. Whole processor
batch work is admitted using division before allocation, with a 2^24 work cap.
Segment input expansion is byte-admitted before copying; source tree depth and
node bounds are checked before path traversal. The result and one replay copy
are admitted before output allocation. Arrays with malformed or nonfinite
point coordinates produce an explicit diagnostic.

Pinned source hashes:

| Source path | SHA-256 |
| --- | --- |
| `scripts/node_scatter_point_fibo/node_scatter_point_fibo.gml` | `1897798498f0a9e74f5432ecfa3dabaf28de3ead8d039d110f61839eb6dffe70` |
| `scripts/node_scatter_point_lattice/node_scatter_point_lattice.gml` | `361059fd9de68ec7fe2407c9fc5977fd143580032e6410be016a681d590aae1b` |
| `scripts/node_segment_filter/node_segment_filter.gml` | `d738403a723346746c89868631d2ba9c1d5e81dc8fcda2fc32c324bcfea95492` |

Tests use real Compile/Evaluate paths, defaults, literal controls, processor
arrays, native document roundtrip, linked frame expressions, and owned replay.
They cover generated coordinates and type/shape, signed stepping, project and
pixel units, area modes, integer rounding, empty axes, strict angle selection,
multiple paths, latest-output retention, and atomic failure/retry.
