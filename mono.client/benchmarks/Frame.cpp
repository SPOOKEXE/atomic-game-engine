// What a client does per frame, before a single triangle reaches a device.
//
// **The frame's CPU work has no benchmark anywhere, and it is the largest CPU
// cost this program has.** `engine.scene.bench.ordering` knows what sorting a
// draw list costs and `engine.graph.bench.cull` knows what culling one costs,
// but both are handed a list that already exists. Building it is this module's,
// it happens in `Phase::PreRender`, and it runs **at the display's rate** rather
// than the tick's - so at 300 Hz it happens ten times for every tick of
// simulation the numbers elsewhere are quoted against.
//
// Four collectors run there and they are four different shapes of work:
//
// - **`CollectInstances`** walks every visible part, interpolates it between the
//   last two ticks and writes a `scene::DrawInstance`. This is the triangle
//   count's proxy and the one that scales with the scene.
// - **`CollectParticleBatches`** refreshes block data only when its resident
//   revision changes and authored batches only when emitter layout changes.
//   Both paths are measured below.
// - **`CollectLights`** resolves each light to where it shines from, which is a
//   walk to a parent, and then sorts to a cap.
// - **`CollectSurfaceViews`** finds every mirror, which is a scan that usually
//   finds nothing and must be cheap when it does.
//
// **Interpolation is why the instance row is not just a copy.** Every part is
// blended between the transform it had last tick and the one it has now, so the
// per-row cost is a quaternion slerp and a vector lerp rather than a memcpy -
// and at 300 Hz over 100,000 parts that is thirty million slerps a second. The
// rows walk the part count so the slope is visible rather than assumed.
//
// **Nothing here opens a window or touches a device.** These are ECS walks that
// write plain structs; what happens to those structs afterwards belongs to
// `render` and to the driver, and `engine.render.bench.overlay` says where that
// line is drawn and why.

#include <engine/core/Name.hpp>
#include <engine/core/Random.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/NumberRange.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/effects/ParticleSystem.hpp>
#include <engine/effects/Particles.hpp>
#include <engine/effects/Registration.hpp>
#include <engine/render/WorldPresentation.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Bench.hpp>

#include <client/ContentDemand.hpp>
#include <client/Scene.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

TEST_SUITE_ID("client.bench.frame")

using engine::core::CFrame;
using engine::core::Random;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Phase;
using engine::ecs::Scheduler;
using engine::ecs::Store;
using engine::testing::Consume;

namespace frame_bench {
	// Parts in the world. Ten thousand is a scene; a hundred thousand is what
	// the debug overlay's own entity counter is scaled for and what a large
	// place reaches.
	constexpr size_t SMALL = 10'000;
	constexpr size_t MEDIUM = 100'000;

	// A world and the systems that present it, built once per size.
	//
	// **The scheduler is held beside the store**, because `InstallPresentation`
	// registers into one and the systems close over nothing else - running the
	// phase is what drives `CollectInstances`, which is a private system rather
	// than a callable function and is right to be.
	struct Presented {
		size_t Parts = 0;
		std::unique_ptr<Store> World;
		std::unique_ptr<Scheduler> Systems;
	};

	Presented &Scene(size_t parts) {
		static std::vector<Presented> built;
		for (Presented &held : built) {
			if (held.Parts == parts) {
				return held;
			}
		}

		Presented made;
		made.Parts = parts;
		made.World = std::make_unique<Store>("client.bench.frame");
		made.Systems = std::make_unique<Scheduler>();

		client::RegisterClientComponents();
		engine::scene::RegisterSceneComponents();

		for (size_t index = 0; index < parts; index++) {
			const auto seed = static_cast<uint32_t>(index);
			const Entity entity = made.World->Create();
			made.World->Set<engine::scene::Transform>(
				entity,
				engine::scene::Transform{CFrame{Vector3{
					Random::Range(seed, 3, -256.0f, 256.0f),
					Random::Range(seed, 5, 0.0f, 64.0f),
					Random::Range(seed, 7, -256.0f, 256.0f),
				}}}
			);

			engine::scene::Bounds bounds;
			bounds.HalfExtent = Vector3{0.5f, 0.5f, 0.5f};
			made.World->Set<engine::scene::Bounds>(entity, bounds);
			made.World->Set<engine::scene::Visual>(entity, engine::scene::Visual{});
		}

		// Reserve what the list will hold, which is what a real client does -
		// a benchmark that let the vector grow inside the measured phase would
		// report the allocator on its first frame and nothing on the rest.
		client::InstallPresentation(*made.World, *made.Systems, static_cast<uint32_t>(parts));

		// Two ticks, so there is a previous transform to interpolate *from*.
		// The first frame of a world has nothing behind it and is the one frame
		// a running client never spends its time in.
		for (int tick = 0; tick < 2; tick++) {
			made.World->AdvanceTick(1.0f / 60.0f);
			made.Systems->RunPhases(*made.World, Phase::Input, Phase::Replication);
		}

		built.push_back(std::move(made));
		return built.back();
	}

