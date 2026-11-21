// Roblox place analysis without an ImGui window.

#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/effects/Registration.hpp>
#include <engine/game/Game.hpp>
#include <engine/gui/Components.hpp>
#include <engine/scene/EditableImage.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Tagging.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/spatial/CollisionGroups.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <studio/Config.hpp>
#include <studio/RobloxImport.hpp>
#include <studio/RojoSync.hpp>
#include <tuple>

TEST_SUITE_ID("studio.robloximport")
TEST_DEPENDS("engine.bake.robloxmodel")
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
	CHECK(analysis.MissingClasses.empty());
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
	CHECK(report.FolderFallbackClasses.empty());
}

TEST_CASE(
	"roblox substitutions preserve spatial part values and report approximations", "[studio][robloximport]"
) {
	engine::scene::EnsureClassTree();
	engine::bake::RobloxModel model;
	for (const std::string_view sourceClass :
		 {"Seat", "VehicleSeat", "TrussPart", "WedgePart", "CornerWedgePart", "UnionOperation"}) {
		engine::bake::RobloxInstance source;
		source.ClassName = std::string(sourceClass);
		source.Name = std::string(sourceClass);
		engine::bake::RobloxValue size;
		size.Set(engine::core::Vector3{0.125f, 3.25f, 0.03125f});
		source.Properties.push_back({"Size", std::move(size)});
		engine::bake::RobloxValue frame;
		frame.Set(engine::core::CFrame(engine::core::Vector3{4.0f, -2.0f, 0.5f}));
		source.Properties.push_back({"CFrame", std::move(frame)});
		engine::bake::RobloxValue color;
		color.Set(engine::core::Color3{0.25f, 0.5f, 0.75f});
		source.Properties.push_back({"Color", std::move(color)});
		model.Roots.push_back(std::move(source));
	}
	engine::bake::RobloxInstance container;
	container.ClassName = "Folder";
	container.Name = "Container";
	engine::bake::RobloxInstance hat;
	hat.ClassName = "Hat";
	hat.Name = "Hat";
	container.Children.push_back(std::move(hat));
	model.Roots.push_back(std::move(container));

	const studio::RobloxImportAnalysis analysis = studio::AnalyzeRobloxImport(model);
	CHECK(analysis.MissingClasses.empty());
	REQUIRE(analysis.Substitutions.size() == 7);

	engine::ecs::Store store("roblox-substitutions");
	studio::RobloxImportResult report;
	std::string error;
	REQUIRE(studio::ImportRobloxPlace(store, model, {}, {}, report, error));
	CHECK(report.Substitutions.size() == analysis.Substitutions.size());
	const engine::ecs::Entity containerEntity = store.FindFirstRoot("Container");
	REQUIRE(containerEntity != engine::ecs::NULL_ENTITY);
	const engine::ecs::Entity accessory = store.FindFirstChild(containerEntity, "Hat");
	REQUIRE(accessory != engine::ecs::NULL_ENTITY);
	CHECK(store.ClassOf(accessory) == engine::ecs::Classes::Find(engine::core::Name("Accessory")));
	for (const std::string_view sourceClass : {"Seat", "VehicleSeat", "TrussPart", "UnionOperation"}) {
		const engine::ecs::Entity instance = store.FindFirstRoot(sourceClass);
		REQUIRE(instance != engine::ecs::NULL_ENTITY);
		CHECK(store.ClassOf(instance) == engine::scene::PartClass());
		const engine::scene::Bounds *bounds = store.Get<engine::scene::Bounds>(instance);
		const engine::scene::Collider *collider = store.Get<engine::scene::Collider>(instance);
		REQUIRE(bounds != nullptr);
		REQUIRE(collider != nullptr);
		engine::core::Vector3 size;
		REQUIRE(store.GetProperty(instance, engine::core::Name("Size"), &size, sizeof(size)));
		CHECK(size == engine::core::Vector3{0.125f, 3.25f, 0.03125f});
		CHECK(bounds->HalfExtent == engine::core::Vector3{0.0625f, 1.625f, 0.015625f});
		CHECK(collider->Extent == bounds->HalfExtent);
		const engine::scene::Visual *visual = store.Get<engine::scene::Visual>(instance);
		REQUIRE(visual != nullptr);
		CHECK(visual->Tint == engine::core::Color3{0.25f, 0.5f, 0.75f});
		engine::core::CFrame frame;
		REQUIRE(store.GetProperty(instance, engine::core::Name("CFrame"), &frame, sizeof(frame)));
		CHECK(frame.Position == engine::core::Vector3{4.0f, -2.0f, 0.5f});
	}
}

TEST_CASE("roblox native and explicit class mappings take precedence", "[studio][robloximport]") {
	engine::scene::EnsureClassTree();
	engine::bake::RobloxModel model;
	engine::bake::RobloxInstance native;
	native.ClassName = "MeshPart";
	native.Name = "Native";
	model.Roots.push_back(std::move(native));
	engine::bake::RobloxInstance explicitMapping;
	explicitMapping.ClassName = "Seat";
	explicitMapping.Name = "Explicit";
	model.Roots.push_back(std::move(explicitMapping));
	const studio::RobloxClassMappings mappings{{"MeshPart", "Part"}, {"Seat", "MeshPart"}};
	const studio::RobloxImportAnalysis analysis = studio::AnalyzeRobloxImport(model, mappings);
	REQUIRE(analysis.Substitutions.empty());
	engine::ecs::Store store("roblox-class-precedence");
	studio::RobloxImportResult report;
	std::string error;
	REQUIRE(studio::ImportRobloxPlace(store, model, {}, mappings, report, error));
	CHECK(
		store.ClassOf(store.FindFirstRoot("Native")) ==
		engine::ecs::Classes::Find(engine::core::Name("MeshPart"))
	);
	CHECK(
		store.ClassOf(store.FindFirstRoot("Explicit")) ==
		engine::ecs::Classes::Find(engine::core::Name("MeshPart"))
	);
	CHECK(report.Substitutions.empty());
}

