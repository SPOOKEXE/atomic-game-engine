// The second hallway demo has a short shell whose entrance opens into a long
// isolated corridor. Render its entry in small forward and reverse steps so a
// portal body and the eye cannot disappear at the seam.

#include <engine/core/Paths.hpp>
#include <engine/testing/Suite.hpp>

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>

#include <client/Client.hpp>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

TEST_SUITE_ID("client.tunnels.render")
TEST_DEPENDS("client.scene.tick")
TEST_DEPENDS("engine.examples.scene")

namespace {

	using namespace engine;

	constexpr int WIDTH = 160;
	constexpr int HEIGHT = 120;
	constexpr int FRAMES = 22;

	std::string ReadText(const std::filesystem::path &path) {
		std::ifstream input(path);
		REQUIRE(input);
		return {std::istreambuf_iterator<char>(input), {}};
	}

	void WriteEntrySweep(const std::filesystem::path &path) {
		std::string source =
			ReadText(core::Paths::Base().parent_path() / "assets/examples/scripts/Tunnels.luau");
		const size_t cameras = source.find("local cameras: { [string]: Camera } = {}");
		const size_t viewSelection = source.find("if WHICH == \"long\" then", cameras);
		REQUIRE(cameras != std::string::npos);
		REQUIRE(viewSelection != std::string::npos);
		// The checked world has no authored `CurrentCamera`; remove its optional
		// still cameras as well, so the client installs its default follow eye.
		source.replace(cameras, viewSelection - cameras, "local cameras: { [string]: Camera } = {}\n\n");
		source += R"(

-- Test capture: use the actual local player and its default follow camera.
-- That is the Studio path, unlike an authored fixed camera or an extra player.
local captureStep = 0
local capturePlayer = assert(Players.LocalPlayer, "local player must exist before the capture script")
local captureCharacter = assert(capturePlayer:LoadCharacter(), "local player must load a character")
local captureRoot = assert(captureCharacter:FindFirstChild("HumanoidRootPart") :: BasePart?)
local captureTorso = assert(captureCharacter:FindFirstChild("Torso") :: BasePart?)
local captureHumanoid = assert(captureCharacter:FindFirstChild("Humanoid") :: Humanoid?)
	captureTorso.Color = Color3.new(1, 0, 1)
RunService.Heartbeat:Connect(function(_deltaTime: number)
	captureTorso.Color = Color3.new(1, 0, 1)
	local phase = captureStep % 22
	local z = if phase <= 10 then 1.5 - phase * 0.1 else 0.5 + (phase - 10) * 0.1
	-- Continue the same authored walk in the far chart after it crosses. Writing
	-- the near coordinate every frame would teleport the real body through the
	-- pane repeatedly, which is not a player walking through it.
	local far = z < 1.125
	local physicalZ = if far then z + 11.75 else z
	local physicalX = if far then 54 else SHORT_X
	captureRoot.CFrame = CFrame.lookAt(
		Vector3.new(physicalX, 3, physicalZ),
		Vector3.new(physicalX, 3, physicalZ - (if phase <= 10 then 1 else -1))
	)
	captureHumanoid.MoveDirection = Vector3.new(0, 0, if phase <= 10 then -1 else 1)
	captureStep += 1
end)
)";
		std::ofstream output(path);
		REQUIRE(output);
		output << source;
		REQUIRE(output);
	}

	struct Pixels {
		size_t Visible = 0;
		size_t Magenta = 0;
	};

	Pixels VisiblePixels(SDL_Surface *image) {
		Pixels pixels;
		for (int y = 0; y < image->h; ++y) {
			for (int x = 0; x < image->w; ++x) {
				Uint8 red = 0, green = 0, blue = 0, alpha = 0;
				REQUIRE(SDL_ReadSurfacePixel(image, x, y, &red, &green, &blue, &alpha));
				pixels.Visible += red != 0 || green != 0 || blue != 0;
				pixels.Magenta += red > 70 && green < 90 && blue > 70;
			}
		}
		return pixels;
	}
}

TEST_CASE(
	"the short tunnel keeps its character and camera visible while crossing its entry",
	"[client][gpu][portal][tunnels][.]"
) {
	const auto root = core::Paths::Base() / "tunnels-entry-sweep";
	std::filesystem::remove_all(root);
	std::filesystem::create_directories(root);
	const auto script = root / "Tunnels-entry-sweep.luau";
	const auto captures = root / "captures";
	WriteEntrySweep(script);

	client::Options options;
	options.Headless = true;
	options.Width = WIDTH;
	options.Height = HEIGHT;
	options.MaximumFrames = FRAMES;
	options.MaximumFrameRate = 60;
	options.Uncapped = true;
	options.ScriptPath = script.string();
	options.CaptureSequence = captures;
	client::Client client;
	REQUIRE(client.Initialise(options));
	CHECK(client.Run() == 0);

	size_t tenthStudSteps = 0;
	size_t exteriorEyes = 0;
	size_t interiorEyes = 0;
	size_t enteredInterior = 0;
	size_t returnedExterior = 0;
	std::vector<core::Vector3> eyes;
	for (int frame = 0; frame < FRAMES; ++frame) {
		const auto stem = captures / std::to_string(frame);
		std::ifstream metadata(stem.string() + ".json");
		REQUIRE(metadata);
		const nlohmann::json sample = nlohmann::json::parse(metadata);
		CHECK(sample.at("frame") == frame);
		CHECK(sample.at("instances").get<size_t>() > 20);

		const auto &position = sample.at("camera").at("position");
		eyes.push_back({position.at(0), position.at(1), position.at(2)});
		const bool exterior = std::abs(position.at(0).get<float>() - 20.0f) < .01f;
		const bool interior = std::abs(position.at(0).get<float>() - 54.0f) < .01f;
		exteriorEyes += exterior;
		interiorEyes += interior;
		CHECK((exterior || interior));
		if (frame > 0) {
			const bool previousExterior = std::abs(eyes[eyes.size() - 2].X - 20.0f) < .01f;
			enteredInterior += previousExterior && interior;
			returnedExterior += !previousExterior && exterior;
		}
		const auto imagePath = stem.string() + ".bmp";
		std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> image(
			SDL_LoadBMP(imagePath.c_str()), SDL_DestroySurface
		);
		CAPTURE(frame, imagePath);
		REQUIRE(image);
		const Pixels pixels = VisiblePixels(image.get());
		CHECK(pixels.Visible > static_cast<size_t>(WIDTH * HEIGHT) / 8);
		CHECK(pixels.Magenta > 0);
	}
	for (size_t index = 1; index < eyes.size(); ++index) {
		const float step = (eyes[index] - eyes[index - 1]).Magnitude();
		if (std::abs(step - 0.1f) < 0.025f) tenthStudSteps++;
	}

	// The captured source owns eleven tenth-stud samples across the one-stud
	// seam span in each direction. Retain the image sequence at `captures` for
	// the visual review that complements these pixel checks.
	CHECK(tenthStudSteps >= 16);
	CHECK(exteriorEyes > 0);
	CHECK(interiorEyes > 0);
	CHECK(enteredInterior == 1);
	CHECK(returnedExterior == 1);
}
