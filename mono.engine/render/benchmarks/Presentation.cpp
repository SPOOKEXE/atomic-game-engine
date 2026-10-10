// CPU cost of publishing a world whose particles move while its parts do not.
//
// StressParticles carries 1,024 visible hosts. The particle system changes each
// frame, but none of those host rows do, so this suite keeps the object source
// still and measures both the rebuild and the individual-cache hit.
//
// On the 24-thread development machine in the `bench` preset, a full rebuild
// measured 20.06 us and the unchanged-source gate measured 228 ns. The warm
// path therefore removes 98.9 per cent of the object publication cost.

#include <engine/ecs/Store.hpp>
#include <engine/render/WorldPresentation.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Visibility.hpp>
#include <engine/testing/Bench.hpp>

#include <cstddef>
#include <vector>

TEST_SUITE_ID("engine.render.bench.presentation")

namespace presentation_bench {
	constexpr size_t MOVING_PART_COUNT = 100'000;

	struct StaticParts {
		engine::ecs::Store World{"bench.presentation"};
		engine::ecs::Entity First;

		StaticParts() {
			engine::scene::RegisterSceneClasses();
			engine::render::RegisterPresentationComponents();
			World.SetResource(engine::render::DrawList{});
			const engine::ecs::Entity workspace = engine::scene::InstallServices(World);
			for (size_t index = 0; index < 1'024; index++) {
				const engine::ecs::Entity part = engine::scene::MakePart(World, engine::scene::PartDesc{});
				(void)World.SetParent(part, workspace);
				if (index == 0) {
					First = part;
				}
			}
			(void)engine::scene::SyncRendered(World);
			engine::render::CollectInstances(World);
			World.ClearChanges();
		}
	};

	struct MovingParts {
		engine::ecs::Store World{"bench.presentation.moving"};
		std::vector<engine::ecs::Entity> Parts;

		MovingParts() {
			engine::scene::RegisterSceneClasses();
			engine::render::RegisterPresentationComponents();
			World.SetResource(engine::render::DrawList{});
			const engine::ecs::Entity workspace = engine::scene::InstallServices(World);
			Parts.reserve(MOVING_PART_COUNT);
			for (size_t index = 0; index < MOVING_PART_COUNT; index++) {
				engine::scene::PartDesc desc;
				desc.Simulated = true;
				desc.Frame.Position = {
					static_cast<float>(index % 160) * 1.5f,
					static_cast<float>((index / 25'600) % 4) * 1.5f,
					static_cast<float>((index / 160) % 160) * 1.5f,
				};
				const engine::ecs::Entity part = engine::scene::MakePart(World, desc);
				(void)World.SetParent(part, workspace);
				Parts.push_back(part);
			}
			(void)engine::scene::SyncRendered(World);
			engine::render::CollectInstances(World);
			World.ClearChanges();
		}

		void AdvanceAndCollect() {
			for (const engine::ecs::Entity part : Parts) {
				auto transform = *World.Get<engine::scene::Transform>(part);
				transform.Frame.Position.X += 0.001f;
				World.Set(part, transform);
			}
			engine::render::CollectInstances(World);
			World.ClearChanges();
		}
	};

	StaticParts &CachedWorld() {
		static StaticParts world;
		return world;
	}

	StaticParts &RebuiltWorld() {
		static StaticParts world;
		return world;
	}

	MovingParts &StressPhysicsWorld() {
		static MovingParts world;
		return world;
	}
}

BENCH("CollectInstances · 1,024 static parts, cached", 10'000) {
	auto &bench = presentation_bench::CachedWorld();
	for (size_t pass = 0; pass < 10'000; pass++) {
		engine::render::CollectInstances(bench.World);
	}
}

BENCH("CollectInstances · 1,024 static parts, rebuild", 1'000) {
	auto &bench = presentation_bench::RebuiltWorld();
	for (size_t pass = 0; pass < 1'000; pass++) {
		auto visual = *bench.World.Get<engine::scene::Visual>(bench.First);
		visual.Tint.R = (pass & 1u) == 0 ? 0.25f : 0.75f;
		bench.World.Set(bench.First, visual);
		engine::render::CollectInstances(bench.World);
	}
}

BENCH("CollectInstances · 1,024 static parts, pose epoch", 1'000) {
	auto &bench = presentation_bench::CachedWorld();
	for (size_t pass = 0; pass < 1'000; pass++) {
		bench.World.MarkChanged<engine::scene::Transform>(bench.First);
		engine::render::CollectInstances(bench.World);
	}
}

// Models the presentation input writes, not physics integration or GPU residency.
BENCH("StressPhysics presentation · 100,000 transform writes plus collection", 1) {
	presentation_bench::StressPhysicsWorld().AdvanceAndCollect();
}
