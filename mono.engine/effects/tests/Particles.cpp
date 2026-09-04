// The pool, the step and the packing - everything the roadmap's number rests on.
//
// **This is the suite that makes a GPU unnecessary.** The whole reason the pool
// and the step live in `shared` rather than in `render` is that the arithmetic
// they do is the part that is easy to get wrong and impossible to see in a
// screenshot: a block whose live prefix is off by one loses a particle somewhere
// off camera, and the frame looks fine.

#include <engine/core/Bytes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/ecs/TypeDescriptor.hpp>
#include <engine/effects/ParticleSystem.hpp>
#include <engine/effects/Registration.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/scene/Attachments.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/TextureCatalogue.hpp>
#include <engine/scene/VectorField.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>

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
		desc.Simulated = false;
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
	// registered in that order - a block handed out after the step emits nothing
	// for one frame.
	engine::effects::ParticleStatistics Frame(Store &store, float delta) {
		engine::scene::ResolveAttachments(store);
		engine::effects::RefreshEmitters(store);
		return engine::effects::StepParticles(store, delta);
	}

	bool HasParent(const Store &store, Entity emitter) {
		return store.ParentOf(emitter) != engine::ecs::NULL_ENTITY;
	}
}

// --- the packing -------------------------------------------------------------

TEST_CASE("a particle's size survives being packed", "[effects]") {
	const uint32_t packed = PackParticleSize(1.5f, 3.25f);

	// A millimetre of resolution over sixty-four metres, so the round trip is
	// close rather than exact - and the tolerance is the quantum rather than a
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

	// Alpha is stored as opacity - one minus transparency - because that is what
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
	const uint32_t slot = store.Get<EmitterSlot>(emitter)->Index;
	REQUIRE(slot != NO_SLOT);

	// Rate times the longest life, rounded up, plus one - the plus one is what
	// stops an emitter at exactly one particle a second with a one-second life
	// oscillating between zero slots and one.
	REQUIRE(system->Blocks[slot].Capacity == 21);
}

TEST_CASE("an activation policy gates blocks and restores resident emission", "[effects][activation]") {
	Store store("effects_test");
	const Entity emitter = MakeEmitter(store);
	const Entity part = store.ParentOf(emitter);
	auto *system = store.ResourceMutable<ParticleSystem>();
	system->DeviceStepped = true;

	Settings(store, emitter).Rate = 20.0f;
	Settings(store, emitter).Lifetime = NumberRange{2.0f, 2.0f};
	store.SetParent(emitter, engine::ecs::NULL_ENTITY);
	engine::effects::RefreshEmitters(store, HasParent, 1);
	CHECK(store.Get<EmitterSlot>(emitter)->Index == NO_SLOT);
	CHECK(system->Blocks.empty());

	store.SetParent(emitter, part);
	engine::effects::RefreshEmitters(store, HasParent, 1);
	const uint32_t slot = store.Get<EmitterSlot>(emitter)->Index;
	REQUIRE(slot != NO_SLOT);
	CHECK(system->RuntimeStates[slot].Enabled);
	CHECK(system->RuntimeStates[slot].ContinuousRate == Catch::Approx(20.0f));

	store.SetParent(emitter, engine::ecs::NULL_ENTITY);
	engine::effects::RefreshEmitters(store, HasParent, 1);
	CHECK_FALSE(system->RuntimeStates[slot].Enabled);
	CHECK(system->RuntimeStates[slot].ContinuousRate == 0.0f);
	CHECK(system->RuntimeStates[slot].DeviceRetiring);

	store.SetParent(emitter, part);
	engine::effects::RefreshEmitters(store, HasParent, 1);
	CHECK(system->RuntimeStates[slot].Enabled);
	CHECK(system->RuntimeStates[slot].ContinuousRate == Catch::Approx(20.0f));
	CHECK_FALSE(system->RuntimeStates[slot].DeviceRetiring);
	CHECK(system->RetiringBlocks.empty());
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
	desc.Simulated = false;
	const Entity part = engine::scene::MakePart(store, desc);

	// Each wants 21 slots against a pool of 64, so the fourth cannot fit.
	std::vector<Entity> emitters;
	for (int index = 0; index < 4; index++) {
		const Entity emitter =
			store.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("ParticleEmitter")));
		emitters.push_back(emitter);
		store.SetParent(emitter, part);
		store.GetMutable<ParticleEmitter>(emitter)->Rate = 10.0f;
		store.GetMutable<ParticleEmitter>(emitter)->Lifetime = NumberRange{2.0f, 2.0f};
	}

	Frame(store, 0.0f);

	const auto *system = store.Resource<ParticleSystem>();
	REQUIRE(system->Statistics.EmittersRefused == 1);
	REQUIRE(system->Statistics.EmitterClaimAttempts == 4);

	// A full pool is a state, not a reason to run the same failed allocation
	// and warning path on every tick. The refusal remains visible in statistics.
	store.ClearChanges();
	Frame(store, 0.0f);
	system = store.Resource<ParticleSystem>();
	CHECK(system->Statistics.EmittersRefused == 1);
	CHECK(system->Statistics.EmitterClaimAttempts == 0);

	// Returning capacity wakes refused rows once. Reclaim follows claim within a
	// refresh, so the newly free range is deliberately consumed next frame.
	store.Destroy(emitters.front());
	store.ClearChanges();
	Frame(store, 0.0f);
	store.ClearChanges();
	Frame(store, 0.0f);
	system = store.Resource<ParticleSystem>();
	CHECK(system->Statistics.EmittersRefused == 0);
	CHECK(system->Statistics.EmitterClaimAttempts == 1);
	CHECK(store.Get<EmitterSlot>(emitters.back())->Index != NO_SLOT);

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

