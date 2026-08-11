
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
- [x] raw folders bake on demand into the editor, memory-only by default, with a tab of their own
- [x] C++ node library suite via imgui in ~/Documents/GitHub/node-graph-template/cpp — model (graph, types, layout, hashing, evaluation, save/load) with no imgui and 93 checks, an imgui canvas over it, and an SDL3 demo
- [x] SETUP-CDN.md: folder-based, a store served from a folder, an origin on localhost and how to expose one; `cdn --ingest-key` added so the editor's Upload has an origin that can accept
- [x] investigated non-euclidean worlds: `docs/NON-EUCLIDEAN.md`. cameras + portal parts is right, and `SurfaceCamera` is most of it — three small changes to how a surface view carries its projection, then traversal, which needs v0.15's character controller
- [x] **portals, and the non-Euclidean spaces they buy** — the first four rows of `NON-EUCLIDEAN.md`'s table, built in the order it argued for because each unblocks the next. `render::SurfaceView` carries a **projection** rather than a field of view; the frustum is fitted **off-axis** to the pane's four corners, which the surface camera already wanted since a symmetric fit spends half a mirror's texels on the far edge it cannot see; and the near plane is skewed onto the destination's plane with a real **oblique clip** — Lengyel's, in its `0..1` depth form, because `GLM_FORCE_DEPTH_ZERO_TO_ONE` is pinned engine-wide and the published `-1..1` derivation would land the near plane somewhere that reads as z-fighting. Then a `Portal` class deriving from `SurfaceCamera` with one `Destination` reference. **The four steps collapse into one code path**: take a placement transform, map the pane by it, fit the camera to the mapped rectangle — a mirror's transform is the reflection through its own plane, which *fixes* that plane, so the mapped rectangle is the pane itself and the old arithmetic falls out of the general rule. The fit must be to the mapped **source** pane and never the destination part, because `opaque.frag` shades a source fragment through the camera's matrix and the two only line up because they moved together. No second renderer, no exotic maths: the impossible space is that nothing constrains the pair of frames to describe one. **The one thing the engine cannot catch is authoring, and the demo made the mistake before the test did**: `Face` is resolved on the *destination* as well, so a hole aimed at a wall whose matching face points out of its room places a camera, fits a frustum, clips obliquely and shows the empty space behind that wall — the first `Portals-1-world.luau` paired north walls with south walls, which is what a corridor looks like on paper, and all four holes rendered nothing. The invariant that catches it is not where the camera stands but that the half-space the clip keeps contains the middle of the room the hole names. `Portals-1-world.luau` now has three rooms 300 apart and six holes — one pair an ordinary adjacency explains, and two that put the vault through both the hall's east and west walls — and it is the fifth world a new game opens with, beside the skygrid, the mirrors, the meshes and the slide. `scripts/demos/run-portals.sh`, `scene/SurfaceCameras.hpp`, `scene::Portal`, `scene::SurfaceLens`
- [x] deferred `D00112`'s traversal half closed below — what remains is the one-frame seam on crossing, which needs the surface pass to recurse in-frame
- [x] as many viewports as somebody wants: the fixed four became a grown list, panels own their imgui titles, and the asset preview's render slot is computed past the last panel rather than fixed — `--viewports N` and a New Viewport menu item
- [x] every shader moved to engine/resources/shaders, owned by a module of its own
- [x] add custom physical properties for BasePart but as a separate PhysicsProperties component. need things like friction, drag, density, etc. Make them visible in properties too.
- [x] create a shared script based but separate LuaSourceContainer and JavaScriptSourceContainer as two separate components.
- [x] add a way to select between luau and javascript for the code as properties for the Script/LocalScript/ModuleScript. Make it a separate component so we can keep both code containers but swap between them. Maybe CodeSourceContainerSelector? This way we can make CodeSourceContainerSelector scriptable but do not allow editing scripts from other scripts by LuaSourceContainer and JavaScriptSourceContainer.
- [x] **autocomplete for luau and js scripting, in the studio's own editor** — pulled forward from v0.15 because the external half was already done: `luau-lsp` and the generated `.d.luau`/`.d.ts` have completed in VS Code since v0.5, and the only place the engine's vocabulary could not be reached was the editor this repository ships. Ctrl+Space, or automatically after a `.`, a `:` or two characters; Enter accepts and Tab still indents, because `imgui_widgets.cpp` asserts that `AllowTabInput` and `CallbackCompletion` are never both set. **Nothing about the surface is written down**: classes, properties and enums come from `ecs::Classes` and `ecs::EnumTable`, and the globals and instance methods from a new `Runtime::Surface` that *walks a freshly built VM* — `_G` in Luau, `globalThis` and `__instanceMethods` in QuickJS. So a global added anywhere in `mono.engine/script` is offered with nothing else changing. That shape was chosen because this module has shipped the other one twice: `Values.cpp` records `Magnitude` and `Unit` promised by `engine.d.luau` for two versions while the run time answered "no member", and `JsSurface.cpp` records a hand-written `10` on a list of sixteen methods. The one surface a walk cannot reach is the fourteen instance signals, which are a branch chain — those are listed beside the chain and `engine.script.vocabulary` checks every offered member against a live VM in both languages. Also offers the tree beside the script, which is the one thing an external language server cannot know. `studio/Complete.hpp`, `script/Vocabulary.hpp`
- [x] **blocky character spawning, and the two engine holes it fell into.** A character is a `Model`, a `HumanoidRootPart` that is the whole capsule, and five anchored limbs carried by `scene::PoseCharacters` — Roblox welds six colliders with `Motor6D`s, and that needs joints, a solver that respects them and a mass distribution nobody authored, none of which changes what a player feels. **The first hole: a sleeping body has no `scene::Motion`.** `physics::Publish` takes it away — the archetype move *is* what sleeping is here — and until now the only thing that ever gave it back was a contact, so a character that stood still settled and could never be walked again: `StepCharacters` had a perfectly good move direction and nowhere to write it. `PhysicsWorld::Wake` is the verb that was missing. **The second: nothing marked an integrated transform changed.** `IntegrateMotion` has said since v0.4 that a consumer needing a replication delta out of an integrated world "marks it in its own publish step", and for three versions there was no such consumer — so a physics-driven body was invisible to `replication` and a character walked on the server and stood still on every client. `ecs::Store::MarkChanged` and one line in `physics::Publish`. **The third: the ground ray hit the character.** It starts *inside* the feet on purpose — a ray that begins exactly on a face is a coin flip about whether it hits it, and the coin lands differently on two machines — so with a root collider the full height of the character the nearest hit is always the character, and `hit->Owner != body` afterwards reads "not grounded" while standing on a floor, for ever. Testing the answer cannot recover it: the floor was never in it. `physics::Raycast` takes an entity to look through, which is what its own comment had said it would eventually want. `scene/Characters.hpp`, `physics/Characters.hpp`
- [x] **very basic character controls (WASD, SPACE), scoped to the body that is yours.** The arithmetic existed and drove *every* humanoid in the world, which is invisible with one player and is two machines fighting over one body with two. `scene::ReadMoveIntent` is the keyboard half split out so a connected client can send an intent without applying one, and `UpdateCharacterControl` now drives the `LocalPlayer`'s character alone — a world with no `LocalPlayer` still drives them all, because there the question has no answer
- [x] **local server: one server, several clients on one machine.** `scripts/demos/run-local-server.sh`, and two things had to exist first. **`scene::LocalPlayer` had no writer anywhere** — it has said since v0.10 that it holds this viewer's player, so `Players.LocalPlayer` was nil on every client that ever ran; it cannot be replicated, because a resource is one row and the answer differs per client, so it travels as a per-client message over `replication::MessageKind::User`. And **movement goes up as intent rather than as state**: `game::MoveInput` says which way a client is walking and the host decides where that puts it, which keeps one simulation and does not hand a client the ability to state where its own body is. `game/Play.hpp`, and `mono.server/tests/Replication.cpp` walks a character with two real clients over a real socket
- [x] **TeleportService, between worlds and natively in the studio — and the bus bug it uncovered.** The Teleport bus, `Postbox::Teleport` and the router's delivery have existed since v0.2, no script could reach any of it, and `PumpDeliveries` dropped every arrival on the floor. Now: a service in both VMs, and the engine admits the arriving player rather than a script, because who is in a game is the host's business. **A barrier is not a tick, and the router assumed it was.** A barrier runs once per host frame and a world's systems run at the world's own rate — the studio measured 200 barriers against 91 ticks — so replacing an inbox unconditionally took mail away before any system saw it, and a world owing catch-up ticks read the same mail once per tick. A teleport was delivered and lost by the first and admitted three times by the second. Two rules now: an empty barrier leaves a full inbox alone, and a world's catch-up ticks after the first see an empty one. The studio follows its player across worlds by name, because a name is the only thing a teleport carries
- [x] **the character through a portal — `D00112`'s traversal half.** It was deferred as blocked on a body to move, and the body arrived early. `scene::CrossPortals` tests the *segment* between `PreviousTransform` and `Transform` for a change of side through the pane, inside the pane's rectangle — a position test misses the tick a walking character was on either side of — and multiplies the body **and its velocity** by the same `destination · half-turn · source⁻¹` the camera goes through. Derived rather than read off `SurfaceLens::Mapping`, because that is presentation and a headless server never aims a surface at all. The seam on the frame you cross is still open and is a render-graph decision
- [x] **studio Play is both halves with somebody in them.** `PlayLink` has held an authority and a replica since v0.7 and nobody was in the replica. It now admits a `Player`, gives it a character and tells the replica which player it is; a viewport showing a client with a body is *played* rather than flown, and looks through that client's own camera. **Spawn Player and Remove Player act on the viewport you are in**, so a Run gains clients one at a time and becomes a Play — and `BeginRun` goes through the same `SpawnPlayer`, so there is one path that admits somebody. A new game opens with two more worlds, `Playground` and `Arena`, with a teleport pad between them
- [x] **a Live Instances panel, and the view that had no way back.** A run is not a scene, and the Worlds panel was being asked to be both: a client's replica had a row among the authored worlds — which is what made its viewport reopenable — while the *server's* view had no row anywhere, because the server is not a separate world at all. Closing that panel lost it. Live Instances lists the run and its clients with a `View` on each, opens itself when a run starts, and holds the counts that used to be a tooltip on a Worlds row. Two things fell out of it: the transport now scopes a **client** viewport to the run that owns it — a replica names no run, so `ModeOf` answered "Edit" and Play from a client panel would have started a *second* run inside somebody's view — and Stop while looking at a client removes that client rather than the scene. `studio/Viewports.hpp` is the panel choice, in a header a test can reach, because opening a second view of a world that is already on screen halves the refresh rate of both and reads as the editor being slow
- [x] **the overlay in a client viewport was aimed at the editor's camera.** `PresentWorld` reads a replica's `ActiveCamera` back after presenting it — that is what makes a client view a client's view — and `ProjectionFor` still built its matrix from the panel's free camera. So the grid slid across the floor as the player walked, a selection outline sat beside the part it outlined, and a click picked whatever was under a camera nobody was looking through. The grid is also gone from a client view entirely: it is authoring furniture, a replica carries no run record so `ModeOf` called it Edit, and the panel it was drawn over is the one picture in this editor that is exactly what a player sees
- [_] deferred `D00111`: listing an HTTP origin's contents — its tab names the address and says why it cannot enumerate
- [_] deferred `D00110`: a variety of default shaders, once something can select one
- [_] deferred `D00114`: no type inference — a local from `Instance.new("Part")` resolves because the class is on the line, one from `FindFirstChild` falls back to the union of every scriptable property
- [_] deferred `D00115`: a character's limbs pay wire for transforms the receiver overwrites, because replication filters by component and not by entity
- [_] deferred `D00116`: a client cannot ask its server to teleport it — the binding refuses on a replica and there is no request channel
- [_] immersive portals (add a new ImmersivePortal of which does cross-world teleportation in real-time, can enter the next world immediately and shows the other world's side through a cross-world texture rendering). Also supports custom logic like "OnPlayerEnterNearby, OnPlayerLeavingNearby, OnPlayerEnterImage, OnPlayerLeftImage", etc. Also custom properties like the rate of which the image updates, range for nearby event, etc. Need a bus system for this.
- [_] i also realised that i don't have a way to control whether changes are client-sided or server-sided when i edit in the explorer/properties. how could i do this considering multiple clients + server support - with run-mode only and play-mode.

### v0.15

- [_] also extra prototype project for rendering pipeline.
- [_] thoroughly implement all user interface elements + surfacegui + billboardgui
- [_] thoroughly implement user input system
- [_] thoroughly implement common services
- [_] thoroughly implement extra functions like PlayerGui:...
- [_] add accessories support

### v0.16

- [_] animation handler
- [_] character controller + humanoid + character states + state controller, etc. More modular than roblox standard humanoid. state machine? node graphs? etc.

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
