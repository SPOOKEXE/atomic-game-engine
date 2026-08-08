// A world's render pipelines survive being saved and loaded.
//
// **The claim is that an editor's work outlives the process**, which is the
// whole reason `graph::PipelineDocument` records edits as data rather than
// letting a panel call the runtime directly. A pipeline that lived only in
// memory would make the Render Pipeline widget a toy.
//
// The format underneath is `graph`'s and is tested there — round trips, refuses
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
using engine::graph::PipelineSet;
using engine::graph::StandardDocument;
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
		set.Set(Name("main"), StandardDocument());

		engine::graph::PipelineDocument cheap;
		engine::graph::Edit resource;
		resource.Kind = engine::graph::EditKind::AddResource;
		resource.Name = Name("colour");
		cheap.Record(resource);

		engine::graph::Edit node;
		node.Kind = engine::graph::EditKind::AddNode;
		node.Name = Name("opaque");
		node.NodeKind = Name("opaque");
		cheap.Record(node);

		engine::graph::Edit writes;
		writes.Kind = engine::graph::EditKind::Writes;
		writes.Target = Name("colour");
		cheap.Record(writes);

		set.Set(Name("reflection"), std::move(cheap));
		return set;
	}
}

TEST_CASE("a world's pipelines survive a save and a load", "[game]") {
	// `RegisterGameClasses` names the pipeline resource beside the classes, so
	// nothing here has to remember to — which is the point of it being there.
	engine::game::RegisterGameClasses();

	Universe source;
	const WorldId start = AddWorld(source, "Start");
	source.Enter(start, [](Store &store) { store.SetResource(TwoPipelines()); });

	const auto path = ScratchFile("engine-game-pipelines.agame");
	std::string error;
	REQUIRE(SaveGame(source, Name("Pipelines"), path, error));
	CHECK(error.empty());

	Universe loaded;
	GameInfo info;
	REQUIRE(LoadGame(loaded, path, info, error));
	CHECK(error.empty());

	const WorldId restored = loaded.Find(Name("Start"));
	REQUIRE(restored.IsValid());

	loaded.Enter(restored, [](Store &store) {
		const auto *set = store.Resource<PipelineSet>();
		REQUIRE(set != nullptr);

		// **Both trees, by name.** A wiring that wrote only the first would pass
		// a count check on a one-pipeline world, which is why this one holds
		// two.
		REQUIRE(set->Count() == 2);
		CHECK(set->Names()[0] == Name("main"));
		CHECK(set->Names()[1] == Name("reflection"));

		// And the main one still describes a frame that compiles, which is the
		// difference between text that reloaded and a pipeline that works.
		const engine::graph::PipelineDocument *main = set->Find(Name("main"));
		REQUIRE(main != nullptr);

		engine::graph::RenderGraph graph;
		Name offender;
		CHECK(Build(*main, graph, offender) == engine::graph::PipelineDocumentStatus::Ok);
	});
}

TEST_CASE("a world with no pipelines writes no element and loads clean", "[game]") {
	// **The ordinary case, and it must not cost a byte.** Every world that has
	// never been near the editor is this one.
	// `RegisterGameClasses` names the pipeline resource beside the classes, so
	// nothing here has to remember to — which is the point of it being there.
	engine::game::RegisterGameClasses();

	Universe source;
	AddWorld(source, "Start");

	const auto path = ScratchFile("engine-game-pipelines-empty.agame");
	std::string error;
	REQUIRE(SaveGame(source, Name("Empty"), path, error));

	std::ifstream file(path);
	const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	CHECK(text.find("<Pipelines>") == std::string::npos);

	Universe loaded;
	GameInfo info;
	REQUIRE(LoadGame(loaded, path, info, error));

	const WorldId restored = loaded.Find(Name("Start"));
	REQUIRE(restored.IsValid());
	loaded.Enter(restored, [](Store &store) { CHECK(store.Resource<PipelineSet>() == nullptr); });
}

TEST_CASE("a corrupted pipeline block loses the pipeline and keeps the world", "[game]") {
	// **The recovery decision, stated as a test.** A world whose parts all
	// loaded and whose pipeline did not is recoverable — it draws with the
	// standard frame and somebody fixes the pipeline. Refusing the document
	// would lose the parts too, which is a worse answer to the same file.
	// `RegisterGameClasses` names the pipeline resource beside the classes, so
	// nothing here has to remember to — which is the point of it being there.
	engine::game::RegisterGameClasses();

	Universe source;
	const WorldId start = AddWorld(source, "Start");
	source.Enter(start, [](Store &store) { store.SetResource(TwoPipelines()); });

	const auto path = ScratchFile("engine-game-pipelines-broken.agame");
	std::string error;
	REQUIRE(SaveGame(source, Name("Broken"), path, error));

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
	loaded.Enter(restored, [](Store &store) { CHECK(store.Resource<PipelineSet>() == nullptr); });
}
