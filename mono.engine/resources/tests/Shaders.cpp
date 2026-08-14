#include <engine/core/Paths.hpp>
#include <engine/resources/Shaders.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.resources.shaders")

namespace fs = std::filesystem;
using engine::core::Paths;
using engine::resources::Shader;
using engine::resources::ShaderForm;

namespace {
	// The staged tree the build writes, spelled out rather than derived from
	// the header, so that a change to either end has to be made here too.
	constexpr const char *STAGED = "shaders/resources";

	// The override is process-wide state; a case that leaves it set breaks
	// every later one — `engine.core.paths` carries the same guard.
	struct Override {
		explicit Override(const fs::path &path) {
			Paths::SetAssetsOverride(path);
		}
		~Override() {
			Paths::SetAssetsOverride({});
		}
	};
}

TEST_CASE("a built-in shader resolves under the staged module directory", "[resources]") {
	const Override elsewhere("/nonexistent/atomic");

	REQUIRE(
		Shader("opaque.vert", ShaderForm::SpirV) ==
		fs::path("/nonexistent/atomic") / STAGED / "opaque.vert.spv"
	);

	// The suffix is added here and nowhere else, which is half the reason this
	// function exists: a caller that spelled it as well would ask for
	// `opaque.vert.spv.spv` and get a "shader not found" a long way from the typo.
	REQUIRE(Shader("opaque.vert", ShaderForm::SpirV).extension() == ".spv");
	REQUIRE(Shader("opaque.vert", ShaderForm::SpirV) != Shader("opaque.frag", ShaderForm::SpirV));
}

// The two forms are staged side by side and a device takes one of them. Asking
// for the wrong one is a file that is there and bytes SDL will not accept, which
// is why they differ in the suffix and in nothing else.
TEST_CASE("the two compiled forms are one name apart", "[resources]") {
	const Override elsewhere("/nonexistent/atomic");

	REQUIRE(
		Shader("opaque.frag", ShaderForm::Msl) == fs::path("/nonexistent/atomic") / STAGED / "opaque.frag.msl"
	);
	REQUIRE(Shader("opaque.frag", ShaderForm::Msl).extension() == ".msl");
	REQUIRE(
		Shader("opaque.frag", ShaderForm::Msl).parent_path() ==
		Shader("opaque.frag", ShaderForm::SpirV).parent_path()
	);
}

TEST_CASE("an assets override moves the built-in shaders with it", "[resources]") {
	const fs::path beside = Shader("shadow.frag", ShaderForm::SpirV);

	{
		const Override elsewhere("/elsewhere");
		REQUIRE(
			Shader("shadow.frag", ShaderForm::SpirV) == fs::path("/elsewhere") / STAGED / "shadow.frag.spv"
		);
		REQUIRE(Shader("shadow.frag", ShaderForm::SpirV) != beside);
	}

	REQUIRE(Shader("shadow.frag", ShaderForm::SpirV) == beside);
}
