
# ROADMAP

## Editing

Do NOT add new deferred work as a roadmap item. Place it in
`docs/DEFERRED.md`. If a TODO item is not FULLY completed, split the
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

### v0.14

- [x] rojo: `--rojo`/`$ATOMIC_ROJO_PROJECT` syncs any project at startup; checked against raceapet, which found and fixed two build bugs (a package's `default.project.json` was joined to its folder instead of replacing it, and a nested project's root `$path` was ignored)
- [x] the topbar is tab-based, and the plugins toolbars are a tab of their own rather than a panel
- [x] a Demo tab on the ribbon, and a Demo Nodes panel: a typed node graph with a cycle guard, cached evaluation, save/load and an imgui canvas — `studio/NodeGraph.hpp`
- [x] `PhysicsProperties` on BasePart: density, friction and elasticity as an override of the material, drag through `RigidBody`'s damping, `Mass` derived and read-only, all of it in the properties panel
- [x] `LuaSourceContainer` and `JavaScriptSourceContainer` as two components, with `CodeSourceContainerSelector` choosing which runs — the selector is scriptable and neither container is, which needed `ecs::PropertyDescriptor::Scriptable`
- [x] the full demo: fBm and ridged noise, warp, terrace, slope, threshold and combine; a colouriser and thumbnails on the nodes; erosion and staged tasks that run off the frame with progress, stages and concurrent branches
- [x] the Demo Nodes panel built out to the reference implementation: a picture and a description on the *data type* rather than the node, so an inspector can draw a node's inputs beside its output; per-type inspector handlers picked by name and otherwise inferred from what a node produced; Library, Inspector and Types tabs with live parameters and a port table; snapshot undo, copy/paste, save/load and image export; frames, collapsing, a context menu, insert-on-link and a filtered palette; and compression — a selection folds into one node whose ports are derived from how it was wired, with the fold as a *view over one graph* rather than a nested one, so the evaluator, the cycle guard and the content hash never learn it happened
- [x] the last of it: a 3-D view of any wire that can be read as elevation (`DataType::Heights`, a software rasteriser producing the same `PreviewImage` as everything else, so no render target is involved), a custom library — a fold files as a named type inside the document rather than outside it, and places independent copies through one `Graph::Absorb` that paste also uses — and a PNG encoder, so an exported picture is a picture
- [x] deferred `D00113`: one node graph implementation rather than two
- [x] default engine assets: the six shape meshes and a pink/grey checkerboard, under an "engine" tab, with a tab per cdn source and an "all" tab
- [_] deferred `D00111`: listing an HTTP origin's contents — its tab names the address and says why it cannot enumerate
- [x] raw folders bake on demand into the editor, memory-only by default, with a tab of their own
- [x] C++ node library suite via imgui in ~/Documents/GitHub/node-graph-template/cpp — model (graph, types, layout, hashing, evaluation, save/load) with no imgui and 93 checks, an imgui canvas over it, and an SDL3 demo
- [x] SETUP-CDN.md: folder-based, a store served from a folder, an origin on localhost and how to expose one; `cdn --ingest-key` added so the editor's Upload has an origin that can accept
- [x] investigated non-euclidean worlds: `docs/NON-EUCLIDEAN.md`. cameras + portal parts is right, and `SurfaceCamera` is most of it — three small changes to how a surface view carries its projection, then traversal, which needs v0.15's character controller
- [_] deferred `D00112`: build the portal once the surface view carries a projection
- [x] as many viewports as somebody wants: the fixed four became a grown list, panels own their imgui titles, and the asset preview's render slot is computed past the last panel rather than fixed — `--viewports N` and a New Viewport menu item
- [x] every shader moved to engine/resources/shaders, owned by a module of its own
- [_] deferred `D00110`: a variety of default shaders, once something can select one

- [x] add custom physical properties for BasePart but as a separate PhysicsProperties component. need things like friction, drag, density, etc. Make them visible in properties too.
- [x] create a shared script based but separate LuaSourceContainer and JavaScriptSourceContainer as two separate components.
- [x] add a way to select between luau and javascript for the code as properties for the Script/LocalScript/ModuleScript. Make it a separate component so we can keep both code containers but swap between them. Maybe CodeSourceContainerSelector? This way we can make CodeSourceContainerSelector scriptable but do not allow editing scripts from other scripts by LuaSourceContainer and JavaScriptSourceContainer.
- [x] **autocomplete for luau and js scripting, in the studio's own editor** — pulled forward from v0.15 because the external half was already done: `luau-lsp` and the generated `.d.luau`/`.d.ts` have completed in VS Code since v0.5, and the only place the engine's vocabulary could not be reached was the editor this repository ships. Ctrl+Space, or automatically after a `.`, a `:` or two characters; Enter accepts and Tab still indents, because `imgui_widgets.cpp` asserts that `AllowTabInput` and `CallbackCompletion` are never both set. **Nothing about the surface is written down**: classes, properties and enums come from `ecs::Classes` and `ecs::EnumTable`, and the globals and instance methods from a new `Runtime::Surface` that *walks a freshly built VM* — `_G` in Luau, `globalThis` and `__instanceMethods` in QuickJS. So a global added anywhere in `mono.engine/script` is offered with nothing else changing. That shape was chosen because this module has shipped the other one twice: `Values.cpp` records `Magnitude` and `Unit` promised by `engine.d.luau` for two versions while the run time answered "no member", and `JsSurface.cpp` records a hand-written `10` on a list of sixteen methods. The one surface a walk cannot reach is the fourteen instance signals, which are a branch chain — those are listed beside the chain and `engine.script.vocabulary` checks every offered member against a live VM in both languages. Also offers the tree beside the script, which is the one thing an external language server cannot know. `studio/Complete.hpp`, `script/Vocabulary.hpp`
- [_] deferred `D00114`: no type inference — a local from `Instance.new("Part")` resolves because the class is on the line, one from `FindFirstChild` falls back to the union of every scriptable property

### v0.15

- [_] blocky character spawning
- [_] animation handler
- [_] character controller + humanoid + character states + state controller, etc. More modular than roblox standard humanoid. state machine? node graphs? etc.
- [_] local server (one server multiple clients on same machine)
- [_] check teleportservice works between worlds and it works in studio natively

### v0.16

- [_] also extra prototype project for rendering pipeline.
- [_] thoroughly implement all user interface elements + surfacegui + billboardgui
- [_] thoroughly implement user input system
- [_] thoroughly implement common services
- [_] thoroughly implement extra functions like PlayerGui:...
- [_] add accessories support

### v0.?? (needs prototype project first)

- [x] ~/Documents/GitHub/node-graph-template
- [_] extended rendering pipeline (handle multiple worlds in parallel, handling gpu traffic)
- [_] rendering pipeline is a node system with a studio editor
- [_] surfaceapperance actually integrated with new pipeline
- [_] render pipeline is per world, not per process
- [_] render pipelines are saved in the world's export data
- [_] rendering pipeline debugger and profiler - shows a graph's per-step operations with their compute costs and wall clock, etc. also shows the images/masks/etc used for each step
- [_] rendering pipeline additions; ambient occulusion, emissive, pbr, default node setup with all these
- [_] https://www.youtube.com/watch?v=SnNm7rSSvlg (Threat Interactive Tutorial: How To Optimize Almost Every Step In Modern Game Rendering)
- [_] https://github.com/fini03/vkDuck
