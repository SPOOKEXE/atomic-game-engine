#include "FrameObservation.hpp"

#include <engine/core/Paths.hpp>
#include <engine/testing/Suite.hpp>

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>

#include <client/Client.hpp>
#include <fstream>
#include <thread>

TEST_SUITE_ID("client.frame-observation")

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
