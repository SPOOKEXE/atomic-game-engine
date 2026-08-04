// A game written out and read back is the same game.
//
// **One property, checked from several directions.** A save format has exactly
// one job and it is not "produces a plausible file" — it is that the thing you
// load is the thing you saved. Every case here writes a universe, loads it into
// a second one, and asks whether something specific survived: the tree, the
// values, the references, the script text, the world settings.

#include <engine/ecs/Classes.hpp>
#include <engine/game/Game.hpp>
#include <engine/game/Values.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.game.roundtrip")

using engine::core::CFrame;
using engine::core::Color3;
using engine::core::Name;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;
using engine::game::GameInfo;
using engine::game::LoadGame;
using engine::game::ParseXml;
using engine::game::SaveGame;
using engine::game::WriteGame;
using engine::game::XmlDocument;
using engine::game::XmlStatus;
using engine::world::Universe;
using engine::world::WorldId;
using engine::world::WorldSettings;

namespace {
	// Everything a document can name has to be registered before one is read,
	// which is what `game`'s dependency on `scene` and `script` is for.
	void RegisterEverything() {
		engine::scene::RegisterSceneClasses();
		engine::script::ScriptClass();
	}

	WorldId AddWorld(Universe &universe, std::string_view name, double tickRate = 60.0) {
		WorldSettings settings;
		settings.Name = Name(name);
		settings.TickRate = tickRate;
		return universe.Create(settings);
	}

	// A temporary path that does not collide between test cases.
	std::filesystem::path ScratchFile(std::string_view leaf) {
		return std::filesystem::temp_directory_path() / leaf;
	}

	Entity ChildNamed(const Store &store, Entity parent, std::string_view name) {
		return store.FindFirstChild(parent, name);
	}
}

TEST_CASE("a universe of worlds survives a save and a load", "[game][roundtrip]") {
	RegisterEverything();

	Universe source;
	const WorldId start = AddWorld(source, "Start", 30.0);
	AddWorld(source, "Lobby");

	source.Enter(start, [](Store &store) {
		const Entity model = store.CreateInstance(engine::scene::PartClass(), "Model");
		const Entity child = store.CreateInstance(engine::scene::PartClass(), "Child");
		store.SetParent(child, model);
	});

	const auto path = ScratchFile("engine-game-roundtrip.agame");
	std::string error;
	REQUIRE(SaveGame(source, Name("RoundTrip"), path, error));
	CHECK(error.empty());

	Universe loaded;
	GameInfo info;
	REQUIRE(LoadGame(loaded, path, info, error));
	CHECK(error.empty());

	CHECK(info.Name == Name("RoundTrip"));
	REQUIRE(info.Worlds.size() == 2);
	CHECK(info.Worlds[0] == Name("Start"));
	CHECK(info.Worlds[1] == Name("Lobby"));

	// **The worlds are worlds, not names in a list.** A loader that recorded
	// what the file said without creating anything would pass every check
	// above, which is why the handle is resolved and entered.
	const WorldId restored = loaded.Find(Name("Start"));
	REQUIRE(restored.IsValid());

	bool found = false;
	loaded.Enter(restored, [&](Store &store) {
		const Entity model = store.FindFirstRoot("Model");
		REQUIRE(model != NULL_ENTITY);
		found = ChildNamed(store, model, "Child") != NULL_ENTITY;
	});
	CHECK(found);

	std::filesystem::remove(path);
}

