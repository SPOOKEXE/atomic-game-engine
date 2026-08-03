# scene — module invariants

L7, `shared` tier. What a thing in a world *is*: the Basic Components, the
`Part` class, the two resources they index, and the draw payload a world
publishes. `v02v03v04.md` §3.1–3.3 is the design; this file is what a reviewer
should refuse.

## Nothing presentation-tier gets in, and no device data at all

This is the rule the whole module exists to make possible, and it is the one a
reasonable-looking change breaks first.

A `shared` module may not link a `client` one, so `mono_check_all_tiers` catches
the *edge* — it will not catch the shape. A `glm::mat4` model matrix in
`DrawInstance`, a packed RGBA8 tint, a mesh index instead of a mesh name, a
field sized to match a uniform buffer's layout: every one of those compiles,
passes the tier check, and puts a GPU's memory layout in a type a headless
server writes.

The test to apply: **could a server with no graphics stack installed produce
this value, and mean it?** If the answer needs a device, a swapchain, an image
format or a shader's struct layout, it belongs in `render`.

Specifically refuse:

- `render::Instance` or `render::Camera` appearing here, in any form. The first
  is what `scene::DrawInstance` replaces; the second is what `ActiveCamera`
  replaces. Two types for one job is the debt this module was created to pay
  off, so recreating it here would be a bad joke.
- A handle where a name belongs. `Visual::Mesh` and `Surface::Material` are
  `core::Name` because they cross a save file and a wire, and rule 4 is that a
  name crosses and a number does not.
- A pointer in anything that reaches `world::ViewChannel`. The day a world is a
  process, every one of them is a use-after-free.

## What it deliberately does not depend on

All three of these are `shared`, so **the build cannot catch any of them** — by
rule 6 that makes them conventions, and this is where they are written down.

- **`physics`** (L8, above this). It reads these components; they must not know
  it exists. An edge that way inverts the layer stack, and it is the edge that
  makes `Collider` acquire a contact list "just for now".
- **`render`** (L12, `client`). See above. The tier check does cover this one.
- **`world`** (L4, below this). Legal by height and still wrong: a component set
  is data, and a data module that knows about universes, buses and ticking is a
  module the physics tests cannot use without standing a world up. `scene` takes
  an `ecs::Store &` and nothing larger.

`core`, `ecs` and `spatial` are the whole dependency list, and it should stay
that way. A new dependency here is a new dependency for everything that will
link this.

## `spatial` is here for `LayerMask` and for nothing else

`Collider::Layer` and `Collider::Mask` are `spatial::LayerMask`, not
`uint32_t`. They are the same width and mean opposite things — the layers a
collider is on, and the layers it is tested against — so passing them to a query
the wrong way round compiles and returns a plausible wrong answer. One named
type on both sides of that call is the only thing that narrows it.

**That is the whole of the edge.** Refuse anything that widens it:

- A `spatial::Proxy` or a `spatial::HashGrid` in a component or a signature
  here. Building the index is `physics`'s job at L8, over these components; a
  component that knew about the index would be the inversion this file's first
  section refuses in the other direction.
- A cached cell index on `Bounds`. A grid is a query structure over
  `{id, AABB, layers}` and knows nothing about components; a cell index here
  would be the world AABB stored twice, once derived and once cached, and the
  cached one goes stale the first tick something moves without going through the
  setter somebody remembered to add.
- Anything that makes `scene` construct or query a grid. `scene` takes an
  `ecs::Store &` and nothing larger, and it holds data and one resolver.

## `Part` is a class, not a component

There is no `struct Part`, and adding one would be adding a second answer to
"what is a part". The class table is the answer: a name, a parent, and a
component set, registered once, so `Instance.new("Part")` in v0.6 and `MakePart`
in C++ resolve through the same rows.

**`MakePart` is the only constructor.** A loader, a test or a demo that assembles
the five components itself is a second definition, and the two disagree the
first time one gains a component. If `MakePart` cannot express what a caller
needs, extend `PartDesc` — that is what it is for.

