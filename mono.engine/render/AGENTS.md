# render - module invariants

L12, `client` tier. Absent from the server binary entirely.

## No SDL GPU header in a public header, and no SDL GPU type in `Renderer.hpp`

Two claims, and they are not the same claim. Keeping them apart is the point,
because one is absolute and the other has two named exceptions.

**No public header includes an SDL header.** That is what keeps this module's
build cost off every consumer, and it holds everywhere with no exceptions.

**`Renderer.hpp` names `struct SDL_Window;` as a forward declaration and
nothing else.** `Renderer.hpp:721` states the consequence at the type: `Device`
is an `SDL_GPUDevice *` and `ColourFormat` an `SDL_GPUTextureFormat`, and
neither name appears. That is what lets a caller hand this module a frame
without acquiring a graphics API.

**`MeshTable.hpp` and `TextureTable.hpp` are the exceptions, and they are
deliberate.** Both forward-declare `SDL_GPUDevice`, `SDL_GPUBuffer` and
`SDL_GPUTexture` and hand them out - `MeshTable.hpp:140` takes a device,
`MeshTable.hpp:242` returns a buffer. A resident-resource table is a handle to
a device object and there is nothing else for it to be; hiding it behind an
opaque integer would buy a caller nothing and cost a lookup per draw. Their
callers are inside this module and `mono.studio`, both of which already speak
SDL.

Until v0.19 this section claimed the first sentence for every public header,
which was true of `Renderer.hpp` and false of the module. If a third header
grows an SDL type, the question to answer here is why it is not a fourth
mistake.

## What crosses into this module is `scene`, and the conversion happens here

`Renderer::Render` takes `scene::DrawInstance` and `scene::Camera`. It used to
take a `render::Instance` and a `render::Camera` of its own, and both are gone:
a `server`-tier host publishes a draw list and a `client`-tier consumer reads
it, so the payload cannot be a type only this tier can name. `v02v03v04.md`
§2.11 is the argument.

Two rules follow, and neither is checkable by the build:

- **The device layout is private and stays private.** `GpuInstance` in
  `Renderer.cpp` is the `mat4` and the RGBA the pipeline binds, and it is built
  straight into the mapped transfer buffer. Promoting it to a public header
  would put a vertex layout back in front of a caller and would be
  `render::Instance` again under a new name.
- **The view-projection comes from `scene::ResolveCamera`.** Building one here
  is what this module did before, and it was a second answer to handedness, clip
  depth and the order of the product - a disagreement that reads as z-fighting
  rather than as a matrix mistake. The aspect ratio is the swapchain's, because
  that is the image the frame is drawn into.

## The design being followed is RENDER_PIPELINE.md

The renderer is a **graph**: a frame declares what it needs produced and in what
order, and the engine works out what to skip, what to reuse from last frame, and
what never needed computing. `RENDER_PIPELINE.md` is that design and it is the
authority for this module. Read it before adding a pass.

**Where decision 11 and RENDER_PIPELINE.md disagree, this module follows
RENDER_PIPELINE.md.** `repo_layout.md` §16 decision 11 says user shaders compile
at cook time with no compiler on the client. §11.9.1 requires the opposite: a
Tier A swap recompiles a `ShaderScript` whose revision changed, while the engine
is running. Its stated reopen condition - "a game needs generated shaders" - is
met by the graph's own design, so the compiler ships. That is why
`ShaderCompiler` exists and why `libshaderc` is a client dependency.

### Its directory names are not ours

`RENDER_PIPELINE.md` was written against the previous engine, whose layout had
no layer stack. Two of its directories map onto three of ours, and getting this
wrong puts the graph runtime at the wrong height with the wrong tier:

| In that document | Here | Why |
|---|---|---|
| `render/` - device primitives, "nothing in it knows what a frame is" | `mono.engine/render/` **[client, L12]** | meshes, images, pipeline building, shader compilation and reflection |
| `renderer/` - graph, nodes, capabilities, caching, executor | split, see below | one directory there, two layers here |
| `renderer/{Graph,NodeDesc,Handle,Capability,Compile,Executor,Cache,Editor}` | `mono.engine/graph/` **[shared, L9]** | the runtime. `repo_layout.md` §9: one graph library, N node sets, and the node families live with their consumers |
| `renderer/nodes/`, `Shadow`, `Frame`, `Pool`, `ShaderCache`, `Device`, `Swapchain` | `mono.engine/render/` **[client, L12]** | the render node set and its device resources |

