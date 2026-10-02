#include <engine/ecs/Store.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <client/ImageGraphRuntime.hpp>
#include <filesystem>
#include <fstream>

TEST_SUITE_ID("client.imagegraphruntime.transform")
TEST_DEPENDS("engine.imagegraph.document")
TEST_DEPENDS("engine.scene.imagegraphbinding")
TEST_DEPENDS("engine.render.liveimagepublisher")

namespace {
	using namespace engine::imagegraph;

	Node Solid() {
		return {
			"solid",
			"image.solid",
			"",
			{},
			{{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{}}}
		};
	}

	Node Transform(std::string id) {
		return {
			std::move(id),
			"image.transform_3d",
			"",
			{},
			{{"position", Vector3{}},
			 {"anchor", Vector3{}},
			 {"rotation", Quaternion{}},
			 {"scale", Vector3{1, 1, 1}},
			 {"texture_tiling", Vector2{1, 1}},
			 {"projection", EnumValue{1}},
			 {"fov", 45.0},
			 {"view_range", Vector2{.001, 10}},
			 {"depth_range", Vector2{0, 1}}}
		};
	}

	struct GraphFile {
		std::filesystem::path Assets =
			std::filesystem::temp_directory_path() / "atomic-imagegraph-live-transform";
		const engine::core::Name Graph{"live-transform-test"};
		GraphFile() {
			std::filesystem::remove_all(Assets);
			std::filesystem::create_directories(Assets / "imagegraphs");
			Document document;
			document.FormatVersion = 6;
			document.Nodes = {Solid(), Transform("left"), Transform("right")};
			document.Links = {{"solid", "image", "left", "surface"}, {"solid", "image", "right", "surface"}};
			document.Outputs = {{"left-output", "left", "rendered"}, {"right-output", "right", "rendered"}};
			std::ofstream file(client::ImageGraphDocumentPath(Assets, Graph));
			file << Write(document);
		}
		~GraphFile() {
			std::filesystem::remove_all(Assets);
		}
	};
}

TEST_CASE(
	"removing one live transform binding leaves its owner's other queued job intact", "[client][imagegraph]"
) {
	GraphFile file;
	engine::scene::RegisterSceneComponents();
	engine::ecs::Store store("imagegraph-transform-owner-test");
	const auto removedEntity = store.Create();
	const auto keptEntity = store.Create();
	const engine::core::Name owner("imagegraph-transform-shared-owner");
	const engine::core::Name removedTexture("imagegraph-transform-removed");
	const engine::core::Name keptTexture("imagegraph-transform-kept");
	engine::scene::ImageGraphBinding removedBinding;
	removedBinding.Graph = file.Graph;
	removedBinding.Output = engine::core::Name("left-output");
	removedBinding.Texture = removedTexture;
	engine::scene::ImageGraphBinding keptBinding;
	keptBinding.Graph = file.Graph;
	keptBinding.Output = engine::core::Name("right-output");
	keptBinding.Texture = keptTexture;
	REQUIRE(engine::scene::SetImageGraphBinding(store, removedEntity, removedBinding));
	REQUIRE(engine::scene::SetImageGraphBinding(store, keptEntity, keptBinding));

	engine::render::Renderer renderer;
	client::ImageGraphRuntime runtime;
	CHECK(runtime.Refresh(store, renderer, owner, file.Assets) == 2);
	CHECK(runtime.LastError().empty());
	store.Remove<engine::scene::ImageGraphBinding>(removedEntity);
	CHECK(runtime.Refresh(store, renderer, owner, file.Assets) == 0);
	CHECK_FALSE(renderer.CancelTransformImage3D(owner, removedTexture, 1));
	CHECK_FALSE(renderer.CancelTransformImage3D(owner, removedTexture, 2));
	const bool keptGenerationOne = renderer.CancelTransformImage3D(owner, keptTexture, 1);
	const bool keptGenerationTwo = renderer.CancelTransformImage3D(owner, keptTexture, 2);
	CHECK(keptGenerationOne != keptGenerationTwo);
	runtime.Clear(renderer);
}
