#include <engine/core/Paths.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/scene/EditableImage.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/DataSceneService.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.examples.data_factory_acceptance")
TEST_DEPENDS("engine.scripthost.scripting")

using engine::core::Name;
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
}

TEST_CASE("data factory image labels stay aligned with identified snapshots", "[data][acceptance]") {
	StagedAssets assets;
	Store store("data-factory-alignment");
	engine::ecs::Scheduler scheduler;
	std::string error;
	REQUIRE(LoadScene(store, scheduler, ExamplePath("DataFactoryDemo.luau"), error));

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
}

TEST_CASE("data factory refuses malformed image data and duplicate identities", "[data][acceptance]") {
	StagedAssets assets;
	Store store("data-factory-invalid");
	engine::ecs::Scheduler scheduler;
	std::string error;
	REQUIRE(LoadScene(store, scheduler, ExamplePath("DataFactoryDemo.luau"), error));

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
