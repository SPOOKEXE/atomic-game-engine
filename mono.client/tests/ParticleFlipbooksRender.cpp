// The particle flipbook demo ships its atlas with the examples rather than
// relying on a developer-local content store. Exercise that normal client path
// on a device and inspect the captured sprite colours in every playback bay.

#include <engine/core/Paths.hpp>
#include <engine/testing/Suite.hpp>

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>

#include <client/Client.hpp>
#include <filesystem>
#include <memory>
#include <string>

TEST_SUITE_ID("client.particleflipbooks.render")
TEST_DEPENDS("client.scene.tick")
TEST_DEPENDS("engine.examples.scene")

namespace {

	using namespace engine;

	constexpr int WIDTH = 320;
	constexpr int HEIGHT = 180;
	constexpr int FRAMES = 90;

	struct SpritePixels {
		size_t Fox = 0;
		size_t Missing = 0;
		bool Readable = true;
	};

	SpritePixels CountSprites(SDL_Surface *image, int firstX, int lastX) {
		SpritePixels pixels;
		for (int y = 0; y < image->h * 2 / 3; ++y) {
			for (int x = firstX; x < lastX; ++x) {
				Uint8 red = 0, green = 0, blue = 0, alpha = 0;
				pixels.Readable &= SDL_ReadSurfacePixel(image, x, y, &red, &green, &blue, &alpha);
				pixels.Fox += red > 150 && green > 35 && green < 230 && blue < 170;
				pixels.Missing += red > 120 && green < 70 && blue > 120;
			}
		}
		return pixels;
	}
}

TEST_CASE(
	"the packaged particle flipbook renders nonrectangular fox sprites without a content source",
	"[client][gpu][particles][flipbook][.]"
) {
	const std::filesystem::path root = core::Paths::Base() / "particle-flipbook-render";
	std::filesystem::remove_all(root);
	std::filesystem::create_directories(root);

	client::Options options;
	options.Headless = true;
	options.Width = WIDTH;
	options.Height = HEIGHT;
	options.MaximumFrames = FRAMES;
	options.MaximumFrameRate = 60;
	options.Uncapped = true;
	options.ScriptPath =
		(core::Paths::Base().parent_path() / "assets/examples/scripts/ParticleFlipbooks.luau").string();
	options.CaptureSequence = root;
	// The atlas must resolve from the staged example tree. No local or remote
	// content source participates in this capture.
	options.ContentSources.clear();
	client::Client client;
	REQUIRE(client.Initialise(options));
	CHECK(client.Run() == 0);

	const std::filesystem::path imagePath = root / (std::to_string(FRAMES - 1) + ".bmp");
	std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> image(
		SDL_LoadBMP(imagePath.string().c_str()), SDL_DestroySurface
	);
	REQUIRE(image);
	CHECK(image->w == WIDTH);
	CHECK(image->h == HEIGHT);

	for (int bay = 0; bay < 4; ++bay) {
		const int firstX = bay * WIDTH / 4;
		const int lastX = (bay + 1) * WIDTH / 4;
		const SpritePixels pixels = CountSprites(image.get(), firstX, lastX);
		CAPTURE(bay, pixels.Fox, pixels.Missing, pixels.Readable);
		CHECK(pixels.Readable);
		CHECK(pixels.Fox > 5);
		CHECK(pixels.Missing == 0);
	}
}
