// A game written out and read back is the same game.
//
// **One property, checked from several directions.** A save format has exactly
// one job and it is not "produces a plausible file" - it is that the thing you
// load is the thing you saved. Every case here writes a universe, loads it into
// a second one, and asks whether something specific survived: the tree, the
// values, the references, the script text, the world settings.

#include <engine/ecs/Classes.hpp>
#include <engine/game/Game.hpp>
#include <engine/game/Values.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
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

	void AddPart(Universe &universe, WorldId world, std::string_view name) {
		universe.Enter(world, [name](Store &store) {
			store.CreateInstance(engine::scene::PartClass(), name);
		});
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

TEST_CASE("a world's settings are an element and survive the trip", "[game][roundtrip]") {
	// **What `<WorldProperties>` is for, checked from both ends.** The settings
	// used to be attributes on `<World>` *and* were written from a
	// default-constructed `WorldSettings`, so every file claimed 60Hz whatever
	// the world was authored at. Moving them into an element is the visible
	// half; writing the world's real numbers is the half that was a bug.
	RegisterEverything();

	Universe source;
	AddWorld(source, "Slow", 30.0);

	const std::string document = WriteGame(source, Name("Settings"));

	// The element exists and the numbers are in it, not on `<World>`.
	XmlDocument parsed;
	REQUIRE(ParseXml(document, parsed) == XmlStatus::Ok);
	REQUIRE(parsed.Root() != nullptr);
	CHECK(parsed.Root()->Attribute("format") == "2");

	const engine::game::XmlElement *world = nullptr;
	for (const uint32_t index : parsed.Root()->Children) {
		const engine::game::XmlElement *child = parsed.At(index);
		if (child != nullptr && child->Name == "World") {
			world = child;
		}
	}
	REQUIRE(world != nullptr);
	CHECK(world->Attribute("name") == "Slow");
	CHECK(world->Attribute("tickRate").empty());

	const engine::game::XmlElement *properties = nullptr;
	for (const uint32_t index : world->Children) {
		const engine::game::XmlElement *child = parsed.At(index);
		if (child != nullptr && child->Name == "WorldProperties") {
			properties = child;
		}
	}
	REQUIRE(properties != nullptr);
	CHECK(properties->Attribute("tickRate") == "30");
	CHECK(properties->Attribute("idleTickRate") == "2");
	CHECK(properties->Attribute("faultLimit") == "3");

	// **The exported world document has the same section**, spelled out as
	// text rather than re-parsed. `scripts/Lobby.aworld` is checked in as the
	// worked example of this format, and a test that only parsed its own
	// output would let the two drift until somebody opened the file.
	std::string error;
	Universe plain;
	AddWorld(plain, "Lobby");
	plain.Enter(plain.Find(Name("Lobby")), [](Store &store) {
		const Entity workspace = engine::scene::InstallServices(store);
		const Entity baseplate = store.CreateInstance(engine::scene::PartClass(), "Baseplate");
		store.SetParent(baseplate, workspace);
	});

	const std::string exported = engine::game::WriteWorldDocument(plain, plain.Find(Name("Lobby")), error);

	CHECK(exported.find(R"(<World format="2" name="Lobby">)") != std::string::npos);

	// **No camera in the example either.** `scripts/Lobby.aworld` is the worked
	// example of this format and it carried one until the viewer's camera
	// stopped being content; a file with somebody's viewpoint in it is exactly
	// what a reader would copy.
	CHECK(exported.find(R"(class="Camera")") == std::string::npos);
	CHECK(exported.find(R"(class="Workspace")") != std::string::npos);
	CHECK(
		exported.find("\t<WorldProperties tickRate=\"60\" idleTickRate=\"2\" faultLimit=\"3\" />") !=
		std::string::npos
	);

	// And it reads back as 30 rather than as the default, which is the whole
	// point of writing it.
	const auto path = ScratchFile("engine-game-settings.agame");
	REQUIRE(SaveGame(source, Name("Settings"), path, error));

	Universe loaded;
	GameInfo info;
	REQUIRE(LoadGame(loaded, path, info, error));

	const WorldId restored = loaded.Find(Name("Slow"));
	REQUIRE(restored.IsValid());
	CHECK(loaded.SettingsOf(restored).TickRate == 30.0);

	std::filesystem::remove(path);
}

TEST_CASE("authored universe tuning survives the game-file trip", "[game][roundtrip]") {
	RegisterEverything();

	engine::world::UniverseSettings settings;
	settings.Mode = engine::world::ExecutionMode::WorldSerial;
	settings.MaximumCatchUpTicks = 3;
	settings.BusBudgetPerTick = 19;

	Universe source(settings);
	AddWorld(source, "Lobby");

	const auto path = ScratchFile("engine-game-universe-settings.agame");
	std::string error;
	REQUIRE(SaveGame(source, Name("Tuned"), path, error));

	Universe loaded;
	GameInfo info;
	REQUIRE(LoadGame(loaded, path, info, error));

	CHECK(loaded.Settings().Mode == engine::world::ExecutionMode::WorldSerial);
	CHECK(loaded.Settings().MaximumCatchUpTicks == 3);
	CHECK(loaded.Settings().BusBudgetPerTick == 19);
	CHECK(info.Universe.Mode == engine::world::ExecutionMode::WorldSerial);
	CHECK(info.Universe.MaximumCatchUpTicks == 3);
	CHECK(info.Universe.BusBudgetPerTick == 19);

	std::filesystem::remove(path);
}

TEST_CASE("a world's services are in the file like anything else", "[game][roundtrip]") {
	// **A service is an instance, so the format needed nothing added for it.**
	// That is the point of making them entities rather than a side table: they
	// are written by the same walk that writes a part, they resolve by the same
	// class lookup, and the only thing that had to be true was that
	// `RegisterGameClasses` reaches them - which it does, through
	// `RegisterSceneClasses`.
	RegisterEverything();

	Universe source;
	const WorldId world = AddWorld(source, "Lobby");
	source.Enter(world, [](Store &store) {
		const Entity workspace = engine::scene::InstallServices(store);
		store.SetParent(store.CreateInstance(engine::scene::PartClass(), "Baseplate"), workspace);
	});

	const auto path = ScratchFile("engine-game-services.agame");
	std::string error;
	REQUIRE(SaveGame(source, Name("Serviced"), path, error));

	Universe loaded;
	GameInfo info;
	REQUIRE(LoadGame(loaded, path, info, error));

	const WorldId restored = loaded.Find(Name("Lobby"));
	REQUIRE(restored.IsValid());

	loaded.Enter(restored, [](Store &store) {
		const Entity workspace = store.FindFirstRoot("Workspace");
		REQUIRE(workspace != NULL_ENTITY);
		CHECK(ChildNamed(store, workspace, "Baseplate") != NULL_ENTITY);
		// The viewer's camera is not in the file - see the transient case below.
		CHECK(ChildNamed(store, workspace, "Camera") == NULL_ENTITY);

		const Entity starter = store.FindFirstRoot("StarterPlayer");
		REQUIRE(starter != NULL_ENTITY);
		CHECK(ChildNamed(store, starter, "StarterPlayerScripts") != NULL_ENTITY);

		// **The scope survives, and it is the one that would not have.** It is
		// a computed property over a `uint8_t`, so it is written as a word and
		// read back through a setter - the path a plain field never takes. A
		// `ServerStorage` that loaded as `Shared` is a container that stops
		// being server-only, which is the kind of thing nobody checks.
		const Entity storage = store.FindFirstRoot("ServerStorage");
		REQUIRE(storage != NULL_ENTITY);
		const auto *service = store.Get<engine::scene::ServiceComponent>(storage);
		REQUIRE(service != nullptr);
		CHECK(service->Scope == engine::scene::ServiceScope::Server);
	});

	std::filesystem::remove(path);
}

TEST_CASE("an instance moves between two worlds", "[game][roundtrip]") {
	// **What the explorer's cross-world drag is made of.** An `ecs::Entity` is
	// an index into one store, so a subtree cannot be handed across - it is
	// described by the same writer a save file uses and rebuilt on the far
	// side. The properties, the children and the script text all have to come
	// with it, and the script text is the one that would not have: a `Script`
	// carries a *path*, and the program itself lives in the source world's
	// `SourceCache`.
	RegisterEverything();

	Universe universe;
	const WorldId from = AddWorld(universe, "From");
	const WorldId to = AddWorld(universe, "To");

	std::string document;
	universe.Enter(from, [&](Store &store) {
		const Entity model = store.CreateInstance(engine::scene::PartClass(), "Model");

		const Vector3 size{4.0f, 8.0f, 4.0f};
		store.SetProperty(model, Name("Size"), &size, sizeof(size));

		const Entity child = store.CreateInstance(engine::scene::PartClass(), "Child");
		store.SetParent(child, model);

		const Entity script = store.CreateInstance(engine::script::ScriptClass(), "Behaviour");
		store.SetParent(script, model);

		const Name path("scripts/behaviour.luau");
		store.SetProperty(script, Name("Source"), &path, sizeof(path));

		engine::script::SourceCache cache;
		cache.Set(path, "print('moved')");

		// A second program the move must *not* drag along: writing the whole
		// source cache would carry every script in the world across.
		cache.Set(Name("scripts/stays.luau"), "print('stays')");
		store.SetResource(cache);

		document = engine::game::WriteInstanceDocument(store, model);
	});

	REQUIRE_FALSE(document.empty());

	std::string error;
	Entity rebuilt = NULL_ENTITY;
	universe.Enter(to, [&](Store &store) {
		// The destination already has something in it, which is the case
		// `ReadWorldBody` refuses and this one has to allow.
		store.CreateInstance(engine::scene::PartClass(), "Existing");

		engine::script::SourceCache cache;
		cache.Set(Name("scripts/local.luau"), "print('local')");
		store.SetResource(cache);

		rebuilt = engine::game::ReadInstanceDocument(store, document, NULL_ENTITY, error);
	});

	REQUIRE(rebuilt != NULL_ENTITY);
	CHECK(error.empty());

	universe.Enter(to, [&](Store &store) {
		CHECK(store.InstanceNameOf(rebuilt) == Name("Model"));
		CHECK(ChildNamed(store, rebuilt, "Child") != NULL_ENTITY);
		CHECK(store.FindFirstRoot("Existing") != NULL_ENTITY);

		Vector3 size;
		REQUIRE(store.GetProperty(rebuilt, Name("Size"), &size, sizeof(size)));
		CHECK(size.Y == 8.0f);

		// **The destination's own scripts survive the merge.** `ReadSources`
		// replaces the resource outright, which is right for an empty world
		// and would have deleted this one.
		const auto *cache = store.Resource<engine::script::SourceCache>();
		REQUIRE(cache != nullptr);

		const std::string *local = cache->Find(Name("scripts/local.luau"));
		REQUIRE(local != nullptr);
		CHECK(*local == "print('local')");

		const std::string *moved = cache->Find(Name("scripts/behaviour.luau"));
		REQUIRE(moved != nullptr);
		CHECK(*moved == "print('moved')");

		// And the program that was not referenced stayed behind. A miss is a
		// null pointer here rather than an empty string, deliberately.
		CHECK(cache->Find(Name("scripts/stays.luau")) == nullptr);
	});
}

TEST_CASE("a viewer's own instances are not written into the file", "[game][roundtrip]") {
	// **The camera belongs to whoever is looking, not to the game.** The editor
	// makes one to show its viewport, a client makes one for its player, and
	// several people editing one game make one each. Writing any of them out
	// would hand one person's viewpoint to everybody who opened the file - and
	// with several editors, would add one per person per save, forever.
	RegisterEverything();

	Universe universe;
	const WorldId world = AddWorld(universe, "Start");

	universe.Enter(world, [](Store &store) {
		const Entity workspace = engine::scene::InstallServices(store);

		// Content: stays.
		store.SetParent(store.CreateInstance(engine::scene::PartClass(), "Floor"), workspace);

		// The viewer's: goes.
		const Entity camera = store.CreateInstance(engine::scene::CameraClass(), "Camera");
		store.SetParent(camera, workspace);
		store.Set(camera, engine::scene::TransientComponent{});

		// **A child of a transient instance is transient too**, without needing
		// its own mark: it belongs to the thing that owns it, and half a subtree
		// in the file would be a parent that does not exist.
		const Entity rig = store.CreateInstance(engine::scene::PartClass(), "CameraRig");
		store.SetParent(rig, camera);
	});

	std::string error;
	const std::string document = engine::game::WriteWorldDocument(universe, world, error);
	REQUIRE_FALSE(document.empty());

	CHECK(document.find("Floor") != std::string::npos);
	CHECK(document.find("\"Camera\"") == std::string::npos);
	CHECK(document.find("CameraRig") == std::string::npos);

	// And it reads back without them, rather than with a hole where they were.
	const WorldId copy = engine::game::ReadWorldDocument(universe, document, Name("Copy"), error);
	REQUIRE(copy.IsValid());

	universe.Enter(copy, [](Store &store) {
		const Entity workspace = store.FindFirstRoot("Workspace");
		REQUIRE(workspace != NULL_ENTITY);
		CHECK(ChildNamed(store, workspace, "Floor") != NULL_ENTITY);
		CHECK(ChildNamed(store, workspace, "Camera") == NULL_ENTITY);
	});
}

TEST_CASE("a format 1 file keeps its own settings", "[game][roundtrip]") {
	// **The compatibility that makes the move safe.** A file written before
	// the settings became an element has them on `<World>`, and a reader that
	// only looked in the child would load somebody's 30Hz scene at 60 without
	// saying anything. Read from the child when it is there and from the
	// element when it is not - no branch on the version number, because the
	// shape is what is being read.
	RegisterEverything();

	const auto path = ScratchFile("engine-game-format1.agame");
	{
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		file << R"(<?xml version="1.0" encoding="UTF-8"?>
			<Game format="1" name="Old">
				<World name="Legacy" tickRate="15" idleTickRate="5" faultLimit="9">
					<Item class="Part" name="Survivor" />
				</World>
			</Game>)";
	}

	Universe loaded;
	GameInfo info;
	std::string error;
	REQUIRE(LoadGame(loaded, path, info, error));
	CHECK(error.empty());

	const WorldId world = loaded.Find(Name("Legacy"));
	REQUIRE(world.IsValid());

	const WorldSettings settings = loaded.SettingsOf(world);
	CHECK(settings.TickRate == 15.0);
	CHECK(settings.IdleTickRate == 5.0);
	CHECK(settings.FaultLimit == 9);

	loaded.Enter(world, [](Store &store) { CHECK(store.FindFirstRoot("Survivor") != NULL_ENTITY); });

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
	// paths would be a game file that does not contain the game - send it to
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

		// The active container's path - a `.luau` file lands in the Luau one and
		// the selector follows it, which `script::SetSourcePath` decides once.
		CHECK(engine::script::ActiveSourceOf(store, script) == Name("Scripts/Main.luau"));
		CHECK(engine::script::ActiveLanguageOf(store, script) == engine::script::Language::Luau);
	});

	std::filesystem::remove(path);
}

