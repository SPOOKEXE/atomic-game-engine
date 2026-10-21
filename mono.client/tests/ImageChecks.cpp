#include "ImageChecks.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <limits>
#include <stdexcept>

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

namespace {
	void Colour(engine::render::SceneCaptureResult &image, uint8_t red, uint8_t green, uint8_t blue) {
		for (size_t pixel = 0; pixel < image.Pixels.size(); pixel += 4) {
			image.Pixels[pixel] = red;
			image.Pixels[pixel + 1] = green;
			image.Pixels[pixel + 2] = blue;
		}
	}
	void MagentaPixel(engine::render::SceneCaptureResult &image, uint32_t x, uint32_t y) {
		const size_t offset = y * image.RowStride + size_t(x) * 4;
		image.Pixels[offset] = image.Pixels[offset + 2] = 255;
	}
	bool MagentaSample(uint8_t red, uint8_t green, uint8_t blue) {
		return red > 200 && green < 10 && blue > 200;
	}
}

TEST_CASE("actual rotated box surfaces reject a matching bounding-rectangle distractor", "[client][image]") {
	using client::tests::ObjectPixelStatus;
	auto image = Image();
	auto object = Object();
	object.HalfExtent = {2, .5f, 1};
	object.Frame = object.Frame * engine::core::CFrame::Angles(0, 0, .78539816339f);
	MagentaPixel(image, 33, 33);
	const auto bounds = client::tests::ProjectObjectBounds(image, object);
	REQUIRE(bounds.Status == client::tests::ProjectionStatus::Candidate);
	CHECK(client::tests::CountPixels(image, bounds.Region, MagentaSample).Matching == 1);
	const auto distractor = client::tests::CountObjectPixels(image, object, MagentaSample);
	REQUIRE(distractor.Status == ObjectPixelStatus::Tested);
	CHECK(distractor.Pixels.Samples > 0);
	CHECK(distractor.Pixels.Matching == 0);
	MagentaPixel(image, 50, 50);
	const auto actual = client::tests::CountObjectPixels(image, object, MagentaSample);
	REQUIRE(actual.Status == ObjectPixelStatus::Tested);
	CHECK(actual.Pixels.Matching == 1);
	std::vector<uint8_t> unionMask(size_t(image.Width) * image.Height, 0);
	(void)client::tests::CountObjectPixels(image, object, MagentaSample, std::nullopt, unionMask);
	(void)client::tests::CountObjectPixels(image, object, MagentaSample, std::nullopt, unionMask);
	CHECK(std::count(unionMask.begin(), unionMask.end(), uint8_t(1)) == 1);
}

TEST_CASE("actual box surface samples retain the copied seam half and aperture masks", "[client][image]") {
	auto image = Image();
	Colour(image, 255, 0, 255);
	auto object = Object();
	const auto full = client::tests::CountObjectPixels(image, object, MagentaSample);
	REQUIRE(full.Status == client::tests::ObjectPixelStatus::Tested);
	CHECK(full.Pixels.Matching == 400);
	object.SeamNormal = {1, 0, 0};
	object.SeamOffset = 0;
	const auto half = client::tests::CountObjectPixels(image, object, MagentaSample);
	CHECK(half.Pixels.Matching == 200);
	object.SeamCentre = {0, 0, -9};
	object.SeamFirst = {.5f, 0, 0};
	object.SeamSecond = {0, .5f, 0};
	object.SeamMask = 1;
	const auto outside = client::tests::CountObjectPixels(image, object, MagentaSample);
	CHECK(outside.Pixels.Matching == 350);
	object.SeamMask = 2;
	const auto inside = client::tests::CountObjectPixels(image, object, MagentaSample);
	CHECK(inside.Pixels.Matching == 50);
	CHECK_FALSE(client::tests::KeepObjectSurface({-.25f, 0, -9}, object));
	CHECK(client::tests::KeepObjectSurface({.25f, 0, -9}, object));
	CHECK_FALSE(client::tests::KeepObjectSurface({.25f, 1, -9}, object));
}

