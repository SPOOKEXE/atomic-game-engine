// Roblox place analysis without an ImGui window.

#include <engine/effects/Registration.hpp>
#include <engine/game/Game.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/spatial/CollisionGroups.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
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

	struct CollisionGroupScope {
		CollisionGroupScope() {
			engine::spatial::CollisionGroups::Reset();
		}

		~CollisionGroupScope() {
			engine::spatial::CollisionGroups::Reset();
		}
	};

	engine::bake::RobloxInstance ScriptNode(std::string name, std::string className, std::string source) {
		engine::bake::RobloxInstance script;
		script.Name = std::move(name);
		script.ClassName = std::move(className);
		engine::bake::RobloxValue value;
		value.Set(std::move(source));
		script.Properties.push_back({"Source", std::move(value)});
		return script;
	}

	engine::bake::RobloxModel RojoSetupModel() {
		engine::bake::RobloxModel model;
		engine::bake::RobloxInstance service;
		service.Name = "ServerScriptService";
		service.ClassName = "ServerScriptService";
		service.Children.push_back(ScriptNode("Boot", "Script", "print('boot')\n"));

		engine::bake::RobloxInstance shared;
		shared.Name = "Shared";
		shared.ClassName = "Folder";
		shared.Children.push_back(ScriptNode("Util", "ModuleScript", "return { ready = true }\n"));
		service.Children.push_back(std::move(shared));

		engine::bake::RobloxInstance rig;
		rig.Name = "Rig";
		rig.ClassName = "Model";
		rig.Children.push_back(ScriptNode("Move", "Script", "print('complex')\n"));
		service.Children.push_back(std::move(rig));
		service.Children.push_back(ScriptNode("init", "ModuleScript", "return {}\n"));
		model.Roots.push_back(std::move(service));

		model.Scripts.push_back({"ServerScriptService/Boot", "Script", "print('boot')\n"});
		model.Scripts.push_back(
			{"ServerScriptService/Shared/Util", "ModuleScript", "return { ready = true }\n"}
		);
		model.Scripts.push_back({"ServerScriptService/Rig/Move", "Script", "print('complex')\n"});
		model.Scripts.push_back({"ServerScriptService/init", "ModuleScript", "return {}\n"});
		return model;
	}
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
	size.Set(engine::core::Vector3{});
	part.Properties.push_back({"Size", size});
	engine::bake::RobloxValue wrongTransparency;
	wrongTransparency.Set(std::string{});
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

TEST_CASE("roblox scripts report simple and invalid rojo subjects", "[studio][robloximport][rojo]") {
	const std::vector<studio::RobloxRojoSubject> subjects = studio::RobloxRojoSubjects(RojoSetupModel());
	REQUIRE(subjects.size() == 4);

	CHECK(subjects[0].Valid);
	CHECK(subjects[0].SourcePath == "src/ServerScriptService/Boot.server.luau");
	CHECK(subjects[1].Valid);
	CHECK(subjects[1].SourcePath == "src/ServerScriptService/Shared/Util.luau");
	CHECK_FALSE(subjects[2].Valid);
	CHECK(subjects[2].Reason.find("Model") != std::string::npos);
	CHECK_FALSE(subjects[3].Valid);
	CHECK(subjects[3].Reason.find("init") != std::string::npos);
}

TEST_CASE(
	"ambiguous and escaping roblox script paths are not rojo subjects", "[studio][robloximport][rojo]"
) {
	engine::bake::RobloxModel model;
	engine::bake::RobloxInstance service;
	service.Name = "ServerScriptService";
	service.ClassName = "ServerScriptService";
	service.Children.push_back(ScriptNode("Boot", "Script", "print(1)"));
	service.Children.push_back(ScriptNode("Boot", "Script", "print(2)"));
	service.Children.push_back(ScriptNode("../Escape", "Script", "print(3)"));
	model.Roots.push_back(std::move(service));
	model.Scripts.push_back({"ServerScriptService/Boot", "Script", "print(1)"});
	model.Scripts.push_back({"ServerScriptService/Boot", "Script", "print(2)"});
	model.Scripts.push_back({"ServerScriptService/../Escape", "Script", "print(3)"});

	const std::vector<studio::RobloxRojoSubject> subjects = studio::RobloxRojoSubjects(model);
	REQUIRE(subjects.size() == 3);
	CHECK_FALSE(subjects[0].Valid);
	CHECK(subjects[0].Reason.find("ambiguous") != std::string::npos);
	CHECK_FALSE(subjects[1].Valid);
	CHECK(subjects[1].Reason.find("ambiguous") != std::string::npos);
	CHECK_FALSE(subjects[2].Valid);
	CHECK(subjects[2].Reason.find("safe Rojo file name") != std::string::npos);
}

