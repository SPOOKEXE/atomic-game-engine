#include <engine/core/Paths.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/scene/Animation.hpp>
#include <engine/scene/EditableImage.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Skinning.hpp>
#include <engine/script/DataSceneService.hpp>
#include <engine/script/RigExport.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.examples.data_factory_acceptance")
TEST_DEPENDS("engine.scripthost.scripting")

using engine::core::CFrame;
using engine::core::Name;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::examples::ExamplePath;
using engine::examples::LoadScene;

namespace {
	struct StagedAssets {
		std::filesystem::path Previous = engine::core::Paths::Assets();

		StagedAssets() {
			engine::core::Paths::SetAssetsOverride(engine::core::Paths::Base().parent_path() / "assets");
		}

		~StagedAssets() {
			engine::core::Paths::SetAssetsOverride(Previous);
		}
	};

	Entity DemoChild(Store &store, std::string_view name) {
		const Entity workspace = engine::scene::WorkspaceOf(store);
		if (workspace != engine::ecs::NULL_ENTITY) {
			const Entity child = store.FindFirstChild(workspace, name);
			if (child != engine::ecs::NULL_ENTITY) return child;
		}
		return store.FindFirstRoot(name);
	}

	const engine::script::ScriptValue *
	Field(const engine::script::ScriptValue &value, std::string_view name) {
		if (value.Tag != engine::script::ValueTag::Map) return nullptr;
		for (const auto &[key, field] : value.Entries)
			if (key == name) return &field;
		return nullptr;
	}

	void PrepareDataFactoryWorld(Store &store) {
		engine::physics::PreparePhysicsWorld(store);
	}
}

TEST_CASE("data factory starter scene exposes three bounded spatial observations", "[data][acceptance]") {
	StagedAssets assets;
	Store store("data-factory-starter");
	engine::ecs::Scheduler scheduler;
	PrepareDataFactoryWorld(store);
	std::string error;
	REQUIRE(LoadScene(store, scheduler, ExamplePath("DataFactoryDemo.luau"), error));
	const Entity camera = DemoChild(store, "DataFactoryCamera");
	REQUIRE(camera != engine::ecs::NULL_ENTITY);
	const engine::script::DataSceneResult rendering =
		engine::script::GetCameraRenderingData(store, camera, 16);
	REQUIRE(rendering.Status == std::string_view("ok"));
	const auto *observations = Field(rendering.Value, "object_observations");
	REQUIRE(observations != nullptr);
	REQUIRE(observations->Items.size() == 3);
	CHECK(Field(observations->Items[0], "id")->Text == "data-factory-demo/behind");
	CHECK(Field(observations->Items[1], "id")->Text == "data-factory-demo/partly-offscreen");
	CHECK(Field(observations->Items[2], "id")->Text == "data-factory-demo/visible");
	const auto *occlusion = Field(observations->Items[2], "occlusion");
	REQUIRE(occlusion != nullptr);
	CHECK(Field(*occlusion, "available")->Boolean == false);
	CHECK(Field(*occlusion, "reason")->Text == "requires_capture_visibility_evidence");
}

