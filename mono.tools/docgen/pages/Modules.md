# Module invariants

One page per module, each holding the rules that catch real mistakes in that
module rather than the ones that apply everywhere. **Read the one for a module
before changing anything in it** - [AGENTS.md](@ref md_AGENTS) says why, and the
layer stack it describes is what decides which of these a module may read.

The engine bottom to top, then the programs, then the tooling. A module carrying
prose of its own beyond its invariants has it listed underneath.

**Generated.** `mono.tools/architecture/WriteModulePages.cmake` walks the tree
for every `AGENTS.md` and orders them by the layers in `expected_graph.json`, so
a module that exists is listed. Run `just docs-pages` after adding one;
`just docs-pages-check` is what fails when it has not been run.

- @subpage md_mono_8engine_2AGENTS
- @subpage md_mono_8engine_2core_2AGENTS
- @subpage md_mono_8engine_2parallel_2AGENTS
- @subpage md_mono_8engine_2ecs_2AGENTS
- @subpage md_mono_8engine_2ecs_2docs_2TODO
- @subpage md_mono_8engine_2world_2AGENTS
- @subpage md_mono_8engine_2collision_2AGENTS
- @subpage md_mono_8engine_2spatial_2AGENTS
- @subpage md_mono_8engine_2gui_2AGENTS
- @subpage md_mono_8engine_2scene_2AGENTS
- @subpage md_mono_8engine_2assets_2AGENTS
- @subpage md_mono_8engine_2effects_2AGENTS
- @subpage md_mono_8engine_2physics_2AGENTS
- @subpage md_mono_8engine_2bake_2AGENTS
- @subpage md_mono_8engine_2bakegraph_2AGENTS
- @subpage md_mono_8engine_2graph_2AGENTS
- @subpage md_mono_8engine_2script_2AGENTS
- @subpage md_mono_8engine_2scriptjs_2AGENTS
- @subpage md_mono_8engine_2scriptluau_2AGENTS
- @subpage md_mono_8engine_2delivery_2AGENTS
- @subpage md_mono_8discord_2AGENTS
- @subpage md_mono_8engine_2msl_2AGENTS
- @subpage md_mono_8engine_2net_2AGENTS
- @subpage md_mono_8engine_2resources_2AGENTS
- @subpage md_mono_8engine_2scripthost_2AGENTS
- @subpage md_mono_8engine_2audio_2AGENTS
- @subpage md_mono_8engine_2datastore_2AGENTS
- @subpage md_mono_8engine_2game_2AGENTS
- @subpage md_mono_8engine_2input_2AGENTS
- @subpage md_mono_8network_2AGENTS
- @subpage md_mono_8engine_2render_2AGENTS
- @subpage md_mono_8engine_2replication_2AGENTS
- @subpage md_mono_8engine_2ui_2AGENTS
- @subpage md_mono_8engine_2control_2AGENTS
- @subpage md_mono_8cdn_2AGENTS
- @subpage md_mono_8cdn_2docs_2index
- @subpage md_mono_8client_2AGENTS
- @subpage md_mono_8launcher_2AGENTS
- @subpage md_mono_8studio_2nodegraph_2AGENTS
- @subpage md_mono_8server_2AGENTS
- @subpage md_mono_8studio_2AGENTS
- @subpage md_mono_8unified__tests_2AGENTS
- @subpage md_mono_8tools_2AGENTS
- @subpage md_mono_8tools_2docgen_2AGENTS
