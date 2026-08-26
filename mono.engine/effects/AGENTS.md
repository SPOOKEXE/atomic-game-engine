# mono.engine/effects - module invariants

Particles, beams and trails. L8, `shared`, beside `physics`.

Everything here is shaped by one number: `ROADMAP.md` v0.10 asks for **100,000
emitters at five particles each**, which is half a million particles a frame.
Read `Particles.hpp`'s header before changing anything; it lists the three
arrangements that number rules out, and most mistakes in this module are one of
them coming back.

---

## The invariants

**A particle is not an entity.** Half a million rows whose whole life is one
create and one destroy is the pair of operations an archetype store is worst at.
The emitter is the entity; the particles are pooled in a resource.

**A particle is not a `scene::DrawInstance`.** `ParticleInstance` is 28 bytes and
every field varies *between two particles of one emitter*. Anything shared - the
texture, the blend mode, the orientation rule - belongs on the emitter and is
reached through `Slot`. The `static_assert` on the size is not a formality: four
more bytes is half a megabyte of extra upload per frame at the target count, so
changing it wants its reason written beside it.

**Nothing on the per-frame path reads `ParticleEmitter`.** It is about 1.5 KB a
row. The step reads `EmitterBlock`, which is the small sampled thing
`RefreshEmitters` derives from it. **If a system appears that walks the emitter
column every frame, the fix is to move the fields it wants into the block** -
not to make the walk faster.

**A block's particles never leave that block.** Retirement is a swap with the last
live particle inside the same block, so no two workers ever touch the same bytes
and `StepParticles` needs no atomic and no lock. A global compaction would be a
data dependency across every worker and would end the parallel step.

**The randomness is stateless and seeded from `(emitter, spawn index, purpose)`.**
Not `core::Random` - that is SHA-256 per number, which is right for a value that
must be bit-identical everywhere and enormously wrong at twenty thousand spawns a
second. What is kept is the property that matters: a pure function of the seed, so
two runs of one scene emit the same particles and a recording replays.

**The seed index is `EmitterBlock::Spawned` and never the slot.** A slot is reused
the moment a particle dies, so seeding from it makes every replacement identical
to what it replaced - a steady emitter settles into a loop of the same handful of
particles within one lifetime.

**A trail records in `Simulation` and everything else derives in `PreRender`.** A
trail is a record of where something has been, so sampling it at frame rate makes
its length depend on the machine drawing it - which is a desync arriving through a
decoration.

---

## What is deliberately absent

Stated here rather than left to be inferred, because `scene::SurfaceAppearance`'s
rule applies: a field nothing reads is a field an author would reasonably assume
worked.

- **Particle collision.** Roblox has it; it needs a broad-phase query per particle
  and that is a different order of cost. The property is not declared.
- **`WindAffectsDrag`.** There is no wind.
- **`LightEmission` and `LightInfluence` are stored and not consumed.** The
  particle pass is unlit. They are the two exceptions to the rule above and they
  are here because they are what a *lit* variant would be selected by - an author
  setting them is describing the effect correctly and the renderer has not caught
  up.
- **A GPU simulation.** The step is a CPU parallel loop. A compute-shader step is
  the right answer at ten million particles and the wrong one at half a million,
  where the upload dominates and a compute path buys a second copy of every
  emitter's rules living in a shader.

---

## Where the numbers are

`engine.effects.bench.particles` carries the measured cost of the two simulation
passes and the two findings that came out of it - the curve-sampling gate, and the
confirmation that the serial spawn loop is free. `examples/Particles.luau` carries
the on-screen figures and the two bugs that only a capture found.

**A reading that disagrees with a comment in this module wins, and the comment
should be corrected in the same change.** Two of them already have been.

---

## What this module may not link

`render`, `assets` and `physics`, and each for its own reason:

- **`render`** - the pipeline is `client` tier and this is `shared`. The particle
  stream crosses as `ParticleInstance` and `render::ParticleBatch` is assembled in
  `mono.client`, which is where the two tiers legitimately meet.
- **`assets`** - a particle names its texture with a `core::Name` exactly as
  `scene::Visual` names a mesh. Nothing here learns what an image is.
- **`physics`** - nothing here collides. The day particle collision exists this
  becomes a real question; today naming it would be an edge that buys nothing.

---

## Nothing here crosses a replication wire, and that is the design

`replication::SHARED_PREFIXES` is `scene.` and `gui.`. No `effects.` component is
in the default replicated table, and a review that reads that as an omission has
the observation right and the conclusion wrong. Three things stand in the way,
and the first is fatal on its own:

- **A replica never registers this module.** `client::BuildReplicatedWorld`
  registers `scene`, `gui`, `script`, `client` and `replication`. Effects reach a
  `--game` client because the world loader registers them, not because the client
  does. `ecs::LoadSnapshot` refuses a snapshot naming a component the build does
  not have, so one `ParticleEmitter` on a server would fail every join.
- **The rows would arrive empty.** `Beam::Attachment0`, `Trail::Attachment0` and
  `EmitterSlot::Index` are dropped by their own writers and cleared by their
  readers, because a handle and a pool index describe one process. A replicated
  beam has no endpoints.
- **These are the three widest rows in the engine.** `ParticleEmitter` is 1264
  bytes, `Trail` 1152 and `Beam` 712, all trivially copyable, so all three would
  be hashed per row per tick on the signature path.

An effect a server means every client to see is the **instance** crossing -
`ecs.InstanceClass` already does - and each machine building its own component
from that class. A reviewer handed a prefix change here should ask for the
registration on the replica side first, because without it the change does not
degrade, it fails the join.

## `Trail`'s history is on the row, and it is not in the file

`Trail` is 1152 bytes and 448 of them are `Top`, `Bottom` and `Age`. That reads
like a large saved row, and it is not: `WriteTrails` walks the authored fields
one at a time and never touches the ring, and `ReadTrails` puts it back at empty.
`engine.effects.ribbon` measures it - a full ring and an empty one write byte for
byte the same thing - so a proposal to split the history out to keep it out of a
file is answering a question that is already answered. A split for a *cache*
reason would need a pass that reads one half without the other, and both passes
that touch a trail read both halves.
