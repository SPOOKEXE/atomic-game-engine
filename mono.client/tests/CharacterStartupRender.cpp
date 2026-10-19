// The real --game path owns its initial local character and camera follow.

#include <engine/core/HeapProfile.hpp>
#include <engine/core/Paths.hpp>
#include <engine/examples/DemosLoader.hpp>
#include <engine/testing/Suite.hpp>

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <client/Client.hpp>
#include <client/Options.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("client.character-startup.render")
TEST_DEPENDS("client.scene.tick")
TEST_DEPENDS("engine.examples.scene")

namespace {
	using namespace engine;

	enum class StartupCase {
		Automatic,
		Disabled,
		Preloaded,
	};

	struct TemporaryWorlds {
		std::filesystem::path Root = core::Paths::Base() / "character-startup-render";
		~TemporaryWorlds() {
			std::error_code ignored;
			std::filesystem::remove_all(Root, ignored);
		}
	};

	struct TemporaryAssetsOverride {
		std::filesystem::path Previous = core::Paths::Assets();
		TemporaryAssetsOverride() {
			core::Paths::SetAssetsOverride(core::Paths::Base());
		}
		~TemporaryAssetsOverride() {
			core::Paths::SetAssetsOverride(Previous);
		}
	};

	std::string ReadText(const std::filesystem::path &path) {
		std::ifstream input(path, std::ios::binary);
		REQUIRE(input);
		return {std::istreambuf_iterator<char>(input), {}};
	}

	void ReplaceOnce(std::string &text, std::string_view before, std::string_view after) {
		const size_t offset = text.find(before);
		REQUIRE(offset != std::string::npos);
		text.replace(offset, before.size(), after);
	}

	std::string ProbeSource(StartupCase scenario) {
		const std::string expected = scenario == StartupCase::Disabled	  ? "none"
									 : scenario == StartupCase::Preloaded ? "preloaded"
																		  : "automatic";
		return R"(
local Players = game:GetService("Players")
local RunService = game:GetService("RunService")
local player = assert(Players.LocalPlayer)
local camera = assert(workspace.CurrentCamera)
local playerGui = assert(player:WaitForChild("PlayerGui", 20))
local gui = Instance.new("ScreenGui")
gui.Name = "CharacterStartupProbe"
gui.DisplayOrder = 10000
gui.IgnoreGuiInset = true
gui.Parent = playerGui
local result = Instance.new("TextLabel")
result.Name = "Result"
result.Size = UDim2.new(1, 0, 1, 0)
result.BackgroundTransparency = 0
result.BackgroundColor3 = Color3.new(1, 0, 0)
result.BorderSizePixel = 0
result.Text = ""
result.Parent = gui

local scenario = ")" +
			   expected + R"("
local ticks = 0
local settled = false
local function finish(ok)
	if ok and scenario == "none" then
		result.BackgroundColor3 = Color3.new(0, 0, 1)
	else
		result.BackgroundColor3 = if ok then Color3.new(0, 1, 0) else Color3.new(1, 0, 0)
	end
	settled = true
end

RunService.Heartbeat:Connect(function()
	if settled then return end
	ticks += 1
	if scenario == "none" then
		if ticks >= 6 then finish(player.Character == nil) end
		return
	end
	if ticks < 2 then return end
	local character = player.Character
	local humanoid = character and character:FindFirstChildOfClass("Humanoid")
	local count = 0
	for _, child in workspace:GetChildren() do
		if (child.Name == player.Name or child.Name == "PreloadedCharacter") and
			child:FindFirstChildOfClass("Humanoid") ~= nil then
			count += 1
		end
	end
	local root = character and character:FindFirstChild("HumanoidRootPart")
	local distance = root and (camera.CFrame.Position - root.Position).Magnitude or math.huge
	local ok = character ~= nil and humanoid ~= nil and camera.CameraSubject == humanoid and count == 1
		and root ~= nil and root.Position.X > 20 and distance < 30
	if scenario == "preloaded" then
		ok = ok and character.Name == "PreloadedCharacter"
	end
	if ok then
		finish(true)
	elseif ticks >= 10 then
		finish(false)
	end
end)
)";
	}

	std::string ServerSource(StartupCase scenario) {
		std::string source = R"(
local Players = game:GetService("Players")
local player = assert(Players:GetPlayers()[1])
local function moveCharacter(character)
	local root = assert(character:WaitForChild("HumanoidRootPart", 20))
	root.CFrame = CFrame.new(25, 4, 8)
end
)";
		if (scenario == StartupCase::Disabled) source += "Players.CharacterAutoLoads = false\n";
		if (scenario == StartupCase::Preloaded) {
			source += R"(
local character = assert(player:LoadCharacter())
character.Name = "PreloadedCharacter"
moveCharacter(character)
)";
		}
		source += R"(
