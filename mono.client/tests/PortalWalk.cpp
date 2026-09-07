#include <engine/core/Log.hpp>
#include <engine/core/Paths.hpp>
#include <engine/game/Game.hpp>
#include <engine/net/Transport.hpp>
#include <engine/parallel/Process.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/testing/Suite.hpp>

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <spdlog/sinks/callback_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <client/Client.hpp>
#include <client/Replicated.hpp>
#include <cmath>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <thread>

TEST_SUITE_ID("client.portalwalk")
TEST_DEPENDS("client.presentationhost")
TEST_DEPENDS("client.portalsession")

static void RunPortalWalk(
	double worldTickRate,
	bool firstPerson,
	bool explicitSubject,
	bool holdThroughAdoption,
	bool lateClear,
	bool imageHandoff = false,
	bool warmPortal = false
) {
	using namespace engine;
	CAPTURE(worldTickRate, firstPerson, explicitSubject, holdThroughAdoption, lateClear, warmPortal);
	const auto programs = core::Paths::Base().parent_path();
	const auto serverProgram = programs / "server" / core::Paths::Program("server");
	const auto clientProgram = programs / "client" / core::Paths::Program("client");
	if (!std::filesystem::exists(serverProgram) || !std::filesystem::exists(clientProgram))
		SKIP("build both product programs before this test");
	const auto scenePath = core::Paths::Base() / "portal-client-walk.agame";
	const std::string sceneSource = R"(
local floor = Instance.new("Part")
floor.Anchored = true
floor.Size = Vector3.new(100, 2, 100)
floor.Position = Vector3.new(0, -1, 0)
floor.Color = game.JobId == "walk.destination" and Color3.new(0.15, 0.25, 0.9) or Color3.new(0.9, 0.3, 0.1)
floor.Parent = workspace
local pane = Instance.new("Part")
pane.Anchored = true
pane.Size = Vector3.new(10, 10, 0.4)
pane.Position = Vector3.new(0, 3, -3)
pane.Parent = workspace
local beyond = Instance.new("Part")
beyond.Anchored = true
beyond.CanCollide = false
beyond.Transparency = 1
beyond.Size = pane.Size
beyond.CFrame = CFrame.new(0, 3, -3.4) * CFrame.Angles(0, math.pi, 0)
beyond.Parent = workspace
local portal = Instance.new("Portal")
portal.Destination = beyond
portal.DestinationWorld = game.JobId == "walk.destination" and "server.world" or "walk.destination"
portal.Parent = pane
)";
	scene::RegisterSceneClasses();
	script::RegisterScriptComponents();
	world::Universe authored;
	for (const auto *name : {"server.world", "walk.destination"}) {
		const auto id = authored.Create({.Name = core::Name(name), .TickRate = worldTickRate});
		authored.Enter(id, [&](ecs::Store &store) {
			scene::InstallServices(store);
			script::SourceCache programs;
			programs.Set(core::Name("walk.luau"), sceneSource);
			programs.Set(
				core::Name("camera.luau"),
				std::string("local lateClear = ") + (lateClear ? "true\n" : "false\n") +
					"local explicitSubject = " + (explicitSubject ? "true\n" : "false\n") +
					"local authority = \"" + name + R"("
local players
local boundCharacter
local cleared = false
local restored = false
game:GetService("RunService").Heartbeat:Connect(function()
    if not players then
        local arrived, service = pcall(function() return game:GetService("Players") end)
        if not arrived then return end
        players = service
    end
    local player = players.LocalPlayer
    if not player or not player:IsA("Player") then return end
    local character = player.Character
    if not character or not character:IsA("Model") then return end
    local humanoid = character and character:FindFirstChildOfClass("Humanoid")
    local camera = workspace.CurrentCamera
    local replaced = boundCharacter and boundCharacter ~= character
    if explicitSubject and humanoid and camera and boundCharacter ~= character then
        camera.CameraSubject = humanoid
        boundCharacter = character
    end
    if lateClear and authority == "server.world" and replaced and not cleared and camera then
        camera.CameraSubject = nil
        cleared = true
        print("portal-camera-cleared", authority)
    end
    if lateClear and authority == "walk.destination" and not restored and camera and humanoid and game:GetService("UserInputService"):IsKeyDown(Enum.KeyCode.R) then
        camera.CameraSubject = humanoid
        restored = true
        print("portal-camera-restored", authority)
    end
    local valid = humanoid and camera and camera.CameraSubject == humanoid
    print("portal-camera", authority, valid and "bound" or "unbound")
    local root = humanoid and humanoid.RootPart
    if valid and root then
        local distance = (camera.CFrame.Position - root.Position - Vector3.new(0, 1.5, 0)).Magnitude
        print("portal-camera-distance", authority, distance < 1 and "first" or "third", humanoid.MoveDirection.Magnitude < 0.001 and "rest" or "moving", distance, camera.CFrame.LookVector.X)
    end
end)
)"
			);
			store.SetResource(programs);
			REQUIRE(store.SetParent(
				script::MakeScript(store, "walk.luau", "Walk"), store.FindFirstRoot("ServerScriptService")
			));
			REQUIRE(store.SetParent(
				script::MakeScript(store, "camera.luau", "CameraObserver", true),
				store.FindFirstRoot("ReplicatedFirst")
			));
		});
	}
	std::string saveError;
	REQUIRE(game::SaveGame(authored, core::Name("portal walk"), scenePath, saveError));
	const auto configPath = core::Paths::Base() / "portal-client-walk.ini";
	std::ofstream(configPath).close();
	const auto localPath = core::Paths::Base() / "portal-client-empty.luau";
	std::ofstream(localPath) << "return\n";
	auto reservation = net::MakeUdpTransport(0);
	REQUIRE(reservation);
	const auto port = reservation->Local().Port;
	reservation->Close();
	parallel::Process server;
	REQUIRE(server.Start(
		serverProgram,
		{"--listen",
		 std::to_string(port),
		 "--game",
		 scenePath.string(),
		 "--remote-world",
		 "walk.destination",
		 "--presentation-program",
		 clientProgram.string(),
		 "--config",
		 configPath.string(),
		 "--mcp-port",
		 "-1"}
	));
	struct Observations {
		std::mutex Mutex;
		bool Joined = false;
		bool PortalReady = false;
		size_t CapturedFrames = 0;
		bool CameraCleared = false, CameraRestored = false;
		std::vector<std::string> Adoptions;
		std::vector<std::string> Refusals;
		std::array<size_t, 3> CameraSamples{};
		std::array<size_t, 3> CameraModeSamples{};
		std::array<bool, 3> Moving{};
		std::array<size_t, 3> LookSamples{};
		std::array<size_t, 3> PointerFrames{};
		std::array<float, 3> MinimumLookX{};
		std::array<float, 3> MaximumLookX{};
		std::array<uint64_t, 3> LastMoveTick{};
		uint64_t LastSubmission = 0;
		std::array<size_t, 3> MoveSamples{};
		bool DuplicateMove = false;
		std::vector<std::string> CameraFailures;
	} observed;
	std::filesystem::path captureDirectory;
	const auto key = [](SDL_Scancode scan, SDL_Keycode code, bool down) {
		SDL_Event event{};
		event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
		event.key.scancode = scan;
		event.key.key = code;
		event.key.down = down;
		(void)SDL_PushEvent(&event);
	};
	const auto turning = [](bool down) {
		SDL_Event button{};
		button.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
		button.button.button = SDL_BUTTON_RIGHT;
		button.button.down = down;
		(void)SDL_PushEvent(&button);
	};
	auto sink = std::make_shared<spdlog::sinks::callback_sink_mt>([&](const spdlog::details::log_msg &entry) {
		std::string_view message(entry.payload.data(), entry.payload.size());
		std::lock_guard lock(observed.Mutex);
		if (message == "[scriptluau] [script] portal-camera-cleared\tserver.world") {
			observed.CameraCleared = true;
			return;
		}
		if (message == "[scriptluau] [script] portal-camera-restored\twalk.destination") {
			observed.CameraRestored = true;
			return;
		}
		if (message.starts_with("[render] captured ")) {
			// Metadata follows the capture log, so inspect the preceding completed frame.
			if (warmPortal && !observed.PortalReady && observed.CapturedFrames != 0) {
				std::ifstream metadata(
					captureDirectory / (std::to_string(observed.CapturedFrames - 1) + ".json")
				);
				const auto sample = nlohmann::json::parse(metadata, nullptr, false);
				if (!sample.is_discarded() && sample.contains("portal_views"))
					for (const auto &portal : sample.at("portal_views"))
						observed.PortalReady |=
							portal.value("external", false) && portal.value("image", uint64_t{0}) != 0;
			}
			++observed.CapturedFrames;
			const size_t stage = observed.Adoptions.size();
			if (stage < 2 && observed.Moving[stage]) {
				SDL_Event motion{};
				motion.type = SDL_EVENT_MOUSE_MOTION;
				motion.motion.xrel = (++observed.PointerFrames[stage] / 40) % 2 == 0 ? 2.0f : -2.0f;
				(void)SDL_PushEvent(&motion);
			}
			return;
		}
		constexpr std::string_view distancePrefix = "[scriptluau] [script] portal-camera-distance\t";
		if (message.starts_with(distancePrefix)) {
			const size_t stage = observed.Adoptions.size();
			const std::string_view authority = stage == 1 ? "walk.destination" : "server.world";
			message.remove_prefix(distancePrefix.size());
			if (!message.starts_with(authority)) return;
			const std::string_view mode = firstPerson ? "\tfirst\t" : "\tthird\t";
			// During movement the predicted eye and authoritative root describe different times.
			if (observed.Moving[stage]) {
				const auto coordinate = message.substr(message.rfind('\t') + 1);
				float lookX = 0;
				const auto parsed =
					std::from_chars(coordinate.data(), coordinate.data() + coordinate.size(), lookX);
				if (parsed.ec != std::errc{}) {
					if (observed.CameraFailures.size() < 8) observed.CameraFailures.emplace_back(message);
					return;
				}
				if (observed.LookSamples[stage] == 0) {
					observed.MinimumLookX[stage] = lookX;
					observed.MaximumLookX[stage] = lookX;
				}
				observed.MinimumLookX[stage] = std::min(observed.MinimumLookX[stage], lookX);
				observed.MaximumLookX[stage] = std::max(observed.MaximumLookX[stage], lookX);
				++observed.LookSamples[stage];
				return;
			}
			if (message.find("\trest\t") == std::string_view::npos) return;
			if (message.find(mode) != std::string_view::npos) {
				++observed.CameraModeSamples[stage];
				if (stage < 2 && observed.CameraModeSamples[stage] >= 3 &&
					(stage != 0 || !warmPortal || observed.PortalReady)) {
					observed.Moving[stage] = true;
					turning(true);
					key(stage == 0 ? SDL_SCANCODE_W : SDL_SCANCODE_S, stage == 0 ? SDLK_W : SDLK_S, true);
				}
			} else if (observed.CameraModeSamples[stage] >= 3) {
				if (observed.CameraFailures.size() < 8) observed.CameraFailures.emplace_back(message);
			} else {
				observed.CameraModeSamples[stage] = 0;
			}
			return;
		}
		constexpr std::string_view cameraPrefix = "[scriptluau] [script] portal-camera\t";
		if (message.starts_with(cameraPrefix)) {
			const size_t stage = observed.Adoptions.size();
			const std::string_view authority = stage == 1 ? "walk.destination" : "server.world";
			message.remove_prefix(cameraPrefix.size());
			if (!message.starts_with(authority)) return;
			if (message.ends_with("\tbound"))
				++observed.CameraSamples[stage];
			else if (!(lateClear && observed.CameraCleared && !observed.CameraRestored) &&
					 observed.CameraSamples[stage] != 0 && observed.CameraFailures.size() < 8)
				observed.CameraFailures.emplace_back(message);
			return;
		}
		if (!message.starts_with("[client] ")) return;
		message.remove_prefix(std::string_view("[client] ").size());
		constexpr std::string_view movePrefix = "move submitted at tick ";
		if (message.starts_with(movePrefix)) {
			message.remove_prefix(movePrefix.size());
			uint64_t tick = 0;
			const auto parsed = std::from_chars(message.data(), message.data() + message.size(), tick);
			const size_t stage = observed.Adoptions.size();
			if (parsed.ec != std::errc{} || tick <= observed.LastSubmission) observed.DuplicateMove = true;
			observed.LastSubmission = tick;
			observed.LastMoveTick[stage] = tick;
			++observed.MoveSamples[stage];
			if (lateClear && stage == 1 && observed.MoveSamples[stage] == 12)
				key(SDL_SCANCODE_R, SDLK_R, true);
			if (lateClear && stage == 1 && observed.MoveSamples[stage] == 14)
				key(SDL_SCANCODE_R, SDLK_R, false);
			if (holdThroughAdoption && stage > 0 && observed.MoveSamples[stage] == 12) {
				if (stage == 1)
					key(SDL_SCANCODE_W, SDLK_W, false);
				else
					key(SDL_SCANCODE_S, SDLK_S, false);
			}
			return;
		}
		if (message.starts_with("joined:") && !observed.Joined) {
			observed.Joined = true;
			SDL_Event focus{};
			focus.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
			(void)SDL_PushEvent(&focus);
			if (firstPerson) {
				SDL_Event zoom{};
				zoom.type = SDL_EVENT_MOUSE_WHEEL;
				zoom.wheel.y = 100;
				(void)SDL_PushEvent(&zoom);
			}
		}
		if (message.starts_with("portal transfer refused:") && observed.Refusals.size() < 8)
			observed.Refusals.emplace_back(message);
		if (!message.starts_with("portal session adopted ") || observed.Adoptions.size() >= 2) return;
		observed.Adoptions.emplace_back(message);
		turning(false);
		if (holdThroughAdoption) return;
		if (observed.Adoptions.size() == 1) {
			key(SDL_SCANCODE_W, SDLK_W, false);
		} else {
			key(SDL_SCANCODE_S, SDLK_S, false);
		}
	});
	struct SinkLifetime {
		spdlog::sink_ptr Sink;
		core::LogLevel PreviousClientLevel;
		core::LogLevel PreviousRenderLevel;
		~SinkLifetime() {
			std::erase(core::Log::Logger().sinks(), Sink);
			core::Log::SetLevel("client", PreviousClientLevel);
			core::Log::SetLevel("render", PreviousRenderLevel);
		}
	} attached{sink, core::Log::LevelOf("client"), core::Log::LevelOf("render")};
	core::Log::SetLevel("client", core::LogLevel::Trace);
	core::Log::SetLevel("render", core::LogLevel::Trace);
	core::Log::Logger().sinks().push_back(sink);
	client::Options options;
	options.Headless = true;
	options.Width = 128;
	options.Height = 128;
	options.ScriptPath = localPath.string();
	options.ConnectAddress = net::Endpoint::LoopbackIPv4(port).Text();
	options.PlayKey = std::string(64, '3');
	options.ProfileSeconds = 45;
	options.MaximumFrameRate = 60;
	options.Uncapped = true;
	options.MaximumFrames = 600;
	options.CaptureSequence =
		core::Paths::Base() /
		("portal-client-walk-" + std::to_string(static_cast<int>(worldTickRate)) +
		 (firstPerson ? "-first" : "-third") + (explicitSubject ? "-explicit" : "") +
		 (holdThroughAdoption ? "-held" : "") +
		 (lateClear ? (imageHandoff ? "-clear-image-frames" : "-clear-frames") : "-frames"));
	if (warmPortal) options.CaptureSequence += "-warm";
	captureDirectory = options.CaptureSequence;
	std::filesystem::remove_all(options.CaptureSequence);
	const auto finalCapture = options.CaptureSequence / (std::to_string(options.MaximumFrames - 1) + ".bmp");
	client::Client player;
	REQUIRE(player.Initialise(options));
	CHECK(player.Run() == 0);
	if (warmPortal) {
		std::lock_guard lock(observed.Mutex);
		CHECK(observed.PortalReady);
	}
	bool characterSeen = false;
	bool explicitSubjectSeen = false;
	std::optional<float> previousYaw;
	float maximumEyeHeightError = 0;
	std::optional<nlohmann::json> previousSample;
	size_t sourceClearSamples = 0, adoptedClearSamples = 0, clearedAdoptions = 0;
	size_t handoffMoveSamples = 0;
	size_t retainedMoveSamples = 0, nativeReturnSamples = 0, nativeHeldSamples = 0, loadingSamples = 0;
	bool returned = false;
	bool outboundMotion = false, returnMotion = false;
	bool outboundReplay = false, returnReplay = false;
	std::vector<std::string> nativeWorlds;
	std::vector<std::string> movingNativeWorlds;
	for (int frame = 0; frame < options.MaximumFrames; ++frame) {
		const auto stem = options.CaptureSequence / std::to_string(frame);
		std::ifstream metadata(stem.string() + ".json");
		REQUIRE(metadata);
		const auto sample = nlohmann::json::parse(metadata);
		CHECK(sample.at("frame") == frame);
		if (sample.value("eye_image", false)) {
			REQUIRE(sample.contains("eye_capture"));
			REQUIRE(sample.at("eye_capture").is_object());
			CHECK(sample.at("eye_capture").at("image") == sample.at("eye_image_handle"));
			CHECK(sample.at("eye_capture").at("projection") == "eye");
			CHECK(sample.at("eye_capture").at("request").get<uint64_t>() != 0);
		}
		const bool clearedFrame = lateClear && sample.value("subject", uint64_t{1}) == 0 &&
								  !sample.value("subject_automatic", true);
		if (clearedFrame && sample.value("retained_character", false)) ++sourceClearSamples;
		if (sample.value("retained_character", false) && !sample.value("eye_image", false))
			++nativeHeldSamples;
		if (clearedFrame && sample.contains("native_prediction")) ++adoptedClearSamples;
		if (clearedFrame && previousSample && previousSample->value("subject", uint64_t{1}) == 0 &&
			previousSample->value("eye_world", "") == sample.value("eye_world", "")) {
			float displacement = 0, rotationDot = 0;
			for (size_t axis = 0; axis < 3; ++axis) {
				const float delta = sample.at("camera").at("position").at(axis).get<float>() -
									previousSample->at("camera").at("position").at(axis).get<float>();
				displacement += delta * delta;
			}
			for (size_t axis = 0; axis < 4; ++axis)
				rotationDot += sample.at("camera").at("rotation").at(axis).get<float>() *
							   previousSample->at("camera").at("rotation").at(axis).get<float>();
			CAPTURE(frame);
			CHECK(displacement < .000001f);
			CHECK(std::abs(rotationDot) > .99999f);
		}
		if (sample.value("loading_portals", false) && sample.contains("predicted_velocity")) {
			++loadingSamples;
			CAPTURE(frame);
			const auto &velocity = sample.at("predicted_velocity");
			CHECK(std::abs(velocity.at(0).get<float>()) < .001f);
			CHECK(std::abs(velocity.at(2).get<float>()) < .001f);
		}
		if (sample.contains("native_prediction")) {
			const auto &native = sample.at("native_prediction");
			CHECK(
				native.at("applied_input_tick").get<uint64_t>() <=
				sample.at("submitted_move_tick").get<uint64_t>()
			);
			if (native.at("applied_pose_tick").get<uint64_t>() != 0) {
				const auto world = sample.at("input_world").get<std::string>();
				if (std::find(nativeWorlds.begin(), nativeWorlds.end(), world) == nativeWorlds.end())
					nativeWorlds.push_back(world);
				if (sample.contains("predicted_velocity")) {
					const auto &velocity = sample.at("predicted_velocity");
					const float moving =
						std::abs(velocity.at(0).get<float>()) + std::abs(velocity.at(2).get<float>());
					if (moving > .01f &&
						std::find(movingNativeWorlds.begin(), movingNativeWorlds.end(), world) ==
							movingNativeWorlds.end())
						movingNativeWorlds.push_back(world);
				}
			}
		}
		if (previousSample && previousSample->contains("portal_handoff") &&
			previousSample->at("input_world") != sample.at("input_world")) {
			CAPTURE(frame);
			returned =
				returned || previousSample->at("portal_handoff").value("destination", "") == "server.world";
			CHECK(sample.contains("predicted_root"));
			if (!firstPerson && !lateClear &&
				previousSample->at("portal_handoff").value("destination", "") == "walk.destination") {
				// These rooms have only orange and blue floors. Yellow belongs to
				// the avatar and must survive the source replica's retirement.
				for (const int bodyFrame : {frame - 1, frame}) {
					CAPTURE(bodyFrame);
					const auto path = options.CaptureSequence / (std::to_string(bodyFrame) + ".bmp");
					std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> body(
						SDL_LoadBMP(path.string().c_str()), SDL_DestroySurface
					);
					REQUIRE(body);
					size_t yellowPixels = 0;
					bool readable = true;
					for (int y = 0; y < body->h; ++y) {
						for (int x = 0; x < body->w; ++x) {
							Uint8 red = 0, green = 0, blue = 0, alpha = 0;
							readable &= SDL_ReadSurfacePixel(body.get(), x, y, &red, &green, &blue, &alpha);
							yellowPixels += red > 90 && green > 90 && blue < 70 && red > .7f * green &&
											red < 1.4f * green;
						}
					}
					REQUIRE(readable);
					CHECK(yellowPixels > 0);
				}
			}
			if (lateClear &&
				previousSample->at("portal_handoff").value("destination", "") == "walk.destination") {
				CHECK(clearedFrame);
				++clearedAdoptions;
			}
			if (imageHandoff) {
				REQUIRE(previousSample->value("eye_world", "") == sample.value("eye_world", ""));
				const auto bluePixels = [](const std::filesystem::path &path) {
					std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> image(
						SDL_LoadBMP(path.string().c_str()), SDL_DestroySurface
					);
					REQUIRE(image);
					size_t blue = 0;
					bool readable = true;
					for (int y = 0; y < image->h; ++y)
						for (int x = 0; x < image->w; ++x) {
							Uint8 red = 0, green = 0, colourBlue = 0, alpha = 0;
							readable &=
								SDL_ReadSurfacePixel(image.get(), x, y, &red, &green, &colourBlue, &alpha);
							blue += 2 * colourBlue > 3 * red && 2 * colourBlue > 3 * green;
						}
					REQUIRE(readable);
					return blue;
				};
				const auto before =
					bluePixels(options.CaptureSequence / (std::to_string(frame - 1) + ".bmp"));
				const auto after = bluePixels(stem.string() + ".bmp");
				CAPTURE(before, after);
				REQUIRE(
					before >
					(previousSample->at("portal_handoff").value("destination", "") == "walk.destination" ? 512
																										 : 64)
				);
				// Allow the moving destination character to obscure part of the floor.
				// A plain pane or the source floor cannot supply this blue landmark.
				CHECK(after >= before / 2);
			}
			CHECK(
				sample.at("submitted_move_tick").get<uint64_t>() >=
				previousSample->at("submitted_move_tick").get<uint64_t>()
			);
		}
		if (previousSample && previousSample->contains("portal_handoff") &&
			sample.contains("portal_handoff") &&
			previousSample->at("portal_handoff").value("proceed", false) &&
			sample.at("portal_handoff").value("proceed", false) &&
			previousSample->at("input_world") == sample.at("input_world") &&
			sample.at("tick").get<uint64_t>() > previousSample->at("tick").get<uint64_t>()) {
			CAPTURE(frame);
			CHECK(
				sample.at("submitted_move_tick").get<uint64_t>() >
				previousSample->at("submitted_move_tick").get<uint64_t>()
			);
			++handoffMoveSamples;
		}
		if (sample.contains("portal_handoff") && sample.at("portal_handoff").contains("completed_motion")) {
			const auto &handoff = sample.at("portal_handoff");
			const auto &motion = handoff.at("completed_motion");
			CHECK(
				motion.at("input_tick").get<uint64_t>() <= sample.at("submitted_move_tick").get<uint64_t>()
			);
			CHECK(motion.at("destination_tick").get<uint64_t>() != 0);
			CHECK(motion.at("destination_incarnation").get<uint64_t>() != 0);
			outboundMotion |= handoff.value("destination", "") == "walk.destination";
			returnMotion |= handoff.value("destination", "") == "server.world";
		}
		if (sample.contains("portal_input_history")) {
			const auto &history = sample.at("portal_input_history");
			CHECK(history.at("last_recorded_tick") == sample.at("submitted_move_tick"));
			CHECK(history.at("retained").get<size_t>() <= client::PortalInputHistory::CAPACITY);
			CHECK(history.at("discarded").get<uint64_t>() == 0);
			if (history.at("applied_destination_tick").get<uint64_t>() != 0) {
				REQUIRE(sample.contains("portal_handoff"));
				const auto destination = sample.at("portal_handoff").value("destination", "");
				CHECK(sample.value("prediction_authority_world", "") == destination);
				outboundReplay |= destination == "walk.destination";
				returnReplay |= destination == "server.world";
			}
		}
		if (frame + 1 == options.MaximumFrames) CHECK(sample.value("eye_world", "") == "server.world");
		CHECK(std::filesystem::file_size(stem.string() + ".bmp") > 128 * 128 * 3);
		characterSeen |= sample.value("subject_is_humanoid", false);
		if (sample.value("subject_is_humanoid", false)) {
			CAPTURE(frame);
			CHECK(sample.at("far").get<float>() == scene::Camera{}.FarPlane);
			explicitSubjectSeen |= !sample.at("subject_automatic").get<bool>();
			if (!explicitSubject || explicitSubjectSeen)
				CHECK(sample.at("subject_automatic").get<bool>() == !explicitSubject);
		}
		if (characterSeen) {
			CAPTURE(frame);
			CHECK((sample.value("subject_is_humanoid", false) || clearedFrame));
			if (firstPerson && !clearedFrame) {
				REQUIRE(sample.contains("player_user_id"));
				CHECK(sample.contains("eye_player"));
				if (sample.contains("eye_player"))
					CHECK(sample.at("eye_player") == sample.at("player_user_id"));
			} else {
				CHECK_FALSE(sample.contains("eye_player"));
			}
			if (sample.value("retained_character", false)) {
				CHECK(sample.contains("retained_root"));
				CHECK_FALSE(sample.contains("authoritative_root"));
				REQUIRE(sample.contains("predicted_root"));
				if (previousSample && previousSample->value("retained_character", false) &&
					previousSample->value("input_world", "") == sample.value("input_world", "")) {
					const auto &before = previousSample->at("presented_predicted_root").at("position");
					const auto &after = sample.at("presented_predicted_root").at("position");
					float displacement = 0;
					for (size_t axis = 0; axis < 3; ++axis) {
						const float delta = after.at(axis).get<float>() - before.at(axis).get<float>();
						displacement += delta * delta;
					}
					const auto &velocity = sample.at("predicted_velocity");
					float speedSquared = 0;
					for (const auto &axis : velocity)
						speedSquared += axis.get<float>() * axis.get<float>();
					if (speedSquared > 1 &&
						sample.at("submitted_move_tick") > previousSample->at("submitted_move_tick")) {
						CHECK(displacement > 0.0001f);
						++retainedMoveSamples;
					}
				}
			}
			const float yaw = sample.at("control_angles").at(1).get<float>();
			if (previousYaw) {
				CAPTURE(frame, yaw, *previousYaw);
				CHECK(std::abs(yaw - *previousYaw) < .03f);
			}
			previousYaw = yaw;
			// Foreign eyes render in the persistent viewport world. A missing reply
			// must fail even when no frame ever claims to have a ready image.
			if (sample.value("view_world", "") == "client.world") {
				CAPTURE(frame);
				CHECK(sample.value("eye_image", false));
			}
			// Both flat rooms place the eye at y=4. A retiring source rig must
			// not send the viewer back to the unrelated local demo camera.
			const float eyeHeight = sample.at("camera").at("position").at(1).get<float>();
			maximumEyeHeightError = std::max(maximumEyeHeightError, std::abs(eyeHeight - 4.0f));
			std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> eye(
				SDL_LoadBMP((stem.string() + ".bmp").c_str()), SDL_DestroySurface
			);
			REQUIRE(eye);
			Uint8 red = 0, green = 0, blue = 0, alpha = 0;
			bool visible = false;
			bool readable = true;
			const bool expectBody =
				!firstPerson && !clearedFrame && sample.value("input_world", "") == "client.replica";
			size_t yellowPixels = 0;
			for (int y = eye->h - 1; y >= 0 && (!visible || expectBody); --y) {
				for (int x = 0; x < eye->w && (!visible || expectBody); ++x) {
					readable &= SDL_ReadSurfacePixel(eye.get(), x, y, &red, &green, &blue, &alpha);
					visible |= red != 0 || green != 0 || blue != 0;
					yellowPixels +=
						red > 90 && green > 90 && blue < 70 && red > .7f * green && red < 1.4f * green;
				}
			}
			CAPTURE(frame);
			REQUIRE(readable);
			CHECK(visible);
			// Prediction can carry the body fully beyond the plane before its
			// authority changes worlds. Check that interval as well as adoption.
			if (expectBody) CHECK(yellowPixels > 0);
			if (sample.value("eye_image", false)) {
				REQUIRE(SDL_ReadSurfacePixel(eye.get(), eye->w / 2, eye->h - 1, &red, &green, &blue, &alpha));
				// Both rooms have a floor below the level eye. A resident handle
				// alone is insufficient: the bound image must reach this draw slot.
				CHECK((red != 0 || green != 0 || blue != 0));
			}
		}
		if (imageHandoff && returned && !sample.value("eye_image", false)) {
			for (const auto &portal : sample.at("portal_views")) {
				if (!portal.value("external", false)) continue;
				CAPTURE(frame);
				CHECK(portal.at("image").get<uint64_t>() != 0);
				++nativeReturnSamples;
			}
		}
		previousSample = sample;
	}
	if (lateClear) {
		CHECK(sourceClearSamples > 0);
		CHECK(adoptedClearSamples >= 3);
		CHECK(clearedAdoptions == 1);
	}
	if (imageHandoff) CHECK(nativeReturnSamples > 16);
	CHECK(handoffMoveSamples > 0);
	CHECK(loadingSamples > 0);
	CHECK(retainedMoveSamples > 0);
	if (imageHandoff) CHECK(nativeHeldSamples > 0);
	CHECK(outboundMotion);
	CHECK(returnMotion);
	CHECK(outboundReplay);
	CHECK(returnReplay);
	CHECK(maximumEyeHeightError < 0.05f);
	CHECK(explicitSubjectSeen == explicitSubject);
	std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> capture(
		SDL_LoadBMP(finalCapture.string().c_str()), SDL_DestroySurface
	);
	REQUIRE(capture);
	CHECK(capture->w == options.Width);
	CHECK(capture->h == options.Height);
	size_t visiblePixels = 0;
	bool readable = true;
	for (int y = 0; y < capture->h; ++y)
		for (int x = 0; x < capture->w; ++x) {
			Uint8 red = 0, green = 0, blue = 0, alpha = 0;
			readable &= SDL_ReadSurfacePixel(capture.get(), x, y, &red, &green, &blue, &alpha);
			visiblePixels += red != 0 || green != 0 || blue != 0;
		}
	CHECK(readable);
	CHECK(visiblePixels > static_cast<size_t>(options.Width * options.Height) / 8);
	player.Shutdown();
	REQUIRE(server.RequestStop());
	const auto stopDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	auto stopped = server.Poll();
	while (stopped.Alive() && std::chrono::steady_clock::now() < stopDeadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		stopped = server.Poll();
	}
	CHECK(stopped.Reason == parallel::ExitReason::Exited);
	CHECK(stopped.Code == 0);
	std::lock_guard lock(observed.Mutex);
	CAPTURE(observed.Refusals);
	CHECK(observed.Joined);
	if (lateClear) {
		CHECK(observed.CameraCleared);
		CHECK(observed.CameraRestored);
	}
	REQUIRE(observed.Adoptions.size() == 2);
	CHECK(nativeWorlds.size() == 2);
	if (holdThroughAdoption) CHECK(movingNativeWorlds.size() == 2);
	CHECK(observed.Adoptions[0].starts_with("portal session adopted walk.destination "));
	CHECK(observed.Adoptions[1].starts_with("portal session adopted server.world "));
	CAPTURE(observed.CameraSamples, observed.CameraModeSamples, observed.CameraFailures);
	for (const auto samples : observed.CameraSamples)
		CHECK(samples > 0);
	for (const auto samples : observed.CameraModeSamples)
		CHECK(samples > 0);
	CHECK(observed.CameraFailures.empty());
	CAPTURE(observed.LookSamples, observed.MinimumLookX, observed.MaximumLookX);
	for (size_t stage = 0; stage < 2; ++stage) {
		CHECK(observed.LookSamples[stage] >= 10);
		CHECK(observed.MaximumLookX[stage] - observed.MinimumLookX[stage] > 0.005f);
	}
