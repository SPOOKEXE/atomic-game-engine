Minimal ImageGraph implementation specification

Evidence and cut point
- Fixes baseline: v0.25.0-fixes. Full archive: archive/v0.26.0-imagegraph-full. Initial native commit: 8ec3532e.
- Whole initial native module is already excessive: Document.cpp has 6,522 lines and includes AudioCapture, TimelineSchedule, recursive array/value evaluation, and Transform3D.
- Current PixelOpsBasicFilters/Blend/SpatialWarp include the enormous archived Document.hpp. Their local equations and independently expected pixel fixtures can be extracted, but whole-file imports are not a minimal dependency closure.
- Ordinary assets::Texture, TexturePixel, Resample; bake::ReadImage; Studio nodegraph::Canvas; assetc bake pipeline already exist in fixes. Keep these.

User promise
Create a durable, editable 2D graph from images and simple 2D nodes, preview it in Studio, save/reopen it, export ordinary image assets usable through the existing ParticleEmitter.Texture and ImageLabel.Image content paths. Engine owns document validation and pixel evaluation. Bake/filesystem and Studio own adapters. No live-runtime graph, 3D, audio, simulation, timeline, foreign Composer project import, HLSL, or new renderer producer.

Suggested node set
- image.source: stable relative content/source path, image output.
- image.solid: positive integer width/height and straight RGBA8 color.
- image.resize: explicit width/height, nearest or bilinear choice with stable names.
- image.crop: integer x/y/width/height, transparent pixels outside input.
- image.transform: explicit output canvas dimensions, translation in pixels, scale X/Y, clockwise degrees, pivot in pixels; inverse mapping at pixel centers; transparent exterior; finite controls and nonzero scales.
- image.flip: horizontal/vertical booleans, preserving dimensions.
- image.blend: background/foreground, opacity, normal straight-alpha source-over composition; differing input dimensions either rejected clearly or handled by explicit transform onto a matching canvas. Prefer reject shape mismatch so placement stays explicit.
- Named output references one node's image port. One output default; an explicit selected output for multiple.

Graph contract
Own a small Image RGBA8 struct plus Source/Solid/Resize/Crop/Transform/Flip/Blend data. Kind and node identifiers are durable strings in serialized project. Plan stores only derived dense indices. Validate duplicate/missing names, input counts, dangling links, unsupported kinds/options, cycles, dimensions, color/control ranges, finite numbers, and byte/pixel-work limits before allocating. Evaluate only the selected dependency cone in stable order, once per shared node, with caller-supplied source resolver and no filesystem or renderer access. Retain image results only for the bounded call. Malformed Read/Compile/Evaluate preserve destination values and return a node-specific diagnostic.

Serialization
Private nlohmann/json is a viable small durable parser, never in public headers. A small versioned JSON schema is simpler than extracting the archived handwritten text/parser/value codecs. Bound input bytes and JSON nesting before parse, then build checked native structs. Do not imply archived project compatibility. Save Studio node positions plus graph and named outputs in project; positions never affect evaluation. Serialize all named node kinds/options and values by stable text. Atomic checked temporary-file publication belongs in host; failing save preserves old project.

Adapter/edit paths
- New mono.engine/imagegraph/CMakeLists.txt, include/engine/imagegraph/Document.hpp, src/Document.cpp, src/Evaluate.cpp, private src/Pixels.hpp, tests/Document.cpp, tests/Evaluate.cpp. Existing AGENTS scope says core-only CPU bounded document/evaluator, no files/decoders/UI. Register L9 shared module in mono.engine/CMakeLists.txt and mono.tools/architecture/expected_graph.json, with proper consumer closures.
- New mono.engine/bake/include/engine/bake/ImageGraph.hpp and src/ImageGraph.cpp provide source-decoder/TextureData bridge with a host resolver callback. tests/ImageGraph.cpp verify ordinary .atex output and decode semantics. Engine imagegraph never depends up on bake or Studio.
- mono.tools/assetc/CMakeLists.txt and src/Bake.cpp: use small .imagegraph project extension, output .atex; settings/output selector and CLI only as needed. Existing .graph support was removed with the oversized work, so deliberate distinct extension avoids silently promising archive compatibility. Source resolver accepts sources under the input root, no graph recursion. New tests/ImageGraph.cpp plus CLI tests demonstrate rebake and failure preserving prior output.
- New mono.studio/include/studio/ImageComposer.hpp and src/ImageComposer.cpp with a small owned state, plus an engine graph to existing nodegraph canvas adapter. Add Editor forward-declared owned state, DrawImageComposer/ShowImageComposer, initialization/shutdown, menu/window routing in Editor.cpp/Interface.cpp. Register only the above node types through the existing nodegraph registry; no duplicate graph UI library, old SourceDynamicGroups, source ports, or timeline.
- Studio export writes an ordinary .atex or PNG into existing authored raw/baked content flow, then returns the content name. Existing NodeDemo.cpp ExportNodeDemoImage only writes PNG and does not publish content, so exporting a preview is not enough on its own. Use Renderer.AddTexture for editor preview only, with a studio-specific name and cleanup. Product content export must have the ordinary asset name discoverable in Studio's configured content origin.

