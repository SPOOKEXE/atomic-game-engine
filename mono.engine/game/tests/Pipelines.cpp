// A universe's rendering profiles survive being saved and loaded.
//
// **The claim is that an editor's work outlives the process**, which is the
// whole reason `graph::PipelineDocument` records edits as data rather than
// letting a panel call the runtime directly. A pipeline that lived only in
// memory would make the Render Pipeline widget a toy.
//
// The format underneath is `graph`'s and is tested there - round trips, refuses
// a malformed line, escapes a name that could forge structure. What is tested
// here is the *wiring*: that a world writes what it holds, reads back what it
// wrote, and survives a file somebody corrupted.

#include <engine/game/Game.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

TEST_SUITE_ID("engine.game.pipelines")
TEST_DEPENDS("engine.graph.pipelinedocument")

using engine::core::Name;
using engine::ecs::Store;
using engine::game::GameInfo;
using engine::game::LoadGame;
using engine::game::SaveGame;
using engine::graph::DefaultPbrDocument;
using engine::graph::PipelineSet;
using engine::world::Universe;
using engine::world::WorldId;
using engine::world::WorldSettings;

namespace {
	WorldId AddWorld(Universe &universe, std::string_view name) {
		WorldSettings settings;
		settings.Name = Name(name);
		return universe.Create(settings);
	}

	std::filesystem::path ScratchFile(std::string_view name) {
		const auto path = std::filesystem::temp_directory_path() / name;
		std::filesystem::remove(path);
		return path;
	}

	// A world holding two named pipelines, which is the case the roadmap line
	// asks for: many trees in one editor.
	PipelineSet TwoPipelines() {
		PipelineSet set;
		set.Set(Name("main"), DefaultPbrDocument());

		engine::graph::PipelineDocument cheap;
		engine::graph::Edit resource;
		resource.Kind = engine::graph::EditKind::AddResource;
		resource.Name = Name("colour");
		cheap.Record(resource);

		engine::graph::Edit node;
		node.Kind = engine::graph::EditKind::AddNode;
		node.Name = Name("capture");
		node.NodeKind = Name("capture");
		cheap.Record(node);

		engine::graph::Edit writes;
		writes.Kind = engine::graph::EditKind::Writes;
		writes.Target = Name("colour");
		cheap.Record(writes);

		set.Set(Name("reflection"), std::move(cheap));
		return set;
	}
}

TEST_CASE("universe rendering profiles survive a save and a load", "[game]") {
	// `RegisterGameClasses` names the pipeline resource beside the classes, so
	// nothing here has to remember to - which is the point of it being there.
	engine::game::RegisterGameClasses();

	Universe source;
	const WorldId start = AddWorld(source, "Start");
	REQUIRE(source.SetRenderingProfile(start, Name("reflection")) == engine::world::WorldStatus::Ok);
	const PipelineSet profiles = TwoPipelines();

	const auto path = ScratchFile("engine-game-pipelines.agame");
	std::string error;
	REQUIRE(SaveGame(source, Name("Pipelines"), profiles, path, error));
	CHECK(error.empty());

	Universe loaded;
	GameInfo info;
	REQUIRE(LoadGame(loaded, path, info, error));
	CHECK(error.empty());

	const WorldId restored = loaded.Find(Name("Start"));
	REQUIRE(restored.IsValid());
	CHECK(loaded.SettingsOf(restored).RenderingProfile == Name("reflection"));

	const PipelineSet &set = info.RenderingProfiles;
	REQUIRE(set.Count() == 2);
	CHECK(set.Names()[0] == Name("main"));
	CHECK(set.Names()[1] == Name("reflection"));

	// And the main one still describes a frame that compiles, which is the
	// difference between text that reloaded and a profile that works.
	const engine::graph::PipelineDocument *main = set.Find(Name("main"));
	REQUIRE(main != nullptr);

	engine::graph::RenderGraph graph;
	Name offender;
	CHECK(Build(*main, graph, offender) == engine::graph::PipelineDocumentStatus::Ok);
}

