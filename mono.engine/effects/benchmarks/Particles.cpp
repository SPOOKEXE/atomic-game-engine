// What `ROADMAP.md` v0.10's number actually costs.
//
// **The roadmap asks for 100,000 emitters with at least five particles each**,
// which is half a million particles a frame, and this is the suite that says
// whether that is true rather than plausible. Every architectural decision in
// `ParticleSystem.hpp` - the pooled blocks, the sampled curves, the split
// instance and state arrays, the block-local retirement - was taken against this
// number, so a reading here that disagrees with one of those comments should win
// and the comment should be corrected.
//
// The measurement is split three ways because "why is the particle system slow"
// has three different answers and one number cannot tell them apart:
//
// - **Refresh** walks the `ParticleEmitter` column, which is about 1.5 KB a row.
//   This is the pass whose cost is proportional to emitter *count* and to nothing
//   else, and it is the one that would go wrong first if a hot loop ever started
//   reading the authored component.
// - **Step** walks the blocks in parallel and is proportional to particles
//   *alive*. This is the loop the module exists for.
// - **Spawn** is inside the step and is proportional to particles *born*, which
//   in a steady scene is alive-over-lifetime and therefore much smaller. It is
//   the serial half, and this is where that decision is checked.
//
// What it measured, in the `bench` preset, on a 24-thread machine. Figures are
// the minimum sample per iteration:
//
// | Row | Cost |
// |---|---|
// | Frame · 1,000 emitters · 5,000 particles | 0.50 us |
// | Frame · 10,000 emitters · 50,000 particles | 21.3 us |
// | **Frame · 100,000 emitters · 500,000 particles** | **583 us** |
// | Refresh only · 100,000 emitters | 304 us |
// | Step only · 100,000 emitters · 500,000 particles | 290 us |
// | Step at zero delta · 100,000 emitters | 285 us |
//
// **The roadmap's number holds with room to spare: 583 microseconds is 3.5 per
// cent of a 60 Hz frame.** The scaling from 10,000 to 100,000 is 27x for 10x the
// work. That cache cost is visible, but it remains far below quadratic growth.
//
// **Two findings came out of this suite rather than out of reading the code.**
//
// - **The refresh pass used to be 70 per cent of the frame.** It was re-sampling
//   four curves per emitter per frame for tables that almost never change.
//   Gating that on `Store::Changed<ParticleEmitter>` remains the single largest
//   optimisation in the module. The old 522 to 192 us comparison actually
//   measured 65,535 emitters because the former uint16 slot cap silently refused
//   the rest. At the true 100,000 target the gated refresh is 304 us.
// - **The spawn half really is free.** "Step at zero delta" ages nothing and
//   spawns nothing and costs 285 us, which is within noise of the full step's
//   290 us. So the entire cost of the step is the walk and the per-particle
//   arithmetic, and the serial spawn loop - the one deliberate serialisation in
//   the module - does not appear in the measurement at all. That is the
//   justification `StepParticles` claims for it, confirmed rather than asserted.
//
// **A harness bug is recorded here rather than quietly fixed**, because it is the
// kind that produces a confident wrong number: the first version of `Frame` did
// not call `ClearChanges`, so the dirty bits the gate reads were never cleared,
// so the gate never fired and the "after" measurement was the "before" one.
// `world::World::Tick` clears at the start of a tick; a benchmark that models a
// frame has to model that too.

#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/NumberRange.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/effects/ParticleSystem.hpp>
#include <engine/effects/Registration.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/scene/Part.hpp>
#include <engine/testing/Bench.hpp>

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.effects.bench.particles")

using engine::core::CFrame;
using engine::core::Name;
using engine::core::NumberRange;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::effects::ParticleEmitter;
using engine::effects::ParticleSystem;
using engine::testing::Consume;

namespace particle_bench {
	// How many particles each emitter sustains.
	//
	// Five, which is the roadmap's floor. The block sizing is `Rate * Lifetime`,
	// so a rate of five over a one-second life is five slots plus the accumulator's
	// spare - and that is what makes the pool exactly as big as the target.
	constexpr float RATE = 5.0f;
	constexpr float LIFETIME = 1.0f;