player.CharacterAdded:Connect(moveCharacter)
if player.Character ~= nil then moveCharacter(player.Character) end
)";
		return source;
	}

	std::filesystem::path WriteFixture(
		const std::filesystem::path &root, const std::filesystem::path &sourceWorld, StartupCase scenario
	) {
		std::string document = ReadText(sourceWorld);
		constexpr std::string_view probePath = "scripts/client/CharacterStartupProbe.client.luau";
		constexpr std::string_view serverPath = "scripts/server/CharacterAutoLoads.server.luau";
		const std::string sourceEntry = "\t\t<Source path=\"" + std::string(probePath) + "\"><![CDATA[" +
										ProbeSource(scenario) + "]]></Source>\n";
		ReplaceOnce(document, "\t</Sources>", sourceEntry + "\t</Sources>");

		const std::string localScript =
			"\t\t\t<Item class=\"LocalScript\" name=\"CharacterStartupProbe\" id=\"2001\">\n"
			"\t\t\t\t<Property name=\"Source\" type=\"string\">" +
			std::string(probePath) + "</Property>\n\t\t\t</Item>\n";
		ReplaceOnce(
			document,
			"\t\t\t</Item>\n\t\t</Item>\n\t</Item>\n\t<Item class=\"StarterGui\"",
			"\t\t\t</Item>\n" + localScript + "\t\t</Item>\n\t</Item>\n\t<Item class=\"StarterGui\""
		);

		const std::string serverSource = ServerSource(scenario);
		if (!serverSource.empty()) {
			const std::string serverEntry = "\t\t<Source path=\"" + std::string(serverPath) + "\"><![CDATA[" +
											serverSource + "]]></Source>\n";
			ReplaceOnce(document, "\t</Sources>", serverEntry + "\t</Sources>");
			const std::string serverScript =
				"\t\t<Item class=\"Script\" name=\"CharacterAutoLoadsProbe\" id=\"2002\">\n"
				"\t\t\t<Property name=\"Source\" type=\"string\">" +
				std::string(serverPath) + "</Property>\n\t\t</Item>\n";
			ReplaceOnce(document, "\t</Item>\n</World>", serverScript + "\t</Item>\n</World>");
		}

		std::filesystem::create_directories(root);
		const auto output = root / (scenario == StartupCase::Automatic	? "automatic.aworld"
									: scenario == StartupCase::Disabled ? "disabled.aworld"
																		: "preloaded.aworld");
		std::ofstream file(output, std::ios::binary);
		REQUIRE(file);
		file << document;
		REQUIRE(file.good());
		return output;
	}

	bool HasProbeColour(const std::filesystem::path &path, StartupCase scenario) {
		std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> image(
			SDL_LoadBMP(path.string().c_str()), SDL_DestroySurface
		);
		REQUIRE(image);
		const auto isSuccess = [scenario](Uint8 red, Uint8 green, Uint8 blue) {
			if (scenario == StartupCase::Disabled) return blue > 140 && red < 80 && green < 100;
			return green > 140 && red < 80 && blue < 100;
		};
		size_t matching = 0;
		for (int y = 0; y < image->h; ++y) {
			for (int x = 0; x < image->w; ++x) {
				Uint8 red = 0, green = 0, blue = 0, alpha = 0;
				REQUIRE(SDL_ReadSurfacePixel(image.get(), x, y, &red, &green, &blue, &alpha));
				matching += isSuccess(red, green, blue);
			}
		}
		return matching > static_cast<size_t>(image->w * image->h) / 2;
	}

	std::filesystem::path
	WriteScript(const std::filesystem::path &root, std::string_view name, std::string_view source) {
		std::filesystem::create_directories(root);
		const auto path = root / std::string(name);
		std::ofstream output(path, std::ios::binary);
		REQUIRE(output);
		output << source;
		REQUIRE(output.good());
		return path;
	}

	std::string FontDemandSource(bool delayed) {
		std::string source = R"(
local Players = game:GetService("Players")
local player = assert(Players.LocalPlayer)
local screen = Instance.new("ScreenGui")
screen.Name = "FontDemand"
screen.IgnoreGuiInset = true
screen.DisplayOrder = 10000
screen.Parent = player:WaitForChild("PlayerGui")

local background = Instance.new("Frame")
background.Size = UDim2.new(1, 0, 1, 0)
background.BackgroundColor3 = Color3.new(0, 0, 0)
background.BorderSizePixel = 0
background.ZIndex = 1
background.Parent = screen

local function authorText()
	local label = Instance.new("TextLabel")
	label.Name = "FontDemandText"
	label.Position = UDim2.new(0, 12, 0, 12)
	label.Size = UDim2.new(0, 296, 0, 64)
	label.BackgroundTransparency = 1
	label.BorderSizePixel = 0
	label.Text = "office AV café"
	label.TextColor3 = Color3.new(1, 1, 1)
	label.TextSize = 28
	label.TextScaled = false
	label.TextWrapped = false
	label.TextTransparency = 0
	label.RichText = false
	label.TextXAlignment = Enum.TextXAlignment.Left
	label.TextYAlignment = Enum.TextYAlignment.Top
	label.ZIndex = 2
	label.Parent = screen
end
)";
		if (!delayed) {
			source += "\nauthorText()\n";
			return source;
		}
		source += R"(
