#include "../src/SourceSdf.hpp"

#include "../src/ImageGraphTransform3DResident.hpp"
#include "../src/SourceUniformBlocks.hpp"

#include <engine/render/SourceSdf.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>
TEST_SUITE_ID("engine.render.sourcesdf")
namespace {
	engine::render::imagegraph::SourceSdfRequest Request() {
		engine::render::imagegraph::SourceSdfRequest request;
		request.Width = request.Height = 8;
		request.Object.Data.emplace().Shapes.emplace_back();
		request.Object.Data->Shapes[0].Shape = 200;
		request.Object.Data->Shapes[0].Identity = "sphere";
		request.Object.Data->Operations.push_back({0, 0});
		return request;
	}
}
TEST_CASE(
	"source SDF request validation preserves the authored atlas and rejects invalid controls",
	"[render][imagegraph][source_sdf]"
) {
	using namespace engine::render::imagegraph;
	auto request = Request();
	REQUIRE(ValidateSourceSdfRequest(request) == SourceSdfStatus::Ok);
	request.Object.Data->Shapes[0].Shape = 201;
	REQUIRE(ValidateSourceSdfRequest(request) == SourceSdfStatus::Ok);
	request.CameraScale = 0;
	CHECK(ValidateSourceSdfRequest(request) == SourceSdfStatus::InvalidControl);
	request = Request();
	request.ViewRange = {6, 3};
	CHECK(ValidateSourceSdfRequest(request) == SourceSdfStatus::InvalidControl);
	request = Request();
	request.CameraRotation[0] = std::numeric_limits<float>::infinity();
	CHECK(ValidateSourceSdfRequest(request) == SourceSdfStatus::InvalidControl);
	request = Request();
	request.TextureAtlasSize = 9000;
	CHECK(ValidateSourceSdfRequest(request) == SourceSdfStatus::OutputLimit);
}
TEST_CASE(
	"source SDF shares queue generation replacement and cancellation ownership",
	"[render][imagegraph][source_sdf]"
) {
	using namespace engine;
	using Result = render::imagegraph::TransformImage3DQueueResult;
	render::Renderer renderer;
	const core::Name owner("sdf-owner"), name("sdf-output");
	CHECK(renderer.QueueSourceSdf({owner, name, 1, Request()}) == Result::Queued);
	CHECK(renderer.QueueSourceSdf({owner, name, 1, Request()}) == Result::Duplicate);
	CHECK(renderer.QueueSourceSdf({owner, name, 2, Request()}) == Result::Replaced);
	CHECK_FALSE(renderer.CancelTransformImage3D(owner, name, 1));
	REQUIRE(renderer.CancelTransformImage3D(owner, name, 2));
	for (const auto &slot : render::test_support::TransformImage3DResidentTestAccess::Slots(renderer))
		CHECK(slot.Phase == render::test_support::TransformImage3DQueuePhase::Free);
	CHECK(render::test_support::TransformImage3DResidentTestAccess::SourceBytes(renderer) == 0);
}

TEST_CASE(
	"source raymarch modes validate their own payloads and account texture residency",
	"[render][imagegraph][source_sdf]"
) {
	using namespace engine::render::imagegraph;
	SourceSdfRequest request;
	request.Width = request.Height = 8;
	request.Mode = SourceRaymarchMode::Cloud;
	request.Cloud.Gradient.Keys = {{0, {0, 0, 0, 255}}, {1, {255, 255, 255, 255}}};
	REQUIRE(ValidateSourceSdfRequest(request) == SourceSdfStatus::Ok);
	request.Cloud.Gradient.Keys.resize(65);
	CHECK(ValidateSourceSdfRequest(request) == SourceSdfStatus::InvalidControl);
	request.Cloud.Gradient.Keys.resize(2);
	request.Cloud.Rotation[0] = std::numeric_limits<float>::quiet_NaN();
	CHECK(ValidateSourceSdfRequest(request) == SourceSdfStatus::InvalidControl);
	request = SourceSdfRequest{};
	request.Width = request.Height = 8;
	request.Mode = SourceRaymarchMode::Terrain;
	REQUIRE(ValidateSourceSdfRequest(request) == SourceSdfStatus::Ok);
	request.Terrain.AtlasUvScale = {1, 4192.f / 1024};
	REQUIRE(ValidateSourceSdfRequest(request) == SourceSdfStatus::Ok);
	auto &image = request.Terrain.Textures[0].emplace();
	image.Width = 4192;
	image.Height = 1024;
	image.Pixels.resize(size_t(image.Width) * image.Height * 4);
	REQUIRE(ValidateSourceSdfRequest(request) == SourceSdfStatus::Ok);
	CHECK(SourceSdfSourceBytes(request) >= image.Pixels.size());
	CHECK(SourceSdfScratchBytes(request) >= image.Pixels.size() * 2);
	request.Terrain.AtlasUvScale[1] = std::numeric_limits<float>::infinity();
	CHECK(ValidateSourceSdfRequest(request) == SourceSdfStatus::InvalidControl);
	request = Request();
	request.Mode = SourceRaymarchMode::Scatter;
	REQUIRE(ValidateSourceSdfRequest(request) == SourceSdfStatus::Ok);
	request.Scatter.Grid[0] = 0;
	CHECK(ValidateSourceSdfRequest(request) == SourceSdfStatus::InvalidControl);
}

TEST_CASE(
	"source SDF preview textures retain atlas coordinates within bounded cropped rows",
	"[render][imagegraph][source_sdf]"
) {
	using namespace engine::render::imagegraph;
	auto request = Request();
	request.ShapeTextureAtlasSize = 8192;
	auto &image = request.Object.Data->Shapes[0].Texture.emplace();
	image.Width = image.Height = 1;
	image.Pixels = {255, 255, 255, 255};
	REQUIRE(ValidateSourceSdfRequest(request) == SourceSdfStatus::Ok);
	CHECK(SourceSdfScratchBytes(request) < 128ull * 1024 * 1024);
	request.Mode = SourceRaymarchMode::Scatter;
	REQUIRE(ValidateSourceSdfRequest(request) == SourceSdfStatus::Ok);
}

TEST_CASE("source camera and SDF uniforms fit every backend slot", "[render][imagegraph][source_sdf]") {
	using namespace engine::render::imagegraph;
	CHECK(SourceUniformBlocksFit(CAMERA_UNIFORM_CUTS, 6672));
	CHECK(SourceUniformBlocksFit(SDF_UNIFORM_CUTS, 10960));
	CHECK(SourceUniformBlocksFit(SDF_UNIFORM_CUTS, 11104));
	CHECK_FALSE(SourceUniformBlocksFit(std::array<size_t, 1>{0}, 6672));
	CHECK_FALSE(SourceUniformBlocksFit(SDF_UNIFORM_CUTS, SDF_UNIFORM_CUTS.back() + 4097));
}