TEST_CASE("a growable pool expands once before refusing emitter blocks", "[effects]") {
	Store store("effects_test");
	engine::effects::RegisterEffectClasses();
	engine::effects::InstallParticles(store, 64, 128);

	engine::scene::PartDesc desc;
	desc.Simulated = false;
	const Entity part = engine::scene::MakePart(store, desc);

	for (int index = 0; index < 7; index++) {
		const Entity emitter =
			store.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("ParticleEmitter")));
		store.SetParent(emitter, part);
		store.GetMutable<ParticleEmitter>(emitter)->Rate = 10.0f;
		store.GetMutable<ParticleEmitter>(emitter)->Lifetime = NumberRange{2.0f, 2.0f};
	}

	Frame(store, 0.0f);

	const auto *system = store.Resource<ParticleSystem>();
	REQUIRE(system->Capacity == 128);
	REQUIRE(system->MaximumCapacity == 128);
	CHECK(system->Used == 126);
	CHECK(system->Instances.size() == 128);
	CHECK(system->States.size() == 128);
	CHECK(system->Statistics.EmittersRefused == 1);
	CHECK(system->Statistics.EmitterClaimAttempts == 7);

	store.ClearChanges();
	Frame(store, 0.0f);
	CHECK(store.Resource<ParticleSystem>()->Statistics.EmitterClaimAttempts == 0);
}

TEST_CASE("a device particle pool grows from no host allocation", "[effects]") {
	Store store("effects_test");
	engine::effects::RegisterEffectClasses();
	engine::effects::InstallParticles(store, 0, 128);
	store.ResourceMutable<ParticleSystem>()->DeviceStepped = true;

	const ParticleSystem *system = store.Resource<ParticleSystem>();
	REQUIRE(system != nullptr);
	CHECK(system->Capacity == 0);
	CHECK(system->Instances.empty());
	CHECK(system->States.empty());

	engine::scene::PartDesc desc;
	desc.Simulated = false;
	const Entity part = engine::scene::MakePart(store, desc);
	const Entity emitter =
		store.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("ParticleEmitter")));
	store.SetParent(emitter, part);
	store.GetMutable<ParticleEmitter>(emitter)->Rate = 10.0f;
	store.GetMutable<ParticleEmitter>(emitter)->Lifetime = NumberRange{2.0f, 2.0f};

	Frame(store, 0.0f);

	system = store.Resource<ParticleSystem>();
	CHECK(system->Capacity == 32);
	CHECK(system->MaximumCapacity == 128);
	CHECK(system->Used == 21);
	CHECK(system->Instances.empty());
	CHECK(system->States.empty());
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
	// lifetime - so the pool is full of particles about to expire.
	//
	// **Thirty-one and not thirty**, because a fresh block owes one particle
	// immediately: `RefreshEmitters` starts the accumulator at 1 so an emitter
	// enabled at a low rate does not go dark for `1 / Rate` seconds. One extra at
	// the very start, and the steady rate is unchanged.
	REQUIRE(store.Resource<ParticleSystem>()->Statistics.Live == 31);

	// **And then back to thirty, which is the claim worth pinning.** The starting
	// debt moves the *first* particle and nothing else: once the extra one has
	// aged out the population is rate times lifetime - sixty a second over half a
	// second - exactly as it was before the debt existed. An emitter that kept its
	// extra particle forever would be one whose rate quietly did not mean what it
	// says.
	const auto later = Frame(store, 1.0f / 60.0f);

	// The population is what this case pins. There used to be a
	// `REQUIRE(later.Retired >= 0)` above this line: `Retired` is a `uint32_t`,
	// so that was true of every value it could hold - a line that read as a
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

