// A Composer export reaches ordinary GUI and particle consumers through signed content delivery.

#include <engine/assets/Signature.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/core/Paths.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cdn/Publisher.hpp>
#include <client/Client.hpp>
#include <filesystem>
#include <fstream>
#include <memory>
#include <studio/ImageComposer.hpp>

TEST_SUITE_ID("studio.imagegraph.content.render")
TEST_DEPENDS("studio.imagecomposer")
TEST_DEPENDS("client.scene.tick")
TEST_DEPENDS("cdn.publisher")

namespace {
	constexpr int WIDTH = 320;
	constexpr int HEIGHT = 180;
	constexpr int FRAMES = 45;

	struct ConsumerPixels {
		size_t Cyan = 0;
		size_t Missing = 0;
		bool Readable = true;
	};

	ConsumerPixels CountConsumer(SDL_Surface *image, int firstX, int firstY, int lastX, int lastY) {
		ConsumerPixels pixels;
		for (int y = firstY; y < lastY; ++y) {
			for (int x = firstX; x < lastX; ++x) {
				Uint8 red = 0, green = 0, blue = 0, alpha = 0;
				pixels.Readable &= SDL_ReadSurfacePixel(image, x, y, &red, &green, &blue, &alpha);
				pixels.Cyan += red < 80 && green > 150 && blue > 150;
				pixels.Missing += red > 120 && green < 70 && blue > 120;
			}
		}
		return pixels;
	}
}

TEST_CASE(
	"a Composer texture export renders in ImageLabel and ParticleEmitter through ordinary content delivery",
	"[studio][imagegraph][gpu][.]"
) {
	using namespace engine;
	const std::filesystem::path root = core::Paths::Base() / "imagegraph-consumer-render";
	std::filesystem::remove_all(root);
	std::filesystem::create_directories(root / "baked");

	studio::ImageComposerState composer;
	studio::InitialiseImageComposer(composer);
	imagegraph::Document document;
	document.Nodes = {
		{"colour", imagegraph::Solid{16, 16, {0, 255, 255, 255}}, {}, {}},
		{"smaller", imagegraph::Resize{8, 8, imagegraph::Sampling::Nearest}, {"colour"}, {}},
	};
	document.Outputs = {{"image", "smaller"}};
	imagegraph::Diagnostic diagnostic;
	REQUIRE(studio::LoadImageComposerGraph(document, composer.Graph, diagnostic));
	composer.Outputs = document.Outputs;
	studio::ImageComposerHost host;
	host.BakedRoot = root / "baked";
	size_t registrations = 0;
	host.Register = [&](std::span<const std::byte> bytes, const std::string &name) {
		++registrations;
		CHECK(name == "composed.atex");
		core::ByteReader reader(bytes);
		assets::TextureData texture;
		REQUIRE(assets::Texture::Read(reader, texture));
		CHECK(texture.Width == 8);
		CHECK(texture.Height == 8);
	};
	REQUIRE(studio::ExportImageComposer(composer, host, "composed"));
	CHECK(registrations == 1);
	REQUIRE(std::filesystem::exists(root / "baked/composed.atex"));

	std::array<std::byte, 32> seed{};
	const auto key = assets::SigningKey::FromSeed(seed);
	REQUIRE(key);
	cdn::PublishSettings publication;
	publication.TrainDictionary = false;
	const auto published = cdn::Publish(root / "baked", root / "store", *key, publication);
	REQUIRE(published);
	CHECK(published->Assets == 1);

	const auto script = root / "consumers.luau";
	{
		std::ofstream output(script);
		REQUIRE(output.good());
		output << R"(
local player = game:GetService("Players").LocalPlayer
assert(player, "ordinary client must have a local player")
local screen = Instance.new("ScreenGui")
screen.Parent = player:FindFirstChild("PlayerGui")
local label = Instance.new("ImageLabel")
label.Position = UDim2.new(0, 20, 0, 20)
label.Size = UDim2.new(0, 48, 0, 48)
label.BackgroundTransparency = 1
label.BorderSizePixel = 0
label.Image = "composed.atex"
label.Parent = screen

local part = Instance.new("Part")
part.Anchored = true
part.Transparency = 1
part.Size = Vector3.new(0.1, 0.1, 0.1)
part.CFrame = CFrame.new(2.5, 0, 0)
part.Parent = workspace
local emitter = Instance.new("ParticleEmitter")
emitter.Texture = "composed.atex"
emitter.Rate = 60
emitter.Lifetime = NumberRange.new(10)
emitter.Speed = NumberRange.new(0)
emitter.Size = NumberSequence.new(1.5)
emitter.Transparency = NumberSequence.new(0)
emitter.Color = ColorSequence.new(Color3.new(1, 1, 1))
emitter.Parent = part
)";
	}
	const auto cameras = root / "client/DemoCameras";
	std::filesystem::create_directories(cameras);
	{
		std::ofstream output(cameras / "consumers.client.luau");
		REQUIRE(output.good());
		output << R"(
local camera = Instance.new("Camera")
camera.FieldOfView = 60
camera.CFrame = CFrame.lookAt(Vector3.new(0, 0, 8), Vector3.new(0, 0, 0))
camera.CameraSubject = nil
camera.Parent = workspace
workspace.CurrentCamera = camera
camera.CameraType = Enum.CameraType.Scriptable
local owned = Instance.new("ObjectValue", script)
owned.Name = "PublishedCamera"
owned.Value = camera
)";
		REQUIRE(output.good());
	}

	client::Options options;
	options.Headless = true;
	options.Width = WIDTH;
	options.Height = HEIGHT;
	options.MaximumFrames = FRAMES;
	options.MaximumFrameRate = 60;
	options.Uncapped = true;
	options.ScriptPath = script.string();
	options.ContentSources = {"dir:" + (root / "store").string()};
	options.ContentPublisherKey = key->Public().ToHex();
	options.CaptureSequence = root / "frames";
	client::Client player;
	REQUIRE(player.Initialise(options));
	CHECK(player.Run() == 0);

	const auto capture = options.CaptureSequence / (std::to_string(FRAMES - 1) + ".bmp");
	std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> image(
		SDL_LoadBMP(capture.string().c_str()), SDL_DestroySurface
	);
	REQUIRE(image);
	REQUIRE(image->w == WIDTH);
	REQUIRE(image->h == HEIGHT);
	const auto labelPixels = CountConsumer(image.get(), 20, 20, 68, 68);
	const auto particlePixels = CountConsumer(image.get(), WIDTH / 2, 0, WIDTH, HEIGHT * 2 / 3);
	CAPTURE(labelPixels.Cyan, labelPixels.Missing, particlePixels.Cyan, particlePixels.Missing);
	CHECK(labelPixels.Readable);
	CHECK(labelPixels.Cyan > 2000);
	CHECK(labelPixels.Missing == 0);
	CHECK(particlePixels.Readable);
	CHECK(particlePixels.Cyan > 100);
	CHECK(particlePixels.Missing == 0);
}
