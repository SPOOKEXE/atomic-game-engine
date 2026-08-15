// A stack frame from a transpiled scene names the line the author wrote.
//
// **What this suite is really guarding is a number nobody checks.** A stack
// frame is read once, by a person, at the moment something is already wrong -
// and a *plausible* wrong line is worse than no line, because it sends them to
// code that is fine. Nothing about a wrong mapping looks wrong from the outside,
// so the mapping is asserted here rather than trusted.
//
// The maps are written by hand rather than by running `tsc`, so the cases hold
// on a checkout with no `node_modules`: `just check` must not depend on whether
// somebody has run `bun install`. The one case that does use the real toolchain
// asks the build's own output whether it still has the shape the reader expects,
// and skips when the transpile did not run.

#include "../src/SourceMap.hpp"

#include <engine/core/Paths.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using engine::script::LoadSourceMap;
using engine::script::MakeRuntime;
using engine::script::MapStackFrames;

namespace {

	// A directory of this suite's own, emptied on the way in so a rerun cannot
	// read a file an earlier run left behind.
	std::filesystem::path Scratch(const std::string &name) {
		const std::filesystem::path directory =
			std::filesystem::temp_directory_path() / "mono-sourcemap-tests" / name;
		std::filesystem::remove_all(directory);
		std::filesystem::create_directories(directory);
		return directory;
	}

	void Write(const std::filesystem::path &path, std::string_view text) {
		std::ofstream out(path, std::ios::binary);
		out << text;
	}

}

TEST_CASE("a source map answers which line a generated line came from", "[script][sourcemap]") {
	const std::filesystem::path directory = Scratch("decode");
	const std::filesystem::path map = directory / "thing.js.map";

	// Four generated lines. A segment is four VLQ fields - generated column,
	// source index, source line, source column - so the **third** letter is the
	// one that moves a line. `AAAA` is four zero deltas; `AACA` is source line
	// +1, because `C` decodes as 2 and the low bit of the first digit is the
	// sign, leaving 1.
	//
	//   generated 1 -> source 1   (AAAA, all deltas zero)
	//   generated 2 -> nothing    (empty group between two semicolons)
	//   generated 3 -> source 2   (AACA, source line +1)
	//   generated 4 -> source 3   (AACA again)
	Write(
		map,
		R"({"version":3,"file":"thing.js","sources":["thing.ts"],"names":[],)"
		R"("mappings":"AAAA;;AACA;AACA"})"
	);

	const auto loaded = LoadSourceMap(map);
	REQUIRE(loaded.has_value());

	CHECK(loaded->LineFor(1) == 1);
	CHECK(loaded->LineFor(2) == 0);
	CHECK(loaded->LineFor(3) == 2);
	CHECK(loaded->LineFor(4) == 3);

	// Past the end of the map is not an error and is not line one.
	CHECK(loaded->LineFor(99) == 0);
	CHECK(loaded->LineFor(0) == 0);

	// Resolved against the map's own directory, so what comes back can be
	// opened rather than only printed.
	CHECK(loaded->Source == (directory / "thing.ts").string());
}

TEST_CASE("a negative source-line delta is read as a step back", "[script][sourcemap]") {
	const std::filesystem::path directory = Scratch("negative");
	const std::filesystem::path map = directory / "back.js.map";

	// `AAEA` steps the source line forward by two, `AADA` steps it back by one.
	// **The sign lives in the low bit of the first digit**, so a decoder that
	// signs the assembled number instead reads `AADA` as +1 and produces a
	// stack frame that is plausible and wrong.
	Write(
		map,
		R"({"version":3,"file":"back.js","sources":["back.ts"],"names":[],)"
		R"("mappings":"AAAA;AAEA;AADA"})"
	);

	const auto loaded = LoadSourceMap(map);
	REQUIRE(loaded.has_value());

	CHECK(loaded->LineFor(1) == 1);
	CHECK(loaded->LineFor(2) == 3);
	CHECK(loaded->LineFor(3) == 2);
}

TEST_CASE("an unreadable map is nothing rather than a failure", "[script][sourcemap]") {
	const std::filesystem::path directory = Scratch("bad");

	CHECK_FALSE(LoadSourceMap(directory / "absent.js.map").has_value());

	Write(directory / "truncated.js.map", "{\"version\":3,\"mappings\":");
	CHECK_FALSE(LoadSourceMap(directory / "truncated.js.map").has_value());

	Write(directory / "nomappings.js.map", R"({"version":3,"sources":["x.ts"]})");
	CHECK_FALSE(LoadSourceMap(directory / "nomappings.js.map").has_value());

	// Valid JSON whose mappings are not base64 VLQ. Rejected rather than
	// silently decoded into whatever the alphabet lookup makes of it.
	Write(directory / "junk.js.map", R"({"version":3,"sources":["x.ts"],"mappings":"!!!!"})");
	CHECK_FALSE(LoadSourceMap(directory / "junk.js.map").has_value());
}

TEST_CASE("stack frames are rewritten only where a map exists", "[script][sourcemap]") {
	const std::filesystem::path directory = Scratch("frames");
	const std::filesystem::path mapped = directory / "mapped.js";
	const std::filesystem::path plain = directory / "plain.js";

	Write(
		std::filesystem::path(mapped.string() + ".map"),
		R"({"version":3,"file":"mapped.js","sources":["mapped.ts"],"names":[],)"
		R"("mappings":"AAAA;AACA;AACA"})"
	);

	const std::string text = "Error: broke\n"
							 "    at go (" +
							 mapped.string() + ":3)\n" + "    at <eval> (" + plain.string() + ":7)\n";

	const std::string rewritten = MapStackFrames(text);

	// The mapped frame names the TypeScript file and its line.
	CHECK(rewritten.find((directory / "mapped.ts").string() + ":3") != std::string::npos);
	CHECK(rewritten.find("mapped.js:3") == std::string::npos);

	// **The unmapped frame is untouched**, which is what makes it safe to run
	// this over every exception rather than only over transpiled ones.
	CHECK(rewritten.find(plain.string() + ":7") != std::string::npos);

	// Everything that is not a frame survives, including the message.
	CHECK(rewritten.find("Error: broke") != std::string::npos);
	CHECK(rewritten.find("at go (") != std::string::npos);
	CHECK(rewritten.find("at <eval> (") != std::string::npos);
}

