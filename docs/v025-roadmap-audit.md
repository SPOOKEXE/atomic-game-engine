# v0.25 roadmap audit

This audit reviews all 49 items originally marked complete under v0.25 in [ROADMAP.md](../ROADMAP.md). It separates checked-in work from demonstrated behavior. A source finding means the named code or test exists and was inspected. It does not certify every visual, performance, or live Studio outcome.

On September 24, 2026, `just preset=ci check` passed with 626 suites, zero failures, plus architecture, source, shader, binding, type, determinism, replay, client smoke, and orphan checks. The ordinary test runner excludes opt-in GPU cases. The approved `just preset=ci render-check` reached 132 GPU cases: 126 passed and 6 failed after the focused fixes below. `just preset=ci studio-smoke` loaded, played, and captured its example scenes. In a live Studio session, a new world and Part were created, the Part's X position was edited to 2.000, and Play started and stopped a client instance. The saved game reopened with four worlds, including the new world and serialized position, but the selected Worlds dock displayed the Live Instances "nothing is running" content after reopen. Release benchmarks remain unverified.

| Item | Roadmap claim | Finding | Evidence and limit |
|---:|---|---|---|
| 1 | Organize documents | Source | DEMOS, ECS_COMPONENTS, and schema files are under docs. The suggested systems folder was optional. |
| 2 | Keep RUNNING as a complete command index | Corrected | Seven public recipes were absent and are added to [RUNNING.md](../RUNNING.md). |
| 3 | Improve schema files | Source | [schema.toml](schema.toml) defines layout; [schema-data.toml](schema-data.toml) contains named records. |
| 4 | Improve seven policy/reference documents | Source | The named files have short opening descriptions. |
| 5 | Clean Doxygen layout | Unverified | Documentation recipes exist in [Justfile](../Justfile), but a successful generation does not prove every page's layout. |
| 6 | Check and prune old documentation statements | Unverified | Deleted and consolidated documents are visible in history. Exhaustive factual review cannot be established from file presence. |
| 7 | Release stale mesh LOD resources | Source | [AutomaticMeshLod.cpp](../mono.engine/render/src/AutomaticMeshLod.cpp) invalidates old artifacts; [tests](../mono.engine/render/tests/AutomaticMeshLod.cpp) cover replacement and release. The full GPU group failed on other cases. |
| 8 | Multi-select property editing | Source | [PropertySelection.cpp](../mono.studio/src/PropertySelection.cpp) intersects shared properties; [tests](../mono.studio/tests/PropertySelection.cpp) cover mixed values and edits. |
| 9 | Per-item LOD distance | Source | [LevelOfDetail.hpp](../mono.engine/scene/include/engine/scene/LevelOfDetail.hpp) and [Studio tests](../mono.studio/tests/LodPreview.cpp) cover the override. |
| 10 | Split LODAuto, LODCustom, LODSettings | Source | The three types and overlay behavior have [scene tests](../mono.engine/scene/tests/LevelOfDetail.cpp). |
| 11 | Rename LOD preference | Source | [Studio settings](../mono.studio/src/Settings.cpp) use the requested label. |
| 12 | Bladeborne camera | Unverified runtime | [BladeborneDemo.aworld](../mono.engine/examples/assets/worlds/BladeborneDemo.aworld) authors the camera; its [test](../mono.engine/examples/tests/BladeborneDemo.cpp) checks source content rather than live framing. |
| 13 | Lights through portals | Source, GPU gate failed | [client portal lighting tests](../mono.client/tests/PortalLighting.cpp) and [GPU transport tests](../mono.engine/render/tests/PortalShadowTransportIntegration.cpp) exist. The full GPU group failed on other cases. |
| 14 | Free camera tests | Source | [SurfaceCameras.cpp](../mono.engine/scene/tests/SurfaceCameras.cpp) covers crossing and camera routes. |
| 15 | Character tests | Source | [Characters.cpp](../mono.engine/scene/tests/Characters.cpp) covers character behavior. |
| 16 | Character camera tests | Source | [client portal walk](../mono.client/tests/PortalWalk.cpp) and [replication tests](../mono.client/tests/Replicated.cpp) cover camera and zoom routes. |
| 17 | Reduce build disk use | Unverified measurement | [Justfile](../Justfile) provides compiler-cache sizing and pruning. No before and after disk baseline was found. |
| 18 | Prune named old plans | Source | The named plans were removed in repository history. |
| 19 | Prune future-work documents | Source | Component notes were moved to topical documents and obsolete content removed in history. |
| 20 | Plan render cleanup | Source | [render cleanup plan](v025-RENDER-PIPELINE-CLEANUP.md) exists with ownership and verification stages. |
| 21 | Plan MCP cleanup | Source | [MCP cleanup plan](v025-MCP-CLEANUP.md) exists with contracts and stages. |
| 22 | Light path visualizer | Partial | [RuntimeDiagnostics.cpp](../mono.engine/render/src/RuntimeDiagnostics.cpp) draws blue influence segments and red first-bound stops. PassThrough and Reflection are reserved but never emitted, so requested orange events are missing. |
| 23 | SkyGridPBR benchmark demo | Source | [BenchmarkSkyGrid.luau](../mono.engine/examples/assets/scripts/BenchmarkSkyGrid.luau) has eight variants and a moving camera; [tests](../mono.engine/examples/tests/BenchmarkSkyGrid.cpp) inspect the constructed scene. |
| 24 | Unique and shared 4K PBR stress demos | Source, performance unverified | [unique](../mono.engine/examples/assets/scripts/PbrTextureUniqueStress.luau) and [shared](../mono.engine/examples/assets/scripts/PbrTextureInstanceStress.luau) demos exist. Device residency and timings were not measured here. |
| 25 | Virtual camera lock for all behavior | Partial | [Editor.cpp](../mono.studio/src/Editor.cpp) freezes culling and LOD visibility. Inspection CameraFrame remains live, so the literal all-camera-behavior claim is broader than the implementation. |
| 26 | Packed render support channels | Source | [data capture tests](../mono.engine/render/tests/DataCapture.cpp) cover packed lanes for supporting render data. |
| 27 | Complete render pipeline cleanup | Partial | Compiler, registry, executor, and frame/resource owners are extracted. [PortalImageRuntime.cpp](../mono.engine/render/src/portal/PortalImageRuntime.cpp) still calls Renderer directly; the [plan](v025-RENDER-PIPELINE-CLEANUP.md) GPU gate failed. |
| 28 | Complete MCP cleanup | Partial | Owner-tagged hooks and several contract fixtures exist, but [MCP completion criteria](v025-MCP-CLEANUP.md) 1, 4, and 5 are unmet. The seven-mode manifest set, typed hook contexts and resource ownership, and unified product manifests remain incomplete. Legacy unowned registration remains. |
| 29 | Stress every engine system and find five optimizations each | Partial | [stress audit](ENGINE_STRESS_AUDIT_2026-09-22.md) records a 200-player run and opportunities, but benchmarks only 22 of 31 engine modules and does not establish all proposed speedups. |
| 30 | Physics observation hooks | Source | [typed hook contract](../mono.engine/physics/include/engine/physics/Observation.hpp), [tests](../mono.engine/physics/tests/Observation.cpp), and MCP adapter exist. |
| 31 | Replication observation hooks | Source | [typed hook contract](../mono.engine/replication/include/engine/replication/Observation.hpp), [tests](../mono.engine/replication/tests/Observation.cpp), and MCP adapter exist. |
| 32 | Optimize server startup | Unverified measurement | No before and after startup timing record was found. The stress audit measures runtime ticks and admission instead. |
| 33 | Improve deterministic server and physics tests | Partial evidence | Direct server tick and observation tests exist, but the requested general immediate-wait test improvements were not established across both subsystems. |
| 34 | Billboard LOD | Source | [render selection](../mono.engine/render/src/ViewRecording.cpp) and [scene tests](../mono.engine/scene/tests/LevelOfDetail.cpp) cover billboard LOD. |
| 35 | Future UI specification | Source | [ui-system.md](future-work/ui-system.md) exists. |
| 36 | Plan seamless portals | Source | [seamless portal plan](v025-SEAMLESS-PORTALS.md) defines stages and an acceptance matrix. |
| 37 | Implement seamless portals plan | Partial | Body splitting, transfer fences, and dynamic island solving have production paths and tests. The [acceptance matrix](v025-SEAMLESS-PORTALS.md) still lacks its frame-rate, network, resolution, profiling, and approved GPU runs. |
| 38 | Forgotten replica rows | Source | [client handling](../mono.client/src/Replicated.cpp) and [arrival tests](../mono.client/tests/ReplicaArrival.cpp) cover cleanup. |
| 39 | Move recovery-row ByteWriter buffer | Source, figures unverified | [Authority.cpp](../mono.engine/replication/src/Authority.cpp) uses TakeBytes. The quoted 15-sample benchmark raw data was not found. |
| 40 | Refine replication prefix with nth_element | Source, figures unverified | [Authority.cpp](../mono.engine/replication/src/Authority.cpp) selects and sorts the prefix; packet-order tests exist. The quoted A/B raw data was not found. |
| 41 | Bloom, depth of field, god rays, fog, clouds | Partial GPU validation | [GPU compositor tests](../mono.engine/render/tests/CompositorDemoGpu.cpp) and example authoring exist. The authored compositor GPU workflow failed to set its pipeline. |
| 42 | Weather demo | Source, visual unverified | [Weather.luau](../mono.engine/examples/assets/scripts/Weather.luau) and [tests](../mono.engine/examples/tests/Weather.cpp) cover authored layers. |
| 43 | Self-contained TornadoSim world | Source, interactive unverified | [TornadoSim.aworld](../mono.engine/examples/assets/worlds/TornadoSim.aworld) embeds scripts; its [test](../mono.engine/examples/tests/TornadoSim.cpp) checks document content. Lower-level storm, physics, and GPU tests exist. |
| 44 | Studio GUI Preview Controls | Source | [Studio view tests](../mono.studio/tests/Viewports.cpp) cover hidden default and enabled interaction. |
| 45 | Remove HarfBuzz notice | Source | [vendor configuration](../mono.build/MonoVendor.cmake) suppresses the notice. |
| 46 | Combined lighting validation | Source, GPU gate failed | [VolumeGpu.cpp](../mono.engine/render/tests/VolumeGpu.cpp) tests overlap and compute outputs. The full GPU group failed, so combined visual behavior is not certified. |
| 47 | Lighting stress and measured optimization | Partial evidence | [LightingStress.cpp](../mono.engine/examples/tests/LightingStress.cpp) covers 256 lights and 256 fog volumes; timing paths exist, but no before and after bottleneck measurements were found. |
| 48 | TornadoSim reference comparison | Source, artifacts unverified | Storm and GPU tests plus [port notes](v025-TORNADOSIM-PORT.md) cover samples and captures; raw matched image artifacts were not found in this audit. |
| 49 | Representative live Studio workflow | Partial verification | [Studio smoke](../RUNNING.md) loaded, played, and captured examples. The approved live session created a world and Part, edited its X position, started and stopped Play, then saved and reopened the game. The reopened document retained the world and position, but its selected Worlds dock showed the Live Instances "nothing is running" content, so the dock needs a repeat check. |

