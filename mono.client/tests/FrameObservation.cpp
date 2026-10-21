#include "FrameObservation.hpp"

#include "ImageChecks.hpp"

#include <engine/core/Paths.hpp>
#include <engine/testing/Suite.hpp>

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <client/Client.hpp>
#include <fstream>
#include <thread>

TEST_SUITE_ID("client.frame-observation")
TEST_DEPENDS("client.imagechecks")

TEST_CASE("frame subscriptions are scoped to one client and generation", "[client][observation]") {
	client::Client first;
	client::Client second;
	auto lease = client::SubscribeClientFrames(first, {}, [](auto) {});
	REQUIRE(lease);
	CHECK(client::ObservationEnabled(first, {}));
	CHECK_FALSE(client::ObservationEnabled(second, {}));
	auto duplicate = client::SubscribeClientFrames(first, {}, [](auto) {});
	CHECK_FALSE(duplicate);
	CHECK(duplicate.Failure() == "duplicate frame subscription");
	const auto generation = lease.Generation();
	lease.Reset();
	CHECK_FALSE(client::HasClientFrameSubscription(first));
	auto replacement = client::SubscribeClientFrames(first, {}, [](auto) {});
	REQUIRE(replacement);
	CHECK(replacement.Generation() != generation);
}

TEST_CASE("frame subscriptions reject wrong thread and late registration", "[client][observation]") {
	client::Client owner;
	auto lease = client::SubscribeClientFrames(owner, {}, [](auto) {});
	REQUIRE(lease);
	std::string wrongThreadFailure;
	std::jthread worker([&] {
		auto wrongThread = client::SubscribeClientFrames(owner, {}, [](auto) {});
		wrongThreadFailure = wrongThread.Failure();
	});
	worker.join();
	CHECK(wrongThreadFailure == "frame subscription belongs to another thread");
	REQUIRE(client::BeginClientFrameObservation(owner));
	auto late = client::SubscribeClientFrames(owner, {}, [](auto) {});
	CHECK_FALSE(late);
	CHECK(late.Failure() == "subscribe before Client::Initialise");
	engine::render::Renderer renderer;
	client::CloseClientFrameObservation(owner, renderer);
	CHECK(lease.Failure() == "required client frame observations were not completed");
	CHECK_FALSE(client::HasClientFrameSubscription(owner));
	lease.Reset();
}

TEST_CASE("selected frame capture keeps observation timing active between samples", "[client][observation]") {
	client::Client owner;
	client::FrameObservationOptions options;
	options.CaptureFrames = {15, 31};
	auto lease = client::SubscribeClientFrames(owner, options, [](auto) {});
	REQUIRE(lease);
	CHECK(client::ObservationEnabled(owner, {}));
	CHECK_FALSE(client::ShouldObserveClientFrame(owner, 0));
	CHECK(client::ShouldObserveClientFrame(owner, 15));
	CHECK_FALSE(client::ShouldObserveClientFrame(owner, 16));
	CHECK(client::ShouldObserveClientFrame(owner, 31));
	CHECK(lease.OwnedBytes() == 0);
}

TEST_CASE(
	"frame metadata exceeding its byte budget reports a failure before renderer admission",
	"[client][observation]"
) {
	client::Client owner;
	client::FrameObservationOptions options;
	options.MaxOwnedBytes = 128;
	auto lease = client::SubscribeClientFrames(owner, options, [](auto) {});
	REQUIRE(lease);
	engine::render::Renderer renderer;
	engine::render::View view;
	client::PrepareClientFrameObservation(
		owner, renderer, view, 0, nlohmann::json{{"payload", std::string(4096, 'x')}}, {}
	);
	CHECK(lease.Failure() == "frame observation owned-byte budget exceeded");
	CHECK(lease.OwnedBytes() == 0);
	CHECK(lease.CapturedFrames() == 0);
}

