// The cache that lets a game file carry its own scripts.
//
// **What is worth testing here is the ordering, not the map.** A vector keyed
// by name is not interesting; "the cache is consulted before the disk" is,
// because getting it backwards produces an editor whose unsaved changes appear
// to run and do not - the file on disk is stale by exactly one edit, which is
// the hardest possible version of that bug to see.

#include <engine/core/Paths.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

TEST_SUITE_ID("engine.script.sourcecache")

using engine::core::Name;
using engine::ecs::Store;
using engine::script::Language;
using engine::script::MakeRuntime;
using engine::script::ReadSource;
using engine::script::RuntimeLimits;
using engine::script::ScriptClass;
using engine::script::SourceCache;

namespace {
	// A world with the class tree registered, which is what makes `Source` and
	// `SourceCache` known types rather than ones minted on first use.
	Store MakeWorld() {
		engine::scene::RegisterSceneClasses();
		ScriptClass();
		return Store{"sourcecache"};
	}
}

TEST_CASE("a cached program is used and the file is not read", "[script][sourcecache]") {
	Store store = MakeWorld();

	SourceCache cache;
	cache.Set(Name("does/not/exist.luau"), "return 42");
	store.SetResource(cache);

	std::string program;
	std::string error;

	// The path names nothing on disk at all. If the resolution order were the
	// other way round this would fail to open a file - which is exactly the
	// assertion, stated as a path that could not possibly succeed by accident.
	REQUIRE(ReadSource(store, Name("does/not/exist.luau"), program, error));
	CHECK(program == "return 42");
	CHECK(error.empty());
}

TEST_CASE("a path with nothing cached falls through to the filesystem", "[script][sourcecache]") {
	Store store = MakeWorld();

	// **The half that keeps v0.6 working.** `--script Rings.luau` and every
	// asset a game references but did not author arrive this way, so a cache
	// that shadowed the disk unconditionally would be a cache that broke every
	// existing entry point.
	const std::filesystem::path path =
		std::filesystem::temp_directory_path() / "engine-sourcecache-fallthrough.luau";
	{
		std::ofstream file(path, std::ios::binary);
		file << "-- from disk";
	}

	std::string program;
	std::string error;
	REQUIRE(ReadSource(store, Name(path.string()), program, error));
	CHECK(program == "-- from disk");

	std::filesystem::remove(path);
}

TEST_CASE("a miss is a failure with a reason, never an empty program", "[script][sourcecache]") {
	Store store = MakeWorld();

	// A script that ran, did nothing and reported success is the failure this
	// refuses. `SourceCache::Find` has no get-or-default for the same reason
	// `scene::SurfaceTable` has none.
	std::string program;
	std::string error;
	CHECK_FALSE(ReadSource(store, Name("nowhere/at/all.luau"), program, error));
	CHECK_FALSE(error.empty());
}

TEST_CASE("setting a path twice replaces rather than appends", "[script][sourcecache]") {
	SourceCache cache;
	cache.Set(Name("Main.luau"), "first");
	cache.Set(Name("Main.luau"), "second");

	// A history of edits would make the serialisation grow every time the
	// editor saved, and insertion order - the thing that makes two loads of one
	// game file byte-identical - would stop meaning anything.
	REQUIRE(cache.Count() == 1);
	REQUIRE(cache.Find(Name("Main.luau")) != nullptr);
	CHECK(*cache.Find(Name("Main.luau")) == "second");

	CHECK(cache.Erase(Name("Main.luau")));
	CHECK(cache.Count() == 0);
	CHECK_FALSE(cache.Erase(Name("Main.luau")));
}

TEST_CASE("a script instance runs from the cache", "[script][sourcecache]") {
	Store store = MakeWorld();

	// The end-to-end version: not `ReadSource` directly, but the path
	// `RunWorldScripts` takes - because the point of the cache is that the
	// runtime consults it, and a helper nobody called would pass every test
	// above while changing nothing.
	SourceCache cache;
	cache.Set(Name("Cached.luau"), "Instance.new('Part', workspace).Name = 'FromCache'");
	store.SetResource(cache);

	engine::script::MakeScript(store, "Cached.luau", "Runner");

	RuntimeLimits limits;
	limits.Role = engine::script::HostRole::OfServer();
	const auto runtime = MakeRuntime(store, Language::Luau, limits);

	REQUIRE(runtime->RunWorldScripts() == 1);
	CHECK(runtime->LastError().empty());

	bool found = false;
	store.EachEntity([&](engine::ecs::Entity entity) {
		if (store.InstanceNameOf(entity) == Name("FromCache")) {
			found = true;
		}
	});
	CHECK(found);
}
