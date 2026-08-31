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
	// The roadmap's hundred thousand simultaneous emitters, with headroom for
	// authored scenes above the benchmark. The particle stream already had a
	// four-byte slot word, previously split into a uint16 and padding, so this
	// limit costs no additional per-particle storage.
	//
	// **Running out is logged once and then silent**, because a message per
	// emitter per frame at this count is a log nobody can read. `Statistics::
	// EmittersRefused` is the number to look at.
	inline constexpr uint32_t MAX_EMITTER_SLOTS = 1000000;

	// What no emitter's slot is.
	inline constexpr uint32_t NO_SLOT = 0xFFFFFFFFu;

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
		// One-shot births requested by `ParticleEmitter:Emit`. Kept on the ECS
		// row so a script call and the simulation do not own two queues.
		uint32_t Requested = 0;

		// Which block, or `NO_SLOT` when this emitter has none.
		//
		// **Not serialised as a meaningful value** - see `Registration.cpp`. A
		// block index is a position in one process's pool, which is rule 4's
		// hazard exactly: restoring it would point an emitter at whatever block
		// happened to take that number.
		uint32_t Index = NO_SLOT;

		// The state the refresh walk needs even before this emitter owns a block,
		// sampled from the much wider authored component when it changes.
		//@{
		bool Enabled = true;
		bool Configured = false;
		//@}

		// Whether the next refresh invalidates every particle in the block.
		bool ClearRequested = false;

		// The last block claim failed and should not be retried until some block
		// returns capacity or the authored emitter changes.
		//
		// This occupies the byte that was explicit padding, so remembering a
		// refusal does not widen the row walked for every emitter every tick.
		bool Refused = false;
	};
	static_assert(sizeof(EmitterSlot) == 12);

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

		// How fast speed is shed, as a fraction per second.
		float Drag = 0.0f;

		// The authored capacity ceiling that sized this block. A change releases
		// and reclaims the run so raising the ceiling can actually add rows.
		int32_t ParticleLimit = 0;

		// Distance births read on the steady refresh path. Sampled here so that
		// moving a parent does not pull the authored emitter row into cache.
		float RateOverDistance = 0.0f;

		// Device-stepped forces and velocity ceiling. These are one value per
		// emitter, not fields repeated on every particle.
		//@{
		float MaxSpeed = 0.0f;
		float NoiseStrength = 0.0f;
		float NoiseFrequency = 0.5f;
		float NoiseScrollSpeed = 0.0f;
		float RadialAcceleration = 0.0f;
		float TangentialAcceleration = 0.0f;
		//@}

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

		// How many times the device-visible half of this block has changed.
		//
		// **Because a block record is hundreds of bytes and a
		// scene may have a hundred thousand of them.** Staging every block every
		// frame is thirty-nine megabytes of writes into a mapped transfer buffer -
		// more than the sixteen the whole particle pool used to cost, which would
		// make the device-resident pool a loss at the scale it exists for.
		//
		// Almost none of it changes. The curves change when a script writes the
		// emitter, the frame changes when the emitter moves, and everything else
		// changes when the block is claimed. So the renderer keeps its own copy
		// of the numbers it last uploaded, indexed by block, and re-sends one only
		// when this disagrees - which for a static scene is never.
		//
		// **Two counters and not one**, because the two halves change on
		// different occasions: a moving emitter rewrites its frame every tick and
		// its curves never, and one counter would re-send the two hundred and
		// fifty-six bytes of curves along with the forty-four bytes that moved.
		//@{
		uint32_t Revision = 1;
		uint32_t CurveRevision = 1;
		//@}

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

		// Which refresh last saw the emitter that owns this block.
		//
		// A generation stamp avoids a separate strided write over all blocks just
		// to clear one byte before the claim walk sets it again. A block whose stamp
		// differs from `ParticleSystem::ClaimGeneration` was not claimed and is
		// reclaimed, which preserves the destroyed-emitter check without the pass.
		uint32_t ClaimedAt = 0;

		// How fast a flipbook runs, in cells per second, under a mode that pays
		// attention to it.
		float FlipbookRate = 12.0f;
	};

	// The compact mutable row consumed by host fallback or copied to device state.
	//
	// Kept beside `EmitterBlock`, indexed identically. The block is hundreds of
	// bytes of curves, transforms and device parameters, while a simulation tick
	// needs only this row until an emitter actually owes a birth. Splitting the
	// mutable counters keeps the hundred-thousand-emitter steady walk contiguous.
	struct EmitterRuntime {
		// How many particles are alive. The host keeps a prefix; a device-owned
		// block keeps the number ever placed in its ring, capped to capacity.
		uint32_t Live = 0;

		// The monotonic per-emitter seed index. A slot is reused, so using it would
		// make replacement particles repeat the ones they replace.
		uint32_t Spawned = 0;

		// One-shot births waiting for an already resident block.
		uint32_t Requested = 0;

		// Fractional continuous-emission debt.
		float Pending = 0.0f;

		// Time since the last birth and the maximum time one can remain alive.
		//@{
		float Idle = 0.0f;
		float Longest = 0.0f;
		//@}

		// Zero while disabled, otherwise authored rate times time scale.
		float ContinuousRate = 0.0f;

		// The guaranteed first birth chooses a deterministic recurring phase after
		// it is planned, preserving immediate start without synchronising a crowd.
		//@{
		bool RatePhasePending = true;
		bool Enabled = true;
		//@}

		// A disabled device emitter remains resident until its longest possible
		// particle has expired. Only those rare blocks are revisited by the host;
		// steady active emitters are advanced entirely by the GPU.
		bool DeviceRetiring = false;
	};

	// The compact authored state needed only when a particle is born.
	//
	// Kept beside `EmitterBlock`, indexed identically, because adding these fields
	// to the block slowed the ageing and refresh passes that stream blocks but do
	// not spawn. Refreshing this row only when the emitter or its parent changes
	// removes the steady 1.5 KiB `ParticleEmitter` walk from `StepParticles` while
	// preserving the small hot block used by both host and device stepping.
	struct EmitterSpawnState {
		// Local emitter extent, emission direction, and inherited parent velocity.
		//@{
		core::Vector3 Half;
		core::Vector3 Emission;
		core::Vector3 Inherited;
		//@}

		// Random ranges sampled once at birth.
		//@{
		core::NumberRange Speed;
		core::NumberRange Lifetime;
		core::NumberRange RotationSpeed;
		core::NumberRange Rotation;
		//@}

		// Authored directional and shape controls.
		//@{
		float SpreadX = 0.0f;
		float SpreadY = 0.0f;
		float ShapePartial = 0.0f;
		float VelocityInheritance = 0.0f;

		ParticleShape Shape = ParticleShape::Box;
		ParticleShapeStyle ShapeStyle = ParticleShapeStyle::Volume;
		ParticleShapeDirection ShapeDirection = ParticleShapeDirection::Outward;
		//@}
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

		// How many block allocations were actually attempted this refresh.
		//
		// A full pool keeps refused emitters visible in `EmittersRefused`, but it
		// must not retry every one every frame. This counter distinguishes those
		// two states in tests and diagnostics.
		uint32_t EmitterClaimAttempts = 0;

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
		//
		// **Empty when `DeviceStepped`**, along with `States`: the device owns the
		// pool and nothing on this side ever reads a particle, so allocating
		// either would be fifty-four megabytes at the default capacity that
		// nothing ever looks at. `StepParticles` releases them on its first
		// device-stepped tick.
		std::vector<ParticleInstance> Instances;

		// What the step reads and writes.
		std::vector<ParticleState> States;

		// One per live emitter.
		std::vector<EmitterBlock> Blocks;

		// Spawn-only rows, indexed exactly as `Blocks`.
		std::vector<EmitterSpawnState> SpawnStates;

		// Mutable rate and ring counters, indexed exactly as `Blocks`.
		std::vector<EmitterRuntime> RuntimeStates;

		// Indices of disabled device blocks waiting for their last possible
		// particle to expire. This keeps retirement proportional to emitters that
		// are actually stopping instead of restoring an all-emitter CPU step.
		std::vector<uint32_t> RetiringBlocks;

		// The immediate parent used to resolve each block's cached frame.
		//
		// Kept beside rather than inside `EmitterBlock`: the device and particle
		// step never read it, and growing their hot row made the earlier cached
		// spawn-plan experiment slower. The index is the block index.
		std::vector<ecs::Entity> FrameParents;

		// The texture catalogue revision already reflected in `Blocks`.
		uint64_t TextureRevision = 0;

		// Observed ECS epochs already folded into the resident emitter rows.
		// Keeping the six exact component epochs makes the steady refresh path
		// independent of the number of quiet entities carrying those components.
		//@{
		uint64_t EmitterChangeVersion = 0;
		uint64_t TransformChangeVersion = 0;
		uint64_t AttachmentChangeVersion = 0;
		uint64_t BoundsChangeVersion = 0;
		uint64_t MotionChangeVersion = 0;
		uint64_t HierarchyChangeVersion = 0;
		//@}

		// Caller-owned activation policy revision already applied to resident
		// blocks. The predicate itself lives only for one refresh call and never
		// crosses the world boundary.
		uint64_t ActivationPolicyRevision = 0;

		// Which completed simulation revision the presentation data describes.
		//
		// **One counter for the whole pool, because rendering needs one answer to
		// "did any batch input change?"** `EmitterBlock` revisions remain the
		// narrow device-table dirtiness checks. This one lets a renderer that runs
		// faster than simulation reuse its ordered emitter list without walking
		// every emitter again between ticks.
		uint64_t PresentationRevision = 1;

		// Which emitter membership and material layout presentation has to order.
		//
		// Simulation advances every tick, while this advances only when a batch is
		// added, removed, or changes draw state. Keeping the two separate lets a
		// resident renderer upload changed block parameters without
		// sorting and rewriting the complete emitter draw table.
		uint64_t LayoutRevision = 1;

		// Which device parameter or curve-table content Blocks describes.
		//
		// Particle ages and emission do not change these tables. A static emitter can
		// therefore advance for any number of ticks without making presentation
		// scan every block to rediscover that all per-block revisions still match.
		uint64_t ResidentRevision = 1;

		// The current block-claim generation. Zero remains the unclaimed marker.
		uint32_t ClaimGeneration = 0;

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
		// and after it reached the fixed row cap refused to emit anything again
		// for the rest of the process. None of that is visible in a scene that builds its
		// emitters once, which is every scene in `examples/`.
		//
		// Indices rather than pointers, because `Blocks` moves when it grows.
		// Refilled by the reclaim sweep and drained by the claim walk, which run
		// in that order in one `RefreshEmitters` - so a row freed this tick is
		// reused on the next one rather than under the walk that freed it.
		std::vector<uint32_t> FreeSlots;

		// A returned particle range or block row makes refused emitters eligible
		// for one new claim pass. It is consumed at the start of that pass.
		bool RetryRefused = false;

		// Whether an explicit emitter operation needs the claim pass.
		//
		// Authored and hierarchy changes have ECS dirty channels of their own.
		// `Emit` and `Clear` write this resource so a steady scene can skip the
		// emitter column without losing operations queued on a row with no block.
		bool RefreshRequested = true;

		// The emitter-row count observed by the last full claim pass.
		//
		// `CountMatching` is an archetype count rather than a row walk. A changed
		// count wakes reclamation for destroyed emitters while the common unchanged
		// case remains independent of emitter count.
		size_t EmitterRows = 0;

		// Whether the device owns the pool and its complete lifecycle.
		//
		// **Set by whoever has a renderer, and the client always does.** When it
		// is on, `StepParticles` skips its ageing pass entirely: the device
		// emits, integrates, shades and draws from the resident particle buffers.
		// Nothing crosses the bus unless an emitter parameter changes. That
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

		// How many slots the pool holds in total.
		//
		// May grow as far as `MaximumCapacity`. Blocks carry indices rather than
		// pointers, so reallocating the host fallback arrays does not invalidate
		// them. A device-owned pool observes the new capacity through presentation
		// and replaces its resident buffers once.
		uint32_t Capacity = 0;

		// The hard ceiling for capacity growth.
		//
		// Equal to `Capacity` for a fixed pool. A rendered client may leave room to
		// grow so a scene pays for the rows it actually claims instead of either
		// reserving the worst case in every loaded world or silently losing effects.
		uint32_t MaximumCapacity = 0;

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

	// Gives a world a pool with an initial size and an optional growth ceiling.
	//
	// Omitting `maximumCapacity` makes a fixed pool, which keeps headless worlds
	// and tests from acquiring an implicit memory policy. A larger ceiling lets
	// the allocator grow geometrically when a block claim first crosses the
	// current capacity.
	//
	// @param store           The world.
	// @param capacity        How many particle rows to allocate initially.
	// @param maximumCapacity The hard row ceiling, or zero to remain fixed.
	void InstallParticles(ecs::Store &store, uint32_t capacity, uint32_t maximumCapacity = 0);

	// Queues a one-shot emission on an emitter, including one that is disabled.
	//
	// @param store   The world.
	// @param emitter The ParticleEmitter instance.
	// @param count   How many particles to request.
	// @return False when the entity is not a ParticleEmitter.
	bool EmitParticles(ecs::Store &store, ecs::Entity emitter, uint32_t count);

	// Clears every live particle owned by an emitter on the next refresh.
	//
	// @param store   The world.
	// @param emitter The ParticleEmitter instance.
	// @return False when the entity is not a ParticleEmitter.
	bool ClearParticles(ecs::Store &store, ecs::Entity emitter);

	// Optional host policy deciding whether one emitter contributes to a snapshot.
	using EmitterActivationPredicate = bool (*)(const ecs::Store &, ecs::Entity);

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
	// @param store              The world.
	// @param activation         Optional caller policy for whether an emitter may own a block.
	// @param activationRevision Revision of that caller policy; changing it forces reevaluation.
	// @return How many blocks are live.
	size_t RefreshEmitters(
		ecs::Store &store, EmitterActivationPredicate activation = nullptr, uint64_t activationRevision = 0
	);

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
