#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <studio/Commands.hpp>
#include <studio/UiExternalImport.hpp>
#include <studio/UiImportAdapters.hpp>

TEST_SUITE_ID("studio.ui_external_import")

namespace {
	struct UnrelatedWrite {
		int Value = 0;
	};

	struct ScopedJobs {
		ScopedJobs() {
			engine::parallel::Jobs::Start(1);
		}
		~ScopedJobs() {
			engine::parallel::Jobs::Stop();
		}
	};

	std::filesystem::path ImportPath(std::string_view suffix) {
		const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		return std::filesystem::temp_directory_path() /
			   ("atomic-ui-import-" + std::to_string(stamp) + std::string(suffix));
	}
}

TEST_CASE("Studio imports a localization CSV as one saved undoable table", "[studio][ui_import]") {
	ScopedJobs jobs;
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();
	engine::gui::RegisterGuiClasses();
	engine::world::Universe universe;
	const auto world = universe.Create({.Name = engine::core::Name("ui.localization.import")});
	studio::CommandLog commands(universe);
	REQUIRE(universe.Enter(world, [](engine::ecs::Store &store) {
		engine::scene::InstallServices(store);
	}) == engine::world::WorldStatus::Ok);

	const auto path = ImportPath(".csv");
	{
		std::ofstream output(path, std::ios::binary);
		output << "locale,key,text\nen,menu.play,Play\nfr,menu.play,Jouer\n";
	}
	const auto recording = commands.TryBeginRecording("Import localization table");
	REQUIRE(recording.has_value());
	engine::ecs::Entity table;
	engine::gui::DocumentReport report;
	bool imported = false;
	REQUIRE(universe.Enter(world, [&](engine::ecs::Store &store) {
		imported = studio::ImportUiLocalizationFileEdit(store, world, path, commands, table, report);
	}) == engine::world::WorldStatus::Ok);
	REQUIRE(imported);
	REQUIRE(commands.FinishRecording(*recording, studio::FinishOperation::Commit));

	universe.Enter(world, [&](engine::ecs::Store &store) {
		const auto replicated = engine::scene::ServiceOf(
			store, engine::ecs::Classes::Find(engine::core::Name("ReplicatedStorage"))
		);
		CHECK(store.ParentOf(table) == replicated);
		std::string value;
		REQUIRE(store.GetProperty(table, engine::core::Name("Value"), &value, sizeof(value)));
		CHECK(value == "locale,key,text\nen,menu.play,Play\nfr,menu.play,Jouer\n");
	});
	REQUIRE(commands.Undo());
	universe.Enter(world, [&](engine::ecs::Store &store) { CHECK_FALSE(store.Alive(table)); });
	REQUIRE(commands.Redo());
	universe.Enter(world, [&](engine::ecs::Store &store) {
		const auto classId = engine::ecs::Classes::Find(engine::core::Name("LocalizationTable"));
		const auto replicated = engine::scene::ServiceOf(
			store, engine::ecs::Classes::Find(engine::core::Name("ReplicatedStorage"))
		);
		bool found = false;
		store.EachChild(replicated, [&](const engine::ecs::Entity child) {
			if (store.ClassOf(child) == classId) found = true;
		});
		CHECK(found);
	});
	std::filesystem::remove(path);
}

TEST_CASE(
	"localization tables merge in hierarchy order and cache only relevant changes", "[studio][ui_import]"
) {
	ScopedJobs jobs;
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();
	engine::gui::RegisterGuiClasses();
	engine::ecs::Store store("ui.localization.cache");
	engine::scene::InstallServices(store);
	const auto tableClass = engine::ecs::Classes::Find(engine::core::Name("LocalizationTable"));
	const auto storage =
		engine::scene::ServiceOf(store, engine::ecs::Classes::Find(engine::core::Name("ReplicatedStorage")));
	const auto first = store.CreateInstance(tableClass, "Base");
	const auto override = store.CreateInstance(tableClass, "Override");
	REQUIRE(store.SetParent(first, storage));
	REQUIRE(store.SetParent(override, storage));
	std::string base = "locale,key,text\nen,menu.play,Play\nfr,menu.play,Jouer\n";
	std::string later = "locale,key,text\nfr,menu.play,Lancer\n";
	REQUIRE(store.SetProperty(first, engine::core::Name("Value"), &base, sizeof(base)));
	REQUIRE(store.SetProperty(override, engine::core::Name("Value"), &later, sizeof(later)));

	engine::gui::LocalizationCache cache;
	const auto *catalogue = cache.Refresh(store);
	engine::gui::LocalizedMessage message{.Key = engine::core::Name("menu.play"), .Source = "Play"};
	CHECK(catalogue->Resolve(message, "fr-CA") == "Lancer");
	CHECK(catalogue->Resolve(message, "de") == "Play");
	const uint64_t revision = catalogue->Revision();

	const auto unrelated = store.Create();
	store.Set(unrelated, UnrelatedWrite{1});
	CHECK(cache.Refresh(store)->Revision() == revision);

	later = "locale,key,text\nfr,menu.play,Démarrer\n";
	REQUIRE(store.SetProperty(override, engine::core::Name("Value"), &later, sizeof(later)));
	CHECK(cache.Refresh(store)->Resolve(message, "fr") == "Démarrer");
}

TEST_CASE("Studio imports a design-token file as one undoable theme transaction", "[studio][ui_import]") {
	ScopedJobs jobs;
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();
	engine::gui::RegisterGuiClasses();
	engine::world::Universe universe;
	const auto world = universe.Create({.Name = engine::core::Name("ui.design.import")});
	studio::CommandLog commands(universe);
	REQUIRE(universe.Enter(world, [](engine::ecs::Store &store) {
		engine::scene::InstallServices(store);
	}) == engine::world::WorldStatus::Ok);

	const auto path = ImportPath(".json");
	{
		std::ofstream output(path, std::ios::binary);
		output << R"json({"version":1,"theme":{"id":"theme.dark","name":"Dark","tokens":{
"BackgroundColor":{"type":"color","value":[0,0,0]}}}})json";
	}
	const auto recording = commands.TryBeginRecording("Import design tokens");
	REQUIRE(recording.has_value());
	std::vector<engine::ecs::Entity> roots;
	engine::gui::DocumentReport report;
	bool imported = false;
	REQUIRE(universe.Enter(world, [&](engine::ecs::Store &store) {
		imported = studio::ImportUiDesignTokensFileEdit(
			store, world, engine::ecs::NULL_ENTITY, path, commands, roots, report
		);
	}) == engine::world::WorldStatus::Ok);
	REQUIRE(imported);
	CHECK(roots.empty());
	REQUIRE(commands.FinishRecording(*recording, studio::FinishOperation::Commit));
	REQUIRE(commands.CanUndo());
	REQUIRE(commands.Undo());
	REQUIRE(commands.Redo());
	std::filesystem::remove(path);
}

TEST_CASE(
	"Studio refuses an oversized external UI import without changing its output", "[studio][ui_import]"
) {
	const auto path = ImportPath(".json");
	{
		std::ofstream output(path, std::ios::binary);
		output.seekp(studio::UiDesignImportLimits::HARD_MAXIMUM_BYTES);
		output.put('x');
	}
	std::string text = "keep";
	engine::gui::DocumentReport report;
	CHECK_FALSE(studio::ReadUiExternalImportFile(path, text, report));
	CHECK(text == "keep");
	REQUIRE_FALSE(report.Issues.empty());
	CHECK(report.Issues.front().Message == "UI import exceeds byte limit");
	std::filesystem::remove(path);
}