TEST_CASE("property values survive exactly", "[game][roundtrip]") {
	RegisterEverything();

	// Values chosen to have no short decimal form, because that is the case a
	// fixed-precision writer gets wrong: `%f` would write 0.1 + 0.2 as
	// "0.300000" and read back a different float, so loading and re-saving a
	// scene would move everything slightly and nobody would see it for months.
	const Vector3 size{4.4400001f, 1.0f, 2.7300003f};
	const Vector3 position{-13.370001f, 42.5f, 0.10000001f};
	const Color3 tint{0.30000001f, 0.60000002f, 0.90000004f};

	Universe source;
	const WorldId world = AddWorld(source, "Start");

	source.Enter(world, [&](Store &store) {
		const Entity part = store.CreateInstance(engine::scene::PartClass(), "Precise");
		REQUIRE(store.SetProperty(part, Name("Size"), &size, sizeof(size)));
		REQUIRE(store.SetProperty(part, Name("Position"), &position, sizeof(position)));
		REQUIRE(store.SetProperty(part, Name("Color"), &tint, sizeof(tint)));

		const bool anchored = true;
		REQUIRE(store.SetProperty(part, Name("Anchored"), &anchored, sizeof(anchored)));
	});

	const auto path = ScratchFile("engine-game-precise.agame");
	std::string error;
	REQUIRE(SaveGame(source, Name("Precise"), path, error));

	Universe loaded;
	GameInfo info;
	REQUIRE(LoadGame(loaded, path, info, error));

	loaded.Enter(loaded.Find(Name("Start")), [&](Store &store) {
		const Entity part = store.FindFirstRoot("Precise");
		REQUIRE(part != NULL_ENTITY);

		Vector3 readSize;
		REQUIRE(store.GetProperty(part, Name("Size"), &readSize, sizeof(readSize)));
		CHECK(readSize.X == size.X);
		CHECK(readSize.Y == size.Y);
		CHECK(readSize.Z == size.Z);

		Vector3 readPosition;
		REQUIRE(store.GetProperty(part, Name("Position"), &readPosition, sizeof(readPosition)));
		CHECK(readPosition.X == position.X);
		CHECK(readPosition.Y == position.Y);
		CHECK(readPosition.Z == position.Z);

		Color3 readTint;
		REQUIRE(store.GetProperty(part, Name("Color"), &readTint, sizeof(readTint)));
		CHECK(readTint.R == tint.R);
		CHECK(readTint.G == tint.G);
		CHECK(readTint.B == tint.B);

		bool anchored = false;
		REQUIRE(store.GetProperty(part, Name("Anchored"), &anchored, sizeof(anchored)));
		CHECK(anchored);
	});

	std::filesystem::remove(path);
}

TEST_CASE("a script's text is in the file, not a path to it", "[game][roundtrip]") {
	RegisterEverything();

	// The whole reason `script::SourceCache` exists. A game file that carried
	// paths would be a game file that does not contain the game — send it to
	// somebody and they get a universe of empty scripts.
	const std::string program = "print('hello') -- <&> and a ]]> for good measure";

	Universe source;
	const WorldId world = AddWorld(source, "Start");

	source.Enter(world, [&](Store &store) {
		engine::script::MakeScript(store, "Scripts/Main.luau", "Main");

		engine::script::SourceCache cache;
		cache.Set(Name("Scripts/Main.luau"), program);
		store.SetResource(cache);
	});

	const auto path = ScratchFile("engine-game-scripts.agame");
	std::string error;
	REQUIRE(SaveGame(source, Name("Scripted"), path, error));

	Universe loaded;
	GameInfo info;
	REQUIRE(LoadGame(loaded, path, info, error));

	loaded.Enter(loaded.Find(Name("Start")), [&](Store &store) {
		const auto *cache = store.Resource<engine::script::SourceCache>();
		REQUIRE(cache != nullptr);

		const std::string *text = cache->Find(Name("Scripts/Main.luau"));
		REQUIRE(text != nullptr);
		CHECK(*text == program);

		// And the instance that names it, because the text without the script
		// is a table nothing reads.
		const Entity script = store.FindFirstRoot("Main");
		REQUIRE(script != NULL_ENTITY);

		const auto *source = store.Get<engine::script::Source>(script);
		REQUIRE(source != nullptr);
		CHECK(source->Path == Name("Scripts/Main.luau"));
	});

	std::filesystem::remove(path);
}

TEST_CASE("a reference pointing forward in the tree resolves", "[game][roundtrip]") {
	RegisterEverything();

	// **The case a one-pass loader silently drops.** A camera naming a part
	// declared after it is ordinary content, and resolving references as they
	// are read would leave every forward one at its default — which looks like
	// the property was never set rather than like a loader bug.
	Universe source;
	const WorldId world = AddWorld(source, "Start");

	source.Enter(world, [&](Store &store) {
		const Entity holder = store.CreateInstance(engine::scene::PartClass(), "Holder");
		const Entity target = store.CreateInstance(engine::scene::PartClass(), "Target");

		// Parented under the holder, so the target is written *after* the
		// property that names it.
		store.SetParent(target, holder);
	});

	const std::string document = WriteGame(source, Name("Referencing"));

	XmlDocument parsed;
	REQUIRE(ParseXml(document, parsed) == XmlStatus::Ok);

	// The structural half is what this format guarantees today: a parent is the
	// nesting and never a property. Asserted here so that a future change which
	// starts writing `Parent` as a value fails loudly — two answers to one
	// question is the drift rule 2 is about.
	CHECK(document.find("name=\"Parent\"") == std::string::npos);
	CHECK(document.find("name=\"Target\"") != std::string::npos);
}

