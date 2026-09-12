#include <engine/core/Paths.hpp>
#include <engine/examples/DemosLoader.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

TEST_SUITE_ID("engine.examples.demos-loader")

namespace {
	struct Fixture {
		std::filesystem::path Root = std::filesystem::temp_directory_path() / "atomic-engine-demos-loader";

		Fixture() {
			std::filesystem::remove_all(Root);
			std::filesystem::create_directories(Root / "scripts" / "nested");
			std::filesystem::create_directories(Root / "worlds");
			std::ofstream(Root / "scripts" / "Zeta.luau") << "return true\n";
			std::ofstream(Root / "scripts" / "Alpha.luau") << "return true\n";
			std::ofstream(Root / "scripts" / "Alpha.js") << "true;\n";
			std::ofstream(Root / "scripts" / "notes.txt") << "not a demo\n";
			std::ofstream(Root / "scripts" / "nested" / "Hidden.luau") << "return true\n";
			std::ofstream(Root / "worlds" / "Arena.aworld") << "<World />\n";
		}

		~Fixture() {
			std::filesystem::remove_all(Root);
		}
	};
}

TEST_CASE("demo loader lists scripts and worlds by kind", "[examples][demos]") {
	const Fixture fixture;
	const engine::examples::DemosLoader demos(fixture.Root);

	const auto scripts = demos.List(engine::examples::DemoKind::Script);
	REQUIRE(scripts.size() == 2);
	CHECK(scripts[0].Name == "Alpha.luau");
	CHECK(scripts[1].Name == "Zeta.luau");
	CHECK(scripts[0].Kind == engine::examples::DemoKind::Script);

	const auto worlds = demos.List(engine::examples::DemoKind::World);
	REQUIRE(worlds.size() == 1);
	CHECK(worlds[0].Name == "Arena.aworld");
	CHECK(worlds[0].Kind == engine::examples::DemoKind::World);

	const auto all = demos.List();
	REQUIRE(all.size() == 3);
	CHECK(all[0].Kind == engine::examples::DemoKind::Script);
	CHECK(all[2].Kind == engine::examples::DemoKind::World);
}

TEST_CASE("demo loader resolves only direct files of the requested kind", "[examples][demos]") {
	const Fixture fixture;
	const engine::examples::DemosLoader demos(fixture.Root);

	CHECK(
		demos.Resolve(engine::examples::DemoKind::Script, "Alpha.luau") ==
		fixture.Root / "scripts" / "Alpha.luau"
	);
	CHECK(
		demos.Resolve(engine::examples::DemoKind::World, "Arena.aworld") ==
		fixture.Root / "worlds" / "Arena.aworld"
	);
	CHECK(demos.Resolve(engine::examples::DemoKind::Script, "../worlds/Arena.aworld").empty());
	CHECK(demos.Resolve(engine::examples::DemoKind::Script, "nested/Hidden.luau").empty());
	CHECK(
		demos.Resolve(engine::examples::DemoKind::Script, "Alpha.js") == fixture.Root / "scripts" / "Alpha.js"
	);
	CHECK(demos.Resolve(engine::examples::DemoKind::World, "Alpha.luau").empty());
	CHECK_FALSE(demos.Find(engine::examples::DemoKind::Script, "Alpha.js").has_value());
	CHECK_FALSE(demos.Find(engine::examples::DemoKind::World, "missing.aworld").has_value());
}

TEST_CASE("staged demo tree contains script and authored world demos", "[examples][demos]") {
	const std::filesystem::path previous = engine::core::Paths::Assets();
	engine::core::Paths::SetAssetsOverride(engine::core::Paths::Base().parent_path() / "assets");

	const engine::examples::DemosLoader demos;
	CHECK(demos.Find(engine::examples::DemoKind::Script, "Rings.luau").has_value());
	CHECK(demos.Find(engine::examples::DemoKind::World, "BladeborneDemo.aworld").has_value());

	engine::core::Paths::SetAssetsOverride(previous);
}
