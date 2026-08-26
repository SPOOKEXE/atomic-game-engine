# scene - module invariants

L7, `shared` tier. What a thing in a world *is*: the Basic Components, the
`Part` class, the two resources they index, and the draw payload a world
publishes. `v02v03v04.md` §3.1–3.3 is the design; this file is what a reviewer
should refuse.

## Nothing presentation-tier gets in, and no device data at all

This is the rule the whole module exists to make possible, and it is the one a
reasonable-looking change breaks first.

A `shared` module may not link a `client` one, so `mono_check_all_tiers` catches
the *edge* - it will not catch the shape. A `glm::mat4` model matrix in
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

All three of these are `shared`, so **the build cannot catch any of them** - by
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
`uint32_t`. They are the same width and mean opposite things - the layers a
collider is on, and the layers it is tested against - so passing them to a query
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

## `collision` is here for `CollisionShapes` and for nothing else

A `Collider` names its geometry with a `core::Name` and `scene::CollisionShapes`
is where that name resolves to a hull or a triangle soup. That table is the whole
of the edge to L5.

**Why the table is at L7 rather than in `physics`.** `physics` is L8 and could
hold it - and then anything that merely wanted to *fill* it would have to reach
L8 to do so, and the thing that fills it is whoever loaded the content. `scene`
already owns the components that name shapes and is below every host.

**Why the geometry could not live in `assets` where the meshes are.**
`assets::MeshData` is L8, the same height as `physics`, and both modules'
`AGENTS.md` refuse an edge between them - two modules at one height have to stay
disjoint. So a mesh collider could not be `physics` reaching sideways for a mesh;
the geometry had to enter the stack from below. `collision/AGENTS.md` carries the
rest of that argument.

Refuse anything that widens it:

- **Reading a file here.** A hull arrives as points and a mesh as vertices and
  indices. Turning a `.glb` into either is `bake`'s job at L9, and an edge from
  here to it would invert the stack.
- **Building a hull here.** `collision::BuildConvexHull` is a bake-time
  algorithm with a point cap and a determinism requirement; calling it from a
  property setter would put quickhull on the frame a script assigned a name.
- **A `collision::ConvexHull` on a component.** `Collider` carries the *name*,
  for rule 4's reason: a name survives a save file, a wire format and a rename,
  and a hull is bytes that would then exist once per part that used it.

**Who fills it is `game`, at L10, and there is one of them.** `game::
AddCollisionShapes` is the only conversion from a mesh to the two shapes; the
client, the studio and a headless server all reach it, so all three resolve a
name to the same geometry. A host that built a hull of its own would be a host
that eventually disagrees with the others about where a body stops.

**`CollisionShapes` is registered with a writer that writes nothing, and that is
deliberate.** A hull is derived from content the receiving side already has, so
putting it on the wire is sending a conclusion instead of its input - and the
conclusion is the half an attacker gets to choose. A shape whose bound disagrees
with its points is a collider that stops things it is not touching.
`assets::MeshData` refuses to store its own bound for the same reason.

It has an empty writer rather than no writer because `Store::Save` refuses a
resource with no serialisation at all, and the studio snapshots a universe every
time Play is pressed - `client::DrawList` is registered the same way for the same
reason. The reader clears, so a restored world is handed its shapes again by
whichever host restored it rather than inheriting a table nothing owns.

## A fixture is found by class and never by name

`WorkspaceOf` was `FindFirstRoot("Workspace")` from v0.7 to v0.17, and so was
every other fixture lookup including the one `InstallServices` uses to decide
what a world is missing. A script renaming the workspace therefore made
`WorkspaceOf` answer nothing **and** made the next `InstallServices` mint a
second `Workspace` beside the one holding the scene - two roots, one of which has
everything in it and neither of which a script can be sure it reached.

`ServiceOf(store, klass)` and `ServiceUnder(store, parent, klass)` are the whole
answer: a class is registered once and a rename cannot touch it.
`engine.scene.services` renames the `Workspace`, the `Players` and the
`StarterPlayer` and requires all three still to resolve and no duplicate to
appear.

**A new fixture lookup that spells a name is the change to refuse.** The one
exception is `gui`, which may not link this module and finds `StarterGui` and
`PlayerGui` by the names `Layout` already walks ancestors comparing - that is a
cross-module contract stated in `gui/Layout.hpp` and pinned by
`examples/tests/Scene.cpp`, not a lookup somebody forgot to convert.

## A join is once and a spawn is every time, and the containers say which

Four `Starter*` services and four per-player containers, and the whole design is
in *when* each pair is copied. `Services.hpp` and `Characters.hpp` carry the two
ordered lists; what belongs here is what a reviewer should refuse.

- **`StarterPlayerScripts` → `PlayerScripts` and `StarterPack` → `StarterGear`
  are `AddPlayer`'s, once.** Copying either on a spawn would double a player's
  client scripts per death and double their gear.
- **`StarterCharacterScripts` → the character and `StarterGear` → `Backpack` are
  `LoadCharacter`'s, every time.** The first is a script about a body and the
  body is new; the second is what makes gear a game granted at run time survive
  dying - add to `StarterGear`, never to `Backpack`.
- **`StarterGui` → `PlayerGui` is `gui::ResetPlayerGui`'s and cannot be here.**
  This module may not link `gui`. Whoever spawns calls both, which is why
  `UpdateRespawns` hands back who it spawned instead of finishing the job.
- **The per-spawn half is the authority's and must not move into
  `SetPlayerCharacter`.** That function is also what a *client* runs when a
  `PlayerCharacter` arrives over the wire, and a client clearing its own
  `Backpack` would be fighting the replication that fills it.

**Everything under a `Player` is that player's**, which `PlayerOwning` already
said and the three new containers inherit for free. A container added beside them
needs no predicate change and needs the scoping test extended in both directions
- `server.replication` asserts a client holds four of its own and none of
anybody else's.

## Equipping is a reparent, and the tree is the only record of it

A `Tool` in a `Player.Backpack` is stowed and a `Tool` that is a direct child of
a character `Model` is equipped. `Tools.hpp` carries the whole design; what
belongs here is what a reviewer should refuse.

- **An `Equipped` flag, or a field naming the equipped tool.** Both are a second
  copy of what the hierarchy already says, and both are the copy that goes stale
  the first time a script reparents one - rule 2. `EquippedTool` is a walk of a
  model's few children and `HolderOf` is one `ParentOf`.
- **A second answer to "may this machine move a tool".** `EquipTool` and
  `UnequipTool` refuse on `ecs::Store::AdoptOnly` and `ecs::Store::SetProperty`
  refuses a `.Parent` write in a replica. That is `TakeDamage`'s pair - one rule
  with a C++ door and a script door - and a flag on the class or a check in a
  host would be a third statement covering one caller.
