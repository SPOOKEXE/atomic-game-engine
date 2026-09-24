// Measures QuickJS execution through the real ECS property binding surface.

#include <engine/ecs/Store.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scriptjs/Runtime.hpp>
#include <engine/testing/Bench.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>

TEST_SUITE_ID("engine.scriptjs.bench.property-access")

namespace {
	struct RuntimeFixture {
		engine::ecs::Store World{"scriptjs.property-access.bench"};
		std::unique_ptr<engine::script::Runtime> Runtime;

		explicit RuntimeFixture(const size_t operations) {
			engine::scene::EnsureClassTree();
			engine::scene::RegisterSceneComponents();
			engine::physics::RegisterPhysicsClasses();
			Runtime = engine::script::MakeJavaScriptRuntime(World);
			if (!Runtime->Run(
					"globalThis.benchPart = Instance.new('Part'); "
					"globalThis.benchStep = function() { "
					"let p = benchPart.Position; "
					"for (let i = 0; i < " +
						std::to_string(operations) +
						"; ++i) { "
						"p = Vector3.new(p.X + 0.01, p.Y, p.Z); benchPart.Position = p; "
						"p = benchPart.Position; } };",
					"property-access-setup.js"
				)) {
				throw std::runtime_error(
					"could not prepare JavaScript property benchmark: " + Runtime->LastError()
				);
			}
		}

		void Run() {
			if (!Runtime->Run("benchStep();", "property-access-bench.js")) {
				throw std::runtime_error("JavaScript property benchmark failed: " + Runtime->LastError());
			}
		}
	};

	RuntimeFixture &Fixture(const size_t operations) {
		static std::array<std::unique_ptr<RuntimeFixture>, 3> fixtures;
		constexpr std::array<size_t, 3> counts{64, 256, 1024};
		for (size_t index = 0; index < counts.size(); ++index) {
			if (counts[index] == operations) {
				if (fixtures[index] == nullptr)
					fixtures[index] = std::make_unique<RuntimeFixture>(operations);
				return *fixtures[index];
			}
		}
		throw std::runtime_error("unknown JavaScript property fixture size");
	}
}

BENCH_PER_ITEM("QuickJS bound Position read and write, 64 cycles", 64) {
	Fixture(64).Run();
}

BENCH_PER_ITEM("QuickJS bound Position read and write, 256 cycles", 256) {
	Fixture(256).Run();
}

BENCH_PER_ITEM("QuickJS bound Position read and write, 1024 cycles", 1024) {
	Fixture(1024).Run();
}
