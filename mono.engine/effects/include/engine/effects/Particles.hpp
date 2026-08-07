#pragma once

// What a particle emitter is, and what one particle is once it exists.
//
// **The scale constraint decides this file and every file under it.**
// `ROADMAP.md` v0.10 asks for 100,000 emitters at five particles each, which is
// half a million particles a frame, and that number rules out three arrangements
// that would otherwise be the obvious ones:
//
// - **A particle is not an entity.** Half a million rows whose whole life is one
//   create and one destroy is the pair of operations an archetype store is worst
//   at. `physics/AGENTS.md` makes the neighbouring argument for why a sleeping
//   body moves archetype rather than carrying a flag; this is the same reasoning
//   pointed the other way. The *emitter* is the entity and the particles are
//   pooled.
// - **A particle is not a `scene::DrawInstance`.** That type is eighty bytes and
//   carries two `core::Name`s, a full `CFrame` and an `AlphaMode` — none of which
//   varies between two particles from one emitter. Half a million of them is
//   forty megabytes written per frame to say the same thing repeatedly.
// - **A particle does not read its emitter's curves directly.** A
//   `core::NumberSequence` is 248 bytes and a `core::ColorSequence` is 408, so
//   the four an emitter carries are about a kilobyte — and evaluating one is a
//   scan over its keypoints. `ParticleSystem.hpp` samples them into a fixed
//   sixteen-entry table once per emitter per change, and the step reads that.
//
// So the split is: **this file holds what an author writes, and what a GPU
// draws.** The pool and the step are `ParticleSystem.hpp`, and the thing that
// connects them is a `uint16_t` slot number on each particle.
//
// **The property surface is Roblox's, in full, minus what is not implemented.**
// `ROADMAP.md` says "support everything you can", and the discipline
// `scene::SurfaceAppearance` states is what keeps that honest: a field that
// nothing reads is a field an author would reasonably assume worked. Every
// member below is consumed by `StepParticles` or by the render pass. What Roblox
// has and this does not — particle collision, `WindAffectsDrag` — is absent
// rather than declared and ignored, and `docs/` says so.
//
// @tier L8 · shared

#include <engine/core/Name.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/core/types/NumberRange.hpp>
#include <engine/core/types/Sequence.hpp>
#include <engine/core/types/Vector2.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/scene/Enums.hpp>

#include <cstdint>

namespace engine::effects {

	// How a particle is turned to face the viewer.
	//
	// **A closed list whose ordinals are the storage**, so a member may be
	// appended and never reordered — `scene::NormalId`'s rule, and for the same
	// reason: this is stored as its ordinal in a trivially-copied component, so
	// the number is the format.
	//
	// @since v0.10
	enum class ParticleOrientation : uint8_t {
		// The quad always faces the camera, spun by the particle's own rotation.
		// The ordinary case and the cheapest — two camera axes and a rotation.
		FacingCamera = 0,

		// Faces the camera but keeps world up as the quad's up, so a column of
		// smoke does not roll when the camera does.
		FacingCameraWorldUp = 1,

		// The quad's long axis lies along the particle's velocity. What a spark
		// or a rain streak wants.
		VelocityParallel = 2,

		// The quad's long axis is across the velocity. What a shockwave ring
		// wants.
		VelocityPerpendicular = 3,
	};

	// Where in the emitter's volume a particle is born.
	//
	// @since v0.10
	enum class ParticleShape : uint8_t {
		// The parent part's own box.
		Box = 0,

		// A sphere inscribed in it.
		Sphere = 1,

		// A cylinder about the local Y axis.
		Cylinder = 2,

		// A flat disc in the local XZ plane.
		Disc = 3,
	};

	// Whether particles are born throughout a shape or only on its skin.
	//
	// @since v0.10
	enum class ParticleShapeStyle : uint8_t {
		// Anywhere inside.
		Volume = 0,

		// On the boundary only, which is what a ring or a shell wants.
		Surface = 1,
	};

	// Which way a shape's particles travel.
	//
	// @since v0.10
	enum class ParticleShapeDirection : uint8_t {
		// Away from the centre. The ordinary case.
		Outward = 0,

		// Towards it, which is what an implosion is.
		Inward = 1,

		// Half each, decided per particle by its own seed.
		InAndOut = 2,
	};

	// How a texture's cells are laid out, when it is a flipbook.
	//
	// **The grid is square and a power of two on each side**, which is Roblox's
	// set and is also what makes the cell arithmetic two shifts rather than two
	// divides — the flipbook is evaluated per particle per frame, so the
	// difference is half a million divides.
	//
	// @since v0.10
	enum class FlipbookLayout : uint8_t {
		// Not a flipbook. The whole texture is one frame.
		None = 0,