- **An `Attachment` carrying the handle.** `ResolveAttachments` runs in
  `PreRender` and *resolves a frame* rather than moving a part;
  `Attachments.hpp` says a caller wanting a weld is asking for what that pass
  does not promise. A `CharacterLimb` is what the rig is already made of, so a
  held handle is one more part in the same formation - and it inherits the
  replication `D00115` already bought, because `scene.CharacterLimb` is
  `replication`'s suppressor for `scene.Transform`.
- **`UpdateToolGrips` reparenting or destroying anything.** It runs on a replica,
  through `PoseCharacters`, on every host that draws a character. Everything it
  writes is a function of where a tool already is, which is what makes that safe;
  the moment it moves something it is a replica fighting its authority.
- **An equip that rewrites `Anchored`, `CanCollide`, `CollisionGroup` or a
  physical property.** Every one of them is a declared property an author set.
  Taking `Motion` away is the exception and is not one of them: it is the
  archetype move `physics` makes for a sleeping body, and it is put back only
  where a `RigidBody` says there is a body to move.
- **Death or departure growing a special case.** A corpse keeps what it was
  holding and `LoadCharacter` destroys the body with the tool in it, which is the
  two-container rule stated one section up: only `StarterGear` survives a death.
  A departing player's `Backpack` goes with the `Player` and their held tool goes
  with the character `ReclaimOrphanedCharacters` collects. Neither needed a line.

**`Humanoid:EquipTool` is absent and it is not a gap.** A class table here
carries properties and no methods - `Sound.Playing` is a property for exactly
that reason - and a Roblox script equips by writing `tool.Parent = character`
anyway, which is a declared property and works. The day classes carry methods,
that method sets a parent and nothing else changes.

## A spawn is found by class *and* by name, and that is the one exception

`scene::FindSpawn` looked for a child called `SpawnLocation` from v0.14 to
v0.15, which `Characters.hpp` recorded as a deliberate stop: Roblox's class
carries teams and a forcefield, none of that existed, and a class with a
footnote is worse than a name. `Teams.hpp` is what lifted it - the class is
real, it derives from `Part`, and its three fields all have a reader.

**The name still resolves, and that is the bridge rather than a leftover.**
Every scene in `mono.engine/examples` builds its pad with
`block("SpawnLocation", ...)`, so a plain `Part` wearing the name is read as an
enabled, neutral spawn - the defaults the class would have given it. Deleting
that half drops every one of those worlds at the origin, silently.

**This is not the lookup the fixture section refuses.** That rule is about
*services*: a fixture found by name is one a script can rename out of existence,
and `InstallServices` mints a second beside it. A spawn pad is content an author
names on purpose, and `IsSpawn` is one function holding both spellings rather
than a lookup somebody forgot to convert.

Three more things a reviewer should hold:

- **The first pad in tree order, never a random one.** Roblox draws at random
  among everything a player may use; a respawn drawn from a random number is a
  recording that does not replay, which is `UpdateRespawns`' rule about its
  deadline arriving one function along.
- **A player's own colour beats a neutral pad, and another team's is never
  used.** That preference is the whole difference between a team and a coloured
  label - `docs/DEFERRED.md` D00119 refused `Player.Team` for eight versions on
  exactly that test.
- **Colours are compared within a tolerance and never with `==`.**
  Roblox matches `BrickColor`s, which are enumerated; a `Color3` reaches the
  comparison through a property setter, a snapshot and a Rojo JSON file that
  writes floats as text. `SameTeamColour` is the single statement of it.

## `MakePart` takes a class, and it is still the only constructor

`PartDesc::Class` exists so that `SpawnLocation` - a `Part` plus one component -
goes through the same door every other part does. The alternative was a second
builder, which is the duplicate this file refuses two sections down, and the two
disagree the first time `BasePart` gains a member.

**Anything that is not a `BasePart` is refused rather than half-built.**
`MakePart` writes `Bounds`, `Visual`, `Collider` and `Surface` unconditionally,
so minting a `Model` through it would produce a model with a part's components
bolted on and no class saying so.

## Death is `Health` reaching zero, and the delay is a tick number

`Humanoid` carries a `Health` and a `MaxHealth` from v0.15, and that is what
"dead" means. `UpdateRespawns` measured `Player.RespawnTime` from the tick a
player was first seen with *no model* until then - Roblox's delay hung off the
wrong event - so the branch now asks whether the body has health rather than
whether it exists. A destroyed model is still the second trigger and has to be:
a teleport, a script tidying up and a host dropping a client all remove a
character without setting anything to zero first.

Four things a reviewer should refuse:

- **A second answer to "may this machine write health".**
  `ecs::Store::SetProperty` refuses every property write in a replica and
  `scene::TakeDamage` refuses on `Store::AdoptOnly`. That is one rule with two
  doors - the script door and the C++ door - and a `Writable = false` on the
  descriptor or a flag beside it would be a third statement of it that only
  covers one caller. It would also stop the *server* script that wants to kill
  somebody, which is the ordinary way a game does it.
- **A damage door beside `TakeDamage`.** It is where the replica refusal, the
  clamp at zero and the once-only death all live, so a second subtracting path
  is three rules re-decided.
- **A `Health <= 0` written out rather than `IsDead`.** Spelled `!(Health > 0)`
  so a NaN is dead; written the obvious way round a NaN is immortal.
- **Anything that destroys the body at the moment of death.** That is the old
  rule wearing a new name. The corpse is `LoadCharacter`'s to remove when the
  deadline arrives, which it has done since v0.14.

Two things about the delay are load-bearing:

- **The deadline is `ceil(seconds / delta)` against the fixed tick delta**, which
  is `DebrisQueue`'s rule. A float accumulated per tick drifts and
  `just replay-check` would fail a long way from here.
- **`RemoveCharacter` stops, then destroys, then releases**, and that order is
  not tidiness. `Player.CharacterRemoving` rides
  `Store::OnDescendantRemoving` and filters on `Character::Owner`, so releasing
  first fired the signal for nobody on every respawn - and the queued half then
  reports it with a handle nothing can read. `StopCharacter` is called directly
  so the release no longer has to happen first to do its job.

## `Part` is a class, not a component

There is no `struct Part`, and adding one would be adding a second answer to
"what is a part". The class table is the answer: a name, a parent, and a
component set, registered once, so `Instance.new("Part")` in v0.6 and `MakePart`
in C++ resolve through the same rows.

**`MakePart` is the only constructor.** A loader, a test or a demo that assembles
the five components itself is a second definition, and the two disagree the
first time one gains a component. If `MakePart` cannot express what a caller
needs, extend `PartDesc` - that is what it is for.

**The adopt-only check in `MakePart` is not redundant.** `Store::Create`
refuses to mint in a replica; `Store::CreateInstance` does not, and goes
straight to the directory. So `MakePart` checks `Store::AdoptOnly()` itself, and
removing that line on the assumption that the storage already covers it reopens
the exact collision `SetAdoptOnly` exists to prevent - same index, same
generation, and `Store::Apply` right to merge the two. The proper fix is in
`ecs`, and until it lands this line is what keeps the one minting path this
module owns closed.

