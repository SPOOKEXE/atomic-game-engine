// `engine.examples.noneuclidean` proves every authored shot reaches the right
// seam. This product-level companion renders the representative hard shots.
// Unit fixtures own scaled seam math and character crossings; this test keeps
// the full client, its renderer and the authored geometry in the same path.

#include "FrameObservation.hpp"
#include "ImageChecks.hpp"
#include "ScenarioChecks.hpp"

#include <engine/core/Paths.hpp>
#include <engine/testing/Suite.hpp>

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <client/Client.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("client.noneuclidean.render")
TEST_DEPENDS("client.scene.tick")
TEST_DEPENDS("engine.testing.scenario")
TEST_DEPENDS("client.imagechecks")
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

	void WriteAngleTour(const std::filesystem::path &builderPath, const std::filesystem::path &cameraPath) {
		const auto scripts = core::Paths::Base().parent_path() / "assets/examples/scripts";
		std::ofstream builderOutput(builderPath);
		REQUIRE(builderOutput);
		builderOutput << ReadText(scripts / "NonEuclidean.luau");
		REQUIRE(builderOutput);

		std::string source = ReadText(scripts / "client/DemoCameras/NonEuclidean.client.luau");
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
			"view:SetAttribute(\"TourSample\", 8)"
		);
		const auto heartbeat = source.find("RunService.Heartbeat:Connect");
		REQUIRE(heartbeat != std::string::npos);
		// Register first so the authored camera consumes each seek on that same tick.
		// The native loader fires one zero-delta startup beat before frame zero.
		std::string actions = "local testShots = {";
		for (const Shot &shot : SHOTS)
			actions += std::to_string(shot.Sample) + ",";
		actions += "}\nlocal testFrame = 0\nRunService.Heartbeat:Connect(function(deltaTime: number)\n"
				   " if deltaTime <= 0 then return end\n"
				   " local index = math.floor(testFrame / " +
				   std::to_string(FRAMES) +
				   ") + 1\n"
				   " if index <= #testShots then view:SetAttribute(\"TourSample\", testShots[index]) end\n"
				   " view:SetAttribute(\"TourSettledFrames\", testFrame % " +
				   std::to_string(FRAMES) +
				   " + 1)\n"
				   " testFrame += 1\nend)\n";
		source.insert(heartbeat, actions);
		std::ofstream output(cameraPath);
		REQUIRE(output);
		output << source;
		REQUIRE(output);
	}

	using AngleSample = std::shared_ptr<const client::FrameObservation>;

	uint64_t Fingerprint(const render::SceneCaptureResult &image) {
		uint64_t fingerprint = 1469598103934665603ULL;
		for (size_t pixel = 0; pixel + 3 < image.Pixels.size(); pixel += 4) {
			fingerprint = (fingerprint ^ image.Pixels[pixel]) * 1099511628211ULL;
			fingerprint = (fingerprint ^ image.Pixels[pixel + 1]) * 1099511628211ULL;
			fingerprint = (fingerprint ^ image.Pixels[pixel + 2]) * 1099511628211ULL;
		}
		return fingerprint;
	}

	core::Vector3 CapturedVector(const nlohmann::json &value) {
		return {value.at(0).get<float>(), value.at(1).get<float>(), value.at(2).get<float>()};
	}

	core::CFrame CapturedFrame(const nlohmann::json &value) {
		const auto &rotation = value.at("rotation");
		return {
			CapturedVector(value.at("position")),
			glm::quat(
				rotation.at(3).get<float>(),
				rotation.at(0).get<float>(),
				rotation.at(1).get<float>(),
				rotation.at(2).get<float>()
			)
		};
	}

	testing::ScenarioCheck<AngleSample> RequiredPortalMarker(const char *shot) {
		return {
			std::string(shot) + " actual tall portal marker",
			"client.noneuclidean.render",
			{{{shot}}},
			[](const auto &observation, testing::CheckResult &result) {
				const auto &frame = *observation.Payload;
				const auto &metadata = frame.Metadata;
				const auto paneName =
					metadata.at("camera_attributes").at("TourPane").template get<std::string>();
				const auto sourceWorld = metadata.at("input_world").template get<std::string>();
				std::vector<const client::FrameObjectObservation *> panes, markers;
				// Identity selection precedes colour sampling and does not depend on pixels.
				for (const auto &object : frame.Objects) {
					if (object.SourceWorld != sourceWorld || object.Variant != 0) continue;
					if (object.Object == paneName) panes.push_back(&object);
					if (object.Object == "ShortInsideMarker") markers.push_back(&object);
				}
				result.Record(
					"one named authored tour pane",
					panes.size() == 1,
					paneName + ";count=" + std::to_string(panes.size())
				);
				result.Record(
					"one named authored inside marker", markers.size() == 1, std::to_string(markers.size())
				);
				if (panes.size() != 1 || markers.size() != 1) return;
				const auto &pane = *panes[0], &marker = *markers[0];
				result.Record(
					"tall authored box",
					marker.HalfExtent.Y > marker.HalfExtent.X && marker.HalfExtent.Y > marker.HalfExtent.Z
				);
				const auto normal = pane.Frame.ZVector();
				const auto centre = pane.Frame.Position + normal * pane.HalfExtent.Z;
				std::vector<const nlohmann::json *> portals;
				for (const auto &portal : metadata.at("portal_views")) {
					if (portal.at("external").template get<bool>()) continue;
					const auto portalCentre = CapturedVector(portal.at("centre"));
					const auto portalNormal = CapturedVector(portal.at("normal"));
					const auto first = CapturedVector(portal.at("first"));
					const auto second = CapturedVector(portal.at("second"));
					const float firstLength = first.Magnitude(), secondLength = second.Magnitude();
					if ((portalCentre - centre).Magnitude() > .01f || portalNormal.Dot(normal) < .999f)
						continue;
					if (firstLength <= 0 || secondLength <= 0 || firstLength > pane.HalfExtent.X + .01f ||
						secondLength > pane.HalfExtent.Y + .01f)
						continue;
					if (std::abs(first.Dot(pane.Frame.RightVector())) / firstLength < .999f ||
						std::abs(second.Dot(pane.Frame.UpVector())) / secondLength < .999f)
						continue;
					portals.push_back(&portal);
				}
				result.Record(
					"one native local portal matching named pane geometry",
					portals.size() == 1,
					std::to_string(portals.size())
				);
				if (portals.size() != 1) return;
				const auto &portal = *portals[0];
				const auto &warp = portal.at("warp");
				const auto origin = CapturedVector(warp.at("origin"));
				const float scale = warp.at("scale").template get<float>();
				result.Record("valid native portal scale", std::isfinite(scale) && scale > 0);
				if (!std::isfinite(scale) || scale <= 0) return;
				const glm::vec3 anchor(origin.X, origin.Y, origin.Z);
				const glm::mat4 toDestination =
					CapturedFrame(warp.at("frame")).ToMatrix() * glm::translate(glm::mat4(1.0f), anchor) *
					glm::scale(glm::mat4(1.0f), glm::vec3(scale)) * glm::translate(glm::mat4(1.0f), -anchor);
				client::tests::ObjectProjectionRoute route{
					{marker.World, frame.Image.WorldName, glm::inverse(toDestination)},
					client::tests::PortalAperture{
						CapturedVector(portal.at("centre")),
						CapturedVector(portal.at("normal")),
						CapturedVector(portal.at("first")),
						CapturedVector(portal.at("second"))
					}
				};
				const auto orange = [](uint8_t red, uint8_t green, uint8_t blue) {
					return int(red) > int(green) + 8 && int(green) > int(blue) + 8;
				};
				const auto pixels = client::tests::CountObjectPixels(frame.Image, marker, orange, route);
				const std::string context = "frame=" + std::to_string(frame.ClientFrame) +
											";surface=" + std::to_string(pixels.Pixels.Samples) +
											";matching=" + std::to_string(pixels.Pixels.Matching) + ";" +
											pixels.Detail;
				result.Record(
					"required native portal box surface tested",
					pixels.Status == client::tests::ObjectPixelStatus::Tested,
					context
				);
				result.Record(
					"positive orange appearance through finite native portal",
					pixels.Pixels.Matching > 0,
					context
				);
			},
			{}
		};
	}

	std::vector<testing::ScenarioCheck<AngleSample>> AngleChecks() {
		std::vector<testing::ScenarioCheck<AngleSample>> checks;
		for (size_t index = 0; index < SHOTS.size(); ++index) {
			checks.push_back(
				{SHOTS[index].Name,
				 "client.noneuclidean.render",
				 {{{SHOTS[index].Name}}},
				 [index](const auto &observation, testing::CheckResult &result) {
					 const auto &frame = *observation.Payload;
					 const auto &metadata = frame.Metadata;
					 const auto &image = frame.Image;
					 const auto expectedFrame = (index + 1) * FRAMES - 1;
					 result.Record(
						 "settled shot frame",
						 frame.ClientFrame == expectedFrame,
						 "expected=" + std::to_string(expectedFrame) +
							 "; observed=" + std::to_string(frame.ClientFrame)
					 );
					 result.Record(
						 "metadata frame",
						 metadata.at("frame") == expectedFrame,
						 "expected=" + std::to_string(expectedFrame) +
							 "; observed=" + metadata.at("frame").dump()
					 );
					 result.Record(
						 "native tick",
						 metadata.at("tick") == expectedFrame + 1,
						 "expected=" + std::to_string(expectedFrame + 1) +
							 "; observed=" + metadata.at("tick").dump()
					 );
					 result.Record(
						 "authored shot sample",
						 metadata.at("camera_attributes").at("TourSample") == SHOTS[index].Sample,
						 "expected=" + std::to_string(SHOTS[index].Sample) +
							 "; observed=" + metadata.at("camera_attributes").at("TourSample").dump()
					 );
					 result.Record(
						 "sixteen settled frames",
						 metadata.at("camera_attributes").at("TourSettledFrames") == FRAMES,
						 "expected=" + std::to_string(FRAMES) +
							 "; observed=" + metadata.at("camera_attributes").at("TourSettledFrames").dump()
					 );
					 result.Record("capture generation", frame.Generation != 0);
					 result.Record(
						 "capture world provenance",
						 image.WorldName == metadata.at("view_world").template get<std::string>()
					 );
					 result.Record("native capture token", image.Token != 0);
					 result.Record("native frame identity", image.Frame != 0);
					 result.Record(
						 "RGBA encoding",
						 image.Format == render::ResourceImageFormat::RGBA8_UNorm ||
							 image.Format == render::ResourceImageFormat::RGBA8_SRGB
					 );
					 result.Record(
						 "field of view",
						 client::tests::Near(metadata.at("field_of_view").template get<float>(), 1.2217305f)
					 );
					 result.Record("portal count", metadata.at("portals") == 26);
					 result.Record("instance count", metadata.at("instances").template get<size_t>() > 90);
					 result.Record("capture ready", image.Status == render::SceneCaptureStatus::Ready);
					 result.Record("image width", image.Width == WIDTH);
					 result.Record("image height", image.Height == HEIGHT);
					 const bool readable =
						 image.RowStride == WIDTH * 4 && image.Pixels.size() == WIDTH * HEIGHT * 4;
					 result.Record("readable RGBA image", readable);
					 if (!readable) return;
					 size_t visible = 0, centralVisible = 0;
					 for (int y = 0; y < HEIGHT; ++y) {
						 for (int x = 0; x < WIDTH; ++x) {
							 const auto offset = (y * WIDTH + x) * 4;
							 const bool nonBlack = image.Pixels[offset] != 0 ||
												   image.Pixels[offset + 1] != 0 ||
												   image.Pixels[offset + 2] != 0;
							 visible += nonBlack;
							 if (x >= WIDTH / 4 && x < WIDTH * 3 / 4 && y >= HEIGHT / 4 && y < HEIGHT * 3 / 4)
								 centralVisible += nonBlack;
						 }
					 }
					 result.Record(
						 "visible pixels",
						 visible > static_cast<size_t>(WIDTH * HEIGHT) / 4,
						 std::to_string(visible)
					 );
					 result.Record(
						 "central visible pixels",
						 centralVisible > static_cast<size_t>(WIDTH * HEIGHT) / 20,
						 std::to_string(centralVisible)
					 );
				 },
				 {}}
			);
		}
		checks.push_back(
			{"distinct shots",
			 "client.noneuclidean.render",
			 {{}, SHOTS.size(), true},
			 {},
			 [](const auto &timeline, testing::CheckResult &result) {
				 std::vector<uint64_t> fingerprints;
				 std::string identities;
				 for (const auto &observation : timeline) {
					 const auto fingerprint = Fingerprint(observation->Payload->Image);
					 fingerprints.push_back(fingerprint);
					 identities += observation->Hook + "=" + std::to_string(fingerprint) + ";";
				 }
				 result.Record("all seven settled images", fingerprints.size() == SHOTS.size());
				 std::sort(fingerprints.begin(), fingerprints.end());
				 result.Record(
					 "distinct image fingerprints",
					 std::adjacent_find(fingerprints.begin(), fingerprints.end()) == fingerprints.end(),
					 identities
				 );
			 }}
		);
		checks.push_back(RequiredPortalMarker("front"));
		checks.push_back(RequiredPortalMarker("rolled"));
		return checks;
	}

}

