# render — module invariants

L12, `client` tier. Absent from the server binary entirely.

## No SDL GPU type in a public header

`Renderer.hpp` names `struct SDL_Window;` as a forward declaration and nothing
else. Everything else is behind the pimpl in `Renderer.cpp`.

That is what lets a caller hand this module a frame without acquiring a graphics
API, and it is what will let the L9 render graph sit on top of this without
inheriting SDL's vocabulary.

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
  depth and the order of the product — a disagreement that reads as z-fighting
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
is running. Its stated reopen condition — "a game needs generated shaders" — is
met by the graph's own design, so the compiler ships. That is why
`ShaderCompiler` exists and why `libshaderc` is a client dependency.

### Its directory names are not ours

`RENDER_PIPELINE.md` was written against the previous engine, whose layout had
no layer stack. Two of its directories map onto three of ours, and getting this
wrong puts the graph runtime at the wrong height with the wrong tier:

| In that document | Here | Why |
|---|---|---|
| `render/` — device primitives, "nothing in it knows what a frame is" | `mono.engine/render/` **[client, L12]** | meshes, images, pipeline building, shader compilation and reflection |
| `renderer/` — graph, nodes, capabilities, caching, executor | split, see below | one directory there, two layers here |
| `renderer/{Graph,NodeDesc,Handle,Capability,Compile,Executor,Cache,Editor}` | `mono.engine/graph/` **[shared, L9]** | the runtime. `repo_layout.md` §9: one graph library, N node sets, and the node families live with their consumers |
| `renderer/nodes/`, `Shadow`, `Frame`, `Pool`, `ShaderCache`, `Device`, `Swapchain` | `mono.engine/render/` **[client, L12]** | the render node set and its device resources |

**Do not create `mono.engine/renderer/`.** It would sit at no defined height and
would have to be `client` tier, which puts the graph runtime out of reach of
physics, replication and audio — the four other consumers decision 12 exists to
serve.

### Where this module actually stands

Stage 0 of §16's twelve. `Renderer::Render` is one instanced opaque pass and one
overlay pass, and it is a placeholder for stage 1's skeleton rather than a first
increment of the graph — it also violates §1's own split by knowing what a frame
is, which the eventual `render/` must not.

Two prerequisites of the design are deliberately absent, and both are recorded
in `docs/DEFERRED.md`: `ecs::ChangeChannel`, which stage 2's per-node cache
invalidation is built on, and `ecs::Column`/`ComponentSet`, which the document
expects nodes to be stored in as rows rather than objects.

So: **do not start building the graph inside this module.** It needs L9 to exist
first, and L9 needs L3's storage. Adding a hand-rolled pass list here in the
meantime is the thing §2 was written against.

## Two shader compilers, and they are not interchangeable

`glslc` compiles this module's `shaders/` during the build. `libshaderc`, behind
`ShaderCompiler`, compiles shaders that do not exist until the engine is
running. Both are the same upstream project and neither replaces the other:

- **A built-in shader failing is a build failure.** That is where it belongs —
  nobody should discover it in a frame. Do not move `shaders/` to runtime
  compilation to save build time.
- **A user shader failing is a diagnostic string.** It goes back to whoever
  authored it, with the shader's name and a line number, and the engine keeps
  running. Do not make it fatal.

The renderer is a graph, so the second case is not an edge case: a
`ShaderScript` whose revision changed, a swapped antialias pass, a permutation
the demand pass solved for — none of those are known at build time.

**The test that matters is the malformed one.** A compiler that reports success
unconditionally passes every "valid shader compiles" test ever written. If
`ShaderCompiler`'s error path is ever reworked, the assertion to keep is that
invalid input produces a *non-empty* error.

## No shaderc type in a public header either

`ShaderCompiler.hpp` takes a string and returns a `vector<uint32_t>`. That is
the whole reason the wrapper exists — `render`'s consumers must not have to
acquire a compiler API to include a header, any more than they should have to
acquire SDL.

## Shaders belong to this module

They live in `shaders/`, they compile into `shaderstage/render/`, and they stage
into `<program>/shaders/render/`. Do not add a shared shader directory. `vfx`
must not reach into `render/shaders/` any more than it may include this module's
headers, and a per-module directory is what makes that structural rather than a
rule.

Shaders a *game* author writes are a different thing entirely and none of them
are in this repository.

## The debug panels do not share the renderer's state

`Overlay.hpp` and `DebugPanels.hpp` draw pixels into a CPU buffer. That is
deliberate: the panels have to work when the renderer is the thing being
debugged, so they must not depend on its pipelines, its descriptor state or its
frame pacing. One texture upload per frame, and only while a panel is open.

Do not reimplement them over an immediate-mode UI library.

## Five passes are not an architecture either

`Renderer::Render` is a shadow pass, a surface pass, an opaque pass, a
transparent pass and an overlay pass, submitted in that order by a function that
knows all five by name. It is enough to prove the staged-shader path, the depth
buffer, an offscreen target and the swapchain, and that is all it claims to be.

