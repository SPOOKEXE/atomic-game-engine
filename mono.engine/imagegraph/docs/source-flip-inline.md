# Source FLIP Inline collection

Pinned source: b69eca232217360cf1502ef0223523d818606652, `scripts/node_FLIP_group_inline/node_FLIP_group_inline.gml`, `scripts/node_collection_inline/node_collection_inline.gml`, and `scripts/node_data/node_data.gml`.

FLIP Inline inherits an empty runtime `Node.update`. Its constructor sets `is_simulation` and `update_on_frame`; the native graph already identifies `pc.flip_*` as temporal simulation. This wrapper owns no solver. Its inherited membership and shape attributes stay in the authored native controls. The existing collection executor performs no work, while implicit owner dependencies make the selected member closure evaluate its owner once. Fluid Domain/Fill/Render continue to own replay mutations.

The import patch narrowly admits FLIP alongside the existing Verlet collection projection. `attri.members` and restored `ictx` become owned native Group membership. Source group clones retain durable renamed owner relations. Ordinary nested groups keep their ports and wires. Import budgets and maximum counts run before storage growth; public import publishes only a successful candidate.

Source `NODE_NEW_MANUAL` builds Domain/Spawner/Render only when creating a new collection in the editor. Importing or evaluating an authored wrapper does not create missing child nodes. No artificial output or preview port is added. Native selected closure evaluation does not execute unrelated members.

Validation uses real compiled wrapped and unwrapped fluid graphs through ticks0..3, fixed seed reset, host seek, final image/replay equality, unrelated member isolation, and failed allocation preserving the previous image and ledger. Public PXCX import covers restored ictx, nested topology, rename, conflicting ownership, and fail-atomic resource refusal. No GPU coverage or licensed GameMaker parity claim.