#if ENGINE_LOG_COMPILED_LEVEL <= ENGINE_LOG_LEVEL_TRACE
	CAPTURE(observed.MoveSamples, observed.LastMoveTick);
	for (const auto samples : observed.MoveSamples)
		CHECK(samples > 0);
	CHECK_FALSE(observed.DuplicateMove);
#endif
}

TEST_CASE("Client walks through product portal images and returns", "[client][portal-product-walk][gpu][.]") {
	const double worldTickRate = GENERATE(30.0, 60.0);
	const bool firstPerson = GENERATE(false, true);
	const bool explicitSubject = GENERATE(false, true);
	const bool holdThroughAdoption = GENERATE(false, true);
	RunPortalWalk(worldTickRate, firstPerson, explicitSubject, holdThroughAdoption, false);
}

TEST_CASE(
	"Client retains a cleared camera through portal adoption and resumes follow",
	"[client][portal-product-camera-clear][gpu][.]"
) {
	const double worldTickRate = GENERATE(30.0, 60.0);
	const bool firstPerson = GENERATE(false, true);
	RunPortalWalk(worldTickRate, firstPerson, true, true, true);
}

TEST_CASE(
	"portal image handoff preserves the destination room landmark",
	"[client][portal-product-image-handoff][gpu][.]"
) {
	const double worldTickRate = GENERATE(30.0, 60.0);
	RunPortalWalk(worldTickRate, false, true, true, true, true);
}

TEST_CASE(
	"player crosses after the initial portal image is available",
	"[client][gpu][portal-product-warm-image-handoff][.]"
) {
	const double worldTickRate = GENERATE(30.0, 60.0);
	RunPortalWalk(worldTickRate, false, true, true, true, true, true);
}