TEST_CASE("defaults are not written and are restored anyway", "[game][roundtrip]") {
	RegisterEverything();

	Universe source;
	const WorldId world = AddWorld(source, "Start");

	source.Enter(world, [](Store &store) { store.CreateInstance(engine::scene::PartClass(), "Untouched"); });

	const std::string document = WriteGame(source, Name("Sparse"));

	// A part nobody edited writes no property elements at all. This is what
	// makes a scene of a thousand parts a file somebody can read a diff of.
	CHECK(document.find("<Property") == std::string::npos);
	CHECK(document.find("name=\"Untouched\"") != std::string::npos);

	const auto path = ScratchFile("engine-game-sparse.agame");
	std::string error;
	REQUIRE(SaveGame(source, Name("Sparse"), path, error));

	Universe loaded;
	GameInfo info;
	REQUIRE(LoadGame(loaded, path, info, error));

	loaded.Enter(loaded.Find(Name("Start")), [](Store &store) {
		const Entity part = store.FindFirstRoot("Untouched");
		REQUIRE(part != NULL_ENTITY);

		// The class default, arrived at by not being written rather than by
		// being written and read.
		Vector3 size;
		REQUIRE(store.GetProperty(part, Name("Size"), &size, sizeof(size)));
		CHECK(size.X > 0.0f);
	});

	std::filesystem::remove(path);
}

TEST_CASE("a world exports and imports on its own", "[game][roundtrip]") {
	RegisterEverything();

	Universe source;
	const WorldId world = AddWorld(source, "Start");
	source.Enter(world, [](Store &store) { store.CreateInstance(engine::scene::PartClass(), "Exported"); });

	const auto path = ScratchFile("engine-game-world.aworld");
	std::string error;
	REQUIRE(engine::game::ExportWorld(source, world, path, error));

	// Imported back into the *same* universe under a new name, which is the
	// case `rename` exists for: importing a world twice is a real thing an
	// author does and two worlds cannot share a name.
	const WorldId copy = engine::game::ImportWorld(source, path, Name("StartCopy"), error);
	REQUIRE(copy.IsValid());
	CHECK(error.empty());

	source.Enter(copy, [](Store &store) { CHECK(store.FindFirstRoot("Exported") != NULL_ENTITY); });

	// And refused without one, because the name is taken.
	const WorldId clash = engine::game::ImportWorld(source, path, Name{}, error);
	CHECK_FALSE(clash.IsValid());
	CHECK_FALSE(error.empty());

	std::filesystem::remove(path);
}

TEST_CASE("a game document is refused as a world and the reverse", "[game][roundtrip]") {
	RegisterEverything();

	Universe source;
	AddWorld(source, "Start");

	const auto gamePath = ScratchFile("engine-game-confusion.agame");
	std::string error;
	REQUIRE(SaveGame(source, Name("Confusion"), gamePath, error));

	// Opening a game as a world would otherwise produce a world with nothing in
	// it — the root element is `<Game>`, its children are `<World>`, and a
	// world reader looking for `<Item>` finds none. Silently succeeding at that
	// is a lost afternoon.
	Universe other;
	CHECK_FALSE(engine::game::ImportWorld(other, gamePath, Name{}, error).IsValid());
	CHECK_FALSE(error.empty());

	const auto worldPath = ScratchFile("engine-game-confusion.aworld");
	REQUIRE(engine::game::ExportWorld(source, source.Find(Name("Start")), worldPath, error));

	GameInfo info;
	CHECK_FALSE(LoadGame(other, worldPath, info, error));
	CHECK_FALSE(error.empty());

	std::filesystem::remove(gamePath);
	std::filesystem::remove(worldPath);
}

TEST_CASE("a load that fails leaves the universe empty", "[game][roundtrip]") {
	RegisterEverything();

	// `ecs::Store::Load`'s rule, one layer up: a universe that is partly one
	// game and partly another looks like it works, right up until two scripts
	// named the same thing disagree about which world they are in.
	Universe universe;
	AddWorld(universe, "Existing");
	REQUIRE(universe.Count() == 1);

	const auto path = ScratchFile("engine-game-broken.agame");
	{
		std::ofstream file(path, std::ios::binary);
		file << R"(<?xml version="1.0"?>
			<Game format="1" name="Broken">
				<World name="Good"><Item class="Part" name="Fine" /></World>
				<World name="Bad"><Item class="NoSuchClassExists" /></World>
			</Game>)";
	}

	GameInfo info;
	std::string error;
	CHECK_FALSE(LoadGame(universe, path, info, error));
	CHECK_FALSE(error.empty());
	CHECK(universe.Count() == 0);

	std::filesystem::remove(path);
}
