
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

- [x] deferred `D00102`'s decided half — `Engine::bakegraph`, the node vocabulary and the bake document format at `shared` tier, linking `Engine::core` and nothing else. **The point is what it does not link.** `bake` is the only thing in the engine that reads a `.glb`, a `.pmx`, a `.png` or a `.bmp`, and its build file states that nothing a shipped game links may link it — so a save format that named `bake` to parse a *text document* would have put every one of those decoders into `server`, which has no reason to decode a JPEG. The register weighed three ways out and settled on the module split as the honest fix; carrying pipelines as opaque text would have given up the load-time validation the render half has, and putting them on the universe rather than the world trades a dependency question for an ownership question nobody has answered. **It was cheaper than the register assumed**: `GraphDocument` needed only `Graph.hpp` and `core`, `Graph.hpp` needed `assets` — which is what a baked mesh *is* rather than a decoder — and only `Graph.cpp`, the evaluator, reaches an importer at all. `Build` is the one function needing both halves and stayed in `bake`; `IsBareNode` became public because a closed list of parameterless node kinds is precisely the thing that must not be copied. The suites split with the code, so `bakegraph`'s proves the format round-trips with no importer linked. **The `<AssetPipelines>` block was deliberately not written**: `Editor::DrawAssetsPipeline`, `WritePipelines`/`ReadPipelines` and `client::InstallWorldPipelines` are all `TODO(render-pipeline)` markers — the panel this entry described went out with the same revert that took `D00038`'s multi-view seam — and a save-format section with no producer and no consumer is a feature that looks present and is not. The format break is cheap to take later precisely because the module split was the expensive part.

- [x] deferred `D00039` closed — the physics module had a caller for the first time. It was complete, tested, benchmarked and connected to nothing: `RegisterPhysicsSystems` was called from `physics/tests/` and nowhere else, and every world this engine shipped was anchored throughout, so integrate, broad phase, narrow phase and solver had never run against a real scene. **Two things were missing and only one of them was the wiring.** `physics` deliberately has no gravity — its own suites say a top-down game should not have to switch one off, so `RigidBody` carries no gravity scale and the pipeline has no gravity step — which means wiring the pipeline alone integrates every body at zero acceleration for ever. That is not hypothetical: the new case failed exactly that way when it was first written against the pipeline alone, which is how the missing half was found. `scene::Gravity` is the second feature: a resource, so a world under water or on the moon authors its own vector, defaulting to 9.81 m/s² down — Earth's in metres, because this engine measures parts in metres and Roblox's 196.2 is the same acceleration in studs. The editor installs both on every world through one `PrepareWorld`, which also closed a smaller bug on the way: five call sites each wrote the same three lines and the one that wrote two left a world restored by Stop without services. A new **Slide** world is the demonstration — a curved ramp of overlapping boxes, blocks spawning at the top and destroyed ten seconds later — chosen over a stack because a stack tests the solver and nothing else, where a slide asks for all four steps at once and each fails visibly. A block was measured leaving the ramp at x 3.4 after 4.9 s and the earlier ones resting by tick 600. `just determinism` is unaffected: the server was deliberately not wired, because giving it suspension-like behaviour changes a program whose output that recipe compares byte for byte.

### v0.14

- use tools and see what needs fixing
- use tools and see what to improve
- blocky character spawning
- animation handler
- character controller + humanoid + character states + state controller, etc. More modular than roblox standard humanoid. state machine? node graphs? etc.

### v0.?? (needs prototype project first)

- [_] extended rendering pipeline (handle multiple worlds in parallel, handling gpu traffic)
- [_] rendering pipeline is a node system with a studio editor
- [_] surfaceapperance actually integrated with new pipeline
- [_] render pipeline is per world, not per process
- [_] render pipelines are saved in the world's export data
- [_] rendering pipeline debugger and profiler - shows a graph's per-step operations with their compute costs and wall clock, etc.
- [_] rendering pipeline additions; ambient occulusion, emissive, pbr, default node setup with all these

