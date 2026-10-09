// A live Luau input drives ordinary signed image consumers.

#include <engine/assets/Signature.hpp>
#include <engine/assets/Texture.hpp>
#include <engine/bake/ImageGraph.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/core/Paths.hpp>
#include <engine/testing/Suite.hpp>

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <assetc/ImageGraph.hpp>
#include <cdn/Publisher.hpp>
#include <client/Client.hpp>
#include <filesystem>
#include <fstream>
#include <memory>

TEST_SUITE_ID("studio.imagegraph.live.content.render")
TEST_DEPENDS("engine.imagegraph.document")
TEST_DEPENDS("client.scene.tick")
TEST_DEPENDS("cdn.publisher")

namespace {
	constexpr int WIDTH = 320;
	constexpr int HEIGHT = 180;
	constexpr int FRAMES = 150;

	enum class PixelColour { Cyan, Yellow, Blue, Red };
	size_t CountPixels(SDL_Surface *surface, PixelColour colour, int x0, int y0, int x1, int y1) {
		size_t count = 0;
		bool readable = true;
		for (int y = y0; y < y1; ++y) {
			for (int x = x0; x < x1; ++x) {
				Uint8 red = 0, green = 0, blue = 0, alpha = 0;
				readable &= SDL_ReadSurfacePixel(surface, x, y, &red, &green, &blue, &alpha);
				switch (colour) {
				case PixelColour::Cyan:
					count += red < 80 && green > 150 && blue > 150;
					break;
				case PixelColour::Yellow:
					count += red > 150 && green > 150 && blue < 80;
					break;
				case PixelColour::Blue:
					count += red < 80 && green < 80 && blue > 150;
					break;
				case PixelColour::Red:
					count += red > 150 && green < 80 && blue < 80;
					break;
				}
			}
		}
		REQUIRE(readable);
		return count;
	}

	using Surface = std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>;
	Surface ReadFrame(const std::filesystem::path &root, int frame) {
		const auto name = root / (std::to_string(frame) + ".bmp");
		Surface surface(SDL_LoadBMP(name.string().c_str()), SDL_DestroySurface);
		REQUIRE(surface);
		REQUIRE(surface->w == WIDTH);
		REQUIRE(surface->h == HEIGHT);
		return surface;
	}
}

