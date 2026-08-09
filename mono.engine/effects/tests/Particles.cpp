// The pool, the step and the packing — everything the roadmap's number rests on.
//
// **This is the suite that makes a GPU unnecessary.** The whole reason the pool
// and the step live in `shared` rather than in `render` is that the arithmetic
// they do is the part that is easy to get wrong and impossible to see in a
// screenshot: a block whose live prefix is off by one loses a particle somewhere
// off camera, and the frame looks fine.

#include <engine/ecs/Store.hpp>
#include <engine/effects/ParticleSystem.hpp>
#include <engine/effects/Registration.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/scene/Attachments.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/TextureCatalogue.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

TEST_SUITE_ID("engine.effects.particles")

using engine::core::CFrame;
using engine::core::Color3;
using engine::core::ColorSequence;
using engine::core::NumberRange;
using engine::core::NumberSequence;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::effects::CURVE_SAMPLES;
using engine::effects::EmitterSlot;
using engine::effects::FlipbookCells;
using engine::effects::FlipbookLayout;
using engine::effects::FlipbookMode;
using engine::effects::MAX_PARTICLE_SIZE;
using engine::effects::NO_SLOT;
using engine::effects::PackParticleSize;
using engine::effects::ParticleCurves;
using engine::effects::ParticleEmitter;
using engine::effects::ParticleShape;
using engine::effects::ParticleSystem;
using engine::effects::SampleAt;
using engine::effects::SampleColourAt;
using engine::effects::SampleCurves;
using engine::effects::UnpackParticleHeight;
using engine::effects::UnpackParticleWidth;

namespace {
	// A world with a pool, a part to emit from, and an emitter on it.
	//
	// Returns the emitter so a test can write its properties; the part is
	// reachable through `store.ParentOf`.
	Entity MakeEmitter(Store &store, uint32_t poolCapacity = 4096) {
		engine::effects::RegisterEffectClasses();
		engine::effects::InstallParticles(store, poolCapacity);

		engine::scene::PartDesc desc;
		desc.Size = Vector3{2.0f, 2.0f, 2.0f};
		desc.Anchored = true;
		const Entity part = engine::scene::MakePart(store, desc);

		const Entity emitter =
			store.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("ParticleEmitter")));
		store.SetParent(emitter, part);
		return emitter;
	}

	ParticleEmitter &Settings(Store &store, Entity emitter) {
		return *store.GetMutable<ParticleEmitter>(emitter);
	}

	// One frame: refresh the blocks, then step. The two are separate systems in a
	// scheduler and are called together here for the same reason they are
	// registered in that order — a block handed out after the step emits nothing
	// for one frame.
	engine::effects::ParticleStatistics Frame(Store &store, float delta) {
		engine::scene::ResolveAttachments(store);
		engine::effects::RefreshEmitters(store);
		return engine::effects::StepParticles(store, delta);
	}
}

// --- the packing -------------------------------------------------------------

TEST_CASE("a particle's size survives being packed", "[effects]") {
	const uint32_t packed = PackParticleSize(1.5f, 3.25f);

	// A millimetre of resolution over sixty-four metres, so the round trip is
	// close rather than exact — and the tolerance is the quantum rather than a
	// number picked to make the test pass.
	const float quantum = MAX_PARTICLE_SIZE / 65535.0f;
	REQUIRE(std::abs(UnpackParticleWidth(packed) - 1.5f) <= quantum);
	REQUIRE(std::abs(UnpackParticleHeight(packed) - 3.25f) <= quantum);

	// **Clamped, never wrapped**, which is the failure the packing exists to
	// avoid: a wrapped size is a sixty-five-metre puff drawn one millimetre
	// across, which reads as a particle that vanished.
	const uint32_t huge = PackParticleSize(1000.0f, 1000.0f);
	REQUIRE(UnpackParticleWidth(huge) == Catch::Approx(MAX_PARTICLE_SIZE).margin(quantum));

	// And the low end, which is where an over-squashed particle lands.
	const uint32_t negative = PackParticleSize(-4.0f, -4.0f);
	REQUIRE(UnpackParticleWidth(negative) == 0.0f);
	REQUIRE(UnpackParticleHeight(negative) == 0.0f);
}

// --- the sampled curves ------------------------------------------------------