		// Four cells.
		Grid2x2 = 1,

		// Sixteen.
		Grid4x4 = 2,

		// Sixty-four.
		Grid8x8 = 3,
	};

	// How many cells a layout has on each side.
	//
	// **`constexpr` and a shift rather than a table**, because this is called
	// per particle per frame inside the step.
	//
	// @param layout The layout.
	// @return 1, 2, 4 or 8.
	constexpr uint32_t FlipbookSide(FlipbookLayout layout) {
		return layout == FlipbookLayout::None ? 1u : 1u << static_cast<uint32_t>(layout);
	}

	// How many cells a layout has in total.
	//
	// @param layout The layout.
	// @return 1, 4, 16 or 64.
	constexpr uint32_t FlipbookCells(FlipbookLayout layout) {
		const uint32_t side = FlipbookSide(layout);
		return side * side;
	}

	// What a flipbook does when it reaches its last cell.
	//
	// @since v0.10
	enum class FlipbookMode : uint8_t {
		// Starts again. The ordinary case for a looping effect.
		Loop = 0,

		// Plays once over the particle's whole lifetime and holds the last cell.
		//
		// **The default, and it is the one that reads right for an explosion**: a
		// puff of smoke that restarted its animation halfway through its life
		// would flicker, and a flipbook stretched over exactly one lifetime is
		// what almost every authored sheet is drawn for.
		OneShot = 1,

		// Runs forwards and back.
		PingPong = 2,

		// Each particle holds one cell, chosen by its own seed. What a sheet of
		// *variations* is for rather than a sheet of *frames* — a page of eight
		// different leaves, not one leaf turning.
		Random = 3,
	};

	// Everything an author says about an emitter.
	//
	// **Big, and deliberately so.** At roughly a kilobyte and a half this is the
	// widest component in the engine by an order of magnitude, and the reason it
	// is acceptable is that **nothing on the per-frame path reads it**. The step
	// reads a sampled table — see `ParticleSystem.hpp` — and this is touched when
	// a particle is *born* and when the sampled table is rebuilt, which is when a
	// property changes.
	//
	// That is the whole argument, so it is worth naming what would break it: a
	// system that walked this column every frame at a hundred thousand rows would
	// be moving a hundred and fifty megabytes a frame to read a handful of floats.
	// If one appears, the fix is to move the fields it wants into the sampled
	// table rather than to make the walk faster.
	//
	// Widest-first, with the flags last and named padding, so the object
	// representation a snapshot writes holds no uninitialised bytes.
	//
	// @since v0.10
	struct ParticleEmitter {
		// How big a particle is over its life, as a multiple of one metre.
		//
		// **A sequence rather than a number, and this is the field that made
		// `ecs::PropertyType` grow.** Almost every authored effect scales in or
		// out over its lifetime, and a constant-size particle system is one
		// nobody can make look like smoke.
		core::NumberSequence Size{1.0f};

		// How see-through it is over its life, 0 solid to 1 gone.
		core::NumberSequence Transparency{0.0f};

		// How much taller than wide it is over its life.
		//
		// Roblox's `Squash`: positive stretches along the quad's local Y and
		// negative along X. Zero is square, which is why the default sequence is
		// the constant zero rather than the constant one.
		core::NumberSequence Squash{0.0f};

		// What colour it is over its life, multiplied by the texture.
		core::ColorSequence Colour{core::Color3{1.0f, 1.0f, 1.0f}};

		// How long a particle lives, in seconds. Sampled per particle.
		//
		// **A range and not two floats**, which is what `PropertyType::NumberRange`
		// exists for: two properties means two writes and a frame in between where
		// the minimum exceeds the maximum, and an emitter in that state emits
		// particles with a negative lifetime.
		core::NumberRange Lifetime{5.0f, 10.0f};

		// How fast a particle leaves, in metres per second.
		core::NumberRange Speed{5.0f, 5.0f};

		// What angle it is born at, in degrees.
		core::NumberRange Rotation{0.0f, 0.0f};

		// How fast it spins, in degrees per second.
		core::NumberRange RotationSpeed{0.0f, 0.0f};

		// How fast a flipbook plays, in cells per second.
		//
		// **Ignored under `FlipbookMode::OneShot`**, which is the default and
		// which paces itself off the particle's lifetime instead. That is Roblox's
		// arrangement and it is the right one: a one-shot sheet is authored to
		// cover a life, so a frame rate would be a second, contradictory way to
		// say how long it takes.
		core::NumberRange FlipbookFramerate{12.0f, 12.0f};