## Confirmed follow-up work

1. Finish truthful light pass-through and reflection diagnostics, or narrow the feature contract to local-light influence and update the roadmap.
2. Complete the render cleanup ownership boundary and run its GPU gate.
3. Finish the MCP cleanup criteria in [its plan](v025-MCP-CLEANUP.md). This audit fixed a discovery inconsistency: a draining hook appeared in `negotiate.operations` while `tools/list` hid it. [Discovery.cpp](../mono.engine/control/src/Discovery.cpp) now filters both views consistently, with [regression coverage](../mono.engine/control/tests/HookRegistry.cpp). The client, server, and Studio fixtures now include the chunked snapshot operations added concurrently.
4. Complete the stress audit coverage and record startup and lighting performance baselines and results.
5. Run the portal acceptance workload matrix and preserve its reports with backend, revision, resolution, and network conditions.
6. Resolve the six failing GPU cases: opaque body composition, renderer exchange, nested aperture composition, retained glass depth, retained camera packet, and lens content ownership. Repeat the full GPU group after each root cause is addressed. The dedicated tessellation and wireframe sampler crashes, quiet cloud upload churn, compositor fixture ordering, and four stale GPU fixture expectations were fixed during this audit.
7. Diagnose the Worlds dock showing Live Instances content after reopening a saved game and repeat the live Studio workflow with a representative capture. The headless smoke passed, but its MeshGrid viewport capture was only 43 by 517 pixels under the existing layout.