**Whether the world may move a part is presence, never a flag.** The component
is `scene::Simulated` and a part that carries it is one the solver may move; a
static part carries neither it nor `Motion`, so it lands in a different archetype
and no dynamic query ever visits it. A reviewer should refuse a change that adds
a `Static` body to static parts "for uniformity": uniformity here costs a row
visit per static part per tick, forever, and static geometry is most of a world.

**Absence is static, and the polarity is deliberate.** Until v0.18 the tag was
`Anchored` and marked the immovable ones, which meant `BasePart`'s class set had
to list a component to reach the safe default and every placed thing in a scene
carried one. The majority case now stores nothing. Refuse a change that puts
`Simulated` back in a class set: that makes every part dynamic on creation, and
the failure is a scene that looks right until something leans on a wall.

**`RigidBody` is on a part either way**, and has been since v0.15. It is what an
author typed - a mass and two drags - not the world's decision about whether it
may be pushed. Refuse a change that removes it when a part is anchored.

**`Simulated` is not `Motion`, and a sleeping body is why.** `physics::Publish`
takes `Motion` away when a body falls asleep and leaves the tag on, so
`!Has<Motion>` means "static *or* asleep" and only the tag tells them apart. Both
take an infinite mass in the solve; only one of them can be woken. Refuse a
change that tests `Motion` where the question is whether the world may move
something - `physics::FactsFor` is the one that has to get this right, and a
settled crate becoming a permanent wall is the failure.

Roblox's `Anchored` is the negation of the tag, and `scene::AnchoredProperty` is
the only place in the engine that inversion is spelled. Refuse a second one.

## The splits are load-bearing, and each has a reason

Refuse a change that merges any of these without a measurement that says the
merge is free.

- **`Motion` apart from `RigidBody`.** Anything with a `Transform` and a
  `Motion` moves and needs no mass - a platform, a projectile, a demo cube. So
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
  cameras - spectator, cutscene, security monitor - and exactly one live one.
  Collapsing the resource into "the entity with the camera" turns a lookup into
  a search, and collapsing the component into the resource makes the second
  camera impossible.

## A content hash is second-best, and this module no longer carries one

Change detection is `ecs::ChangeChannel`. A content hash exists for one gap: a
write through a raw column pointer in the batch path advances no per-row stamp,
because there is no per-row write to hang one on.

It costs a pass over the data every tick whether anything moved or not, so it is
the fallback rather than the answer. `scene::QuickHash` was this module's
version of it from v0.4 to v0.19 and was **deleted at v0.19 having never
acquired a reader or a writer** - it was a column in every archetype and a row in
every `.agame` schema, describing a comparison nothing ever made. The two places
that do need row granularity over batch-written data fold their own hash where
they read it: `gui::Compiled` and `studio::HierarchyView`.

A reviewer handed a new one here should be able to name the batch writer it is
for, and the consumer that compares it. Neither existed for the one that was
removed.

## The wire grid is beside `WorldBounds` because it is the same decision

`Wire.hpp` is what `Transform` and `Motion` look like on a replication
datagram: a position as three fixed-point axes, a rotation as smallest-three,
twenty-eight bytes as ten. **How coarse that grid is depends entirely on how far
the world reaches** - two millimetres over 128 metres is a different figure over
four kilometres - so the extent it covers is stated in the same terms as
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
not inside the encoder - which sees one component and not a world.
`WireCoversWorld` is the check and `mono.server`'s placeholder world holds it as
a `static_assert`.

## There is no transform hierarchy, and there is not going to be one here

`Transform` is world space. Parenting is organisational, exactly as Roblox's is:
it moves nothing and re-resolves nothing. A `LocalTransform`, a `WorldTransform`,
a dirty flag or a propagation system in this module is the change to refuse - the
absence of that pass is what buys physics a `Transform` read with no resolve step
and a tree that costs four handles per node.

## There is no sleeping flag on `RigidBody`, and there must not be one again

`v02v03v04.md`'s allocation table puts a sleeping body in a *different
archetype* so the query never visits it. A flag on the row is the opposite of
that: it is only readable by making the visit the archetype move exists to
avoid, and it is the same state the solver already has to keep - two answers to
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
`Collider::Reserved`, `Visual::Reserved`, `ActiveCamera::Reserved` and
`InputState::Reserved` exist for that reason and are not spare fields to
repurpose. Adding a member that reintroduces a hole makes two runs of one scene
produce different bytes, and `just determinism` reports it a long way from here.

**`InputState` is the one that has been added to most, and its rule is to take
the bytes out of `Reserved` rather than off the end.** `Pressed`, then
`PreviousFocused`, then `MouseIconEnabled` and the two source fields all came out
of it; a field appended instead would grow the object, move the layout
`Column::Write` sends and do it silently. `SIZE_IS_PINNED` in `Input.cpp` is what
turns that from a habit into a compile error, and the number in it is what the
members add up to rather than a target - changing it is a decision somebody has
to make there.

Anything holding a `core::Name` is registered with an explicit writer that
writes the name as **text**. Registering one of these with the plain
`Components::Register<T>(name)` overload would write the name's process-local
id - a file that loads, and is wrong.

## Registration order is a format

`RegisterSceneComponents` registers in the order `v02v03v04.md` §3.2 lists, not
alphabetically. Component ids are a dense counter, an archetype is a sorted list
of them, and archetypes iterate in id order - so reordering those lines changes
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
went the same way - it existed only to declare the server's components a second
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

## The property surface is declared, and a property is a conversion

`Part.cpp` declares what a script can name: `CFrame`, `Position`, `Orientation`,
`PivotOffset`, `Size`, `Color`, `Visible`, `CanCollide`, `Anchored`, and
`Name`/`Parent` on `Instance`. `MeshId` and `TextureID` are `MeshPart`'s.

**A class declares what its *kind* of thing has, and v0.10 corrected `BasePart`
against that.** `BasePart` is what `Part`, `MeshPart` and a future
`UnionOperation` share; geometry loaded from a file is not shared by any of them,
so `Mesh` and `ColorMap` moved to `MeshPart` under the names that class shows.
Offering a mesh reference on a `Part` was worse than not having it - an author
sets it, the six-sided box does not change, and nothing says the class was wrong.

**The storage did not move and must not.** `Visual` and `SurfaceAppearance` stay
on `BasePart`, because `client::CollectInstances` is a batched parallel walk over
a fixed signature and an optional column is what that shape cannot express. A
dense column of mostly-invalid names is sixteen bytes an entity and no branches;
a per-class component is a join per row per frame. What moved is the vocabulary.

**A pivot is storage and `GetPivot` is not.** `Pivot::Offset` is a column on
every `PVInstance` - a placement says where a thing's *centre* is and almost
nothing is placed by its centre. `PivotOf` and `PivotTo` are free functions over
it, and `PivotTo` is `target * Offset⁻¹` rather than a plain move; getting that
inverse the wrong way round is what "PivotTo ignores the offset" bugs are.