TEST_CASE("device emitters retain timers without a host planning walk", "[effects][device]") {
	Store store("effects_test");
	engine::effects::RegisterEffectClasses();
	constexpr uint32_t emitters = 600;
	engine::effects::InstallParticles(store, emitters * 6);
	store.ResourceMutable<ParticleSystem>()->DeviceStepped = true;

	engine::scene::PartDesc desc;
	desc.Simulated = false;
	const Entity part = engine::scene::MakePart(store, desc);
	const auto emitterClass = engine::ecs::Classes::Find(engine::core::Name("ParticleEmitter"));
	for (uint32_t index = 0; index < emitters; index++) {
		const Entity emitter = store.CreateInstance(emitterClass);
		store.SetParent(emitter, part);
		Settings(store, emitter).Rate = 5.0f;
		Settings(store, emitter).Lifetime = NumberRange{1.0f, 1.0f};
	}

	Frame(store, 1.0f / 60.0f);
	const ParticleSystem *system = store.Resource<ParticleSystem>();
	REQUIRE(system != nullptr);
	REQUIRE(system->RuntimeStates.size() == emitters);
	for (const engine::effects::EmitterRuntime &runtime : system->RuntimeStates) {
		CHECK(runtime.ContinuousRate == 5.0f);
		CHECK(runtime.Spawned == 0);
		CHECK(runtime.Pending == 1.0f);
	}

	// A second tick still does no work proportional to the emitter count. The
	// device runtime table advances these fields when the scene is presented.
	const uint64_t revision = system->PresentationRevision;
	Frame(store, 1.0f / 60.0f);
	system = store.Resource<ParticleSystem>();
	CHECK(system->PresentationRevision == revision + 1);
	CHECK(system->RuntimeStates.back().Spawned == 0);
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
	// would otherwise spend the difference showing nothing - an effect that
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

	// Semi-implicit Euler, so it has fallen - the exact distance is the
	// integrator's and is not what this pins.
	REQUIRE(fallen.Y < born.Y);
}

TEST_CASE("a particle samples the nearest vector field without an ECS lookup per particle", "[effects]") {
	Store store("effects_field");
	const Entity emitter = MakeEmitter(store);
	const Entity part = store.ParentOf(emitter);

	engine::scene::RegisterSceneClasses();
	const Entity field =
		store.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("VectorField3D")));
	store.SetParent(part, field);
	engine::scene::VectorField3D *description = store.GetMutable<engine::scene::VectorField3D>(field);
	REQUIRE(description != nullptr);
	description->Vector = Vector3{8.0f, 0.0f, 0.0f};
	description->LocalSpace = false;

	Settings(store, emitter).Rate = 1.0f;
	Settings(store, emitter).Lifetime = NumberRange{10.0f, 10.0f};
	Settings(store, emitter).Speed = NumberRange{0.0f, 0.0f};
	Settings(store, emitter).Shape = ParticleShape::Box;

	Frame(store, 0.1f);
	Frame(store, 0.1f);

	const ParticleSystem *system = store.Resource<ParticleSystem>();
	REQUIRE(system->Statistics.Live > 0);
	CHECK(system->States[0].Velocity.X == Catch::Approx(0.8f));
	CHECK(system->States[0].Velocity.Y == 0.0f);
	CHECK(system->States[0].Velocity.Z == 0.0f);
}

TEST_CASE("adding a field to an emitter ancestor refreshes retained particle blocks", "[effects]") {
	Store store("effects_field_refresh");
	const Entity emitter = MakeEmitter(store);
	const Entity part = store.ParentOf(emitter);

	Settings(store, emitter).Rate = 60.0f;
	Settings(store, emitter).Lifetime = NumberRange{10.0f, 10.0f};
	Settings(store, emitter).Speed = NumberRange{0.0f, 0.0f};
	Settings(store, emitter).Shape = ParticleShape::Box;

	Frame(store, 0.1f);
	ParticleSystem *system = store.ResourceMutable<ParticleSystem>();
	REQUIRE(system != nullptr);
	const uint32_t slot = store.Get<EmitterSlot>(emitter)->Index;
	REQUIRE(slot != NO_SLOT);
	REQUIRE(system->Blocks[slot].ForceField.Source == engine::ecs::NULL_ENTITY);

	engine::scene::VectorField3D field;
	field.Vector = Vector3{8.0f, 0.0f, 0.0f};
	field.LocalSpace = false;
	store.SetComponent(part, engine::ecs::Components::Of<engine::scene::VectorField3D>(), &field);

	Frame(store, 0.1f);
	REQUIRE(system->Blocks[slot].ForceField.Source == part);
	CHECK(system->States[0].Velocity.X == Catch::Approx(0.8f));
}