Meaningful validation seams
- Known 2x2 asymmetric colors test source, crop, resize, both flips, translation, rotation and negative scale. Assert literal pixels, not the implementation formula mirrored in tests.
- Blend transparent colored pixels and two half-alpha layers, testing exact expected source-over RGB/alpha. Identity transform and resized/pivot behavior.
- Shared DAG, disconnected unsupported node policy, reachable cycles, missing source, source dimension/pixel inconsistency, over-budget graph, malformed/truncated/deep JSON, save/read/layout roundtrip preserving image result, failure preserving previous result.
- Adapter end-to-end: PNG source -> transform -> blend -> ordinary Texture::Write/Read; assetc repeat bake after source/graph edit; unsafe source path refusal; failed rebake leaves old output intact.
- Studio headless ImGui frame harness: create/link nodes, inspector edit, undo/redo, save/open, preview/output selection, export resulting content name. Existing Studio AGENTS explicitly favors real ImGui context tests over extracting helpers merely to avoid testing UI.
- Consumer check: generated asset read through ordinary content loading into both ParticleEmitter and ImageLabel. Existing engine/client consumer path should need no graph-specific ECS property or renderer code.
- Headless dev build, selected suites, architecture and source checks; server-safe dependency checks. Small release evaluation benchmark added to Justfile prints timings without files. Headless Vulkan texture and particle verification was explicitly approved by the user. No interactive Studio session was run.


Execution checklist
- [x] classify: minimal static 2D images and Studio composition, confirmed by user.
- [x] discover-core: archived graph is too broad; ordinary .atex consumers already exist.
- [x] specify: bounded CPU graph, versioned project and ordinary export contracts above.
- [x] localize: finalize public API and adapter entry points before dependent edits.
- [x] prototype: use literal pixel fixtures for sampling uncertainty; skip throwaway duplicate code.
- [x] stubs: agree the engine public header across Studio and bake adapters.
- [x] implement: engine first, adapters and Studio next, root owns build wiring.
- [x] validate: pixel/document tests, export/consumer tests, real ImGui context tests, headless Vulkan texture checks.
- [x] optimize: measure a small release evaluation benchmark; no unsupported performance claims.
- [x] review: independent read, generated contracts, clean committed branch and evidence.

Function logic statements
- Read: bound encoded size and nesting, parse stable named kinds into temporary authored data, validate it, then replace the destination only on success.
- Write: validate authored data, encode version and stable names plus canvas placement, and report unsupported or invalid data rather than silently discarding it.
- Compile: validate identity, ports, parameters, limits and cycles, then produce a stable derived dependency order; preserve the previous plan on refusal.
- Evaluate: check the selected output, resolve bounded decoded sources, visit its dependency cone once per node, perform bounded 2D pixel operations, then replace the output only after success.
- BakeImageGraph: compile and evaluate with the supplied source resolver, then move the accepted RGBA8 pixels into ordinary TextureData. Assetc binds confined project-relative file reads and atomic publication at its host boundary.
- Composer adapter: map the existing nodegraph canvas to the strict engine document and back with stable identities and placements; reject unknown types or invalid links before accepting a load.
- Composer refresh: compare semantic graph and source revisions, evaluate only after a change, preserve the previous good preview on failure and upload a successful preview once.
- Composer export: validate the selected output and asset leaf name, serialize ordinary .atex bytes, atomically publish them in configured baked content and register the accepted asset through the existing Editor path.

Verification evidence
- Complete dev and server-only builds pass; optimized unity assetc build passes.
- Engine: 13 cases / 419 assertions; the same suite passes ASan, UBSan and leak detection.
- Complete Studio: 662 cases / 13,020 assertions; bake: 128 / 15,425; assetc: 40 / 319; nodegraph: 23 / 204.
- Headless Vulkan texture and particle checks: 10 cases / 364 assertions; actual Composer export, publication and consumer rendering: 1 / 23.
- Five actual MCP contract suites: 22 cases / 2,135 assertions. Only the engine/imagegraph resource was added.
- Architecture: 49 modules / 6 programs / 35 layered modules plus six fixtures; independent server graph: 38 / 1 / 29. Shader contracts: 76 modules.
- CPU evaluation benchmark ran with five samples on the optimized bench preset, including normal allocation and profiling costs. Timings stayed on the terminal.
- Independent reads caught and fixed linear source color conversion, PNG inflation bounds and durable copied-node identity. Final checks also cover bounded GIF atlases.
- Whole repository tests were not repeated after the minimal integration. The fixes baseline completed 647 suites, with its four failures subsequently repaired and verified. No interactive Studio session or full archive revalidation was performed.
