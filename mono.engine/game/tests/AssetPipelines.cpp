// A world's bake pipelines survive being saved and loaded.
//
// **The claim is that an editor's work outlives the process**, which is the
// whole reason `bake::Document` records edits as data rather than letting a
// panel call the runtime directly. A pipeline that lived only in memory would
// make an Assets Pipeline widget a toy.
//
// The format underneath is `bakegraph`'s and is tested there — it round trips,
// it refuses a malformed line, it escapes a name that could forge structure.
// What is tested here is the *wiring*: that a world writes what it holds, reads
// back what it wrote, costs nothing when it holds none, and survives a file
// written by a build with a node kind this one does not have.
//
// **And that this suite links no importer**, which is the property `D00102` was
// about. `game` names `Engine::bakegraph`; if it ever named `Engine::bake`, a
// dedicated server would carry a JPEG decoder to parse a text document.

#include <engine/bakegraph/Document.hpp>
#include <engine/game/Game.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

TEST_SUITE_ID("engine.game.assetpipelines")
TEST_DEPENDS("engine.bakegraph.document")

using engine::bake::Document;
using engine::bake::Operation;
using engine::bake::OperationKind;
using engine::bake::PipelineSet;
using engine::core::Name;
using engine::ecs::Store;
using engine::game::GameInfo;
using engine::game::LoadGame;
using engine::game::SaveGame;
using engine::world::Universe;
using engine::world::WorldId;
using engine::world::WorldSettings;

namespace {
	WorldId AddWorld(Universe &universe, std::string_view name) {
		WorldSettings settings;
		settings.Name = Name(name);
		return universe.Create(settings);
	}

	std::filesystem::path ScratchFile(std::string_view leaf) {
		const auto path = std::filesystem::temp_directory_path() / leaf;
		std::filesystem::remove(path);
		return path;
	}

	Operation Builtin(std::string name) {
		Operation operation;
		operation.Kind = OperationKind::AddBuiltin;
		operation.Text = std::move(name);
		return operation;
	}

	Operation Bare(engine::bake::NodeKind kind) {
		Operation operation;
		operation.Kind = OperationKind::AddNode;
		operation.Node = kind;
		return operation;
	}

	Operation WriteNode(std::string name) {
		Operation operation;
		operation.Kind = OperationKind::AddWrite;
		operation.Text = std::move(name);
		return operation;
	}

	Operation Wire(uint32_t from, uint32_t to) {
		Operation operation;
		operation.Kind = OperationKind::Connect;
		operation.From = from;
		operation.To = to;
		return operation;
	}

	// A world holding two named pipelines, which is the case the roadmap line
	// asks for: many trees in one editor.
	//
	// **The damage the cases below do is always to the second one**, which is
	// what makes them mean anything: a set is written in name order, so a
	// refusal in `icons` happens with `boxes` already parsed and sitting in the
	// reader's hand. A file broken in its first pipeline would leave the reader
	// holding nothing, and a wiring that ignored the status entirely would still
	// end up putting nothing on the world — passing a test that had proved
	// nothing.
	PipelineSet TwoPipelines() {
		Document box;
		box.Record(Builtin("engine.Cube"));
		box.Record(Bare(engine::bake::NodeKind::Smooth));
		box.Record(WriteNode("box.amesh"));
		box.Record(Wire(1, 2));
		box.Record(Wire(2, 3));

		Document icon;
		icon.Record(Builtin("engine.Sphere"));
		icon.Record(Bare(engine::bake::NodeKind::Opaque));
		icon.Record(WriteNode("icon.amesh"));
		icon.Record(Wire(1, 2));
		icon.Record(Wire(2, 3));

		PipelineSet set;
		set.Set(Name("boxes"), std::move(box));
		set.Set(Name("icons"), std::move(icon));
		return set;
	}

	std::string TextOf(const std::filesystem::path &path) {
		std::ifstream file(path);
		return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	}

	void Rewrite(const std::filesystem::path &path, std::string_view from, std::string_view to) {
		std::string text = TextOf(path);
		const size_t at = text.find(from);
		REQUIRE(at != std::string::npos);
		text.replace(at, from.size(), to);

		std::ofstream out(path, std::ios::trunc);
		out << text;
	}
}