TEST_CASE(
	"native frame observations match simultaneous file captures byte for byte",
	"[client][observation][gpu][.]"
) {
	const auto root = engine::core::Paths::Base() / "frame-observation-parity";
	std::filesystem::remove_all(root);
	client::Options options;
	options.Headless = true;
	options.Width = 64;
	options.Height = 48;
	options.Entities = 8;
	options.MaximumFrames = 1;
	options.MaximumFrameRate = 60;
	options.Uncapped = true;
	options.CaptureSequence = root;
	std::shared_ptr<const client::FrameObservation> observed;
	client::Client owner;
	auto lease = client::SubscribeClientFrames(owner, {}, [&](auto frame) { observed = std::move(frame); });
	REQUIRE(lease);
	REQUIRE(owner.Initialise(options));
	REQUIRE(owner.Run() == 0);
	REQUIRE(lease.Finish());
	REQUIRE(lease.Failure().empty());
	REQUIRE(observed);
	CHECK(observed->Generation == lease.Generation());
	CHECK(observed->ClientFrame == 0);
	CHECK(observed->Image.Token != 0);
	CHECK(observed->Image.Frame != 0);
	CHECK(observed->Image.Status == engine::render::SceneCaptureStatus::Ready);
	CHECK(observed->OwnedBytes == lease.OwnedBytes());
	CHECK(observed->Image.RowStride == observed->Image.Width * 4);
	CHECK_FALSE(observed->Objects.empty());
	const auto stem = root / std::to_string(observed->ClientFrame);
	std::ifstream metadata(stem.string() + ".json");
	REQUIRE(metadata);
	nlohmann::json fromFile;
	metadata >> fromFile;
	CHECK(fromFile == observed->Metadata);
	std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> bitmap(
		SDL_LoadBMP((stem.string() + ".bmp").c_str()), SDL_DestroySurface
	);
	REQUIRE(bitmap);
	REQUIRE(bitmap->w == static_cast<int>(observed->Image.Width));
	REQUIRE(bitmap->h == static_cast<int>(observed->Image.Height));
	bool identical = true;
	for (uint32_t y = 0; y < observed->Image.Height; ++y) {
		for (uint32_t x = 0; x < observed->Image.Width; ++x) {
			Uint8 red = 0, green = 0, blue = 0, alpha = 0;
			REQUIRE(SDL_ReadSurfacePixel(
				bitmap.get(), static_cast<int>(x), static_cast<int>(y), &red, &green, &blue, &alpha
			));
			const size_t offset = y * observed->Image.RowStride + x * 4;
			const auto &pixels = observed->Image.Pixels;
			identical &= pixels[offset] == red && pixels[offset + 1] == green && pixels[offset + 2] == blue &&
						 pixels[offset + 3] == alpha;
		}
	}
	CHECK(identical);
	observed.reset();
	CHECK(lease.OwnedBytes() == 0);
	std::filesystem::remove_all(root);
}

TEST_CASE(
	"finishing or releasing an incomplete started subscription preserves its failure", "[client][observation]"
) {
	client::Client owner;
	client::FrameObservationOptions options;
	options.CaptureFrames = {5};
	auto lease = client::SubscribeClientFrames(owner, options, [](auto) {});
	REQUIRE(lease);
	REQUIRE(client::BeginClientFrameObservation(owner));
	CHECK_FALSE(lease.Finish());
	CHECK(lease.Failure() == "required client frame observations were not completed");
	lease.Reset();
	CHECK(lease.Failure() == "required client frame observations were not completed");
	CHECK_FALSE(client::HasClientFrameSubscription(owner));
}