TEST_CASE("a rewritten frame drops the generated column", "[script][sourcemap]") {
	const std::filesystem::path directory = Scratch("columns");
	const std::filesystem::path mapped = directory / "col.js";
	const std::filesystem::path plain = directory / "raw.js";

	Write(
		std::filesystem::path(mapped.string() + ".map"),
		R"({"version":3,"file":"col.js","sources":["col.ts"],"names":[],)"
		R"("mappings":"AAAA;AACA;AACA"})"
	);

	// QuickJS prints `file:line:column`. **The column belongs to the generated
	// file**, so carrying it onto a source line names a character that is
	// somewhere else - which reads as precision and is not.
	const std::string text =
		"    at go (" + mapped.string() + ":3:15)\n" + "    at raw (" + plain.string() + ":4:22)\n";

	const std::string rewritten = MapStackFrames(text);

	CHECK(rewritten.find((directory / "col.ts").string() + ":3)") != std::string::npos);
	CHECK(rewritten.find(":3:15") == std::string::npos);

	// The frame that was not rewritten keeps its column, because nothing about
	// it has moved.
	CHECK(rewritten.find(plain.string() + ":4:22") != std::string::npos);
}

TEST_CASE("a line the map does not cover keeps the frame it had", "[script][sourcemap]") {
	const std::filesystem::path directory = Scratch("uncovered");
	const std::filesystem::path generated = directory / "sparse.js";

	// Two generated lines mapped, and the frame below asks about the ninth.
	Write(
		std::filesystem::path(generated.string() + ".map"),
		R"({"version":3,"file":"sparse.js","sources":["sparse.ts"],"names":[],)"
		R"("mappings":"AAAA;AACA"})"
	);

	const std::string text = "    at <eval> (" + generated.string() + ":9)";
	CHECK(MapStackFrames(text) == text);
}

TEST_CASE("a TypeScript scene that throws names its own line", "[script][sourcemap][js]") {
	const std::filesystem::path directory = Scratch("thrown");
	const std::filesystem::path generated = directory / "scene.js";

	// Three generated lines, the third of which throws. The map sends generated
	// line 3 back to source line 12 - the shape a stripped block of type
	// annotations produces, where the generated file is far shorter than the
	// file somebody wrote.
	Write(generated, "const a = 1;\nconst b = 2;\nthrow new Error('deliberate');\n");
	Write(
		std::filesystem::path(generated.string() + ".map"),
		R"({"version":3,"file":"scene.js","sources":["scene.ts"],"names":[],)"
		R"("mappings":"AAAA;AACA;AAUA"})"
	);

	engine::ecs::Store store("sourcemap.thrown");
	const auto runtime = MakeRuntime(store, engine::script::Language::JavaScript);
	REQUIRE(runtime != nullptr);

	REQUIRE_FALSE(runtime->RunFile(generated.string()));

	const std::string &error = runtime->LastError();
	INFO(error);

	// The message itself is untouched.
	CHECK(error.find("deliberate") != std::string::npos);

	// **The frame names `scene.ts:12` and not `scene.js:3`**, which is the whole
	// of what this feature is. Asserting the absence matters as much as the
	// presence: a rewriter that appended rather than replaced would pass the
	// first check and still leave a reader two line numbers to choose between.
	CHECK(error.find((directory / "scene.ts").string() + ":12") != std::string::npos);
	CHECK(error.find("scene.js:3") == std::string::npos);
}

TEST_CASE("the build's own transpile emits a map this reader understands", "[script][sourcemap]") {
	// The staged twin of `mono.engine/examples/Mirrors-4-worlds.ts`, which
	// exists only where `tsc` was available at configure time. **The point of
	// this case is that the hand-written maps above cannot go stale silently**:
	// they are this suite's model of what the toolchain emits, and a real map is
	// what says the model is still right.
	//
	// Looked for in both places for `examples::ExamplePath`'s reason - the
	// scenes stage into a sibling of `Paths::Assets()` rather than into it -
	// spelled out here rather than depending on `examples` from `script`.
	const std::string name = "Mirrors-4-worlds.js.map";
	std::filesystem::path staged = engine::core::Paths::Assets() / "examples" / name;
	if (!std::filesystem::exists(staged)) {
		staged = engine::core::Paths::Base().parent_path() / "assets" / "examples" / name;
	}

	if (!std::filesystem::exists(staged)) {
		SUCCEED("no transpiled twin; tsc was not available at configure time");
		return;
	}

	const auto loaded = LoadSourceMap(staged);
	REQUIRE(loaded.has_value());

	// It names the TypeScript the scene was written in, at a path that exists.
	CHECK(loaded->Source.ends_with("Mirrors-4-worlds.ts"));
	CHECK(std::filesystem::exists(loaded->Source));

	// And it maps something rather than decoding to a run of zeroes, which is
	// what a reader that silently gave up would produce.
	REQUIRE_FALSE(loaded->SourceLines.empty());
	size_t covered = 0;
	for (const uint32_t line : loaded->SourceLines) {
		covered += line != 0 ? 1 : 0;
	}
	CHECK(covered > loaded->SourceLines.size() / 2);
}
