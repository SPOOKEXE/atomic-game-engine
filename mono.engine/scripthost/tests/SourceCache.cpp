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
#include <engine/scripthost/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

TEST_SUITE_ID("engine.scripthost.sourcecache")

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

TEST_CASE("a failed top-level script is disabled without stopping its world", "[script][sourcecache]") {
	Store store = MakeWorld();

	SourceCache cache;
	cache.Set(Name("Broken.luau"), "error('intentional failure')");
	cache.Set(Name("Healthy.luau"), "Instance.new('Part', workspace).Name = 'StillRunning'");
	store.SetResource(cache);

	const engine::ecs::Entity broken = engine::script::MakeScript(store, "Broken.luau", "Broken");
	engine::script::MakeScript(store, "Healthy.luau", "Healthy");

	RuntimeLimits limits;
	limits.Role = engine::script::HostRole::OfServer();
	const auto runtime = MakeRuntime(store, Language::Luau, limits);

	CHECK(runtime->RunWorldScripts() == 1);
	CHECK_FALSE(runtime->LastError().empty());
	CHECK(store.Has<engine::script::Disabled>(broken));

	bool healthyRan = false;
	store.EachEntity([&](engine::ecs::Entity entity) {
		healthyRan = healthyRan || store.InstanceNameOf(entity) == Name("StillRunning");
	});
	CHECK(healthyRan);
}

TEST_CASE("the mirror fills a client-runnable script and nothing else", "[script][sourcecache]") {
	Store store = MakeWorld();

	SourceCache cache;
	cache.Set(Name("Local.luau"), "-- a client's");
	cache.Set(Name("Server.luau"), "-- a server's");
	cache.Set(Name("Module.luau"), "return {}");
	store.SetResource(cache);

	const engine::ecs::Entity local = engine::script::MakeScript(store, "Local.luau", "Local", true);
	const engine::ecs::Entity server = engine::script::MakeScript(store, "Server.luau", "Server", false);
	const engine::ecs::Entity module = engine::script::MakeModule(store, "Module.luau", "Module");

	engine::script::SourceMirror mirror;
	engine::script::MirrorSourcePrograms(store, mirror);

	// **What a client may run, and only that.** A `Script` is the server's, so
	// its text never becomes a component at all - which means no interest
	// predicate has to be right for a game's server logic to stay on the server.
	REQUIRE(store.Get<engine::script::Program>(local) != nullptr);
	CHECK(store.Get<engine::script::Program>(local)->Text == "-- a client's");
	CHECK(store.Get<engine::script::Program>(module) != nullptr);
	CHECK(store.Get<engine::script::Program>(server) == nullptr);
}

TEST_CASE("a tick that edited nothing writes no row", "[script][sourcecache]") {
	Store store = MakeWorld();

	SourceCache cache;
	cache.Set(Name("Local.luau"), "-- first");
	store.SetResource(cache);

	const engine::ecs::Entity local = engine::script::MakeScript(store, "Local.luau", "Local", true);

	engine::script::SourceMirror mirror;
	engine::script::MirrorSourcePrograms(store, mirror);
	REQUIRE(store.Get<engine::script::Program>(local) != nullptr);

	// **The claim the per-tick cost rests on, made where it can be seen
	// directly.** `ChangeDetection::Observed` reads the store's dirty bits, so
	// "this costs nothing on a quiet tick" is exactly "the mirror does not
	// write" - and `Store::Set` is what sets a bit. Sixty of these is what a
	// second of a running world does to ten kilobytes of Luau.
	store.Observe<engine::script::Program>();
	store.ClearChanges();

	for (int tick = 0; tick < 60; tick++) {
		engine::script::MirrorSourcePrograms(store, mirror);
	}

	CHECK_FALSE(store.Changed<engine::script::Program>(local));

	// And an edit does write, so the case above is not passing because nothing
	// works.
	auto *held = store.ResourceMutable<SourceCache>();
	REQUIRE(held != nullptr);
	held->Set(Name("Local.luau"), "-- second");

	engine::script::MirrorSourcePrograms(store, mirror);

	CHECK(store.Changed<engine::script::Program>(local));
	CHECK(store.Get<engine::script::Program>(local)->Text == "-- second");
}