**The adopt-only check in `MakePart` is not redundant.** `Store::Create`
refuses to mint in a replica; `Store::CreateInstance` does not, and goes
straight to the directory. So `MakePart` checks `Store::AdoptOnly()` itself, and
removing that line on the assumption that the storage already covers it reopens
the exact collision `SetAdoptOnly` exists to prevent — same index, same
generation, and `Store::Apply` right to merge the two. The proper fix is in
`ecs`, and until it lands this line is what keeps the one minting path this
module owns closed.

**`Anchored` decides presence, never a flag.** An anchored part carries no
`RigidBody` and no `Motion`, so it lands in a different archetype and no dynamic
query ever visits it. A reviewer should refuse a change that adds a `Static`
body to anchored parts "for uniformity": uniformity here costs a row visit per
static part per tick, forever, and static geometry is most of a world.

## The splits are load-bearing, and each has a reason

Refuse a change that merges any of these without a measurement that says the
merge is free.

- **`Motion` apart from `RigidBody`.** Anything with a `Transform` and a
  `Motion` moves and needs no mass — a platform, a projectile, a demo cube. So
  `Integrate` runs over `<Transform, const Motion>` and never loads a mass it
  does not use.
- **`Surface` as a name, not two floats.** Friction and restitution are the same
  two floats on thousands of rows. That is the resource case out of
  `ecs/AGENTS.md`; the floats live in `SurfaceTable` and the narrow phase reads
  a row once.
- **`Bounds` as a local half-extent.** The single source both the broad phase
  and render culling derive a world AABB from. A cached world AABB is a second
  copy of a derived fact, and it goes stale silently.
- **`PreviousTransform` on its own.** Not the velocity buffer, not the temporal
  AA history. `RENDER_PIPELINE.md` §14 flags that reuse as coupling two
  independent features.
- **`Camera` a component and `ActiveCamera` a resource.** A world holds several
  cameras — spectator, cutscene, security monitor — and exactly one live one.
  Collapsing the resource into "the entity with the camera" turns a lookup into
  a search, and collapsing the component into the resource makes the second
  camera impossible.

## `QuickHash` is second-best and the comment says so

Change detection is `ecs::ChangeChannel`. `QuickHash` exists for one gap: a
write through a raw column pointer in the batch path advances no per-row stamp,
because there is no per-row write to hang one on.

It costs a pass over the data every tick whether anything moved or not. **If it
starts appearing beside components nothing writes in bulk, it has spread** — and
the fix is to delete it there, not to make the hash cheaper. A reviewer adding
one should be able to name the batch writer.

## The wire grid is beside `WorldBounds` because it is the same decision

`Wire.hpp` is what `Transform` and `Motion` look like on a replication
datagram: a position as three fixed-point axes, a rotation as smallest-three,
twenty-eight bytes as ten. **How coarse that grid is depends entirely on how far
the world reaches** — two millimetres over 128 metres is a different figure over
four kilometres — so the extent it covers is stated in the same terms as
`WorldBounds::HalfExtent` and the error is stated in metres rather than left to
be worked out.

Three things a reviewer should refuse:

- **A wire form on a component that does not replicate.** `PreviousTransform`
  has none deliberately: it is render-side history and never reaches a wire, so
  declaring a compact form for it would be declaring a format nothing uses and
  nothing tests.
- **A stated bound without a case that measures it.** `engine.scene.wire`
  asserts both that nothing exceeds the bound and that something comes close to
  it. A bound only the first half of that guards is one a widened grid slips
  under.
- **Wrapping instead of clamping outside the extent.** A clamped entity piles up
  against a wall somebody can see; a wrapped one appears at the far side of the
  world and is indistinguishable from a teleport the server meant. Every decode
  clamps as well as every encode, because the bits arriving from a peer are not
  the bits an encoder wrote.

**A world larger than the grid is a decision made where the world is authored**,
not inside the encoder — which sees one component and not a world.
`WireCoversWorld` is the check and `mono.server`'s placeholder world holds it as
a `static_assert`.

## There is no transform hierarchy, and there is not going to be one here

`Transform` is world space. Parenting is organisational, exactly as Roblox's is:
it moves nothing and re-resolves nothing. A `LocalTransform`, a `WorldTransform`,
a dirty flag or a propagation system in this module is the change to refuse — the
absence of that pass is what buys physics a `Transform` read with no resolve step
and a tree that costs four handles per node.

## There is no sleeping flag on `RigidBody`, and there must not be one again