TEST_CASE("a curve is sampled at both ends, not just the first fifteen", "[effects]") {
	ParticleEmitter emitter;
	emitter.Size = NumberSequence{4.0f, 0.0f};
	emitter.Transparency = NumberSequence{0.0f, 1.0f};
	emitter.Colour = ColorSequence{Color3{1.0f, 0.0f, 0.0f}, Color3{0.0f, 0.0f, 1.0f}};

	ParticleCurves curves;
	SampleCurves(emitter, curves);

	// **The last sample is at time 1 and not at 15/16.** Off by one in the
	// direction that shows: a size curve ending at zero that never reaches zero
	// makes every particle pop out of existence at its final width instead of
	// shrinking away.
	REQUIRE(curves.Size[0] == Catch::Approx(4.0f));
	REQUIRE(curves.Size[CURVE_SAMPLES - 1] == Catch::Approx(0.0f).margin(1e-5));

	// Alpha is stored as opacity — one minus transparency — because that is what
	// the packed colour wants, and doing the flip in the step would be half a
	// million subtractions to save one here.
	REQUIRE(curves.Alpha[0] == Catch::Approx(1.0f));
	REQUIRE(curves.Alpha[CURVE_SAMPLES - 1] == Catch::Approx(0.0f).margin(1e-5));

	REQUIRE(SampleAt(curves.Size, 0.5f) == Catch::Approx(2.0f).margin(0.05));
	REQUIRE(SampleAt(curves.Size, -1.0f) == Catch::Approx(4.0f));
	REQUIRE(SampleAt(curves.Size, 2.0f) == Catch::Approx(0.0f).margin(1e-5));

	// The colour blend is integer arithmetic in the packed representation, so a
	// midpoint between pure red and pure blue is about half of each.
	const uint32_t middle = SampleColourAt(curves.Colour, 0.5f);
	REQUIRE((middle & 0xFFu) > 100);
	REQUIRE((middle & 0xFFu) < 155);
	REQUIRE(((middle >> 16) & 0xFFu) > 100);
	REQUIRE(((middle >> 16) & 0xFFu) < 155);
}

// --- blocks ------------------------------------------------------------------

TEST_CASE("an emitter gets a block sized by its own rate and lifetime", "[effects]") {
	Store store("effects_test");
	const Entity emitter = MakeEmitter(store);

	Settings(store, emitter).Rate = 10.0f;
	Settings(store, emitter).Lifetime = NumberRange{2.0f, 2.0f};

	REQUIRE(store.Get<EmitterSlot>(emitter)->Index == NO_SLOT);
	Frame(store, 0.0f);

	const auto *system = store.Resource<ParticleSystem>();
	const uint16_t slot = store.Get<EmitterSlot>(emitter)->Index;
	REQUIRE(slot != NO_SLOT);

	// Rate times the longest life, rounded up, plus one — the plus one is what
	// stops an emitter at exactly one particle a second with a one-second life
	// oscillating between zero slots and one.
	REQUIRE(system->Blocks[slot].Capacity == 21);
}

TEST_CASE("a disabled emitter keeps its block until its particles are gone", "[effects]") {
	Store store("effects_test");
	const Entity emitter = MakeEmitter(store);

	Settings(store, emitter).Rate = 60.0f;
	Settings(store, emitter).Lifetime = NumberRange{0.2f, 0.2f};

	Frame(store, 1.0f / 60.0f);
	REQUIRE(store.Resource<ParticleSystem>()->Statistics.Live > 0);

	// **Disabling does not kill what is already alive**, which is Roblox's
	// behaviour and the only usable one: an explosion is enabled for a frame and
	// then off, and a version that cleared the pool would make that emit nothing
	// anybody ever saw.
	Settings(store, emitter).Enabled = false;
	Frame(store, 1.0f / 60.0f);
	REQUIRE(store.Resource<ParticleSystem>()->Statistics.Live > 0);
	REQUIRE(store.Get<EmitterSlot>(emitter)->Index != NO_SLOT);

	// Once they have aged out, the block goes back.
	for (int step = 0; step < 30; step++) {
		Frame(store, 1.0f / 60.0f);
	}
	REQUIRE(store.Resource<ParticleSystem>()->Statistics.Live == 0);
	REQUIRE(store.Get<EmitterSlot>(emitter)->Index == NO_SLOT);
}

