// Measures one tick of the shipped Luau rings scene, including its BulkMoveTo.

#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/testing/Bench.hpp>

#include <stdexcept>
#include <string>

TEST_SUITE_ID("engine.examples.bench.motion")
TEST_DEPENDS("engine.scripthost.scripting")

namespace {
	struct MotionFixture {
		engine::ecs::Store World{"examples.motion.bench"};
		engine::ecs::Scheduler Systems;

		MotionFixture() {
			std::string error;
			const std::string path = engine::examples::ExamplePath("Rings.luau");
			if (!engine::examples::LoadScene(World, Systems, path, error)) {
				throw std::runtime_error("could not load Rings.luau motion benchmark: " + error);
			}
		}

		void Tick() {
			Systems.Tick(World, 1.0f / 60.0f);
		}
	};

	MotionFixture &Fixture() {
		static MotionFixture fixture;
		return fixture;
	}
}

BENCH_PER_ITEM("Rings.luau scripted motion tick, 512 Parts", 512) {
	Fixture().Tick();
}