TEST_CASE(
	"the authored non-euclidean angle tour renders useful portal views",
	"[client][gpu][portal][non-euclidean][.]"
) {
	const auto root = core::Paths::Base() / "non-euclidean-render";
	std::filesystem::remove_all(root);
	std::filesystem::create_directories(root / "client/DemoCameras");
	const auto script = root / "AngleTour.luau";
	WriteAngleTour(script, root / "client/DemoCameras/AngleTour.client.luau");

	testing::ScenarioKey key;
	key.Scene = "NonEuclidean.luau";
	key.ContentRevision = "staged-examples";
	key.Configuration = "headless;256x192;worlds=1;native-client";
	key.TickSchedule = "112@float(1/60)";
	key.FrameSchedule = "112@float(1/60);16 settling frames per angle";
	key.Actions = "TourMode=angles;TourSample=8,9,36,37,38,39,40@frames=0,16,32,48,64,80,96";
	key.CaptureConfiguration = "native-final-scene;RGBA;frames=15,31,47,63,79,95,111";
	key.ObservationSchema = "client.frame-observation.v1";
	key.CheckSet = "seven-angle-image-oracles;distinct-fingerprints;actual-portal-marker.v2";

	std::vector<AngleSample> frames;
	frames.reserve(SHOTS.size());
	std::vector<size_t> frameBytes;
	frameBytes.reserve(SHOTS.size());
	size_t retainedBytes = 0;
	constexpr size_t MAX_ANGLE_BYTES = 16u * 1024u * 1024u;
	testing::ScenarioSession<AngleSample> session(key, AngleChecks());
	testing::ScenarioDriver<AngleSample> driver;
	uint64_t capturedFrames = 0;
	driver.Run = [&](auto &scenario) {
		client::Client client;
		client::FrameObservationOptions captureOptions;
		captureOptions.MaxOwnedBytes = MAX_ANGLE_BYTES;
		captureOptions.MaxPendingFrames = SHOTS.size();
		for (size_t index = 0; index < SHOTS.size(); ++index)
			captureOptions.CaptureFrames.push_back((index + 1) * FRAMES - 1);
		auto subscription = client::SubscribeClientFrames(client, captureOptions, [&](AngleSample frame) {
			const auto index = frames.size();
			if (index >= SHOTS.size()) throw std::runtime_error("unexpected angle image");
			const size_t owned = frame->OwnedBytes;
			if (owned > MAX_ANGLE_BYTES - retainedBytes)
				throw std::runtime_error("angle observations exceed byte limit");
			retainedBytes += owned;
			frames.push_back(frame);
			frameBytes.push_back(owned);
			scenario.Publish({SHOTS[index].Name, frame->ClientFrame, std::move(frame), owned});
		});
		if (!subscription) throw std::runtime_error(subscription.Failure());
		client::Options options;
		options.Headless = true;
		options.Width = WIDTH;
		options.Height = HEIGHT;
		options.MaximumFrames = FRAMES * SHOTS.size();
		options.ScriptPath = script.string();
		if (!client.Initialise(options)) throw std::runtime_error("angle client initialization failed");
		if (client.Run() != 0) throw std::runtime_error("angle client run failed");
		client.Shutdown();
		if (!subscription.Failure().empty()) throw std::runtime_error(subscription.Failure());
		capturedFrames = subscription.CapturedFrames();
	};
	const auto report = session.Run(driver);
	for (const auto &check : report.Checks)
		client::tests::CheckScenario(report, check.Name);
	CHECK(report.Metrics.SetupCount == 1);
	CHECK(report.Metrics.PublishedSamples == SHOTS.size());
	CHECK(capturedFrames == SHOTS.size());

	// Replay the exact immutable bytes to verify dispatch parity without another client.
	testing::ScenarioOptions serialOptions;
	serialOptions.Context = parallel::JobContext::Serial;
	testing::ScenarioSession<AngleSample> serial(key, AngleChecks(), serialOptions);
	testing::ScenarioDriver<AngleSample> replay;
	replay.Run = [&](auto &scenario) {
		for (size_t index = 0; index < frames.size(); ++index) {
			const auto &frame = frames[index];
			scenario.Publish({SHOTS[index].Name, frame->ClientFrame, frame, frameBytes[index]});
		}
	};
	const auto serialReport = serial.Run(replay);
	CHECK(serialReport.Failures == report.Failures);
	CHECK(serialReport.Checks == report.Checks);
	std::cout << "angle tour: setups=" << report.Metrics.SetupCount << " frames=" << FRAMES * SHOTS.size()
			  << " captures=" << capturedFrames << " retained-bytes=" << retainedBytes
			  << " driver-ms=" << report.Metrics.DriverNanoseconds / 1'000'000.0 << '\n';
}