TEST_CASE("data factory package fixture replaces only its owned workspace subtree", "[data][acceptance]") {
	StagedAssets assets;
	Store store("data-factory-package");
	engine::ecs::Scheduler scheduler;
	PrepareDataFactoryWorld(store);
	std::string error;
	REQUIRE(LoadScene(store, scheduler, ExamplePath("DataFactoryPackageDemo.luau"), error));
	const Entity workspace = engine::scene::WorkspaceOf(store);
	REQUIRE(workspace != engine::ecs::NULL_ENTITY);
	const Entity firstRoot = store.FindFirstChild(workspace, "DataFactoryPackageDemo");
	REQUIRE(firstRoot != engine::ecs::NULL_ENTITY);
	const Entity unrelated = store.CreateInstance(engine::scene::PartClass(), "UnrelatedWorldContent");
	REQUIRE(unrelated != engine::ecs::NULL_ENTITY);
	REQUIRE(store.SetParent(unrelated, workspace));
	REQUIRE(LoadScene(store, scheduler, ExamplePath("DataFactoryPackageDemo.luau"), error));
	const Entity root = store.FindFirstChild(workspace, "DataFactoryPackageDemo");
	REQUIRE(root != engine::ecs::NULL_ENTITY);
	CHECK(root != firstRoot);
	CHECK(store.FindFirstChild(workspace, "UnrelatedWorldContent") == unrelated);
	const Entity camera = store.FindFirstChild(root, "DataFactoryPackageCamera");
	REQUIRE(camera != engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(root, "VisibleBox") != engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(root, "OffscreenBox") != engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(root, "BehindBox") != engine::ecs::NULL_ENTITY);
	const engine::script::DataSceneResult snapshot = engine::script::GetSceneSnapshot(store);
	REQUIRE(snapshot.Status == std::string_view("ok"));
	const auto *entities = Field(snapshot.Value, "entities");
	REQUIRE(entities != nullptr);
	std::vector<std::string> ids;
	for (const auto &entity : entities->Items) {
		const auto *id = Field(entity, "id");
		REQUIRE(id != nullptr);
		ids.push_back(id->Text);
	}
	CHECK(
		ids == std::vector<std::string>{
				   "data-factory-package/behind",
				   "data-factory-package/camera",
				   "data-factory-package/light/bulb",
				   "data-factory-package/offscreen",
				   "data-factory-package/visible"
			   }
	);
}

TEST_CASE("data factory image labels stay aligned with identified snapshots", "[data][acceptance]") {
	StagedAssets assets;
	Store store("data-factory-alignment");
	engine::ecs::Scheduler scheduler;
	PrepareDataFactoryWorld(store);
	std::string error;
	REQUIRE(LoadScene(store, scheduler, ExamplePath("DataFactoryAdvancedDemo.luau"), error));
	engine::physics::SyncBroadphase(store);

	const Entity label = DemoChild(store, "DataFactoryLabel");
	REQUIRE(label != engine::ecs::NULL_ENTITY);
	engine::ecs::AttributeValue id;
	REQUIRE(engine::ecs::GetAttribute(store, label, Name("DataFactoryId"), id));
	REQUIRE(id.Type == engine::ecs::PropertyType::String);
	REQUIRE(id.String == "data-factory-demo/label");

	const std::vector<std::byte> pixels = engine::scene::EditableImageToBuffer(store, label);
	REQUIRE(pixels.size() == 256u * 256u * 4u);
	CHECK(static_cast<unsigned char>(pixels[0]) == 255);
	CHECK(static_cast<unsigned char>(pixels[1]) == 128);
	CHECK(static_cast<unsigned char>(pixels[2]) == 0);
	CHECK(static_cast<unsigned char>(pixels[3]) == 255);

	const engine::script::DataSceneResult snapshot = engine::script::GetSceneSnapshot(store);
	REQUIRE(snapshot.Status == std::string_view("ok"));
	const auto *entities = Field(snapshot.Value, "entities");
	REQUIRE(entities != nullptr);
	bool found = false;
	for (const auto &entity : entities->Items) {
		const auto *entityId = Field(entity, "id");
		if (entityId != nullptr && entityId->Text == id.String) found = true;
	}
	CHECK(found);

	const Entity rig = DemoChild(store, "DataFactoryRig");
	REQUIRE(rig != engine::ecs::NULL_ENTITY);
	const Entity keypoint = store.FindFirstChild(rig, "Nose");
	REQUIRE(keypoint != engine::ecs::NULL_ENTITY);
	const auto *point = store.Get<engine::scene::RigKeypoint>(keypoint);
	REQUIRE(point != nullptr);
	CHECK(point->Keypoint.Text() == "nose");
	CHECK(point->Joint == 0);
	const Entity clip = DemoChild(store, "DataFactoryWave");
	const Entity buffer = DemoChild(store, "DataFactoryWaveBuffer");
	REQUIRE(clip != engine::ecs::NULL_ENTITY);
	REQUIRE(buffer != engine::ecs::NULL_ENTITY);
	const auto *definition = store.Get<engine::scene::AnimationClip>(clip);
	const auto *baked = store.Get<engine::scene::AnimationBuffer>(buffer);
	REQUIRE(definition != nullptr);
	REQUIRE(baked != nullptr);
	CHECK(definition->Buffer == buffer);
	CHECK(baked->Data.size() == 84);
	const engine::script::RigExportResult rigExport =
		engine::script::GetRigExport(store, "data-factory-demo/rig-export", {"data-factory-demo/rig"});
	REQUIRE(rigExport.Status == std::string_view("ok"));
	const auto *rigEntities = Field(rigExport.Value, "entities");
	REQUIRE(rigEntities != nullptr);
	REQUIRE(rigEntities->Items.size() == 1);
	const auto *exportedClips = Field(rigEntities->Items[0], "clips");
	REQUIRE(exportedClips != nullptr);
	REQUIRE(exportedClips->Items.size() == 1);
	CHECK(Field(exportedClips->Items[0], "clip_id")->Text == "data-factory-demo/clip/wave");
	CHECK(Field(exportedClips->Items[0], "end_tick")->Number == 1);
	const auto *channels = Field(exportedClips->Items[0], "channels");
	REQUIRE(channels != nullptr);
	REQUIRE(channels->Items.size() == 2);
	const auto *keys = Field(channels->Items[1], "keys");
	REQUIRE(keys != nullptr);
	CHECK(keys->Items.size() == 2);

	// The script demonstrates only first-frame query envelopes. This host syncs
	// after scene construction, then the fixture proves the crate's stable-id
	// labels without relying on captured stdout.
	const engine::script::DataSceneResult raycast =
		engine::script::Raycast(store, {Vector3{-3.0f, 1.0f, 0.0f}, Vector3{1.0f, 0.0f, 0.0f}, 6.0f});
	REQUIRE(raycast.Status == std::string_view("ok"));
	const auto *raycastId = Field(raycast.Value, "id");
	REQUIRE(raycastId != nullptr);
	CHECK(raycastId->Text == "data-factory-demo/crate");

	const engine::script::DataSceneResult aabb =
		engine::script::OverlapAABB(store, {Vector3{-1.1f, -0.1f, -1.1f}, Vector3{1.1f, 2.1f, 1.1f}});
	REQUIRE(aabb.Status == std::string_view("ok"));
	const auto *aabbIds = Field(aabb.Value, "ids");
	REQUIRE(aabbIds != nullptr);
	CHECK(std::ranges::any_of(aabbIds->Items, [](const engine::script::ScriptValue &entry) {
		return entry.Text == "data-factory-demo/crate";
	}));

	const engine::script::DataSceneResult obb =
		engine::script::OverlapOBB(store, {CFrame{Vector3{0.0f, 1.0f, 0.0f}}, Vector3{1.1f, 1.1f, 1.1f}});
	REQUIRE(obb.Status == std::string_view("ok"));
	const auto *obbIds = Field(obb.Value, "ids");
	REQUIRE(obbIds != nullptr);
	CHECK(std::ranges::any_of(obbIds->Items, [](const engine::script::ScriptValue &entry) {
		return entry.Text == "data-factory-demo/crate";
	}));
}

