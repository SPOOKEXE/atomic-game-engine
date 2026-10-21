# Module guides

Find module invariants and guides by engine layer or owning product. Read the
owning module's invariants before changing its code;
[AGENTS.md](@ref md_AGENTS) explains the shared rules.

- @subpage docs_modules_overview
- @subpage docs_modules_foundations
- @subpage docs_modules_simulation
- @subpage docs_modules_execution
- @subpage docs_modules_runtime
- @subpage docs_modules_products
- @subpage docs_modules_tools

Generated from the module graph and source guides. After adding module documentation,
run `just docs-pages`; `just docs-pages-check` checks this index.

@page docs_modules_overview Engine overview

Shared invariants for every engine library.

- @subpage md_mono_8engine_2AGENTS

@page docs_modules_foundations Foundations (L1-4)

Platform values, parallel work, storage and world ticking.

- @subpage md_mono_8engine_2core_2AGENTS
- @subpage md_mono_8engine_2graphcore_2AGENTS
- @subpage md_mono_8engine_2parallel_2AGENTS
- @subpage md_mono_8engine_2computegraph_2AGENTS
- @subpage docs_module_mono_8engine_2ecs
- @subpage md_mono_8engine_2procedural_2AGENTS
- @subpage docs_module_mono_8engine_2testing
- @subpage md_mono_8engine_2world_2AGENTS

@page docs_modules_simulation Scene and simulation (L5-8)

Geometry, authored scenes, assets and simulation systems.

- @subpage md_mono_8engine_2collision_2AGENTS
- @subpage md_mono_8engine_2spatial_2AGENTS
- @subpage md_mono_8engine_2gui_2AGENTS
- @subpage docs_module_mono_8engine_2scene
- @subpage md_mono_8engine_2assets_2AGENTS
- @subpage docs_module_mono_8engine_2audiocodec
- @subpage md_mono_8engine_2effects_2AGENTS
- @subpage md_mono_8engine_2imagecodec_2docs_2ImageCodec
- @subpage md_mono_8engine_2physics_2AGENTS
- @subpage docs_module_mono_8engine_2weather

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
- @subpage docs_module_mono_8engine_2control

@page docs_modules_products Products

Programs and their owned modules outside the engine layer stack.

- @subpage docs_module_mono_8cdn
- @subpage docs_module_mono_8client
- @subpage md_mono_8launcher_2AGENTS
- @subpage md_mono_8studio_2nodegraph_2AGENTS
- @subpage md_mono_8server_2AGENTS
- @subpage docs_module_mono_8studio
- @subpage md_mono_8unified__tests_2AGENTS

@page docs_modules_tools Tools

Developer tools and their module guides.

- @subpage md_mono_8tools_2AGENTS
- @subpage md_mono_8tools_2docgen_2AGENTS

@page docs_module_mono_8engine_2ecs ecs

Guides and invariants for `mono.engine/ecs/`.

- @subpage md_mono_8engine_2ecs_2AGENTS
- @subpage md_mono_8engine_2ecs_2docs_2TODO

@page docs_module_mono_8engine_2testing testing

Guides and invariants for `mono.engine/testing/`.

- @subpage md_mono_8engine_2testing_2AGENTS
- @subpage md_mono_8engine_2testing_2docs_2SCENARIOS

@page docs_module_mono_8engine_2scene scene

Guides and invariants for `mono.engine/scene/`.

- @subpage md_mono_8engine_2scene_2AGENTS
- @subpage md_mono_8engine_2scene_2docs_2GpuParticleField

@page docs_module_mono_8engine_2audiocodec audiocodec

Guides and invariants for `mono.engine/audiocodec/`.

- @subpage md_mono_8engine_2audiocodec_2AGENTS
- @subpage md_mono_8engine_2audiocodec_2docs_2index

@page docs_module_mono_8engine_2weather weather

Guides and invariants for `mono.engine/weather/`.

- @subpage md_mono_8engine_2weather_2AGENTS
- @subpage md_mono_8engine_2weather_2docs_2Design

@page docs_module_mono_8engine_2control control

Guides and invariants for `mono.engine/control/`.

- @subpage md_mono_8engine_2control_2AGENTS
- @subpage md_mono_8engine_2control_2docs_2MCP

@page docs_module_mono_8cdn mono.cdn

Guides and invariants for `mono.cdn/`.

- @subpage md_mono_8cdn_2AGENTS
- @subpage md_mono_8cdn_2docs_2index

@page docs_module_mono_8client mono.client

Guides and invariants for `mono.client/`.

- @subpage md_mono_8client_2AGENTS
- @subpage md_mono_8client_2docs_2FramePacing

@page docs_module_mono_8studio mono.studio

Guides and invariants for `mono.studio/`.

- @subpage md_mono_8studio_2AGENTS
- @subpage md_mono_8studio_2docs_2FramePacing
