// Runs the example scene's installed motion systems over a growing ECS world.

#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Bench.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <stdexcept>

TEST_SUITE_ID("engine.examples.bench.motion")

namespace {
	using engine::ecs::Entity;
	using engine::ecs::Scheduler;
	using engine::ecs::Store;
	using engine::examples::Orbit;
	using engine::examples::Spin;
	using engine::scene::Transform;

	struct MotionFixture {
		Store World{"examples.motion.bench"};
		Scheduler Systems;

		explicit MotionFixture(const size_t count) {
			engine::examples::RegisterExampleComponents();
			engine::scene::RegisterSceneClasses();
			for (size_t index = 0; index < count; ++index) {
				const Entity entity = World.CreateInstance(engine::scene::PartClass());
				if (entity == engine::ecs::NULL_ENTITY) {
					throw std::runtime_error("could not create example motion benchmark Part");
				}
				World.Set(entity, Transform{});
				World.Set(
					entity,
					Orbit{
						.Centre = {},
						.Radius = 8.0f + static_cast<float>(index % 97),
						.RadiansPerSecond = 0.5f + static_cast<float>(index % 11) * 0.1f,
						.Phase = static_cast<float>(index % 360) * 0.0174532925f,
						.Height = static_cast<float>(index % 13),
					}
				);
				World.Set(entity, Spin{.Rate = {0.1f, 0.2f + static_cast<float>(index % 7) * 0.05f, 0.3f}});
			}
			engine::examples::InstallMotionSystems(Systems);
		}

		void Tick() {
			Systems.Tick(World, 1.0f / 60.0f);
		}
	};

	MotionFixture &Fixture(const size_t count) {
		static std::array<std::unique_ptr<MotionFixture>, 3> fixtures;
		constexpr std::array<size_t, 3> counts{128, 1024, 4096};
		for (size_t index = 0; index < counts.size(); ++index) {
			if (counts[index] == count) {
				if (fixtures[index] == nullptr) fixtures[index] = std::make_unique<MotionFixture>(count);
				return *fixtures[index];
			}
		}
		throw std::runtime_error("unknown example motion fixture size");
	}
}

BENCH_PER_ITEM("example motion tick, 128 orbiting and spinning Parts", 128) {
	Fixture(128).Tick();
}

BENCH_PER_ITEM("example motion tick, 1024 orbiting and spinning Parts", 1024) {
	Fixture(1024).Tick();
}

BENCH_PER_ITEM("example motion tick, 4096 orbiting and spinning Parts", 4096) {
	Fixture(4096).Tick();
}
