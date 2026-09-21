// `engine.examples.noneuclidean` proves every authored shot reaches the right
// seam. This product-level companion renders the representative hard shots.
// Unit fixtures own scaled seam math and character crossings; this test keeps
// the full client, its renderer and the authored geometry in the same path.

#include <engine/core/Paths.hpp>
#include <engine/testing/Suite.hpp>

#include <SDL3/SDL.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <client/Client.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

TEST_SUITE_ID("client.noneuclidean.render")
TEST_DEPENDS("client.scene.tick")
TEST_DEPENDS("engine.examples.noneuclidean")
TEST_DEPENDS("engine.render.portalcharacterfixtures")

namespace {

	using namespace engine;

	struct Shot {
		const char *Name;
		int Sample;
	};

	// The last nine tour shots are the authored front, back, rolled and near-plane
	// cases. Samples 8 and 9 are the two nearly edge-on azimuths in its main grid.
	constexpr std::array SHOTS{
		Shot{"grazing-positive", 8},
		Shot{"grazing-negative", 9},
		Shot{"front", 36},
		Shot{"back", 37},
		Shot{"rolled", 38},
		Shot{"near-positive", 39},
		Shot{"near-negative", 40},
	};
	constexpr int WIDTH = 256;
	constexpr int HEIGHT = 192;
	constexpr int FRAMES = 16;

	std::string ReadText(const std::filesystem::path &path) {
		std::ifstream input(path);
		REQUIRE(input);
		return {std::istreambuf_iterator<char>(input), {}};
	}

	void WriteAngleTour(const std::filesystem::path &path, int sample) {
		std::string source =
			ReadText(core::Paths::Base().parent_path() / "assets/examples/scripts/NonEuclidean.luau");
		const auto mode = source.find("view:SetAttribute(\"TourMode\", \"overview\")");
		REQUIRE(mode != std::string::npos);
		source.replace(
			mode,
			std::string_view("view:SetAttribute(\"TourMode\", \"overview\")").size(),
			"view:SetAttribute(\"TourMode\", \"angles\")"
		);
		const auto selected = source.find("view:SetAttribute(\"TourSample\", -1)");
		REQUIRE(selected != std::string::npos);
		source.replace(
			selected,
			std::string_view("view:SetAttribute(\"TourSample\", -1)").size(),
			"view:SetAttribute(\"TourSample\", " + std::to_string(sample) + ")"
		);
		std::ofstream output(path);
		REQUIRE(output);
		output << source;
		REQUIRE(output);
	}

	uint64_t CheckCapture(const std::filesystem::path &captureDirectory, const Shot &shot) {
		const auto metadataPath = captureDirectory / (std::to_string(FRAMES - 1) + ".json");
		std::ifstream metadataFile(metadataPath);
		REQUIRE(metadataFile);
		const nlohmann::json metadata = nlohmann::json::parse(metadataFile);
		CAPTURE(shot.Name, metadataPath.string());
		CHECK(metadata.at("frame") == FRAMES - 1);
		CHECK(metadata.at("field_of_view").get<float>() == Catch::Approx(1.2217305f));
		CHECK(metadata.at("portals") == 26);
		CHECK(metadata.at("instances").get<size_t>() > 90);

		const auto imagePath = captureDirectory / (std::to_string(FRAMES - 1) + ".bmp");
		std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> image(
			SDL_LoadBMP(imagePath.string().c_str()), SDL_DestroySurface
		);
		REQUIRE(image);
		CHECK(image->w == WIDTH);
		CHECK(image->h == HEIGHT);

		size_t visible = 0;
		size_t centralVisible = 0;
		uint64_t fingerprint = 1469598103934665603ULL;
		bool readable = true;
		for (int y = 0; y < image->h; ++y) {
			for (int x = 0; x < image->w; ++x) {
				Uint8 red = 0, green = 0, blue = 0, alpha = 0;
				readable &= SDL_ReadSurfacePixel(image.get(), x, y, &red, &green, &blue, &alpha);
				const bool nonBlack = red != 0 || green != 0 || blue != 0;
				visible += nonBlack;
				fingerprint = (fingerprint ^ red) * 1099511628211ULL;
				fingerprint = (fingerprint ^ green) * 1099511628211ULL;
				fingerprint = (fingerprint ^ blue) * 1099511628211ULL;
				if (x >= image->w / 4 && x < image->w * 3 / 4 && y >= image->h / 4 && y < image->h * 3 / 4)
					centralVisible += nonBlack;
			}
		}
		CAPTURE(imagePath.string(), visible, centralVisible, fingerprint);
		CHECK(readable);
		CHECK(visible > static_cast<size_t>(WIDTH * HEIGHT) / 4);
		CHECK(centralVisible > static_cast<size_t>(WIDTH * HEIGHT) / 20);
		return fingerprint;
	}
}

TEST_CASE(
	"the authored non-euclidean angle tour renders useful portal views",
	"[client][gpu][portal][non-euclidean][.]"
) {
	const auto root = core::Paths::Base() / "non-euclidean-render";
	std::filesystem::remove_all(root);
	std::filesystem::create_directories(root);
	std::vector<uint64_t> fingerprints;

	for (const Shot &shot : SHOTS) {
		CAPTURE(shot.Name, shot.Sample);
		const auto script = root / (std::string(shot.Name) + ".luau");
		const auto captureDirectory = root / shot.Name;
		WriteAngleTour(script, shot.Sample);

		client::Options options;
		options.Headless = true;
		options.Width = WIDTH;
		options.Height = HEIGHT;
		options.MaximumFrames = FRAMES;
		options.ScriptPath = script.string();
		options.CaptureSequence = captureDirectory;
		client::Client client;
		REQUIRE(client.Initialise(options));
		CHECK(client.Run() == 0);
		fingerprints.push_back(CheckCapture(captureDirectory, shot));
	}
	std::sort(fingerprints.begin(), fingerprints.end());
	CHECK(std::adjacent_find(fingerprints.begin(), fingerprints.end()) == fingerprints.end());
}