TEST_CASE(
	"a signed live graph responds to Luau input changes in labels particles materials and every sky face",
	"[studio][imagegraph][live][gpu][.]"
) {
	using namespace engine;
	const auto root = core::Paths::Base() / "live-imagegraph-consumer-render";
	std::filesystem::remove_all(root);
	std::filesystem::create_directories(root);
	imagegraph::Document document;
	document.Nodes = {
		{"colour", imagegraph::Solid{16, 16, {0, 255, 255, 255}}, {}, {}},
		{"orange", imagegraph::Solid{16, 16, {255, 64, 0, 255}}, {}, {}}
	};
	document.Outputs = {{"image", "colour"}, {"orange", "orange"}};
	document.Parameters = {{"tint", std::array<uint8_t, 4>{0, 255, 255, 255}}};
	document.Bindings = {{"colour", "colour", "tint"}};
	imagegraph::Diagnostic diagnostic;
	bake::CookedImageGraph cooked;
	REQUIRE(bake::CookImageGraph(document, "live-signal.aimagegraph", {}, cooked, diagnostic));
	std::string failure;
	REQUIRE(assetc::PublishCookedImageGraph(root / "baked", cooked, failure));
	const auto key = assets::SigningKey::FromSeed(std::array<std::byte, 32>{});
	REQUIRE(key);
	cdn::PublishSettings publication;
	publication.TrainDictionary = false;
	const auto published = cdn::Publish(root / "baked", root / "store", *key, publication);
	REQUIRE(published);
	CHECK(published->Assets == 1);
	const auto script = root / "live-consumers.luau";
	{
		std::ofstream output(script);
		REQUIRE(output.good());
		output << R"(
local graph = Instance.new("ImageGraph")
graph.Name = "LiveSignal"
graph.InstanceKey = "live-consumers"
graph.Graph = "live-signal.aimagegraph"
graph.Parent = workspace
local image = graph:GetImage("image")
local player = game:GetService("Players").LocalPlayer
assert(player)
local screen = Instance.new("ScreenGui")
screen.Parent = player:FindFirstChild("PlayerGui")
local label = Instance.new("ImageLabel")
label.Position = UDim2.new(0, 20, 0, 20)
label.Size = UDim2.new(0, 48, 0, 48)
label.BackgroundTransparency = 1
label.BorderSizePixel = 0
label.Image = image
label.Parent = screen

local midtone = Instance.new("ImageLabel")
midtone.Position = UDim2.new(0, 80, 0, 20)
midtone.Size = UDim2.new(0, 32, 0, 32)
midtone.BackgroundTransparency = 1
midtone.BorderSizePixel = 0
midtone.Image = graph:GetImage("orange")
midtone.Parent = screen

local part = Instance.new("Part")
part.Anchored = true
part.Transparency = 1
part.Size = Vector3.new(0.1, 0.1, 0.1)
part.CFrame = CFrame.new(2.5, 0, 0)
part.Parent = workspace
local emitter = Instance.new("ParticleEmitter")
emitter.Texture = image
emitter.Rate = 60
emitter.Lifetime = NumberRange.new(10)
emitter.Speed = NumberRange.new(0)
emitter.Size = NumberSequence.new(1.5)
emitter.Transparency = NumberSequence.new(0)
emitter.Color = ColorSequence.new(Color3.new(1, 0, 1))
emitter.Parent = part

local material = Instance.new("MeshPart")
material.Name = "LiveMaterial"
material.Anchored = true
material.Size = Vector3.new(2, 2, 1)
material.CFrame = CFrame.new(-2.5, 0, 0)
material.Color = Color3.new(1, 0, 1)
material.TextureID = image
material.EmissiveMap = image
material.EmissiveTint = Color3.new(1, 0, 1)
material.EmissiveStrength = 1
material.RenderFeatureEnableMask = 4
material.Parent = workspace

local sky = Instance.new("SkyboxTextures")
sky.Front = image
sky.Back = image
sky.Left = image
sky.Right = image
sky.Up = image
sky.Down = image
sky.Parent = game:GetService("Lighting")

local camera = Instance.new("Camera")
camera.FieldOfView = 60
camera.CFrame = CFrame.lookAt(Vector3.new(0, 0, 8), Vector3.new(0, 0, 0))
camera.Parent = workspace
workspace.CurrentCamera = camera
local directions = {
    Vector3.new(0, 0, -1), Vector3.new(0, 0, 1),
    Vector3.new(-1, 0, 0), Vector3.new(1, 0, 0),
    Vector3.new(0, 1, 0), Vector3.new(0, -1, 0),
}
local beat = 0
game:GetService("RunService").Heartbeat:Connect(function()
    beat += 1
    if beat == 45 then
        graph:SetInput("tint", Color3.new(1, 1, 0))
        assert(graph:GetInput("tint") == Color3.new(1, 1, 0))
    end
    if beat >= 80 then
        local face = math.min(6, math.floor((beat - 80) / 10) + 1)
        local up = face >= 5 and Vector3.new(0, 0, 1) or Vector3.new(0, 1, 0)
        camera.CFrame = CFrame.lookAt(Vector3.new(0, 0, 8), Vector3.new(0, 0, 8) + directions[face], up)
    end
end)
)";
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
	REQUIRE(player.Run() == 0);
	const auto before = ReadFrame(options.CaptureSequence, 30);
	const auto after = ReadFrame(options.CaptureSequence, 70);
	Uint8 red = 0, green = 0, blue = 0, alpha = 0;
	REQUIRE(SDL_ReadSurfacePixel(before.get(), 90, 30, &red, &green, &blue, &alpha));
	CHECK(red == 255);
	CHECK(green >= 63);
	CHECK(green <= 65);
	CHECK(blue == 0);
	CHECK(CountPixels(before.get(), PixelColour::Cyan, 20, 20, 68, 68) > 2000);
	CHECK(CountPixels(after.get(), PixelColour::Yellow, 20, 20, 68, 68) > 2000);
	CHECK(CountPixels(before.get(), PixelColour::Blue, WIDTH / 2, 0, WIDTH, HEIGHT * 2 / 3) > 100);
	CHECK(CountPixels(after.get(), PixelColour::Red, WIDTH / 2, 0, WIDTH, HEIGHT * 2 / 3) > 100);
	CHECK(CountPixels(before.get(), PixelColour::Blue, 98, 80, 118, 100) > 300);
	CHECK(CountPixels(after.get(), PixelColour::Red, 98, 80, 118, 100) > 300);
	for (int face = 0; face < 6; ++face) {
		CAPTURE(face);
		const auto frame = ReadFrame(options.CaptureSequence, 87 + face * 10);
		CHECK(CountPixels(frame.get(), PixelColour::Yellow, 80, 130, 150, 175) > 2800);
	}
}