TEST_CASE(
	"an unjoined two-world capture sequence writes every image with one claim per portal",
	"[client][gpu][portal][non-euclidean][.]"
) {
	const auto root = core::Paths::Base() / "cross-world-capture-sequence";
	std::filesystem::remove_all(root);
	client::Options options;
	options.Headless = true;
	options.Worlds = 2;
	options.ViewSpacing = 0;
	options.Width = WIDTH;
	options.Height = HEIGHT;
	options.MaximumFrames = 24;
	options.MaximumFrameRate = 30;
	options.ScriptPath =
		(core::Paths::Base().parent_path() / "assets/examples/scripts/CrossWorldSeam.luau").string();
	options.CaptureSequence = root;
	client::Client client;
	REQUIRE(client.Initialise(options));
	REQUIRE(client.Run() == 0);
	for (int frame = 0; frame < options.MaximumFrames; ++frame) {
		CAPTURE(frame);
		const auto stem = root / std::to_string(frame);
		std::ifstream metadataFile(stem.string() + ".json");
		REQUIRE(metadataFile);
		const nlohmann::json metadata = nlohmann::json::parse(metadataFile);
		CHECK(metadata.at("frame") == frame);
		CHECK(metadata.at("view_world") == "client.world");
		const auto &portals = metadata.at("portal_views");
		CHECK(metadata.at("portals") == portals.size());
		std::vector<int> claimedIndices;
		for (const auto &portal : portals)
			claimedIndices.push_back(portal.at("index").get<int>());
		std::sort(claimedIndices.begin(), claimedIndices.end());
		CHECK(std::adjacent_find(claimedIndices.begin(), claimedIndices.end()) == claimedIndices.end());
		if (frame == options.MaximumFrames - 1) {
			REQUIRE(portals.size() == 1);
			CHECK(portals[0].at("external") == true);
			CHECK(portals[0].at("key") == "Workspace.PortalBlock.Hole");
			CHECK(portals[0].at("image").get<uint64_t>() != 0);
			CHECK(portals[0].at("capture").is_object());
		}
		std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> image(
			SDL_LoadBMP((stem.string() + ".bmp").c_str()), SDL_DestroySurface
		);
		REQUIRE(image);
		CHECK(image->w == WIDTH);
		CHECK(image->h == HEIGHT);
	}
}