TEST_CASE("distance emission follows parent travel rather than frame time", "[effects]") {
	Store store("effects_test");
	const Entity emitter = MakeEmitter(store);

	Settings(store, emitter).Rate = 0.0f;
	Settings(store, emitter).RateOverDistance = 2.0f;
	Settings(store, emitter).Lifetime = NumberRange{10.0f, 10.0f};
	Settings(store, emitter).Speed = NumberRange{0.0f, 0.0f};

	CHECK(Frame(store, 0.0f).Emitted == 0);

	const Entity part = store.ParentOf(emitter);
	auto *transform = store.GetMutable<engine::scene::Transform>(part);
	REQUIRE(transform != nullptr);
	transform->Frame.Position.X += 3.0f;

	// Two particles per metre over three metres, with no elapsed time. A
	// time-rate implementation would emit none here.
	CHECK(Frame(store, 0.0f).Emitted == 6);
}

TEST_CASE("reparenting refreshes an emitter's cached frame", "[effects]") {
	Store store("effects_test");
	const Entity emitter = MakeEmitter(store);
	Frame(store, 0.0f);

	engine::scene::PartDesc desc;
	desc.Frame = CFrame{Vector3{12.0f, 3.0f, -7.0f}};
	desc.Simulated = false;
	const Entity destination = engine::scene::MakePart(store, desc);

	store.ClearChanges();
	REQUIRE(store.SetParent(emitter, destination));
	engine::effects::RefreshEmitters(store);

	const uint32_t slot = store.Get<EmitterSlot>(emitter)->Index;
	REQUIRE(slot != NO_SLOT);
	const auto &frame = store.Resource<ParticleSystem>()->Blocks[slot].Frame;
	CHECK(frame.Position == desc.Frame.Position);
}

TEST_CASE("moving an attachment refreshes its emitter's cached frame", "[effects]") {
	Store store("effects_test");
	const Entity emitter = MakeEmitter(store);
	const Entity part = store.ParentOf(emitter);
	const Entity point = store.CreateInstance(engine::scene::AttachmentClass(), "EmitterPoint");
	REQUIRE(store.SetParent(point, part));
	store.GetMutable<engine::scene::Attachment>(point)->Frame = CFrame{Vector3{0.0f, 2.0f, 0.0f}};
	REQUIRE(store.SetParent(emitter, point));
	Frame(store, 0.0f);

	store.ClearChanges();
	auto *transform = store.GetMutable<engine::scene::Transform>(part);
	REQUIRE(transform != nullptr);
	transform->Frame.Position.X = 9.0f;
	Frame(store, 0.0f);

	const uint32_t slot = store.Get<EmitterSlot>(emitter)->Index;
	REQUIRE(slot != NO_SLOT);
	const auto &frame = store.Resource<ParticleSystem>()->Blocks[slot].Frame;
	CHECK(frame.Position == Vector3{9.0f, 2.0f, 0.0f});
}

TEST_CASE("a disabled emitter accepts a burst and clear invalidates its block", "[effects]") {
	Store store("effects_test");
	const Entity emitter = MakeEmitter(store);

	Settings(store, emitter).Enabled = false;
	Settings(store, emitter).Rate = 0.0f;
	Settings(store, emitter).Lifetime = NumberRange{10.0f, 10.0f};

	REQUIRE(engine::effects::EmitParticles(store, emitter, 5));
	CHECK(Frame(store, 0.0f).Emitted == 5);
	CHECK(store.Resource<ParticleSystem>()->Statistics.Live == 5);

	REQUIRE(engine::effects::ClearParticles(store, emitter));
	Frame(store, 0.0f);
	CHECK(store.Resource<ParticleSystem>()->Statistics.Live == 0);
	CHECK(store.Get<EmitterSlot>(emitter)->Index == NO_SLOT);
}

