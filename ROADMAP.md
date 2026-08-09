
# ROADMAP

## Editing

Do NOT add new deferred work as a roadmap item. Place it in
`docs/DEFERRED.md`. The existing `Deferred:` blocks below are version history
and pointers to that register. If a TODO item is not FULLY completed, split the
TODO item, keeping the concise short dash point list with block infront and
split it as another item under the same version.

For example, if we complete A and B but not C and D:
```
### v0.5
- [x] do: A1, A3, A5
- [_] do: A, B, C, D
- [x] do: G, H, I
- [x] do: K, K2, K3
```

becomes:
```
### v0.5
- [x] do: A1, A3, A5
- [x] do: A, B
- [x] do: G, H, I
- [x] do: K, K2, K3
- [_] do: C, D
- [_] deferred `D0001` for later.
```

or defer to another version.

## VERSIONS

The milestone headings below are development labels. Not in line with project versioning.

### v0.11

* v0.20 (needs prototype project first)

### v0.12

- [_] improved physics pipeline with spatial optimisations
- [_] studio editing tools; select, move, rotate, 3d scene interactable gimbals, grid step amount input, rotation amount input, anchor toggle, "lock" toggle, pivot editor mode, reset pivot button. Reference "~/Pictures/Screenshot from 2026-08-08 15-59-40.png" and "~/Pictures/Screenshot from 2026-08-08 16-00-19.png".
- [_] studio tooling tabs
- [_] plugin system
- [_] expose ECS underlying to luau and typescript (locked down but can query engine for entities using direct and returns instances)
- [_] rojo folder syncing + tests, support multi-world by subfoldering (main.universe.json for universe mapping, or assume per-viewport state for rojo-like sync system with main.default.json being in each subfolder)
- [_] world => rojo sync, universe => multi-rojo sync (i reckon keep them separately syncing so even if one has bad formatting, the rest still sync)
- [_] update built-in MCP integrations, add a studio ui button to enable/disable and information panel
- [_] create a unified networking system for LAN, Peer2Peer and remote connections. Put in mono.network, then, build by import for the engine, studio and the cdn. I plan the engine to support direct connections between users (the host runs a server + client, then other clients connect to the local server) like LAN and Peer2Peer, then the studio supports it as well for team create with many users, then the cdn supports it to support different distribution streams like LAN, Peer2Peer, private (key accessed) networks and public distribution.
- [_] deferred items

### v0.20 (needs prototype project first)

- [_] extended rendering pipeline (handle multiple worlds in parallel, handling gpu traffic)
- [_] rendering pipeline is a node system with a studio editor
- [_] surfaceapperance actually integrated with new pipeline
- [_] render pipeline is per world, not per process
- [_] render pipelines are saved in the world's export data
- [_] rendering pipeline debugger and profiler - shows a graph's per-step operations with their compute costs and wall clock, etc.
- [_] rendering pipeline additions; ambient occulusion, emissive, pbr, default node setup with all these
