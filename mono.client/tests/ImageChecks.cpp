#include "ImageChecks.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <limits>

TEST_SUITE_ID("client.imagechecks")

namespace {
	engine::render::SceneCaptureResult Image() {
		engine::render::SceneCaptureResult image;
		image.Status = engine::render::SceneCaptureStatus::Ready;
		image.Format = engine::render::ResourceImageFormat::RGBA8_UNorm;
		image.Width = image.Height = 100;
		image.RowStride = 400;
		image.Pixels.resize(40'000, 0);
		image.WorldName = "eye";
		image.Projection = glm::orthoRH_ZO(-5.0f, 5.0f, -5.0f, 5.0f, 1.0f, 20.0f);
		return image;
	}
	// A conservative raster ROI contains the ideal footprint. Float projection
	// may move a boundary across one integer pixel before outward rounding.
	void CheckConservativeRegion(client::tests::ImageRegion actual, client::tests::ImageRegion ideal) {
		CAPTURE(
			actual.Left,
			actual.Top,
			actual.Right,
			actual.Bottom,
			ideal.Left,
			ideal.Top,
			ideal.Right,
			ideal.Bottom
		);
		CHECK(actual.Left <= ideal.Left);
		CHECK(actual.Left + 1 >= ideal.Left);
		CHECK(actual.Top <= ideal.Top);
		CHECK(actual.Top + 1 >= ideal.Top);
		CHECK(actual.Right >= ideal.Right);
		CHECK(actual.Right <= ideal.Right + 1);
		CHECK(actual.Bottom >= ideal.Bottom);
		CHECK(actual.Bottom <= ideal.Bottom + 1);
	}

	client::FrameObjectObservation Object() {
		client::FrameObjectObservation object;
		object.World = "eye";
		object.Frame = engine::core::CFrame(engine::core::Vector3{0, 0, -10});
		object.HalfExtent = {1, 1, 1};
		return object;
	}
}

TEST_CASE("owned image statistics clamp regions and reject incomplete planes", "[client][image]") {
	auto image = Image();
	const size_t offset = 45 * image.RowStride + 45 * 4;
	image.Pixels[offset] = image.Pixels[offset + 2] = 255;
	const auto magenta = [](uint8_t red, uint8_t green, uint8_t blue) {
		return red > 70 && green < 90 && blue > 70;
	};
	const auto all = client::tests::CountPixels(image, {0, 0, 200, 200}, magenta);
	REQUIRE(all.Available);
	CHECK(all.Samples == 10'000);
	CHECK(all.NonBlack == 1);
	CHECK(all.Matching == 1);
	CHECK(all.Region == client::tests::ImageRegion{0, 0, 100, 100});
	CHECK(client::tests::CountPixels(image, {50, 50, 60, 60}, magenta).Matching == 0);
	image.Pixels.pop_back();
	CHECK_FALSE(client::tests::CountPixels(image, {0, 0, 100, 100}, magenta).Available);
	image = Image();
	image.Format = engine::render::ResourceImageFormat::RGBA16_Float;
	CHECK_FALSE(client::tests::CountPixels(image, {0, 0, 100, 100}, magenta).Available);
}

TEST_CASE("object regions use actual oriented extents and the captured projection", "[client][image]") {
	using client::tests::ImageRegion;
	using client::tests::ProjectionStatus;
	auto image = Image();
	auto object = Object();
	const auto first = client::tests::ProjectObjectBounds(image, object);
	REQUIRE(first.Status == ProjectionStatus::Candidate);
	CheckConservativeRegion(first.Region, ImageRegion{40, 40, 60, 60});
	object.HalfExtent = {2, .5f, 1};
	const auto wide = client::tests::ProjectObjectBounds(image, object);
	REQUIRE(wide.Status == ProjectionStatus::Candidate);
	CheckConservativeRegion(wide.Region, ImageRegion{30, 45, 70, 55});
	object.Frame = engine::core::CFrame(engine::core::Vector3{0, 0, -10}) *
				   engine::core::CFrame::Angles(0, 0, 1.57079632679f);
	const auto rotated = client::tests::ProjectObjectBounds(image, object);
	REQUIRE(rotated.Status == ProjectionStatus::Candidate);
	CheckConservativeRegion(rotated.Region, ImageRegion{45, 30, 55, 70});
	object.Frame = engine::core::CFrame::LookAt({1, 1, -10}, {1, 1, -11});
	const auto shifted = client::tests::ProjectObjectBounds(image, object);
	REQUIRE(shifted.Status == ProjectionStatus::Candidate);
	CheckConservativeRegion(shifted.Region, ImageRegion{40, 35, 80, 45});
	object = Object();
	image.Projection = glm::perspectiveRH_ZO(1.57079632679f, 1.0f, 1.0f, 20.0f);
	const auto perspective = client::tests::ProjectObjectBounds(image, object);
	REQUIRE(perspective.Status == ProjectionStatus::Candidate);
	CheckConservativeRegion(perspective.Region, ImageRegion{44, 44, 56, 56});
	// A candidate region containing only black pixels cannot establish visibility.
	const auto pixels =
		client::tests::CountPixels(image, perspective.Region, [](auto, auto, auto) { return true; });
	CHECK(pixels.NonBlack == 0);
}

TEST_CASE("projection applicability is explicit at chart and clipping boundaries", "[client][image]") {
	using client::tests::ProjectionStatus;
	auto image = Image();
	auto object = Object();
	object.Frame.Position.X = 100;
	CHECK(client::tests::ProjectObjectBounds(image, object).Status == ProjectionStatus::Outside);
	object.World = "source";
	CHECK(client::tests::ProjectObjectBounds(image, object).Status == ProjectionStatus::MissingWorldMapping);
	client::tests::ObjectWorldMapping mapping{
		"source", "eye", glm::translate(glm::mat4(1.0f), glm::vec3(-100, 0, 0))
	};
	CHECK(client::tests::ProjectObjectBounds(image, object, mapping).Status == ProjectionStatus::Candidate);
	object = Object();
	object.Frame.Position.Z = -.5f;
	CHECK(client::tests::ProjectObjectBounds(image, object).Status == ProjectionStatus::NearClipped);
	object = Object();
	object.HalfExtent.X = std::numeric_limits<float>::quiet_NaN();
	CHECK(client::tests::ProjectObjectBounds(image, object).Status == ProjectionStatus::Invalid);
}
