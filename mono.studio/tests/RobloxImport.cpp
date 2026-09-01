// Roblox place analysis without an ImGui window.

#include <engine/effects/Registration.hpp>
#include <engine/game/Game.hpp>
#include <engine/scene/Part.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <studio/Config.hpp>
#include <studio/RobloxImport.hpp>
#include <studio/RojoSync.hpp>

TEST_SUITE_ID("studio.robloximport")
TEST_DEPENDS("engine.scene.part")

namespace {
	struct ScratchConfig {
		std::filesystem::path Root = std::filesystem::temp_directory_path() / "atomic-roblox-import-test";

		ScratchConfig() {
			std::filesystem::remove_all(Root);
			studio::SetConfigRoot(Root);
		}

		~ScratchConfig() {
			studio::SetConfigRoot({});
			std::filesystem::remove_all(Root);
		}
	};
}

TEST_CASE(
	"roblox analysis separates missing classes properties and type conflicts", "[studio][robloximport]"
) {
	engine::scene::EnsureClassTree();

	engine::bake::RobloxModel model;
	engine::bake::RobloxInstance part;
	part.ClassName = "Part";
	part.Name = "Known";
	engine::bake::RobloxValue size;
	size.Kind = engine::bake::RobloxValueKind::Vector3;
	part.Properties.push_back({"Size", size});
	engine::bake::RobloxValue wrongTransparency;
	wrongTransparency.Kind = engine::bake::RobloxValueKind::Text;
	part.Properties.push_back({"Transparency", wrongTransparency});
	part.Properties.push_back({"RobloxOnly", size});

	engine::bake::RobloxInstance future;
	future.ClassName = "FutureRobloxClass";
	future.Name = "Future";
	part.Children.push_back(std::move(future));
	model.Roots.push_back(std::move(part));

	const studio::RobloxImportAnalysis analysis = studio::AnalyzeRobloxImport(model);
	CHECK(analysis.Instances == 2);
	CHECK(analysis.Classes == 2);
	REQUIRE(analysis.MissingClasses.size() == 1);
	CHECK(analysis.MissingClasses[0].ClassName == "FutureRobloxClass");
	REQUIRE(analysis.MissingProperties.size() == 1);
	CHECK(analysis.MissingProperties[0].PropertyName == "RobloxOnly");
	REQUIRE(analysis.ConflictingProperties.size() == 1);
	CHECK(analysis.ConflictingProperties[0].PropertyName == "Transparency");
	CHECK(analysis.ConflictingProperties[0].SourceType == "Text");
}

TEST_CASE("roblox asset choices group uses and keep persistent selections", "[studio][robloximport]") {
	engine::bake::RobloxModel model;
	model.Assets.push_back({
		"123",
		"rbxassetid://123",
		engine::bake::RobloxAssetKind::Animation,
		"Workspace/Animation",
		"Animation",
		"AnimationId",
	});
	model.Assets.push_back({
		"123",
		"https://www.roblox.com/asset/?id=123",
		engine::bake::RobloxAssetKind::Animation,
		"Workspace/Driver",
		"LocalScript",
		"Source",
	});

	studio::RobloxAssetMappings mappings{{"123", "animations/slash.anim"}};
	const std::vector<studio::RobloxAssetChoice> choices = studio::RobloxAssetChoices(model, mappings);
	REQUIRE(choices.size() == 1);
	CHECK(choices[0].Identifier == "123");
	CHECK(choices[0].Uses == 2);
	CHECK(choices[0].LocalAsset == "animations/slash.anim");
}

TEST_CASE("roblox asset mappings survive a config round trip", "[studio][robloximport]") {
	ScratchConfig scratch;
	studio::RobloxAssetMappings written{
		{"123", "animations/slash.anim"},
		{"456", "textures/sword.png"},
	};
	std::string error;
	REQUIRE(studio::SaveRobloxAssetMappings(written, error));

	studio::RobloxAssetMappings loaded;
	REQUIRE(studio::LoadRobloxAssetMappings(loaded, error));
	CHECK(loaded == written);
}

TEST_CASE("roblox class mappings survive a config round trip", "[studio][robloximport]") {
	ScratchConfig scratch;
	studio::RobloxClassMappings written{
		{"MeshPart", "Part"},
		{"RobloxOnlyContainer", "Folder"},
	};
	std::string error;
	REQUIRE(studio::SaveRobloxClassMappings(written, error));

	studio::RobloxClassMappings loaded;
	REQUIRE(studio::LoadRobloxClassMappings(loaded, error));
	CHECK(loaded == written);
}