**Do not create `mono.engine/renderer/`.** It would sit at no defined height and
would have to be `client` tier, which puts the graph runtime out of reach of
physics, replication and audio - the four other consumers decision 12 exists to
serve.

### Where this module actually stands

Stage 0 of §16's twelve. `Renderer::Render` is one instanced opaque pass and one
overlay pass, and it is a placeholder for stage 1's skeleton rather than a first
increment of the graph - it also violates §1's own split by knowing what a frame
is, which the eventual `render/` must not.

Two prerequisites of the design are deliberately absent, and both are recorded
in `docs/DEFERRED.md`: `ecs::ChangeChannel`, which stage 2's per-node cache
invalidation is built on, and `ecs::Column`/`ComponentSet`, which the document
expects nodes to be stored in as rows rather than objects.

So: **do not start building the graph inside this module.** It needs L9 to exist
first, and L9 needs L3's storage. Adding a hand-rolled pass list here in the
meantime is the thing §2 was written against.

## Two shader compilers, and they are not interchangeable

`glslc` compiles `Engine::resources`' `shaders/` during the build. `libshaderc`,
behind `ShaderCompiler`, compiles shaders that do not exist until the engine is
running. Both are the same upstream project and neither replaces the other:

- **A built-in shader failing is a build failure.** That is where it belongs -
  nobody should discover it in a frame. Do not move the built-in `shaders/` to
  runtime compilation to save build time.
- **A user shader failing is a diagnostic string.** It goes back to whoever
  authored it, with the shader's name and a line number, and the engine keeps
  running. Do not make it fatal.

The renderer is a graph, so the second case is not an edge case: a
`ShaderScript` whose revision changed, a swapped antialias pass, a permutation
the demand pass solved for - none of those are known at build time.

**`ShaderLibrary` is the consumer, and it is what closed `D00110` at v0.15.**
Until then `ShaderCompiler` had no caller and `ShaderScript` was a name in this
file with no class behind it - the compiler existed for a case nothing could
reach. `ShaderLibrary::Resolve` is now the one path from a name to a module: a
`ShaderScript` in the world compiled here, else a built-in `glslc` staged at
build time, else a diagnostic. The two compilers above meet in that order and
nowhere else.

**A shader no longer reaches the GPU only through `CreatePipelines`.** That was
true while the pipeline set was fixed, and it is the sentence `D00110` was
written around. `DrawSlots` binds a per-name variant per run now, so adding a
`.frag` to `Engine::resources` is a change in two places: the file, and
`BuiltInShaderNames`. A file added without the second is a shader nothing can
select, which is exactly the trap `resources/AGENTS.md` refuses for meshes and
textures.

**The test that matters is the malformed one.** A compiler that reports success
unconditionally passes every "valid shader compiles" test ever written. If
`ShaderCompiler`'s error path is ever reworked, the assertion to keep is that
invalid input produces a *non-empty* error.

## The shader format comes from the device, and nothing here names one

`SDL_CreateGPUDevice` used to be asked for `SDL_GPU_SHADERFORMAT_SPIRV` and
three `SDL_GPUShaderCreateInfo::format` lines used to repeat it. That is a
literal describing what the build happened to produce on the machine it was
written on, and SDL's Metal backend takes MSL or a `metallib` and never SPIR-V -
so on macOS the first of those returned null before a shader was opened and the
other three would have been wrong afterwards.

`src/ShaderBinary.hpp` is the one place that answers it now, and it answers
three things at once because they move together: the format enumerator, which
of the two staged files to open, and the entry point name. **MSL reserves
`main`, so a translated module's entry point is `main0`** - a caller that opened
the right file and asked for the wrong name gets a per-shader failure that names
the shader and not the reason.

**Preferring SPIR-V where a device offers both is deliberate**, and MoltenVK is
why it is not hypothetical: a Vulkan device on Apple hardware takes SPIR-V, and
that is the path this repository has actually run.

## Translating is `Engine::msl`, and it is not a second compiler here

`Engine::msl` turns SPIR-V into MSL. `render` calls it from
`AddShaderVariant` - a `ShaderScript` does not exist at build time, so a device
that takes MSL gets nothing at all unless the engine can translate one while it
runs - and `mono.tools/shadercross` calls the same function over the built-in
shaders during the build.

