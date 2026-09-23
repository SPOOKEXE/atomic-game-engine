# v0.25 roadmap audit

This audit reviews all 49 items originally marked complete under v0.25 in [ROADMAP.md](../ROADMAP.md). It separates checked-in work from demonstrated behavior. A source finding means the named code or test exists and was inspected. It does not certify every visual, performance, or live Studio outcome.

On September 24, 2026, the final `just preset=ci check` passed with 629 suites, zero failures, plus architecture, source, shader, binding, type, determinism, replay, client smoke, and orphan checks. The ordinary test runner excludes opt-in GPU cases. The approved `just preset=ci render-check` rerun passed all 132 GPU cases with 1,677,900 assertions. The six failures came from a half-resolution lens fixture consumed at full resolution, emissive reference fixtures lacking a nonzero strength, and a portal demand expectation that omitted seam radiance. `just preset=ci studio-smoke` loaded, played, and captured its example scenes. A later live Studio session reopened the saved four-world game with the Worlds dock visible, inserted a Part, changed its X position to 3.000, played it, and stopped. Reopening and stopping have focused dock-focus fixes, and a live Play to Stop rerun captured the Worlds dock with its world rows visible. Release benchmarks remain unverified.

| Item | Roadmap claim | Finding | Evidence and limit |
|---:|---|---|---|
| 1 | Organize documents | Source | DEMOS, ECS_COMPONENTS, and schema files are under docs. The suggested systems folder was optional. |
| 2 | Keep RUNNING as a complete command index | Corrected | Seven public recipes were absent and are added to [RUNNING.md](../RUNNING.md). |
| 3 | Improve schema files | Source | [schema.toml](schema.toml) defines layout; [schema-data.toml](schema-data.toml) contains named records. |
| 4 | Improve seven policy/reference documents | Source | The named files have short opening descriptions. |
| 5 | Clean Doxygen layout | Unverified | Documentation recipes exist in [Justfile](../Justfile), but a successful generation does not prove every page's layout. |
| 6 | Check and prune old documentation statements | Unverified | Deleted and consolidated documents are visible in history. Exhaustive factual review cannot be established from file presence. |
| 7 | Release stale mesh LOD resources | Source | [AutomaticMeshLod.cpp](../mono.engine/render/src/AutomaticMeshLod.cpp) invalidates old artifacts; [tests](../mono.engine/render/tests/AutomaticMeshLod.cpp) cover replacement and release. The full GPU group passes. |
| 8 | Multi-select property editing | Source | [PropertySelection.cpp](../mono.studio/src/PropertySelection.cpp) intersects shared properties; [tests](../mono.studio/tests/PropertySelection.cpp) cover mixed values and edits. |
| 9 | Per-item LOD distance | Source | [LevelOfDetail.hpp](../mono.engine/scene/include/engine/scene/LevelOfDetail.hpp) and [Studio tests](../mono.studio/tests/LodPreview.cpp) cover the override. |
| 10 | Split LODAuto, LODCustom, LODSettings | Source | The three types and overlay behavior have [scene tests](../mono.engine/scene/tests/LevelOfDetail.cpp). |
| 11 | Rename LOD preference | Source | [Studio settings](../mono.studio/src/Settings.cpp) use the requested label. |
| 12 | Bladeborne camera | Unverified runtime | [BladeborneDemo.aworld](../mono.engine/examples/assets/worlds/BladeborneDemo.aworld) authors the camera; its [test](../mono.engine/examples/tests/BladeborneDemo.cpp) checks source content rather than live framing. |
| 13 | Lights through portals | Source, GPU gate passed | [client portal lighting tests](../mono.client/tests/PortalLighting.cpp) and [GPU transport tests](../mono.engine/render/tests/PortalShadowTransportIntegration.cpp) exist. The full GPU group passes. |
| 14 | Free camera tests | Source | [SurfaceCameras.cpp](../mono.engine/scene/tests/SurfaceCameras.cpp) covers crossing and camera routes. |
| 15 | Character tests | Source | [Characters.cpp](../mono.engine/scene/tests/Characters.cpp) covers character behavior. |
| 16 | Character camera tests | Source | [client portal walk](../mono.client/tests/PortalWalk.cpp) and [replication tests](../mono.client/tests/Replicated.cpp) cover camera and zoom routes. |
| 17 | Reduce build disk use | Unverified measurement | [Justfile](../Justfile) provides compiler-cache sizing and pruning. No before and after disk baseline was found. |
| 18 | Prune named old plans | Source | The named plans were removed in repository history. |
| 19 | Prune future-work documents | Source | Component notes were moved to topical documents and obsolete content removed in history. |
| 20 | Plan render cleanup | Source | [render cleanup plan](v025-RENDER-PIPELINE-CLEANUP.md) exists with ownership and verification stages. |
| 21 | Plan MCP cleanup | Source | [MCP cleanup plan](v025-MCP-CLEANUP.md) exists with contracts and stages. |
| 22 | Light path visualizer | Partial | [RuntimeDiagnostics.cpp](../mono.engine/render/src/RuntimeDiagnostics.cpp) draws blue influence segments, red first-bound stops, and bounded orange segments through transparent, transmissive, or non-shadow-casting bounds. Reflection remains reserved; mirror portals render camera views rather than bouncing local light. |
| 23 | SkyGridPBR benchmark demo | Source | [BenchmarkSkyGrid.luau](../mono.engine/examples/assets/scripts/BenchmarkSkyGrid.luau) has eight variants and a moving camera; [tests](../mono.engine/examples/tests/BenchmarkSkyGrid.cpp) inspect the constructed scene. |
| 24 | Unique and shared 4K PBR stress demos | Source, performance unverified | [unique](../mono.engine/examples/assets/scripts/PbrTextureUniqueStress.luau) and [shared](../mono.engine/examples/assets/scripts/PbrTextureInstanceStress.luau) demos exist. Device residency and timings were not measured here. |
| 25 | Virtual camera lock for all behavior | Partial | [Editor.cpp](../mono.studio/src/Editor.cpp) freezes culling and LOD visibility. Inspection CameraFrame remains live, so the literal all-camera-behavior claim is broader than the implementation. |
| 26 | Packed render support channels | Source | [data capture tests](../mono.engine/render/tests/DataCapture.cpp) cover packed lanes for supporting render data. |
| 27 | Complete render pipeline cleanup | Partial | Compiler, registry, executor, and frame/resource owners are extracted. [PortalImageRuntime.cpp](../mono.engine/render/src/portal/PortalImageRuntime.cpp) still calls Renderer directly; the [plan](v025-RENDER-PIPELINE-CLEANUP.md) GPU gate passes. |
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
| 41 | Bloom, depth of field, god rays, fog, clouds | GPU gate passed | [GPU compositor tests](../mono.engine/render/tests/CompositorDemoGpu.cpp) and example authoring exist. The authored compositor GPU workflow passes after correcting its lens resolution fixture. |
| 42 | Weather demo | Source, visual unverified | [Weather.luau](../mono.engine/examples/assets/scripts/Weather.luau) and [tests](../mono.engine/examples/tests/Weather.cpp) cover authored layers. |
| 43 | Self-contained TornadoSim world | Source, interactive unverified | [TornadoSim.aworld](../mono.engine/examples/assets/worlds/TornadoSim.aworld) embeds scripts; its [test](../mono.engine/examples/tests/TornadoSim.cpp) checks document content. Lower-level storm, physics, and GPU tests exist. |
| 44 | Studio GUI Preview Controls | Source | [Studio view tests](../mono.studio/tests/Viewports.cpp) cover hidden default and enabled interaction. |
| 45 | Remove HarfBuzz notice | Source | [vendor configuration](../mono.build/MonoVendor.cmake) suppresses the notice. |
| 46 | Combined lighting validation | Source, GPU gate passed | [VolumeGpu.cpp](../mono.engine/render/tests/VolumeGpu.cpp) tests overlap and compute outputs. The full GPU group passes; the remaining limitation is broader visual scene coverage. |
| 47 | Lighting stress and measured optimization | Partial evidence | [LightingStress.cpp](../mono.engine/examples/tests/LightingStress.cpp) covers 256 lights and 256 fog volumes; timing paths exist, but no before and after bottleneck measurements were found. |
| 48 | TornadoSim reference comparison | Source, artifacts unverified | Storm and GPU tests plus [port notes](v025-TORNADOSIM-PORT.md) cover samples and captures; raw matched image artifacts were not found in this audit. |
| 49 | Representative live Studio workflow | Partial verification | [Studio smoke](../RUNNING.md) loaded, played, and captured examples. A later live session reopened the saved four-world game with Worlds visible, inserted and edited a Part, then played and stopped. Both dock focus paths have regression fixes; a live Play to Stop rerun captured the Worlds rows visible. Broad dropdown and class coverage remains open. |

## Confirmed follow-up work

1. Finish truthful light pass-through and reflection diagnostics, or narrow the feature contract to local-light influence and update the roadmap.
2. Complete the render cleanup ownership boundary; its GPU gate now passes.
3. Finish the MCP cleanup criteria in [its plan](v025-MCP-CLEANUP.md). This audit fixed a discovery inconsistency: a draining hook appeared in `negotiate.operations` while `tools/list` hid it. [Discovery.cpp](../mono.engine/control/src/Discovery.cpp) now filters both views consistently, with [regression coverage](../mono.engine/control/tests/HookRegistry.cpp). The client, server, and Studio fixtures now include the chunked snapshot operations added concurrently.
4. Complete the stress audit coverage and record startup and lighting performance baselines and results.
5. Run the portal acceptance workload matrix and preserve its reports with backend, revision, resolution, and network conditions.
6. Extend GPU scene coverage beyond the 132 passing cases where authored examples still lack representative visual checks. The six former failures were corrected in fixtures and expectations, and the full group passed on rerun.
7. Exercise the remaining Studio dropdowns and object classes with representative captures. The headless smoke passed, but its MeshGrid viewport capture was only 43 by 517 pixels under the existing layout. A live Play to Stop rerun confirmed the Worlds dock focus fix.