**`mono.engine/graph` describes that order and does not execute it.**
`graph::StandardPipeline` is the same five stages as data, and
`Pipeline::Validate` catches the one mistake that matters — a stage reading a
target nothing earlier wrote.

**Keeping the two in step is a check, not a convention.** `render::Pass` and
`PassOrder()` name this module's five in submission order, and
`tests/Passes.cpp` compares them against that pipeline's stage names, in order,
with no device. A sixth stage on one side and not the other fails the build.
`PassRecorder` walks the same list as `Render` submits and refuses to go
backwards, which is the half a headless test cannot see.

**So: enter every pass through `PassRecorder`, and add its stage to
`StandardPipeline` in the same change.** The first is what the check hangs on —
a pass drawn by calling `SDL_BeginGPURenderPass` inline is invisible to all of
the above, and that is the one hole left. See `D00016`.

The render-node system is where passes become nodes and the description becomes
the execution. When it arrives, this class becomes the backend those nodes
compile to — so do not grow the hand-rolled list further in the meantime. Two
competing ways to describe a frame is worse than either.

## The two textures this module owns, and what each pass may assume

- **The shadow map** is written by the shadow pass and read by every pass that
  shades. It is `SAMPLER | DEPTH_STENCIL_TARGET`, and both usages are required:
  a depth attachment that is only a target cannot be read, and a shadow map that
  cannot be read is a pass that costs a draw and changes nothing.
- **The surface texture** is written by the surface pass and read by the opaque
  one, both in the same frame and in that order — so a mirror shows *this*
  frame, not the one before it. `SurfaceReady` is what stops the first frame
  sampling whatever the driver handed back.
- **There are two of them and the pair is not a recursion trick.** Binding a
  render target as its own sampler is undefined behaviour; writing one and
  binding the other is what makes the surface pass legal. It does **not** give a
  mirror inside a mirror, because `sceneReflected` partitions surface instances
  out of that pass — no mirror is ever drawn into a mirror's texture. This file
  and two comments claimed otherwise, and the claim was not harmless: it is why
  `Flags.z` was set for the entire surface pass instead of for the mirrors in
  it, which made the floor sample the previous reflection and show the clear
  colour as a black wedge in the pane. **Nothing in the surface pass may set
  `Flags.z`.** Real recursion needs a per-view exclusion this pipeline has no
  shape for, and belongs with the render-node system.

**The shadow and surface passes draw the whole scene; the screen passes draw the
culled set.** A caster outside the camera's frustum still shadows into it and a
mirror shows what is behind the viewer, so culling either to the eye is the
classic version of that bug — shadows that vanish as their casters leave the
screen. That is why the instance buffer holds two ranges rather than one.

## Winding is counter-clockwise seen from outside

`CULLMODE_BACK` plus `FRONTFACE_COUNTER_CLOCKWISE`, so for every triangle
`(v1 - v0) x (v2 - v0)` must point the same way as the face's declared normal.

**Get it backwards and it does not look like a winding bug.** The faces you are
looking at get culled and the ones behind them do not, so a cube renders as an
open box showing its own interior — and as it turns, faces appear and vanish.
That reads as the renderer dropping triangles at random. It shipped that way
once and was diagnosed from a screenshot rather than from the symptom
description.

The geometry lives in `Primitives.hpp` rather than inside `Renderer.cpp` for one
reason: so `tests/Primitives.cpp` can assert this for all twelve triangles
without a GPU. Any mesh added there gets the same check.

## SDL's clip space is Y-up. Do not "fix" it

The reflex when writing Vulkan is to negate `projection[1][1]` after
`glm::perspective`, because Vulkan's NDC Y points down and GLM builds for
OpenGL's. **That is wrong here.** SDL's Vulkan backend submits a
negative-height viewport — `SDL_gpu_vulkan.c`, "Viewport flip for consistency
with other backends" — so what reaches a shader is already Y-up on Vulkan,
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
presented, no overlay and no interface pass — and the world still drawn, into
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
would serialise at submit and buy nothing — and it would cost the property that
makes a viewport correct, which is that the world pass writes a scene target
*before* the interface pass samples it in the same buffer. An editor's imgui
draw lists are recorded before the renderer runs and bind whatever texture
exists at that moment, so the ordering inside one buffer is the whole reason a
docked viewport shows this frame instead of the last one.

**It is checked rather than written down, because this repository has twice
found a claim that had quietly stopped being true.** v0.7 recorded the
sequential-drawing decision in `ROADMAP.md` and enforced nothing; a sentence in
a document is not a constraint, and the failure it guards against — a second
viewport recording from a worker — produces a driver validation error or a frame
of somebody else's geometry, neither of which points back at the thread that
caused it.

There is deliberately **no handoff**, unlike `ecs::Store::BindToCallingThread`.
A store is picked up by a different worker every tick and a device is not, so a
public rebind here would be a seam for exactly the thing the contract forbids.
Revisit when a viewport's *record* is measurable; at the sizes measured it is
not.