TEST_CASE(
	"roblox sky faces and surface appearances map to supported parent properties", "[studio][robloximport]"
) {
	engine::scene::EnsureClassTree();
	engine::bake::RobloxModel model;
	engine::bake::RobloxInstance sky;
	sky.ClassName = "Sky";
	sky.Name = "Sky";
	for (const auto &[source, target] : std::array<std::pair<std::string_view, std::string_view>, 6>{{
			 {"SkyboxFt", "front.atex"},
			 {"SkyboxBk", "back.atex"},
			 {"SkyboxLf", "left.atex"},
			 {"SkyboxRt", "right.atex"},
			 {"SkyboxUp", "up.atex"},
			 {"SkyboxDn", "down.atex"},
		 }}) {
		engine::bake::RobloxValue value;
		value.Set(std::string(target));
		sky.Properties.push_back({std::string(source), std::move(value)});
	}
	model.Roots.push_back(std::move(sky));

	engine::bake::RobloxInstance mesh;
	mesh.ClassName = "MeshPart";
	mesh.Name = "Mesh";
	engine::bake::RobloxValue size;
	size.Set(engine::core::Vector3{0.125f, 3.25f, 0.03125f});
	mesh.Properties.push_back({"Size", std::move(size)});
	engine::bake::RobloxValue meshId;
	meshId.Set(std::string("mesh.amesh"));
	mesh.Properties.push_back({"MeshId", std::move(meshId)});
	engine::bake::RobloxInstance appearance;
	appearance.ClassName = "SurfaceAppearance";
	appearance.Name = "Appearance";
	for (const auto &[name, value] : std::array<std::pair<std::string_view, std::string_view>, 4>{{
			 {"ColorMap", "colour.atex"},
			 {"NormalMap", "normal.atex"},
			 {"RoughnessMap", "rough.atex"},
			 {"MetalnessMap", "metal.atex"},
		 }}) {
		engine::bake::RobloxValue property;
		property.Set(std::string(value));
		appearance.Properties.push_back({std::string(name), std::move(property)});
	}
	engine::bake::RobloxValue color;
	color.Set(engine::core::Color3{0.1f, 0.2f, 0.3f});
	appearance.Properties.push_back({"Color", std::move(color)});
	engine::bake::RobloxValue unsupported;
	unsupported.Set(std::string("ignored"));
	appearance.Properties.push_back({"Unsupported", std::move(unsupported)});
	mesh.Children.push_back(std::move(appearance));
	model.Roots.push_back(std::move(mesh));

	engine::ecs::Store store("roblox-sky-surface");
	studio::RobloxImportResult report;
	std::string error;
	REQUIRE(studio::ImportRobloxPlace(store, model, {}, {}, report, error));
	const engine::ecs::Entity skybox = store.FindFirstRoot("Sky");
	REQUIRE(skybox != engine::ecs::NULL_ENTITY);
	for (const auto &[name, expected] : std::array<std::pair<std::string_view, std::string_view>, 6>{{
			 {"Front", "front.atex"},
			 {"Back", "back.atex"},
			 {"Left", "left.atex"},
			 {"Right", "right.atex"},
			 {"Up", "up.atex"},
			 {"Down", "down.atex"},
		 }}) {
		engine::core::Name actual;
		REQUIRE(store.GetProperty(skybox, engine::core::Name(std::string(name)), &actual, sizeof(actual)));
		CHECK(actual == engine::core::Name(std::string(expected)));
	}
	const engine::ecs::Entity meshPart = store.FindFirstRoot("Mesh");
	REQUIRE(meshPart != engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(meshPart, "Appearance") == engine::ecs::NULL_ENTITY);
	const engine::scene::Bounds *bounds = store.Get<engine::scene::Bounds>(meshPart);
	const engine::scene::Collider *collider = store.Get<engine::scene::Collider>(meshPart);
	REQUIRE(bounds != nullptr);
	REQUIRE(collider != nullptr);
	engine::core::Vector3 fullSize;
	REQUIRE(store.GetProperty(meshPart, engine::core::Name("Size"), &fullSize, sizeof(fullSize)));
	CHECK(fullSize == engine::core::Vector3{0.125f, 3.25f, 0.03125f});
	CHECK(bounds->HalfExtent == engine::core::Vector3{0.0625f, 1.625f, 0.015625f});
	CHECK(collider->Extent == bounds->HalfExtent);
	const auto readName = [&](std::string_view name) {
		engine::core::Name value;
		REQUIRE(store.GetProperty(meshPart, engine::core::Name(std::string(name)), &value, sizeof(value)));
		return value;
	};
	CHECK(readName("TextureID") == engine::core::Name("colour.atex"));
	CHECK(readName("NormalMap") == engine::core::Name("normal.atex"));
	CHECK(readName("RoughnessMap") == engine::core::Name("rough.atex"));
	CHECK(readName("MetalnessMap") == engine::core::Name("metal.atex"));
	engine::core::Color3 surface;
	REQUIRE(store.GetProperty(meshPart, engine::core::Name("SurfaceColor"), &surface, sizeof(surface)));
	CHECK(surface == engine::core::Color3{0.1f, 0.2f, 0.3f});
	CHECK(report.Instances == 3);
	REQUIRE(report.SkippedProperties.size() == 1);
	CHECK(report.SkippedProperties[0].ClassName == "SurfaceAppearance");
	CHECK(report.SkippedProperties[0].PropertyName == "Unsupported");
	REQUIRE(report.Substitutions.size() == 2);
	CHECK(report.Substitutions[1].SourceClass == "SurfaceAppearance");
}