TEST_CASE("rendering profiles are written once outside every world", "[game]") {
	Universe source;
	AddWorld(source, "One");
	AddWorld(source, "Two");

	const std::string text = engine::game::WriteGame(source, Name("Profiles"), TwoPipelines());
	const size_t first = text.find("<RenderingProfiles>");
	REQUIRE(first != std::string::npos);
	CHECK(text.find("<RenderingProfiles>", first + 1) == std::string::npos);
	CHECK(text.find("<Pipelines>") == std::string::npos);
}

TEST_CASE("an empty rendering profile library writes no element and loads clean", "[game]") {
	// The ordinary case for a program constructing a universe directly, and it
	// must not cost a byte.
	Universe source;
	AddWorld(source, "Start");

	const auto path = ScratchFile("engine-game-pipelines-empty.agame");
	std::string error;
	REQUIRE(SaveGame(source, Name("Empty"), path, error));

	std::ifstream file(path);
	const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	CHECK(text.find("<RenderingProfiles>") == std::string::npos);

	Universe loaded;
	GameInfo info;
	REQUIRE(LoadGame(loaded, path, info, error));

	const WorldId restored = loaded.Find(Name("Start"));
	REQUIRE(restored.IsValid());
	CHECK(info.RenderingProfiles.Count() == 0);
}

TEST_CASE("a corrupted profile block loses the profiles and keeps every world", "[game]") {
	// **The recovery decision, stated as a test.** A universe whose worlds all
	// loaded and whose profiles did not is recoverable. It draws with the
	// standard frame and somebody fixes the pipeline. Refusing the document
	// would lose the parts too, which is a worse answer to the same file.
	Universe source;
	AddWorld(source, "Start");

	const auto path = ScratchFile("engine-game-pipelines-broken.agame");
	std::string error;
	REQUIRE(SaveGame(source, Name("Broken"), TwoPipelines(), path, error));

	// Corrupt one edit inside the block, leaving the XML itself well formed.
	{
		std::ifstream in(path);
		std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		in.close();

		const size_t at = text.find("pipeline \"main\"");
		REQUIRE(at != std::string::npos);
		text.replace(at, std::string("pipeline \"main\"").size(), "explode        ");

		std::ofstream out(path, std::ios::trunc);
		out << text;
	}

	Universe loaded;
	GameInfo info;
	REQUIRE(LoadGame(loaded, path, info, error));

	const WorldId restored = loaded.Find(Name("Start"));
	REQUIRE(restored.IsValid());
	CHECK(info.RenderingProfiles.Count() == 0);
}

TEST_CASE("format two world pipelines migrate into universe profiles", "[game]") {
	const auto path = ScratchFile("engine-game-pipelines-legacy.agame");
	const std::string legacy = "<Game format=\"2\" name=\"Legacy\">\n"
							   "\t<Universe mode=\"WorldParallel\" catchUp=\"8\" busBudget=\"64\" />\n"
							   "\t<World name=\"Start\">\n"
							   "\t\t<WorldProperties tickRate=\"60\" idleTickRate=\"2\" faultLimit=\"3\" />\n"
							   "\t\t<Pipelines><![CDATA[" +
							   engine::graph::Write(TwoPipelines()) +
							   "]]></Pipelines>\n"
							   "\t</World>\n"
							   "</Game>\n";
	{
		std::ofstream file(path, std::ios::trunc);
		file << legacy;
	}

	Universe loaded;
	GameInfo info;
	std::string error;
	REQUIRE(LoadGame(loaded, path, info, error));
	REQUIRE(info.RenderingProfiles.Count() == 2);

	const WorldId restored = loaded.Find(Name("Start"));
	REQUIRE(restored.IsValid());
	CHECK(loaded.SettingsOf(restored).RenderingProfile == Name("main"));
	loaded.Enter(restored, [](Store &store) { CHECK(store.Resource<PipelineSet>() == nullptr); });
}