local RunService = game:GetService("RunService")
local ticks = 0
local connection
connection = RunService.Heartbeat:Connect(function()
	ticks += 1
	if ticks >= 10 then
		connection:Disconnect()
		authorText()
	end
end)
)";
		return source;
	}

	struct FontPackageHeap {
		uint64_t TotalBytes = 0;
		int64_t LiveBytes = 0;
	};

	FontPackageHeap ReadFontPackageHeap() {
		FontPackageHeap result;
		for (uint32_t index = 0; index < core::HeapProfile::NodeCount(); index++) {
			const core::HeapNodeView node = core::HeapProfile::Node(index);
			if (node.Name != "client.font package") continue;
			result.TotalBytes += node.TotalBytes;
			result.LiveBytes += node.LiveBytes;
		}
		return result;
	}

	uint64_t BundledFontPayloadBytes() {
		uint64_t total = 0;
		for (const char *file : {"Inter.ttf", "Roboto.ttf", "NotoSans.ttf", "JetBrainsMono.ttf"}) {
			const auto path = core::Paths::Fonts() / file;
			CAPTURE(path.string());
			REQUIRE(std::filesystem::is_regular_file(path));
			total += static_cast<uint64_t>(std::filesystem::file_size(path));
		}
		return total;
	}

	struct GlyphCrop {
		std::vector<std::array<Uint8, 3>> Pixels;
		size_t NonBlack = 0;
	};

	GlyphCrop ReadGlyphCrop(const std::filesystem::path &path) {
		std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> image(
			SDL_LoadBMP(path.string().c_str()), SDL_DestroySurface
		);
		REQUIRE(image);
		REQUIRE(image->w == 320);
		REQUIRE(image->h == 160);
		// Decode once into byte-ordered RGBA; BMP channel packing is platform-specific.
		std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> rgba(
			SDL_ConvertSurface(image.get(), SDL_PIXELFORMAT_RGBA32), SDL_DestroySurface
		);
		REQUIRE(rgba);
		REQUIRE(rgba->format == SDL_PIXELFORMAT_RGBA32);
		REQUIRE(rgba->pixels != nullptr);
		const std::span pixels(
			static_cast<const Uint8 *>(rgba->pixels), size_t(rgba->pitch) * size_t(rgba->h)
		);
		constexpr size_t CROP_LEFT = 8, CROP_TOP = 8;
		constexpr size_t CROP_WIDTH = 304, CROP_HEIGHT = 80;
		GlyphCrop crop;
		crop.Pixels.resize(CROP_WIDTH * CROP_HEIGHT);
		for (size_t y = CROP_TOP; y < CROP_TOP + CROP_HEIGHT; y++) {
			for (size_t x = CROP_LEFT; x < CROP_LEFT + CROP_WIDTH; x++) {
				const auto pixel = pixels.subspan(y * size_t(rgba->pitch) + x * 4, 3);
				crop.Pixels[(y - CROP_TOP) * CROP_WIDTH + x - CROP_LEFT] = {pixel[0], pixel[1], pixel[2]};
				crop.NonBlack += pixel[0] > 8 || pixel[1] > 8 || pixel[2] > 8;
			}
		}
		return crop;
	}
}

TEST_CASE(
	"headless --game startup respects automatic character loading and follows the resulting character",
	"[client][gpu][camera][character][.]"
) {
	const auto demo = examples::DemosLoader().Find(examples::DemoKind::World, "BladeborneDemo.aworld");
	REQUIRE(demo.has_value());

	TemporaryWorlds temporary;
	std::filesystem::remove_all(temporary.Root);
	for (const StartupCase scenario :
		 {StartupCase::Automatic, StartupCase::Disabled, StartupCase::Preloaded}) {
		CAPTURE(static_cast<int>(scenario));
		const auto game = WriteFixture(temporary.Root, demo->Path, scenario);
		const auto captures = temporary.Root / "captures" / game.stem();
		client::Options options;
		options.Headless = true;
		options.Width = 160;
		options.Height = 120;
		options.MaximumFrames = 40;
		options.Uncapped = true;
		options.MaximumFrameRate = 60;
		options.GameFile = game;
		options.CaptureSequence = captures;
		client::Client player;
		REQUIRE(player.Initialise(options));
		REQUIRE(player.Run() == 0);
		CHECK(HasProbeColour(captures / "39.bmp", scenario));
	}
}