TEST_CASE("roblox UI endpoints import without a folder fallback", "[studio][robloximport]") {
	engine::bake::RobloxModel model;
	for (const std::string_view className :
		 {"RemoteEvent", "BindableEvent", "SoundGroup", "GuiService", "ChangeHistoryService"}) {
		engine::bake::RobloxInstance instance;
		instance.ClassName = std::string(className);
		instance.Name = std::string(className);
		model.Roots.push_back(std::move(instance));
	}

	engine::game::RegisterGameClasses();
	const studio::RobloxImportAnalysis analysis = studio::AnalyzeRobloxImport(model);
	CHECK(analysis.MissingClasses.empty());

	engine::ecs::Store store("roblox-import-ui-endpoints");
	studio::RobloxImportResult report;
	std::string error;
	REQUIRE(
		studio::ImportRobloxPlace(
			store, model, studio::RobloxAssetMappings{}, studio::RobloxClassMappings{}, report, error
		)
	);
	CHECK(error.empty());
	CHECK(report.FolderFallbackClasses.empty());
	CHECK(report.ReusedRoots == 2);
}

TEST_CASE("roblox import groups fallback classes and skipped properties", "[studio][robloximport]") {
	engine::scene::EnsureClassTree();

	engine::bake::RobloxModel model;
	engine::bake::RobloxInstance root;
	root.ClassName = "Folder";
	root.Name = "Root";

	engine::bake::RobloxValue sourceText;
	sourceText.Set(std::string("unsupported"));
	engine::bake::RobloxInstance second;
	second.ClassName = "UnmappedContainer";
	second.Name = "Second";
	second.Properties.push_back({"RobloxOnly", sourceText});
	root.Children.push_back(std::move(second));

	engine::bake::RobloxInstance first;
	first.ClassName = "UnmappedContainer";
	first.Name = "First";
	first.Properties.push_back({"RobloxOnly", sourceText});
	root.Children.push_back(std::move(first));

	engine::bake::RobloxInstance mapped;
	mapped.ClassName = "MappedPart";
	mapped.Name = "Mapped";
	mapped.Properties.push_back({"Size", sourceText});
	root.Children.push_back(std::move(mapped));
	model.Roots.push_back(std::move(root));

	engine::ecs::Store store("roblox-import-report");
	studio::RobloxImportResult report;
	std::string error;
	REQUIRE(
		studio::ImportRobloxPlace(
			store,
			model,
			studio::RobloxAssetMappings{},
			studio::RobloxClassMappings{{"MappedPart", "Part"}},
			report,
			error
		)
	);
	CHECK(error.empty());
	REQUIRE(report.FolderFallbackClasses.size() == 1);
	CHECK(report.FolderFallbackClasses[0].ClassName == "UnmappedContainer");
	CHECK(report.FolderFallbackClasses[0].Instances == 2);
	REQUIRE(report.SkippedProperties.size() == 2);
	CHECK(report.SkippedProperties[0].ClassName == "MappedPart");
	CHECK(report.SkippedProperties[0].PropertyName == "Size");
	CHECK(report.SkippedProperties[0].Reason == "source value type is incompatible");
	CHECK(report.SkippedProperties[0].Occurrences == 1);
	CHECK(report.SkippedProperties[1].ClassName == "UnmappedContainer");
	CHECK(report.SkippedProperties[1].PropertyName == "RobloxOnly");
	CHECK(report.SkippedProperties[1].Reason == "no matching engine property");
	CHECK(report.SkippedProperties[1].Occurrences == 2);
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
	engine::bake::RobloxValue transparency;
	transparency.Set(0.375f);
	part.Properties.push_back({"Transparency", transparency});
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
	// while `Color` and `Transparency` populate the part visual.
	const engine::ecs::Entity floor = store.FindFirstChild(workspace, "Floor");
	REQUIRE(floor != engine::ecs::NULL_ENTITY);
	const engine::scene::Bounds *bounds = store.Get<engine::scene::Bounds>(floor);
	REQUIRE(bounds != nullptr);
	CHECK(bounds->HalfExtent == engine::core::Vector3{10.0f, 0.5f, 10.0f});
	const engine::scene::Visual *visual = store.Get<engine::scene::Visual>(floor);
	REQUIRE(visual != nullptr);
	CHECK(visual->Tint == engine::core::Color3{0.2f, 0.4f, 0.8f});
	CHECK(visual->Transparency == 0.375f);
}

