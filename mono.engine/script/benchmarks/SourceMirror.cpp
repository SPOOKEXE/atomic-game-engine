// Measures the unchanged per-tick mirror scan over cached script instances.

#include <engine/core/Name.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/testing/Bench.hpp>

#include <array>
#include <memory>
#include <stdexcept>
#include <string>

TEST_SUITE_ID("engine.script.bench.source-mirror")

namespace {
	struct MirrorFixture {
		engine::ecs::Store World{"script.source-mirror.bench"};
		engine::script::SourceMirror Mirror;

		explicit MirrorFixture(const size_t count) {
			engine::script::ScriptClass();
			engine::script::SourceCache cache;
			for (size_t index = 0; index < count; ++index) {
				const std::string path = "bench/scripts/" + std::to_string(index) + ".luau";
				cache.Set(engine::core::Name(path), "return 1\n");
				if (engine::script::MakeScript(World, path, "Script" + std::to_string(index), true) ==
					engine::ecs::NULL_ENTITY) {
					throw std::runtime_error("could not create source-mirror benchmark script");
				}
			}
			World.SetResource(std::move(cache));
			engine::script::MirrorSourcePrograms(World, Mirror);
			size_t programCount = 0;
			World.Each<const engine::script::Program>(
				[&programCount](engine::ecs::Entity, const engine::script::Program &) { ++programCount; }
			);
			if (programCount != count) {
				throw std::runtime_error("source-mirror benchmark did not populate every cached script");
			}
		}

		void Tick() {
			engine::script::MirrorSourcePrograms(World, Mirror);
		}
	};

	MirrorFixture &Fixture(const size_t count) {
		static std::array<std::unique_ptr<MirrorFixture>, 3> fixtures;
		constexpr std::array<size_t, 3> counts{64, 512, 2048};
		for (size_t index = 0; index < counts.size(); ++index) {
			if (counts[index] == count) {
				if (fixtures[index] == nullptr) fixtures[index] = std::make_unique<MirrorFixture>(count);
				return *fixtures[index];
			}
		}
		throw std::runtime_error("unknown source-mirror fixture size");
	}
}

BENCH_PER_ITEM("unchanged source mirror tick, 64 cached scripts", 64) {
	Fixture(64).Tick();
}

BENCH_PER_ITEM("unchanged source mirror tick, 512 cached scripts", 512) {
	Fixture(512).Tick();
}

BENCH_PER_ITEM("unchanged source mirror tick, 2048 cached scripts", 2048) {
	Fixture(2048).Tick();
}