TEST_CASE("changing MaxParticles reclaims the block at the new capacity", "[effects]") {
	Store store("effects_test");
	const Entity emitter = MakeEmitter(store);

	Settings(store, emitter).Enabled = false;
	Settings(store, emitter).Rate = 0.0f;
	Settings(store, emitter).MaxParticles = 2;
	REQUIRE(engine::effects::EmitParticles(store, emitter, 8));
	CHECK(Frame(store, 0.0f).Emitted == 2);

	const uint32_t firstSlot = store.Get<EmitterSlot>(emitter)->Index;
	REQUIRE(firstSlot != NO_SLOT);
	CHECK(store.Resource<ParticleSystem>()->Blocks[firstSlot].Capacity == 2);

	Settings(store, emitter).MaxParticles = 6;
	REQUIRE(engine::effects::EmitParticles(store, emitter, 6));
	CHECK(Frame(store, 0.0f).Emitted == 6);

	const uint32_t grownSlot = store.Get<EmitterSlot>(emitter)->Index;
	REQUIRE(grownSlot != NO_SLOT);
	CHECK(grownSlot == firstSlot);
	CHECK(store.Resource<ParticleSystem>()->Blocks[grownSlot].Capacity == 6);
}

TEST_CASE("capacity edits reuse the emitter runtime row", "[effects]") {
	Store store("effects_test");
	const Entity emitter = MakeEmitter(store);

	Settings(store, emitter).Enabled = false;
	Settings(store, emitter).Rate = 0.0f;
	for (int edit = 0; edit < 32; edit++) {
		Settings(store, emitter).MaxParticles = edit % 2 == 0 ? 2 : 3;
		REQUIRE(engine::effects::EmitParticles(store, emitter, 3));
		Frame(store, 0.0f);
		REQUIRE(store.Get<EmitterSlot>(emitter)->Index != NO_SLOT);
	}

	CHECK(store.Resource<ParticleSystem>()->Blocks.size() == 1);
}

TEST_CASE("resident force modules accelerate and cap a particle", "[effects]") {
	Store store("effects_test");
	const Entity emitter = MakeEmitter(store);

	Settings(store, emitter).Rate = 60.0f;
	Settings(store, emitter).Lifetime = NumberRange{10.0f, 10.0f};
	Settings(store, emitter).Speed = NumberRange{0.0f, 0.0f};
	Settings(store, emitter).Shape = ParticleShape::Sphere;
	Settings(store, emitter).RadialAcceleration = 20.0f;
	Settings(store, emitter).TangentialAcceleration = 10.0f;
	Settings(store, emitter).NoiseStrength = 5.0f;
	Settings(store, emitter).NoiseFrequency = 0.75f;
	Settings(store, emitter).NoiseScrollSpeed = 2.0f;
	Settings(store, emitter).MaxSpeed = 1.0f;

	Frame(store, 0.1f);
	Frame(store, 0.1f);

	const auto *system = store.Resource<ParticleSystem>();
	REQUIRE(system->Statistics.Live > 0);
	const float speed = system->States[0].Velocity.Magnitude();
	CHECK(speed > 0.9f);
	CHECK(speed <= 1.001f);
}

