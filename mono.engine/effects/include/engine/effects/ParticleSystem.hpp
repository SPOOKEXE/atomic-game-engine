#pragma once

// The pool half a million particles live in, and the two passes that move them.
//
// **A resource, because there is one of it per world**, which is `ecs/AGENTS.md`'s
// rule applied to the largest piece of state the engine has. The alternative - a
// particle buffer per emitter - is a hundred thousand allocations and a hundred
// thousand pointer chases to reach one particle, which is the arrangement the
// roadmap's target rules out before anything else.
//
// ## The three arrays, and why they are three
//
// A particle is described by three separate things and they are read by three
// different loops, so they are three arrays rather than one array of structs:
//
// - **`ParticleInstance`** - what a GPU reads. Written by the step, read by the
//   upload, never read by the simulation.
// - **`ParticleState`** - what the step reads and writes. Velocity, age,
//   lifetime, seed. Never touched by the upload.
// - **`EmitterBlock`** - what an emitter contributes, sampled small. One per live
//   emitter, not one per particle.
//
// The instance array and the state array are indexed identically, so a particle
// is one subscript in both. Splitting them means the upload streams thirty-two
// contiguous bytes per particle instead of striding over the twenty-eight it does
// not want, and the step never pulls a colour into cache to ignore it.
//
// ## Blocks, and why a particle never moves between them
//
// Each emitter owns a **contiguous capacity block**, sized when it first emits
// and never resized. Inside a block the live particles are a prefix; a particle
// that dies is swapped with the last live one and the count drops.
//
// **A swap inside a block rather than a global compaction**, and that is the
// decision the whole parallel step rests on: a global compaction is a data
// dependency across every worker, so it either serialises the step or needs an
// atomic per particle. A block-local swap touches only that block's memory, which
// means `Jobs::For` can hand one worker a range of blocks and no two workers ever
// write the same byte. No atomics, no locks, and the output is in the same order
// every frame - so a recorded run replays, which is rule 5.
//
// ## The sampled curves
//
// `ParticleEmitter` carries four curves totalling about a kilobyte, and
// evaluating one is a scan over its keypoints. At half a million particles a
// frame that is half a million scans over data that has not changed. So each
// block holds a **sixteen-entry sampled table** - `ParticleCurves`, 256 bytes -
// rebuilt when the emitter's column version moves and read by the step as an
// index and a lerp.
//
// **Sixteen samples is a visible-quality decision and it is stated as one.** A
// curve that ramps over a whole lifetime is smooth at sixteen; one authored with
// a hard step in it - two keypoints at the same time, which `Sequence.hpp` says
// is how an edge is written - lands the edge on the nearest sixteenth of the
// life. That is up to about three per cent of a lifetime late, which at a
// five-second particle is 150 ms. Noticeable for a deliberate flash and invisible
// for everything else. The fix, when somebody needs it, is a per-emitter sample
// count rather than a bigger constant for everybody.
//
// @tier L8 · shared