	// A world whose particles are running, built once per emitter count.
	//
	// **Emitters rather than particles, because since v0.17 the device owns the
	// pool.** `CollectParticleBatches` emits one batch per emitter with a block
	// and the batch points *at the block*, so what this costs scales with the
	// emitter count and not with the half million particles they hold between
	// them. That is the claim the redesign was made on and this is where it
	// becomes a number.
	Store &Emitting(size_t emitters) {
		static std::vector<std::pair<size_t, std::unique_ptr<Store>>> built;
		for (auto &[count, store] : built) {
			if (count == emitters) {
				return *store;
			}
		}

		auto store = std::make_unique<Store>("client.bench.frame.particles");
		engine::effects::RegisterEffectClasses();
		engine::effects::InstallParticles(*store, static_cast<uint32_t>(emitters) * 6);

		const engine::ecs::ClassId emitterClass =
			engine::ecs::Classes::Find(engine::core::Name("ParticleEmitter"));

		engine::scene::PartDesc desc;
		desc.Simulated = false;
		for (size_t index = 0; index < emitters; index++) {
			desc.Frame = CFrame{
				Vector3{static_cast<float>(index % 320) * 4.0f, 0.0f, static_cast<float>(index / 320) * 4.0f}
			};
			const Entity part = engine::scene::MakePart(*store, desc);

			const Entity emitter = store->CreateInstance(emitterClass);
			store->SetParent(emitter, part);

			auto *settings = store->GetMutable<engine::effects::ParticleEmitter>(emitter);
			settings->Rate = 30.0f;
			settings->Lifetime = engine::core::NumberRange{2.0f, 2.0f};
			settings->Speed = engine::core::NumberRange{1.0f, 3.0f};
			settings->Acceleration = Vector3{0.0f, -4.0f, 0.0f};
		}

		// Run to the steady state, so the measured frames are the ones a running
		// game has rather than the ramp up to them.
		for (int frame = 0; frame < 90; frame++) {
			store->ClearChanges();
			engine::effects::RefreshEmitters(*store);
			engine::effects::StepParticles(*store, 1.0f / 60.0f);
		}

		built.emplace_back(emitters, std::move(store));
		return *built.back().second;
	}

	// One frame's presentation phase, which is what the client runs between two
	// ticks however many times the display asks for.
	size_t Present(Presented &scene) {
		scene.Systems->RunPhases(*scene.World, Phase::RenderPreparation, Phase::Render);
		const auto *list = scene.World->Resource<engine::render::DrawList>();
		return list == nullptr ? 0 : list->Instances.size();
	}
}

using namespace frame_bench;

// --- the draw list ------------------------------------------------------------
//
// Ten times the parts between these two rows. Ten times the cost is a collector
// that scales with the scene; anything steeper is a query that is not as cached
// as it looks or a vector reallocating inside the phase.

BENCH("PreRender · 10,000 parts", 1) {
	Consume(Present(Scene(SMALL)));
}

BENCH("PreRender · 100,000 parts", 1) {
	// **The row a frame budget is spent against.** At 300 Hz a millisecond here
	// is a third of the budget, and it is paid before culling, before ordering
	// and before anything is uploaded - all of which are measured elsewhere and
	// all of which take this list as their input.
	Consume(Present(Scene(MEDIUM)));
}

// --- what runs beside it ------------------------------------------------------

BENCH("CollectParticleBatches · a world with no emitters", 10'000) {
	// Most worlds have no particles. The first call establishes that fact and
	// render-rate repeats reuse it without scanning an empty emitter column.
	Presented &scene = Scene(SMALL);
	engine::render::ParticleFrame frame;
	size_t batches = 0;
	for (size_t call = 0; call < 10'000; call++) {
		batches += engine::render::CollectParticleBatches(*scene.World, frame);
	}
	Consume(batches);
}

