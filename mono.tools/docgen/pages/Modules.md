# Module guides

Module invariants and nonempty module prose, grouped by the current layer stack
and owning directories. Read a module's invariants before changing its code;
[AGENTS.md](@ref md_AGENTS) explains the shared rules.

**Generated.** `mono.tools/architecture/WriteModulePages.cmake` walks `AGENTS.md`
files and prose in graph modules, ordering modules by the layers in
`expected_graph.json`. Run `just docs-pages` after adding either;
`just docs-pages-check` is what fails when it has not been run.

- @subpage docs_modules_overview
- @subpage docs_modules_foundations
- @subpage docs_modules_simulation
- @subpage docs_modules_execution
- @subpage docs_modules_runtime
- @subpage docs_modules_products
- @subpage docs_modules_tools

@page docs_modules_overview Engine overview

Shared invariants for every engine library.

- @subpage md_mono_8engine_2AGENTS

@page docs_modules_foundations Foundations (L1-4)

Platform values, parallel work, storage and world ticking.

- @subpage md_mono_8engine_2core_2AGENTS
- @subpage md_mono_8engine_2graphcore_2AGENTS
- @subpage md_mono_8engine_2parallel_2AGENTS
- @subpage md_mono_8engine_2computegraph_2AGENTS
- @subpage md_mono_8engine_2ecs_2AGENTS
- @subpage md_mono_8engine_2ecs_2docs_2TODO
- @subpage md_mono_8engine_2procedural_2AGENTS
- @subpage md_mono_8engine_2testing_2AGENTS
- @subpage md_mono_8engine_2testing_2docs_2SCENARIOS
- @subpage md_mono_8engine_2world_2AGENTS

@page docs_modules_simulation Scene and simulation (L5-8)

Geometry, authored scenes, assets and simulation systems.

- @subpage md_mono_8engine_2collision_2AGENTS
- @subpage md_mono_8engine_2spatial_2AGENTS
- @subpage md_mono_8engine_2gui_2AGENTS
- @subpage md_mono_8engine_2scene_2AGENTS
- @subpage md_mono_8engine_2scene_2docs_2GpuParticleField
- @subpage md_mono_8engine_2assets_2AGENTS
- @subpage md_mono_8engine_2audiocodec_2AGENTS
- @subpage md_mono_8engine_2audiocodec_2docs_2index
- @subpage md_mono_8engine_2effects_2AGENTS
- @subpage md_mono_8engine_2imagecodec_2docs_2ImageCodec
- @subpage md_mono_8engine_2physics_2AGENTS
- @subpage md_mono_8engine_2weather_2AGENTS
- @subpage md_mono_8engine_2weather_2docs_2Design

@page docs_modules_execution Content and execution (L9-11)

Content processing, scripting and transport services.

- @subpage md_mono_8engine_2bake_2AGENTS
- @subpage md_mono_8engine_2bakegraph_2AGENTS
- @subpage md_mono_8engine_2graph_2AGENTS
- @subpage md_mono_8engine_2imagegraph_2AGENTS
- @subpage md_mono_8engine_2script_2AGENTS
- @subpage md_mono_8engine_2scriptjs_2AGENTS
- @subpage md_mono_8engine_2scriptluau_2AGENTS
- @subpage md_mono_8engine_2delivery_2AGENTS
- @subpage md_mono_8discord_2AGENTS
- @subpage md_mono_8engine_2msl_2AGENTS
- @subpage md_mono_8engine_2net_2AGENTS
- @subpage md_mono_8engine_2resources_2AGENTS
- @subpage md_mono_8engine_2scripthost_2AGENTS

@page docs_modules_runtime Runtime (L12-13)

High-level runtime and presentation systems.

- @subpage md_mono_8engine_2audio_2AGENTS
- @subpage md_mono_8engine_2datastore_2AGENTS
- @subpage md_mono_8engine_2examples_2docs_2TornadoSim
- @subpage md_mono_8engine_2game_2AGENTS
- @subpage md_mono_8engine_2input_2AGENTS
- @subpage md_mono_8network_2AGENTS
- @subpage md_mono_8engine_2render_2AGENTS
- @subpage md_mono_8engine_2replication_2AGENTS
- @subpage md_mono_8engine_2ui_2AGENTS
- @subpage md_mono_8engine_2control_2AGENTS
- @subpage md_mono_8engine_2control_2docs_2MCP

@page docs_modules_products Products

Programs and their owned modules outside the engine layer stack.

- @subpage md_mono_8cdn_2AGENTS
- @subpage md_mono_8cdn_2docs_2index
- @subpage md_mono_8client_2AGENTS
- @subpage md_mono_8client_2docs_2FramePacing
- @subpage md_mono_8launcher_2AGENTS
- @subpage md_mono_8studio_2nodegraph_2AGENTS
- @subpage md_mono_8server_2AGENTS
- @subpage md_mono_8studio_2AGENTS
- @subpage md_mono_8studio_2docs_2FramePacing
- @subpage md_mono_8unified__tests_2AGENTS

@page docs_modules_tools Tools

Developer tools and their module guides.

- @subpage md_mono_8tools_2AGENTS
- @subpage md_mono_8tools_2docgen_2AGENTS