**`Material` was on that list until v0.10 and is not a property at all now.** It
was an `Enum` over seventeen names that no renderer sampled differently - a
property that looked like it worked, on the most obvious control in the panel.
A material is content: `Instance.new("Material")` under a part names an `.amat`,
and `Materials.hpp` carries the whole design, including why the resolve pass
writes into `SurfaceAppearance::ColourMap` rather than anywhere new. Do not
reintroduce a material *word* beside it - two ways to say one thing is the debt
`AGENTS.md` calls the most expensive kind, and this one has already been paid
off once.

This section used to forbid exactly that, and the reason it gave was right:
*"the useful properties do not map to a field - Roblox's `Size` is a full extent
and `Bounds::HalfExtent` is half of one, which a member pointer cannot express.
Declaring the ones that do map and leaving the rest is worse than declaring
none, because it reads as a complete list."*

**What changed is the primitive, not the rule.** A `PropertyDescriptor` is a
getter and a setter now rather than a component and an offset, so a doubled
half-extent, a translation with its rotation kept, and a flag that is really an
archetype move are all expressible. The list is complete because it can be, and
the invariant was satisfied rather than excepted.

Two things it still forbids, and both for the original reason:

- **Do not reshape a component to match a property.** `Bounds` keeps a
  half-extent because that is the form every containment and overlap test wants.
  The shim converts; the storage does not accommodate.
- **Do not declare a property for scripts alone.** Every consumer of the class
  table gets what is declared here - the manifest, the generated declarations,
  both VMs - so a property that exists for one caller is a fact with one reader
  and several places to go stale.

`Transparency` is the one entry with nothing to project onto: `Visual` has no
such field, and it becomes a renderer feature at v0.6 rather than a float
nothing draws. `CollisionGroup` waits on a name-to-layer registry that does not
exist, because inventing one here would be this module deciding a physics
policy.

## A `Sound` has no place, and the omission is the whole design

`Sound` derives from `Instance` and **not** from `PVInstance`. Where it is heard
from is its parent's: under a service it is heard everywhere at one level, and
inside something with a `Transform` it is heard from that thing and falls off
with distance. Roblox's rule, kept for this file's standing reason - a tree that
differs from the one scripts expect is a migration nobody asked for - and it is
also the right rule here.

**Do not give it a position.** A `Sound` with coordinates of its own would be a
second opinion about where a thing is, which is rule 2 with a speaker attached,
and it would turn "attach a sound to a thing" from `Parent = thing` into a field
somebody has to keep in step with a hierarchy that already says it.

**Nothing in this module plays anything, and nothing here may.** `scene` is
`shared` and a server has no mixer - it decides what is audible and replicates
that, and the sound is produced where somebody is listening. The client walks
these rows and drives `Engine::audio`; that is the same split `Visual::Mesh`
already has against the renderer, and it is why this module still does not
depend on `audio`.

**`Playing` is a property rather than a `Play()` method**, and that is a fact
about the script binding rather than about audio: methods live on one metatable
shared by every instance, so a `Play` there would be a method on every `Part` in
the world. The day classes can carry their own methods, `Play()` sets this
property and nothing else changes.

**`Volume` clamps at 10 rather than at 1**, which is Roblox's ceiling and the
honest one: the graph works in floats precisely so a value over full scale
passes through harmlessly and is clamped once at the device. Refusing above 1
would make a quietly authored sound impossible to bring up.

`Sound` holds a `core::Name`, so it is registered with a **hand-written wire
pair** and every field is written the day it is added - including `Playing`, so
that a level whose ambience is a looping sound comes back sounding rather than
mute with nothing in the file saying why.

## Not here yet, so do not add half of one

- **No simulation systems.** `IntegrateMotion`, `SyncBroadphase` and the rest
  of `v02v03v04.md` §3.5 belong to `physics` at L8, which reads these components
  and is not read by them.

  Two systems *are* registered here, and the line between them and the ones
  above is worth stating rather than leaving to be rediscovered:
  `RegisterGravitySystem` (`Gravity.hpp:62`) and `RegisterOwnershipSystem`
  (`Ownership.hpp:101`). Both compute a property of a row from that row and its
  ancestors and from nothing else - no broadphase, no pairs, no solver, no other
  module's components - which is the same argument `ResolveAttachments` makes
  about an attachment's world frame being a function of its parent's placement.
  A system that has to see a second module's data is the kind that does not
  belong here.

  This bullet read "No systems" until v0.19, by which time there were two.
- **No world AABB, and no `Ray` anything.** `core::AABB`, `core::Ray` and
  `core::RayHit` exist now - they landed with `spatial`, the module that gave
  them a consumer - and none of them belongs in a component here. A world AABB
  is a function of `Transform` and `Bounds`, and storing a derived fact is how
  it goes stale. `WorldBounds` is not that: it is authored, it is one per world,
  and nothing derives it from anything.
- **No `Camera` class in the class table.** Only `Part`. A camera is an instance
  in the v0.6 model and will want one; registering it before anything creates one
  would be guessing at its component set.
- **No predicted-entity index range.** That changes how the entity directory is
  laid out and belongs in `ecs`; `Store::SetAdoptOnly` is what guards the hole
  meanwhile.

## `SurfaceAppearance` and `Tags` are on `BasePart`, and that is a paid-for choice

Both are in `BasePart`'s class set rather than `MeshPart`'s, which puts sixteen
bytes on every part in the world including four thousand plain cubes. The
alternative is an optional component, and it does not work here:
`client::CollectInstances` is a batched parallel walk over a fixed signature -
`EachBatchParallel` is handed columns and deliberately no entity - so a
component only some rows had could not be read at all without walking the world
a second time and joining by entity.

A dense column of mostly-invalid names is sixteen bytes and no branches. Moving
either of them down to `MeshPart` would mean rewriting the draw-list pass, and
the reviewer's question is whether that rewrite was actually done rather than
whether the move looks tidier.

## A tag's bit is its index, so the table is never sorted and a name never leaves

`TagTable::Names` is in registration order because **the index is the bit**.
Sorting it renumbers every mask already stored on a row - the same rule
`SurfaceTable::Rows` carries - and `RemoveTag` deliberately leaves the name in
the table for the same reason: freeing a bit would let the next registration
reuse it while another row's mask still had it set.

**The thirty-third tag is refused rather than aliased.** An alias is one tag's
objects appearing in another tag's pass, which is invisible until somebody
notices the wrong thing reflected in a mirror. If the ceiling ever needs
raising, raise the mask's width; do not make registration wrap.

## The tag names travel with the masks or neither is meaningful

`Tags` is a bare integer whose bits mean whatever the `TagTable` resource says.
A world restored with the masks and without the table has every tagged object in
a group with no name, and a filter by name matches nothing - silently. Both are
registered in `RegisterSceneComponents` and the table has an explicit writer.

