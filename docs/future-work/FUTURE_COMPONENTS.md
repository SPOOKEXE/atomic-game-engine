# The storage that ships before its feature

**What this file is.** Ten component types landed at v0.19 that nothing in the
engine reads. This says why they are here, what each one gets wired to, and in
what order, so that v0.22 and v0.24 start from a decision instead of a blank
page.

It is not a roadmap. `ROADMAP.md` decides what is in scope and when;
this decides what the storage those versions will need already looks like, and
records the arguments so they are not had twice.

**The rule this sits under is decision 16**: *"a surface may ship complete and
frozen with its implementation deliberately unwired"*, revisited never, because
it is a state and not a stage. Root `AGENTS.md` says the same thing from the
other side: an unwired subsystem is not dead code. `just source-check`'s
`public-header` rule reports rather than gates for exactly this reason.

**Why now rather than with the feature.** `ecs::Components::Seal()` is called by
the client and the server at start-up, so a component that first reaches
`Components::Of<T>` after the seal aborts the process. A component declared and
left to register itself works in a unit test and takes down every host the first
time a game file carries one. That is the failure `scene.Sun`,
`scene.PortalProxy` and `physics.PoppercamState` were all found in at v0.19, and
declaring the storage up front is what makes the version that wires it a change
to behaviour rather than a change to the save format.

---

## The ten, and where each is going

| Component | Wired by | Reads it | Writes it |
|---|---|---|---|
| `scene.Skeleton` | v0.21 | the skinning palette build in `render` | an importer or author |
| `scene.Bone` | v0.21 | `scene::ResolveBones`, then the palette | the animation handler |
| `scene.AnimationClip` | v0.21 | the animation handler | an author, or a `.rbxl` import |
| `scene.Animator` | v0.21 | the animation handler | an author |
| `scene.AnimationTrack` | v0.21 | the animation handler | a script, through `Instance.new` |
| `scene.Constraint` | after v0.24 | a constraint solver in `physics` | an author |
| `scene.LevelOfDetail` | after v0.24 | `scene::SelectLevel`, from the draw-list build | the publisher, out of `bake` |
| `scene.Atmosphere` | v0.22 | a render-graph node, through `scene::WorldLighting` | an author |
| `scene.Clouds` | v0.22 | a render-graph node, through `scene::WorldLighting` | an author |
| `scene.Terrain` | FUTURE | a terrain generator node set | an author |

Three of them already have a reader inside this module and are therefore not
fully unwired: `ResolveBones` fills every `Bone::WorldFrame`, `SelectLevel` is
the single statement of decision 19, and `LightingOf` resolves the atmosphere
and the cloud layer into `WorldLighting` beside the fog terms. Each has a suite.

---

## The order, and why it is that order

### 1. Skinning, completed at v0.21

`bake` preserves glTF `JOINTS_0` and `WEIGHTS_0`, the mesh format carries a
bounded skin-local palette size, and the renderer uploads resolved palettes
beside its resident instance rows. Imported bone-tree authoring remains a
separate model-import concern because the current bake graph exports one mesh
payload rather than a scene tree.

**`scene::Skeleton` and `scene::Bone` are where the rig half lands.** The
per-vertex half is four joint indices and four quantised weights on
`assets::MeshVertex`; mesh format version two carries them without widening the
world-to-renderer instance payload.

The order inside skinning is forced by what depends on what:

1. `assets::MeshVertex` gains joint indices and weights; `Mesh::VERSION` is
   bumped. Save-format break, which `docs/RELEASING.md` says is allowed
   pre-release.
2. `bake` fills the per-vertex streams. A future scene-tree importer must emit
   bones so that every `ParentJoint` is lower than its `Joint`; a glTF `joints`
   array is not required to be sorted that way.
3. `render` builds a palette per rig from `SkinningFrameOf`, and skins in the
   vertex shader.
4. The animation handler samples clips and writes `Bone::Transform`.

Steps 1 to 3 give a rig standing in its bind pose, which is a visible,
testable result with no animation system in it at all. That ordering is the
point of the split.

### 2. The animation handler, started at v0.21

The roadmap's line is "character controller + humanoid + character states +
state controller + bone controller, etc. More modular than roblox standard
humanoid", and the storage is shaped by that sentence rather than by Roblox's.

**A track is an instance here and a userdata in Roblox.** Roblox's
`AnimationTrack` is opaque because Roblox's `Animator` is a black box; the
roadmap says in as many words that this engine's is not going to be one. A row
in the store saves, replicates, is readable from a script and shows up in a
debugger. The cost is that playing a clip is `Instance.new` rather than a method
call, which is the trade `Sound.Playing` already makes.

