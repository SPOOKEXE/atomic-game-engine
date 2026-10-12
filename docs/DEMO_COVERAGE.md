# Demo coverage

This is a source-level audit of every top-level Luau scene and authored world in the example catalog, plus the separate virtual-camera capture fixture. The roles describe what each scene is useful for. Visual quality and interaction still require a live client review. Narrow fixtures are kept because they expose specific renderer, scripting, network, or physics behavior.

## Launch and assets

Build with `just preset=dev client`, then run a script with `scripts/demos/run-demo.sh Rings` or a world with `scripts/demos/run-demo.sh TornadoSim.aworld`. The Windows launcher is `scripts\demos\run-demo.bat`. A bare stem selects `.luau`. Luau module trees and Arena's client shooting companion stage with their scenes. `just preset=dev demo-check` captures each listed scene headlessly; it checks startup and image validity, not appearance or interaction.

- `Mirrors-4-worlds` needs `--worlds 4`. `CrossWorldSeam` compares `--worlds 1` with `--worlds 2 --view-spacing 0`; the zero spacing is required for its pane alignment. `ImmersivePortals` needs the same script in two named worlds. The `Playground` and `Arena` scenes need their respective pad scripts and both worlds for the teleport round trip. `Arena` shows its targets in a standalone preview; shooting and target resets require a server hosting `Arena.luau` and its Luau client companion.
- `DataFactoryDemo` and `DataFactoryAdvancedDemo` need client data-factory mode and an MCP port, for example `--data-factory --mcp-port 8738`. `DataFactoryPackageDemo` exercises the external package workflow. `RenderFeaturesDemo` pairs with `examples/pipelines/RenderFeatures.pipeline`. `VirtualCameraCapture.agame` is launched by `scripts/studio-virtual-camera-capture-test.sh`, outside the normal demo list.
- `Assets`, `MeshGrid`, and `Meshes` need the published content store to show the full imported mesh and texture inventory. `Meshes` expects the named glTF and PMX assets. `SkyEnvironment` needs six published Allsky cubemap faces. `ParticleFlipbooks` stages `effects/fox_dance.atex`; `TornadoSim.aworld` stages five WAV files.
- `ReplicationStress` needs a server and at least one client. `Stress` and `PlayerList` benefit from the local-server harness and multiple clients. The paired `PbrTextureInstanceStress` and `PbrTextureUniqueStress` scenes compare shared and unique 4096-square textures; the unique case allocates about 6.25 GiB of raw image data before renderer overhead.

## Interactive showcase

