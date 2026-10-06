# source obj index audit

grug read pinned pixel composer `b69eca232217360cf1502ef0223523d818606652`, `scripts/node_3d_mesh_export/node_3d_mesh_export.gml`. lines 152-154 format each attribute to five decimal places; lines 158-192 assign independent first-seen indices; lines 248-250 create shared maps for the whole export.

grug native export now follows these index rules across mesh parts. same formatted position can keep separate UV and normal indices at seams. source flattened transform, rotation-only normals, axis inversion and UV inversion stay in their existing order.

grug reserve bounded index buckets and charge retained key capacity and map nodes before insertion. receipt admission happens before file publication and includes allocated receipts with empty authored ids. publication prepares backup paths before renames and restores on every failed exit; failed recovery leaves staging files and a recovery path where diagnostic allocation succeeds.

grug joined batch passed all 20 checks, including 91 export cases and 31 offscreen render cases. FrameGraph scope and vertex, unique-attribute and payload counters checked at actual encoding boundary. no speed claim, no benchmark run.

grug has no licensed reference capture. native record ordering and whitespace still differ from source export, so this proves index structure rather than exact file bytes. allocation-exception rollback was reviewed without allocator fault injection; headed Studio remains unverified.

validation receipt: [native obj indices](../../../docs/pixel-composer-m0/native-obj-index-validation-2026-10-07.json).