TEST_CASE(
	"bundled GUI fonts load on text demand and render the same after delayed authoring",
	"[client][gpu][interface][.]"
) {
	TemporaryWorlds temporary;
	TemporaryAssetsOverride assets;
	std::filesystem::remove_all(temporary.Root);
	const auto scripts = temporary.Root / "scripts";
	const auto empty = WriteScript(scripts, "empty.luau", "\n");
	const auto immediate = WriteScript(scripts, "immediate.luau", FontDemandSource(false));
	const auto delayed = WriteScript(scripts, "delayed.luau", FontDemandSource(true));
	const auto immediateCaptures = temporary.Root / "immediate-captures";
	const auto delayedCaptures = temporary.Root / "delayed-captures";
	const uint64_t fontPayloadBytes = BundledFontPayloadBytes();
	const bool heapProfiled = core::HeapProfile::IsCompiledIn();
	struct FontRunHeap {
		FontPackageHeap Before;
		FontPackageHeap AfterInitialise;
		FontPackageHeap AfterRun;
		FontPackageHeap AfterShutdown;
	};
	auto run = [&](const std::filesystem::path &script, const std::filesystem::path &captures) {
		FontRunHeap heap;
		heap.Before = ReadFontPackageHeap();
		client::Client player;
		client::Options options;
		options.Headless = true;
		options.Width = 320;
		options.Height = 160;
		options.MaximumFrames = 40;
		// Capture sequences retain fixed 1/60 simulation intervals without a wall deadline.
		options.MaximumFrameRate = captures.empty() ? 60 : 0;
		options.Uncapped = true;
		options.ScriptPath = script.string();
		options.CaptureSequence = captures;
		REQUIRE(player.Initialise(options));
		heap.AfterInitialise = ReadFontPackageHeap();
		REQUIRE(player.Run() == 0);
		heap.AfterRun = ReadFontPackageHeap();
		player.Shutdown();
		heap.AfterShutdown = ReadFontPackageHeap();
		return heap;
	};
	auto checkEmptyRun = [&](const FontRunHeap &heap) {
		if (!heapProfiled) return;
		CHECK(heap.AfterInitialise.TotalBytes == heap.Before.TotalBytes);
		CHECK(heap.AfterInitialise.LiveBytes == heap.Before.LiveBytes);
		CHECK(heap.AfterRun.TotalBytes == heap.Before.TotalBytes);
		CHECK(heap.AfterRun.LiveBytes == heap.Before.LiveBytes);
	};

	checkEmptyRun(run(empty, {}));
	const FontRunHeap immediateHeap = run(immediate, immediateCaptures);
	const FontPackageHeap &beforeImmediate = immediateHeap.Before;
	const FontPackageHeap &afterImmediate = immediateHeap.AfterRun;
	if (heapProfiled) {
		CHECK(afterImmediate.TotalBytes - beforeImmediate.TotalBytes < 3 * fontPayloadBytes);
		CHECK(afterImmediate.LiveBytes - beforeImmediate.LiveBytes >= static_cast<int64_t>(fontPayloadBytes));
	}
	const GlyphCrop immediateCrop = ReadGlyphCrop(immediateCaptures / "39.bmp");
	CHECK(immediateCrop.NonBlack > 100);
	if (heapProfiled)
		CHECK(
			afterImmediate.LiveBytes - immediateHeap.AfterShutdown.LiveBytes >=
			static_cast<int64_t>(fontPayloadBytes)
		);

	const FontRunHeap delayedHeap = run(delayed, delayedCaptures);
	const FontPackageHeap &beforeDelayed = delayedHeap.Before;
	const FontPackageHeap &afterDelayed = delayedHeap.AfterRun;
	if (heapProfiled) {
		CHECK(afterDelayed.TotalBytes - beforeDelayed.TotalBytes < 3 * fontPayloadBytes);
		CHECK(afterDelayed.LiveBytes - beforeDelayed.LiveBytes >= static_cast<int64_t>(fontPayloadBytes));
	}
	const GlyphCrop delayedCrop = ReadGlyphCrop(delayedCaptures / "39.bmp");
	CHECK(delayedCrop.NonBlack > 100);
	CHECK(delayedCrop.Pixels == immediateCrop.Pixels);
	if (heapProfiled)
		CHECK(
			afterDelayed.LiveBytes - delayedHeap.AfterShutdown.LiveBytes >=
			static_cast<int64_t>(fontPayloadBytes)
		);

	checkEmptyRun(run(empty, {}));
}