TEST_CASE("data factory refuses malformed image data and duplicate identities", "[data][acceptance]") {
	StagedAssets assets;
	Store store("data-factory-invalid");
	engine::ecs::Scheduler scheduler;
	PrepareDataFactoryWorld(store);
	std::string error;
	REQUIRE(LoadScene(store, scheduler, ExamplePath("DataFactoryAdvancedDemo.luau"), error));

	const Entity label = DemoChild(store, "DataFactoryLabel");
	REQUIRE(label != engine::ecs::NULL_ENTITY);
	const std::vector<std::byte> before = engine::scene::EditableImageToBuffer(store, label);
	const std::vector<std::byte> malformed(before.size() - 1, std::byte{0});
	CHECK_FALSE(engine::scene::EditableImageFromBuffer(store, label, malformed));
	CHECK(engine::scene::EditableImageToBuffer(store, label) == before);

	const Entity crate = DemoChild(store, "DataFactoryCrate");
	engine::ecs::AttributeValue duplicate;
	duplicate.Type = engine::ecs::PropertyType::String;
	duplicate.String = "data-factory-demo/label";
	REQUIRE(engine::ecs::SetAttribute(store, crate, Name("DataFactoryId"), duplicate));
	const engine::script::DataSceneResult snapshot = engine::script::GetSceneSnapshot(store);
	CHECK(snapshot.Status == std::string_view("identity_conflict"));
}