TEST_CASE("a roblox place port writes and reloads its world", "[studio][robloximport]") {
	ScratchConfig scratch;
	CollisionGroupScope collisionGroups;
	std::filesystem::create_directories(scratch.Root);
	const std::filesystem::path source = scratch.Root / "source.rbxlx";
	const std::filesystem::path destination = scratch.Root / "Ported.aworld";
	std::ofstream(source) << R"xml(
		<roblox version="4">
			<Item class="Part" referent="RBX0">
				<Properties>
					<string name="Name">Block</string>
					<Vector3 name="size"><X>0.125</X><Y>3.25</Y><Z>0.03125</Z></Vector3>
					<Color3uint8 name="Color">4294901760</Color3uint8>
					<float name="Transparency">0.375</float>
					<string name="CollisionGroup">Ground</string>
				</Properties>
			</Item>
			<Item class="Part" referent="RBX6">
				<Properties>
					<string name="Name">TintedBlock</string>
					<Vector3 name="Size"><X>3.5</X><Y>7</Y><Z>1.25</Z></Vector3>
					<Color3 name="Color"><R>0.25</R><G>0.5</G><B>0.75</B></Color3>
				</Properties>
			</Item>
			<Item class="RemoteEvent" referent="RBX1">
				<Properties><string name="Name">InterfaceRemote</string></Properties>
			</Item>
			<Item class="BindableEvent" referent="RBX2">
				<Properties><string name="Name">InterfaceBindable</string></Properties>
			</Item>
			<Item class="SoundGroup" referent="RBX3">
				<Properties><string name="Name">InterfaceAudio</string></Properties>
			</Item>
			<Item class="GuiService" referent="RBX4">
				<Properties><string name="Name">GuiService</string></Properties>
			</Item>
			<Item class="ChangeHistoryService" referent="RBX5">
				<Properties><string name="Name">ChangeHistoryService</string></Properties>
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
	CHECK(report.Analysis.Instances == 7);
	CHECK(report.Import.Instances == 7);
	CHECK(std::filesystem::file_size(destination) > 0);

	// PortRobloxPlace has already registered Ground while building the source
	// world. A fresh Studio process has not, which is the load path the emitted
	// .aworld must support on its own.
	engine::spatial::CollisionGroups::Reset();
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
		CHECK(bounds->HalfExtent == engine::core::Vector3{0.0625f, 1.625f, 0.015625f});
		const engine::scene::Collider *collider = store.Get<engine::scene::Collider>(block);
		REQUIRE(collider != nullptr);
		CHECK(collider->Extent == bounds->HalfExtent);
		const engine::scene::Visual *visual = store.Get<engine::scene::Visual>(block);
		REQUIRE(visual != nullptr);
		CHECK(visual->Tint == engine::core::Color3{1.0f, 0.0f, 0.0f});
		CHECK(visual->Transparency == 0.375f);
		const uint32_t group = engine::spatial::CollisionGroups::IndexOf(engine::core::Name("Ground"));
		REQUIRE(group != engine::spatial::NO_GROUP);
		CHECK(collider->Layer.Bits == engine::spatial::LayerMask::Only(group).Bits);
		CHECK(collider->Mask.Bits == engine::spatial::CollisionGroups::MaskFor(group).Bits);

		const engine::ecs::Entity tinted = store.FindFirstRoot("TintedBlock");
		REQUIRE(tinted != engine::ecs::NULL_ENTITY);
		const engine::scene::Bounds *tintedBounds = store.Get<engine::scene::Bounds>(tinted);
		const engine::scene::Visual *tintedVisual = store.Get<engine::scene::Visual>(tinted);
		REQUIRE(tintedBounds != nullptr);
		REQUIRE(tintedVisual != nullptr);
		CHECK(tintedBounds->HalfExtent == engine::core::Vector3{1.75f, 3.5f, 0.625f});
		CHECK(tintedVisual->Tint == engine::core::Color3{0.25f, 0.5f, 0.75f});

		for (const std::string_view name :
			 {"InterfaceRemote",
			  "InterfaceBindable",
			  "InterfaceAudio",
			  "GuiService",
			  "ChangeHistoryService"}) {
			const engine::ecs::Entity instance = store.FindFirstRoot(name);
			REQUIRE(instance != engine::ecs::NULL_ENTITY);
		}
		CHECK(
			store.ClassOf(store.FindFirstRoot("InterfaceRemote")) ==
			engine::ecs::Classes::Find(engine::core::Name("RemoteEvent"))
		);
		CHECK(
			store.ClassOf(store.FindFirstRoot("InterfaceBindable")) ==
			engine::ecs::Classes::Find(engine::core::Name("BindableEvent"))
		);
		CHECK(
			store.ClassOf(store.FindFirstRoot("InterfaceAudio")) ==
			engine::ecs::Classes::Find(engine::core::Name("SoundGroup"))
		);
	});
}

TEST_CASE("roblox import preserves integers and rejects unrepresentable numbers", "[studio][robloximport]") {
	engine::scene::EnsureClassTree();
	engine::ecs::Store store("roblox-numbers");
	engine::bake::RobloxModel model;
	const auto add = [&](std::string name, auto number) {
		engine::bake::RobloxInstance node;
		node.ClassName = "IntValue";
		node.Name = std::move(name);
		engine::bake::RobloxValue value;
		value.Set(number);
		node.Properties.push_back({"Value", value});
		model.Roots.push_back(std::move(node));
	};
	add("Precise", int64_t{9007199254740993});
	add("Maximum", std::numeric_limits<int64_t>::max());
	add("Minimum", std::numeric_limits<int64_t>::min());
	add("Overflow", 9223372036854775808.0);
	add("Fraction", 1.5);
	add("NotFinite", std::numeric_limits<double>::infinity());
	studio::RobloxImportResult report;
	std::string error;
	REQUIRE(studio::ImportRobloxPlace(store, model, {}, {}, report, error));
	CHECK(report.Properties == 3);
	const auto integer = [&](const char *name) {
		int64_t value = 0;
		REQUIRE(
			store.GetProperty(store.FindFirstRoot(name), engine::core::Name("Value"), &value, sizeof(value))
		);
		return value;
	};
	CHECK(integer("Precise") == 9007199254740993);
	CHECK(integer("Maximum") == std::numeric_limits<int64_t>::max());
	CHECK(integer("Minimum") == std::numeric_limits<int64_t>::min());
	CHECK(integer("Overflow") == 0);
	CHECK(integer("Fraction") == 0);
	CHECK(integer("NotFinite") == 0);
}