TEST_CASE(
	"explicit same-world portal routes transform boxes and require the finite aperture", "[client][image]"
) {
	using client::tests::ObjectPixelStatus;
	auto image = Image();
	Colour(image, 255, 0, 255);
	auto object = Object();
	object.Frame.Position.X = 100;
	client::tests::ObjectProjectionRoute route{
		{"eye", "eye", glm::translate(glm::mat4(1.0f), glm::vec3(-100, 0, 0))},
		client::tests::PortalAperture{{0, 0, -5}, {0, 0, 1}, {.5f, 0, 0}, {0, .5f, 0}}
	};
	const auto portal = client::tests::CountObjectPixels(image, object, MagentaSample, route);
	REQUIRE(portal.Status == ObjectPixelStatus::Tested);
	CHECK(portal.Pixels.Matching == 100);
	object.HiddenForEye = true;
	CHECK(client::tests::CountObjectPixels(image, object, MagentaSample).Status == ObjectPixelStatus::Hidden);
	CHECK(client::tests::CountObjectPixels(image, object, MagentaSample, route).Pixels.Matching == 100);
	route.Aperture->Centre.X = 10;
	const auto missed = client::tests::CountObjectPixels(image, object, MagentaSample, route);
	REQUIRE(missed.Status == ObjectPixelStatus::Tested);
	CHECK(missed.Pixels.Samples == 0);
	CHECK(missed.Pixels.Matching == 0);
	route.Aperture->Centre = {0, 0, -15};
	CHECK(client::tests::CountObjectPixels(image, object, MagentaSample, route).Pixels.Matching == 0);
	// A native inverse uniform scale must retain its affine meaning after float rounding.
	object = Object();
	object.Frame.Position.Z = -1.38f;
	object.HalfExtent = {.138f, .138f, .138f};
	route.Aperture.reset();
	route.Mapping.SourceToEye = glm::inverse(glm::scale(glm::mat4(1.0f), glm::vec3(.138f)));
	const auto scaled = client::tests::CountObjectPixels(image, object, MagentaSample, route);
	REQUIRE(scaled.Status == ObjectPixelStatus::Tested);
	CHECK(scaled.Pixels.Matching == 400);
}

TEST_CASE(
	"unsupported object forms and missing geometry cannot pass actual surface checks", "[client][image]"
) {
	using client::tests::ObjectPixelStatus;
	auto image = Image();
	auto object = Object();
	for (const char *mesh : {"engine.Sphere", "engine.Wedge", "engine.Cylinder", "authored.Mesh"}) {
		object.Mesh = mesh;
		const auto unsupported = client::tests::CountObjectPixels(image, object, MagentaSample);
		CHECK(unsupported.Status == ObjectPixelStatus::Unsupported);
		CHECK(unsupported.Pixels.Matching == 0);
	}
	object = Object();
	object.Mesh = "engine.Cube";
	CHECK(client::tests::CountObjectPixels(image, object, MagentaSample).Status == ObjectPixelStatus::Tested);
	object.World = "foreign";
	CHECK(
		client::tests::CountObjectPixels(image, object, MagentaSample).Status ==
		ObjectPixelStatus::Unsupported
	);
	object = Object();
	object.Frame.Position.Z = -.5f;
	CHECK(
		client::tests::CountObjectPixels(image, object, MagentaSample).Status ==
		ObjectPixelStatus::Unsupported
	);
	object = Object();
	object.SeamNormal.X = std::numeric_limits<float>::quiet_NaN();
	CHECK(
		client::tests::CountObjectPixels(image, object, MagentaSample).Status == ObjectPixelStatus::Invalid
	);
	object = Object();
	image.Projection[0][0] = std::numeric_limits<float>::infinity();
	CHECK(
		client::tests::CountObjectPixels(image, object, MagentaSample).Status == ObjectPixelStatus::Invalid
	);
	image = Image();
	image.Pixels.pop_back();
	CHECK(
		client::tests::CountObjectPixels(image, object, MagentaSample).Status == ObjectPixelStatus::Invalid
	);
}

TEST_CASE("failed surface checks cannot publish partial matching variant masks", "[client][image]") {
	auto image = Image();
	Colour(image, 255, 0, 255);
	auto object = Object();
	std::vector<uint8_t> unionMask(size_t(image.Width) * image.Height, 0);
	size_t calls = 0;
	const auto brokenPredicate = [&](auto, auto, auto) {
		if (++calls == 2) throw std::runtime_error("synthetic appearance check failure");
		return true;
	};
	CHECK_THROWS_AS(
		client::tests::CountObjectPixels(image, object, brokenPredicate, std::nullopt, unionMask),
		std::runtime_error
	);
	CHECK(std::count(unionMask.begin(), unionMask.end(), uint8_t(1)) == 0);
	client::tests::ObjectProjectionRoute route{{"eye", "eye", glm::mat4(1.0f)}, std::nullopt};
	route.Mapping.SourceToEye[0][3] = .01f;
	CHECK(
		client::tests::CountObjectPixels(image, object, MagentaSample, route).Status ==
		client::tests::ObjectPixelStatus::Invalid
	);
	image.CameraFrame = engine::core::CFrame({}, glm::quat(0, 0, 0, .70710678118f));
	CHECK(
		client::tests::CountObjectPixels(image, object, MagentaSample).Status ==
		client::tests::ObjectPixelStatus::Invalid
	);
}