| Scene | Feature mix |
| --- | --- |
| [`Animation.luau`](../mono.engine/examples/assets/scripts/Animation.luau) | procedural AnimationBuffer keyframes; skeletal rig and Bone; baked AnimationTrack playback |
| [`Arena.luau`](../mono.engine/examples/assets/scripts/Arena.luau) | second-world teleport destination; standable ring and target preview; server-hosted Luau remote shooting, target damage, and hit feedback; present-time server raycast with no rewind lag compensation |
| [`Assets.luau`](../mono.engine/examples/assets/scripts/Assets.luau) | mesh and texture bays; image and flipbook sampling; audio marker and video status |
| [`BlackHoleSimulator.luau`](../mono.engine/examples/assets/scripts/BlackHoleSimulator.luau) | GravitationalLens component; moving apparent star positions; accretion geometry and light |
| [`Hallway.luau`](../mono.engine/examples/assets/scripts/Hallway.luau) | paired corridor portals; recursive interior illusion; player movement and lighting |
| [`Interface.luau`](../mono.engine/examples/assets/scripts/Interface.luau) | screen GUI layout; text entry and drag; buttons, grids and styling |
| [`InterfaceWorld.luau`](../mono.engine/examples/assets/scripts/InterfaceWorld.luau) | surface GUI; billboard GUI; nested viewport GUI; client interaction and lighting |
| [`Magic.luau`](../mono.engine/examples/assets/scripts/Magic.luau) | spell recipes and projectile effects; editable terrain craters; visual runtime and status effects |
| [`Meshes.luau`](../mono.engine/examples/assets/scripts/Meshes.luau) | imported glTF/PMX; built-in mesh and textures; surface camera mirror |
| [`Mirrors-1-world.luau`](../mono.engine/examples/assets/scripts/Mirrors-1-world.luau) | mirror room; surface cameras; shadows and animated object; shared render passes |
| [`NonEuclidean.luau`](../mono.engine/examples/assets/scripts/NonEuclidean.luau) | six impossible-space layouts; explicit return portals; scripted camera tour |
| [`PbrShaderMaterialDemo.luau`](../mono.engine/examples/assets/scripts/PbrShaderMaterialDemo.luau) | procedural stone PBR maps; EditableMesh masonry relief; point and spot lighting |
| [`Planet.luau`](../mono.engine/examples/assets/scripts/Planet.luau) | three procedural planet types; editable quadsphere LOD and textures; atmosphere, ocean, rings and view feedback |
| [`PlayerList.luau`](../mono.engine/examples/assets/scripts/PlayerList.luau) | live Players membership; replicated ownership; UI list updates and local/network paths |
| [`Playground.luau`](../mono.engine/examples/assets/scripts/Playground.luau) | playable spawn; jumpable props; floor and basic player loop |
| [`Portals-1-world.luau`](../mono.engine/examples/assets/scripts/Portals-1-world.luau) | multi-room portal building; player navigation; lighting and moving markers |
| [`Rings.luau`](../mono.engine/examples/assets/scripts/Rings.luau) | orbiting and spinning parts; Heartbeat update; CFrame composition and bulk moves |
| [`Shaders.luau`](../mono.engine/examples/assets/scripts/Shaders.luau) | runtime ShaderScript; mesh material fragment; surface-camera pane and screen pass |
| [`Slide.luau`](../mono.engine/examples/assets/scripts/Slide.luau) | gravity descent; collision steps; moving loose bodies and cleanup |
| [`Terrain.luau`](../mono.engine/examples/assets/scripts/Terrain.luau) | streamed EditableMesh landscape; chunk remesh and edits; walking player and view feedback |
| [`Tunnels.luau`](../mono.engine/examples/assets/scripts/Tunnels.luau) | folded single-building tunnels; reciprocal portal doorways; moving lights and player walk |
| [`Weather.luau`](../mono.engine/examples/assets/scripts/Weather.luau) | interactive weather presets; blended fog and sky; clouds, rain and light effects |
| [`BladeborneDemo.aworld`](../mono.engine/examples/assets/worlds/BladeborneDemo.aworld) | client HUD with hotbar, minimap, quests and on-screen controls; replicated Luau module tree; server arena and spawn |
| [`TornadoSim.aworld`](../mono.engine/examples/assets/worlds/TornadoSim.aworld) | authoritative storm field and controls; debris/vegetation interaction and GPU particles; client HUD, camera and five packaged audio tracks |

## Focused fixture