TEST_CASE("a world's asset pipelines survive a save and a load", "[game]") {
	// `RegisterGameClasses` names the pipeline resource beside the classes, so
	// nothing here has to remember to — which is the point of it being there.
	engine::game::RegisterGameClasses();

	Universe source;
	const WorldId start = AddWorld(source, "Start");
	source.Enter(start, [](Store &store) {
		store.CreateInstance(engine::scene::PartClass(), "Baseplate");
		store.SetResource(TwoPipelines());
	});

	const auto path = ScratchFile("engine-game-assetpipelines.agame");
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
		CHECK(set->Names()[0] == Name("boxes"));
		CHECK(set->Names()[1] == Name("icons"));

		// And the edits themselves, not merely the names: the wires are what a
		// format that wrote nodes and forgot links would lose silently.
		const Document *boxes = set->Find(Name("boxes"));
		REQUIRE(boxes != nullptr);
		REQUIRE(boxes->Count() == 5);
		CHECK(boxes->NodeCount() == 3);
		CHECK(boxes->Operations()[0].Text == "engine.Cube");
		CHECK(boxes->Operations()[1].Node == engine::bake::NodeKind::Smooth);
		CHECK(boxes->Operations()[4].To == 3);
	});
}

TEST_CASE("a world with no asset pipelines writes no element and loads clean", "[game]") {
	// **The ordinary case, and it must not cost a byte.** Every world that has
	// never been near a bake pipeline is this one, which is also why the new
	// element did not move `FORMAT_VERSION`.
	engine::game::RegisterGameClasses();

	Universe source;
	AddWorld(source, "Start");

	const auto path = ScratchFile("engine-game-assetpipelines-empty.agame");
	std::string error;
	REQUIRE(SaveGame(source, Name("Empty"), path, error));
	CHECK(TextOf(path).find("<AssetPipelines>") == std::string::npos);

	Universe loaded;
	GameInfo info;
	REQUIRE(LoadGame(loaded, path, info, error));

	const WorldId restored = loaded.Find(Name("Start"));
	REQUIRE(restored.IsValid());
	loaded.Enter(restored, [](Store &store) { CHECK(store.Resource<PipelineSet>() == nullptr); });
}

TEST_CASE("a node kind this build does not have loses the pipelines and keeps the world", "[game]") {
	// **The decision `D00102` left open, stated as a test.** A file from a build
	// with a `bevel` node is a file this one cannot honestly bake, and the
	// vocabulary is a closed list so it cannot tell that from a typo. It keeps
	// the parts and drops the pipelines: a world whose instances all loaded and
	// whose recipes did not is recoverable, and refusing the document would lose
	// somebody's level over a pipeline they can re-author.
	engine::game::RegisterGameClasses();

	Universe source;
	const WorldId start = AddWorld(source, "Start");
	source.Enter(start, [](Store &store) {
		store.CreateInstance(engine::scene::PartClass(), "Baseplate");
		store.SetResource(TwoPipelines());
	});

	const auto path = ScratchFile("engine-game-assetpipelines-future.agame");
	std::string error;
	REQUIRE(SaveGame(source, Name("Future"), path, error));

	// A node kind from a build that does not exist, in the *second* pipeline and
	// leaving the XML itself well formed — the failure has to be the pipeline's
	// and not the document's.
	Rewrite(path, "node opaque", "node bevel");

	Universe loaded;
	GameInfo info;
	REQUIRE(LoadGame(loaded, path, info, error));
	CHECK(error.empty());

	const WorldId restored = loaded.Find(Name("Start"));
	REQUIRE(restored.IsValid());
	loaded.Enter(restored, [](Store &store) {
		// The world, still here. This is the half that matters.
		CHECK(store.FindFirstRoot("Baseplate") != engine::ecs::NULL_ENTITY);

		// **The whole set and not the readable half.** Keeping `icons` because
		// it happened to parse would be this reader deciding that part of a
		// saved set is a set; it is not, it is a file from a newer build.
		CHECK(store.Resource<PipelineSet>() == nullptr);
	});
}

TEST_CASE("a corrupted pipeline block loses the pipelines and keeps the world", "[game]") {
	// The same recovery, reached the other way: a file somebody hand-edited
	// badly rather than one written by a newer build.
	engine::game::RegisterGameClasses();

	Universe source;
	const WorldId start = AddWorld(source, "Start");
	source.Enter(start, [](Store &store) {
		store.CreateInstance(engine::scene::PartClass(), "Baseplate");
		store.SetResource(TwoPipelines());
	});

	const auto path = ScratchFile("engine-game-assetpipelines-broken.agame");
	std::string error;
	REQUIRE(SaveGame(source, Name("Broken"), path, error));

	Rewrite(path, "write \"icon.amesh\"", "explode \"icon.amesh\"");

	Universe loaded;
	GameInfo info;
	REQUIRE(LoadGame(loaded, path, info, error));

	const WorldId restored = loaded.Find(Name("Start"));
	REQUIRE(restored.IsValid());
	loaded.Enter(restored, [](Store &store) {
		CHECK(store.FindFirstRoot("Baseplate") != engine::ecs::NULL_ENTITY);
		CHECK(store.Resource<PipelineSet>() == nullptr);
	});
}