TEST_CASE("roblox particle sequences reach their authored properties", "[studio][robloximport]") {
	const std::string xml =
		R"xml(<roblox version="4"><Item class="ParticleEmitter" referent="RBX0"><Properties>
		<string name="Name">Emitter</string>
		<NumberSequence name="Size">0 2 0.5 0.5 4 1 1 0 0</NumberSequence>
		<ColorSequence name="Color">0 1 0 0 0 1 0 0 1 0</ColorSequence>
	</Properties></Item></roblox>)xml";
	engine::bake::RobloxModel model;
	std::string error;
	REQUIRE(engine::bake::ReadRobloxFile(std::as_bytes(std::span(xml.data(), xml.size())), model, error));
	REQUIRE(model.LostProperties.empty());
	engine::ecs::Store store("roblox-sequences");
	studio::RobloxImportResult report;
	REQUIRE(studio::ImportRobloxPlace(store, model, {}, {}, report, error));
	CHECK(report.Properties == 2);
	const auto emitter = store.FindFirstRoot("Emitter");
	engine::core::NumberSequence size;
	REQUIRE(store.GetProperty(emitter, engine::core::Name("Size"), &size, sizeof(size)));
	REQUIRE(size.Count == 3);
	CHECK(size.Keypoints[1] == engine::core::NumberKeypoint{0.5f, 4.0f, 1.0f});
	engine::core::ColorSequence colour;
	REQUIRE(store.GetProperty(emitter, engine::core::Name("Color"), &colour, sizeof(colour)));
	REQUIRE(colour.Count == 2);
	CHECK(colour.Keypoints[1] == engine::core::ColorKeypoint{1.0f, {0.0f, 0.0f, 1.0f}});
}

TEST_CASE(
	"roblox import refuses invalid sequences without changing their authored order", "[studio][robloximport]"
) {
	engine::bake::RobloxModel model;
	engine::bake::RobloxInstance emitter;
	emitter.Name = "Emitter";
	emitter.ClassName = "ParticleEmitter";
	engine::bake::RobloxValue size;
	size.Set(engine::bake::RobloxNumberSequence{{0, 1}, {0.75f, 2}, {0.5f, 3}, {1, 4}});
	emitter.Properties.push_back({"Size", size});
	model.Roots.push_back(std::move(emitter));
	engine::ecs::Store store("roblox-invalid-sequences");
	studio::RobloxImportResult report;
	std::string error;
	REQUIRE(studio::ImportRobloxPlace(store, model, {}, {}, report, error));
	CHECK(report.Properties == 0);
	REQUIRE(report.SkippedProperties.size() == 1);
	CHECK(report.SkippedProperties[0].PropertyName == "Size");
}

namespace {
	engine::bake::RobloxInstance PartNode(std::string className, std::string name) {
		engine::bake::RobloxInstance part;
		part.ClassName = std::move(className);
		part.Name = std::move(name);
		return part;
	}

	template <typename T> void AddValue(engine::bake::RobloxInstance &node, std::string name, T value) {
		engine::bake::RobloxValue held;
		held.Set(std::move(value));
		node.Properties.push_back({std::move(name), std::move(held)});
	}

	// Three interleaved big-endian int32 chunk deltas, as SmoothGrid stores them.
	void ChunkPosition(std::string &grid, int32_t x, int32_t y, int32_t z) {
		const std::array<uint32_t, 3> axes{
			static_cast<uint32_t>(x), static_cast<uint32_t>(y), static_cast<uint32_t>(z)
		};
		for (int shift = 24; shift >= 0; shift -= 8) {
			for (const uint32_t axis : axes) {
				grid.push_back(static_cast<char>((axis >> shift) & 0xFF));
			}
		}
	}

	// Fills the rest of a chunk with air in runs of 256.
	void AirRuns(std::string &grid, size_t voxels) {
		while (voxels > 0) {
			const size_t run = std::min<size_t>(voxels, 256);
			grid.push_back(static_cast<char>(0x80));
			grid.push_back(static_cast<char>(run - 1));
			voxels -= run;
		}
	}
}

TEST_CASE(
	"roblox smooth terrain decodes chunk deltas runs and occupancy", "[studio][robloximport][terrain]"
) {
	std::string grid{"\x01\x05", 2};
	ChunkPosition(grid, 1, 0, -1);
	grid.push_back(static_cast<char>(0x82)); // grass, full, run follows
	grid.push_back(3);						 // four voxels along x
	grid.push_back(static_cast<char>(0x42)); // grass, occupancy follows
	grid.push_back(10);						 // mostly empty, so air
	AirRuns(grid, 32768 - 5);

	studio::RobloxTerrainMesh mesh;
	std::string error;
	REQUIRE(studio::BuildRobloxTerrainMesh(grid, mesh, error));
	CHECK(mesh.Chunks == 1);
	CHECK(mesh.SolidVoxels == 4);
	// One bar of four voxels: six faces, each a merged rectangle.
	CHECK(mesh.Solid.Indices.size() == 6 * 6);
	CHECK(mesh.Solid.Positions.size() == 6 * 4);
	CHECK(mesh.Water.Indices.empty());
	CHECK(mesh.Solid.Minimum == engine::core::Vector3{128.0f, 0.0f, -128.0f});
	CHECK(mesh.Solid.Maximum == engine::core::Vector3{144.0f, 4.0f, -124.0f});
	CHECK(studio::RobloxTerrainMaterialName(mesh.Solid.Materials.front()) == "Grass");

	std::string overflow{"\x01\x05", 2};
	ChunkPosition(overflow, 0, 0, 0);
	AirRuns(overflow, 32768 - 1);
	overflow.push_back(static_cast<char>(0x82));
	overflow.push_back(5);
	studio::RobloxTerrainMesh untouched;
	untouched.Chunks = 7;
	CHECK_FALSE(studio::BuildRobloxTerrainMesh(overflow, untouched, error));
	CHECK(untouched.Chunks == 7);
	CHECK_FALSE(error.empty());
}

