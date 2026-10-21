# Module guides

Find module invariants and guides by engine layer or owning product. Read the
owning module's invariants before changing its code;
[AGENTS.md](@ref md_AGENTS) explains the shared rules.


@htmlonly
<div class="docs-directory">
@endhtmlonly

- @subpage docs_modules_overview

  Shared invariants for every engine library.

- @subpage docs_modules_foundations

  Platform values, parallel work, storage and world ticking.

- @subpage docs_modules_simulation

  Geometry, authored scenes, assets and simulation systems.

- @subpage docs_modules_execution

  Content processing, scripting and transport services.

- @subpage docs_modules_runtime

  High-level runtime and presentation systems.

- @subpage docs_modules_products

  Programs and their owned modules outside the engine layer stack.

- @subpage docs_modules_tools

  Developer tools and their module guides.


@htmlonly
</div>
@endhtmlonly


Generated from the module graph and source guides. After adding module documentation,
run `just docs-pages`; `just docs-pages-check` checks this index.

@page docs_modules_overview Engine overview

Shared invariants for every engine library.

@htmlonly
<div class="docs-directory docs-module-directory">
@endhtmlonly

- @subpage md_mono_8engine_2AGENTS "engine"

  `mono.engine/`


@htmlonly
</div>
@endhtmlonly


@page docs_modules_foundations Foundations (L1-4)

Platform values, parallel work, storage and world ticking.

@htmlonly
<div class="docs-directory docs-module-directory">
@endhtmlonly

- @subpage md_mono_8engine_2core_2AGENTS "core"

  `mono.engine/core/`

- @subpage md_mono_8engine_2graphcore_2AGENTS "graphcore"

  `mono.engine/graphcore/`

- @subpage md_mono_8engine_2parallel_2AGENTS "parallel"

  `mono.engine/parallel/`

- @subpage md_mono_8engine_2computegraph_2AGENTS "computegraph"

  `mono.engine/computegraph/`

- @subpage docs_module_mono_8engine_2ecs "ecs"

  `mono.engine/ecs/`

- @subpage md_mono_8engine_2procedural_2AGENTS "procedural"

  `mono.engine/procedural/`

- @subpage docs_module_mono_8engine_2testing "testing"

  `mono.engine/testing/`

- @subpage md_mono_8engine_2world_2AGENTS "world"

  `mono.engine/world/`


@htmlonly
</div>
@endhtmlonly


@page docs_modules_simulation Scene and simulation (L5-8)

Geometry, authored scenes, assets and simulation systems.

@htmlonly
<div class="docs-directory docs-module-directory">
@endhtmlonly

- @subpage md_mono_8engine_2collision_2AGENTS "collision"

  `mono.engine/collision/`

- @subpage md_mono_8engine_2spatial_2AGENTS "spatial"

  `mono.engine/spatial/`

- @subpage md_mono_8engine_2gui_2AGENTS "gui"

  `mono.engine/gui/`

- @subpage docs_module_mono_8engine_2scene "scene"

  `mono.engine/scene/`

- @subpage md_mono_8engine_2assets_2AGENTS "assets"

  `mono.engine/assets/`

- @subpage docs_module_mono_8engine_2audiocodec "audiocodec"

  `mono.engine/audiocodec/`

- @subpage md_mono_8engine_2effects_2AGENTS "effects"

  `mono.engine/effects/`

- @subpage md_mono_8engine_2imagecodec_2docs_2ImageCodec "imagecodec"

  `mono.engine/imagecodec/`

- @subpage md_mono_8engine_2physics_2AGENTS "physics"

  `mono.engine/physics/`

- @subpage docs_module_mono_8engine_2weather "weather"

  `mono.engine/weather/`


@htmlonly
</div>
@endhtmlonly


@page docs_modules_execution Content and execution (L9-11)

Content processing, scripting and transport services.

@htmlonly
<div class="docs-directory docs-module-directory">
@endhtmlonly

- @subpage md_mono_8engine_2bake_2AGENTS "bake"

  `mono.engine/bake/`

- @subpage md_mono_8engine_2bakegraph_2AGENTS "bakegraph"

  `mono.engine/bakegraph/`

- @subpage md_mono_8engine_2graph_2AGENTS "graph"

  `mono.engine/graph/`

- @subpage md_mono_8engine_2imagegraph_2AGENTS "imagegraph"

  `mono.engine/imagegraph/`

- @subpage md_mono_8engine_2script_2AGENTS "script"

  `mono.engine/script/`

- @subpage md_mono_8engine_2scriptjs_2AGENTS "scriptjs"

  `mono.engine/scriptjs/`

- @subpage md_mono_8engine_2scriptluau_2AGENTS "scriptluau"

  `mono.engine/scriptluau/`

- @subpage md_mono_8engine_2delivery_2AGENTS "delivery"

  `mono.engine/delivery/`

- @subpage md_mono_8discord_2AGENTS "discord"

  `mono.discord/`

- @subpage md_mono_8engine_2msl_2AGENTS "msl"

  `mono.engine/msl/`

- @subpage md_mono_8engine_2net_2AGENTS "net"

  `mono.engine/net/`

- @subpage md_mono_8engine_2resources_2AGENTS "resources"

  `mono.engine/resources/`

- @subpage md_mono_8engine_2scripthost_2AGENTS "scripthost"

  `mono.engine/scripthost/`


@htmlonly
</div>
@endhtmlonly


@page docs_modules_runtime Runtime (L12-13)

High-level runtime and presentation systems.

@htmlonly
<div class="docs-directory docs-module-directory">
@endhtmlonly

