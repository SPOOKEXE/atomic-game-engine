---
description: Drive one local data-factory world through the atomic-client MCP host and export verified observations.
---

Confirm that `atomic-client` is connected through stdio `mcpbridge` to a host
started with `client --data-factory --mcp-port 8736`. Call `tools/list`, then
`negotiate`, before choosing any operation.

Use an existing local world first. Call `lifecycle_inspect`, pause it with the
`all_systems` scope, and retain the returned tick, epoch, and version. Run
`script_check`, then submit a pinned `atomic.data-script.v1` package from
`mono.engine/examples/assets/scripts/DataFactoryPackageDemo.luau`. Use the
exact paused tick, epoch, and version, plus a unique `operation_id`.
The package replaces only its owned `DataFactoryPackageDemo` workspace subtree.

Observation calls happen after the package reaches a terminal result. Create a
snapshot and read it with `get_scene_snapshot`. Call
`get_camera_rendering_data` with an object limit.
Capture only channels negotiated by the host and use the exact capture schema.
Poll `poll_capture` within its advertised bound. Read every ready resource with
ranged `get_resource` calls, then verify the published checksum after assembling
the complete byte stream. Call `release_capture` after the full verified fetch.

Report unsupported operations and channels exactly as the host returns them.
Do not invent reset, retirement, capture backends, render labels, or checkpoint
support. Do not claim reset or retire behavior until `negotiate` advertises it.

Read `datafactories-docs/MCP-ADDITIONS.md` for the evolving external-factory
contract and its explicit unsupported states.