The current handler covers the content-to-pose path:

- **Sample a clip.** `.aanim` channels and keyframes live in `assets`, beside
  `MeshData`. `AnimationClip::Asset` names one exactly as `Visual::Mesh` names a
  mesh, and demand loading fetches only clips a world names.
- **Blend by priority and weight.** `AnimationTrack` carries both. Nothing about
  the blend needs new storage.
- **Advance the play head.** `AnimationTrack::TimePosition` and fades advance on
  the fixed simulation tick; presentation samples that stored position.
- **Root motion remains controller work.** `Animator::RootMotion` and
  `RootMotionWeight` say what to do; applying sampled root deltas to a body stays
  with the character-controller integration rather than the presentation pass.

**`Humanoid` is not touched and must not be.** A component agent deleted
`Humanoid::Radius` at v0.19 for having two writers and no reader, and kept
`Height` and `Health` with reasons written beside them. Nothing here overlaps
it: an `Animator` may sit under a `Humanoid` and may equally sit over a door
hinge, which is the modularity the roadmap asks for, and requiring a humanoid to
reach an animator would be requiring a health bar to open a door.

### 3. Atmosphere and clouds, at v0.22

**There is no `Fog` type and its absence is the decision.**
`docs/ARCH_REVIEW.md` D4 reads the tree as having no fog at all. It has:
`LightingServiceComponent::FogColor`, `FogStart` and `FogEnd` are authored on
the `Lighting` service, `LightingOf` resolves them into `WorldLighting`, and
`render::ViewRecording` puts them in a uniform. A `scene::Fog` component would
be a second answer to what a world's distance fade is, which is rule 2 and is
exactly what `Humanoid::Radius` was deleted for.

What linear fog cannot express is scattering, which is why `Atmosphere` is a
separate type rather than more fields on the service: two distances and a colour
give a fade that is the same looking up as looking along the ground. Roblox
draws the same line, and lets an `Atmosphere` take over from the legacy fog
where one is present.

**Both are per-world presentation state and neither reaches a simulation
input.** That is decision 20: *"render graphs may vary per platform. Anything
reaching a simulation input may not."* The test to apply to a field somebody
wants to add: if a body's trajectory would change, it does not belong here.
Nothing in `Atmosphere` or `Clouds` is read by physics, by a character
controller or by a query, and `just determinism` and `just replay-check` are
byte-identical with them registered.

The wiring is a render-graph node that reads the `Air` and `Sky` members of
`scene::WorldLighting`, and the decision about which of the two fog models a given graph reads
is that graph's - which is what decision 20 permits and what carrying both makes
possible.

**Dynamic ambient occlusion is in the same roadmap bullet and gets no component,
deliberately.** It is a property of a render pass rather than of a world:
strength, radius and sample count are node parameters, Roblox has no instance
for it, and a component would be a per-world number that a graph varying per
platform would have to override anyway.

### 4. Level of detail, after v0.24

Decision 19 constrains this before anything is built: *"LOD selection targets
quad utilization. No virtualized geometry"*, revisited only when measurement on
this engine's own content contradicts it. So `scene::SelectLevel` compares
pixels of projected area per triangle and never a distance, and the component
stores that target rather than a distance ladder.

The two disagree exactly where it costs. A hundred-metre tower and a coffee cup
at the same distance cover wildly different areas of the screen, so a distance
ladder either over-tessellates the cup or under-tessellates the tower - per
camera, per field of view, per resolution. Area per triangle is the same number
on a phone and on a workstation.

What is left to wire:

1. `bake` grows the two generated strategies. `Decimated` is edge collapse by
   quadric error; `Reduced` orders collapses by how little surface area an edge
   carries, which is the roadmap's "smart triangle reduction thinking of nanite
   triangle surface area". Both fill `LevelOfDetail::Ratios` and publish the
   coarse meshes under derived names.
2. The draw-list build calls `SelectLevel` and substitutes `LevelMesh`'s answer
   for `Visual::Mesh`.

**Which pass that is, is decided and it is not `client::CollectInstances`.**
That walk is `EachBatchParallel` over a fixed signature and cannot read an
optional column at all, which is why `SurfaceAppearance` and `Tags` are on every
part. Level selection is the opposite case: it runs over
`<Visual, LevelOfDetail>` and touches only the parts that have levels, which in
a world of four thousand plain cubes is none of them. Putting forty bytes on
every cube to save a join on the few hundred that are models is the trade
backwards.

**The chosen level is not stored anywhere and must not become stored.** It is
derived, it changes per view, and a mirror looks at the same part from somewhere
else in the same frame.

### 5. Constraints, after v0.24