| Scene | Feature mix |
| --- | --- |
| [`Adornments.luau`](../mono.engine/examples/assets/scripts/Adornments.luau) | selection box and sphere; handle solids and arcs; per-target offsets and colours |
| [`BloomEnvironment.luau`](../mono.engine/examples/assets/scripts/BloomEnvironment.luau) | HDR bloom; procedural sky and clouds; bright sun and geometry |
| [`DataFactoryAdvancedDemo.luau`](../mono.engine/examples/assets/scripts/DataFactoryAdvancedDemo.luau) | stable entity identities; spatial queries and authored affordances; editable-image metadata; animation and capture APIs |
| [`DataFactoryDemo.luau`](../mono.engine/examples/assets/scripts/DataFactoryDemo.luau) | spatial observation; entity metadata; scene snapshot query |
| [`DataFactoryPackageDemo.luau`](../mono.engine/examples/assets/scripts/DataFactoryPackageDemo.luau) | package-safe world setup; stable object tree; point-light and geometry targets |
| [`DepthOfFieldGodRays.luau`](../mono.engine/examples/assets/scripts/DepthOfFieldGodRays.luau) | depth of field; god rays; procedural atmosphere and cloud HDR |
| [`EditableImage.luau`](../mono.engine/examples/assets/scripts/EditableImage.luau) | DrawCircle and DrawLine; live image revision; texture on a 3D mesh |
| [`EditableMesh.luau`](../mono.engine/examples/assets/scripts/EditableMesh.luau) | procedural vertices and triangles; per-vertex colour; live geometry deformation |
| [`GuiInteraction.luau`](../mono.engine/examples/assets/scripts/GuiInteraction.luau) | 2D text entry; button/image activation; event callbacks and visual state |
| [`Libraries.luau`](../mono.engine/examples/assets/scripts/Libraries.luau) | MagicCore compiler and validation; TerrainCore generation; runtime module loading |
| [`Lighting.luau`](../mono.engine/examples/assets/scripts/Lighting.luau) | directional sun; fog; ambient/outdoor lighting over coloured blocks |
| [`LodDecimation.luau`](../mono.engine/examples/assets/scripts/LodDecimation.luau) | automatic mesh decimation; built-in and EditableMesh sources; triangle count inspection |
| [`MagicTests.luau`](../mono.engine/examples/assets/scripts/MagicTests.luau) | ported spell specs; terrain library specs; in-world pass/fail marker |
| [`MeshGrid.luau`](../mono.engine/examples/assets/scripts/MeshGrid.luau) | published and built-in mesh inventory; turntables; labels, textures and fallback detection |
| [`MipProbe.luau`](../mono.engine/examples/assets/scripts/MipProbe.luau) | long receding checkerboard; minified texture sampling; near marker and far-strip variance control |
| [`MirrorCorridor.luau`](../mono.engine/examples/assets/scripts/MirrorCorridor.luau) | facing surface mirrors; repeated reflections; recursion-depth cues |
| [`MirrorDepth.luau`](../mono.engine/examples/assets/scripts/MirrorDepth.luau) | three chained mirrors; off-axis camera; visibility and recursion |
| [`NodeCanvas.luau`](../mono.engine/examples/assets/scripts/NodeCanvas.luau) | typed compute graph; retained GUI tree; node groups and ports; evaluator bypass |
| [`ParticleFlipbooks.luau`](../mono.engine/examples/assets/scripts/ParticleFlipbooks.luau) | packaged atlas; particle image sampling; animated flipbook frames |
| [`PortalLightMix.luau`](../mono.engine/examples/assets/scripts/PortalLightMix.luau) | two coloured local lights; portal transfer; combined pools in both rooms |
| [`PortalLightOut.luau`](../mono.engine/examples/assets/scripts/PortalLightOut.luau) | light placed at a portal; remote emission; dark control room |
| [`PortalProbe.luau`](../mono.engine/examples/assets/scripts/PortalProbe.luau) | portal pane orientation; cutout visibility; adjacent control geometry |
| [`PortalSeam.luau`](../mono.engine/examples/assets/scripts/PortalSeam.luau) | moving bar crossing portal; near/far copy; attached light and particles |
| [`PortalSeamMatchedRoom.luau`](../mono.engine/examples/assets/scripts/PortalSeamMatchedRoom.luau) | moving yellow bar; matching camera and room colours; direct-room baseline |
| [`PortalShadow.luau`](../mono.engine/examples/assets/scripts/PortalShadow.luau) | straddling portal body; shadow on near/far floors; portal lighting |
| [`ProceduralEnvironment.luau`](../mono.engine/examples/assets/scripts/ProceduralEnvironment.luau) | built-in atmosphere; skybox and cloud compute; selectable shader variants |
| [`RecursiveMirrors.luau`](../mono.engine/examples/assets/scripts/RecursiveMirrors.luau) | facing mirrors; moving cube; same-frame recursion history |
| [`RenderFeaturesDemo.luau`](../mono.engine/examples/assets/scripts/RenderFeaturesDemo.luau) | scene, camera and graph policy masks; built-in and edited mesh LOD; lazy shaders |
| [`ServerDefault.luau`](../mono.engine/examples/assets/scripts/ServerDefault.luau) | deterministic anchored cube field; heartbeat bounce and velocity attributes; BulkMoveTo snapshots |
| [`SkyEnvironment.luau`](../mono.engine/examples/assets/scripts/SkyEnvironment.luau) | external cubemap; procedural atmosphere; compute clouds |
| [`SkyGrid.luau`](../mono.engine/examples/assets/scripts/SkyGrid.luau) | deterministic block lattice; bulk movement; sky-only depth and view |
| [`UiLab.luau`](../mono.engine/examples/assets/scripts/UiLab.luau) | scrollable responsive layout; click and text focus; drag/constraints and capture points |
| [`UiTextVisual.luau`](../mono.engine/examples/assets/scripts/UiTextVisual.luau) | coloured glyph regions; text layout at two viewport sizes; frame/stroke contrast |
| [`ValueObjects.luau`](../mono.engine/examples/assets/scripts/ValueObjects.luau) | all ValueBase leaf types; hierarchy and typed writes; ObjectValue references |
| [`Welds.luau`](../mono.engine/examples/assets/scripts/Welds.luau) | legacy Weld; WeldConstraint; anchored-root-driven assemblies |
| [`VirtualCameraCapture.agame`](../mono.engine/examples/assets/worlds/VirtualCameraCapture.agame) | client-owned fixed camera; LOD mesh and point light; particle emitter and paired portals |