TEST_CASE("roblox smooth terrain reads the zero count escape", "[studio][robloximport][terrain]") {
	// Newer places write a count byte of 0 and one more byte; the run is one voxel.
	std::string grid{"\x01\x05", 2};
	ChunkPosition(grid, 0, 0, 0);
	grid.push_back(static_cast<char>(0xC2)); // grass, occupancy and count follow
	grid.push_back(static_cast<char>(0xF4)); // solid
	grid.push_back(0);						 // escape
	grid.push_back(static_cast<char>(0xFF)); // skipped
	AirRuns(grid, 32768 - 1);

	studio::RobloxTerrainMesh mesh;
	std::string error;
	REQUIRE(studio::BuildRobloxTerrainMesh(grid, mesh, error));
	CHECK(mesh.SolidVoxels == 1);
	CHECK(mesh.Solid.Indices.size() == 6 * 6);
	CHECK(mesh.Solid.Maximum == engine::core::Vector3{4.0f, 4.0f, 4.0f});
}

TEST_CASE(
	"roblox terrain hides faces between neighbours, across chunks too", "[studio][robloximport][terrain]"
) {
	// Voxel x = 31 of chunk 0 and x = 0 of chunk 1 touch across the chunk edge;
	// the face between them must not be drawn. A water voxel beside the second
	// shows the solid face toward it, and hides its own face toward the solid.
	std::string grid{"\x01\x05", 2};
	ChunkPosition(grid, 0, 0, 0);
	AirRuns(grid, 31);
	grid.push_back(static_cast<char>(0x02)); // grass at x = 31
	AirRuns(grid, 32768 - 32);
	ChunkPosition(grid, 1, 0, 0);
	grid.push_back(static_cast<char>(0x08)); // rock at x = 0
	grid.push_back(static_cast<char>(0x01)); // water at x = 1
	AirRuns(grid, 32768 - 2);

	studio::RobloxTerrainMesh mesh;
	std::string error;
	REQUIRE(studio::BuildRobloxTerrainMesh(grid, mesh, error));
	CHECK(mesh.Chunks == 2);
	// Two solid voxels, five faces each: the shared face is hidden on both sides.
	CHECK(mesh.Solid.Indices.size() / 6 == 10);
	// The water voxel shows five faces: not the one against the rock.
	CHECK(mesh.Water.Indices.size() / 6 == 5);
	// Every face points away from its voxel: no normal crosses x = 128 inward.
	for (size_t vertex = 0; vertex < mesh.Solid.Positions.size(); ++vertex)
		if (mesh.Solid.Positions[vertex].X == 128.0f) CHECK(mesh.Solid.Normals[vertex].X == 0.0f);
}

TEST_CASE(
	"roblox terrain becomes one mesh part beside the existing terrain", "[studio][robloximport][terrain]"
) {
	std::string grid{"\x01\x05", 2};
	ChunkPosition(grid, 0, 0, 0);
	grid.push_back(static_cast<char>(0x81)); // water
	grid.push_back(1);
	AirRuns(grid, 32768 - 2);

	engine::bake::RobloxModel model;
	engine::bake::RobloxInstance workspace = PartNode("Workspace", "Workspace");
	engine::bake::RobloxInstance terrain = PartNode("Terrain", "Terrain");
	AddValue(terrain, "SmoothGrid", grid);
	AddValue(terrain, "WaterColor", engine::core::Color3{0.0f, 0.25f, 0.5f});
	workspace.Children.push_back(std::move(terrain));
	model.Roots.push_back(std::move(workspace));

	const studio::RobloxImportAnalysis analysis = studio::AnalyzeRobloxImport(model);
	CHECK(analysis.MissingProperties.empty());

	engine::ecs::Store store("roblox-terrain");
	studio::RobloxImportResult report;
	std::string error;
	REQUIRE(studio::ImportRobloxPlace(store, model, {}, {}, report, error));
	CHECK(report.TerrainChunks == 1);
	CHECK(report.TerrainTriangles == 0);
	CHECK(report.TerrainWaterTriangles == 12);

	const engine::ecs::Entity root = store.FindFirstRoot("Workspace");
	REQUIRE(root != engine::ecs::NULL_ENTITY);
	const engine::ecs::ClassId terrainClass = engine::ecs::Classes::Find(engine::core::Name("Terrain"));
	size_t terrains = 0;
	store.EachChild(root, [&](engine::ecs::Entity child) {
		terrains += store.ClassOf(child) == terrainClass ? 1 : 0;
	});
	CHECK(terrains == 1);

	const engine::ecs::Entity meshes = store.FindFirstChild(root, "TerrainMesh");
	REQUIRE(meshes != engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(meshes, "TerrainSurface") == engine::ecs::NULL_ENTITY);
	const engine::ecs::Entity water = store.FindFirstChild(meshes, "TerrainWater");
	REQUIRE(water != engine::ecs::NULL_ENTITY);
	engine::core::Vector3 size;
	REQUIRE(store.GetProperty(water, engine::core::Name("Size"), &size, sizeof(size)));
	CHECK(size == engine::core::Vector3{8.0f, 4.0f, 4.0f});
	bool collides = true;
	REQUIRE(store.GetProperty(water, engine::core::Name("CanCollide"), &collides, sizeof(collides)));
	CHECK_FALSE(collides);

	// The part draws the mesh beneath it.
	const engine::ecs::Entity editable = store.FindFirstChild(water, "TerrainWaterMesh");
	REQUIRE(editable != engine::ecs::NULL_ENTITY);
	const auto *held = store.Get<engine::scene::EditableMesh>(editable);
	REQUIRE(held != nullptr);
	CHECK(held->Indices.size() == 36);
	engine::core::Name meshId;
	REQUIRE(store.GetProperty(water, engine::core::Name("MeshId"), &meshId, sizeof(meshId)));
	CHECK(meshId == engine::scene::EditableMeshContentName(store, editable));

	// Coloured by the terrain's palette image, one block per material.
	const engine::ecs::Entity palette = store.FindFirstChild(meshes, "TerrainPalette");
	REQUIRE(palette != engine::ecs::NULL_ENTITY);
	engine::core::Name texture;
	REQUIRE(store.GetProperty(water, engine::core::Name("TextureID"), &texture, sizeof(texture)));
	CHECK(texture == engine::scene::EditableImageContentName(store, palette));
	const auto *image = store.Get<engine::scene::EditableImage>(palette);
	REQUIRE(image != nullptr);
	// The water slot holds the place's water colour: (0, 0.25, 0.5) linear.
	const size_t waterTexel = (static_cast<size_t>(image->Width) * 8 + 16 + 8) * 4;
	CHECK(image->Pixels[waterTexel] == 0);
	CHECK(image->Pixels[waterTexel + 2] > image->Pixels[waterTexel + 1]);
}