		// A constant push, in metres per second squared. Gravity, wind, a
		// draught.
		core::Vector3 Acceleration;

		// How wide the emission cone is, in degrees on each of two axes.
		//
		// Two numbers rather than one because a fire is a narrow cone and a
		// waterfall is a wide flat one, and one angle cannot say that.
		core::Vector2 SpreadAngle;

		// Which texture a particle is drawn with.
		//
		// A name, for `scene::Visual::Mesh`'s reason: a texture reference has to
		// survive a save file and a wire. An invalid name draws an untextured
		// quad, which is a visible flat square rather than nothing — the missing
		// texture rule.
		core::Name Texture;

		// How many particles a second, while `Enabled`.
		float Rate = 20.0f;

		// How fast speed is shed, as a fraction per second.
		float Drag = 0.0f;

		// How much of the parent's own velocity a new particle keeps, 0 to 1.
		float VelocityInheritance = 0.0f;

		// How self-lit the particle is, 0 to 1.
		//
		// At 1 the colour is emitted rather than lit, which is what a spark is.
		float LightEmission = 0.0f;

		// How much the world's lighting affects it, 0 to 1.
		float LightInfluence = 0.0f;

		// A flat multiplier on the emitted colour, above what the sequence says.
		float Brightness = 1.0f;

		// How much of the shape emits, 0 for the whole of it.
		//
		// Roblox's `ShapePartial`: at 0.5 only half the volume or surface is used,
		// which is how a half-ring or a hemisphere is authored without a second
		// shape.
		float ShapePartial = 0.0f;

		// How far towards the camera a particle is nudged, in metres.
		//
		// **A depth-sorting aid rather than a position**, which is why it is here
		// and not folded into the spawn point: it moves the particle in *view*
		// space at draw time, so an effect can be pulled in front of the geometry
		// it is attached to without being moved in the world where its physics
		// happen.
		float ZOffset = 0.0f;

		// How fast the emitter's own clock runs, with 1 being real time.
		float TimeScale = 1.0f;

		// Which face of the parent part particles leave from.
		scene::NormalId EmissionDirection = scene::NormalId::Top;

		// How a particle is turned to face the viewer.
		ParticleOrientation Orientation = ParticleOrientation::FacingCamera;

		// Where in the parent's volume a particle is born.
		ParticleShape Shape = ParticleShape::Box;

		// Throughout that volume, or only on its skin.
		ParticleShapeStyle ShapeStyle = ParticleShapeStyle::Volume;

		// Which way the shape sends them.
		ParticleShapeDirection ShapeDirection = ParticleShapeDirection::Outward;

		// How the texture's cells are laid out.
		FlipbookLayout Flipbook = FlipbookLayout::None;

		// What the flipbook does at its last cell.
		FlipbookMode FlipbookPlayback = FlipbookMode::OneShot;

		// How many of the grid's cells hold a frame, or 0 for all of them.
		//
		// **Not Roblox's, and a real GIF is why it exists.** Roblox's flipbooks
		// are authored sheets that fill their grid, so the cell count and the
		// frame count are the same number there. A GIF has whatever number of
		// frames the animation has — `fox_dance.gif` has 24 — and the grid is the
		// next square power of two that fits, which is 8x8. Playing all 64 cells
		// would spend five eighths of every particle's life showing nothing, and
		// the symptom is an effect that flashes on and vanishes rather than one
		// that animates.
		//
		// **Zero means the whole grid**, so an authored sheet needs nothing said
		// and only an import that knows better has to say it.
		//
		// A `uint8_t` because the ceiling is 64 — the widest grid this engine
		// draws. It sits here rather than beside `Flipbook` because it fits the
		// padding after the flags, which is what named padding is for.
		uint8_t FlipbookFrames = 0;

		// Whether every particle starts on a cell of its own choosing.
		bool FlipbookStartRandom = false;

		// Whether particles move with the parent after they are born.
		//
		// **Off by default, and the default matters more here than usual.** A
		// locked particle is one whose position is recomputed from the parent
		// every frame, so a trail of smoke behind a moving rocket becomes a rigid
		// column stuck to it — which is right for an engine glow and wrong for
		// almost everything else.
		bool LockedToPart = false;

		// Whether the emitter is producing particles.
		//
		// **Disabling does not kill what is already alive**, which is Roblox's
		// behaviour and the only one that is usable: an explosion is `Enabled =
		// true` for a frame and then false, and a version that cleared the pool
		// would make that emit nothing anybody ever saw.
		bool Enabled = true;

