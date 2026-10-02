#include "../src/ImageGraphTransform3DResident.hpp"
#include "RenderFixture.hpp"

#include <engine/render/SourceSdf.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.render.sourcesdf_gpu")
TEST_DEPENDS("engine.render.fixtures")
TEST_DEPENDS("engine.render.sourcesdf")
TEST_CASE(
	"source SDF raymarch shader renders a sphere and publishes its real GPU texture",
	"[render][gpu][imagegraph]"
) {
	using namespace engine;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	render::imagegraph::SourceSdfRequest request;
	request.Width = request.Height = 8;
	request.CameraRotation = {0, 0, 0};
	request.UseLight = false;
	auto &data = request.Object.Data.emplace();
	data.Shapes.emplace_back();
	data.Shapes[0].Shape = 200;
	data.Shapes[0].Identity = "sphere";
	data.Shapes[0].Radius = 1;
	data.Shapes[0].Diffuse = {255, 0, 0, 255};
	data.Operations.push_back({0, 0});
	render::imagegraph::SourceSdfResult result;
	REQUIRE(
		render::imagegraph::ExecuteSourceSdf(fixture.Render, request, result) ==
		render::imagegraph::SourceSdfStatus::Ok
	);
	REQUIRE(result.Pixels.size() == 256);
	const size_t center = (4 * 8 + 4) * 4;
	CHECK(result.Pixels[center] == std::byte{255});
	CHECK(result.Pixels[center + 1] == std::byte{0});
	CHECK(result.Pixels[center + 3] == std::byte{255});
	const core::Name owner("sdf-gpu-owner"), name("sdf-gpu-output");
	REQUIRE(
		fixture.Render.QueueSourceSdf({owner, name, 1, request}) ==
		render::imagegraph::TransformImage3DQueueResult::Queued
	);
	REQUIRE(render::test_support::TransformImage3DResidentTestAccess::RecordAndSubmit(fixture.Render));
	REQUIRE(SDL_WaitForGPUIdle(static_cast<SDL_GPUDevice *>(fixture.Render.Backend().Device)));
	render::test_support::TransformImage3DResidentTestAccess::Poll(fixture.Render);
	REQUIRE(
		render::test_support::TransformImage3DResidentTestAccess::PublishedTexture(
			fixture.Render, owner, name
		)
	);
	CHECK(
		render::test_support::TransformImage3DResidentTestAccess::PublishedGeneration(
			fixture.Render, owner, name
		) == 1
	);
	// An opaque white texture retains the red material through the cropped source atlas.
	request.ShapeTextureAtlasSize = 8192;
	auto &texture = data.Shapes[0].Texture.emplace();
	texture.Width = texture.Height = 1;
	texture.Pixels = {255, 255, 255, 255};
	REQUIRE(
		render::imagegraph::ExecuteSourceSdf(fixture.Render, request, result) ==
		render::imagegraph::SourceSdfStatus::Ok
	);
	CHECK(result.Pixels[center] == std::byte{255});
	CHECK(result.Pixels[center + 3] == std::byte{255});
	request.Mode = render::imagegraph::SourceRaymarchMode::Scatter;
	request.Scatter.Grid = {1, 1};
	REQUIRE(
		render::imagegraph::ExecuteSourceSdf(fixture.Render, request, result) ==
		render::imagegraph::SourceSdfStatus::Ok
	);
	CHECK(result.Pixels[center] == std::byte{255});
	CHECK(result.Pixels[center + 3] == std::byte{255});
	request.Mode = render::imagegraph::SourceRaymarchMode::Render;
	data.Shapes[0].Position = {100, 0, 0};
	REQUIRE(
		render::imagegraph::ExecuteSourceSdf(fixture.Render, request, result) ==
		render::imagegraph::SourceSdfStatus::Ok
	);
	CHECK(result.Pixels[center + 3] == std::byte{0});
}

TEST_CASE("source cloud and terrain use their distinct shader controls", "[render][gpu][imagegraph]") {
	using namespace engine;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	render::imagegraph::SourceSdfRequest request;
	request.Width = request.Height = 8;
	request.Mode = render::imagegraph::SourceRaymarchMode::Cloud;
	// A constant black gradient removes the accumulated density color at every sample.
	request.Cloud.Gradient.Keys = {{0, {0, 0, 0, 255}}, {1, {0, 0, 0, 255}}};
	render::imagegraph::SourceSdfResult result;
	REQUIRE(
		render::imagegraph::ExecuteSourceSdf(fixture.Render, request, result) ==
		render::imagegraph::SourceSdfStatus::Ok
	);
	REQUIRE(result.Pixels.size() == 256);
	for (size_t p = 0; p < result.Pixels.size(); p += 4) {
		CHECK(result.Pixels[p] == std::byte{0});
		CHECK(result.Pixels[p + 1] == std::byte{0});
		CHECK(result.Pixels[p + 2] == std::byte{0});
		CHECK(result.Pixels[p + 3] == std::byte{255});
	}
	request.Mode = render::imagegraph::SourceRaymarchMode::Terrain;
	// The source hit predicate stays zero below the flat zero-height surface.
	request.Terrain.Position = {0, 100, 0};
	request.Terrain.Background = {1, 0, 0, 1};
	REQUIRE(
		render::imagegraph::ExecuteSourceSdf(fixture.Render, request, result) ==
		render::imagegraph::SourceSdfStatus::Ok
	);
	const size_t center = (4 * 8 + 4) * 4;
	CHECK(result.Pixels[center] == std::byte{255});
	CHECK(result.Pixels[center + 1] == std::byte{0});
	CHECK(result.Pixels[center + 3] == std::byte{255});
}