- @subpage md_mono_8engine_2audio_2AGENTS "audio"

  `mono.engine/audio/`

- @subpage md_mono_8engine_2datastore_2AGENTS "datastore"

  `mono.engine/datastore/`

- @subpage md_mono_8engine_2examples_2docs_2TornadoSim "examples"

  `mono.engine/examples/`

- @subpage md_mono_8engine_2game_2AGENTS "game"

  `mono.engine/game/`

- @subpage md_mono_8engine_2input_2AGENTS "input"

  `mono.engine/input/`

- @subpage md_mono_8network_2AGENTS "network"

  `mono.network/`

- @subpage md_mono_8engine_2render_2AGENTS "render"

  `mono.engine/render/`

- @subpage md_mono_8engine_2replication_2AGENTS "replication"

  `mono.engine/replication/`

- @subpage md_mono_8engine_2ui_2AGENTS "ui"

  `mono.engine/ui/`

- @subpage docs_module_mono_8engine_2control "control"

  `mono.engine/control/`


@htmlonly
</div>
@endhtmlonly


@page docs_modules_products Products

Programs and their owned modules outside the engine layer stack.

@htmlonly
<div class="docs-directory docs-module-directory">
@endhtmlonly

- @subpage docs_module_mono_8cdn "cdn"

  `mono.cdn/`

- @subpage docs_module_mono_8client "client"

  `mono.client/`

- @subpage md_mono_8launcher_2AGENTS "launcher"

  `mono.launcher/`

- @subpage md_mono_8studio_2nodegraph_2AGENTS "studio/nodegraph"

  `mono.studio/nodegraph/`

- @subpage md_mono_8server_2AGENTS "server"

  `mono.server/`

- @subpage docs_module_mono_8studio "studio"

  `mono.studio/`

- @subpage md_mono_8unified__tests_2AGENTS "unified_tests"

  `mono.unified_tests/`


@htmlonly
</div>
@endhtmlonly


@page docs_modules_tools Tools

Developer tools and their module guides.

@htmlonly
<div class="docs-directory docs-module-directory">
@endhtmlonly

- @subpage md_mono_8tools_2AGENTS "tools"

  `mono.tools/`

- @subpage md_mono_8tools_2docgen_2AGENTS "docgen"

  `mono.tools/docgen/`


@htmlonly
</div>
@endhtmlonly


@page docs_module_mono_8engine_2ecs ecs

Guides and invariants for `mono.engine/ecs/`.

@htmlonly
<div class="docs-directory">
@endhtmlonly

- @subpage md_mono_8engine_2ecs_2AGENTS "Invariants"
- @subpage md_mono_8engine_2ecs_2docs_2TODO

@htmlonly
</div>
@endhtmlonly


@page docs_module_mono_8engine_2testing testing

Guides and invariants for `mono.engine/testing/`.

@htmlonly
<div class="docs-directory">
@endhtmlonly

- @subpage md_mono_8engine_2testing_2AGENTS "Invariants"
- @subpage md_mono_8engine_2testing_2docs_2SCENARIOS

@htmlonly
</div>
@endhtmlonly


@page docs_module_mono_8engine_2scene scene

Guides and invariants for `mono.engine/scene/`.

@htmlonly
<div class="docs-directory">
@endhtmlonly

- @subpage md_mono_8engine_2scene_2AGENTS "Invariants"
- @subpage md_mono_8engine_2scene_2docs_2GpuParticleField

@htmlonly
</div>
@endhtmlonly


@page docs_module_mono_8engine_2audiocodec audiocodec

Guides and invariants for `mono.engine/audiocodec/`.

@htmlonly
<div class="docs-directory">
@endhtmlonly

- @subpage md_mono_8engine_2audiocodec_2AGENTS "Invariants"
- @subpage md_mono_8engine_2audiocodec_2docs_2index

@htmlonly
</div>
@endhtmlonly


@page docs_module_mono_8engine_2weather weather

Guides and invariants for `mono.engine/weather/`.

@htmlonly
<div class="docs-directory">
@endhtmlonly

- @subpage md_mono_8engine_2weather_2AGENTS "Invariants"
- @subpage md_mono_8engine_2weather_2docs_2Design

@htmlonly
</div>
@endhtmlonly


@page docs_module_mono_8engine_2control control

Guides and invariants for `mono.engine/control/`.

@htmlonly
<div class="docs-directory">
@endhtmlonly

- @subpage md_mono_8engine_2control_2AGENTS "Invariants"
- @subpage md_mono_8engine_2control_2docs_2MCP

@htmlonly
</div>
@endhtmlonly


@page docs_module_mono_8cdn cdn

Guides and invariants for `mono.cdn/`.

@htmlonly
<div class="docs-directory">
@endhtmlonly

- @subpage md_mono_8cdn_2AGENTS "Invariants"
- @subpage md_mono_8cdn_2docs_2index

@htmlonly
</div>
@endhtmlonly


@page docs_module_mono_8client client

Guides and invariants for `mono.client/`.

@htmlonly
<div class="docs-directory">
@endhtmlonly

- @subpage md_mono_8client_2AGENTS "Invariants"
- @subpage md_mono_8client_2docs_2FramePacing

@htmlonly
</div>
@endhtmlonly


@page docs_module_mono_8studio studio

Guides and invariants for `mono.studio/`.

@htmlonly
<div class="docs-directory">
@endhtmlonly

- @subpage md_mono_8studio_2AGENTS "Invariants"
- @subpage md_mono_8studio_2docs_2FramePacing

@htmlonly
</div>
@endhtmlonly
