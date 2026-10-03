# Source Strand simulation integration

The bounded native Strand routes are registered and landed. Fresh joined56 validation is pending.

The public `StrandValue` owns a `StrandData2D` through the existing compact owned
payload wrapper. It appends alternatives at Value index 38 and ElementValue
index 37. Earlier alternatives retain their indices. Static assertions check
that Value remains 88 bytes and StrandValue is 8 bytes.

There is no second persistent Strand ledger. Existing caller-owned DataReplay
stores one latest geometry frame per creator node ID and processor row. The
payload carries the authoring revision. Create requires tick-zero initialization
or a contiguous previous snapshot. Gravity and Update publish replacement entries
with the same origin. The evaluator's candidate ledger makes those changes visible
to later nodes, and commits output and state only after the complete selected
closure succeeds. Existing recursive simulation alias resolution additionally
resolves Strand leaves against the final candidate DataReplay. Sibling outputs,
arrays and structs therefore observe the final source object state.

Registered routes: `pc.strand_create`, `pc.strand_gravity`, `pc.strand_update`,
`pc.strand_group_inline`. Create implements the source unbaked uniform Point
branch. Unsupported Path/Mesh generation, random distribution and grooming
produce explicit diagnostics. Rendering, cloth construction and other Strand
routes are not included or claimed. Inline is the source no-op collection
wrapper; native owned groups already recognize this type. The isolated IO patch extends source attrs.members/ictx and renamed clone projection
to Strand wrappers. Existing FLIP and Verlet import coverage is retained.

Create consumes exact borrowed builtin call observations for each new hair:
length random_range, ID irandom_range(100000,999999), then the seeded root-strength
random_range only if its endpoints differ. Existing binding validation matches
all resolved controls, node, row and frame. No ambient random generator is
invented. Geometry generation uses the verified official HTML5 math profile.
Licensed desktop math and RNG parity remains unverified.

Source lifecycle quirks are retained. Decreasing density does not truncate old
hairs. Growing density appends hairs, retaining old IDs and geometry. Existing
attached roots move without translating their remaining points. The source
mesh.step has one parameter, so authored Iteration is ignored and the hair runs
four chain/angle/spring passes per Step. Either nearly stationary motion axis
prevents propagation. rootForce resets before detachment. Fractional spring
indices receive an explicit captured-coercion diagnostic.

The version-9 strand2 native codec preserves geometry, constraints, point motion,
optional IK values and durable identity. Counts, finite state, origin length,
rows, point totals and allocation admission are checked before owned growth.
Read failure preserves the destination. The generic array codec transports owned
Strand leaves without flattening their geometry.

Prior validation used complete immutable imagegraph and imagegraphio source snapshots
with matching public/private headers. The registered graph and importer checks
passed before landing. Fresh joined56 validation is authoritative for the current
shared headers and source integration.

Evidence pins:

- Pixel Composer source revision: `b69eca232217360cf1502ef0223523d818606652`,
  `scripts/__strandSim/__strandSim.gml`, `node_strand_create`, `node_strand_gravity`,
  `node_strand_update`, `node_strand_group_inline`, `random_function`.
- Official GameMaker HTML5 runtime revision: `60e51be51ce7f3d52025ef18106cf172b8e22a00`,
  `scripts/functions/Function_Maths.js`. Source point-direction and lengthdir math
  are checked against extracted actual runtime functions.
- The retained `make-reference.py` evidence script extracts actual pinned GML constraint/motion
  bodies into an independent JavaScript reference. `reference-golden.json` records
  the constraint position checked by the C++ suite to 1e-12.

The constructor admits retained prior, displaced output and complete candidate
nested buffers before copying. Aliased prior/output counts retained storage once.
The final hair buffer is reserved before copying old hairs to avoid transient
outer-vector growth. Array updates admit aggregate constraint visits before clone.
Joined release56 verifies 191 assertions in 17 Strand cases and 127 assertions
in nine Inline import cases. The full core suite passes. GPU validation and
performance profiling remain open. Executor scopes feed the existing profiling
paths.

Small reference scripts, generated JavaScript, golden values, input hashes and
probe logs are retained under
`.cache/build/dev/evidence/pixel-composer-2026-10-03/strand56/`.
The reference script takes the pinned GML and official runtime math files as
explicit arguments, rather than depending on a particular temporary checkout.