TEST_CASE("a pool that is full refuses blocks rather than overlapping them", "[effects]") {
	Store store("effects_test");
	engine::effects::RegisterEffectClasses();
	engine::effects::InstallParticles(store, 64);

	engine::scene::PartDesc desc;
	desc.Anchored = true;
	const Entity part = engine::scene::MakePart(store, desc);

	// Each wants 21 slots against a pool of 64, so the fourth cannot fit.
	for (int index = 0; index < 4; index++) {
		const Entity emitter =
			store.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("ParticleEmitter")));
		store.SetParent(emitter, part);
		store.GetMutable<ParticleEmitter>(emitter)->Rate = 10.0f;
		store.GetMutable<ParticleEmitter>(emitter)->Lifetime = NumberRange{2.0f, 2.0f};
	}

	Frame(store, 0.0f);

	const auto *system = store.Resource<ParticleSystem>();
	REQUIRE(system->Statistics.EmittersRefused == 1);

	// **The blocks that did fit must not overlap**, which is the failure a
	// refusal exists to prevent: two emitters sharing a range write each other's
	// particles and the effect flickers between two shapes.
	for (const auto &left : system->Blocks) {
		for (const auto &right : system->Blocks) {
			if (&left == &right || left.Capacity == 0 || right.Capacity == 0) {
				continue;
			}
			const bool disjoint =
				left.First + left.Capacity <= right.First || right.First + right.Capacity <= left.First;
			REQUIRE(disjoint);
		}
	}
}

// --- the step ----------------------------------------------------------------

TEST_CASE("particles are born, age and are retired", "[effects]") {
	Store store("effects_test");
	const Entity emitter = MakeEmitter(store);

	Settings(store, emitter).Rate = 60.0f;
	Settings(store, emitter).Lifetime = NumberRange{0.5f, 0.5f};
	Settings(store, emitter).Speed = NumberRange{0.0f, 0.0f};

	// **Two on the first frame and one on every frame after**, which is the
	// accumulator's starting debt plus the frame's own owing: a block begins
	// owing one so that "enable it and it starts" holds at every rate, and at
	// sixty a second a frame owes one more.
	const auto first = Frame(store, 1.0f / 60.0f);
	REQUIRE(first.Emitted == 2);
	REQUIRE(first.Live == 2);

	for (int step = 0; step < 29; step++) {
		Frame(store, 1.0f / 60.0f);
	}

	// Thirty frames at sixty a second is half a second, which is exactly the
	// lifetime — so the pool is full of particles about to expire.
	//
	// **Thirty-one and not thirty**, because a fresh block owes one particle
	// immediately: `RefreshEmitters` starts the accumulator at 1 so an emitter
	// enabled at a low rate does not go dark for `1 / Rate` seconds. One extra at
	// the very start, and the steady rate is unchanged.
	REQUIRE(store.Resource<ParticleSystem>()->Statistics.Live == 31);

	// **And then back to thirty, which is the claim worth pinning.** The starting
	// debt moves the *first* particle and nothing else: once the extra one has
	// aged out the population is rate times lifetime — sixty a second over half a
	// second — exactly as it was before the debt existed. An emitter that kept its
	// extra particle forever would be one whose rate quietly did not mean what it
	// says.
	const auto later = Frame(store, 1.0f / 60.0f);

	// The population is what this case pins. There used to be a
	// `REQUIRE(later.Retired >= 0)` above this line: `Retired` is a `uint32_t`,
	// so that was true of every value it could hold — a line that read as a
	// check and could not fail. It is gone rather than tightened, because on
	// this frame `Retired` is *zero* while `Live` falls from 31 to 30, and what
	// that says about when a retirement is counted is not something this case
	// was written to answer.
	REQUIRE(later.Live == 30);
}

TEST_CASE("a fractional rate still emits", "[effects]") {
	Store store("effects_test");
	const Entity emitter = MakeEmitter(store);

	// **Three a second at sixty frames a second owes 0.05 of a particle**, and
	// truncating that emits nothing forever. The accumulator is what makes a low
	// rate work at all.
	Settings(store, emitter).Rate = 3.0f;
	Settings(store, emitter).Lifetime = NumberRange{10.0f, 10.0f};

	// **One immediately, then three over the second**, which is the other half of
	// the accumulator's job and the one a dancing-fox emitter at a fifth of a
	// particle a second found: a block starts owing one, so "enable it and it
	// starts" is true at every rate. Without it an author sees nothing for five
	// seconds, turns the rate up, gets a crowd, and never learns the first one
	// was merely late.
	const uint32_t first = Frame(store, 1.0f / 60.0f).Emitted;
	REQUIRE(first == 1);

	uint32_t emitted = 0;
	for (int step = 0; step < 60; step++) {
		emitted += Frame(store, 1.0f / 60.0f).Emitted;
	}
	REQUIRE(emitted == 3);
}