TEST_CASE("roblox part shapes become builtin meshes in the roblox frame", "[studio][robloximport]") {
	engine::bake::RobloxModel model;
	engine::bake::RobloxInstance ball = PartNode("Part", "Ball");
	AddValue(ball, "Shape", std::string("Ball"));
	model.Roots.push_back(std::move(ball));

	engine::bake::RobloxInstance corner = PartNode("CornerWedgePart", "Corner");
	AddValue(corner, "Size", engine::core::Vector3{1.0f, 2.0f, 3.0f});
	AddValue(corner, "CFrame", engine::core::CFrame(engine::core::Vector3{5.0f, 6.0f, 7.0f}));
	model.Roots.push_back(std::move(corner));

	engine::bake::RobloxInstance block = PartNode("Part", "Block");
	AddValue(block, "Shape", std::string("Block"));
	model.Roots.push_back(std::move(block));

	const studio::RobloxImportAnalysis analysis = studio::AnalyzeRobloxImport(model);
	CHECK(analysis.MissingClasses.empty());
	CHECK(analysis.MissingProperties.empty());

	engine::ecs::Store store("roblox-shapes");
	studio::RobloxImportResult report;
	std::string error;
	REQUIRE(studio::ImportRobloxPlace(store, model, {}, {}, report, error));
	CHECK(report.SkippedProperties.empty());

	const engine::ecs::ClassId meshPart = engine::ecs::Classes::Find(engine::core::Name("MeshPart"));
	const engine::ecs::Entity sphere = store.FindFirstRoot("Ball");
	REQUIRE(sphere != engine::ecs::NULL_ENTITY);
	CHECK(store.ClassOf(sphere) == meshPart);
	CHECK(store.ClassOf(store.FindFirstRoot("Block")) == engine::scene::PartClass());

	const engine::ecs::Entity wedge = store.FindFirstRoot("Corner");
	REQUIRE(wedge != engine::ecs::NULL_ENTITY);
	CHECK(store.ClassOf(wedge) == meshPart);
	engine::core::Vector3 size;
	REQUIRE(store.GetProperty(wedge, engine::core::Name("Size"), &size, sizeof(size)));
	CHECK(size == engine::core::Vector3{3.0f, 2.0f, 1.0f});
	engine::core::CFrame frame;
	REQUIRE(store.GetProperty(wedge, engine::core::Name("CFrame"), &frame, sizeof(frame)));
	CHECK(frame.Position == engine::core::Vector3{5.0f, 6.0f, 7.0f});
	// The builtin apex at (-x, -z) lands on Roblox's (+x, -z) corner.
	const engine::core::Vector3 apex = frame.PointToWorldSpace({-0.5f, 0.5f, -0.5f}) - frame.Position;
	CHECK(apex.X > 0.49f);
	CHECK(apex.Z < -0.49f);
}

TEST_CASE("roblox tags attributes and legacy joints import", "[studio][robloximport]") {
	engine::bake::RobloxModel model;
	engine::bake::RobloxInstance part = PartNode("Part", "Guard");
	part.Tags = {"Enemy"};
	engine::bake::RobloxValue speed;
	speed.Set(2.5);
	part.Attributes.push_back({"Speed", speed});
	part.Children.push_back(PartNode("Snap", "Snap"));
	model.Roots.push_back(std::move(part));

	const studio::RobloxImportAnalysis analysis = studio::AnalyzeRobloxImport(model);
	CHECK(analysis.MissingClasses.empty());

	engine::ecs::Store store("roblox-tags");
	studio::RobloxImportResult report;
	std::string error;
	REQUIRE(studio::ImportRobloxPlace(store, model, {}, {}, report, error));
	CHECK(report.Tags == 1);
	CHECK(report.Attributes == 1);
	const engine::ecs::Entity guard = store.FindFirstRoot("Guard");
	REQUIRE(guard != engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(guard, "Snap") == engine::ecs::NULL_ENTITY);
	CHECK(engine::scene::HasTag(store, guard, engine::core::Name("Enemy")));
	engine::ecs::AttributeValue value;
	REQUIRE(engine::ecs::GetAttribute(store, guard, engine::core::Name("Speed"), value));
	CHECK(value.Double == 2.5);
}

