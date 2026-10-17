// The real --game path owns its initial local character and camera follow.

#include <engine/core/Paths.hpp>
#include <engine/examples/DemosLoader.hpp>
#include <engine/testing/Suite.hpp>

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>

#include <client/Client.hpp>
#include <client/Options.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>

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