`ROADMAP.md` has "constraints system" under FUTURE. `scene::Constraint` is one
generic six-degree-of-freedom joint rather than a struct per kind: two
attachments, six axes, and a motion mode per axis. Bullet calls the shape
`btGeneric6DofSpring2Constraint` and PhysX calls it a D6 joint.

Roblox's family falls out of the modes rather than out of a `Kind` field, and
each class says which member of it by setting the modes as a **prototype
default** - which is `Light`'s three-classes-one-component trade at greater
width. A `Kind` field would make a `HingeConstraint` whose axes were all free
expressible, and the solver would have to decide which of the two statements it
believed.

Seven classes are registered: `WeldConstraint`, `BallSocketConstraint`,
`HingeConstraint`, `PrismaticConstraint`, `CylindricalConstraint`,
`RopeConstraint` and `SpringConstraint`. `AlignPosition` and `AlignOrientation`
are the drive rather than classes of their own: an axis with a `Stiffness` is
pulled towards `Constraint::Target`, and an axis without one is clamped by its
limit.

**`NoCollisionConstraint` is deliberately absent.** It is not a joint: it is a
broadphase filter, and `Collider::Layer` and `Collider::Mask` are already this
engine's answer to what may touch what.

What is left to wire is entirely in `physics` at L8:

1. `physics::PhysicsWorld` grows a joint array beside its contact manifolds, and
   `SyncBroadphase`'s pass gathers `<Constraint>` into it the way it gathers
   colliders.
2. The sequential-impulse solver gains a joint row per non-`Free` axis. A
   `Locked` axis is a hard row, a `Limited` axis is a one-sided row at each
   bound, and a driven axis is a soft row with `Stiffness`, `Damping` and the
   force caps.
3. Warm starting keeps accumulated impulses **in `PhysicsWorld` and never on the
   authored row**. That is the decision `RigidBody` already made once when it
   refused a sleeping flag: solver state belongs where the solver's other
   scratch is, and `PhysicsWorld` does not cross the wire for
   `CollisionShapes`' reason.

Nothing about the solver needs a change to `Constraint`. That is the test the
one-component design was chosen against.

### 6. Terrain, FUTURE

The roadmap asks for a "(procedural, node-based) terrain generator", so the
authored thing is a graph and a seed and **the chunks it produces are never
stored**.

Two arguments, and the second is the stronger one:

- A generated chunk is a derived fact stored beside its inputs, which is what
  this module refuses everywhere - a world AABB off `Bounds`, a triangle count
  off `Visual` - at a scale where getting it wrong costs gigabytes rather than
  sixteen bytes.
- It is the only form that can cross a wire at all. `CollisionShapes` makes
  exactly this argument for hulls: sending a conclusion instead of its input
  hands an attacker the half they get to choose. Both ends run the same graph
  over the same seed and get the same ground, which is decision 14's strict IEEE
  arithmetic doing the work it exists to do.

**A resource and not a component on a `Terrain` instance**, which is where
Roblox puts it. Roblox's `Terrain` derives from `BasePart` and is a singleton
under `Workspace` that `Instance.new` refuses to make a second of, and this
engine has no way to register a class that cannot be constructed. A resource is
one per world by construction, which is the property that mattered;
`WorldBounds` is the existing type with the same shape.

What is left to wire:

1. A terrain node set in `graph`, and the chunk store the generator owns -
   exactly as the broadphase grids are `physics::PhysicsWorld`'s.
2. Residency driven by `Terrain::ViewDistance` around whatever holds an
   `ActiveCamera`, plus a collider per resident chunk built through
   `scene::CollisionShapes`, which already exists and already takes a triangle
   soup.

---

## Three things this work deliberately did not declare

- **Per-vertex joints and weights**, for the reason under skinning above: they
  are a mesh format change and belong with the importer that fills them.
- **A `Fog` component**, because the world already has fog and a second one
  would be rule 2.
- **An ambient-occlusion component**, because it is a render-pass parameter and
  decision 20 permits a graph to vary per platform.

## Two script surfaces that are deliberately absent

`Skeleton` and `LevelOfDetail` declare no property. Both are optional columns on
a `MeshPart`, so a property on that class would fail on every mesh part that is
not skinned and has no ladder - which is almost all of them - and a property
that usually fails is worse than one that does not exist. Both are written by
the publisher, which is C++. `AGENTS.md`'s "do not declare a property for
scripts alone" is the same rule read from the other end.

`Bone::Joint` and `Bone::ParentJoint` are refused for a sharper reason: a script
renumbering a palette slot is a script that makes a vertex's joint index name a
slot nothing filled.
