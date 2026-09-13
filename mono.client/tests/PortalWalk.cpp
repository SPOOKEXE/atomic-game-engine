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

enum class RoomShader { None, Material, Spatial, SpatialOverlay, Lens };
enum class PortalWalkFault { None, Delay, RestartDestinationPresentation, DropAcknowledgement };

static void RunPortalWalk(
	double worldTickRate,
	bool firstPerson,
	bool explicitSubject,
	bool holdThroughAdoption,
	bool lateClear,
	bool imageHandoff = false,
	bool warmPortal = false,
	bool ownedContent = false,
	RoomShader roomShader = RoomShader::None,
	PortalWalkFault fault = PortalWalkFault::None
) {
	using namespace engine;
	const bool authoredShaders = roomShader != RoomShader::None;
	const bool spatialOverlay = roomShader == RoomShader::SpatialOverlay || roomShader == RoomShader::Lens;
	CAPTURE(roomShader);
	CAPTURE(worldTickRate, firstPerson, explicitSubject, holdThroughAdoption, lateClear, warmPortal, fault);
	const auto programs = core::Paths::Base().parent_path();
	const auto serverProgram = programs / "server" / core::Paths::Program("server");
	const auto clientProgram = programs / "client" / core::Paths::Program("client");
	if (!std::filesystem::exists(serverProgram) || !std::filesystem::exists(clientProgram))
		SKIP("build both product programs before this test");
	const auto scenePath = core::Paths::Base() / "portal-client-walk.agame";
	std::string sceneSource =
		std::string("local ownedRoomContent = ") + (ownedContent ? "true\n" : "false\n") + R"(
local floor = Instance.new(ownedRoomContent and "MeshPart" or "Part")
if ownedRoomContent then floor.MeshId = "engine.Cube" end
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
	if (fault != PortalWalkFault::None)
		sceneSource += R"(
local obstruction = Instance.new("Part")
obstruction.Anchored = true
obstruction.Size = Vector3.new(4, 4, 0.2)
obstruction.Position = Vector3.new(0, 4, 5)
obstruction.Parent = workspace
)";
	if (ownedContent)
		sceneSource += R"(
local image = Instance.new("EditableImage")
image.Name = "RoomColour"
image.Parent = workspace
assert(image:Resize(2, 2))
image:DrawRectangle(Vector2.new(0, 0), Vector2.new(2, 2), floor.Color)
floor.Color = Color3.new(1, 1, 1)
floor.TextureID = image.ContentId
)";
	if (authoredShaders)
		sceneSource += R"shader(
local shader = Instance.new("ShaderScript")
shader.Name = "RoomShader"
shader.Source = "#version 450\nlayout(location=0) out vec4 colour;\nvoid main(){colour=" ..
    (game.JobId == "walk.destination" and "vec4(0,0,1,1)" or "vec4(1,0,0,1)") .. ";}"
shader.Parent = workspace
floor.Color = Color3.new(1, 1, 1)
)shader";
	if (roomShader == RoomShader::Material)
		sceneSource += R"(
local material = Instance.new("Material")
material.Shader = shader.Name
material.Parent = floor
)";
	if (roomShader == RoomShader::Spatial || spatialOverlay)
		sceneSource += R"(
local image = Instance.new("EditableImage")
image.Name = "RoomWhite"
image.Parent = workspace
assert(image:Resize(2, 2))
image:DrawRectangle(Vector2.new(0, 0), Vector2.new(2, 2), Color3.new(1, 1, 1))
local canvas = Instance.new("SurfaceGui")
canvas.Face = Enum.NormalId.Top
canvas.CanvasSize = Vector2.new(100, 100)
canvas.AlwaysOnTop = false
canvas.Parent = floor
local picture = Instance.new("ImageLabel")
picture.BackgroundTransparency = 1
picture.Size = UDim2.new(1, 0, 1, 0)
picture.Image = image.ContentId
picture.Shader = "RoomShader"
picture.Parent = canvas
)";
	if (spatialOverlay)
		sceneSource += R"(
