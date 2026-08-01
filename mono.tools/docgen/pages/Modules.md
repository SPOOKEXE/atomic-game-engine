# Module invariants

One page per module, each holding the rules that catch real mistakes in that
module rather than the ones that apply everywhere. **Read the one for a module
before changing anything in it** — [AGENTS.md](@ref md_AGENTS) says why, and the
layer stack it describes is what decides which of these a module may read.

The engine, bottom to top, then the two programs and the tooling.

- @subpage md_mono_8engine_2AGENTS
- @subpage md_mono_8engine_2core_2AGENTS
- @subpage md_mono_8engine_2parallel_2AGENTS
- @subpage md_mono_8engine_2ecs_2AGENTS
- @subpage md_mono_8engine_2input_2AGENTS
- @subpage md_mono_8engine_2render_2AGENTS
- @subpage md_mono_8client_2AGENTS
- @subpage md_mono_8server_2AGENTS
- @subpage md_mono_8tools_2AGENTS
- @subpage md_mono_8tools_2docgen_2AGENTS