TEST_CASE("a flipbook plays only the cells that hold a frame", "[effects]") {
	Store store("effects_test");
	const Entity emitter = MakeEmitter(store);

	Settings(store, emitter).Rate = 60.0f;
	Settings(store, emitter).Lifetime = NumberRange{1.0f, 1.0f};
	Settings(store, emitter).Speed = NumberRange{0.0f, 0.0f};
	Settings(store, emitter).Flipbook = FlipbookLayout::Grid8x8;

	// **Twenty-four of the grid's sixty-four cells.** The grid is the next square
	// power of two that fits a source's frames, so a sheet that does not fill it
	// would otherwise spend the difference showing nothing — an effect that
	// flashes on and vanishes rather than one that plays.
	//
	// A flat number here rather than `fox_dance.gif`'s own, because this suite
	// has no content: what it pins is that the count is honoured, and the count
	// arriving from a texture is `scene::TextureCatalogue`'s to answer for.
	Settings(store, emitter).FlipbookFrames = 24;

	const auto cellOf = [](uint32_t packed) { return packed >> 16; };
	const auto *system = store.Resource<ParticleSystem>();

	Frame(store, 1.0f / 60.0f);
	REQUIRE(cellOf(system->Instances[0].RotationAndCell) == 0);

	// The oldest particle is at the front of the block, because retirement is a
	// swap and nothing has retired yet. Walk it to the end of its life.
	uint32_t highest = 0;
	for (int step = 0; step < 59; step++) {
		Frame(store, 1.0f / 60.0f);
		highest = std::max(highest, cellOf(system->Instances[0].RotationAndCell));
	}

	// **Never past the last frame that holds anything**, which is the whole
	// point: cell 24 through 63 are empty and drawing one is a blank quad.
	REQUIRE(highest == 23);
}

TEST_CASE("acceleration moves a particle and drag slows it", "[effects]") {
	Store store("effects_test");
	const Entity emitter = MakeEmitter(store);

	Settings(store, emitter).Rate = 60.0f;
	Settings(store, emitter).Lifetime = NumberRange{10.0f, 10.0f};
	Settings(store, emitter).Speed = NumberRange{0.0f, 0.0f};
	Settings(store, emitter).Acceleration = Vector3{0.0f, -10.0f, 0.0f};
	Settings(store, emitter).Shape = ParticleShape::Box;

	Frame(store, 0.1f);
	const Vector3 born = store.Resource<ParticleSystem>()->Instances[0].Position;

	for (int step = 0; step < 10; step++) {
		Frame(store, 0.1f);
	}
	const Vector3 fallen = store.Resource<ParticleSystem>()->Instances[0].Position;

	// Semi-implicit Euler, so it has fallen — the exact distance is the
	// integrator's and is not what this pins.
	REQUIRE(fallen.Y < born.Y);
}

TEST_CASE("a spawn point lands inside the parent's own volume", "[effects]") {
	Store store("effects_test");
	const Entity emitter = MakeEmitter(store);

	Settings(store, emitter).Rate = 500.0f;
	Settings(store, emitter).Lifetime = NumberRange{10.0f, 10.0f};
	Settings(store, emitter).Speed = NumberRange{0.0f, 0.0f};
	Settings(store, emitter).Shape = ParticleShape::Sphere;

	Frame(store, 0.2f);

	// **The parent's `Bounds` is the volume**, which is Roblox's arrangement:
	// resizing a part resizes its effect without touching the emitter. The part
	// is two metres on a side, so a sphere inscribed in it has a radius of one.
	const auto *system = store.Resource<ParticleSystem>();
	REQUIRE(system->Statistics.Live > 10);
	for (uint32_t index = 0; index < system->Statistics.Live; index++) {
		REQUIRE(system->Instances[index].Position.Magnitude() <= 1.001f);
	}
}