if game.JobId == "walk.destination" then
    local anchor = Instance.new("Part")
    anchor.Name = "TopMarker"
    anchor.Anchored = true
    anchor.CanCollide = false
    anchor.Transparency = 1
    anchor.Size = Vector3.new(2, 1, 0.1)
    anchor.Position = Vector3.new(2, 5, -14)
    anchor.Parent = workspace
    local canvas = Instance.new("SurfaceGui")
    canvas.Face = Enum.NormalId.Back
    canvas.CanvasSize = Vector2.new(100, 50)
    canvas.AlwaysOnTop = true
    canvas.LightInfluence = 0
    canvas.Parent = anchor
    local marker = Instance.new("Frame")
    marker.Size = UDim2.new(1, 0, 1, 0)
    marker.BackgroundColor3 = Color3.new(0, 1, 0)
    marker.BorderSizePixel = 0
    marker.Parent = canvas
end
)";
	if (roomShader == RoomShader::Lens)
		sceneSource += R"lens(
local lensShader = Instance.new("LensShader")
lensShader.Name = "RoomLens"
lensShader.Source = [[#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 colour;
layout(set=2,binding=0) uniform sampler2D sceneColour;
layout(set=2,binding=1) uniform sampler2D sceneDepth;
void main(){
    vec2 sampleUv = uv;
    if (uv.x > 0.25 && uv.x < 0.95 && uv.y < 0.75)
        sampleUv.x += ]] .. (game.JobId == "walk.destination" and "-0.0625" or "0.03125") .. [[;
    colour = texture(sceneColour, sampleUv);
}]]
lensShader.Parent = workspace
local lens = Instance.new("ShaderLens")
lens.Name = "RoomLensEffect"
lens.Shader = lensShader.Name
lens.Radius = 100
lens.Strength = 1
lens.Parent = workspace
)lens";
	scene::RegisterSceneClasses();
	script::RegisterScriptComponents();
	world::Universe authored;
	for (const auto *name : {"server.world", "walk.destination"}) {
		const auto id = authored.Create(
			{.Name = core::Name(name),
			 .TickRate = worldTickRate,
			 .GlobalSimulatedNetworkLatency = fault == PortalWalkFault::Delay ? 150.0 : 0.0}
		);
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
        print("portal-camera-distance", authority, distance < 1 and "first" or "third", humanoid.MoveDirection.Magnitude < 0.001 and "rest" or "moving", distance, distance < 6 and "blocked" or "clear", camera.CFrame.LookVector.X)
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
	const auto faultReportPath = core::Paths::Base() / "portal-client-walk-fault-report.txt";
	std::filesystem::remove(faultReportPath);
	std::ofstream(configPath).close();
	const auto localPath = core::Paths::Base() / "portal-client-empty.luau";
	std::ofstream(localPath) << "return\n";
	auto reservation = net::MakeUdpTransport(0);
	REQUIRE(reservation);
	const auto port = reservation->Local().Port;
	reservation->Close();
	parallel::Process server;
	std::vector<std::string> serverArguments{
		"--listen",
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
		"-1"
	};
	if (fault == PortalWalkFault::RestartDestinationPresentation) {
		serverArguments.insert(
			serverArguments.end(), {"--test-restart-presentation-world", "walk.destination"}
		);
	}
	if (fault == PortalWalkFault::DropAcknowledgement)
		serverArguments.emplace_back("--test-drop-next-portal-crossed-acknowledgement");
	if (fault != PortalWalkFault::None) {
		serverArguments.emplace_back("--test-portal-fault-report");
		serverArguments.emplace_back(faultReportPath.string());
	}
	REQUIRE(server.Start(serverProgram, serverArguments));
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
		std::array<size_t, 3> ObstructedSamples{};
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
			const bool blocked = message.find("\tblocked\t") != std::string_view::npos;
			if (blocked) ++observed.ObstructedSamples[stage];
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
			if (message.find(mode) != std::string_view::npos || (!firstPerson && blocked)) {
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
		core::LogLevel PreviousRowLevel;
		~SinkLifetime() {
			std::erase(core::Log::Logger().sinks(), Sink);
			core::Log::SetLevel("client", PreviousClientLevel);
			core::Log::SetLevel("render", PreviousRenderLevel);
			core::Log::SetLevel("replication-row", PreviousRowLevel);
		}
	} attached{
		sink,
		core::Log::LevelOf("client"),
		core::Log::LevelOf("render"),
		core::Log::LevelOf("replication-row")
	};
	core::Log::SetLevel("client", core::LogLevel::Trace);
	core::Log::SetLevel("render", core::LogLevel::Trace);
	if (const char *levels = std::getenv("ATOMIC_ENGINE_LOG_LEVEL");
		levels && std::string_view(levels).find("replication-row=trace") != std::string_view::npos)
		core::Log::SetLevel("replication-row", core::LogLevel::Trace);
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
	options.MaximumFrames = fault == PortalWalkFault::DropAcknowledgement ? 960
							: fault == PortalWalkFault::Delay			  ? 660
																		  : 600;
	options.CaptureSequence =
		core::Paths::Base() /
		("portal-client-walk-" + std::to_string(static_cast<int>(worldTickRate)) +
		 (firstPerson ? "-first" : "-third") + (explicitSubject ? "-explicit" : "") +
		 (holdThroughAdoption ? "-held" : "") +
		 (lateClear ? (imageHandoff ? "-clear-image-frames" : "-clear-frames") : "-frames"));
	if (warmPortal) options.CaptureSequence += "-warm";
	if (ownedContent) options.CaptureSequence += "-owned";
	if (roomShader == RoomShader::Material) options.CaptureSequence += "-shaders";
	if (roomShader == RoomShader::Spatial) options.CaptureSequence += "-spatial-shaders";
	if (roomShader == RoomShader::SpatialOverlay) options.CaptureSequence += "-spatial-overlay";
	if (roomShader == RoomShader::Lens) options.CaptureSequence += "-captured-lens";
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
	std::array<size_t, 2> shaderRoomSamples{};
	size_t shaderObservedSamples = 0;
	size_t spatialOverlayAdoptions = 0;
	size_t observedEyeSamples = 0;
	size_t successorEyeSamples = 0;
	bool returned = false;
	bool outboundMotion = false, returnMotion = false;
	bool outboundReplay = false, returnReplay = false;
	size_t exactFaultCameraSamples = 0;
	size_t faultClipSamples = 0;
	size_t faultVisibleFrames = 0;
	size_t faultBodyFrames = 0;
	size_t faultNonBlackEyeFrames = 0;
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
			if (spatialOverlay &&
				previousSample->at("portal_handoff").value("destination", "") == "walk.destination") {
				++spatialOverlayAdoptions;
				for (const int markerFrame : {frame - 1, frame}) {
					const auto &markerSample = markerFrame == frame ? sample : *previousSample;
					const auto &portals = markerSample.at("portal_views");
					REQUIRE(portals.size() == 1);
					const auto &capture = portals.at(0).at("capture");
					REQUIRE(capture.is_object());
					if (markerFrame != frame) {
						CHECK(capture.at("producer") == "walk.destination");
						CHECK(capture.at("scope") == "opaque-lighting");
						CHECK(capture.at("spatial_overlay_image").get<uint64_t>() != 0);
						if (roomShader == RoomShader::Lens) {
							CHECK(capture.at("lens_count") == 1);
							REQUIRE(capture.at("lenses").size() == 1);
							CHECK(capture.at("lenses").at(0).at("shader") == "RoomLens");
							CHECK(capture.at("lenses").at(0).at("program_hash") != std::string(64, '0'));
							CHECK(std::isfinite(capture.at("lens_time").get<float>()));
						}
					}
					const auto &position = capture.at("position");
					core::CFrame camera({position.at(0), position.at(1), position.at(2)});
					const auto &orientation = capture.at("orientation");
					camera.QuaternionX = orientation.at(0);
					camera.QuaternionY = orientation.at(1);
					camera.QuaternionZ = orientation.at(2);
					camera.QuaternionW = orientation.at(3);
					const auto &frustum = capture.at("frustum");
					const auto path = options.CaptureSequence / (std::to_string(markerFrame) + ".bmp");
					std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> image(
						SDL_LoadBMP(path.string().c_str()), SDL_DestroySurface
					);
					REQUIRE(image);
					// Sample the marker's interior through the accepted destination camera.
					for (const float markerX : {1.6f, 2.0f, 2.4f}) {
						const auto local = camera.PointToObjectSpace({markerX, 5.0f, -13.95f});
						REQUIRE(local.Z < 0);
						const float scale = frustum.at(4).get<float>() / -local.Z;
						const float u = (local.X * scale - frustum.at(0).get<float>()) /
										(frustum.at(1).get<float>() - frustum.at(0).get<float>());
						const float v = (local.Y * scale - frustum.at(2).get<float>()) /
										(frustum.at(3).get<float>() - frustum.at(2).get<float>());
						// Destination shifts right, then the eye world's lens shifts the portal left.
						const int shift =
							roomShader == RoomShader::Lens
								? image->w / 16 - (markerSample.value("eye_world", "") == "server.world"
													   ? image->w / 32
													   : 0)
								: 0;
						const int x = static_cast<int>(u * image->w) + shift;
						const int y = static_cast<int>((1 - v) * image->h);
						CAPTURE(markerFrame, markerX, x, y);
						REQUIRE(x >= 0);
						REQUIRE(x < image->w);
						REQUIRE(y >= 0);
						REQUIRE(y < image->h);
						Uint8 red = 0, green = 0, blue = 0, alpha = 0;
						REQUIRE(SDL_ReadSurfacePixel(image.get(), x, y, &red, &green, &blue, &alpha));
						CAPTURE(red, green, blue);
						CHECK(green > 90);
						CHECK(green > 2 * red);
						CHECK(green > 2 * blue);
						if (roomShader == RoomShader::Lens && markerX == 1.6f) {
							// The interior sample can overlap the shifted marker as the camera approaches.
							// Use a distinct point close to its original left edge for the vacated check.
							const auto edge = camera.PointToObjectSpace({1.2f, 5.0f, -13.95f});
							const float edgeScale = frustum.at(4).get<float>() / -edge.Z;
							const float edgeU = (edge.X * edgeScale - frustum.at(0).get<float>()) /
												(frustum.at(1).get<float>() - frustum.at(0).get<float>());
							const int vacatedX = static_cast<int>(edgeU * image->w);
							CAPTURE(vacatedX);
							REQUIRE(
								SDL_ReadSurfacePixel(image.get(), vacatedX, y, &red, &green, &blue, &alpha)
							);
							CAPTURE(red, green, blue);
							CHECK_FALSE((green > 90 && green > 2 * red && green > 2 * blue));
						}
					}
				}
			}
			if (imageHandoff && fault == PortalWalkFault::None) {
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
				CHECK(
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
			if (fault == PortalWalkFault::None && sample.value("view_world", "") == "client.world") {
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
			if (authoredShaders || (ownedContent && sample.contains("observed_world"))) {
				Uint8 roomRed = 0, roomGreen = 0, roomBlue = 0, roomAlpha = 0;
				REQUIRE(SDL_ReadSurfacePixel(
					eye.get(), eye->w / 8, eye->h - 2, &roomRed, &roomGreen, &roomBlue, &roomAlpha
				));
				const auto eyeWorld = sample.value("observed_world", sample.value("eye_world", ""));
				CAPTURE(frame, roomRed, roomGreen, roomBlue, eyeWorld);
				bool blueRoom = eyeWorld == "walk.destination";
				const auto vector = [](const auto &values) {
					return core::Vector3{
						values.at(0).template get<float>(),
						values.at(1).template get<float>(),
						values.at(2).template get<float>()
					};
				};
				core::CFrame camera(vector(sample.at("camera").at("position")));
				const auto &rotation = sample.at("camera").at("rotation");
				camera.QuaternionX = rotation.at(0);
				camera.QuaternionY = rotation.at(1);
				camera.QuaternionZ = rotation.at(2);
				camera.QuaternionW = rotation.at(3);
				for (const auto &portal : sample.at("portal_views")) {
					if (!portal.contains("capture") || !portal.at("capture").is_object()) continue;
					const auto &frustum = portal.at("capture").at("frustum");
					const float u = (eye->w / 8 + .5f) / eye->w;
					const float v = 1.f - (eye->h - 1.5f) / eye->h;
					const float near = frustum.at(4);
					const auto ray = camera.VectorToWorldSpace(
						{std::lerp(frustum.at(0).get<float>(), frustum.at(1).get<float>(), u) / near,
						 std::lerp(frustum.at(2).get<float>(), frustum.at(3).get<float>(), v) / near,
						 -1}
					);
					const auto normal = vector(portal.at("normal"));
					const float denominator = normal.Dot(ray);
					if (ray.Y >= 0 || std::abs(denominator) < 1e-6f) continue;
					const auto centre = vector(portal.at("centre"));
					const float distance = (centre - camera.Position).Dot(normal) / denominator;
					const auto offset = camera.Position + ray * distance - centre;
					const auto first = vector(portal.at("first")), second = vector(portal.at("second"));
					// The room behind an aperture supplies this pixel if its plane precedes the floor.
					if (distance > 0 && distance < -camera.Position.Y / ray.Y &&
						std::abs(offset.Dot(first)) < first.Dot(first) &&
						std::abs(offset.Dot(second)) < second.Dot(second))
						blueRoom = !blueRoom;
				}
				if (authoredShaders) {
					++shaderRoomSamples[blueRoom ? 1 : 0];
					shaderObservedSamples += sample.contains("observed_world");
				}
				if (blueRoom)
					CHECK(roomBlue > roomRed * 2);
				else
					CHECK(roomRed > roomBlue * 2);
			}
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
			if (fault == PortalWalkFault::None) CHECK(visible);
			if (fault != PortalWalkFault::None && visible) ++faultVisibleFrames;
			// Prediction can carry the body fully beyond the plane before its
			// authority changes worlds. Check that interval as well as adoption.
			if (expectBody && fault == PortalWalkFault::None) CHECK(yellowPixels > 0);
			if (fault != PortalWalkFault::None && expectBody && yellowPixels > 0) ++faultBodyFrames;
			if (sample.value("eye_image", false)) {
				REQUIRE(SDL_ReadSurfacePixel(eye.get(), eye->w / 2, eye->h - 1, &red, &green, &blue, &alpha));
				// Both rooms have a floor below the level eye. A resident handle
				// alone is insufficient: the bound image must reach this draw slot.
				CHECK((red != 0 || green != 0 || blue != 0));
				if (fault != PortalWalkFault::None && (red != 0 || green != 0 || blue != 0))
					++faultNonBlackEyeFrames;
			}
		}
		if (fault != PortalWalkFault::None)
			for (const auto &portal : sample.at("portal_views")) {
				if (!portal.value("external", false) || !portal.contains("capture") ||
					!portal.at("capture").is_object())
					continue;
				const auto &clip = portal.at("capture").at("clip_plane");
				CHECK(clip.size() == 4);
				const float normal = std::abs(clip.at(0).get<float>()) + std::abs(clip.at(1).get<float>()) +
									 std::abs(clip.at(2).get<float>());
				CHECK(normal > .99f);
				++faultClipSamples;
			}
		if (imageHandoff && sample.contains("observed_world")) {
			CHECK_FALSE(sample.value("eye_image", true));
			CHECK(sample.at("view_world") != sample.at("input_world"));
			CHECK(sample.at("observed_tick").get<uint64_t>() > 0);
			CHECK(sample.at("instances").get<size_t>() > 0);
			for (const auto &portal : sample.at("portal_views")) {
				if (!portal.value("external", false)) continue;
				REQUIRE(portal.contains("capture"));
				if (fault != PortalWalkFault::None && !portal.at("capture").is_object()) continue;
				REQUIRE(portal.at("capture").is_object());
				CHECK(portal.at("capture").at("producer") == sample.at("input_world"));
			}
			++observedEyeSamples;
			if (sample.value("observed_successor", false)) {
				REQUIRE(sample.contains("portal_handoff"));
				const auto &handoff = sample.at("portal_handoff");
				CHECK(handoff.value("scene_admitted", false));
				CHECK(handoff.value("scene_joined", false));
				CHECK(handoff.value("scene_live", false));
				CHECK_FALSE(handoff.value("scene_rejected", true));
				CHECK_FALSE(handoff.value("refused", true));
				CHECK(handoff.at("failure").get<std::string>().empty());
				CHECK(sample.at("observed_world") == handoff.at("destination"));
				if (handoff.value("drawing_player", false)) CHECK(handoff.value("ready", false));
				++successorEyeSamples;
			}
		}
		if (imageHandoff && !sample.value("eye_image", false) &&
			(sample.contains("observed_world") || returned)) {
			for (const auto &portal : sample.at("portal_views")) {
				if (!portal.value("external", false)) continue;
				REQUIRE(portal.contains("capture"));
				if (fault != PortalWalkFault::None && !portal.at("capture").is_object()) continue;
				REQUIRE(portal.at("capture").is_object());
				// This fixture's seam mapping is identity apart from wire rounding.
				// A stale camera can pass the room-colour check while exposing a gray border.
				const auto &capturedEye = portal.at("capture").at("position");
				const auto &liveEye = sample.at("camera").at("position");
				float distanceSquared = 0;
				for (size_t axis = 0; axis < 3; ++axis) {
					const float delta = capturedEye.at(axis).get<float>() - liveEye.at(axis).get<float>();
					distanceSquared += delta * delta;
				}
				CHECK(distanceSquared < 0.001f * 0.001f);
				const auto &capturedRotation = portal.at("capture").at("orientation");
				const auto &liveRotation = sample.at("camera").at("rotation");
				float rotationDot = 0;
				for (size_t axis = 0; axis < 4; ++axis)
					rotationDot +=
						capturedRotation.at(axis).get<float>() * liveRotation.at(axis).get<float>();
				CHECK(std::abs(rotationDot) > 1.0f - 1e-6f);
				if (fault != PortalWalkFault::None) ++exactFaultCameraSamples;
			}
		}
		if (imageHandoff && returned && !sample.value("eye_image", false)) {
			for (const auto &portal : sample.at("portal_views")) {
				if (!portal.value("external", false)) continue;
				CAPTURE(frame);
				CHECK(portal.at("image").get<uint64_t>() != 0);
				REQUIRE(portal.contains("capture"));
				CHECK(portal.at("capture").at("scope") == "complete-world");
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
	if (imageHandoff && !firstPerson) CHECK(observedEyeSamples > 0);
	if (imageHandoff) CHECK(successorEyeSamples > 0);
	CHECK(handoffMoveSamples > 0);
	CHECK(loadingSamples > 0);
	CHECK(retainedMoveSamples > 0);
	if (imageHandoff) CHECK(nativeHeldSamples > 0);
	CHECK(outboundMotion);
	CHECK(returnMotion);
	CHECK(outboundReplay);
	CHECK(returnReplay);
	if (fault != PortalWalkFault::None) {
		CHECK(exactFaultCameraSamples > 0);
		CHECK(faultClipSamples > 16);
		CHECK(faultVisibleFrames > 16);
		CHECK(faultBodyFrames > 0);
		if (imageHandoff) CHECK(faultNonBlackEyeFrames > 0);
	}
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
	if (authoredShaders) {
		CHECK(shaderRoomSamples[0] > 10);
		CHECK(shaderRoomSamples[1] > 10);
		CHECK(shaderObservedSamples > 0);
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
	if (fault != PortalWalkFault::None) {
		std::ifstream report(faultReportPath);
		const std::string markers(std::istreambuf_iterator<char>(report), {});
		if (fault == PortalWalkFault::RestartDestinationPresentation)
			CHECK(markers.find("restarted-presentation-producer") != std::string::npos);
		if (fault == PortalWalkFault::DropAcknowledgement) {
			CHECK(markers.find("dropped-lease-adopted") != std::string::npos);
			CHECK(markers.find("recovered-lease-adopted") != std::string::npos);
		}
	}
	std::lock_guard lock(observed.Mutex);
	CAPTURE(observed.Refusals);
	CHECK(observed.Joined);
	if (lateClear) {
		CHECK(observed.CameraCleared);
		CHECK(observed.CameraRestored);
	}
	REQUIRE(observed.Adoptions.size() == 2);
	CHECK(nativeWorlds.size() == 2);
	if (holdThroughAdoption && fault != PortalWalkFault::Delay) CHECK(movingNativeWorlds.size() == 2);
	if (spatialOverlay) CHECK(spatialOverlayAdoptions == 1);
	CHECK(observed.Adoptions[0].starts_with("portal session adopted walk.destination "));
	CHECK(observed.Adoptions[1].starts_with("portal session adopted server.world "));
	CAPTURE(observed.CameraSamples, observed.CameraModeSamples, observed.CameraFailures);
	for (const auto samples : observed.CameraSamples)
		CHECK(samples > 0);
	for (const auto samples : observed.CameraModeSamples)
		CHECK(samples > 0);
	if (fault != PortalWalkFault::None) {
		CHECK(observed.ObstructedSamples[0] > 0);
		CHECK(
			observed.ObstructedSamples[0] + observed.ObstructedSamples[1] + observed.ObstructedSamples[2] > 16
		);
	}
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

TEST_CASE(
	"faulted product portal round trips retain humanoid camera continuity",
	"[client][portal-product-fault-roundtrip][gpu][.]"
) {
	const auto fault = GENERATE(
		PortalWalkFault::Delay,
		PortalWalkFault::RestartDestinationPresentation,
		PortalWalkFault::DropAcknowledgement
	);
	RunPortalWalk(60, false, true, true, false, true, false, false, RoomShader::None, fault);
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

TEST_CASE(
	"portal worlds retain their editable room colours across adoption",
	"[client][gpu][portal-product-content-owner][.]"
) {
	RunPortalWalk(60, false, true, true, true, true, true, true);
}

TEST_CASE(
	"portal worlds resolve same-name authored shaders across both adoptions",
	"[client][gpu][portal-product-shader-owner][.]"
) {
	RunPortalWalk(60, false, true, true, true, true, true, false, RoomShader::Material);
}

TEST_CASE(
	"portal worlds resolve same-name spatial GUI shaders across both adoptions",
	"[client][gpu][portal-product-spatial-shader-owner][.]"
) {
	RunPortalWalk(60, false, true, true, true, true, true, false, RoomShader::Spatial);
}

TEST_CASE(
	"portal destination top GUI survives ordered capture and adoption",
	"[client][gpu][portal-product-spatial-overlay][.]"
) {
	RunPortalWalk(60, false, true, true, true, true, true, false, RoomShader::SpatialOverlay);
}

TEST_CASE(
	"product crossing applies captured destination lens to the spatial marker",
	"[client][gpu][portal-product-captured-lens][.]"
) {
	RunPortalWalk(60, false, true, true, true, true, true, false, RoomShader::Lens);
}
