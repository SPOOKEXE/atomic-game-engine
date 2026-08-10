
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
- [x] the rest of Rojo's file table: .meta.json and init.meta.json patch properties onto whatever the file of that stem built, .model.json builds a class with its properties and children, .json becomes a ModuleScript with generated source, .txt and .csv become a StringValue and a LocalizationTable over the new scene::TextContent, and a nested .project.json is followed with a cycle check.
- [x] room in scene::Visual again — Surface, CastShadow and Locked used the original three padding bytes, so it was widened once on purpose and the next four one-byte fields are free. engine.scene.components pins the size.
- [x] expanded ecs::Schemas from 256 to 2048 described components. The cap was never the real constraint: the generated hook bodies were inlinable, so every thunk carried a copy of the PropertyType switch — 113 MB of object at 4096 slots. Held out of line it is about 192 bytes of .text a slot, and the measurements are in Schema.cpp.
- [x] update built-in MCP integrations (component_list, entity_query, component_get, component_set over the v0.12 storage surface; first suite for `control`), add a studio ui button to enable/disable and information panel
- [x] walked the deferred register. Every open entry's reopen trigger is unfired: `D00102` wants the `bake` module split, `D00039` wants a world that ticks physics (v0.13), `D00038`/`D00046`/`D00103` want a render pass executor, `D00030`/`D00031` are tooling gaps with working workarounds, and `D00014`/`D00015`/`D00018`/`D00019` are net and replication decisions waiting on a deployment.
- [_] deferred `D00104` — the last three rows of Rojo's file table. `.rbxmx` needs an XML parser and `.toml` a TOML parser, neither of which `mono.vendor` carries; `.rbxm` is a binary format reader that belongs beside the model decoders in `bake`.
- [x] create a api for setting up configs in the studio and saving them to the ~/Documents/atomic-game-engine/studio folder. Save for preferences.json, cdn.json, recent.json (last 5 projects), keybinds.json. Also materials preview now renders without a hover and fetches a missing colour map through the delivery client.
- [x] studio editing tools; select, move, rotate, 3d scene interactable gimbals, grid step amount input, rotation amount input, anchor toggle, "lock" toggle, pivot editor mode, reset pivot button.
- [x] studio tooling tabs (Home, Model, Script, View)
- [x] improved physics pipeline with spatial optimisations — the broad phase's grids size themselves from the colliders they hold (spatial::SuggestCellSize), which lands on the hand-picked optimum at every density: 22% off the default at 4000 colliders, 17% at 1000. An author who names a size still gets it.
- [x] plugin system — a plugin is a folder in the studio config directory holding a plugin.json and a script, run as its own script::Runtime against the world being edited. It reaches everything a game script does, including World; the selection crosses as the studio.Selected component rather than as an API. One runtime each and one failure each: a plugin that will not start, or that throws three beats running, is switched off and named while the rest keep running.
- [x] plugin editor API — script::HostSurface is the seam (one virtual, a value tree, no lua_State crossing a module boundary) and script::HostValue carries an Instance and a Callback where ScriptValue carries neither. A plugin creates toolbar sections and buttons with real handlers, dock widgets that draw immediate-mode like every other panel, and reads or writes the source of scripts in the scene. `D00105` is closed.

### v0.13

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