TEST_CASE(
	"a signed graph renders its ready output while another demanded output has no source",
	"[studio][imagegraph][live][independent][gpu][.]"
) {
	using namespace engine;
	const auto root = core::Paths::Base() / "live-imagegraph-independent-outputs";
	std::filesystem::remove_all(root);
	std::filesystem::create_directories(root / "baked");
	imagegraph::Document document;
	document.Nodes = {
		{"available", imagegraph::Source{"ready.atex"}, {}, {}},
		{"unavailable", imagegraph::Source{"missing.atex"}, {}, {}}
	};
	document.Outputs = {{"ready", "available"}, {"waiting", "unavailable"}};
	imagegraph::Diagnostic diagnostic;
	std::string encoded;
	REQUIRE(imagegraph::Write(document, encoded, diagnostic));
	{
		std::ofstream output(root / "baked/independent.aimagegraph", std::ios::binary);
		output.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
		REQUIRE(output.good());
	}
	assets::TextureData texture;
	texture.Width = texture.Height = 16;
	texture.Format = assets::TextureFormat::RGBA8;
	texture.Pixels.resize(16 * 16 * 4);
	for (size_t i = 0; i < texture.Pixels.size(); i += 4) {
		texture.Pixels[i] = std::byte{0};
		texture.Pixels[i + 1] = texture.Pixels[i + 2] = texture.Pixels[i + 3] = std::byte{255};
	}
	core::ByteWriter writer;
	REQUIRE(assets::Texture::Write(writer, texture));
	{
		std::ofstream output(root / "baked/ready.atex", std::ios::binary);
		const auto bytes = writer.Bytes();
		output.write(
			reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())
		);
		REQUIRE(output.good());
	}
	const auto key = assets::SigningKey::FromSeed(std::array<std::byte, 32>{});
	REQUIRE(key);
	cdn::PublishSettings publication;
	publication.TrainDictionary = false;
	const auto published = cdn::Publish(root / "baked", root / "store", *key, publication);
	REQUIRE(published);
	REQUIRE(published->Assets == 2);
	const auto script = root / "independent.luau";
	{
		std::ofstream output(script);
		output << R"(
local player = game:GetService("Players").LocalPlayer
assert(player)
local screen = Instance.new("ScreenGui")
screen.Parent = player:FindFirstChild("PlayerGui")
local graph = Instance.new("ImageGraph")
graph.InstanceKey = "independent"
graph.Graph = "independent.aimagegraph"
graph.Parent = workspace
for index, output in ipairs({"ready", "waiting"}) do
    local label = Instance.new("ImageLabel")
    label.Position = UDim2.new(0, 20 + (index-1)*60, 0, 20)
    label.Size = UDim2.new(0, 48, 0, 48)
    label.BackgroundTransparency = 1
    label.BorderSizePixel = 0
    label.Image = graph:GetImage(output)
    label.Parent = screen
end
)";
		REQUIRE(output.good());
	}
	client::Options options;
	options.Headless = true;
	options.Width = WIDTH;
	options.Height = HEIGHT;
	options.MaximumFrames = 45;
	options.MaximumFrameRate = 60;
	options.Uncapped = true;
	options.ScriptPath = script.string();
	options.ContentSources = {"dir:" + (root / "store").string()};
	options.ContentPublisherKey = key->Public().ToHex();
	options.Capture = root / "ready-and-waiting.bmp";
	client::Client player;
	REQUIRE(player.Initialise(options));
	REQUIRE(player.Run() == 0);
	Surface surface(SDL_LoadBMP(options.Capture.string().c_str()), SDL_DestroySurface);
	REQUIRE(surface);
	CHECK(CountPixels(surface.get(), PixelColour::Cyan, 20, 20, 68, 68) > 2000);
	CHECK(CountPixels(surface.get(), PixelColour::Cyan, 80, 20, 128, 68) == 0);
}

