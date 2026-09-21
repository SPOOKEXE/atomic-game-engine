// Runtime compilation of every authored SkyGrid benchmark shader.
//
// The benchmark owns its GLSL as Luau strings, so compiling copied source here
// would only prove a second, stale version. Load the staged script instead and
// run `ShaderLibrary`, the exact name-to-SPIR-V route the client refreshes.

#include <engine/core/Paths.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/render/ShaderLibrary.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>

TEST_SUITE_ID("engine.render.benchmark-skygrid")
TEST_DEPENDS("engine.examples.benchmark-skygrid")

namespace {
	struct StagedAssets {
		std::filesystem::path Previous = engine::core::Paths::Assets();

		StagedAssets() {
			engine::core::Paths::SetAssetsOverride(engine::core::Paths::Base().parent_path() / "assets");
		}

		~StagedAssets() {
			engine::core::Paths::SetAssetsOverride(Previous);
		}
	};
}

TEST_CASE("SkyGrid's eight selected shader variants compile through ShaderLibrary", "[render][shaders][benchmark]") {
	const StagedAssets assets;
	engine::ecs::Store store("render.benchmark.skygrid");
	engine::ecs::Scheduler systems;
	std::shared_ptr<engine::script::Runtime> runtime;
	std::string error;
	const bool loaded =
		engine::examples::LoadScene(
			store, systems, engine::examples::ExamplePath("BenchmarkSkyGrid.luau"), error, &runtime
		);
	INFO(error);
	REQUIRE(loaded);
	REQUIRE(runtime != nullptr);

	for (size_t tick = 0; tick < 16; tick++) {
		systems.Tick(store, 1.0f / 60.0f);
	}
	REQUIRE(runtime->LastError().empty());

	engine::render::ShaderLibrary library;
	REQUIRE(library.Refresh(store, engine::core::Name("benchmark.skygrid")) == 8);
	const std::array shaderNames{
		"SkyGridBasalt",
		"SkyGridAmber",
		"SkyGridVerdant",
		"SkyGridCobalt",
		"SkyGridViolet",
		"SkyGridCopper",
		"SkyGridGlacier",
		"SkyGridEmber",
	};
	for (const char *shaderName : shaderNames) {
		const engine::render::ShaderModule *module =
			library.Find(engine::core::Name(shaderName), engine::core::Name("benchmark.skygrid"));
		REQUIRE(module != nullptr);
		INFO(shaderName);
		CHECK(module->Error.empty());
		CHECK(module->AttemptError.empty());
		CHECK_FALSE(module->SpirV.empty());
		CHECK_FALSE(module->BuiltIn);
	}

	// A second refresh is the profiling steady state: no source is compiled
	// again after all eight variants have been accepted.
	CHECK(library.Refresh(store, engine::core::Name("benchmark.skygrid")) == 0);
}