## A tag filter is authored by name and compared as a bit

`SurfaceCamera::TagFilter` is a mask, and `TagFilterProperty` is what turns a
name into one - once, where the name is written. The alternative is a name on
the component and a lookup per instance per pass, which is a string compare in
the draw loop.

**A filter names several tags, comma-separated.** A mask holds thirty-two and a
property holds one value, so the list lives in the string rather than in a new
`ecs::PropertyType` that exactly one property would use. The setter builds the
whole mask in a local and assigns once - a loop writing straight into the
component leaves a half-built filter behind when the table fills part way
through, and a redirected pass drawing *some* of its group is harder to notice
than one drawing none of it.

**The property's type is `PropertyType::Name` and not `String`.** The two
marshal different payloads - a `core::Name` and an owning `std::string` - and
declaring the wrong one compiles, passes any test that writes raw bytes through
`SetProperty`, and fails at the first script that assigns to it. That is how it
was found. A test on a computed property should assert the declared type, not
only the round-trip.

## A mesh's size is a fact about the mesh, so it is keyed by name and never cached on a row

`MeshCatalogue` holds triangles per mesh name; `MeshPart::TrianglesCount` reads
`Visual::Mesh` and looks the answer up. Refuse the two obvious-looking
alternatives:

- **A count on the component.** A thousand parts naming one mesh would hold a
  thousand copies of one number, and repointing `MeshId` would leave every one
  of them reporting the old mesh. This file already refuses the same shape for a
  cached world AABB, and for the same reason: a derived fact stored beside the
  thing it was derived from goes stale silently.
- **Reaching into `render::MeshTable` for it.** That is the tier edge the first
  section of this file exists to refuse, and it would also make the property
  answer nothing on a server. A triangle count is `Indices.size() / 3` of a file
  on disk - a headless server could produce it and mean it - which is exactly
  why it is allowed to live here at all, and why a GPU buffer offset is not.

**Zero means "this world has not been told".** Not "empty": `assets::Mesh::Read`
refuses a mesh with no triangles, so the two cannot be confused. It is the
honest answer on a server, on a client before the content pump has run, and for
a `MeshId` naming something no publisher published - and that last case is the
same condition that draws the fallback cube, so a part reading zero and drawing
a cube is one fact reported twice rather than two faults.

**`TrianglesOf` never creates the resource and `MeshesOf` may.** The property
getter calls the first. A getter that acquired a resource would be a structural
write from inside a read, on every mesh part in the scene, during `PreRender`.

**The catalogue is not saved.** Its registration writes nothing and reads back
empty, as `RenderedSignature`'s does. A file carrying last run's counts would
disagree with the content sitting beside it, and a wrong number is worse than a
zero that says it does not know.

## What a mirror does to a camera is a function, and it must stay one

`ReflectCamera` takes a `SurfacePane` and a viewer and returns where that pane's
camera stands. **It does not take an `ecs::Store`, and a reviewer should refuse
the change that gives it one.** A mirror seen inside another mirror is looked at
from *that* mirror's camera rather than from the eye, so the rule has to compose
- and it can only compose if it is a function. While it lived inside
`AimSurfaceCameras`' walk over `ActiveCamera` there was exactly one viewer it
could ever answer for, which is what drew the inner panes of
`examples/MirrorDepth.luau` as flat tint.

Three shapes to refuse, and the reason is the same each time:

- **A second derivation of the reflection anywhere.** `AimSurfaceCameras` calls
  this for its own mirrors and `engine.scene.surfacecameras` asserts the
  `Transform` and `SurfaceLens` it writes are bit-for-bit what the function
  returns. That is `SeamMapping`'s argument applied to a reflection: a second
  statement of one rule is a second chance to disagree about a sign.
- **A pane gathered by both halves.** `GatherSurfacePanes` skips a linked portal
  and `GatherPortalSeams` takes it; an unlinked one is a mirror in both, because
  a hole leading nowhere is a wall. `LinkedPortalOf` is the single test, and two
  passes disagreeing about which a pane is means one drawn twice or not at all.
- **A viewer's frustum passed as anything but four corners or none.**
  `FrustumCorners`' two overloads hand back directions one unit deep - a
  perspective camera has a field of view and a reflected one has an already
  fitted off-axis lens, and a level of recursion that fell back to "no corners"
  drops the clamp exactly where the pane is nearest.

## An unarrived mesh draws nothing, and no mesh at all draws a cube

`KeepLoaded` is that rule, and the two cases it separates are why it is a
function rather than an `if` in the renderer:

- **No mesh named** - an ordinary `Part` - is kept. The renderer's default cube
  is what a part *is*, not a stand-in for something missing.
- **A mesh named and not resident** - a `MeshPart` whose geometry has not been
  uploaded - is dropped. `render::MeshTable::Resolve` answers the default for a
  name it does not hold, so without this a scene of mesh parts comes up as a
  field of cubes that turn into models one at a time as content lands.

The second is the point: an empty space reads as *still loading* and a wrong
cube reads as *the asset is broken*, and only one of those is true. It is the
same fact `TrianglesOf` reports as zero - a `Visual::Mesh` this world has not
been told about - which is why the section above says a part reading zero and
drawing a cube is one fact reported twice.

**Here rather than in `render`, for `OrderScene`'s reason.** A renderer is the
one module a test cannot exercise, so a rule deciding what reaches a draw call
is the last place it should live. A template rather than a `std::function`, so
the residency test stays a direct call in the hottest pass of the frame; the
caller owns the output buffer and it is cleared rather than rebuilt, like every
other buffer on that path.


## A latch is owed for every device, not for the one somebody needed

`InputState` carries `Pressed` for keys and `PressedButtons` for mouse buttons,
and both exist for the same reason: frames outnumber ticks, so an edge that
began and ended between two ticks happened on a frame no tick ever looked at.
`LatchPresses` records both and a *writer* owes that call once per frame;
`ConsumeTaps` clears both and a *reader* owes it once per tick.

The button half was missing for five versions, and nothing noticed because
nothing in the engine acted on a click. It arrived the moment `ReadAimIntent`
did. **A third device gets its own latch in the same call** - two calls would be
two things a writer has to remember, and one of them is forgotten the first time
somebody adds a second input path, which is exactly how `PlayLink::PendingJump`
and the client's own private latch came to exist side by side.

**A new field comes out of `Reserved` or it is a save-format change.** A member
appended to the end grows the object and rewrites the layout `Column::Write`
sends, silently. `SIZE_IS_PINNED` in `Input.cpp` is what refuses it.

## `ReadMoveIntent` and `ReadAimIntent` are read by two hosts, so they live here

A client sends *intent* and never result - `game/Play.hpp` carries the whole
argument - and the studio applies the same intent from a `PlayLink` with no
socket in the middle. Both functions are `const` and write nothing at all, so
the arithmetic that turns W into "away from the camera", and a camera into a
ray, is reachable without the thing that acts on it. A copy of either in
`mono.client` is a copy that drifts, and drifts first in the editor.