#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/effects/Particles.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::effects {

	// How many points a curve is sampled at.
	//
	// Sixteen, for the reason the header gives. A power of two, so the index and
	// the fraction come out of one multiply and a shift rather than a divide.
	inline constexpr uint32_t CURVE_SAMPLES = 16;

	// How many emitters one world may have live at once.
	//
	// **65,535, which is below `ROADMAP.md`'s hundred thousand, and that is a
	// deliberate limit rather than an oversight.** `ParticleInstance::Slot` is
	// sixteen bits because thirty-two would make the instance thirty-six bytes -
	// four more across half a million particles is two megabytes a frame of extra
	// upload to address emitters that are not on screen.
	//
	// What the roadmap's number actually asks for is a hundred thousand emitters
	// *existing*, and a world may hold any number: an emitter without a block
	// emits nothing and costs one skipped row. Blocks are handed out to the
	// emitters that are enabled and in view, so the cap is on how many are
	// *emitting at once* - and a scene with sixty-five thousand of those has half
	// a million particles before it runs out, which is the other limit.
	//
	// **Running out is logged once and then silent**, because a message per
	// emitter per frame at this count is a log nobody can read. `Statistics::
	// EmittersRefused` is the number to look at.
	inline constexpr uint32_t MAX_EMITTER_SLOTS = 65535;

	// What no emitter's slot is.
	inline constexpr uint16_t NO_SLOT = 0xFFFF;

	// One emitter's curves, sampled flat.
	//
	// **256 bytes, against about a kilobyte for the four sequences it comes
	// from**, and the saving is not the point - the point is that reading it is an
	// index rather than a scan. The step does this four times per particle per
	// frame.
	//
	// @since v0.10
	struct ParticleCurves {
		// Width in metres at each sixteenth of a life.
		float Size[CURVE_SAMPLES] = {};

		// Alpha - one minus transparency - at each. **Stored as opacity rather
		// than as transparency**, because that is what the packed colour needs and
		// the flip belongs where the table is built rather than in a loop that
		// runs half a million times.
		float Alpha[CURVE_SAMPLES] = {};

		// How much taller than wide, at each.
		float Squash[CURVE_SAMPLES] = {};

		// Colour at each, as RGB8 in the low twenty-four bits.
		//
		// **Packed here rather than in the step**, for `Alpha`'s reason: the
		// conversion from linear float to eight bits is three multiplies and three
		// clamps, and doing it sixteen times per emitter beats doing it once per
		// particle.
		uint32_t Colour[CURVE_SAMPLES] = {};
	};

	// Samples one emitter's curves.
	//
	// @param emitter The authored emitter.
	// @param out     Filled in.
	void SampleCurves(const ParticleEmitter &emitter, ParticleCurves &out);

	// Reads a sampled curve at a normalised age, interpolating between samples.
	//
	// @param samples The sixteen values.
	// @param age     Where in the life, 0 to 1. Clamped.
	// @return The interpolated value.
	float SampleAt(const float (&samples)[CURVE_SAMPLES], float age);

	// The same, for the packed colour table.
	//
	// **A separate function rather than a template, because the interpolation is
	// not the same operation.** These are three eight-bit channels in one word,
	// so blending them means unpacking, lerping and repacking - and doing that
	// through a generic `SampleAt` would mean either a float table (four times
	// the size, which is what packing avoided) or a specialisation that shares no
	// line with the original.
	//
	// @param samples The sixteen packed colours.
	// @param age     Where in the life, 0 to 1. Clamped.
	// @return The interpolated colour, RGB in the low twenty-four bits.
	uint32_t SampleColourAt(const uint32_t (&samples)[CURVE_SAMPLES], float age);

	// Which block an emitter owns, on the emitter's own row.
	//
	// **A component and not a map**, because finding an emitter's block happens
	// once per emitter per frame in two passes, and a hash lookup at a hundred
	// thousand rows is a hundred thousand cache misses to answer what a column
	// read answers for free. `ecs/AGENTS.md`'s rule - componentise what you
	// iterate - with the iteration named.
	//
	// **Not in the `ParticleEmitter` class's authored set by accident**: it is in
	// it deliberately, so `Instance.new("ParticleEmitter")` has one and the
	// refresh pass never has to add a component during iteration. It carries no
	// property, because a block index is this module's bookkeeping and not
	// something an author has any use for.
	//
	// @since v0.10
	struct EmitterSlot {
		// Which block, or `NO_SLOT` when this emitter has none.
		//
		// **Not serialised as a meaningful value** - see `Registration.cpp`. A
		// block index is a position in one process's pool, which is rule 4's
		// hazard exactly: restoring it would point an emitter at whatever block
		// happened to take that number.
		uint16_t Index = NO_SLOT;

		// Explicit padding, for the reason every other `Reserved` gives.
		uint16_t Reserved = 0;
	};

	// Where an emitter emits from, in world space.
	//
	// **One column read and never a walk.** An emitter parented to an
	// `Attachment` takes that attachment's resolved world frame, and one parented
	// to a part takes the part's `Transform` - both of which are already there by
	// the time this runs, because `ResolveAttachments` is registered ahead of it.
	// An emitter parented to neither emits from the origin, which is visible and
	// therefore better than emitting nothing.
	//
	// @param store   The world.
	// @param emitter The emitter instance.
	// @return Its world-space frame.
	core::CFrame EmitterFrame(const ecs::Store &store, ecs::Entity emitter);

	// How big the volume an emitter spawns inside is, as a half-extent.
	//
	// The parent part's `Bounds`, which is Roblox's arrangement: resizing a part
	// resizes its effect without touching the emitter. An emitter on an
	// attachment or on nothing spawns from a point, which is a zero half-extent
	// rather than a default box - a default would make every attachment-parented
	// effect a metre wide for reasons nobody authored.
	//
	// @param store   The world.
	// @param emitter The emitter instance.
	// @return The half-extent, or zero.
	core::Vector3 HalfExtentOf(const ecs::Store &store, ecs::Entity emitter);

	// One live emitter's runtime state, beside the sampled curves.
	//
	// **Everything the step needs and nothing else**, which is the whole of why
	// this type exists separately from `ParticleEmitter`: the step walks these
	// every frame, and the authored component is fifteen hundred bytes of which
	// the step wants forty.
	//
	// @since v0.10
	struct EmitterBlock {
		// The curves, sampled.
		ParticleCurves Curves;

		// Where the emitter was when this block was last refreshed.
		//
		// **Kept rather than looked up, because a locked emitter needs it per
		// particle.** `LockedToPart` recomputes a particle's position from the
		// parent's frame every step, and a lookup per particle would be a random
		// access into another archetype half a million times.
		core::CFrame Frame;

		// A constant push, in metres per second squared.
		core::Vector3 Acceleration;

		// Where this block's particles start in the pool.
		uint32_t First = 0;

		// How many slots it owns.
		uint32_t Capacity = 0;

		// How many of them are alive, contiguous from `First`.
		uint32_t Live = 0;

		// How many this emitter has ever spawned.
		//
		// **The seed's index, and it must not be the slot number.** A slot is
		// reused the instant a particle dies, so seeding from it makes every
		// replacement identical to what it replaced - a steady emitter settles
		// into a loop of the same handful of particles within one lifetime, which
		// reads as a stuttering effect rather than as a seeding mistake. A
		// monotonic counter gives every particle its own draw.
		//
		// Wraps at four billion, which at a thousand a second is seven weeks.
		// Wrapping is harmless: it repeats a sequence, it does not corrupt one.
		uint32_t Spawned = 0;

		// How fast speed is shed, as a fraction per second.
		float Drag = 0.0f;

		// Which tenancy of this run of the pool the block is on.
		//
		// **Because the device cannot be told to forget.** A block's rows are
		// returned to the free list and handed to the next emitter that asks,
		// and the host used to make that safe by resetting `Live` to zero - the
		// old particles were still in the array, but nothing drew them. The
		// device draws a block's whole capacity, so they would be drawn, with
		// the new emitter's curves and for as long as their old lifetimes had
		// left.
		//
		// A particle carries the generation it was born under and the step
		// treats a mismatch as death, so a recycled block starts empty without
		// anything being cleared. Bumped once when the range is taken; the
		// alternative was uploading a zeroed state for every row of every block
		// claimed, which at scene load is the whole pool.
		uint32_t Generation = 1;

		// Seconds since this block last spawned anything.
		//
		// **What lets `Live` reach zero when the device owns the pool.** The
		// host cannot see a particle die, so `Live` is what it has ever put in
		// the block - which never falls, so a disabled emitter would hold its
		// rows for as long as it existed and go on costing its capacity in
		// quads with no extent. Past `Longest` nothing born before can still be
		// alive, whatever the device is doing, and the block is empty by
		// arithmetic rather than by observation.
		float Idle = 0.0f;

		// The longest a particle of this emitter can live, in seconds. Read with
		// `Idle` and set from `ParticleEmitter::Lifetime`.
		float Longest = 0.0f;

		// What is left over from the last frame's emission, in particles.
		//
		// **A fractional accumulator, and it is what makes a low rate work at
		// all.** An emitter at three particles a second over a sixtieth of a
		// second owes 0.05 of a particle; truncating that emits nothing, forever.
		// Roblox's emitters have the same accumulator for the same reason.
		float Pending = 0.0f;

		// The entity this block belongs to, so a dead emitter's block is freed.
		ecs::Entity Owner;

		// How many cells the flipbook has, and how it plays.
		//
		// Copied off the emitter rather than read through it, for `Frame`'s
		// reason. Four bytes here against a fifteen-hundred-byte load there.
		//@{
		FlipbookLayout Flipbook = FlipbookLayout::None;
		FlipbookMode FlipbookPlayback = FlipbookMode::OneShot;
		//@}

		// How many cells hold a frame, resolved from the emitter - never zero.
		//
		// **Resolved here rather than left as the emitter's zero-means-all**, so
		// the step never has to ask which meaning it is looking at.
		uint8_t Frames = 1;

		// Whether particles are recomputed from the parent's frame each step.
		bool Locked = false;

		// Whether an emitter claimed this block on the current refresh.
		//
		// **Named `Reserved` and it is not padding**, which is worth saying
		// because every other `Reserved` in the engine is. `RefreshEmitters`
		// clears this over every block, lets the emitter walk set it, and frees
		// whatever is still clear - which is how a block belonging to a destroyed
		// emitter is reclaimed without keeping a second list of live owners.
		uint8_t Reserved = 0;

		// How fast a flipbook runs, in cells per second, under a mode that pays
		// attention to it.
		float FlipbookRate = 12.0f;
	};

	// One particle's simulation half.
	//
	// **Never read by the upload and never written by it**, which is the whole
	// reason it is a separate array from `ParticleInstance`. Sixty bytes, so
	// half a million is thirty megabytes touched by the step and by nothing
	// else.
	//
	// @since v0.10
	struct ParticleState {
		// Metres per second, in world space.
		core::Vector3 Velocity;

		// How long it has been alive, in seconds.
		float Age = 0.0f;

		// How long it gets, in seconds. Zero means the slot is free.
		//
		// **Zero is the free marker rather than a separate flag**, because a
		// particle with no lifetime is not a particle and there is no other
		// meaning to give the value. That saves a byte per particle and, more
		// usefully, makes "is this alive" a comparison the step already does.
		float Lifetime = 0.0f;

		// Where it started, in the emitter's local space.
		//
		// **Only read by a locked emitter**, and carried by every particle
		// anyway. An optional field would mean two state arrays or a branch on a
		// pointer per particle; four bytes times half a million is two megabytes,
		// and the branch is worse.
		//
		// Kept as a `Vector3` rather than as a direction and a distance, because
		// the step integrates it and a polar form would mean converting twice per
		// particle per frame to save four bytes on an array only the step reads.
		core::Vector3 LocalOffset;

		// How fast it spins, in turns per second.
		//
		// **Turns and not degrees, and resolved at spawn rather than each step.**
		// The packed rotation is a turn over 65,536, so keeping the speed in the
		// same unit makes the integration one multiply-add with no conversion -
		// and the alternative, redrawing it from the seed every frame, is a hash
		// per particle per frame to reproduce a number that never changes.
		float Spin = 0.0f;

		// Which draw this particle was, from its emitter.
		//
		// **Not the slot it occupies**, which is reused the moment a particle
		// dies. See `EmitterBlock::Spawned` for what seeding from the slot does to
		// a steady emitter.
		uint32_t Seed = 0;

		// Where it is, in world space.
		//
		// **Here and not in `ParticleInstance`, and that is what lets the whole
		// instance stream be output.** Of the five things an instance carries,
		// only the position and the accumulated turn are read by the *next*
		// step - size, colour and the flipbook cell are functions of age and are
		// recomputed from scratch every frame. Keeping the two that persist on
		// this side means nothing in the instance stream is ever read back, so
		// the step can write it straight to wherever the draw wants it rather
		// than to a pool that then has to be gathered.
		//
		// It is also what keeps a particle carried through a portal honest: the
		// seam moves the *drawn* position and leaves this one alone, so the
		// spark goes on integrating in the space it was born in and is carried
		// afresh each frame, which is exactly what the host-side carry did.
		core::Vector3 Position;

		// Which tenancy of its block this particle belongs to.
		//
		// **Compared by the step, which is what makes a recycled block start
		// empty.** See `EmitterBlock::Generation`; a mismatch is death.
		uint32_t Generation = 0;

		// The accumulated turn, over 65,536, and the cell a fixed flipbook drew.
		//
		// Laid out as `ParticleInstance::RotationAndCell` is, because that is
		// where it is going. The cell half is only meaningful for the random
		// flipbook mode, which picks once at spawn and keeps it - every other
		// mode is a function of age and the step works it out.
		uint32_t Rotation = 0;
	};

	// What one step did, for the panel and for a test.
	//
	// @since v0.10
	struct ParticleStatistics {
		// How many particles are alive across every block.
		uint32_t Live = 0;

		// How many were born this step.
		uint32_t Emitted = 0;

		// How many died.
		uint32_t Retired = 0;

		// How many emitters had a block.
		uint32_t Blocks = 0;

		// How many wanted one and did not get one.
		//
		// **The number that explains a scene with missing effects**, and it is
		// reported rather than logged for the reason `render::FrameResult::
		// SurfacePasses` is: at this count a line per refusal is a log nobody can
		// read, and a count that is not zero is the whole diagnosis.
		uint32_t EmittersRefused = 0;

		// How many particles a block wanted to emit and had no room for.
		//
		// Distinct from `EmittersRefused`: this is an emitter that *has* a block
		// and has filled it, which is the ordinary steady state of a looping
		// effect rather than a problem. Reported because "my rate went up and
		// nothing changed" is the question it answers.
		uint32_t SpawnsDropped = 0;
	};

	// The pool, the blocks, and the two passes' scratch.
	//
	// @since v0.10
	struct ParticleSystem {
		// What a GPU reads. Indexed identically with `States`.
		std::vector<ParticleInstance> Instances;

		// What the step reads and writes.
		std::vector<ParticleState> States;

		// One per live emitter.
		std::vector<EmitterBlock> Blocks;

		// Rows of `Blocks` whose emitter has gone, waiting to be handed to the
		// next one that arrives.
		//
		// **Without this the cap above is on emitters that have *ever* existed
		// rather than on emitters emitting at once, which is what it says it
		// is.** Reclaiming a block put its particle range back on `Free` and
		// left the row itself in `Blocks` for ever, while every new emitter did
		// a `push_back` - so a game doing what a game does, one emitter per
		// explosion and muzzle flash and footstep, walked a row per effect it
		// had ever played on every tick, held three hundred-odd bytes for each,
		// and after 65,535 of them refused to emit anything again for the rest
		// of the process. None of that is visible in a scene that builds its
		// emitters once, which is every scene in `examples/`.
		//
		// Indices rather than pointers, because `Blocks` moves when it grows.
		// Refilled by the reclaim sweep and drained by the claim walk, which run
		// in that order in one `RefreshEmitters` - so a row freed this tick is
		// reused on the next one rather than under the walk that freed it.
		std::vector<uint32_t> FreeSlots;

		// Whether the device owns the pool and this module only spawns into it.
		//
		// **Set by whoever has a renderer, and the client always does.** When it
		// is on, `StepParticles` skips its ageing pass entirely: the device
		// integrates, shades and draws from `particle-step.comp` and nothing has
		// to cross the bus but the block parameters and the frame's births. That
		// is worth about two milliseconds a frame at half a million particles,
		// which is most of what a particle frame used to cost.
		//
		// **The host-side pass is kept rather than deleted, and not out of
		// timidity.** It is the reference the module's tests pin the behaviour
		// to - a compute shader cannot be asserted about from a unit test - and
		// it is what a build with no compute device falls back on. The two are
		// the same integration written twice, so a change to one is a change to
		// both; `particle-step.comp` says so at each point where it matters.
		bool DeviceStepped = false;

		// Which pool rows were spawned into this tick.
		//
		// **Rows and not states**, because the state is already in `States` and
		// the renderer is going to read it there. A birth is four bytes here and
		// fifty-six in the array it points at, and the array is written by the
		// same worker that decides the birth - so nothing is copied twice.
		//
		// Empty unless `DeviceStepped`. The host-side pass has no need of it:
		// it spawns into the array it also ages.
		std::vector<uint32_t> Births;

		// How many slots the pool holds in total.
		//
		// **Fixed at install time rather than grown on demand**, and the reason is
		// the same one that made blocks contiguous: growing the pool reallocates
		// under every block's indices, so it would have to happen between frames
		// with nothing running - which is exactly when nobody knows how much is
		// needed. A pool that is full drops spawns and says so.
		uint32_t Capacity = 0;

		// How many particles one emitter may ever hold.
		//
		// A block is `Rate * max Lifetime` rounded up and capped here, so one
		// emitter with a runaway rate cannot take the whole pool.
		uint32_t BlockCeiling = 4096;

		// What the last step did.
		ParticleStatistics Statistics;

		// How far the pool has been handed out, in slots.
		//
		// **A bump allocator with a free list and not a general one**, because
		// blocks are all that is allocated and they are never resized. Freeing a
		// block pushes its range here; allocating takes an exact-fit range if
		// there is one and bumps otherwise.
		uint32_t Used = 0;

		// Ranges freed by dead emitters, as `(first, capacity)` pairs.
		std::vector<std::pair<uint32_t, uint32_t>> Free;
	};

	// Gives a world a pool of a stated size.
	//
	// @param store    The world.
	// @param capacity How many particles it may hold at once.
	void InstallParticles(ecs::Store &store, uint32_t capacity);

	// Hands out and reclaims blocks, and refreshes each one from its emitter.
	//
	// **The pass that touches `ParticleEmitter`**, and the only one. It walks the
	// emitter column, gives a block to anything enabled that has none, frees the
	// blocks of emitters that have died or been disabled and emptied, and
	// re-samples the curves of any whose properties changed.
	//
	// Registered in `PreSimulation`, before `StepParticles`, because a block
	// handed out after the step is a block that emits nothing for one frame.
	//
	// @param store The world.
	// @return How many blocks are live.
	size_t RefreshEmitters(ecs::Store &store);

	// Ages every particle, spawns new ones, and writes the instance stream.
	//
	// **The loop the whole module is shaped around.** Parallel over blocks, no
	// atomics, no allocation, and the output order is a function of the block
	// order - which is a function of the emitter column's order, which is stable
	// within a tick. So two runs of one scene produce the same stream, which is
	// what rule 5 asks of anything inside a tick.
	//
	// @param store The world.
	// @param delta How much time passed, in seconds.
	// @return What it did.
	ParticleStatistics StepParticles(ecs::Store &store, float delta);
}