`v02v03v04.md`'s allocation table puts a sleeping body in a *different
archetype* so the query never visits it. A flag on the row is the opposite of
that: it is only readable by making the visit the archetype move exists to
avoid, and it is the same state the solver already has to keep — two answers to
one question, which rule 2 refuses.

`physics` owns it now. A sleeping body loses its `Motion`, which is the
archetype move done with the components that already exist, and how long a body
has been still lives in `physics::PhysicsWorld`. `physics/AGENTS.md` carries the
whole decision, including why a tag would not have worked.

**`RigidBody::Reserved` is three bytes because the flag used to be one of
them.** It is named padding and not a spare field; see the section below.

## Padding is named, because it reaches a file

A trivially copyable component is serialised as its object representation,
padding included, and padding is never initialised. `RigidBody::Reserved`,
`Collider::Reserved`, `Visual::Reserved` and `ActiveCamera::Reserved` exist for
that reason and are not spare fields to repurpose. Adding a member that
reintroduces a hole makes two runs of one scene produce different bytes, and
`just determinism` reports it a long way from here.

Anything holding a `core::Name` is registered with an explicit writer that
writes the name as **text**. Registering one of these with the plain
`Components::Register<T>(name)` overload would write the name's process-local
id — a file that loads, and is wrong.

## Registration order is a format

`RegisterSceneComponents` registers in the order `v02v03v04.md` §3.2 lists, not
alphabetically. Component ids are a dense counter, an archetype is a sorted list
of them, and archetypes iterate in id order — so reordering those lines changes
the order rows are visited engine-wide and a floating-point sum over them can
come out differently.

**Add at the end.** Never insert.

## Both programs are on this module, and there is no second definition left

`mono.client/include/client/Demo.hpp` and
`mono.server/include/server/Simulation.hpp` each carried their own components
until v0.4. They do not any more: the client's `Transform`,
`PreviousTransform`, `Visual`, `SceneBounds` and `ActiveCamera` and the server's
`Position`, `Velocity` and `WorldBounds` are all gone, and both programs now
register these types under these names. `mono.client/include/client/Replicated.hpp`
went the same way — it existed only to declare the server's components a second
time so a snapshot could resolve, and a shared set is what made it unnecessary.

That is what makes a snapshot cross without a translation layer, so **a program
reintroducing a component of its own that means one of these is reopening the
debt**. `Spin`, `Orbit` and `DrawList` in the client and `Chatter` and `Heard`
in the server are demo and placeholder types rather than duplicates, and they
stay where they are.

`render` converts a `DrawInstance` to whatever the GPU wants and calls
`ResolveCamera` for its view-projection, so `render::Instance` and
`render::Camera` are gone too. **`ResolveCamera` is the only place the engine
decides what a camera's matrices are.** A second one reappearing in a
presentation module is the change to refuse.

## Not here yet, so do not add half of one

- **No property declarations.** `Classes::Property` is what the v0.5 bindings
  manifest is generated from, and the useful properties do not map to a field:
  Roblox's `Size` is a full extent and `Bounds::HalfExtent` is half of one, which
  a member pointer cannot express. Declaring the ones that *do* map and leaving
  the rest is worse than declaring none — it reads as a complete list.
- **No systems.** `IntegrateMotion`, `SyncBroadphase` and the rest of
  `v02v03v04.md` §3.5 belong to `physics` at L8, which reads these components
  and is not read by them. This module holds the data and one resolver.
  `ResolveActiveCamera` is here because a camera's matrices are a function of a
  camera and nothing else.
- **No world AABB, and no `Ray` anything.** `core::AABB`, `core::Ray` and
  `core::RayHit` exist now — they landed with `spatial`, the module that gave
  them a consumer — and none of them belongs in a component here. A world AABB
  is a function of `Transform` and `Bounds`, and storing a derived fact is how
  it goes stale. `WorldBounds` is not that: it is authored, it is one per world,
  and nothing derives it from anything.
- **No `Camera` class in the class table.** Only `Part`. A camera is an instance
  in the v0.6 model and will want one; registering it before anything creates one
  would be guessing at its component set.
- **No predicted-entity index range.** That changes how the entity directory is
  laid out and belongs in `ecs`; `Store::SetAdoptOnly` is what guards the hole
  meanwhile.