**An aim is the live camera's `Transform` and not the controller's angles.** The
two agree after `PlaceCamera` has run and disagree before it, and a `Scriptable`
camera has no angles at all - so deriving the ray from yaw and pitch would aim a
cutscene's shot wherever the player last left the mouse.

## A fixture is protected by `InstallServices` and by nothing else

`ServiceComponent::Fixture` says an author may not delete or reparent a service,
and until v0.15 nothing read it - a script could `Destroy()` `Lighting` and the
editor could delete it with the Delete key.

`ecs::Store::Protect` is the seam and **`InstallServices` is its only filler**.
That is what keeps it from being configuration somebody forgets: it is the one
door a service arrives through, it is idempotent, and it runs on a world loaded
from a file as well as on a fresh one - so a world that has services has this
by construction. `Store::SetAdoptOnly` is the shape *not* to copy; it is set by
exactly one caller and nothing checks that it was.

**The store holds identity and nothing else.** `Fixture` is `scene` at L7 and
the store is L2, so a mirrored flag on an `ecs` row would be the second copy of
one fact rule 2 refuses. `Protect` takes an entity; what made it protected stays
here.

**The four authored doors are the whole enforcement**: a script's `Destroy()`,
the editor's Delete key, the explorer's drag and the `.Parent` setter - the last
of which is one lambda serving both VMs and the properties panel, so enforcing
the other three alone would ship a rule that holds in Luau and not in the panel.
Everything else keeps `DestroyInstance`/`SetParent`, because `PlayLink`
destroying a player, `Debris` draining its queue and `RojoSync` rebuilding a
subtree are the engine moving its own furniture and must not be refused.

## The world owns the surface depth, and the frame measures what the world leaves out

`workspace.SurfaceBounces` is how deep a scene resolves surface-in-surface. It is a computed property
over a `scene::SurfaceBounces` resource - `workspace.CurrentCamera`'s arrangement - so the resource is
the only storage and there is nothing for a second copy to drift from.

**Zero means measure it, and that is the default.** Each viewport keeps what the frame it just drew
reached: the deepest level that actually rendered a pane, and whether that level still had one in view
it was not allowed to descend into. `NextSurfaceBounces` answers `resolved + (blocked ? 1 : 0)`,
clamped to `render::MAX_SURFACE_DEPTH`. A corridor of facing panes deepens itself one level a frame to
the ceiling; a room with one mirror costs one level; nobody types a number.

Three things hold it up, and two of them are only obvious once broken:

- **The depth is part of a surface's signature.** A surface whose signature has not moved is not
  redrawn - so without this the measurement says "one deeper", nothing refreshes, the deeper level is
  never drawn, and the next frame measures the same shallow answer. `MirrorCorridor.luau` sat at one
  level for ever. Found by running it.
- **The probe is written back only where the pass actually ran.** Writing on a skipped frame says
  "nothing resolved" and throws away what the drawing frames worked out.
- **"Would one more level render" and not "is a pane visible".** An edge-on pane is visible and draws
  nothing, so the looser question oscillates.

**The ceiling is the caller's and the count is the world's.** `--surface-bounces N` is an override for
one run, not a setting; a view with no pane rectangle of its own - a cross-world pane iterates rather
than descends - keeps `DEFAULT_SURFACE_BOUNCES` because there is nothing there to measure.

## `SurfaceCameras` stays in this module, and the measurement is why

`docs/ARCH_REVIEW.md` §C6 proposed lifting `src/SurfaceCameras.cpp` out as its
own L7 module. **It was measured at v0.19 and refused.** Both of the numbers the
finding rested on are wrong, and the claim underneath them - "lifts out cleanly"
- is the opposite of what the edges say. This section exists so the next person
to notice that the file is the biggest one here does not re-propose it.

**The numbers.** The file was **2,909 lines** when this was measured, not 4,108,
and it has never been 4,108: the four commits that touched it read 2,607, 2,684,
2,895 and 2,910. The 4,108 is closest to the file plus its header (4,343) and
matches nothing exactly.
The share is **22.7% of `src/`**, or 18.9% of `src/` and `include/` together, or
22.0% of the module counting tests and benchmarks. There is no reading of `just
linecount mono.engine/scene --files` that produces 36%.

**The edges run both ways, which is the part nobody checked.** Four places in
this module call into `SurfaceCameras` - three sources and, worse, a *public
header signature*. It was five when this was measured, and the one that went is
still listed, because losing it changed nothing about the conclusion:

- `include/engine/scene/ActiveCamera.hpp` declares
  `glm::mat4 SeamMatrix(const SeamTransform &)`. A `SurfaceCameras` type is in
  this module's public API, and `render::ShadowNodes` calls it through that
  header.
- `src/ActiveCamera.cpp` used to build the world's camera matrices with
  `PortalNearPlane(camera->NearPlane, NearestSeamDistance(store, ...))`, inside
  `ResolveActiveCamera`. **That function and the cached `ActiveCamera::Matrices`
  were both deleted later in v0.19**, because one cached set cannot serve three
  consumers that each project against a different target, so this edge is gone
  and the count above is four rather than five. It is left in the list because
  the argument does not turn on it: the four that remain are still four sources
  and a public header signature calling upward. `NearestSeamDistance` survives
  the deletion with nothing calling it, on purpose, and its header says why.
- `src/Controls.cpp` calls `PortalCrossing` and `ClearOfPanes` inside
  `PlaceCamera`.
- `src/Services.cpp` registers `workspace.SurfaceBounces` and
  `workspace.MaxSurfaces` over `SurfaceBouncesOf` and `SurfaceLimitOf`.
- `src/Registration.cpp` registers `scene.SurfaceBounces` and
  `scene.SurfaceLimit`.

And `SurfaceCameras.cpp` reads back down into `scene`: `Character`,
`CharacterLimb`, `Humanoid`, `SunOf`, `ActiveCamera`, `DrawInstance`,
`SurfaceLens`, `SurfaceCamera`, `Portal`, `Bounds`, `Transform`, `NormalId`.
That is a cycle, not a layer.

**So all three placements fail, and they fail differently.** Above L7: four
`scene` sources and one public header would call upward. Below L7: the
module could not name a dozen `scene` types it is written in terms of. Lateral
at L7: a lateral edge in this repository is one-directional and named in a
`lateral` array, and this is mutual, which is a different thing.

**Splitting out only the pure maths does not rescue it either.** Of 2,909 lines,
about 370 take no `ecs::Store`: `SeamMapping`, `SeamOffset`, `RectangleDistance`,
`SeamDistance`, `PortalNearPlane`, `PortalClipBias`, `SeamCarries`,
`SeamStraddled`, `CutOfSeam`, `NextSurfaceBounces`, both `FrustumCorners` and
`ReflectCamera`. The other 87% walks a store full of this module's components.
And the 370 lines are not a leaf: `SurfacePane`, `PortalSeam`, `MirrorEye` and
`SeamCut` carry `ecs::Entity` and `SurfaceLens`, so the "pure" half still names
L7. `docs/ARCH_REVIEW.md` §C7 and decision 22 are the standing rule for this
shape: **this repository does not create a directory for one thing**, and the bar
is three.