BENCH("ContentDemand revision · unchanged world with 10,000 emitters", 10'000) {
	Store &store = Emitting(10'000);
	const uint64_t settled = client::WantedContentRevision(store);
	for (size_t call = 0; call < 10'000; call++) {
		Consume(client::WantedContentRevision(store) == settled);
	}
}

BENCH("ContentDemand scan · 10,000 emitters sharing one texture", 100) {
	Store &store = Emitting(10'000);
	std::vector<engine::core::Name> wanted;
	for (size_t call = 0; call < 100; call++) {
		wanted.clear();
		client::CollectWantedContent(store, wanted);
		Consume(wanted.size());
	}
}

BENCH("CollectParticleBatches · reuse 1,000 live emitters", 1000) {
	// The render-rate path between simulation revisions. This is one source-name
	// and revision comparison regardless of emitter count.
	Store &store = Emitting(1000);
	engine::render::ParticleFrame frame;
	engine::render::CollectParticleBatches(store, frame);
	size_t batches = 0;
	for (size_t call = 0; call < 1000; call++) {
		batches += engine::render::CollectParticleBatches(store, frame);
	}
	Consume(batches);
}

BENCH("CollectParticleBatches · refresh 1,000 resident emitters", 100) {
	Store &store = Emitting(1000);
	auto *system = store.ResourceMutable<engine::effects::ParticleSystem>();
	engine::render::ParticleFrame frame;
	size_t batches = 0;
	for (size_t call = 0; call < 100; call++) {
		system->PresentationRevision++;
		batches += engine::render::CollectParticleBatches(store, frame);
	}
	Consume(batches);
}

BENCH("CollectParticleBatches · refresh 10,000 resident emitters", 100) {
	// A simulation revision refreshes resident block pointers without
	// walking the authored emitter column or rebuilding material order.
	Store &store = Emitting(10'000);
	auto *system = store.ResourceMutable<engine::effects::ParticleSystem>();
	engine::render::ParticleFrame frame;
	size_t batches = 0;
	for (size_t call = 0; call < 100; call++) {
		system->PresentationRevision++;
		batches += engine::render::CollectParticleBatches(store, frame);
	}
	Consume(batches);
}

BENCH("CollectParticleBatches · rebuild 10,000 emitter layout", 100) {
	// This is the deliberately rare path: membership or material state changed,
	// so every emitter has to publish new batch metadata.
	Store &store = Emitting(10'000);
	auto *system = store.ResourceMutable<engine::effects::ParticleSystem>();
	engine::render::ParticleFrame frame;
	size_t batches = 0;
	for (size_t call = 0; call < 100; call++) {
		system->PresentationRevision++;
		system->LayoutRevision++;
		batches += engine::render::CollectParticleBatches(store, frame);
	}
	Consume(batches);
}

BENCH("ParticleFrame::Detach · 10,000 live emitters", 100) {
	// What the studio pays and the client does not. `Renderer::Render` happens
	// after `Universe::Enter` has returned there, so the batches have to stop
	// pointing into a world that may be stepping again. The first call copies the
	// blocks; later simulation-only revisions retain that detached storage because
	// no device table input changed.
	Store &store = Emitting(10'000);
	auto *system = store.ResourceMutable<engine::effects::ParticleSystem>();
	engine::render::ParticleFrame frame;
	size_t blocks = 0;
	for (size_t call = 0; call < 100; call++) {
		system->PresentationRevision++;
		engine::render::CollectParticleBatches(store, frame);
		frame.Detach();
		blocks += frame.Blocks.size();
	}
	Consume(blocks);
}

BENCH("CollectLights · a world with no lights", 10'000) {
	// The same shape, and the same reason: a world with no lamp in it still
	// pays this once a frame.
	Presented &scene = Scene(SMALL);
	std::vector<engine::render::SceneLight> lights;
	size_t found = 0;
	for (size_t call = 0; call < 10'000; call++) {
		found += engine::render::CollectLights(*scene.World, Vector3{0.0f, 0.0f, 0.0f}, lights);
	}
	Consume(found);
}

BENCH("CollectSurfaceViews · a world with no mirrors", 10'000) {
	// Zero is the ordinary case in a scene with no mirror in it, and is not a
	// failure - `Scene.hpp` says so. It is also the answer nearly every frame of
	// nearly every game gets, which is what makes the cost of arriving at it
	// worth a row.
	Presented &scene = Scene(SMALL);
	std::vector<engine::render::SurfaceView> views;
	size_t found = 0;
	for (size_t call = 0; call < 10'000; call++) {
		found += client::CollectSurfaceViews(*scene.World, views);
	}
	Consume(found);
}