		// Whether a particle's colour is added to the target rather than blended
		// into it.
		//
		// **Not a Roblox property, and it is here because it is the one thing
		// their `LightEmission` conflates.** Additive blending is
		// order-independent — which is why every particle system in the world
		// offers it — so an additive emitter's particles need no back-to-front
		// sort at all. At half a million particles that is the difference between
		// a sort and no sort, and it is far too large a difference to leave
		// implied by a float.
		bool Additive = false;
	};

	// One particle, in the layout a vertex shader reads.
	//
	// **Twenty-eight bytes, and every field is one that varies between two
	// particles of one emitter.** Anything shared — the texture, the blend mode,
	// the orientation rule — is on the emitter and reached through `Slot`, which
	// is what keeps this small enough to write half a million of per frame.
	//
	// Trivially copyable and flat, for `scene::DrawInstance`'s reason: the day a
	// world is a process this has to survive being memcpy'd.
	//
	// @since v0.10
	struct ParticleInstance {
		// Where it is, in world space.
		//
		// **World space even for a locked emitter.** `LockedToPart` decides how
		// this is *computed* and not what it means; a consumer that had to know
		// which of two spaces a particle was in would be a consumer that branches
		// per particle.
		core::Vector3 Position;

		// How wide it is, in metres, with `Squash` already applied to the height.
		//
		// Packed as two halves of a `uint32_t` rather than two floats, because
		// four bytes here is sixteen megabytes across the target count. Both are
		// unsigned normalised over a sixty-four metre ceiling, which is past any
		// particle anybody authors and gives about a millimetre of resolution.
		uint32_t Size = 0;

		// Its spin about the view axis, and its flipbook cell.
		//
		// Rotation in the low sixteen bits as a turn over 65,536, which is a
		// hundredth of a degree — finer than a screen can show. The cell in the
		// high sixteen, which is sixty-four values used out of the range.
		uint32_t RotationAndCell = 0;

		// Its colour and alpha, as RGBA8.
		//
		// **Eight bits a channel, and that is not a compromise here.** The value
		// is a `Color3` sampled off a sequence and multiplied by a brightness, and
		// it lands in an 8-bit-per-channel target through a blend. Carrying floats
		// would quadruple the field to preserve precision that the framebuffer
		// discards.
		uint32_t Colour = 0xFFFFFFFF;

		// Which emitter this came from, as an index into the system's blocks.
		//
		// **The whole reason this type is thirty-two bytes.** Texture, blend mode,
		// orientation rule, Z offset and light response are all per emitter, and
		// this is how a particle names them without carrying them. Sixteen bits
		// caps a world at 65,536 live emitters, which is under the roadmap's
		// hundred thousand — `ParticleSystem.hpp` says what happens at the cap and
		// why the number is what it is.
		uint16_t Slot = 0;

		// Explicit padding, so the object representation carries no uninitialised
		// bytes into a recording. Two bytes, which is the type's alignment tail
		// rather than an interior hole — and it is the only slack there is, so the
		// next field anybody wants here costs four bytes across half a million
		// particles rather than none.
		uint16_t Reserved = 0;
	};

	// **Twenty-eight and not thirty-two, which is worth pinning rather than
	// leaving to whatever the compiler laid out.** Rounding up to thirty-two was
	// considered — it is the friendlier stride for a GPU fetch and it would leave
	// four bytes of room — and the arithmetic settles it: four bytes across half a
	// million particles is two megabytes uploaded every frame to carry nothing.
	// Twenty-eight is four-byte aligned, which is all a vertex attribute stream
	// asks for.
	//
	// **A failure here is a decision rather than a formality.** Anything that
	// changes this number changes the per-frame upload by half a megabyte per
	// added byte, so it should arrive with its reason written beside it.
	static_assert(sizeof(ParticleInstance) == 28, "the instance stream's size is the design");

	// Packs a width and a height into `ParticleInstance::Size`.
	//
	// @param width  Metres across.
	// @param height Metres tall.
	// @return The packed pair.
	uint32_t PackParticleSize(float width, float height);

	// Unpacks a width from `ParticleInstance::Size`.
	//
	// @param packed The packed pair.
	// @return Metres across.
	float UnpackParticleWidth(uint32_t packed);

	// Unpacks a height from `ParticleInstance::Size`.
	//
	// @param packed The packed pair.
	// @return Metres tall.
	float UnpackParticleHeight(uint32_t packed);

	// The largest particle this packing can describe, in metres.
	//
	// Sixty-four, and a particle asked to be larger is clamped rather than
	// wrapped — a wrapped size is a sixty-five-metre puff of smoke drawn one
	// millimetre across, which reads as a particle that vanished.
	inline constexpr float MAX_PARTICLE_SIZE = 64.0f;
}