TEST_CASE(
	"signed graph buffer source updates reach labels and particles on the same Heartbeat frame",
	"[studio][imagegraph][live][editable][sameframe][gpu][.]"
) {
	using namespace engine;
	const auto root = core::Paths::Base() / "live-imagegraph-editable-sameframe";
	std::filesystem::remove_all(root);
	std::filesystem::create_directories(root / "baked");
	imagegraph::Document document;
	document.Nodes = {{"source", imagegraph::Source{"default.atex"}, {}, {}}};
	document.Outputs = {{"image", "source"}};
	document.Parameters = {{"pixels", std::string("default.atex")}};
	document.Bindings = {{"source", "path", "pixels"}};
	imagegraph::Diagnostic diagnostic;
	std::string encoded;
	REQUIRE(imagegraph::Write(document, encoded, diagnostic));
	{
		std::ofstream output(root / "baked/buffer.aimagegraph", std::ios::binary);
		output.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
		REQUIRE(output.good());
	}
	assets::TextureData texture;
	texture.Width = texture.Height = 8;
	texture.Format = assets::TextureFormat::RGBA8_LINEAR;
	texture.Pixels.resize(8 * 8 * 4, std::byte{255});
	core::ByteWriter writer;
	REQUIRE(assets::Texture::Write(writer, texture));
	{
		std::ofstream output(root / "baked/default.atex", std::ios::binary);
		output.write(
			reinterpret_cast<const char *>(writer.Bytes().data()),
			static_cast<std::streamsize>(writer.Bytes().size())
		);
		REQUIRE(output.good());
	}
	const auto key = assets::SigningKey::FromSeed(std::array<std::byte, 32>{});
	REQUIRE(key);
	cdn::PublishSettings publication;
	publication.TrainDictionary = false;
	const auto published = cdn::Publish(root / "baked", root / "store", *key, publication);
	REQUIRE(published);
	REQUIRE(published->Assets == 2);
	const auto script = root / "sameframe.luau";
	{
		std::ofstream output(script);
		REQUIRE(output.good());
		output << R"(
local pixels = Instance.new("EditableImage")
pixels:Resize(8, 8)
pixels.Parent = workspace
local graph = Instance.new("ImageGraph")
graph.InstanceKey = "buffer-source"
graph.Graph = "buffer.aimagegraph"
graph.Parent = workspace
graph:SetInput("pixels", pixels.ContentId)
local screen = Instance.new("ScreenGui")
screen.Parent = game:GetService("Players").LocalPlayer:FindFirstChild("PlayerGui")
local label = Instance.new("ImageLabel")
label.Position = UDim2.new(0, 20, 0, 20)
label.Size = UDim2.new(0, 48, 0, 48)
label.BackgroundTransparency = 1
label.BorderSizePixel = 0
label.Image = graph:GetImage("image")
label.Parent = screen
local marker = Instance.new("Frame")
marker.Position = UDim2.new(0, 80, 0, 20)
marker.Size = UDim2.new(0, 32, 0, 32)
marker.BorderSizePixel = 0
marker.Parent = screen
local function paint(red, green, blue)
    local bytes = buffer.create(8 * 8 * 4)
    for offset = 0, buffer.len(bytes) - 4, 4 do
        buffer.writeu8(bytes, offset, red)
        buffer.writeu8(bytes, offset + 1, green)
        buffer.writeu8(bytes, offset + 2, blue)
        buffer.writeu8(bytes, offset + 3, 255)
    end
    assert(pixels:FromBuffer(bytes))
    marker.BackgroundColor3 = Color3.fromRGB(red, green, blue)
end
paint(0, 255, 255)
local part = Instance.new("Part")
part.Anchored = true
part.Transparency = 1
part.CFrame = CFrame.new(2.5, 0, 0)
part.Parent = workspace
local emitter = Instance.new("ParticleEmitter")
emitter.Texture = graph:GetImage("image")
emitter.Rate = 60
emitter.Lifetime = NumberRange.new(10)
emitter.Speed = NumberRange.new(0)
emitter.Size = NumberSequence.new(1.5)
emitter.Color = ColorSequence.new(Color3.new(1, 0, 1))
emitter.Parent = part
local camera = Instance.new("Camera")
camera.FieldOfView = 60
camera.CFrame = CFrame.lookAt(Vector3.new(0, 0, 8), Vector3.new(0, 0, 0))
camera.Parent = workspace
workspace.CurrentCamera = camera
local beat = 0
game:GetService("RunService").Heartbeat:Connect(function()
    beat += 1
    if beat == 25 then paint(255, 255, 0) end
end)
)";
	}
	client::Options options;
	options.Headless = true;
	options.Width = WIDTH;
	options.Height = HEIGHT;
	options.MaximumFrames = 70;
	options.MaximumFrameRate = 60;
	options.Uncapped = true;
	options.ScriptPath = script.string();
	options.ContentSources = {"dir:" + (root / "store").string()};
	options.ContentPublisherKey = key->Public().ToHex();
	options.CaptureSequence = root / "frames";
	client::Client player;
	REQUIRE(player.Initialise(options));
	REQUIRE(player.Run() == 0);
	bool sawCyan = false, sawYellow = false;
	// The marker changes in the same callback as the buffer. Compare every frame,
	// rather than assuming a simulation tick and capture index are identical.
	for (int frame = 15; frame < 65; ++frame) {
		CAPTURE(frame);
		const auto surface = ReadFrame(options.CaptureSequence, frame);
		Uint8 red = 0, green = 0, blue = 0, alpha = 0;
		REQUIRE(SDL_ReadSurfacePixel(surface.get(), 90, 30, &red, &green, &blue, &alpha));
		REQUIRE(green > 240);
		REQUIRE(((red < 10 && blue > 240) || (red > 240 && blue < 10)));
		const bool yellow = red > 240;
		sawYellow |= yellow;
		sawCyan |= !yellow;
		CHECK(
			CountPixels(surface.get(), yellow ? PixelColour::Yellow : PixelColour::Cyan, 20, 20, 68, 68) >
			2000
		);
		CHECK(
			CountPixels(
				surface.get(),
				yellow ? PixelColour::Red : PixelColour::Blue,
				WIDTH / 2,
				0,
				WIDTH,
				HEIGHT * 2 / 3
			) > 100
		);
	}
	CHECK(sawCyan);
	CHECK(sawYellow);
}