**Do not move that code in here.** Two callers at two times is the whole reason
it is a module: a build tool must not link a renderer to get it, and two
implementations would disagree about which texture is `[[texture(0)]]` the first
time somebody edited one. That disagreement is invisible on Linux and is a
surface sampling somebody else's map on a Mac.

A translation failure is a diagnostic and a refused variant, not a fatal - the
same split as the two compilers above, for the same reason.

## No shaderc type in a public header either

`ShaderCompiler.hpp` takes a string and returns a `vector<uint32_t>`. That is
the whole reason the wrapper exists - `render`'s consumers must not have to
acquire a compiler API to include a header, any more than they should have to
acquire SDL.

## The built-in shaders belong to `Engine::resources`, and are reached by name

They live in `mono.engine/resources/shaders/`, compile into
`shaderstage/resources/` and stage into `<program>/shaders/resources/`. This
module *binds* them; it does not own them. `resources::Shader("opaque.vert")` is
the only way to name one - the staged directory is spelled once, in that module,
so nothing here carries a path that a rename could leave behind.

**The per-module directory is still what keeps two shader sets apart.** A module
that grows GLSL of its own compiles it under its own name, and reaching sideways
into another module's `shaders/` is as wrong as including its private headers.
What changed in v0.14 is which module the engine's defaults sit in, not that
each set has one owner: `resources` is linked for its files exactly as a library
is linked for its symbols, and a program that does not link it does not stage
them.

Shaders a *game* author writes are a different thing entirely and none of them
are in this repository.

## The debug panels do not share the renderer's state

`Overlay.hpp` and `DebugPanels.hpp` draw pixels into a CPU buffer. That is
deliberate: the panels have to work when the renderer is the thing being
debugged, so they must not depend on its pipelines, its descriptor state or its
frame pacing. One texture upload per frame, and only while a panel is open.

Do not reimplement them over an immediate-mode UI library.

## The pass list is gone. A pipeline is a graph, and this module runs its nodes

**Until v0.15 `Renderer::Render` submitted six passes by name** - shadow,
surface, opaque, transparent, overlay, interface - and this section described a
`render::Pass` enum, a `PassOrder()`, a `PassRecorder` and a
`graph::StandardPipeline` kept in step with it by `tests/Passes.cpp`. None of
those five names exists in the tree. The render-node system that section said
"when it arrives, this class becomes the backend those nodes compile to"
arrived, and this is that backend.

**The seam is `graph::NodeRunner` and this module's adapter is
`render::GraphRunner`.** `graph/RenderGraph.hpp:47` states it from the other
side: the graph is executed through a `NodeRunner` the caller supplies, so
`render` implements one over SDL. `graph` knows the order, the resources and
which node reads a target nothing wrote; this module knows how to draw one. The
edge runs `render` to `graph` and never back, which is what §6.2 of
`docs/CODE_ARCH.md` means by `graph` not depending on `render`.

**What `tests/Passes.cpp` checks now is acceptance, not order.** Its own header
says it: "The renderer no longer has a parallel enum or fixed pass list. A
pipeline is accepted only when every enabled node has a backend implementation."
A node enabled in a document with nothing here to draw it fails without a
device. That is the check that replaced the two-lists-in-step one, and it is
strictly better: the old one could only catch a seventh entry added to one side.

**The hole the old section named was closed at v0.19, and this is the shape it
left.** A pass drawn by calling `SDL_BeginGPURenderPass` inline is invisible to
the graph, and `src/Renderer.cpp` did that inside a `RenderView` that ran 5,485
lines - two fifths of the module - holding its node handlers as lambdas over its
own locals. `docs/ARCH_REVIEW.md` C2 is the finding; `D00016` is the entry.

**Fifteen calls rather than the eighteen C2 counted**, because three of the
eighteen were `ENGINE_ERROR` strings naming the function rather than calls to
it. Eight are inside a node family's runner now and five are inside the shared
recording in `src/ScenePasses.cpp` - `OpenScenePass`, `Fullscreen`, `DrawImage`,
`DrawOverlayImage` and `ClearOcclusion` - each of which runs only from a node,
so the pass it opens is still inside a node's execution and still named in the
frame graph.

**Two stay outside the graph deliberately, and both are in
`ViewRecording::Finish`.** Neither is a candidate for a node family:

- **The host chrome pass.** Studio panels are a host concern rather than a stage
  of a universe's render graph, and they are recorded *after* `output-image` for
  exactly that reason - so a graph preview, an authored capture and a rendering
  profile hold only the game image and the game interface. Making it a node
  would put it in the graph's description of the frame and therefore in every
  capture taken from one.
- **The clear of a window nothing touched.** It exists precisely for the frame
  where *no* node reached the swapchain: the world went offscreen and neither
  the overlay nor the interface is open. There is no node to put it in, because
  its whole condition is that none ran. Presenting a texture the driver handed
  back unwritten shows last frame's image or uninitialised memory.

### Where a pass lives

| File | What is in it |
|---|---|
| `src/RendererState.hpp` | `Renderer::Impl` - every device object the module owns |
| `src/RenderTypes.hpp` | the GPU layouts and the frame-wide constants |
| `src/ViewRecording.hpp` | what one view's recording holds, and the operations every family shares |
| `src/ViewRecording.cpp` | `Begin`, which works the frame out, and `Finish`, which runs the graph and submits |
| `src/ScenePasses.cpp` | `OpenScenePass`, `DrawWorldInto`, `Fullscreen`, `DrawImage` and the rest of the shared recording |
| `src/nodes/*.cpp` | one file per node family, each registering its own runners |

**`ViewRecording` is what the handlers used to close over, and naming it is the
whole trick.** A handler needed `openScenePass`, so it had to be written in the
same function as it; the state is a type now, the shared operations are its
members, and a family is a file. `Renderer` makes it a friend so that
`Renderer::Impl` stays private to `src/` - nothing about the split reaches
`include/`.

**Its members are not published from locals, they *are* the locals.** `Begin`
binds a reference per name and writes through it, and each handler binds the
same names back the other way. A value the passes read a thousand lines from
where it was decided therefore has one home rather than two that can drift.

**It is also why this module is no longer slow to build.** One 13,680-line
translation unit cannot be split across cores, so the whole module waited on one
compile. Measured on a 24-core machine with `release`'s flags, `CCACHE_DISABLE=1`
and no unity build, compiling every source in the module at `-j24`:

| | before | after |
|---|---|---|
| units | 22 | 38 |
| wall clock | **11.0 s** | **6.5 s** |
| CPU seconds | 34.9 | 76.3 |
| slowest unit | `Renderer.cpp`, **10.6 s** | `ViewRecording.cpp`, **3.8 s** |

**The CPU seconds nearly double, and that is the trade rather than a
regression.** Sixteen more files parse `RendererState.hpp` and `RenderTypes.hpp`,
and 72% of this repository's first-party compile cost is the frontend. What a
developer waits on is the wall clock, and the module no longer has a unit long
enough to be the whole build's critical path - which was the other half of C2's
argument.

**`docs/ARCH_REVIEW.md` E2 estimated 22 s off the tree's wall clock and that
number is stale.** It was derived from `Renderer.cpp` at 31.2 s, measured with
another job stealing 420% CPU and before E1's include fixes landed. The same
unit is 10.6 s today, so the saving available from this change was at most about
7 s of critical path, not 22.

**So: a new way of drawing is a node with a backend in `src/nodes/`, and never
an inline render pass.** Adding the second is what makes the graph a description
of some of the frame rather than of the frame - and there is now nowhere else to
put one, which is the point of the layout above.

## The textures this module owns, and what each pass may assume

- **The shadow map** is written by the shadow pass and read by every pass that
  shades. It is `SAMPLER | DEPTH_STENCIL_TARGET`, and both usages are required:
  a depth attachment that is only a target cannot be read, and a shadow map that
  cannot be read is a pass that costs a draw and changes nothing.
- **The surface textures** are written by the surface pass and read by the
  opaque one, both in the same frame and in that order - so a mirror shows
  *this* frame, not the one before it. `SurfaceSlotState::Ready` is what stops
  the first frame sampling whatever the driver handed back.
- **There is a pair per surface index, and the pair is not a recursion trick.**
  Binding a render target as its own sampler is undefined behaviour; writing one
  and binding the other is what makes the surface pass legal. `Surfaces` is
  `MAX_SURFACES` slots, each a `Texture[2]`, a depth buffer and the matrices
  that drew them; a slot is allocated the first time an index renders and kept
  until shutdown.