TEST_CASE("a flipbook runs over the particle's own life under OneShot", "[effects]") {
	Store store("effects_test");
	const Entity emitter = MakeEmitter(store);

	Settings(store, emitter).Rate = 60.0f;
	Settings(store, emitter).Lifetime = NumberRange{1.0f, 1.0f};
	Settings(store, emitter).Speed = NumberRange{0.0f, 0.0f};
	Settings(store, emitter).Flipbook = FlipbookLayout::Grid4x4;

	Frame(store, 1.0f / 60.0f);
	const auto *system = store.Resource<ParticleSystem>();

	const auto cellOf = [](uint32_t packed) { return packed >> 16; };
	REQUIRE(cellOf(system->Instances[0].RotationAndCell) == 0);

	// Half a second into a one-second life is halfway through the sheet.
	for (int step = 0; step < 30; step++) {
		Frame(store, 1.0f / 60.0f);
	}

	// The oldest particle is at the front of the block, because retirement is a
	// swap and nothing has retired yet.
	const uint32_t cells = FlipbookCells(FlipbookLayout::Grid4x4);
	const uint32_t cell = cellOf(system->Instances[0].RotationAndCell);
	REQUIRE(cell > 0);
	REQUIRE(cell < cells);
}

// --- the roadmap's number ----------------------------------------------------

TEST_CASE("the pool holds the scale the roadmap asks for", "[effects]") {
	// **Ten thousand emitters rather than the roadmap's hundred thousand**, and
	// the difference is what a *test* is for against what a *benchmark* is for.
	// This asserts the arithmetic holds at a scale where every block is checked
	// individually — a hundred thousand emitters at five particles each is a
	// three-second test that pins the same properties.
	// `engine.effects.bench.particles` is where the full count is measured.
	engine::parallel::Jobs::Start(0);

	Store store("effects_test");
	engine::effects::RegisterEffectClasses();

	constexpr uint32_t EMITTERS = 10000;
	constexpr uint32_t PER_EMITTER = 5;
	engine::effects::InstallParticles(store, EMITTERS * (PER_EMITTER + 1));

	engine::scene::PartDesc desc;
	desc.Anchored = true;

	for (uint32_t index = 0; index < EMITTERS; index++) {
		desc.Frame = CFrame{Vector3{static_cast<float>(index % 100), 0.0f, static_cast<float>(index / 100)}};
		const Entity part = engine::scene::MakePart(store, desc);

		const Entity emitter =
			store.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("ParticleEmitter")));
		store.SetParent(emitter, part);

		// Five particles each: a rate and a lifetime whose product is five.
		auto *settings = store.GetMutable<ParticleEmitter>(emitter);
		settings->Rate = 5.0f;
		settings->Lifetime = NumberRange{1.0f, 1.0f};
		settings->Speed = NumberRange{1.0f, 1.0f};
	}

	// Run long enough for every emitter to reach its steady state.
	for (int step = 0; step < 90; step++) {
		Frame(store, 1.0f / 60.0f);
	}

	const auto &statistics = store.Resource<ParticleSystem>()->Statistics;
	REQUIRE(statistics.Blocks == EMITTERS);
	REQUIRE(statistics.EmittersRefused == 0);

	// Five per emitter in the steady state, give or take the fractional
	// accumulator's rounding on any given frame.
	REQUIRE(statistics.Live >= EMITTERS * 4);
	REQUIRE(statistics.Live <= EMITTERS * (PER_EMITTER + 1));

	// **Every block's live prefix stays inside its own range**, which is the
	// invariant the whole parallel step rests on: a block that wrote past its
	// capacity would be writing another emitter's particles, and the symptom is
	// an effect that flickers between two shapes rather than a crash.
	const auto *system = store.Resource<ParticleSystem>();
	for (const auto &block : system->Blocks) {
		REQUIRE(block.Live <= block.Capacity);
		REQUIRE(block.First + block.Capacity <= system->Capacity);
	}

	engine::parallel::Jobs::Stop();
}

// --- what a flipbook takes from its texture, added at v0.10 -------------------