TEST_CASE("particle force controls survive component serialisation", "[effects]") {
	engine::effects::RegisterEffectComponents();

	ParticleEmitter emitter;
	emitter.RateOverDistance = 7.0f;
	emitter.MaxParticles = 321;
	emitter.MaxSpeed = 12.0f;
	emitter.NoiseStrength = 3.0f;
	emitter.NoiseFrequency = 0.25f;
	emitter.NoiseScrollSpeed = -2.0f;
	emitter.RadialAcceleration = 4.0f;
	emitter.TangentialAcceleration = -5.0f;

	const auto component = engine::ecs::Components::Find(engine::core::Name("effects.ParticleEmitter"));
	const engine::ecs::TypeDescriptor &type = engine::ecs::Components::Describe(component);
	engine::core::ByteWriter writer;
	type.Write(writer, &emitter, 1);

	ParticleEmitter restored;
	engine::core::ByteReader reader(writer.Bytes());
	type.Read(reader, &restored, 1);

	CHECK(restored.RateOverDistance == 7.0f);
	CHECK(restored.MaxParticles == 321);
	CHECK(restored.MaxSpeed == 12.0f);
	CHECK(restored.NoiseStrength == 3.0f);
	CHECK(restored.NoiseFrequency == 0.25f);
	CHECK(restored.NoiseScrollSpeed == -2.0f);
	CHECK(restored.RadialAcceleration == 4.0f);
	CHECK(restored.TangentialAcceleration == -5.0f);
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

TEST_CASE("a flipbook can start every particle at a deterministic random phase", "[effects]") {
	Store store("effects_test");
	const Entity emitter = MakeEmitter(store);

	Settings(store, emitter).Rate = 120.0f;
	Settings(store, emitter).Lifetime = NumberRange{5.0f, 5.0f};
	Settings(store, emitter).Speed = NumberRange{0.0f, 0.0f};
	Settings(store, emitter).Flipbook = FlipbookLayout::Grid4x4;
	Settings(store, emitter).FlipbookFrames = 16;
	Settings(store, emitter).FlipbookStartRandom = true;

	// The first-step cells are the test. A later animation can hide a failed
	// start phase because ordinary playback has already moved every particle.
	Frame(store, 0.1f);
	const auto *system = store.Resource<ParticleSystem>();
	REQUIRE(system->Statistics.Live > 8);

	std::set<uint32_t> cells;
	for (uint32_t index = 0; index < system->Statistics.Live; index++) {
		cells.insert(system->Instances[index].RotationAndCell >> 16);
	}
	CHECK(cells.size() > 1);
	const uint32_t initial = system->Instances[0].RotationAndCell >> 16;

	// A randomized start is a phase, not the Random playback mode: each Loop
	// particle still advances after birth.
	Settings(store, emitter).FlipbookPlayback = FlipbookMode::Loop;
	Settings(store, emitter).FlipbookFramerate = NumberRange{4.0f, 4.0f};
	Frame(store, 0.25f);

	const uint32_t advanced = system->Instances[0].RotationAndCell >> 16;
	CHECK(advanced == (initial + 1) % 16);
}

// --- the roadmap's number ----------------------------------------------------

TEST_CASE("the pool holds the scale the roadmap asks for", "[effects]") {
	// **Ten thousand emitters rather than the roadmap's hundred thousand**, and
	// the difference is what a *test* is for against what a *benchmark* is for.
	// This asserts the arithmetic holds at a scale where every block is checked
	// individually - a hundred thousand emitters at five particles each is a
	// three-second test that pins the same properties.
	// `engine.effects.bench.particles` is where the full count is measured.
	engine::parallel::Jobs::Start(0);

	Store store("effects_test");
	engine::effects::RegisterEffectClasses();

	constexpr uint32_t EMITTERS = 10000;
	constexpr uint32_t PER_EMITTER = 5;
	engine::effects::InstallParticles(store, EMITTERS * (PER_EMITTER + 1));

	engine::scene::PartDesc desc;
	desc.Simulated = false;

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
	for (size_t index = 0; index < system->Blocks.size(); index++) {
		const auto &block = system->Blocks[index];
		REQUIRE(system->RuntimeStates[index].Live <= block.Capacity);
		REQUIRE(block.First + block.Capacity <= system->Capacity);
	}

	engine::parallel::Jobs::Stop();
}

// --- what a flipbook takes from its texture, added at v0.10 -------------------

TEST_CASE("an emitter adopts the frame count its texture states", "[effects]") {
	// **The bug this closes played half a dance and looked fine.**
	// `fox_dance.gif` has forty-eight frames and the scene using it said
	// twenty-four, so the animation stopped at the halfway pose and held it -
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

	// Twenty-four cells hold a frame, so the last one drawn is 23 - not 63,
	// which is what the grid would give.
	CHECK(highest == 23);
}

TEST_CASE("an existing emitter adopts texture facts when content arrives", "[effects]") {
	Store store("effects_test");
	const Entity emitter = MakeEmitter(store);
	const engine::core::Name texture("effects/late.atex");

	Settings(store, emitter).Texture = texture;
	Settings(store, emitter).Flipbook = FlipbookLayout::Grid8x8;
	Frame(store, 0.0f);

	const uint32_t slot = store.Get<EmitterSlot>(emitter)->Index;
	REQUIRE(slot != NO_SLOT);
	const auto *system = store.Resource<ParticleSystem>();
	REQUIRE(system->Blocks[slot].Frames == 64);
	REQUIRE(system->Blocks[slot].FlipbookRate == 12.0f);

	// The emitter row stays untouched. The catalogue revision alone must
	// invalidate playback values cached before the content pump knew the file.
	store.ClearChanges();
	REQUIRE(
		engine::scene::RecordTexture(
			store, texture, engine::scene::FlipbookFacts{.Side = 8, .Frames = 24, .FrameRate = 30.0f}
		)
	);
	engine::effects::RefreshEmitters(store);

	CHECK(system->Blocks[slot].Frames == 24);
	CHECK(system->Blocks[slot].FlipbookRate == 30.0f);
}

TEST_CASE("what the emitter says beats what the texture says", "[effects]") {
	// **An author overriding a number means it.** The texture is the default,
	// not the authority - a scene deliberately playing the first eight cells of
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
	// off the particle's lifetime and ignores every rate - that is Roblox's
	// arrangement and it is unchanged - so a source's authored fps is for
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
	// whatever rate was used - which is the only thing that makes the one above
	// worth asserting.
	CHECK(static_cast<uint32_t>((29.0f / 60.0f) * 12.0f) == 5);
}

// --- cleanup ------------------------------------------------------------------

TEST_CASE("a destroyed emitter gives its block row back", "[effects]") {
	Store store("effects_test");

	// One emitter, so the pool and the classes exist, and it is the row every
	// later emitter should be handed.
	const Entity first = MakeEmitter(store, 4096);
	Settings(store, first).Rate = 10.0f;
	Settings(store, first).Lifetime = NumberRange{1.0f, 1.0f};
	Frame(store, 1.0f / 60.0f);

	const auto *system = store.Resource<engine::effects::ParticleSystem>();
	REQUIRE(system != nullptr);
	REQUIRE(system->Blocks.size() == 1);

	const Entity part = store.ParentOf(first);
	REQUIRE(part != engine::ecs::NULL_ENTITY);

	// **Twenty rounds of create-and-destroy, which is what a game does.** An
	// explosion, a muzzle flash and a footstep are each an emitter that exists
	// for a moment and goes; a scene that builds its emitters once and keeps
	// them - which is every scene in `examples/` - never reaches this at all,
	// and that is why it went unseen.
	for (int round = 0; round < 20; round++) {
		const Entity emitter =
			store.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("ParticleEmitter")));
		store.SetParent(emitter, part);
		Settings(store, emitter).Rate = 10.0f;
		Settings(store, emitter).Lifetime = NumberRange{1.0f, 1.0f};

		// Claimed on this frame.
		Frame(store, 1.0f / 60.0f);

		store.Destroy(emitter);

		// **Two frames to recycle, and that is the design rather than a
		// shortfall.** The reclaim sweep runs after the claim walk inside one
		// `RefreshEmitters`, so a row freed on this frame is offered on the
		// next - which is what stops a row being reused underneath the walk
		// that freed it.
		Frame(store, 1.0f / 60.0f);
		Frame(store, 1.0f / 60.0f);
	}

	// **Two, not twenty-one.** One row for the emitter that never went away and
	// one that the twenty short-lived ones passed between them. Before the free
	// list this was twenty-one, and `MAX_EMITTER_SLOTS` was therefore a cap on
	// emitters ever made rather than on emitters emitting at once.
	CHECK(system->Blocks.size() == 2);
	CHECK(system->FrameParents.size() == system->Blocks.size());
	CHECK(system->SpawnStates.size() == system->Blocks.size());
	CHECK(system->RuntimeStates.size() == system->Blocks.size());

	// And the surviving emitter still has its own row and is still emitting.
	CHECK(system->Statistics.Blocks == 1);
	CHECK(store.Get<engine::effects::EmitterSlot>(first)->Index != engine::effects::NO_SLOT);
}