- **It does give a mirror inside a mirror, and since v0.15 that inner picture is
  a recursion rather than a stale texture.** The exclusion used to be every
  surface, so no mirror was ever drawn into a mirror's texture; it is per view
  now - `if (index == self) continue;`, so a pass excludes only the index it is
  rendering *for*.
- **A pane inside another pane's picture must be drawn from a camera derived
  from that pane's camera, and never from the eye. This is the invariant.**
  `scene::AimSurfaceCameras` places every surface camera by reflecting the
  world's *active* camera, which is the right answer for the screen and the wrong
  one everywhere below it: the coordinate leaves the texture's 0..1 rectangle and
  `opaque.frag` falls back to the plain lit pane, which looks like the pane being
  culled and is a projection fault. `fillMirror` descends depth-first, deriving
  each level with `scene::ReflectCamera` - the same function the aim pass calls,
  so a chain cannot drift from the screen by a sign - into `MirrorLevel`, a pool
  indexed by level *and* slot for `PortalLevel`'s reason.
- **Do not "fix" a depth problem here by running the pass again.** Iterating
  refreshes textures and never moves a camera; that is exactly what was tried
  before v0.15, and `--surface-bounces 5` against 2 came out byte-for-byte
  identical. A number that buys nothing is the shape of this mistake.
- **A view with no pane rectangle keeps the iterating path, and that is not a
  leftover.** A surface camera parented to the world has no face to reflect
  through, and a cross-world pane's picture is a second simulation - nothing can
  reflect a camera through a pane it was never told about. `SurfaceView::
  PaneNormal` being zero is how the pass is told.
- **`Flags.z` is per draw and never per pass.** It means "this draw samples a
  surface texture instead of its own tint". Setting it for the *whole* surface
  pass is what made the floor sample the previous reflection and show the clear
  colour as a black wedge in the pane - found by eye, not by a test. The world
  draws in the surface pass leave it at zero and the mirror runs in that same
  pass set it; both are correct, and the rule is the granularity rather than the
  value. An earlier version of this file said nothing in the surface pass may
  set it, which stopped being true when mirrors began appearing in mirrors.
- **One opaque white texel, bound wherever a real texture is missing.** The
  pipelines declare two fragment samplers and a draw must bind both - an unbound
  sampler is undefined behaviour on several backends where a wrongly bound one
  is merely ignored. The shadow map exists only when something casts and the
  overlay texture only while a panel is open, so a scene of nothing but
  transparent geometry with the panels closed had neither, and the screen pass
  bound no samplers at all and drew anyway. `Impl::FallbackTexture` is a
  resource for the job rather than another texture borrowed for it.

**The shadow and surface passes draw the whole scene; the screen passes draw the
culled set.** A caster outside the camera's frustum still shadows into it and a
mirror shows what is behind the viewer, so culling either to the eye is the
classic version of that bug - shadows that vanish as their casters leave the
screen. That is why the instance buffer holds two ranges rather than one.

## Winding is counter-clockwise seen from outside

`CULLMODE_BACK` plus `FRONTFACE_COUNTER_CLOCKWISE`, so for every triangle
`(v1 - v0) x (v2 - v0)` must point the same way as the face's declared normal.

**Get it backwards and it does not look like a winding bug.** The faces you are
looking at get culled and the ones behind them do not, so a cube renders as an
open box showing its own interior - and as it turns, faces appear and vanish.
That reads as the renderer dropping triangles at random. It shipped that way
once and was diagnosed from a screenshot rather than from the symptom
description.

**Geometry that can be asserted without a GPU should live where a test can reach
it.** `AdornmentGeometry.hpp` is the module's public example: it is a public
header precisely so `tests/AdornmentGeometry.cpp` can check every triangle it
emits with no device. `src/Primitives.hpp` is the private one, checked by
`tests/Primitives.cpp` - a module's own tests may reach its `src/`, so a header
does not have to be published to be tested.

**What that file is, and what it is not.** Until v0.19 this section pointed at a
`Primitives.hpp` and a `tests/Primitives.cpp` that did not exist, and
`docs/ARCH_REVIEW.md` B recorded the gap as "the built-in shapes are back inside
`src/Renderer.cpp` and `src/InterfacePass.cpp`, where nothing checks their
winding". **That description was wrong and the gap was real.** The built-in
shapes are `assets::MakeBuiltin` and `assets/tests/Builtin.cpp` checks every
triangle of every one of them against its declared normal - the winding rule
above is enforced, one module down. What genuinely had no home and no suite was
the arithmetic those two files did around the shapes:

- **the portal beam atlas quadrant**, which was `index % 2` and `index / 2`
  written out three times - once as the shader's lookup window, once as a
  viewport and once as a scissor. Three expressions of one rectangle, and a beam
  that drew into one quadrant while sampling another shadows through the wrong
  doorway. `BeamQuadrant` is the one expression; the suite checks that the four
  tile the atlas exactly and that each one's window is its own viewport.
- **the quad a spatial canvas occupies**, whose normal is deliberately *not* the
  cross product of its own axes: a canvas is laid out in interface pixels so
  `AxisY` runs down the image, and a caller deriving the normal from the axes
  would light every billboard from behind. `SpatialQuad` carries both and the
  suite asserts the sign.

## SDL's clip space is Y-up. Do not "fix" it

The reflex when writing Vulkan is to negate `projection[1][1]` after
`glm::perspective`, because Vulkan's NDC Y points down and GLM builds for
OpenGL's. **That is wrong here.** SDL's Vulkan backend submits a
negative-height viewport - `SDL_gpu_vulkan.c`, "Viewport flip for consistency
with other backends" - so what reaches a shader is already Y-up on Vulkan,
Metal and D3D12 alike.

Adding the correction anyway flips the scene *and* the lighting, which presents
as a shading bug rather than an orientation one. It cost an hour the first time.

The same applies in reverse to the overlay: `overlay.vert` maps `v = 0`, the
first row of the image, to clip `y = +1`.

`GLM_FORCE_DEPTH_ZERO_TO_ONE` **is** still required and is set in `core`. Depth
runs 0..1 on every backend SDL's GPU API targets; only the Y convention is
already handled.

## A null window is headless, and every format question goes through one function

`Initialise(nullptr)` is a device with nothing claimed: no swapchain, nothing
presented, no overlay and no interface pass - and the world still drawn, into
the `SceneTarget` the caller passes to `Render`.

That makes the colour format a question with two answers, so it has exactly one
asker: `Impl::ColourFormat`. Every pipeline and the scene target are built
against whatever it returns, and a second call to
`SDL_GetGPUSwapchainTextureFormat` anywhere else is a pipeline built for one
target and bound to another.

**Headless with no scene target draws nothing rather than pretending to.** Every
pass would run and its result would be discarded, which is a caller mistake
worth reporting rather than a state to tolerate.

## A frame is recorded by one thread, and that is a contract

`Render` and `Shutdown` abort when called from a thread other than the one that
called `Initialise`. Draw several viewports one after another; do not record two
frames at once.

This is a design decision rather than a limitation nobody got round to lifting.
The passes share **one command buffer and one device**, so parallel recording
would serialise at submit and buy nothing - and it would cost the property that
makes a viewport correct, which is that the world pass writes a scene target
*before* the interface pass samples it in the same buffer. An editor's imgui
draw lists are recorded before the renderer runs and bind whatever texture
exists at that moment, so the ordering inside one buffer is the whole reason a
docked viewport shows this frame instead of the last one.

**It is checked rather than written down, because this repository has twice
found a claim that had quietly stopped being true.** v0.7 recorded the
sequential-drawing decision in `ROADMAP.md` and enforced nothing; a sentence in
a document is not a constraint, and the failure it guards against - a second
viewport recording from a worker - produces a driver validation error or a frame
of somebody else's geometry, neither of which points back at the thread that
caused it.

There is deliberately **no handoff**, unlike `ecs::Store::BindToCallingThread`.
A store is picked up by a different worker every tick and a device is not, so a
public rebind here would be a seam for exactly the thing the contract forbids.
Revisit when a viewport's *record* is measurable; at the sizes measured it is
not.

## One buffer pair for every mesh, and a name resolves to a range

`MeshTable` packs every registered mesh into a single vertex buffer and a single
index buffer. A buffer pair per mesh would make every change of mesh a rebind,
which a driver cannot batch across; with one pair a mesh is `(firstIndex,
indexCount, vertexOffset)` and switching costs three integers on a draw call
that was going to be issued anyway.

