// The second hallway demo has a short shell whose entrance opens into a long
// isolated corridor. Render its entry in small forward and reverse steps so a
// portal body and the eye cannot disappear at the seam.

#include "FrameObservation.hpp"
#include "ImageChecks.hpp"
#include "ScenarioChecks.hpp"

#include <engine/core/Paths.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <client/Client.hpp>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

TEST_SUITE_ID("client.tunnels.render")
TEST_DEPENDS("client.scene.tick")
TEST_DEPENDS("engine.testing.scenario")
TEST_DEPENDS("client.imagechecks")
TEST_DEPENDS("engine.examples.scene")

namespace {

	using namespace engine;

	constexpr int WIDTH = 160;
	constexpr int HEIGHT = 120;
	constexpr int FRAMES = 64;

	std::string ReadText(const std::filesystem::path &path) {
		std::ifstream input(path);
		REQUIRE(input);
		return {std::istreambuf_iterator<char>(input), {}};
	}

	void WriteEntrySweep(const std::filesystem::path &path) {
		std::string source =
			ReadText(core::Paths::Base().parent_path() / "assets/examples/scripts/Tunnels.luau");
		const std::string namedCameras = "cameraScript:SetAttribute(\"NamedCameras\", true)";
		const size_t cameras = source.find(namedCameras);
		REQUIRE(cameras != std::string::npos);
		// Keep the demo's real client companion, configured to leave the default
		// follow eye in control of this character-driven crossing.
		source.replace(cameras, namedCameras.size(), "cameraScript:SetAttribute(\"NamedCameras\", false)");
		const auto companionDirectory = path.parent_path() / "client/DemoCameras";
		std::filesystem::create_directories(companionDirectory);
		REQUIRE(
			std::filesystem::copy_file(
				core::Paths::Base().parent_path() /
					"assets/examples/scripts/client/DemoCameras/Tunnels.client.luau",
				companionDirectory / (path.stem().string() + ".client.luau")
			)
		);
		source += R"(

-- Test capture: use the actual local player and its default follow camera.
-- That is the Studio path, unlike an authored fixed camera or an extra player.
--
-- **The walk goes deep enough for the eye to end up in the corridor, and that is
-- the whole reason it is not twenty-two frames of tenths.** A follow camera
-- twelve studs behind a body that has just come through the mouth is still
-- outside it: the arm reaches back out through the mouth the body came in by,
-- and `PlaceCamera` maps that arm through the seam, so the eye stands on the
-- plain twelve studs from the tunnel looking in through the pane. That is the
-- seamless answer and it is what the first eleven frames photograph. The eye
-- only joins the body in the corridor's own chart once the body is more than
-- twelve studs past the mouth, and a walk that stops one stud in never gets
-- there - which is what this case asserted before, and it could not pass.
local captureStep = 0
local capturePlayer = assert(Players.LocalPlayer, "local player must exist before the capture script")
local captureCharacter = assert(capturePlayer:LoadCharacter(), "local player must load a character")
local captureRoot = assert(captureCharacter:FindFirstChild("HumanoidRootPart") :: BasePart?)
local captureTorso = assert(captureCharacter:FindFirstChild("Torso") :: BasePart?)
local captureHumanoid = assert(captureCharacter:FindFirstChild("Humanoid") :: Humanoid?)
	captureTorso.Color = Color3.new(1, 0, 1)
RunService.Heartbeat:Connect(function(_deltaTime: number)
	captureTorso.Color = Color3.new(1, 0, 1)
	local step = captureStep % 64
	-- Tenths at both mouths, where the seam is, and six-tenths between them,
	-- where the only thing being measured is how far the eye has walked.
	local z = if step <= 10 then
		1.5 - step * 0.1
	elseif step <= 30 then
		0.5 - (step - 10) * 0.6
	elseif step <= 50 then
		-11.5 + (step - 30) * 0.6
	else
		-0.5 + math.min(step - 50, 11) * 0.1
	-- Continue the same authored walk in the far chart after it crosses. Writing
	-- the near coordinate every frame would teleport the real body through the
	-- pane repeatedly, which is not a player walking through it.
	local far = z < 1.125
	local physicalZ = if far then z + 11.75 else z
	local physicalX = if far then 54 else SHORT_X
	captureRoot.CFrame = CFrame.lookAt(
		Vector3.new(physicalX, 3, physicalZ),
		Vector3.new(physicalX, 3, physicalZ - (if step <= 30 then 1 else -1))
	)
	captureHumanoid.MoveDirection = Vector3.new(0, 0, if step <= 30 then -1 else 1)
	captureStep += 1
end)
)";
		std::ofstream output(path);
		REQUIRE(output);
		output << source;
		REQUIRE(output);
	}

	using TunnelFrame = std::shared_ptr<const client::FrameObservation>;

	bool Magenta(uint8_t red, uint8_t green, uint8_t blue) {
		return red > 70 && green < 90 && blue > 70;
	}

	std::vector<testing::ScenarioCheck<TunnelFrame>> TunnelChecks() {
		using testing::CheckResult;
		std::vector<testing::ScenarioCheck<TunnelFrame>> checks;
		checks.push_back(
			{"scene observations",
			 "client.tunnels.render",
			 {{{"frame", FRAMES}}, FRAMES},
			 [](const auto &observation, CheckResult &result) {
				 const auto &frame = *observation.Payload;
				 const auto &sample = frame.Metadata;
				 result.Record(
					 "ordered frame",
					 frame.ClientFrame + 1 == result.Samples,
					 "observed=" + std::to_string(frame.ClientFrame)
				 );
				 result.Record("metadata frame", sample.at("frame") == frame.ClientFrame);
				 result.Record("instance count", sample.at("instances").template get<size_t>() > 20);
				 result.Record(
					 "native world provenance",
					 frame.Image.WorldName == sample.at("view_world").template get<std::string>()
				 );
			 },
			 {}}
		);
		for (const bool body : {false, true}) {
			checks.push_back(
				{body ? "visible magenta body" : "nonblack scene",
				 "client.tunnels.render",
				 {{{"frame", FRAMES}}, FRAMES},
				 [body](const auto &observation, CheckResult &result) {
					 const auto &image = observation.Payload->Image;
					 const auto pixels =
						 client::tests::CountPixels(image, {0, 0, image.Width, image.Height}, Magenta);
					 const std::string context = "frame=" + std::to_string(observation.Tick);
					 result.Record(
						 "canonical image readable", pixels.Available, context + "; " + pixels.Failure
					 );
					 if (!pixels.Available) return;
					 if (body)
						 result.Record(
							 "positive magenta body pixels",
							 pixels.Matching > 0,
							 context + "; pixels=" + std::to_string(pixels.Matching)
						 );
					 else
						 result.Record(
							 "nonblack scene pixels",
							 pixels.NonBlack > static_cast<size_t>(WIDTH * HEIGHT) / 8,
							 context + "; pixels=" + std::to_string(pixels.NonBlack)
						 );
				 },
				 {}}
			);
		}
		struct Movement {
			core::Vector3 Previous;
			bool HasPrevious = false;
			size_t TenthStudSteps = 0;
			size_t ExteriorEyes = 0;
			size_t InteriorEyes = 0;
			size_t EnteredInterior = 0;
			size_t ReturnedExterior = 0;
		};
		auto movement = std::make_shared<Movement>();
		checks.push_back(
			{"camera charts and seam movement",
			 "client.tunnels.render",
			 {{{"frame", FRAMES}}, FRAMES},
			 [movement](const auto &observation, CheckResult &result) {
				 const auto &position = observation.Payload->Metadata.at("camera").at("position");
				 const core::Vector3 eye{position.at(0), position.at(1), position.at(2)};
				 const bool exterior = std::abs(eye.X - 20.0f) < .01f;
				 const bool interior = std::abs(eye.X - 54.0f) < .01f;
				 movement->ExteriorEyes += exterior;
				 movement->InteriorEyes += interior;
				 result.Record(
					 "eye in authored chart",
					 exterior || interior,
					 "frame=" + std::to_string(observation.Tick) + "; x=" + std::to_string(eye.X)
				 );
				 if (movement->HasPrevious) {
					 const bool previousExterior = std::abs(movement->Previous.X - 20.0f) < .01f;
					 movement->EnteredInterior += previousExterior && interior;
					 movement->ReturnedExterior += !previousExterior && exterior;
					 const float step = (eye - movement->Previous).Magnitude();
					 movement->TenthStudSteps += std::abs(step - .1f) < .025f;
				 }
				 movement->Previous = eye;
				 movement->HasPrevious = true;
			 },
			 [movement](const auto &, CheckResult &result) {
				 result.Record(
					 "at least sixteen tenth-stud steps",
					 movement->TenthStudSteps >= 16,
					 std::to_string(movement->TenthStudSteps)
				 );
				 result.Record(
					 "exterior eyes observed",
					 movement->ExteriorEyes > 0,
					 std::to_string(movement->ExteriorEyes)
				 );
				 result.Record(
					 "interior eyes observed",
					 movement->InteriorEyes > 0,
					 std::to_string(movement->InteriorEyes)
				 );
				 result.Record(
					 "exactly one entry",
					 movement->EnteredInterior == 1,
					 std::to_string(movement->EnteredInterior)
				 );
				 result.Record(
					 "exactly one return",
					 movement->ReturnedExterior == 1,
					 std::to_string(movement->ReturnedExterior)
				 );
			 }}
		);
		return checks;
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
	WriteEntrySweep(script);
	testing::ScenarioKey key;
	key.Scene = "Tunnels.luau";
	key.ContentRevision = "staged-examples";
	key.Configuration = "headless;160x120;worlds=1;native-character-follow-camera;NamedCameras=false";
	key.TickSchedule = "native-zero-delta-startup;64@float(1/60)";
	key.FrameSchedule = "64@float(1/60);uncapped;captureAlpha=default";
	key.Actions = "authored-entry-sweep;64steps;magenta-torso;forward-return";
	key.CaptureConfiguration = "native-final-scene;canonicalRGBA;frames=0..63;streamed";
	key.ObservationSchema = "client.frame-observation.v1";
	key.CheckSet = "scene;nonblack;magenta;chart;tenths;entry-return.v1";

	testing::ScenarioSession<TunnelFrame> session(key, TunnelChecks());
	testing::ScenarioDriver<TunnelFrame> driver;
	uint64_t capturedFrames = 0;
	size_t peakOwnedBytes = 0;
	driver.Run = [&](auto &scenario) {
		client::Client client;
		client::FrameObservationOptions observationOptions;
		observationOptions.MaxOwnedBytes = 8u * 1024u * 1024u;
		for (uint64_t frame = 0; frame < FRAMES; ++frame)
			observationOptions.CaptureFrames.push_back(frame);
		auto lease = client::SubscribeClientFrames(client, observationOptions, [&](TunnelFrame frame) {
			peakOwnedBytes = std::max(peakOwnedBytes, frame->OwnedBytes);
			const auto nativeFrame = frame->ClientFrame;
			const auto bytes = frame->OwnedBytes;
			scenario.Publish({"frame", nativeFrame, std::move(frame), bytes});
		});
		if (!lease) throw std::runtime_error(lease.Failure());
		client::Options options;
		options.Headless = true;
		options.Width = WIDTH;
		options.Height = HEIGHT;
		options.MaximumFrames = FRAMES;
		options.MaximumFrameRate = 60;
		options.Uncapped = true;
		options.ScriptPath = script.string();
		if (!client.Initialise(options)) throw std::runtime_error("tunnel client initialization failed");
		if (client.Run() != 0) throw std::runtime_error("tunnel client run failed");
		client.Shutdown();
		if (!lease.Finish()) throw std::runtime_error(lease.Failure());
		capturedFrames = lease.CapturedFrames();
		if (lease.OwnedBytes() != 0)
			throw std::runtime_error("tunnel observations retained after streaming checks joined");
	};
	const auto report = session.Run(driver);
	for (const auto &check : report.Checks)
		client::tests::CheckScenario(report, check.Name);
	CHECK(report.Metrics.SetupCount == 1);
	CHECK(report.Metrics.PublishedSamples == FRAMES);
	CHECK(capturedFrames == FRAMES);
	CHECK(report.Metrics.PeakRetainedSamples == 0);
	std::cout << "tunnel entry sweep: setups=" << report.Metrics.SetupCount << " frames=" << FRAMES
			  << " captures=" << capturedFrames << " peak-owned-frame-bytes=" << peakOwnedBytes
			  << " driver-ms=" << report.Metrics.DriverNanoseconds / 1'000'000.0 << '\n';
}