TEST_CASE("a reference pointing forward in the tree resolves", "[game][roundtrip]") {
	RegisterEverything();

	// **The case a one-pass loader silently drops.** A camera naming a part
	// declared after it is ordinary content, and resolving references as they
	// are read would leave every forward one at its default - which looks like
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
	// starts writing `Parent` as a value fails loudly - two answers to one
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
	// it - the root element is `<Game>`, its children are `<World>`, and a
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

TEST_CASE("a world copies and renames without touching a disk", "[game][roundtrip]") {
	// **What the studio's Duplicate and Rename are made of.** Both are a write
	// followed by a read, and going through a temporary file to do them would
	// make two ordinary editor actions depend on somewhere being writable.
	RegisterEverything();

	Universe universe;
	const WorldId original = AddWorld(universe, "Arena");
	AddPart(universe, original, "Pillar");

	std::string error;
	const std::string document = engine::game::WriteWorldDocument(universe, original, error);
	REQUIRE_FALSE(document.empty());
	CHECK(error.empty());

	// Duplicate: the same document, read back under a free name.
	const WorldId copy = engine::game::ReadWorldDocument(universe, document, Name("Arena 2"), error);
	REQUIRE(copy.IsValid());
	CHECK(universe.Count() == 2);

	universe.Enter(copy, [](Store &store) { CHECK(store.FindFirstRoot("Pillar") != NULL_ENTITY); });

	// The original is untouched. A copy that moved what it copied would be a
	// rename wearing a duplicate's name.
	universe.Enter(original, [](Store &store) { CHECK(store.FindFirstRoot("Pillar") != NULL_ENTITY); });

	// Reading it back under a name already in use is refused rather than
	// producing two worlds nothing can tell apart.
	CHECK_FALSE(engine::game::ReadWorldDocument(universe, document, Name("Arena"), error).IsValid());
	CHECK_FALSE(error.empty());
}

TEST_CASE("a renamed world keeps its handle and its place", "[game][roundtrip]") {
	// **The property that stops a rename reordering the save file.** The studio
	// renames by destroying the world and reading it straight back;
	// `Universe::Adopt` reuses the hole a destroyed world leaves, so the scene
	// keeps its `WorldId` and its position among the others. Checked here
	// because it is a fact about `world::Universe` that the editor depends on
	// and neither module states in code.
	RegisterEverything();

	Universe universe;
	AddWorld(universe, "First");
	const WorldId middle = AddWorld(universe, "Middle");
	AddWorld(universe, "Last");

	std::string error;
	const std::string document = engine::game::WriteWorldDocument(universe, middle, error);
	REQUIRE_FALSE(document.empty());

	universe.Destroy(middle);
	const WorldId renamed = engine::game::ReadWorldDocument(universe, document, Name("Centre"), error);

	REQUIRE(renamed.IsValid());
	CHECK(renamed == middle);

	const auto worlds = universe.Worlds();
	REQUIRE(worlds.size() == 3);
	CHECK(universe.NameOf(worlds[0]) == Name("First"));
	CHECK(universe.NameOf(worlds[1]) == Name("Centre"));
	CHECK(universe.NameOf(worlds[2]) == Name("Last"));
}
