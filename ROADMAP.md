
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

### v0.12

- [x] expose ECS underlying to luau and typescript (query engine for entities using direct and returns instances, create custom components and create custom entities and attach components, set component values, etc).
- [x] rojo folder syncing + tests, support multi-world by subfoldering (main.universe.json for universe mapping, main.default.json accepted beside Rojo's own default.project.json in each subfolder)
- [x] world => rojo sync, universe => multi-rojo sync (each world syncs separately, so one bad project file costs its own world and the rest still sync)
- [_] deferred `D00104` — the rest of Rojo's file table: .rbxm, .model.json, .meta.json, .txt, .csv, .json, .toml.
- [x] update built-in MCP integrations (component_list, entity_query, component_get, component_set over the v0.12 storage surface; first suite for `control`), add a studio ui button to enable/disable and information panel
- [x] walked the deferred register. One new item filed — `D00104`, the rest of
      Rojo's file table. Every other open entry's reopen trigger is unfired:
      `D00102` wants the `bake` module split, `D00039` wants a world that ticks
      physics (v0.13), `D00038`/`D00046`/`D00103` want a render pass executor,
      `D00030`/`D00031` are tooling gaps with working workarounds, and
      `D00014`/`D00015`/`D00018`/`D00019` are net and replication decisions
      waiting on a deployment.
- [x] create a api for setting up configs in the studio and saving them to the ~/Documents/atomic-game-engine/studio folder. Save for preferences.json, cdn.json, recent.json (last 5 projects), keybinds.json. Also materials preview now renders without a hover and fetches a missing colour map through the delivery client.
- [x] studio editing tools; select, move, rotate, 3d scene interactable gimbals, grid step amount input, rotation amount input, anchor toggle, "lock" toggle, pivot editor mode, reset pivot button.
- [x] studio tooling tabs (Home, Model, Script, View)

### v0.13

- [_] improved physics pipeline with spatial optimisations
- [_] plugin system
- [_] create a unified networking system for LAN, Peer2Peer and remote connections. Put in mono.network, then, build by import for the engine, studio and the cdn. I plan the engine to support direct connections between users (the host runs a server + client, then other clients connect to the local server) like LAN and Peer2Peer, then the studio supports it as well for team create with many users, then the cdn supports it to support different distribution streams like LAN, Peer2Peer, private (key accessed) networks and public distribution.
- [_] implement breakpoints into luau and typescript and script editor and create tab window for it to view stack, upvalues, etc.

### v0.?? (needs prototype project first)

- [_] extended rendering pipeline (handle multiple worlds in parallel, handling gpu traffic)
- [_] rendering pipeline is a node system with a studio editor
- [_] surfaceapperance actually integrated with new pipeline
- [_] render pipeline is per world, not per process
- [_] render pipelines are saved in the world's export data
- [_] rendering pipeline debugger and profiler - shows a graph's per-step operations with their compute costs and wall clock, etc.
- [_] rendering pipeline additions; ambient occulusion, emissive, pbr, default node setup with all these