TEST_CASE("roblox rojo setup writes a new repeatable project", "[studio][robloximport][rojo]") {
	ScratchConfig scratch;
	std::filesystem::create_directories(scratch.Root);
	const std::filesystem::path destination = scratch.Root / "game-rojo";
	const engine::bake::RobloxModel model = RojoSetupModel();

	studio::RobloxRojoSetupResult setup;
	std::string error;
	REQUIRE(studio::SetupRobloxRojoProject(model, destination, "ImportedGame", setup, error));
	CHECK(error.empty());
	CHECK(setup.ScriptsWritten == 2);
	CHECK(setup.ProjectFile == destination / "default.project.json");

	std::ifstream boot(destination / "src/ServerScriptService/Boot.server.luau", std::ios::binary);
	std::ostringstream bootText;
	bootText << boot.rdbuf();
	CHECK(bootText.str() == "print('boot')\n");
	CHECK_FALSE(std::filesystem::exists(destination / "src/ServerScriptService/Rig/Move.server.luau"));

	std::ifstream projectFile(setup.ProjectFile, std::ios::binary);
	std::ostringstream projectText;
	projectText << projectFile.rdbuf();
	studio::RojoProject project;
	REQUIRE(studio::ParseRojoProject(projectText.str(), project, error));

	engine::ecs::Store store("generated-rojo");
	engine::scene::InstallServices(store);
	studio::RojoSyncReport first;
	REQUIRE(studio::SyncRojoProject(project, destination, store, first, error));
	CHECK(first.Scripts == 2);
	const engine::ecs::Entity service = store.FindFirstRoot("ServerScriptService");
	REQUIRE(service != engine::ecs::NULL_ENTITY);
	const engine::ecs::Entity bootScript = store.FindFirstChild(service, "Boot");
	REQUIRE(bootScript != engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(bootScript, "Boot") == engine::ecs::NULL_ENTITY);
	const engine::script::SourceCache *cache = store.Resource<engine::script::SourceCache>();
	REQUIRE(cache != nullptr);
	const std::string *source = cache->Find(engine::core::Name("src/ServerScriptService/Boot.server.luau"));
	REQUIRE(source != nullptr);
	CHECK(*source == "print('boot')\n");

	studio::RojoSyncReport second;
	REQUIRE(studio::SyncRojoProject(project, destination, store, second, error));
	CHECK(second.Instances == 0);
	CHECK(second.Scripts == 2);

	studio::RobloxRojoSetupResult refused;
	CHECK_FALSE(studio::SetupRobloxRojoProject(model, destination, "Changed", refused, error));
	CHECK(error.find("no files were overwritten") != std::string::npos);
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
	size.Set(engine::core::Vector3{4.0f, 2.0f, 6.0f});
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

TEST_CASE("a roblox collision group is registered before the part uses it", "[studio][robloximport]") {
	CollisionGroupScope collisionGroups;
	engine::scene::EnsureClassTree();

	engine::bake::RobloxModel model;
	engine::bake::RobloxInstance part;
	part.ClassName = "Part";
	part.Name = "Enemy";
	engine::bake::RobloxValue group;
	group.Set(std::string("Enemies"));
	part.Properties.push_back({"CollisionGroup", std::move(group)});
	model.Roots.push_back(std::move(part));

	engine::ecs::Store store("roblox-collision-group");
	studio::RobloxImportResult report;
	std::string error;
	REQUIRE(
		studio::ImportRobloxPlace(
			store, model, studio::RobloxAssetMappings{}, studio::RobloxClassMappings{}, report, error
		)
	);

	const uint32_t index = engine::spatial::CollisionGroups::IndexOf(engine::core::Name("Enemies"));
	REQUIRE(index != engine::spatial::NO_GROUP);
	const engine::ecs::Entity enemy = store.FindFirstRoot("Enemy");
	REQUIRE(enemy != engine::ecs::NULL_ENTITY);
	const engine::scene::Collider *collider = store.Get<engine::scene::Collider>(enemy);
	REQUIRE(collider != nullptr);
	CHECK(collider->Layer.Bits == engine::spatial::LayerMask::Only(index).Bits);
	CHECK(collider->Mask.Bits == engine::spatial::CollisionGroups::MaskFor(index).Bits);
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
	size.Set(engine::core::Vector3{20.0f, 1.0f, 20.0f});
	part.Properties.push_back({"Size", size});
	engine::bake::RobloxValue colour;
	colour.Set(engine::core::Color3{0.2f, 0.4f, 0.8f});
	part.Properties.push_back({"Color", colour});
	root.Children.push_back(std::move(part));

	engine::bake::RobloxInstance script;
	script.ClassName = "LocalScript";
	script.Name = "Driver";
	engine::bake::RobloxValue source;
	source.Set(std::string("local animation = 'rbxassetid://123'"));
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

	// The import must apply the values, not merely count the recognized
	// properties. `Size` writes both the render bounds and collision extent,
	// while `Color` maps Roblox's spelling to the part visual tint.
	const engine::ecs::Entity floor = store.FindFirstChild(workspace, "Floor");
	REQUIRE(floor != engine::ecs::NULL_ENTITY);
	const engine::scene::Bounds *bounds = store.Get<engine::scene::Bounds>(floor);
	REQUIRE(bounds != nullptr);
	CHECK(bounds->HalfExtent == engine::core::Vector3{10.0f, 0.5f, 10.0f});
	const engine::scene::Visual *visual = store.Get<engine::scene::Visual>(floor);
	REQUIRE(visual != nullptr);
	CHECK(visual->Tint == engine::core::Color3{0.2f, 0.4f, 0.8f});
}

TEST_CASE("a roblox place port writes and reloads its world", "[studio][robloximport]") {
	ScratchConfig scratch;
	std::filesystem::create_directories(scratch.Root);
	const std::filesystem::path source = scratch.Root / "source.rbxlx";
	const std::filesystem::path destination = scratch.Root / "Ported.aworld";
	std::ofstream(source) << R"xml(
		<roblox version="4">
			<Item class="Part" referent="RBX0">
				<Properties>
					<string name="Name">Block</string>
					<Vector3 name="Size"><X>8</X><Y>3</Y><Z>2</Z></Vector3>
					<Color3uint8 name="Color">4294901760</Color3uint8>
				</Properties>
			</Item>
		</roblox>
	)xml";

	studio::RobloxWorldPortResult report;
	std::string error;
	REQUIRE(
		studio::PortRobloxPlace(
			source, destination, studio::RobloxAssetMappings{}, studio::RobloxClassMappings{}, report, error
		)
	);
	CHECK(error.empty());
	CHECK(report.Analysis.Instances == 1);
	CHECK(report.Import.Instances == 1);
	CHECK(std::filesystem::file_size(destination) > 0);

	engine::world::Universe loaded;
	const engine::world::WorldId world =
		engine::game::ImportWorld(loaded, destination, engine::core::Name{}, error);
	REQUIRE(world.IsValid());
	CHECK(error.empty());
	loaded.Enter(world, [](engine::ecs::Store &store) {
		const engine::ecs::Entity block = store.FindFirstRoot("Block");
		REQUIRE(block != engine::ecs::NULL_ENTITY);
		const engine::scene::Bounds *bounds = store.Get<engine::scene::Bounds>(block);
		REQUIRE(bounds != nullptr);
		CHECK(bounds->HalfExtent == engine::core::Vector3{4.0f, 1.5f, 1.0f});
		const engine::scene::Visual *visual = store.Get<engine::scene::Visual>(block);
		REQUIRE(visual != nullptr);
		CHECK(visual->Tint == engine::core::Color3{1.0f, 0.0f, 0.0f});
	});
}