## Stress or paired fixture

| Scene | Feature mix |
| --- | --- |
| [`ArenaPad.luau`](../mono.engine/examples/assets/scripts/ArenaPad.luau) | return teleport pad; authority-only touch detection; destination world lookup |
| [`BenchmarkSkyGrid.luau`](../mono.engine/examples/assets/scripts/BenchmarkSkyGrid.luau) | moving PBR course; eight procedural EditableMesh terrain balls; material and shader routes; lights |
| [`CrossWorldSeam.luau`](../mono.engine/examples/assets/scripts/CrossWorldSeam.luau) | two world panes; foreign view sampling; matched spawn camera and marker geometry |
| [`Cube.luau`](../mono.engine/examples/assets/scripts/Cube.luau) | ten-thousand loose blocks; collapse into a pit; collision and solver stress |
| [`ImmersivePortals.luau`](../mono.engine/examples/assets/scripts/ImmersivePortals.luau) | cross-world passage; position and momentum continuity; room palette change |
| [`LightingStress.luau`](../mono.engine/examples/assets/scripts/LightingStress.luau) | many point and spot lights; fog volumes; atmosphere and bloom passes |
| [`Mirrors-4-worlds.luau`](../mono.engine/examples/assets/scripts/Mirrors-4-worlds.luau) | four world producers; composited frame; palette and motion per world |
| [`Particles.luau`](../mono.engine/examples/assets/scripts/Particles.luau) | large emitter counts; particle properties; Beam and Trail geometry |
| [`PbrTextureInstanceStress.luau`](../mono.engine/examples/assets/scripts/PbrTextureInstanceStress.luau) | one 4096-square shared image; many sphere instances; PBR lights |
| [`PbrTextureUniqueStress.luau`](../mono.engine/examples/assets/scripts/PbrTextureUniqueStress.luau) | separate 4096-square image per sphere; PBR residency; same geometry as instance control |
| [`PlaygroundPad.luau`](../mono.engine/examples/assets/scripts/PlaygroundPad.luau) | outbound teleport pad; authority-only touch detection; world lookup |
| [`ReplicationStress.luau`](../mono.engine/examples/assets/scripts/ReplicationStress.luau) | 20,000 moving parts; continuous dirty rows; interest window and network publication |
| [`Stress.luau`](../mono.engine/examples/assets/scripts/Stress.luau) | floor and spawn; multi-client placement; server stress harness |
| [`StressMirrors.luau`](../mono.engine/examples/assets/scripts/StressMirrors.luau) | ico-sphere mirror facets; surrounding pillar reflections; rotating high-surface workload |
| [`StressParticles.luau`](../mono.engine/examples/assets/scripts/StressParticles.luau) | full emitter-property gallery; 100,000 emitters; disable and emission controls |
| [`StressPhysics.luau`](../mono.engine/examples/assets/scripts/StressPhysics.luau) | 100,000 loose blocks; continuously tilting tray; sustained collision workload |

## Catalog decisions

The focused scenes stay because their narrow compositions make failures visible: for example `MirrorDepth` fixes a reflection angle, `PortalSeamMatchedRoom` supplies a direct-room control frame, `MipProbe` isolates minification, and `MagicTests` runs Luau library checks. The heavy scenes and paired controls remain separate so their workloads and baselines are comparable. The most useful polish targets are the launch instructions for multi-world scenes, external asset setup, and live visual QA of the larger authored worlds.

The demo source tree now contains Luau script assets only. Removed alternate scene sources: `Rings.js`, `Mirrors-1-world.ts`, and `Mirrors-4-worlds.ts`. The examples asset build no longer transpiles or stages those alternates, and the demo resolver accepts Luau scripts or authored worlds. The general JavaScript runtime, API, and source-map tests remain. The default server scene is now `ServerDefault.luau`, a deterministic moving cube field, and `Arena.luau` contains the arena shooting behavior with a Luau client companion.

## Validation scope

This inventory describes the v0.25.0-fixes sources. Branch-specific build, startup and interaction checks are recorded after the backport is validated.

Arena's C++ integration tests drive its actual Luau scripts through authenticated network clients. They cover valid shots, rejected aim payloads, cooldowns, obstructions, replica authority and client input. The default server scene has C++ wall-bounce parity and snapshot/replay checks.
