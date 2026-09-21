---
description: Build an isolated scene through the engine MCP host and export verified observations with the same tools available to an external factory.
---

Confirm that `atomic-client` is connected through stdio `mcpbridge` to a host
started with `client --data-factory --mcp-port 8736`. Call `tools/list`, then
`negotiate`, before choosing any operation.

When `world_create`, `world_reset`, and `world_retire` are advertised, create an
isolated factory-owned world with a unique operation ID. Use `world_reset` for a
new episode only with the exact tick, epoch, and version returned by the host.
Create and reset return an all-systems-paused tick-zero revision; use that
revision directly for the first package or patch.
If those tools are unavailable, select the existing local world as a limited
compatibility path and leave it running when finished.

For the compatibility path, call `lifecycle_inspect`, pause the selected world
with the `all_systems` scope, and retain the returned tick, epoch, and version.
Run `script_check`, then submit a pinned `atomic.data-script.v1` package from
`mono.engine/examples/assets/scripts/DataFactoryPackageDemo.luau`. Use the
exact paused tick, epoch, and version, plus a unique `operation_id`.
The package replaces only its owned `DataFactoryPackageDemo` workspace subtree.

Observation calls happen after the package reaches a terminal result. Create a
snapshot and read it with `get_scene_snapshot`. Call
`get_camera_rendering_data` with an object limit.
Capture only channels negotiated by the host and use the exact capture schema.
Request `object_ids` with the image when it is advertised. Treat integer zero as
background or unidentified, and retain the returned dense `object_labels` table
beside the plane so each nonzero pixel resolves to a stable `DataFactoryId`.
The current plane records opaque and masked gbuffer coverage. Transparent
surfaces, particles and later composited layers do not write it, so do not treat
it as final-image visibility or occlusion truth.
Poll `poll_capture` within its advertised bound. Read every ready resource with
ranged `get_resource` calls, then verify the published checksum after assembling
the complete byte stream. Call `release_capture` after the full verified fetch.
Retire the world only when this run created it and the host advertises
`world_retire`.

Report unsupported operations and channels exactly as the host returns them.
Do not invent lifecycle support, capture backends, render labels, or checkpoint
support. A tool is usable only when both `tools/list` and `negotiate` expose it.

Read `datafactories-docs/MCP-ADDITIONS.md` for the evolving external-factory
contract and its explicit unsupported states.