TEST_CASE("one script's edit does not rewrite every other script's row", "[script][sourcecache]") {
	Store store = MakeWorld();

	SourceCache cache;
	cache.Set(Name("One.luau"), "-- one");
	cache.Set(Name("Two.luau"), "-- two");
	store.SetResource(cache);

	const engine::ecs::Entity one = engine::script::MakeScript(store, "One.luau", "One", true);
	const engine::ecs::Entity two = engine::script::MakeScript(store, "Two.luau", "Two", true);

	engine::script::SourceMirror mirror;
	engine::script::MirrorSourcePrograms(store, mirror);

	store.Observe<engine::script::Program>();
	store.ClearChanges();

	// The generation says *something* was written and never what, so the walk
	// visits every script - and then compares. Without that comparison, saving
	// one file in an editor would put every program in the world back on the
	// wire.
	store.ResourceMutable<SourceCache>()->Set(Name("Two.luau"), "-- two, edited");
	engine::script::MirrorSourcePrograms(store, mirror);

	CHECK_FALSE(store.Changed<engine::script::Program>(one));
	CHECK(store.Changed<engine::script::Program>(two));
}

TEST_CASE("a replica does not mirror and reads what arrived", "[script][sourcecache]") {
	Store store = MakeWorld();

	// **A file this machine happens to have at the path the instance names**,
	// which is what makes the refusal visible. A replica fills from the wire and
	// a `LuaSourceContainer` can land a tick before the `Program` beside it - so
	// a mirror running here would put the *client's own copy* of that path into
	// the row, and `RunNewScripts` would start it. A client quietly running a
	// file of its own in place of the program it was sent is the one outcome
	// worse than not running it.
	const std::filesystem::path own = std::filesystem::temp_directory_path() / "sourcecache_replica_own.luau";
	{
		std::ofstream file(own, std::ios::trunc);
		REQUIRE(file);
		file << "-- the client's own copy";
	}

	const engine::ecs::Entity arrived = engine::script::MakeScript(store, own.string(), "Arrived", true);
	store.SetAdoptOnly(true);

	engine::script::SourceMirror mirror;
	engine::script::MirrorSourcePrograms(store, mirror);

	CHECK(store.Get<engine::script::Program>(arrived) == nullptr);

	// And once the authority's row has landed, that is what the runtime reads -
	// which is the whole point of the row: a client has no cache and no game
	// file, so a resolver that only knew about those two would answer "could not
	// open" on a world it had been sent every byte of.
	store.Set(arrived, engine::script::Program{Name(own.string()), "-- what the server sent"});

	Name path;
	std::string program;
	std::string error;
	REQUIRE(engine::script::ReadProgram(store, arrived, path, program, error));
	CHECK(path == Name(own.string()));
	CHECK(program == "-- what the server sent");

	std::error_code ignored;
	std::filesystem::remove(own, ignored);
}

TEST_CASE("the world's own table outranks a row that arrived", "[script][sourcecache]") {
	Store store = MakeWorld();

	const engine::ecs::Entity instance = engine::script::MakeScript(store, "Both.luau", "Both", true);
	store.Set(instance, engine::script::Program{Name("Both.luau"), "-- the mirror"});

	SourceCache cache;
	cache.Set(Name("Both.luau"), "-- the unsaved edit");
	store.SetResource(cache);

	// **The cache wins wherever it has an answer**, which is what keeps an
	// editor's unsaved edit the thing that runs: the row is a mirror taken at
	// some earlier tick, and a resolver that preferred it would run the last
	// saved version of a file somebody is in the middle of changing.
	Name path;
	std::string program;
	std::string error;
	REQUIRE(engine::script::ReadProgram(store, instance, path, program, error));
	CHECK(program == "-- the unsaved edit");
}

TEST_CASE("a row naming another path is not the program to run", "[script][sourcecache]") {
	Store store = MakeWorld();

	const engine::ecs::Entity instance = engine::script::MakeScript(store, "Now.luau", "Moved", true);

	// The instance was pointed somewhere else after the row was written. A
	// resolver that trusted the row would run the program the author has just
	// stopped naming, which is worse than failing to find one.
	store.Set(instance, engine::script::Program{Name("Before.luau"), "-- stale"});

	Name path;
	std::string program;
	std::string error;
	CHECK_FALSE(engine::script::ReadProgram(store, instance, path, program, error));
	CHECK(path == Name("Now.luau"));
	CHECK_FALSE(error.empty());
}
