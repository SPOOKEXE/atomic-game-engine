// The real --game path owns its initial local character and camera follow.

#include "FrameObservation.hpp"
#include "ImageChecks.hpp"
#include "ScenarioChecks.hpp"

#include <engine/core/HeapProfile.hpp>
#include <engine/core/Paths.hpp>
#include <engine/examples/DemosLoader.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <client/Client.hpp>
#include <client/Options.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("client.character-startup.render")
TEST_DEPENDS("client.scene.tick")
TEST_DEPENDS("engine.testing.scenario")
TEST_DEPENDS("client.imagechecks")
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
		std::vector<std::array<uint8_t, 3>> Pixels;
		size_t NonBlack = 0;
	};

	GlyphCrop ReadGlyphCrop(const render::SceneCaptureResult &image) {
		if (!client::tests::CanonicalImage(image) || image.Width != 320 || image.Height != 160)
			throw std::runtime_error("font capture must be a complete 320x160 canonical RGBA image");
		constexpr size_t CROP_LEFT = 8, CROP_TOP = 8;
		constexpr size_t CROP_WIDTH = 304, CROP_HEIGHT = 80;
		GlyphCrop crop;
		crop.Pixels.resize(CROP_WIDTH * CROP_HEIGHT);
		for (size_t y = CROP_TOP; y < CROP_TOP + CROP_HEIGHT; y++) {
			for (size_t x = CROP_LEFT; x < CROP_LEFT + CROP_WIDTH; x++) {
				const auto *pixel = image.Pixels.data() + y * image.RowStride + x * 4;
				crop.Pixels[(y - CROP_TOP) * CROP_WIDTH + x - CROP_LEFT] = {pixel[0], pixel[1], pixel[2]};
				crop.NonBlack += pixel[0] > 8 || pixel[1] > 8 || pixel[2] > 8;
			}
		}
		return crop;
	}

	struct FinalFrameFacts {
		uint64_t ClientFrame = 0;
		uint64_t MetadataFrame = 0;
		bool GenerationMatches = false;
		bool WorldMatches = false;
		bool Canonical = false;
		uint32_t Width = 0;
		uint32_t Height = 0;
		size_t MatchingProbePixels = 0;
		GlyphCrop Crop;
	};

	FinalFrameFacts CopyFinalFrame(
		const client::FrameObservation &frame, uint64_t generation, std::optional<StartupCase> startup
	) {
		FinalFrameFacts facts;
		facts.ClientFrame = frame.ClientFrame;
		facts.MetadataFrame = frame.Metadata.at("frame").get<uint64_t>();
		facts.GenerationMatches = frame.Generation == generation && generation != 0;
		facts.WorldMatches = frame.Image.WorldName == frame.Metadata.at("view_world").get<std::string>();
		facts.Canonical = client::tests::CanonicalImage(frame.Image);
		facts.Width = frame.Image.Width;
		facts.Height = frame.Image.Height;
		if (startup) {
			const auto isSuccess = [scenario = *startup](uint8_t red, uint8_t green, uint8_t blue) {
				if (scenario == StartupCase::Disabled) return blue > 140 && red < 80 && green < 100;
				return green > 140 && red < 80 && blue < 100;
			};
			facts.MatchingProbePixels =
				client::tests::CountPixels(
					frame.Image, {0, 0, frame.Image.Width, frame.Image.Height}, isSuccess
				)
					.Matching;
		} else
			facts.Crop = ReadGlyphCrop(frame.Image);
		return facts;
	}

	std::vector<testing::ScenarioCheck<FinalFrameFacts>> FinalFrameChecks(bool startup) {
		std::vector<testing::ScenarioCheck<FinalFrameFacts>> checks;
		checks.push_back(
			{"capture identity",
			 "client.character-startup.render",
			 {{{"final-frame"}}},
			 [startup](const auto &observation, testing::CheckResult &result) {
				 const auto &facts = observation.Payload;
				 result.Record("native frame 39", facts.ClientFrame == 39, std::to_string(facts.ClientFrame));
				 result.Record(
					 "metadata frame 39", facts.MetadataFrame == 39, std::to_string(facts.MetadataFrame)
				 );
				 result.Record("subscription generation", facts.GenerationMatches);
				 result.Record("native world provenance", facts.WorldMatches);
				 result.Record("canonical image readable", facts.Canonical);
				 result.Record("image width", facts.Width == (startup ? 160u : 320u));
				 result.Record("image height", facts.Height == (startup ? 120u : 160u));
			 },
			 {}}
		);
		checks.push_back(
			{startup ? "startup probe colour" : "glyph crop",
			 "client.character-startup.render",
			 {{{"final-frame"}}},
			 [startup](const auto &observation, testing::CheckResult &result) {
				 const auto &facts = observation.Payload;
				 if (startup)
					 result.Record(
						 "positive success colour covers more than half image",
						 facts.MatchingProbePixels > size_t(facts.Width) * facts.Height / 2,
						 std::to_string(facts.MatchingProbePixels)
					 );
				 else
					 result.Record(
						 "glyph crop contains more than 100 nonblack pixels",
						 facts.Crop.NonBlack > 100,
						 std::to_string(facts.Crop.NonBlack)
					 );
			 },
			 {}}
		);
		return checks;
	}

	testing::ScenarioKey FinalFrameKey(std::string scene, const client::Options &options) {
		testing::ScenarioKey key;
		key.Scene = std::move(scene);
		key.ContentRevision = "staged-examples-fonts-and-authored-probe";
		key.Configuration = "headless;uncapped;width=" + std::to_string(options.Width) +
							";height=" + std::to_string(options.Height) +
							";maximum-frame-rate=" + std::to_string(options.MaximumFrameRate);
		key.TickSchedule = "native-startup;40@float(1/60)";
		key.FrameSchedule = "40 capture-paced frames;final=39";
		key.Actions = options.GameFile.empty() ? options.ScriptPath : options.GameFile.string();
		key.CaptureConfiguration = "native-final-scene;canonicalRGBA;frame=39";
		key.ObservationSchema = "client.character-final-frame.v1";
		key.CheckSet = options.GameFile.empty() ? "capture-identity;glyph-crop.v1"
												: "capture-identity;startup-colour.v1";
		return key;
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
		client::Options options;
		options.Headless = true;
		options.Width = 160;
		options.Height = 120;
		options.MaximumFrames = 40;
		options.Uncapped = true;
		options.MaximumFrameRate = 60;
		options.GameFile = game;
		testing::ScenarioSession<FinalFrameFacts> session(
			FinalFrameKey(game.stem().string(), options), FinalFrameChecks(true)
		);
		testing::ScenarioDriver<FinalFrameFacts> driver;
		uint64_t captures = 0;
		driver.Run = [&](auto &owner) {
			client::Client player;
			uint64_t generation = 0;
			client::FrameObservationOptions captureOptions;
			captureOptions.CaptureFrames = {39};
			auto lease = client::SubscribeClientFrames(player, captureOptions, [&](auto frame) {
				auto facts = CopyFinalFrame(*frame, generation, scenario);
				owner.Publish({"final-frame", frame->ClientFrame, std::move(facts), sizeof(FinalFrameFacts)});
			});
			if (!lease) throw std::runtime_error(lease.Failure());
			generation = lease.Generation();
			if (!player.Initialise(options)) throw std::runtime_error("startup client initialization failed");
			if (player.Run() != 0) throw std::runtime_error("startup client run failed");
			player.Shutdown();
			if (!lease.Finish()) throw std::runtime_error(lease.Failure());
			captures = lease.CapturedFrames();
			if (lease.OwnedBytes() != 0)
				throw std::runtime_error("startup image retained after joined checks");
		};
		const auto report = session.Run(driver);
		for (const auto &check : report.Checks)
			client::tests::CheckScenario(report, check.Name);
		CHECK(captures == 1);
		CHECK(report.Metrics.SetupCount == 1);
		CHECK(report.Metrics.PublishedSamples == 1);
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
	const uint64_t fontPayloadBytes = BundledFontPayloadBytes();
	const bool heapProfiled = core::HeapProfile::IsCompiledIn();
	struct FontRunHeap {
		FontPackageHeap Before;
		FontPackageHeap AfterInitialise;
		FontPackageHeap AfterRun;
		FontPackageHeap AfterShutdown;
	};
	auto run = [&](const std::filesystem::path &script, GlyphCrop *crop) {
		FontRunHeap heap;
		heap.Before = ReadFontPackageHeap();
		client::Client player;
		client::Options options;
		options.Headless = true;
		options.Width = 320;
		options.Height = 160;
		options.MaximumFrames = 40;
		// Observed text runs retain fixed 1/60 intervals without a wall deadline.
		options.MaximumFrameRate = crop == nullptr ? 60 : 0;
		options.Uncapped = true;
		options.ScriptPath = script.string();
		if (crop == nullptr) {
			// Empty runs retain wall pacing and deliberately never subscribe to capture.
			REQUIRE(player.Initialise(options));
			heap.AfterInitialise = ReadFontPackageHeap();
			REQUIRE(player.Run() == 0);
			heap.AfterRun = ReadFontPackageHeap();
			player.Shutdown();
			heap.AfterShutdown = ReadFontPackageHeap();
			return heap;
		}
		testing::ScenarioSession<FinalFrameFacts> session(
			FinalFrameKey(script.stem().string(), options), FinalFrameChecks(false)
		);
		testing::ScenarioDriver<FinalFrameFacts> driver;
		uint64_t captures = 0;
		driver.Run = [&](auto &owner) {
			uint64_t generation = 0;
			client::FrameObservationOptions captureOptions;
			captureOptions.CaptureFrames = {39};
			auto lease = client::SubscribeClientFrames(player, captureOptions, [&](auto frame) {
				auto facts = CopyFinalFrame(*frame, generation, std::nullopt);
				*crop = facts.Crop;
				const size_t bytes =
					sizeof(FinalFrameFacts) + facts.Crop.Pixels.capacity() * sizeof(facts.Crop.Pixels[0]);
				owner.Publish({"final-frame", frame->ClientFrame, std::move(facts), bytes});
			});
			if (!lease) throw std::runtime_error(lease.Failure());
			generation = lease.Generation();
			if (!player.Initialise(options)) throw std::runtime_error("font client initialization failed");
			heap.AfterInitialise = ReadFontPackageHeap();
			if (player.Run() != 0) throw std::runtime_error("font client run failed");
			heap.AfterRun = ReadFontPackageHeap();
			player.Shutdown();
			heap.AfterShutdown = ReadFontPackageHeap();
			if (!lease.Finish()) throw std::runtime_error(lease.Failure());
			captures = lease.CapturedFrames();
			if (lease.OwnedBytes() != 0) throw std::runtime_error("font image retained after crop copy");
		};
		const auto report = session.Run(driver);
		for (const auto &check : report.Checks)
			client::tests::CheckScenario(report, check.Name);
		CHECK(captures == 1);
		CHECK(report.Metrics.SetupCount == 1);
		CHECK(report.Metrics.PublishedSamples == 1);

		return heap;
	};
	auto checkEmptyRun = [&](const FontRunHeap &heap) {
		if (!heapProfiled) return;
		CHECK(heap.AfterInitialise.TotalBytes == heap.Before.TotalBytes);
		CHECK(heap.AfterInitialise.LiveBytes == heap.Before.LiveBytes);
		CHECK(heap.AfterRun.TotalBytes == heap.Before.TotalBytes);
		CHECK(heap.AfterRun.LiveBytes == heap.Before.LiveBytes);
	};

	checkEmptyRun(run(empty, nullptr));
	GlyphCrop immediateCrop;
	const FontRunHeap immediateHeap = run(immediate, &immediateCrop);
	const FontPackageHeap &beforeImmediate = immediateHeap.Before;
	const FontPackageHeap &afterImmediate = immediateHeap.AfterRun;
	if (heapProfiled) {
		CHECK(afterImmediate.TotalBytes - beforeImmediate.TotalBytes < 3 * fontPayloadBytes);
		CHECK(afterImmediate.LiveBytes - beforeImmediate.LiveBytes >= static_cast<int64_t>(fontPayloadBytes));
	}
	CHECK(immediateCrop.NonBlack > 100);
	if (heapProfiled)
		CHECK(
			afterImmediate.LiveBytes - immediateHeap.AfterShutdown.LiveBytes >=
			static_cast<int64_t>(fontPayloadBytes)
		);

	GlyphCrop delayedCrop;
	const FontRunHeap delayedHeap = run(delayed, &delayedCrop);
	const FontPackageHeap &beforeDelayed = delayedHeap.Before;
	const FontPackageHeap &afterDelayed = delayedHeap.AfterRun;
	if (heapProfiled) {
		CHECK(afterDelayed.TotalBytes - beforeDelayed.TotalBytes < 3 * fontPayloadBytes);
		CHECK(afterDelayed.LiveBytes - beforeDelayed.LiveBytes >= static_cast<int64_t>(fontPayloadBytes));
	}
	CHECK(delayedCrop.NonBlack > 100);
	CHECK(delayedCrop.Pixels == immediateCrop.Pixels);
	if (heapProfiled)
		CHECK(
			afterDelayed.LiveBytes - delayedHeap.AfterShutdown.LiveBytes >=
			static_cast<int64_t>(fontPayloadBytes)
		);

	checkEmptyRun(run(empty, nullptr));
}