// --- the device-stepped pool -------------------------------------------------
//
// The unit boundary can pin what crosses to the GPU without pretending to
// execute a shader. The visual suite covers emission and integration together.
TEST_CASE("a device-stepped pool sends emitter state instead of particle births", "[effects][device]") {
	Store store("effects_test");
	const Entity emitter = MakeEmitter(store);
	store.ResourceMutable<engine::effects::ParticleSystem>()->DeviceStepped = true;

	Settings(store, emitter).Rate = 60.0f;
	Settings(store, emitter).Lifetime = NumberRange{1.0f, 1.0f};

	Frame(store, 1.0f / 60.0f);
	const auto stats = Frame(store, 1.0f / 60.0f);

	const auto *system = store.Resource<engine::effects::ParticleSystem>();
	CHECK(stats.Emitted == 0);

	// **And the host pool is gone.** Neither array is read on this side once the
	// device owns it, so `StepParticles` releases both - fifty-four megabytes at
	// the client's default capacity that nothing would ever look at.
	CHECK(system->States.empty());
	CHECK(system->Instances.empty());

	REQUIRE(system->RuntimeStates.size() == 1);
	CHECK(system->RuntimeStates[0].ContinuousRate == 60.0f);
	CHECK(system->RuntimeStates[0].Spawned == 0);
}

TEST_CASE("device burst requests are resident deltas", "[effects][device]") {
	Store store("effects_test");
	const Entity emitter = MakeEmitter(store);
	store.ResourceMutable<engine::effects::ParticleSystem>()->DeviceStepped = true;

	Settings(store, emitter).Rate = 0.0f;
	Settings(store, emitter).Lifetime = NumberRange{0.25f, 0.25f};
	Frame(store, 1.0f / 60.0f);
	const auto *before = store.Resource<engine::effects::ParticleSystem>();
	const uint64_t residentRevision = before->ResidentRevision;
	const uint32_t blockRevision = before->Blocks[0].Revision;

	REQUIRE(engine::effects::EmitParticles(store, emitter, 7));
	const auto *after = store.Resource<engine::effects::ParticleSystem>();
	CHECK(after->RuntimeStates[0].Requested == 7);
	CHECK(after->ResidentRevision == residentRevision + 1);
	CHECK(after->Blocks[0].Revision == blockRevision + 1);

	REQUIRE(engine::effects::EmitParticles(store, emitter, 5));
	CHECK(store.Resource<engine::effects::ParticleSystem>()->RuntimeStates[0].Requested == 12);
}