The price is that adding a mesh re-uploads the whole thing, and that is the
right trade while meshes arrive when *content* does - a handful of times over a
session and never inside a frame. **Growth is a full re-upload rather than a
suballocator**: a free list over device memory is a real allocator with real
fragmentation, and the thing it buys is cheap eviction, which nothing does yet.

`MeshTable::Resolve` never returns null. A mesh that has not arrived is the
ordinary state of a streaming game, and an unknown name draws as a cube -
visibly - rather than making the inner loop branch on null.

## `TextureTable::Find` *does* return null, and the asymmetry is deliberate

It stays honest about absence because two callers need to tell "not registered"
from "registered as something": a thumbnail that has not been built and a
particle run whose sheet has not streamed both want null, and neither wants a
picture. A caller that wants a picture asks `Default` or `Missing` for one, and
which of those it asks for is a decision - see the three-way split below.

## The shared sampler's `max_lod` is what turns the mip chain on

`mipmap_mode` has said `LINEAR` since v0.8 and it bought nothing on its own:
`SDL_GPUSamplerCreateInfo` is zero-initialised, and a `max_lod` of zero clamps
every fetch to level zero whatever the texture holds. A chain built, serialised
and uploaded would still have shimmered, with three modules all looking correct.

The clamp is a constant past `assets::MipLevelCount`'s largest answer rather than
a per-texture number, because SDL takes a LOD range rather than a level count -
a per-texture bound would mean a sampler per texture, and one sampler is what
makes a draw call cheap here.

**This module never builds levels for a texture that arrived from content**, and
it does build them for the two it compiles in itself. The filter is
`assets::BuildMipChain` - L8, below this module, so calling it here is an
ordinary downward edge. It was `bake::ResizeImage` until v0.15, which is why
`DefaultTexture` and `MissingTexture` shipped with one level for five versions:
nothing a shipped game links may link `bake`, so the two sheets generated here
had no filter they were allowed to reach. They now build a chain at first use -
see their `.cpp` files - and a streamed texture still uploads exactly the levels
it was baked with.

## `DrawSlots` splits consecutive runs and must not sort them

Sorting each run by mesh would produce fewer draw calls, and the blended pass
may not have it: that order is back-to-front from the eye, and reordering it is
exactly the transparency bug the sort exists to prevent. One rule for both
passes is worth more than the draw calls - and a rule that held for one pass and
not the other is the kind that gets applied to the wrong one later.

## The model matrix is not rigid, and the normal matrix is not its upper 3x3

`ToGpu` folds the half-extent into the model matrix, so `mat3(model)` scales
normals as well as rotating them. `opaque.vert` corrects for it by scaling the
normal by one over the square of each axis first - three multiplies rather than
an inverse transpose per vertex.

**This was wrong for four versions and invisible**, because an axis-aligned
normal comes out of the wrong matrix pointing the right way and is renormalised
in the fragment. It became visible the moment a sphere or an imported mesh was
scaled unevenly. The comment in the shader that said the transforms were rigid
was describing what the code assumed rather than what it did.

## What is not here yet

- **No filter on the screen pass, and there must not be one.** A surface camera
  filters by tag; the window shows the world. `DrawSlots` takes a filter and
  every screen-pass call site passes zero, deliberately - a filtered window is a
  game where the player sees a group and cannot tell why.

## The fallback texel, the default texture and the marker are three things

`FallbackTexture` is one white texel and exists so a sampler a pipeline declares
is never unbound - undefined behaviour on some backends, a validation error on
others. It is a stand-in for a **binding**.

`DefaultTexture` is what a drawable with no material is *made of*: a real sheet
of white plastic, compiled in, bound in the colour slot whenever `Textures.Find`
answers nothing. `TextureTable::Default` holds it outside the map so no `Add` can
replace it and no `Drop` can release it.

**Conflating the two is the bug this split exists to record.** The colour slot
took the fallback texel and set the shader's "no texture" flag, so every
untextured part in every scene was flat, untextured white - which is what a
seventeen-name `Material` enum was supposed to be fixing and could not, because a
name is not a texture. Do not reach for `FallbackTexture` in the colour slot
again; the shadow and surface slots are where it is still right, because those
are genuinely absent features rather than defaulted ones.

**It is compiled in and it must stay that way.** It is what a part draws with
before any content has streamed, on a machine with no content store, and on the
frame a fetch fails. A default that had to be fetched would be absent in exactly
the cases it exists for.