**What would have to change first**, if somebody ever wants to revisit it: the
components would have to stop being `scene.*`. `Portal`, `SurfaceCamera`,
`SurfaceLens`, `PortalTransit`, `PortalTransitSeen`, `SurfaceBounces` and
`SurfaceLimit` are registered here under a `scene.` prefix, and that prefix is
what `just components` files them under and what a `.agame` file and a
replication schema carry. Moving the code without moving the names files them
under a module they no longer live in; moving the names is a save-format break
for no gain the measurement above can find.

## What is actually unfinished about portals

`ROADMAP.md` v0.22 asks for "lighting, physics, projection, clipping and geometry
crossing the seam" to be seamless. This is what each of those five is missing
today, written by somebody who had just read the pass. It is a survey and not a
plan: nothing here is a promise about which version closes it.

### Lighting: occlusion crosses, illumination does not

A portal transports the *absence* of light and never light itself.
`render::ShadowNodes` maps the sun's direction through the partner's warp and
carries a far-side fragment back with `scene::SeamMatrix`, so a caster on the far
side darkens the near floor. Nothing adds a second contribution, deliberately:
that would double-light every floor near a doorway. The consequences:

- **Four beams per frame**, ranked by distance, the rest dropped with a warning.
- **The beam shadow only reaches the forward path.** `opaque.frag` has
  `BeamFactor` and applies it; `gbuffer.frag` declares `beamMap` and never
  samples it, and `deferred-lighting.frag` has no beam code. The shipped pipeline
  document builds `gbuffer` and `deferred-lighting`, so transported occlusion is
  visible in portal and mirror captures and not on the default screen path. That
  is the single largest lighting gap and it is a shader gap, not a maths one.
- **Seam spill is a two-mouth prototype.** `deferred-lighting.frag` treats the
  seam as a textured window with a fixed 45 degree spread at 128x128, and the
  budget is one pair. It exists only in the deferred shader, so a doorway's light
  pool is invisible inside any portal or mirror capture: the exact complement of
  the beam gap above.
- **Transported local lamps ignore the aperture** and spill into the whole far
  room. One hop, never across worlds, and the copies compete for the same
  sixteen-light budget as the room's own.
- **A straddling body is lit as one body and shadowed as two.** The far half is
  lit through the map and still shadowed along the near room's light matrix.

### Physics: static geometry crosses, momentum does not

`physics::GhostPortalBodies` copies the far room's **static** colliders into the
near room for a body standing in a seam. That is the whole mechanism, and it is
deliberately not the picture's mechanism.

- **A far-side dynamic body is refused by name**, because two copies of one body
  with its own momentum would fight through the wall between them. So a crate on
  the far side cannot be pushed by a body in the doorway, in either direction,
  and bodies stacked across a seam interpenetrate.
- **Angular velocity is mapped since v0.19, and by `Rotate` rather than
  `Carry`.** `scene::Motion` carries `Linear` and `Angular` and `CrossPortals`
  now maps both. It went four versions mapping `Linear` alone, and the reason it
  survived is that the case is invisible in a wall: a seam's destination is
  `LookAt(centre, centre + normal, UpFor(normal))` and the half-turn is a yaw, so
  any two upright panes compose to a map that leaves `Y` fixed and a body
  spinning about `Y` comes out looking right either way. Point a pane at the sky
  and it does not. `engine.scene.surfacecameras`' "a spinning body keeps its spin
  in the room it arrives in" is that pane, and it fails on the old pass. **The
  scale must not touch it**: radians per second have no length in them, so a hole
  that halves a body halves the radius and halves the speed of every point on it,
  and the rate is unchanged. `Carry` would spin a body down to nothing over a few
  crossings of a shrinking pair.
- **Both portal passes are gated on `scene::Motion`, sleep removes it, and that
  is a state rather than a hole.** `physics::Publish` drops `Motion` when a body
  sleeps, and both the straddler walk in `GhostPortalBodies` and the crossing
  walk in `CrossPortals` query for it, so a body that settles in a doorway is
  visited by neither. Neither omission can drop it. The same absence takes the
  row out of `IntegrateMotion`, so removing the far room's floor is removing a
  floor from under something that was not falling; and a crossing is the segment
  between `PreviousTransform` and `Transform`, which for a row nothing integrates
  is a point. Waking is putting `Motion` back, and `character.control` does that
  in `PreSimulation` **before** `portal.ghost` runs in the same phase - which is
  safe because registration order inside a phase is `ecs::Scheduler`'s stated
  contract, not an accident. `engine.physics.portals`' "a body asleep in a seam
  has no proxies and cannot fall through one" is the case; it was listed here as
  untested until v0.19.
- **The proxy carries four components** (`Transform`, `Bounds`, `Collider`,
  `PortalProxy`) and not `Surface` or `PhysicsProperties`, so a proxy of an ice
  floor has default friction.
- **`MAX_PROXIES_PER_BODY` is 32 and the overflow is a warning plus a partial
  prefix.** The overlap writes dynamic candidates before static ones and the loop
  then discards every dynamic one, so a busy far room can spend the whole budget
  on rows that are thrown away and leave the straddler no floor at all. The
  warning says the opposite ("the rest are not holding it up").
- **One seam per body per tick**, by an explicit `break`. A body where two panes
  meet gets one far room.
- **Only `Raycast` is portal-aware.** `ShapeCast`, `OverlapBox` and
  `OverlapSphere` are not, and the character's collide-and-slide sweep uses the
  raw broadphase, so the sweep reaches the far room only through the proxies.
  Ray continuation is one hop and does not recurse.
- **No test runs the solver.** `physics/tests/Portals.cpp` proves a proxy is
  created and retired; nothing proves a proxy generates a contact or holds
  anything up.

### Projection: the recursion is real, the default is contested

The recursive portal pass derives each level's camera from the camera the pass is
rendering from, which is what makes a hole seen through a hole correct rather
than merely fresh. What is open:

- **`RendererState::PortalDepth` is `2` and the comment above it argues at length
  for `1`**, reporting twenty of twenty runs hanging the device at depth two
  against none of thirteen at depth one, and naming the fix it needs (a command
  buffer per top-level hole) as unbuilt. Read in an uncommitted tree while
  `render` was being edited, so confirm with whoever owns `render` before acting
  on it, but the comment and the constant cannot both be right.
- **A view with no pane rectangle still iterates rather than descends.** A camera
  parented to the world and a cross-world pane have nothing to reflect through,
  so they resolve their chain by running the whole pass again from the eye. That
  is the residual of the v0.15 viewpoint fix and it is stated in
  `SurfaceCameras.hpp` rather than hidden.
