#include <engine/core/Paths.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.core.paths")

namespace fs = std::filesystem;
using engine::core::Paths;

namespace {
	// The override is process-wide state set once at startup. A test that
	// leaves it set breaks every later one, so every case that touches it
	// restores.
	struct Override {
		explicit Override(const fs::path &path) {
			Paths::SetAssetsOverride(path);
		}
		~Override() {
			Paths::SetAssetsOverride({});
		}
	};
}

TEST_CASE("the base path is the directory holding the binary", "[paths]") {
	const fs::path &base = Paths::Base();

	REQUIRE_FALSE(base.empty());
	REQUIRE(fs::is_directory(base));
	// The test binary stages into <build>/tests/, and everything resolves
	// relative to the executable rather than the working directory.
	REQUIRE(fs::exists(base));
}

TEST_CASE("the base path is stable", "[paths]") {
	// Resolved once and cached: the answer cannot change while the process
	// lives, and callers hold the reference.
	const fs::path *first = &Paths::Base();
	const fs::path *second = &Paths::Base();

	REQUIRE(first == second);
	REQUIRE(*first == *second);
}

TEST_CASE("assets defaults to the base path", "[paths]") {
	REQUIRE(Paths::Assets() == Paths::Base());
}

TEST_CASE("shaders sit under the owning module's own name", "[paths]") {
	// Two modules must not be able to collide on fullscreen.vert, which is why
	// the module name is part of the path rather than one flat directory.
	REQUIRE(Paths::Shaders("render") == Paths::Assets() / "shaders" / "render");
	REQUIRE(Paths::Shaders("vfx") == Paths::Assets() / "shaders" / "vfx");
	REQUIRE(Paths::Shaders("render") != Paths::Shaders("vfx"));
}

TEST_CASE("an override redirects assets and shaders together", "[paths]") {
	const fs::path elsewhere = fs::temp_directory_path() / "atomic-assets-probe";
	Override guard(elsewhere);

	REQUIRE(Paths::Assets() == elsewhere);
	// Shaders has to follow the override, or pointing a release binary at a
	// working tree moves the data and leaves the shaders behind.
	REQUIRE(Paths::Shaders("render") == elsewhere / "shaders" / "render");
	REQUIRE(Paths::Assets() != Paths::Base());
}

TEST_CASE("clearing the override restores the base path", "[paths]") {
	{
		Override guard(fs::temp_directory_path() / "atomic-assets-probe");
		REQUIRE(Paths::Assets() != Paths::Base());
	}
	REQUIRE(Paths::Assets() == Paths::Base());
}

TEST_CASE("an override need not exist to be set", "[paths]") {
	// Resolving a path is not opening it. The client reports a missing shader
	// with the full path it looked at, which is only possible if building the
	// path succeeds first.
	Override guard("/nonexistent/atomic");

	REQUIRE(Paths::Assets() == fs::path("/nonexistent/atomic"));
	REQUIRE(Paths::Shaders("render") == fs::path("/nonexistent/atomic/shaders/render"));
}