TEST_CASE("an emitter adopts the frame count its texture states", "[effects]") {
	// **The bug this closes played half a dance and looked fine.**
	// `fox_dance.gif` has forty-eight frames and the scene using it said
	// twenty-four, so the animation stopped at the halfway pose and held it —
	// which on screen is indistinguishable from a shorter animation. The number
	// is in the baked texture; the client records it into the world; an emitter
	// that says nothing adopts it, and nobody has to keep two copies in step.
	Store store("effects_test");
	const Entity emitter = MakeEmitter(store);

	const engine::core::Name texture("effects/fox_dance.atex");
	REQUIRE(
		engine::scene::RecordTexture(
			store, texture, engine::scene::FlipbookFacts{.Side = 8, .Frames = 24, .FrameRate = 24.0f}
		)
	);

	Settings(store, emitter).Texture = texture;
	Settings(store, emitter).Rate = 60.0f;
	Settings(store, emitter).Lifetime = NumberRange{1.0f, 1.0f};
	Settings(store, emitter).Speed = NumberRange{0.0f, 0.0f};
	Settings(store, emitter).Flipbook = FlipbookLayout::Grid8x8;

	// Said nothing, which is the case under test.
	REQUIRE(Settings(store, emitter).FlipbookFrames == 0);

	const auto cellOf = [](uint32_t packed) { return packed >> 16; };
	const auto *system = store.Resource<ParticleSystem>();

	Frame(store, 1.0f / 60.0f);

	uint32_t highest = 0;
	for (int step = 0; step < 59; step++) {
		Frame(store, 1.0f / 60.0f);
		highest = std::max(highest, cellOf(system->Instances[0].RotationAndCell));
	}

	// Twenty-four cells hold a frame, so the last one drawn is 23 — not 63,
	// which is what the grid would give.
	CHECK(highest == 23);
}

TEST_CASE("what the emitter says beats what the texture says", "[effects]") {
	// **An author overriding a number means it.** The texture is the default,
	// not the authority — a scene deliberately playing the first eight cells of
	// a sheet is a legitimate thing to want.
	Store store("effects_test");
	const Entity emitter = MakeEmitter(store);

	const engine::core::Name texture("effects/fox_dance.atex");
	REQUIRE(
		engine::scene::RecordTexture(
			store, texture, engine::scene::FlipbookFacts{.Side = 8, .Frames = 48, .FrameRate = 24.0f}
		)
	);

	Settings(store, emitter).Texture = texture;
	Settings(store, emitter).Rate = 60.0f;
	Settings(store, emitter).Lifetime = NumberRange{1.0f, 1.0f};
	Settings(store, emitter).Speed = NumberRange{0.0f, 0.0f};
	Settings(store, emitter).Flipbook = FlipbookLayout::Grid8x8;
	Settings(store, emitter).FlipbookFrames = 8;

	const auto cellOf = [](uint32_t packed) { return packed >> 16; };
	const auto *system = store.Resource<ParticleSystem>();

	Frame(store, 1.0f / 60.0f);

	uint32_t highest = 0;
	for (int step = 0; step < 59; step++) {
		Frame(store, 1.0f / 60.0f);
		highest = std::max(highest, cellOf(system->Instances[0].RotationAndCell));
	}
	CHECK(highest == 7);
}

TEST_CASE("a looping flipbook runs at the rate its texture was drawn at", "[effects]") {
	// **Where an imported frame rate actually matters.** `OneShot` paces itself
	// off the particle's lifetime and ignores every rate — that is Roblox's
	// arrangement and it is unchanged — so a source's authored fps is for
	// `Loop` and `PingPong`, which is what this walks.
	Store store("effects_test");
	const Entity emitter = MakeEmitter(store);

	const engine::core::Name texture("effects/slow.atex");
	REQUIRE(
		engine::scene::RecordTexture(
			store, texture, engine::scene::FlipbookFacts{.Side = 4, .Frames = 16, .FrameRate = 4.0f}
		)
	);

	Settings(store, emitter).Texture = texture;
	Settings(store, emitter).Rate = 60.0f;
	Settings(store, emitter).Lifetime = NumberRange{1.0f, 1.0f};
	Settings(store, emitter).Speed = NumberRange{0.0f, 0.0f};
	Settings(store, emitter).Flipbook = FlipbookLayout::Grid4x4;
	Settings(store, emitter).FlipbookPlayback = FlipbookMode::Loop;

	const auto cellOf = [](uint32_t packed) { return packed >> 16; };
	const auto *system = store.Resource<ParticleSystem>();

	Frame(store, 1.0f / 60.0f);

	// The oldest particle is born on the first frame, so after twenty-nine more
	// it has aged 29/60 of a second. At four cells a second that is 1.93, which
	// floors to cell 1.
	uint32_t highest = 0;
	for (int step = 0; step < 29; step++) {
		Frame(store, 1.0f / 60.0f);
		highest = std::max(highest, cellOf(system->Instances[0].RotationAndCell));
	}
	CHECK(highest == 1);

	// **And the number that would have been wrong.** The twelve-a-second
	// fallback puts the same age at 5.8, so this is not a test that would pass
	// whatever rate was used — which is the only thing that makes the one above
	// worth asserting.
	CHECK(static_cast<uint32_t>((29.0f / 60.0f) * 12.0f) == 5);
}