	// A world of `count` emitters, built once and reused.
	//
	// Lazily rather than at static-initialisation time, because a store binds its
	// owning thread on construction - `physics/benchmarks/Broadphase.cpp`'s
	// reason, and it is the same store.
	//
	// **Every emitter is on its own part**, which is what the shape actually is:
	// an emitter draws its spawn volume from its parent's `Bounds`, so a hundred
	// thousand emitters sharing one part would be measuring a case nobody builds
	// and would also make every spawn read one cache line.
	Store &WorldOf(size_t count) {
		static std::vector<std::pair<size_t, std::unique_ptr<Store>>> built;

		for (auto &[key, store] : built) {
			if (key == count) {
				return *store;
			}
		}

		auto store = std::make_unique<Store>("effects.bench.particles");
		engine::effects::RegisterEffectClasses();

		// One spare slot an emitter, for the frame the accumulator rounds up on.
		engine::effects::InstallParticles(*store, static_cast<uint32_t>(count) * 6);

		const engine::ecs::ClassId emitterClass = engine::ecs::Classes::Find(Name("ParticleEmitter"));

		engine::scene::PartDesc desc;
		desc.Simulated = false;

		for (size_t index = 0; index < count; index++) {
			// Spread over a grid so the scene is a world rather than a point. The
			// step does not read a position, so this changes nothing it measures -
			// it is here so that a profile taken against this suite looks like a
			// profile taken against a scene.
			desc.Frame = CFrame{
				Vector3{static_cast<float>(index % 320) * 4.0f, 0.0f, static_cast<float>(index / 320) * 4.0f}
			};
			const Entity part = engine::scene::MakePart(*store, desc);

			const Entity emitter = store->CreateInstance(emitterClass);
			store->SetParent(emitter, part);

			auto *settings = store->GetMutable<ParticleEmitter>(emitter);
			settings->Rate = RATE;
			settings->Lifetime = NumberRange{LIFETIME, LIFETIME};
			settings->Speed = NumberRange{1.0f, 3.0f};
			settings->Acceleration = Vector3{0.0f, -4.0f, 0.0f};
		}

		// Run to the steady state once, so the measured frames are the ones a
		// running game has rather than the ramp.
		for (int frame = 0; frame < 90; frame++) {
			store->ClearChanges();
			engine::effects::RefreshEmitters(*store);
			engine::effects::StepParticles(*store, 1.0f / 60.0f);
		}

		built.emplace_back(count, std::move(store));
		return *built.back().second;
	}

	// Both passes, which is what a frame actually costs.
	//
	// **`ClearChanges` first, because a real tick does it first.**
	// `world::World::Tick` clears at the *start* of a tick, and leaving it out
	// here was not a small omission: `RefreshEmitters` gates its curve sampling
	// on `Store::Changed<ParticleEmitter>`, and a dirty bit that is never cleared
	// is a gate that never fires. The first version of this suite measured the
	// ungated path and reported it as the gated one.
	uint32_t Frame(Store &store) {
		store.ClearChanges();
		engine::effects::RefreshEmitters(store);
		return engine::effects::StepParticles(store, 1.0f / 60.0f).Live;
	}
}

using namespace particle_bench;

// --- the ladder --------------------------------------------------------------
//
// Powers of ten, so the shape of the curve is readable rather than a single
// point. The step is parallel over blocks with a dispatch floor of 256, so the
// 1,000 row is above the floor and the 100 row would not be - which is why the
// ladder starts where it does.

BENCH("Frame · 1,000 emitters · 5,000 particles", 200) {
	Store &store = WorldOf(1000);
	Consume(Frame(store));
}

BENCH("Frame · 10,000 emitters · 50,000 particles", 50) {
	Store &store = WorldOf(10000);
	Consume(Frame(store));
}

// **The roadmap's row.** A hundred thousand emitters at five particles each is
// what v0.10 asks for by name, and this is the figure that says whether it holds.
BENCH("Frame · 100,000 emitters · 500,000 particles", 20) {
	Store &store = WorldOf(100000);
	Consume(Frame(store));
}

// --- the halves, at the target count ------------------------------------------
//
// The same scene, one pass at a time, so a slow frame can be attributed. Both
// rows below sum to the row above; a large gap between the sum and the whole is
// itself a finding.

BENCH("Refresh only · 100,000 emitters", 20) {
	Store &store = WorldOf(100000);
	store.ClearChanges();
	Consume(engine::effects::RefreshEmitters(store));
}

BENCH("Step only · 100,000 emitters · 500,000 particles", 20) {
	Store &store = WorldOf(100000);
	Consume(engine::effects::StepParticles(store, 1.0f / 60.0f).Live);
}

// **The serial spawn half, isolated.** `StepParticles` runs its ageing loop in
// parallel and its spawn loop serially, and the argument for that is that a
// steady scene spawns alive-over-lifetime particles a frame - a hundred thousand
// a second here, which is under two thousand a frame. This row is the check on
// that argument: a step at a delta of zero ages nothing and spawns nothing, so
// the difference between it and the row above is the work rather than the walk.
BENCH("Step at zero delta · 100,000 emitters", 20) {
	Store &store = WorldOf(100000);
	Consume(engine::effects::StepParticles(store, 0.0f).Live);
}