TEST_CASE("a missing roblox class can map to an engine class", "[studio][robloximport]") {
	engine::scene::EnsureClassTree();
	engine::bake::RobloxModel model;
	engine::bake::RobloxInstance source;
	source.ClassName = "FuturePart";
	source.Name = "Mapped";
	engine::bake::RobloxValue size;
	size.Kind = engine::bake::RobloxValueKind::Vector3;
	size.Vector3 = {4.0f, 2.0f, 6.0f};
	source.Properties.push_back({"Size", size});
	model.Roots.push_back(std::move(source));

	const studio::RobloxClassMappings classMappings{{"FuturePart", "Part"}};
	const studio::RobloxImportAnalysis analysis = studio::AnalyzeRobloxImport(model, classMappings);
	REQUIRE(analysis.MissingClasses.size() == 1);
	CHECK(analysis.MissingProperties.empty());
	CHECK(analysis.ConflictingProperties.empty());

	engine::ecs::Store store("roblox-class-map");
	studio::RobloxImportResult report;
	std::string error;
	REQUIRE(
		studio::ImportRobloxPlace(store, model, studio::RobloxAssetMappings{}, classMappings, report, error)
	);
	const engine::ecs::Entity mapped = store.FindFirstRoot("Mapped");
	REQUIRE(mapped != engine::ecs::NULL_ENTITY);
	CHECK(store.ClassOf(mapped) == engine::scene::PartClass());
	CHECK(report.Properties == 1);
}

TEST_CASE("a roblox place merges service roots and stages mapped script source", "[studio][robloximport]") {
	engine::scene::EnsureClassTree();
	engine::ecs::Store store("roblox-import");
	const engine::ecs::Entity workspace = store.CreateInstance(studio::FolderClass(), "Workspace");
	REQUIRE(workspace != engine::ecs::NULL_ENTITY);

	engine::bake::RobloxModel model;
	engine::bake::RobloxInstance root;
	root.ClassName = "Workspace";
	root.Name = "Workspace";

	engine::bake::RobloxInstance part;
	part.ClassName = "Part";
	part.Name = "Floor";
	engine::bake::RobloxValue size;
	size.Kind = engine::bake::RobloxValueKind::Vector3;
	size.Vector3 = {20.0f, 1.0f, 20.0f};
	part.Properties.push_back({"Size", size});
	root.Children.push_back(std::move(part));

	engine::bake::RobloxInstance script;
	script.ClassName = "LocalScript";
	script.Name = "Driver";
	engine::bake::RobloxValue source;
	source.Kind = engine::bake::RobloxValueKind::Text;
	source.Text = "local animation = 'rbxassetid://123'";
	script.Properties.push_back({"Source", source});
	root.Children.push_back(std::move(script));
	model.Roots.push_back(std::move(root));
	model.Assets.push_back({
		"123",
		"rbxassetid://123",
		engine::bake::RobloxAssetKind::Animation,
		"Workspace/Driver",
		"LocalScript",
		"Source",
	});

	studio::RobloxImportResult report;
	std::string error;
	REQUIRE(
		studio::ImportRobloxPlace(
			store,
			model,
			studio::RobloxAssetMappings{{"123", "animations/slash.anim"}},
			studio::RobloxClassMappings{},
			report,
			error
		)
	);
	CHECK(error.empty());
	CHECK(report.ReusedRoots == 1);
	CHECK(report.Instances == 3);
	CHECK(report.Scripts == 1);
	CHECK(report.DisabledScripts == 1);

	const engine::script::SourceCache *cache = store.Resource<engine::script::SourceCache>();
	REQUIRE(cache != nullptr);
	const std::string *program = cache->Find(engine::core::Name("Workspace/Driver"));
	REQUIRE(program != nullptr);
	CHECK(program->find("animations/slash.anim") != std::string::npos);
	CHECK(store.FindFirstRoot("Workspace") == workspace);
	const engine::ecs::Entity driver = store.FindFirstChild(workspace, "Driver");
	REQUIRE(driver != engine::ecs::NULL_ENTITY);
	CHECK(store.Has<engine::script::Disabled>(driver));
}