`MissingTexture` is the third, and it answers a question the other two do not:
a drawable that *named* a texture this table does not hold. The colour slot
therefore resolves three ways, and the distinction between the last two is the
whole reason the marker exists:

| what the drawable says | what the slot gets |
| --- | --- |
| no texture named | `Default` - the plastic. `Material = None` is a finished state. |
| a name, registered | that texture. |
| a name, not registered, **expected** | `Default` - it is on its way. |
| a name, not registered, not expected | `Missing` - the purple checkerboard. Not a finished state. |

`ChooseTexture` is that table as a function, and it is a free one so a suite can
state the rule without a device.

**Drawing the last two the same way is the bug this closes.** An author's typo
and a sheet that never published both rendered as the default material, which
looks exactly like a part somebody deliberately left untextured - so a missing
texture had no symptom at all until somebody noticed the model was the wrong
colour. It is the same split `scene::KeepLoaded` makes for geometry: no mesh
named draws the cube, a mesh named and absent draws nothing.

**The base colour stops applying when the marker is bound**, and that is not an
optimisation to remove. Every other texture in the slot is modulated by the
material's colour - one grey sheet serving a whole palette - but a magenta check
multiplied by a dark red part is a dark pattern that reads as intent. A marker
that can be tinted into looking deliberate is not a marker.

**"Not here yet" and "never coming" used to be the same state here, and that was
`D00107`.** This module knows what it holds and not what is in flight, so a
sheet still streaming wore the marker for the frames it took to arrive - a purple
shimmer across every imported model on a scene load, indistinguishable from forty
misspellings.

**The fix is that the content pump says what it has outstanding**, through
`Renderer::ExpectTexture` and `StopExpectingTexture`, and the marker now means
only *nothing is coming*. Not a timer in this module, and that was never a close
call: a grace period hides a genuinely missing texture for exactly as long as it
hides a streaming one, and with a byte budget in the path there is no N right for
both a small scene and a large one.

**The load-bearing half is the unmark, and it goes on the request *finishing*
rather than on it succeeding.** A host that unmarked only on arrival would leave
a misspelled sheet expected for ever and the marker would never appear for the
one case it exists for. A failure carries no name - `Take` answers nothing - so
`delivery::AssetClient::NameOf` exists for it, and both hosts read the name
*before* taking because a take is what destroys the record.

## Cascaded presentation caches describe dependencies, not extra copies

`PresentationDamageTracker` is the common invalidation boundary used by the
client and by every Studio viewport. Its graph has three kinds of node:

- resident sources: object rows, particle rows, environment inputs and portal
  inputs;
- retained images: portal history, the scene image, the game interface and the
  Studio interface;
- compositions: game, Studio, then the final presented image.

This is a dependency graph, not an instruction to allocate one full-size
texture for every named node. A node may be a resident buffer, a retained draw
list, an existing renderer target or the decision to avoid acquiring a
swapchain image. Adding a duplicate image merely to make the diagram literal is
wrong unless a measured consumer needs it.

`ScenePresentationSignaturesOf` signs the four source groups independently.
Those signatures are causes, not another scene registry and not dirty flags.
The ECS remains the storage, renderer residency remains keyed by its stable
slots, and a signature only answers whether the retained result may be reused.
A source change invalidates the scene image and every composition above it; it
does not invalidate either interface sideways. A game-interface change starts
at the game composition. A Studio-interface change starts at the Studio
composition.

The baseline advances only after pixels were successfully rendered or a
headless render completed. A failed swapchain acquisition must leave the old
baseline intact so the same damage is retried. Portals are the special retained
input: their history is the last image actually rendered for that portal, and
`FrameResult::PortalPasses` is the evidence that history was written. Do not
infer a portal-history write from a changed portal descriptor alone.

Cache profiling uses hit and write counters in
`PresentationCacheProfile`. A hit has no duration, so it must not be represented
as a fabricated timing span. The Frame Graph panel's `Cascaded Cache Hits` view
shows the dependency depth, last decision and cumulative hit rate per viewport.
Any new retained layer must add one row there and tests for its upward cascade.

On a hit, that layer owes no upload, no command buffer and no transient
allocation. Validate this in a steady scene with the release preset, the cache
counters, `FrameResult` traffic counters and GPU heap statistics together. A
high hit rate with upload bytes or logical GPU memory still growing is a bug,
not a successful cache.
