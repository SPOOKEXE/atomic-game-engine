#include <engine/render/ImageGraphTransform3D.hpp>
#include <engine/render/SourceSkyboxGroup.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.render.sourceskyboxgroup")
namespace {
	using namespace engine;
	using namespace engine::render::imagegraph;
	void Check(bool okay, const char *message) {
		INFO(message);
		REQUIRE(okay);
	}
	SourceSkyboxRequest Request() {
		SourceCamera3DRequest camera;
		camera.Width = camera.Height = 64;
		camera.Format = assets::TextureFormat::RGBA8;
		camera.View[0] = camera.View[5] = camera.View[10] = camera.View[15] = 1;
		camera.Projection = camera.View;
		SourceSdfRequest sdf;
		sdf.Width = sdf.Height = 64;
		sdf.Format = assets::TextureFormat::RGBA8;
		sdf.Object.Data.emplace().Shapes.emplace_back();
		sdf.Object.Data->Shapes.front().Shape = 200;
		sdf.Object.Data->Shapes.front().Identity = "box";
		sdf.Object.Data->Operations.push_back({0, 0});
		SourceSkyboxRequest request;
		request.Owner = core::Name("source.skybox.owner");
		request.StagingOwner = core::Name("source.skybox.private");
		for (size_t i = 0; i < 6; ++i) {
			request.Targets[i] = {core::Name("skybox.face." + std::to_string(i)), i + 1};
			if (i % 2)
				request.Faces[i] = sdf;
			else
				request.Faces[i] = camera;
		}
		return request;
	}
}
TEST_CASE(
	"source skybox admission and completion enforce the six-face contract",
	"[render][imagegraph][source_skybox]"
) {
	auto request = Request();
	const auto sdf = std::get<SourceSdfRequest>(request.Faces[1]);
	std::string error;
	REQUIRE(ValidateSourceSkybox(request, error) == SourceSkyboxStatus::Pending);
	auto duplicate = request;
	duplicate.Targets[5].Name = duplicate.Targets[0].Name;
	Check(
		ValidateSourceSkybox(duplicate, error) == SourceSkyboxStatus::Invalid,
		"duplicate names refused atomically"
	);
	auto owner = request;
	owner.StagingOwner = owner.Owner;
	Check(
		ValidateSourceSkybox(owner, error) == SourceSkyboxStatus::Invalid, "cannot stage into visible owner"
	);
	auto generation = request;
	generation.Targets[2].Generation = 0;
	Check(ValidateSourceSkybox(generation, error) == SourceSkyboxStatus::Invalid, "zero generation refused");
	auto numeric = request;
	std::get<SourceCamera3DRequest>(numeric.Faces[0]).Format = assets::TextureFormat::RGBA8_LINEAR;
	Check(
		ValidateSourceSkybox(numeric, error) == SourceSkyboxStatus::Invalid,
		"linear face refused for display skybox"
	);
	std::array<bool, 6> admitted{}, ready{};
	std::array<render::SourceTextureStatus, 6> observed{};
	Check(
		ObserveSourceSkybox(admitted, ready, observed) == SourceSkyboxStatus::Pending,
		"unadmitted group pending"
	);
	for (size_t i = 0; i < 4; ++i) {
		admitted[i] = true;
		observed[i] = render::SourceTextureStatus::Pending;
	}
	Check(
		ObserveSourceSkybox(admitted, ready, observed) == SourceSkyboxStatus::Pending,
		"four slots cannot publish partial group"
	);
	for (size_t i = 0; i < 4; ++i)
		observed[i] = render::SourceTextureStatus::Ready;
	Check(
		ObserveSourceSkybox(admitted, ready, observed) == SourceSkyboxStatus::Pending,
		"four completions remain private"
	);
	for (size_t i = 4; i < 6; ++i) {
		admitted[i] = true;
		observed[i] = render::SourceTextureStatus::Pending;
	}
	Check(
		ObserveSourceSkybox(admitted, ready, observed) == SourceSkyboxStatus::Pending,
		"remaining queue wave stays pending"
	);
	observed[4] = render::SourceTextureStatus::Ready;
	Check(
		ObserveSourceSkybox(admitted, ready, observed) == SourceSkyboxStatus::Pending,
		"five completions remain private"
	);
	observed[5] = render::SourceTextureStatus::Ready;
	Check(
		ObserveSourceSkybox(admitted, ready, observed) == SourceSkyboxStatus::ReadyToPublish,
		"all six permit single publication"
	);
	observed[3] = render::SourceTextureStatus::Absent;
	Check(
		ObserveSourceSkybox(admitted, ready, observed) == SourceSkyboxStatus::Failed,
		"admitted generation disappearing is terminal failure"
	);
	std::fill(admitted.begin(), admitted.end(), false);
	std::fill(ready.begin(), ready.end(), true);
	Check(
		ObserveSourceSkybox(admitted, ready, observed) == SourceSkyboxStatus::ReadyToPublish,
		"CPU faces use completed upload facts"
	);
	auto retained = request;
	assets::TextureData huge;
	huge.Width = huge.Height = 1;
	huge.Pixels.assign(4, std::byte{0});
	huge.Pixels.reserve(MAXIMUM_TRANSFORM_IMAGE_3D_OUTPUT_BYTES + 1);
	retained.Faces[0] = std::move(huge);
	Check(
		ValidateSourceSkybox(retained, error) == SourceSkyboxStatus::OverLimit,
		"capacity charged before group retention"
	);
	auto output = request;
	for (auto &face : output.Faces) {
		SourceSdfRequest full = sdf;
		full.Width = full.Height = 1100;
		full.Format = assets::TextureFormat::RGBA32_FLOAT;
		face = std::move(full);
	}
	Check(
		ValidateSourceSkybox(output, error) == SourceSkyboxStatus::OverLimit,
		"aggregate output budget refused"
	);
}