TEST_CASE("roblox references resolve to the instances they name", "[studio][robloximport]") {
	// Preorder: Boards 0, Board 1, Stray 2, Sign 3. The board's adornee comes
	// later in the tree, so it can only resolve once everything is built.
	engine::bake::RobloxModel model;
	engine::bake::RobloxInstance boards;
	boards.ClassName = "Folder";
	boards.Name = "Boards";
	const auto board = [](std::string name, uint32_t target) {
		engine::bake::RobloxInstance gui;
		gui.ClassName = "SurfaceGui";
		gui.Name = std::move(name);
		engine::bake::RobloxValue adornee;
		adornee.Set(engine::bake::RobloxReference{target});
		gui.Properties.push_back({"Adornee", adornee});
		return gui;
	};
	boards.Children.push_back(board("Board", 3));
	boards.Children.push_back(board("Stray", 99));
	model.Roots.push_back(std::move(boards));
	model.Roots.push_back(PartNode("Part", "Sign"));

	engine::game::RegisterGameClasses();
	engine::ecs::Store store("roblox-references");
	studio::RobloxImportResult report;
	std::string error;
	REQUIRE(studio::ImportRobloxPlace(store, model, {}, {}, report, error));

	const engine::ecs::Entity sign = store.FindFirstRoot("Sign");
	const engine::ecs::Entity folder = store.FindFirstRoot("Boards");
	REQUIRE(sign != engine::ecs::NULL_ENTITY);
	REQUIRE(folder != engine::ecs::NULL_ENTITY);
	const engine::gui::Surface *placed =
		store.Get<engine::gui::Surface>(store.FindFirstChild(folder, "Board"));
	REQUIRE(placed != nullptr);
	CHECK(placed->Adornee == sign);

	// A reference past the tree is reported, never aimed at something else.
	const engine::gui::Surface *stray =
		store.Get<engine::gui::Surface>(store.FindFirstChild(folder, "Stray"));
	REQUIRE(stray != nullptr);
	CHECK(stray->Adornee == engine::ecs::NULL_ENTITY);
	CHECK(std::any_of(report.SkippedProperties.begin(), report.SkippedProperties.end(), [](const auto &skip) {
		return skip.PropertyName == "Adornee";
	}));
}

TEST_CASE("roblox script lint names what the engine cannot provide", "[studio][robloximport]") {
	engine::bake::RobloxModel model;
	model.Scripts.push_back(
		{"ServerScriptService/Main",
		 "Script",
		 "local players = game:GetService(\"Players\")\n"
		 "local fake = game:GetService('NoSuchService') -- game:GetService(\"Hidden\")\n"
		 "local part = Instance.new(\"Part\")\n"
		 "local body = Instance.new(\"NoSuchClass\")\n"
		 "--[[ Instance.new(\"AlsoHidden\") ]]\n"
		 "local mode = Enum.NoSuchEnum.Value\n"
		 "local lib = require(12345)\n"
		 "local again = game:GetService('NoSuchService')\n"}
	);
	const std::vector<studio::RobloxScriptFinding> findings = studio::LintRobloxScripts(model);
	std::vector<std::tuple<std::string, std::string, size_t>> seen;
	for (const studio::RobloxScriptFinding &finding : findings) {
		seen.emplace_back(finding.Kind, finding.Name, finding.Line);
	}
	CHECK(
		seen == std::vector<std::tuple<std::string, std::string, size_t>>{
					{"service", "NoSuchService", 2},
					{"class", "NoSuchClass", 4},
					{"enum", "Enum.NoSuchEnum", 6},
					{"require", "12345", 7},
				}
	);
}

TEST_CASE("roblox special meshes and empty service roots import", "[studio][robloximport]") {
	engine::bake::RobloxModel model;
	engine::bake::RobloxInstance rock = PartNode("Part", "Rock");
	engine::bake::RobloxInstance mesh = PartNode("SpecialMesh", "Mesh");
	AddValue(mesh, "MeshType", std::string("FileMesh"));
	AddValue(mesh, "MeshId", std::string("rbxassetid://111"));
	AddValue(mesh, "TextureId", std::string("rbxassetid://222"));
	rock.Children.push_back(std::move(mesh));
	model.Roots.push_back(std::move(rock));
	model.Roots.push_back(PartNode("Selection", "Selection"));

	const studio::RobloxImportAnalysis analysis = studio::AnalyzeRobloxImport(model);
	CHECK(analysis.MissingClasses.empty());
	CHECK(analysis.MissingProperties.empty());

	engine::ecs::Store store("roblox-special-mesh");
	studio::RobloxImportResult report;
	std::string error;
	REQUIRE(studio::ImportRobloxPlace(store, model, {}, {}, report, error));
	CHECK(store.FindFirstRoot("Selection") == engine::ecs::NULL_ENTITY);
	const engine::ecs::Entity part = store.FindFirstRoot("Rock");
	REQUIRE(part != engine::ecs::NULL_ENTITY);
	CHECK(store.ClassOf(part) == engine::ecs::Classes::Find(engine::core::Name("MeshPart")));
	CHECK(store.FindFirstChild(part, "Mesh") == engine::ecs::NULL_ENTITY);
	engine::core::Name meshId;
	REQUIRE(store.GetProperty(part, engine::core::Name("MeshId"), &meshId, sizeof(meshId)));
	CHECK(meshId == engine::core::Name("rbxassetid://111"));
}

TEST_CASE("a rojo project synced through the roblox translator is linted", "[studio][robloximport][rojo]") {
	const std::filesystem::path root =
		std::filesystem::temp_directory_path() / "atomic-roblox-rojo-translator";
	std::filesystem::remove_all(root);
	std::filesystem::create_directories(root / "src");
	std::ofstream(
		root / "default.project.json"
	) << R"({"name":"Demo","tree":{"$className":"DataModel","ServerScriptService":{"$className":"ServerScriptService","$path":"src"}}})";
	std::ofstream(root / "src" / "Boot.server.luau") << "local fake = game:GetService(\"NoSuchService\")\n";

	engine::ecs::Store store("roblox-rojo-translator");
	studio::RobloxWorldPortResult result;
	std::string error;
	REQUIRE(studio::ImportRobloxRojoProject(store, root / "default.project.json", {}, result, error));
	CHECK(result.Import.Scripts == 1);
	const engine::ecs::Entity service = store.FindFirstRoot("ServerScriptService");
	REQUIRE(service != engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(service, "Boot") != engine::ecs::NULL_ENTITY);
	REQUIRE(result.Analysis.ScriptFindings.size() == 1);
	CHECK(result.Analysis.ScriptFindings[0].Name == "NoSuchService");
	CHECK(result.Analysis.ScriptFindings[0].Line == 1);
	std::filesystem::remove_all(root);
}