- **A cross-world pane is still a frame behind and does not recurse**, by design:
  it is a window onto a second simulation rather than a hole in one space.
- Inside a deep reflection, blended geometry is sorted for the wrong eye, and a
  faded mirror shows as glass rather than as a reflection.

### Clipping: the oblique clip is real, the bias is measured from the wrong hole

The near plane is a true skew onto the mapped pane, at every level, re-derived
each time from the unskewed screen projection. What is left:

- **`PortalClipBias` is fed the distance from the viewer to the *nearest* pane in
  the frame, and then used for every hole at every level.** A distant hole seen
  while standing in a near one gets the near one's bias, and a level-one
  sub-camera gets the eye's bias rather than its own. The residual artefact is
  named in `render::PortalNodes`: a hairline of background around the inside of
  every hole, with parts poking through it, and pushing the bias the other way
  cuts a straddling body in two instead.
- **Two bias conventions coexist**: the seam-light capture uses a fixed stand-off
  rather than the viewer's distance.
- **The body cut is a `discard` and not a clip plane**, so it defeats early-Z,
  which is why what may be cut is bounded by what fits through the hole.

### Geometry: meshes cross, everything else does not

`CutAndCloneSeams` cuts a straddling body at the plane and appends the far half
as draw rows, carrying rigs in one piece. What does not cross:

- **A body wider than the aperture gets no far half at all.** `CutOfSeam::Fits`
  refuses it, because the rule that refuses a floor slab is the same rule.
- **Particles are moved rather than cloned, and only for same-world seams**, so
  sparks do not cross a cross-world portal.
- **Ribbons and trails have no seam clause anywhere** in `effects`.
- **No particles or ribbons are drawn inside any capture**, so a portal or mirror
  picture contains no smoke, no sparks and no trails.
- **Transparent panes are omitted from every sub-render.**
- **Spatial GUI is drawn into sub-renders but is never cut or cloned**; there is
  no GUI equivalent of `CutAndCloneSeams`.
- **A partly transparent part in the far world is drawn opaque.**
- Collision and the picture are deliberately different mechanisms
  (`GhostPortalBodies` against `CutAndCloneSeams`), so they can disagree, and the
  size rules they use are not the same rule.

### The sixth seam, which the roadmap does not name: authority

`scene.PortalTransitSeen` was replicated until v0.19, and the bug is the one to
keep in mind for everything above. It is a **derived local latch**:
`SnapPortalTransit` writes it after collapsing a client's interpolation onto
where a body arrived. Replicating it meant the authority's serial arrived in the
same snapshot as the crossing it described, the client's own pass found
`seen->Serial == went.Serial` on its first look, and the body interpolated
between two rooms instead of arriving. The fix is one entry in
`replication::Defaults`.

Two things follow, and both are conventions rather than mechanisms:

- **`CameraController::SeenTransit` is the same latch in another place**, and it
  is safe only because the whole `scene.CameraController` resource is local.
  Replicating any field of it reintroduces the identical defeat, and the view
  would keep pointing the way it did in the room the player left.
- **`CrossPortals` writes `PortalTransitSeen` inline**, so the snap is a no-op on
  the machine that simulated the crossing. That is a simulation pass writing a
  presentation latch, and it is correct only while the component stays on the
  local-only list. Nothing but this paragraph and the comment in `Defaults.cpp`
  says so, which by rule 6 makes it documentation.

`PortalTransit::Serial` is also compared for equality rather than ordering, so a
client that rolls prediction back to before a crossing holds a `Seen` ahead of
the `Transit` and snaps a second time. Untested.

## Ten components here are read by nothing, and that is a state rather than a gap

`Skeleton`, `Bone`, `AnimationClip`, `Animator`, `AnimationTrack`, `Constraint`,
`LevelOfDetail`, `Atmosphere`, `Clouds` and `Terrain` landed at v0.19 with no
consumer in the engine. That is decision 16 - *a surface may ship complete and
frozen with its implementation deliberately unwired*, revisited never - and root
`AGENTS.md`'s warning that an unwired subsystem is not dead code.
`docs/FUTURE_COMPONENTS.md` is what each gets wired to and in what order; do not
repeat that argument here, and do not delete one for having no reader.

**They are registered rather than left to register themselves, and that is not
tidiness.** `ecs::Components::Seal()` is called by the client and the server at
start-up, so a type that first reaches `Components::Of<T>` after the seal aborts
the process - which is the failure `scene.Sun` and `scene.PortalProxy` were both
found in at v0.19. Declaring the storage up front is also what makes the version
that wires it a change to behaviour rather than a change to the save format.

Four things a reviewer should refuse:

- **A `Fog` component.** The world already has fog:
  `LightingServiceComponent::FogColor`, `FogStart` and `FogEnd`, resolved by
  `LightingOf` and read by `render::ViewRecording`. A second one is rule 2, and
  it is exactly what `Humanoid::Radius` was deleted for. `Atmosphere` is a
  different model rather than a second copy of that one - linear fog fades the
  same looking up as along the ground, and scattering does not.
- **A `Kind` field on `Constraint`.** The class is how a kind is said: seven
  classes share one component and differ by the prototype row `Instance.new`
  copies, which is `Light`'s trade at greater width. A `Kind` beside the six
  motion modes makes a `HingeConstraint` whose axes are all free expressible,
  and the solver would have to pick which of the two statements it believed.
- **A stored chosen LOD level, or a stored terrain chunk.** Both are derived -
  the first changes per view within one frame, the second is gigabytes - and
  this file already refuses the same shape for a cached world AABB. `Terrain`
  stores the recipe; `SelectLevel` is a function.
- **A `Transform` on a `Bone`, or an `Attachment` under one.** A bone's frame is
  relative to its parent joint and `ResolveBones` composes it; `Attachment`
  resolves against the parent *part*. A class carrying both would have two world
  frames on one row filled by two passes against two different parents, which is
  the pair of opinions `Attachment`'s own class comment refuses. That is why
  `Bone` derives from `Instance` and not, as in Roblox, from `Attachment`.

**`Bone::ParentJoint` is strictly lower than `Bone::Joint`, and the build does
not check it.** By rule 6 that makes it a convention, and this is where it is
written down: it is what turns `ResolveBones` into a forward pass over a sorted
palette instead of a recursive walk, and an importer that cannot produce that
ordering has produced a cycle. The pass degrades to the rig's own frame rather
than looping, and `engine.scene.skinning` holds both halves.

## Visibility synchronization keys the walk, it does not hash the world

`SyncRendered` uses the `Hierarchy` and `Visual` component revisions plus their
matching counts to decide whether its membership walk is owed. It must not hash
every row on a steady presentation: that makes a resident scene cost linear CPU
time merely to prove that nothing changed. Revisions catch additions and
writes; counts catch removals. The slower walk and stale-row sweep have their own
profiling spans and should be absent from a steady-world trace.
