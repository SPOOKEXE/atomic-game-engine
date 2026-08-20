#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/effects/ParticleSystem.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/scene/Attachments.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/TextureCatalogue.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace engine::effects {

	namespace {
		using core::CFrame;
		using core::Vector3;

		constexpr float RADIANS_PER_DEGREE = std::numbers::pi_v<float> / 180.0f;
		constexpr float TURNS_PER_DEGREE = 1.0f / 360.0f;

		// --- randomness ------------------------------------------------------
		//
		// **Not `core::Random`, and that is a measured trade rather than a
		// shortcut.** `core::Random::Float` is the first thirty-two bits of a
		// SHA-256 over its two arguments, which is the right answer for a value
		// that must be identical on every machine down to the last bit and is
		// enormously the wrong one here: a spawning particle draws about ten
		// numbers, and a scene emitting twenty thousand a second would be running
		// two hundred thousand SHA-256 blocks a second to decide where puffs of
		// smoke go.
		//
		// **What is kept is the property that actually matters**, which is not
		// cryptographic quality - it is that the value is a pure function of
		// `(emitter, particle index, tick)` and involves no state. So two runs of
		// one scene emit the same particles, a recording replays, and a worker
		// needs no generator of its own to carry between ranges. That is the same
		// contract `core::Random` offers; this reaches it with a different
		// primitive because the cost profile is different.
		//
		// SplitMix64's finaliser, which is a published constant sequence rather
		// than three numbers somebody picked - `AGENTS.md` rule 6 applies to
		// magic constants as much as to invariants.
		uint32_t Mix(uint64_t seed) {
			seed += 0x9E3779B97F4A7C15ull;
			uint64_t z = seed;
			z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
			z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
			return static_cast<uint32_t>((z ^ (z >> 31)) >> 32);
		}

		// A float in [0, 1), by the same construction `core::Random::Float` uses:
		// a 24-bit integer scaled by a power of two, which IEEE-754 represents
		// exactly. No compiler disagrees about the last bit.
		float Unit(uint64_t seed) {
			return static_cast<float>(Mix(seed) >> 8) * (1.0f / 16777216.0f);
		}

		float Between(uint64_t seed, const core::NumberRange &range) {
			return range.Minimum + Unit(seed) * (range.Maximum - range.Minimum);
		}

		// One seed per particle per purpose.
		//
		// The purpose is folded in rather than the seed being advanced, because
		// advancing is state and state is what a stateless draw exists to avoid.
		uint64_t SeedOf(uint32_t emitter, uint32_t particle, uint32_t purpose) {
			return (static_cast<uint64_t>(emitter) << 40) ^ (static_cast<uint64_t>(particle) << 8) ^ purpose;
		}

		// --- colour ----------------------------------------------------------

		uint32_t PackRgb(const core::Color3 &colour, float brightness) {
			const auto channel = [brightness](float value) {
				return static_cast<uint32_t>(std::clamp(value * brightness, 0.0f, 1.0f) * 255.0f + 0.5f);
			};
			return channel(colour.R) | (channel(colour.G) << 8) | (channel(colour.B) << 16);
		}

		uint32_t WithAlpha(uint32_t rgb, float alpha) {
			const auto byte = static_cast<uint32_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
			return rgb | (byte << 24);
		}

		// --- spawning --------------------------------------------------------

		// A point inside the emitter's shape, in the parent's local space.
		//
		// The half-extent is the parent's `Bounds`, so an emitter's volume is the
		// part it is on - which is Roblox's arrangement and is the one that makes
		// resizing a part resize its effect without touching the emitter.
		// Everything one emitter's births need, copied off its row.
		//
		// **The spawn pass was serial because it read `ParticleEmitter`**, which
		// is fifteen hundred bytes a row that a worker has no promise it may
		// walk. That is a fact about the *column*, not about the work: what a
		// birth actually needs is a shape, four ranges and a frame, and once
		// those are copied out nothing in the loop touches the store at all.
		//
		// So the pass is two now. A serial walk fills one of these per emitter
		// that owes a particle - which is the only thing that walk does, and it
		// costs 0.145 ms over five thousand emitters - and the births themselves
		// dispatch over the plans.
		//
		// **It is also the hoist.** The emission normal, the spread in radians
		// and the inherited velocity were resolved once per *particle* inside
		// the helpers; they are resolved once per emitter here, because that is
		// where they were always constant.
		//
		// **Determinism is unaffected, and that is not luck.** Every draw is
		// seeded from the emitter's own id and the block's own spawn counter -
		// `SeedOf` - so a particle's value depends on nothing outside its
		// emitter and the order emitters are visited in cannot reach it. Within
		// one plan the loop stays serial, because the counter advances.
		struct SpawnPlan {
			uint32_t Block = 0;
			uint32_t Id = 0;
			uint32_t Owed = 0;
			Vector3 Half;

			ParticleShape Shape = ParticleShape::Box;
			ParticleShapeStyle ShapeStyle = ParticleShapeStyle::Volume;
			ParticleShapeDirection ShapeDirection = ParticleShapeDirection::Outward;
			float ShapePartial = 0.0f;

			// Resolved rather than carried as an enum and two degrees: the
			// helpers wanted a normal and radians, and turning them into one was
			// per-particle work.
			Vector3 Emission;
			float SpreadX = 0.0f;
			float SpreadY = 0.0f;

			core::NumberRange Speed;
			core::NumberRange Lifetime;
			core::NumberRange RotationSpeed;
			core::NumberRange Rotation;

			// The parent's motion times `VelocityInheritance`, or zero. Resolved
			// here because it is a store lookup and the store is what the worker
			// may not touch.
			Vector3 Inherited;

			// Written by whichever worker takes this plan, summed afterwards.
			// Per plan rather than one shared counter, which is the reason the
			// block counters are per block.
			uint32_t Dropped = 0;
		};

		Vector3 SpawnPoint(const SpawnPlan &plan, const Vector3 &half, uint32_t id, uint32_t index) {
			const float x = Unit(SeedOf(id, index, 1)) * 2.0f - 1.0f;
			const float y = Unit(SeedOf(id, index, 2)) * 2.0f - 1.0f;
			const float z = Unit(SeedOf(id, index, 3)) * 2.0f - 1.0f;

			Vector3 point{x, y, z};
			switch (plan.Shape) {
			case ParticleShape::Sphere:
				// **Normalised then scaled by a cube root, not by a uniform
				// draw.** A radius drawn uniformly concentrates particles at the
				// centre, because the volume at radius r grows as r-cubed - a
				// "sphere" of smoke that is a dense ball with a thin halo. The
				// cube root is the inverse of that and costs one `cbrt` per
				// spawn, which is on the spawn path and not the step path.
				point = point.Unit();
				if (plan.ShapeStyle == ParticleShapeStyle::Volume) {
					point = point * std::cbrt(Unit(SeedOf(id, index, 4)));
				}
				point = point * half;
				break;
			case ParticleShape::Cylinder: {
				const float angle = Unit(SeedOf(id, index, 4)) * 2.0f * std::numbers::pi_v<float>;
				// Square root for the same reason the cube root is there: area
				// grows as r-squared.
				const float radius = plan.ShapeStyle == ParticleShapeStyle::Surface
										 ? 1.0f
										 : std::sqrt(Unit(SeedOf(id, index, 5)));
				point =
					Vector3{std::cos(angle) * radius * half.X, y * half.Y, std::sin(angle) * radius * half.Z};
				break;
			}
			case ParticleShape::Disc: {
				const float angle = Unit(SeedOf(id, index, 4)) * 2.0f * std::numbers::pi_v<float>;
				const float radius = plan.ShapeStyle == ParticleShapeStyle::Surface
										 ? 1.0f
										 : std::sqrt(Unit(SeedOf(id, index, 5)));
				point = Vector3{std::cos(angle) * radius * half.X, 0.0f, std::sin(angle) * radius * half.Z};
				break;
			}
			case ParticleShape::Box:
				if (plan.ShapeStyle == ParticleShapeStyle::Surface) {
					// Snap the largest component to the face it is nearest. Not
					// an area-uniform draw over six faces, which would need a
					// weighted pick; this is what Roblox's does and it reads the
					// same on a cube.
					const float ax = std::abs(point.X);
					const float ay = std::abs(point.Y);
					const float az = std::abs(point.Z);
					if (ax >= ay && ax >= az) {
						point.X = point.X < 0.0f ? -1.0f : 1.0f;
					} else if (ay >= az) {
						point.Y = point.Y < 0.0f ? -1.0f : 1.0f;
					} else {
						point.Z = point.Z < 0.0f ? -1.0f : 1.0f;
					}
				}
				point = point * half;
				break;
			}

			// `ShapePartial` keeps the near half of the shape's own axis, which is
			// how a half-ring or a hemisphere is authored without a second shape.
			if (plan.ShapePartial > 0.0f && Unit(SeedOf(id, index, 6)) < plan.ShapePartial) {
				point.Y = std::abs(point.Y);
			}
			return point;
		}

		// Which way a new particle goes, in the parent's local space.
		Vector3
		SpawnDirection(const SpawnPlan &plan, const Vector3 &localPoint, uint32_t id, uint32_t index) {
			Vector3 direction = plan.Emission;

			// A shaped emitter throws outward from its own centre rather than
			// along a face, which is what makes a sphere an explosion.
			if (plan.Shape != ParticleShape::Box) {
				// **Squared, because the question is only whether it is
				// non-zero.** `Vector3::Unit` already returns `Zero` for a
				// directionless vector, so this guard is asking "did that
				// succeed" - and asking it with `Magnitude` takes a second
				// square root of a vector whose length is now one. Squared
				// magnitude answers the same question with none, and `sqrt` is
				// monotonic so the two can never disagree.
				const Vector3 radial = localPoint.Unit();
				if (radial.MagnitudeSquared() > 0.0f) {
					direction = radial;
				}
			}

			if (plan.ShapeDirection == ParticleShapeDirection::Inward) {
				direction = -direction;
			} else if (plan.ShapeDirection == ParticleShapeDirection::InAndOut &&
					   Unit(SeedOf(id, index, 7)) < 0.5f) {
				direction = -direction;
			}

			// The spread is two independent tilts, so a wide flat cone - a
			// waterfall - is expressible where one angle could only give a circle.
			const float spreadX = plan.SpreadX;
			const float spreadY = plan.SpreadY;
			if (spreadX != 0.0f || spreadY != 0.0f) {
				const float tiltX = (Unit(SeedOf(id, index, 8)) * 2.0f - 1.0f) * spreadX;
				const float tiltY = (Unit(SeedOf(id, index, 9)) * 2.0f - 1.0f) * spreadY;
				// **Rotated rather than composed, and it is the same
				// arithmetic.** `CFrame{direction}` is a translation with no
				// turn in it, so `(A * B).Position` is `A.Rotation` applied to
				// `B.Position` plus `A.Position`, and `A.Position` is zero -
				// which is exactly what `VectorToWorldSpace` computes. The
				// composed form built a second `CFrame`, multiplied two frames
				// and threw the rotation half of the answer away, once per
				// particle born.
				direction = CFrame::Angles(tiltX, tiltY, 0.0f).VectorToWorldSpace(direction);
			}

			// The same pair, for the reason above: two square roots where the
			// second one is asking whether the first produced anything.
			const Vector3 unit = direction.Unit();
			return unit.MagnitudeSquared() > 0.0f ? unit : Vector3{0.0f, 1.0f, 0.0f};
		}

		// --- flipbook --------------------------------------------------------

		// How many of an emitter's grid cells hold a frame.
		//
		// Zero on the emitter means the whole grid, which is what an authored
		// sheet is; a GIF import says how many it actually decoded. Clamped to the
		// grid, because a count larger than the grid would index past the sheet.
		uint8_t ResolvedFrames(const ParticleEmitter &emitter, const scene::FlipbookFacts &facts) {
			const auto cells = static_cast<uint8_t>(FlipbookCells(emitter.Flipbook));
			if (emitter.FlipbookFrames != 0) {
				return std::min(emitter.FlipbookFrames, cells);
			}

			// **What the texture said, when the emitter did not.** A GIF states
			// how many of its grid's cells hold a frame and the bake carries the
			// number through - `scene::TextureCatalogue` - so a scene pointing an
			// emitter at one does not have to repeat it. The alternative is a
			// script with `FlipbookFrames = 24` in it, which is wrong the moment
			// somebody re-exports the animation with a frame added.
			if (facts.Frames != 0) {
				return std::min(facts.Frames, cells);
			}
			return cells;
		}

		// What a flipbook plays at when neither the emitter nor the texture says.
		//
		// Twelve, because that is what this engine used before either could.
		constexpr float DEFAULT_FLIPBOOK_RATE = 12.0f;

		// How fast the block's flipbook advances, in cells a second.
		//
		// **Three answers in priority order, and the middle one is the point.**
		// What the emitter says wins, because an author overriding a rate means
		// it; what the texture says is next, because a GIF states a delay per
		// frame and nothing else in the chain knows it; and twelve is the
		// fallback, which is what this engine used before there was anything to
		// ask.
		//
		// **`FlipbookMode::OneShot` ignores all three**, which is unchanged and
		// deliberate: a one-shot sheet paces itself off the particle's lifetime,
		// so a frame rate would be a second and contradictory way of saying how
		// long it takes - `ParticleEmitter::FlipbookFramerate` carries that
		// argument. The rate matters for `Loop` and `PingPong`, which is where a
		// source's authored fps actually belongs.
		float ResolvedRate(const ParticleEmitter &emitter, const scene::FlipbookFacts &facts) {
			if (emitter.FlipbookFramerate.Maximum > 0.0f) {
				return emitter.FlipbookFramerate.Maximum;
			}
			if (facts.FrameRate > 0.0f) {
				return facts.FrameRate;
			}
			return DEFAULT_FLIPBOOK_RATE;
		}

		// Everything a block takes from its emitter every refresh.
		//
		// **One function because there are two callers that must not drift** -
		// the block being reused and the block being created - and a field added
		// to one and not the other is a difference that only shows on the frame
		// an emitter is first enabled.
		void ApplyPlayback(const ecs::Store &store, const ParticleEmitter &emitter, EmitterBlock &block) {
			const scene::FlipbookFacts facts = scene::FlipbookOf(store, emitter.Texture);

			block.Acceleration = emitter.Acceleration;
			block.Drag = emitter.Drag;
			block.Locked = emitter.LockedToPart;
			block.Flipbook = emitter.Flipbook;
			block.FlipbookPlayback = emitter.FlipbookPlayback;
			block.FlipbookRate = ResolvedRate(emitter, facts);
			block.Frames = ResolvedFrames(emitter, facts);
		}

		uint32_t FlipbookCell(const EmitterBlock &block, float age, float lifetime, uint32_t seed) {
			// **What the *sheet* holds, not what the grid could.** A GIF has
			// whatever number of frames the animation has and the grid is the next
			// square power of two that fits, so playing every cell would spend the
			// difference showing nothing - see `ParticleEmitter::FlipbookFrames`.
			const uint32_t cells = std::min<uint32_t>(block.Frames, FlipbookCells(block.Flipbook));
			if (cells <= 1) {
				return 0;
			}

			switch (block.FlipbookPlayback) {
			case FlipbookMode::Random:
				return Mix(seed) % cells;
			case FlipbookMode::OneShot: {
				// Stretched over the whole life, which is what a one-shot sheet is
				// drawn for. Clamped to the last cell rather than wrapping, so a
				// particle that outlives its animation holds the final frame
				// instead of snapping back to the first.
				const float fraction = lifetime > 0.0f ? age / lifetime : 0.0f;
				const auto cell = static_cast<uint32_t>(fraction * static_cast<float>(cells));
				return std::min(cell, cells - 1);
			}
			case FlipbookMode::PingPong: {
				const auto step = static_cast<uint32_t>(age * block.FlipbookRate);
				const uint32_t span = cells * 2 - 2;
				const uint32_t at = span == 0 ? 0 : step % span;
				return at < cells ? at : span - at;
			}
			case FlipbookMode::Loop:
				break;
			}
			return static_cast<uint32_t>(age * block.FlipbookRate) % cells;
		}
	}

	// --- the sampled curves --------------------------------------------------

	void SampleCurves(const ParticleEmitter &emitter, ParticleCurves &out) {
		for (uint32_t index = 0; index < CURVE_SAMPLES; index++) {
			// **The last sample is at time 1 and not at 15/16**, which is off by
			// one in the direction that shows: a size curve ending at zero would
			// never reach zero, so every particle would pop out of existence at
			// its final width instead of shrinking away.
			const float age = static_cast<float>(index) / static_cast<float>(CURVE_SAMPLES - 1);

			out.Size[index] = emitter.Size.Evaluate(age);
			out.Alpha[index] = 1.0f - emitter.Transparency.Evaluate(age);
			out.Squash[index] = emitter.Squash.Evaluate(age);
			out.Colour[index] = PackRgb(emitter.Colour.Evaluate(age), emitter.Brightness);
		}
	}

	namespace {
		// Where one age lands in a sampled curve: the pair of samples either
		// side of it and how far between them it is.
		//
		// **One age, four curves, and it used to be resolved four times.** The
		// step loop asks `Size`, `Squash`, `Alpha` and `Colour` for the *same*
		// particle age, and each call clamped, scaled, truncated, took a min and
		// subtracted to find the same pair of indices and the same fraction.
		// Three quarters of that is arithmetic done to reach an answer already
		// in hand.
		//
		// It is a small thing in a release build, where the compiler can see
		// through four inlined calls and common up the work. It is not a small
		// thing at `-O0`, which is what `dev` builds first-party code at and
		// therefore what anybody profiling this engine is looking at: nothing is
		// inlined, so those are four real calls doing the same six operations on
		// their own stack frames, half a million times a tick.
		struct CurveCursor {
			uint32_t Low = 0;
			uint32_t High = 0;
			float Fraction = 0.0f;
		};

		CurveCursor CurveAt(float age) {
			const float scaled = std::clamp(age, 0.0f, 1.0f) * static_cast<float>(CURVE_SAMPLES - 1);

			CurveCursor cursor;
			cursor.Low = static_cast<uint32_t>(scaled);
			cursor.High = std::min(cursor.Low + 1, CURVE_SAMPLES - 1);
			cursor.Fraction = scaled - static_cast<float>(cursor.Low);
			return cursor;
		}

		float SampleCurve(const float (&samples)[CURVE_SAMPLES], const CurveCursor &cursor) {
			return samples[cursor.Low] + (samples[cursor.High] - samples[cursor.Low]) * cursor.Fraction;
		}

		uint32_t SampleColourCurve(const uint32_t (&samples)[CURVE_SAMPLES], const CurveCursor &cursor);
	}

	float SampleAt(const float (&samples)[CURVE_SAMPLES], float age) {
		return SampleCurve(samples, CurveAt(age));
	}

	uint32_t SampleColourAt(const uint32_t (&samples)[CURVE_SAMPLES], float age) {
		return SampleColourCurve(samples, CurveAt(age));
	}

	namespace {
		uint32_t SampleColourCurve(const uint32_t (&samples)[CURVE_SAMPLES], const CurveCursor &cursor) {
			const uint32_t low = cursor.Low;
			const uint32_t high = cursor.High;

			if (low == high || samples[low] == samples[high]) {
				return samples[low];
			}

			// **Integer lerp per channel, in the packed representation.** The
			// obvious alternative is to unpack to floats, lerp, and repack -
			// which is six divides and six multiplies for a value that lands in
			// eight bits. This is the same arithmetic in fixed point: an
			// eight-bit blend factor, three byte lerps, no conversion in either
			// direction.
			const auto factor = static_cast<uint32_t>(cursor.Fraction * 256.0f);
			uint32_t blended = 0;
			for (uint32_t shift = 0; shift < 24; shift += 8) {
				const uint32_t from = (samples[low] >> shift) & 0xFFu;
				const uint32_t to = (samples[high] >> shift) & 0xFFu;
				blended |= (((from * (256 - factor)) + (to * factor)) >> 8) << shift;
			}
			return blended;
		}
	}

	core::CFrame EmitterFrame(const ecs::Store &store, ecs::Entity emitter) {
		const ecs::Entity parent = store.ParentOf(emitter);
		if (parent == ecs::NULL_ENTITY) {
			return {};
		}

		// The attachment first, because an emitter on an attachment is the
		// arrangement every authored effect uses - a rocket's exhaust hangs off a
		// point on the rocket, not off the rocket's centre.
		if (const scene::Attachment *point = store.Get<scene::Attachment>(parent)) {
			return point->WorldFrame;
		}
		if (const scene::Transform *placement = store.Get<scene::Transform>(parent)) {
			return placement->Frame;
		}
		return {};
	}

	namespace {
		// What the emitter's parent is doing, for `VelocityInheritance`.
		//
		// **Two steps rather than one, because an emitter on an attachment is
		// two parents from the part that moves.** Everywhere else in this file one
		// step is enough - a frame and a half-extent both live on the immediate
		// parent, and an attachment carries its own. Motion does not: an
		// attachment has no velocity of its own and never will, so stopping at it
		// would mean every attachment-parented effect inherited nothing, which is
		// precisely the arrangement a rocket exhaust uses.
		//
		// It stops at two. A deeper chain is the transform hierarchy `scene`
		// refuses to have.
		const scene::Motion *ParentMotion(const ecs::Store &store, ecs::Entity emitter) {
			const ecs::Entity parent = store.ParentOf(emitter);
			if (parent == ecs::NULL_ENTITY) {
				return nullptr;
			}
			if (const scene::Motion *motion = store.Get<scene::Motion>(parent)) {
				return motion;
			}
			const ecs::Entity grandparent = store.ParentOf(parent);
			return grandparent == ecs::NULL_ENTITY ? nullptr : store.Get<scene::Motion>(grandparent);
		}
	}

	core::Vector3 HalfExtentOf(const ecs::Store &store, ecs::Entity emitter) {
		const ecs::Entity parent = store.ParentOf(emitter);
		if (parent == ecs::NULL_ENTITY) {
			return {0.0f, 0.0f, 0.0f};
		}
		if (const scene::Bounds *bounds = store.Get<scene::Bounds>(parent)) {
			return bounds->HalfExtent;
		}
		return {0.0f, 0.0f, 0.0f};
	}

	// --- installation and block management -----------------------------------

	void InstallParticles(ecs::Store &store, uint32_t capacity) {
		ParticleSystem system;
		system.Capacity = capacity;
		system.Instances.resize(capacity);
		system.States.resize(capacity);
		store.SetResource(std::move(system));

		// **Observed here rather than wherever the first emitter is made**, which
		// is `Store::Observe`'s own warning about observing late: a component
		// observed after rows already exist has no dirty bits for them, so the
		// first write to an existing emitter would go unnoticed. Installing the
		// pool is the earliest point a world can be said to have particles in it.
		//
		// What this buys is the gate in `RefreshEmitters` that took the refresh
		// pass from 522 us to 74 us at a hundred thousand emitters. What it costs
		// is a dirty-bit column on the emitter table, which is one bit a row.
		store.Observe<ParticleEmitter>();
	}

	namespace {
		// Takes a range out of the pool, exact-fit from the free list or bumped.
		//
		// **Exact fit and not best fit, because a block is never resized.** A
		// best-fit split would leave a tail that only a smaller block could use,
		// and blocks come in a handful of sizes derived from rate and lifetime -
		// so exact fit reuses almost everything and costs a linear scan over a
		// list that is empty in a steady scene.
		bool Take(ParticleSystem &system, uint32_t wanted, uint32_t &first) {
			for (size_t index = 0; index < system.Free.size(); index++) {
				if (system.Free[index].second == wanted) {
					first = system.Free[index].first;
					system.Free[index] = system.Free.back();
					system.Free.pop_back();
					return true;
				}
			}

			if (system.Used + wanted > system.Capacity) {
				return false;
			}
			first = system.Used;
			system.Used += wanted;
			return true;
		}

		// How many slots one emitter needs to sustain its own rate.
		//
		// Rate times the longest life it can draw, rounded up, plus one - the
		// plus one is what stops an emitter at exactly one particle a second with
		// a one-second life from oscillating between zero and one slot.
		uint32_t BlockSizeFor(const ParticleEmitter &emitter, uint32_t ceiling) {
			const float wanted = std::max(emitter.Rate, 0.0f) * std::max(emitter.Lifetime.Maximum, 0.0f);
			const auto slots = static_cast<uint32_t>(std::ceil(wanted)) + 1;
			return std::clamp(slots, 1u, ceiling);
		}
	}

	size_t RefreshEmitters(ecs::Store &store) {
		ENGINE_PROFILE_CAT("refresh emitters", core::ProfileCategory::Simulation);

		auto *system = store.ResourceMutable<ParticleSystem>();
		if (system == nullptr) {
			return 0;
		}

		// **The block index lives on the emitter's own row**, as a component, so
		// finding an emitter's block is a column read rather than a search. That
		// is the same reason `ActiveCamera` names an entity rather than being
		// searched for.
		system->Statistics.EmittersRefused = 0;

		// Mark every block unclaimed, then let the walk claim them back. A block
		// still unclaimed at the end belonged to an emitter that has died.
		for (EmitterBlock &block : system->Blocks) {
			block.Reserved = 0;
		}

		// **Three spans, and the split is where the three costs actually are.**
		// The claim walk is proportional to *emitters* and is where the curve
		// sampling lives; the reclaim sweep is proportional to blocks ever
		// allocated, which is not the same number and does not come back down;
		// and the compaction below is what stops the second from growing
		// forever. A single `refresh emitters` bar could say none of that.
		{
			ENGINE_PROFILE_CAT("emitters.claim", core::ProfileCategory::Simulation);

			store.Each<const ParticleEmitter, EmitterSlot>(
				[&](ecs::Entity entity, const ParticleEmitter &emitter, EmitterSlot &slot) {
					// An emitter that is off and has nothing left alive gives its
					// block back. One that is off and still has particles keeps it,
					// because disabling must not kill what is already in the air -
					// `ParticleEmitter::Enabled` carries the argument.
					const bool holds = slot.Index != NO_SLOT && slot.Index < system->Blocks.size();
					if (holds) {
						EmitterBlock &block = system->Blocks[slot.Index];
						block.Reserved = 1;

						if (!emitter.Enabled && block.Live == 0) {
							system->Free.emplace_back(block.First, block.Capacity);
							block.Capacity = 0;
							block.Owner = ecs::NULL_ENTITY;
							block.Reserved = 0;
							slot.Index = NO_SLOT;
							return;
						}

						// **Re-sampled only when the emitter was written, and this
						// gate is the single largest thing in the module's frame
						// cost.** Sampling four curves is sixty-four `Evaluate`
						// calls, each a scan over a keypoint list; at a hundred
						// thousand emitters that is 6.4 million evaluations a frame
						// for tables that almost never change.
						//
						// Measured before and after, `bench` preset, 24 threads, at
						// the roadmap's 100,000 emitters and 500,000 particles:
						// **522 us unconditional against 192 us gated**, taking the
						// whole frame from 738 us to 389 us. The refresh pass was 70
						// per cent of the frame and is now half of it - the remaining
						// 192 us is the archetype walk itself and the scalar copies,
						// which is what a walk over a hundred thousand rows costs.
						//
						// `ecs::ChangeChannel` is the mechanism and `Observe` in
						// `InstallParticles` is what turns it on. What it costs is
						// **one frame of latency on a curve edit** - a script writing
						// `emitter.Size` during `Simulation` is seen by the next
						// frame's refresh, because `ClearChanges` runs at the phase
						// boundary between them. That is invisible for an authored
						// property and would matter for a curve driven per frame,
						// which is a thing to drive through `Brightness` instead.
						if (store.Changed<ParticleEmitter>(entity)) {
							SampleCurves(emitter, block.Curves);
						}
						ApplyPlayback(store, emitter, block);

						// Where the emitter is now. An emitter on an `Attachment`
						// takes the attachment's resolved world frame; one on a part
						// takes the part's. Neither is a walk - both are one column
						// read on the parent, and `ResolveAttachments` has already run.
						block.Frame = EmitterFrame(store, entity);
						return;
					}

					if (!emitter.Enabled) {
						return;
					}

					uint32_t first = 0;
					const uint32_t wanted = BlockSizeFor(emitter, system->BlockCeiling);
					// **The ceiling is on rows in use, not rows ever made** - which
					// is what `MAX_EMITTER_SLOTS` has always claimed to be. A free
					// row costs nothing to take, so it is only a fresh one that has
					// to fit under the cap.
					const bool recycling = !system->FreeSlots.empty();
					if ((!recycling && system->Blocks.size() >= MAX_EMITTER_SLOTS) ||
						!Take(*system, wanted, first)) {
						system->Statistics.EmittersRefused++;
						return;
					}

					EmitterBlock block;
					block.First = first;
					block.Capacity = wanted;
					block.Owner = entity;
					block.Reserved = 1;

					// **A fresh block owes one particle immediately.**
					//
					// The accumulator adds `Rate * delta` per frame, so an emitter at
					// a low rate produces nothing for `1 / Rate` seconds after it is
					// enabled - a fifth of a particle a second waits five seconds, and
					// what an author sees is an effect that does not work. They turn
					// the rate up, get a crowd, and never find out the first one was
					// merely late.
					//
					// Starting the accumulator owing one makes "enable it and it
					// starts" true at every rate. It is not free particles: the debt
					// is subtracted like any other, so the *steady* rate is unchanged
					// and only the first particle moves.
					block.Pending = 1.0f;
					SampleCurves(emitter, block.Curves);
					ApplyPlayback(store, emitter, block);
					block.Frame = EmitterFrame(store, entity);

					if (recycling) {
						const uint32_t reused = system->FreeSlots.back();
						system->FreeSlots.pop_back();
						slot.Index = static_cast<uint16_t>(reused);
						system->Blocks[reused] = block;
					} else {
						slot.Index = static_cast<uint16_t>(system->Blocks.size());
						system->Blocks.push_back(block);
					}
				}
			);
		}

		// Reclaim the blocks nobody claimed. Their particles go with them: an
		// emitter that has been destroyed takes its effect with it, which is what
		// deleting one is for.
		ENGINE_PROFILE_CAT("emitters.reclaim", core::ProfileCategory::Simulation);

		size_t live = 0;
		for (size_t index = 0; index < system->Blocks.size(); index++) {
			EmitterBlock &block = system->Blocks[index];
			if (block.Reserved == 0 && block.Capacity > 0) {
				system->Free.emplace_back(block.First, block.Capacity);
				block.Capacity = 0;
				block.Live = 0;
				block.Owner = ecs::NULL_ENTITY;

				// **And the row itself, which used to be abandoned.** See
				// `ParticleSystem::FreeSlots`: the particles came back and the
				// row did not, so the walk above and the memory under it grew
				// with every effect the game had ever played.
				system->FreeSlots.push_back(static_cast<uint32_t>(index));
			}
			if (block.Capacity > 0) {
				live++;
			}
		}

		system->Statistics.Blocks = static_cast<uint32_t>(live);
		return live;
	}

	// --- the step ------------------------------------------------------------

	namespace {
		// The smallest run of blocks worth handing to another worker.
		//
		// **Sixteen, and it is a block count rather than a particle count**, which
		// is the whole reason it is so much smaller than the other grains in the
		// engine. `physics::INTEGRATE_GRAIN` and `client::DRAW_LIST_GRAIN` are
		// thousands because their unit is one row of arithmetic; this loop's unit
		// is one *emitter*, and an emitter carries as many particles as its rate
		// and lifetime bought it - a hundred is ordinary. So sixteen blocks is
		// somewhere between sixteen and a few thousand particles, which puts the
		// dispatch floor in the same place their thousands do.
		//
		// **Estimated by that analogy and not measured**, which is the honest
		// label. `engine.effects.bench.particles` is the suite that would settle
		// it, laddering block counts either side of the floor, and a reading that
		// disagrees with this constant should win.
		constexpr size_t BLOCK_GRAIN = 16;

		// The dispatch floor, in blocks.
		//
		// Passed explicitly rather than derived from the grain, because
		// `Jobs::For`'s derivation assumes one index is cheap and one index here
		// is an emitter's whole particle list. Two hundred and fifty-six blocks is
		// where a scene stops being a handful of effects and starts being the
		// thing this module was built for.
		constexpr size_t BLOCK_MINIMUM = 256;

		// The same two numbers for the spawn dispatch, and they are smaller
		// because a plan is not a block.
		//
		// **A plan is one emitter that owes at least one particle**, so at a
		// steady rate it is a fraction of the emitter count rather than all of
		// it - `StressParticles.luau` has 5,120 emitters and about 1,700 plans a
		// tick. The floor is therefore lower than `BLOCK_MINIMUM` or a scene
		// that genuinely has thousands of births a tick would never dispatch;
		// the grain is lower because a plan carries a whole birth and a block
		// often carries none.
		constexpr size_t SPAWN_GRAIN = 8;
		constexpr size_t SPAWN_MINIMUM = 64;
	}

	ParticleStatistics StepParticles(ecs::Store &store, float delta) {
		ENGINE_PROFILE_CAT("step particles", core::ProfileCategory::Simulation);

		auto *system = store.ResourceMutable<ParticleSystem>();
		if (system == nullptr) {
			return {};
		}

		ParticleInstance *const instances = system->Instances.data();
		ParticleState *const states = system->States.data();
		const uint32_t capacity = system->Capacity;

		// **One counter per block rather than one shared atomic**, because the
		// counts are summed after the dispatch rather than during it. An atomic
		// incremented half a million times a frame is a cache line every worker
		// fights over, and the sum is the same number either way.
		std::vector<EmitterBlock> &blocks = system->Blocks;

		// **Two spans, because the two halves answer to different things.**
		// Ageing is proportional to particles *alive* and is parallel over
		// blocks; spawning is proportional to particles *born* and is serial
		// because it reads `ParticleEmitter`. One bar over both could only say
		// the tick was expensive, and the first question anybody asks of this
		// module is which of those two grew.
		{
			ENGINE_PROFILE_CAT("particles.age", core::ProfileCategory::Simulation);

			parallel::Jobs::For(
				blocks.size(),
				BLOCK_GRAIN,
				[&blocks, instances, states, capacity, delta](size_t begin, size_t end) {
					for (size_t index = begin; index < end; index++) {
						EmitterBlock &block = blocks[index];
						if (block.Capacity == 0 || block.First + block.Capacity > capacity) {
							continue;
						}

						const auto slot = static_cast<uint16_t>(index);
						const float scaled = delta;

						// **The block's own terms, resolved once rather than per
						// particle.** Every one of these is a function of the block
						// and the step and of nothing a particle carries, so a block
						// of a hundred computed each of them a hundred times. That
						// is invisible in a release build and is a measurable share
						// of the tick at `-O0`, where `Vector3 operator*` is a call
						// that returns through memory.
						const float damping = std::max(0.0f, 1.0f - block.Drag * scaled);
						const core::Vector3 push = block.Acceleration * scaled;

						// **The flipbook, decided once.** `FlipbookCell` starts by
						// working out how many cells the sheet holds and returning
						// zero when there are not two - which is every emitter in
						// every scene that has no flipbook, and they are almost all
						// of them. Asking here means those never make the call.
						const uint32_t cells =
							std::min<uint32_t>(block.Frames, FlipbookCells(block.Flipbook));
						const bool animated = cells > 1;

						// --- age what is alive ---------------------------------
						//
						// Backwards, so a swap-with-last cannot move a row this loop
						// has yet to visit. Forwards with a swap is the classic way to
						// skip a particle: the one swapped in takes the index the loop
						// has already passed.
						uint32_t live = block.Live;
						for (uint32_t at = live; at-- > 0;) {
							const uint32_t row = block.First + at;
							ParticleState &state = states[row];

							state.Age += scaled;
							if (state.Age >= state.Lifetime || state.Lifetime <= 0.0f) {
								live--;
								if (at != live) {
									states[row] = states[block.First + live];
									instances[row] = instances[block.First + live];
								}
								states[block.First + live].Lifetime = 0.0f;
								continue;
							}

							// Drag first, then acceleration, then position - which is
							// semi-implicit Euler and is what `physics::Integrate`
							// uses. Explicit Euler on a drag term diverges at large
							// steps, and a particle system is exactly where a large
							// step happens: a frame after a stall is a tenth of a
							// second.
							state.Velocity = state.Velocity * damping;
							state.Velocity = state.Velocity + push;

							ParticleInstance &instance = instances[row];
							if (block.Locked) {
								// A locked particle's offset from its parent is what is
								// preserved, so it is recomputed rather than
								// integrated. That is what makes an engine glow stay
								// on the engine.
								state.LocalOffset = state.LocalOffset + state.Velocity * scaled;
								instance.Position = block.Frame.PointToWorldSpace(state.LocalOffset);
							} else {
								instance.Position = instance.Position + state.Velocity * scaled;
							}

							// **One cursor for four curves.** `Size`, `Squash`,
							// `Alpha` and `Colour` are all asked about this same
							// age, and resolving where that age falls is the bulk of
							// what asking costs. See `CurveCursor`.
							const float age = state.Lifetime > 0.0f ? state.Age / state.Lifetime : 1.0f;
							const CurveCursor cursor = CurveAt(age);

							const float width = SampleCurve(block.Curves.Size, cursor);
							const float squash = SampleCurve(block.Curves.Squash, cursor);

							// Squash stretches one axis and shrinks the other, so the
							// particle's area is roughly kept - a squash that only
							// stretched would make a flattening particle grow.
							instance.Size = PackParticleSize(
								width * (squash < 0.0f ? 1.0f - squash : 1.0f / (1.0f + squash)),
								width * (squash > 0.0f ? 1.0f + squash : 1.0f / (1.0f - squash))
							);

							// **Wrapped in sixteen-bit integer arithmetic**, which is
							// the whole reason the rotation is stored as a turn rather
							// than as an angle: adding past 65,535 wraps to zero for
							// free, so a spinning particle needs no `fmod` and cannot
							// drift out of range after a few thousand frames.
							const auto turned = static_cast<int32_t>(state.Spin * scaled * 65536.0f);
							const uint32_t rotation =
								(static_cast<uint32_t>(
									static_cast<int32_t>(instance.RotationAndCell & 0xFFFFu) + turned
								)) &
								0xFFFFu;

							const uint32_t cell =
								animated ? FlipbookCell(block, state.Age, state.Lifetime, state.Seed) : 0u;
							instance.RotationAndCell = rotation | (cell << 16);
							instance.Colour = WithAlpha(
								SampleColourCurve(block.Curves.Colour, cursor),
								SampleCurve(block.Curves.Alpha, cursor)
							);
							instance.Slot = slot;
						}

						block.Live = live;
					}
				},
				BLOCK_MINIMUM
			);
		}

		// Spawning is a second pass and a serial one.
		//
		// **Serial because it reads `ParticleEmitter`**, which is fifteen hundred
		// bytes a row and is not in the block. Making it parallel would mean
		// either duplicating the spawn parameters into every block - which is the
		// kilobyte this module exists to keep out of the per-frame path - or
		// walking the emitter column from inside a worker, which `Store::Each`
		// does not promise is safe.
		//
		// **And it is the right half to leave serial**, because it is proportional
		// to particles *born* rather than particles alive. A steady scene at half
		// a million particles with a five-second average life spawns a hundred
		// thousand a second, which is under two thousand a frame.
		ParticleStatistics counted;
		counted.Blocks = system->Statistics.Blocks;
		counted.EmittersRefused = system->Statistics.EmittersRefused;

		// **Two phases, because what made this serial was the column and not the
		// work.** Reading `ParticleEmitter` from a worker is what `Store::Each`
		// makes no promise about; the births themselves touch nothing but their
		// own block's slice of the pool. So the walk copies what a birth needs
		// off each row that owes one - see `SpawnPlan` - and the births
		// dispatch.
		//
		// Measured with every rate at zero, the walk alone is 0.145 ms over five
		// thousand emitters, so phase one is not what this pass costs.
		static thread_local std::vector<SpawnPlan> plans;
		plans.clear();

		{
			ENGINE_PROFILE_CAT("particles.plan", core::ProfileCategory::Simulation);

			store.Each<const ParticleEmitter, const EmitterSlot>(
				[&](ecs::Entity entity, const ParticleEmitter &emitter, const EmitterSlot &slot) {
					if (slot.Index == NO_SLOT || slot.Index >= blocks.size()) {
						return;
					}
					EmitterBlock &block = blocks[slot.Index];
					if (block.Capacity == 0 || !emitter.Enabled) {
						return;
					}

					// **The accumulator stays here**, on the thread that owns the
					// world. It is the emitter's own debt and it has to advance
					// on every tick whether or not it comes due, so it is not
					// part of what dispatches.
					block.Pending += emitter.Rate * delta * std::max(emitter.TimeScale, 0.0f);
					const auto owed = static_cast<uint32_t>(block.Pending);
					if (owed == 0) {
						return;
					}
					block.Pending -= static_cast<float>(owed);

					SpawnPlan plan;
					plan.Block = slot.Index;
					plan.Id = entity.Id;
					plan.Owed = owed;
					plan.Half = HalfExtentOf(store, entity);

					plan.Shape = emitter.Shape;
					plan.ShapeStyle = emitter.ShapeStyle;
					plan.ShapeDirection = emitter.ShapeDirection;
					plan.ShapePartial = emitter.ShapePartial;

					plan.Emission = scene::NormalOf(emitter.EmissionDirection);
					plan.SpreadX = emitter.SpreadAngle.X * RADIANS_PER_DEGREE;
					plan.SpreadY = emitter.SpreadAngle.Y * RADIANS_PER_DEGREE;

					plan.Speed = emitter.Speed;
					plan.Lifetime = emitter.Lifetime;
					plan.RotationSpeed = emitter.RotationSpeed;
					plan.Rotation = emitter.Rotation;

					// The one store lookup a birth used to make, made once here
					// - and it is the reason a worker could not do this at all.
					if (emitter.VelocityInheritance != 0.0f) {
						if (const scene::Motion *motion = ParentMotion(store, entity)) {
							plan.Inherited = motion->Linear * emitter.VelocityInheritance;
						}
					}

					plans.push_back(plan);
				}
			);
		}

		{
			ENGINE_PROFILE_CAT("particles.spawn", core::ProfileCategory::Simulation);

			// **The buffer is taken by pointer, and naming it inside the body
			// would have been a bug.** `plans` is `thread_local`, so a worker
			// that named it would resolve its *own* copy - which is empty, and
			// indexing it is a segmentation fault rather than a wrong answer.
			// Resolved once here, on the thread that filled it.
			SpawnPlan *const planned = plans.data();

			parallel::Jobs::For(
				plans.size(),
				SPAWN_GRAIN,
				[instances, states, &blocks, planned](size_t begin, size_t end) {
					for (size_t at = begin; at < end; at++) {
						SpawnPlan &plan = planned[at];
						EmitterBlock &block = blocks[plan.Block];

						const uint32_t id = plan.Id;
						const Vector3 &half = plan.Half;

						// A newborn particle is at the head of every curve, so
						// its size and its colour are the block's first sample
						// and the same for all of them.
						const uint32_t bornSize =
							PackParticleSize(block.Curves.Size[0], block.Curves.Size[0]);
						const uint32_t bornColour = WithAlpha(block.Curves.Colour[0], block.Curves.Alpha[0]);
						const auto bornSlot = static_cast<uint16_t>(plan.Block);

						for (uint32_t spawn = 0; spawn < plan.Owed; spawn++) {
							if (block.Live >= block.Capacity) {
								plan.Dropped = plan.Owed - spawn;
								break;
							}

							const uint32_t row = block.First + block.Live;

							// **A per-particle index that keeps advancing rather
							// than the slot number**, because the slot is reused
							// the moment a particle dies - so seeding from it
							// would make every replacement particle identical to
							// the one it replaced, and a steady emitter would
							// settle into a repeating loop of the same few
							// particles.
							const uint32_t index = block.Spawned++;

							const Vector3 local = SpawnPoint(plan, half, id, index);
							const Vector3 direction = SpawnDirection(plan, local, id, index);
							const float speed = Between(SeedOf(id, index, 10), plan.Speed);

							ParticleState &state = states[row];
							state.Age = 0.0f;
							state.Lifetime = std::max(Between(SeedOf(id, index, 11), plan.Lifetime), 1e-4f);
							state.LocalOffset = local;
							state.Seed = index;
							state.Spin =
								Between(SeedOf(id, index, 13), plan.RotationSpeed) * TURNS_PER_DEGREE;
							state.Velocity = block.Frame.VectorToWorldSpace(direction) * speed;

							// A new particle keeps some of what its parent was
							// doing, so smoke from a moving vehicle trails behind
							// it rather than being left in a neat line at the
							// origin of each frame.
							state.Velocity = state.Velocity + plan.Inherited;

							ParticleInstance &instance = instances[row];
							instance.Position = block.Frame.PointToWorldSpace(local);
							instance.Slot = bornSlot;

							const float degrees = Between(SeedOf(id, index, 12), plan.Rotation);
							const auto turns = static_cast<uint32_t>(
								std::fmod(degrees * TURNS_PER_DEGREE + 1.0f, 1.0f) * 65535.0f
							);
							instance.RotationAndCell = turns & 0xFFFFu;

							instance.Size = bornSize;
							instance.Colour = bornColour;

							block.Live++;
						}
					}
				},
				SPAWN_MINIMUM
			);
		}

		// Summed after the dispatch rather than during it, which is the reason
		// the block counters are per block: an atomic incremented once a birth
		// is a cache line every worker fights over, and the total is the same
		// number either way.
		for (const SpawnPlan &plan : plans) {
			counted.Emitted += plan.Owed - plan.Dropped;
			counted.SpawnsDropped += plan.Dropped;
		}

		for (const EmitterBlock &block : blocks) {
			counted.Live += block.Live;
		}

		system->Statistics = counted;
		return counted;
	}

}