TEST_CASE(
	"foreign draw observations retain their raster chart and actual body cut", "[client][observation]"
) {
	using namespace engine;
	scene::DrawInstance row;
	row.SourceWorld = core::Name("origin");
	// The portal producer has already mapped this copy into the raster eye chart.
	row.Frame = core::CFrame(core::Vector3{0.2f, 0, 0.5f});
	row.HalfExtent = {0.1f, 0.1f, 0.1f};
	row.Tint = {0.25f, 0.5f, 0.75f};
	row.Source = 17;
	row.Rig = 41;
	row.Variant = 3;
	row.BodyKeyHigh = 123;
	row.BodyKeyLow = 456;
	row.BodyGeneration = 7;
	row.SeamNormal = {1, 0, 0};
	row.SeamOffset = 0.2f;
	row.SeamFirst = {0, 2, 0};
	row.SeamSecond = {0, 0, 3};
	row.SeamCentre = {0.2f, 0, 0.5f};
	row.SeamMask = 2;
	render::View view;
	view.WorldName = core::Name("eye");
	view.Instances = std::span<const scene::DrawInstance>(&row, 1);
	view.EyeRig = row.Rig;
	const auto objects = client::CopyFrameObjects(view);
	REQUIRE(objects.size() == 1);
	const auto &object = objects.front();
	CHECK(object.World == "eye");
	CHECK(object.SourceWorld == "origin");
	CHECK(object.Source == row.Source);
	CHECK(object.Rig == row.Rig);
	CHECK(object.Variant == row.Variant);
	CHECK(object.BodyKeyHigh == row.BodyKeyHigh);
	CHECK(object.BodyKeyLow == row.BodyKeyLow);
	CHECK(object.BodyGeneration == row.BodyGeneration);
	CHECK(object.Frame.Position == row.Frame.Position);
	CHECK(object.HalfExtent == row.HalfExtent);
	CHECK(object.Tint == row.Tint);
	CHECK(object.SeamNormal == row.SeamNormal);
	CHECK(object.SeamOffset == row.SeamOffset);
	CHECK(object.SeamFirst == row.SeamFirst);
	CHECK(object.SeamSecond == row.SeamSecond);
	CHECK(object.SeamCentre == row.SeamCentre);
	CHECK(object.SeamMask == row.SeamMask);
	CHECK(object.DrawIndex == 0);
	CHECK_FALSE(object.HiddenForEye);

	render::SceneCaptureResult image;
	image.Status = render::SceneCaptureStatus::Ready;
	image.Format = render::ResourceImageFormat::RGBA8_UNorm;
	image.Width = image.Height = 64;
	image.RowStride = 256;
	image.Pixels.resize(image.RowStride * image.Height, 0);
	image.WorldName = "eye";
	image.Projection = glm::mat4{1};
	const auto bounds = client::tests::ProjectObjectBounds(image, object);
	REQUIRE(bounds.Status == client::tests::ProjectionStatus::Candidate);
	CHECK(bounds.Region == client::tests::ImageRegion{35, 28, 42, 36});
	client::tests::ObjectWorldMapping mapping;
	mapping.SourceWorld = "origin";
	mapping.EyeWorld = "eye";
	mapping.SourceToEye[3][0] = 2;
	const auto alreadyMapped = client::tests::ProjectObjectBounds(image, object, mapping);
	CHECK(alreadyMapped.Status == client::tests::ProjectionStatus::Candidate);
	CHECK(alreadyMapped.Region == bounds.Region);
	// This ROI says where to inspect. The black synthetic image proves no visibility.
	CHECK(client::tests::CountPixels(image, bounds.Region, [](auto, auto, auto) {
			  return false;
		  }).NonBlack == 0);
}

TEST_CASE("owned eye exclusions preserve world and variant scoped body selection", "[client][observation]") {
	using namespace engine;
	std::array<scene::DrawInstance, 6> rows;
	rows[0].Rig = 9;
	rows[1].Rig = 9;
	rows[1].SourceWorld = core::Name("foreign");
	rows[2].Rig = 9;
	rows[2].Variant = 4;
	rows[3].Source = 9;
	rows[4].Rig = 8;
	rows[5].Rig = 8;
	const std::array<uint32_t, 1> hidden{4};
	render::View view;
	view.WorldName = core::Name("eye");
	view.Instances = rows;
	view.EyeRig = 9;
	view.EyeHiddenRows = hidden;
	const auto objects = client::CopyFrameObjects(view);
	REQUIRE(objects.size() == rows.size());
	CHECK(objects[0].HiddenForEye);
	CHECK_FALSE(objects[1].HiddenForEye);
	CHECK_FALSE(objects[2].HiddenForEye);
	CHECK(objects[3].HiddenForEye);
	CHECK(objects[4].HiddenForEye);
	CHECK_FALSE(objects[5].HiddenForEye);
	CHECK(objects[0].SourceWorld == "eye");
	CHECK(objects[1].SourceWorld == "foreign");
	CHECK(objects[4].DrawIndex == hidden.front());

	view.EyeRig = 0;
	const auto withoutEyeRig = client::CopyFrameObjects(view);
	CHECK_FALSE(withoutEyeRig[0].HiddenForEye);
	CHECK_FALSE(withoutEyeRig[3].HiddenForEye);
	CHECK(withoutEyeRig[4].HiddenForEye);
}
