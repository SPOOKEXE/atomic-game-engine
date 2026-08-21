// Finding the programs, which is the one thing this launcher cannot be wrong
// about quietly: a wrong path starts something, it looks like it worked, and
// what ran was the wrong build.

#include <engine/core/Paths.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <launcher/Programs.hpp>

TEST_SUITE_ID("launcher.programs")

using namespace launcher;

TEST_CASE("the staged root is the parent of the program's own directory", "[launcher]") {
	// `mono_add_program` stages every program as `<stage>/<name>/<name>`, so a
	// launcher at `<stage>/launcher/` finds its siblings one level up.
	CHECK(StageRoot("/build/dev/launcher") == std::filesystem::path("/build/dev"));

	// **With a trailing separator too**, which is what `SDL_GetBasePath`
	// returns on every platform. Without this, `parent_path` answers the
	// launcher's own directory and every program is looked for inside it.
	CHECK(StageRoot("/build/dev/launcher/") == std::filesystem::path("/build/dev"));
}

TEST_CASE("a program's path is its own directory and its own name", "[launcher]") {
	const std::filesystem::path client = ProgramPath("/build/dev", "client");

	CHECK(client.parent_path() == std::filesystem::path("/build/dev/client"));
	CHECK(client.stem() == "client");

	// The suffix is `core::Paths::Program`'s answer rather than a second
	// `#ifdef` here, which is what that function's own comment asked for.
	CHECK(client.filename() == engine::core::Paths::Program("client"));
}

TEST_CASE("a path is returned even when nothing is there", "[launcher]") {
	// So that the failure can say *where* it looked. A launcher reporting "no
	// client" without a path is one nobody can debug from a screenshot.
	const std::filesystem::path missing = ProgramPath("/nowhere-at-all", "client");
	CHECK_FALSE(missing.empty());
	CHECK_FALSE(ProgramPresent(missing));
}

TEST_CASE("a directory is not a program", "[launcher]") {
	// `is_regular_file` and not `exists`: a staged tree where the build was
	// interrupted can leave the directory without the binary, and "the folder
	// is there" is not the question being asked.
	CHECK_FALSE(ProgramPresent(std::filesystem::temp_directory_path()));
}