TEST_CASE("a recycled device block disagrees with what it inherited", "[effects][device]") {
	Store store("effects_test");
	const Entity first = MakeEmitter(store);
	store.ResourceMutable<engine::effects::ParticleSystem>()->DeviceStepped = true;

	Settings(store, first).Rate = 60.0f;
	Settings(store, first).Lifetime = NumberRange{4.0f, 4.0f};

	Frame(store, 1.0f / 60.0f);
	Frame(store, 1.0f / 60.0f);

	auto *system = store.ResourceMutable<engine::effects::ParticleSystem>();
	const uint32_t generation = system->Blocks[0].Generation;
	const uint32_t row = system->Blocks[0].First;

	// The emitter goes and another takes its rows. Nothing clears them: the
	// particle written above is still in the array with four seconds left.
	const Entity part = store.ParentOf(first);
	store.Destroy(first);
	Frame(store, 1.0f / 60.0f);
	Frame(store, 1.0f / 60.0f);

	const Entity second =
		store.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("ParticleEmitter")));
	store.SetParent(second, part);
	Settings(store, second).Rate = 60.0f;
	Settings(store, second).Lifetime = NumberRange{4.0f, 4.0f};
	Frame(store, 1.0f / 60.0f);

	system = store.ResourceMutable<engine::effects::ParticleSystem>();
	const uint32_t slot = store.Get<engine::effects::EmitterSlot>(second)->Index;
	REQUIRE(slot != engine::effects::NO_SLOT);

	// **The same rows and a different number**, which is what the step compares.
	// Without this the new emitter would draw the old one's particles, with its
	// own curves, for the four seconds they had left - the device still holds
	// them, because nothing was cleared.
	CHECK(system->Blocks[slot].First == row);
	CHECK(system->Blocks[slot].Generation != generation);
}

TEST_CASE("a device block empties once nothing has been born for a lifetime", "[effects][device]") {
	Store store("effects_test");
	const Entity emitter = MakeEmitter(store);
	store.ResourceMutable<engine::effects::ParticleSystem>()->DeviceStepped = true;

	Settings(store, emitter).Rate = 60.0f;
	Settings(store, emitter).Lifetime = NumberRange{0.25f, 0.25f};

	for (int frame = 0; frame < 30; frame++) {
		Frame(store, 1.0f / 60.0f);
	}
	REQUIRE(store.Resource<engine::effects::ParticleSystem>()->RuntimeStates[0].Live > 0);

	// Turned off. The host cannot see a particle die, so this is the only thing
	// that ever brings `Live` back down - and without it a disabled emitter
	// holds its rows and goes on drawing its capacity in quads with no extent.
	Settings(store, emitter).Enabled = false;
	for (int frame = 0; frame < 30; frame++) {
		Frame(store, 1.0f / 60.0f);
	}

	CHECK(store.Resource<engine::effects::ParticleSystem>()->RuntimeStates[0].Live == 0);
}

TEST_CASE("a disabled device burst releases its block after its lifetime", "[effects][device]") {
	Store store("effects_test");
	const Entity emitter = MakeEmitter(store);
	store.ResourceMutable<engine::effects::ParticleSystem>()->DeviceStepped = true;

	Settings(store, emitter).Enabled = false;
	Settings(store, emitter).Rate = 0.0f;
	Settings(store, emitter).Lifetime = NumberRange{0.25f, 0.25f};
	REQUIRE(engine::effects::EmitParticles(store, emitter, 8));
	Frame(store, 1.0f / 60.0f);

	const auto *slot = store.Get<engine::effects::EmitterSlot>(emitter);
	REQUIRE(slot != nullptr);
	REQUIRE(slot->Index != engine::effects::NO_SLOT);
	REQUIRE(store.Resource<engine::effects::ParticleSystem>()->RetiringBlocks.size() == 1);

	for (int frame = 0; frame < 30; frame++) {
		Frame(store, 1.0f / 60.0f);
	}

	CHECK(store.Get<engine::effects::EmitterSlot>(emitter)->Index == engine::effects::NO_SLOT);
	CHECK(store.Resource<engine::effects::ParticleSystem>()->RetiringBlocks.empty());
}
