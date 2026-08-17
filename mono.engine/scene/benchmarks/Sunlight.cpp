// The cost of resolving authored lighting once per presented world and frame.
//
// The resolver walks the small root service set, evaluates the solar arc, and
// copies a value into the renderer. This row keeps that fixed cost visible as
// worlds are added to one client.

#include <engine/ecs/Store.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Sunlight.hpp>
#include <engine/testing/Bench.hpp>

#include <cstdint>

TEST_SUITE_ID("engine.scene.bench.sunlight")

namespace {
	engine::ecs::Store &World() {
		static engine::ecs::Store store("bench.sunlight");
		static const bool ready = [] {
			engine::scene::RegisterSceneClasses();
			engine::scene::InstallServices(store);
			return true;
		}();
		(void)ready;
		return store;
	}
}

BENCH_PER_ITEM("LightingOf · furnished world", 100'000) {
	engine::ecs::Store &store = World();
	const engine::ecs::Entity service = store.FindFirstRoot("Lighting");
	auto *authored = store.GetMutable<engine::scene::LightingServiceComponent>(service);

	for (uint32_t frame = 0; frame < 100'000; frame++) {
		// Vary a real input so the optimiser cannot hoist one immutable answer
		// out of the measured frame loop.
		authored->ClockTime = static_cast<float>(frame % 24u);
		engine::testing::Consume(engine::scene::LightingOf(store));
	}
}
