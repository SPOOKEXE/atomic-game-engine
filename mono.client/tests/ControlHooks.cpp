#include "ControlHooks.hpp"

#include "../../mono.engine/control/tests/HookFixture.hpp"

#include <engine/control/Surface.hpp>
#include <engine/control/features/DataScene.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/ScriptDataCaptureBridge.hpp>
#include <engine/script/DataCaptureBridge.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/DataFactory.hpp>
#include <engine/world/Universe.hpp>

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <client/DataAudioObservation.hpp>
#include <memory>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

TEST_SUITE_ID("client.controlhooks")
TEST_DEPENDS("engine.graph.pipelinedocument")
TEST_DEPENDS("engine.render.passes")

namespace {
	nlohmann::json
	McpCall(engine::control::Surface &surface, std::string_view name, nlohmann::json arguments) {
		return nlohmann::json::parse(surface.Answer(
			nlohmann::json{
				{"jsonrpc", "2.0"},
				{"id", 1},
				{"method", "tools/call"},
				{"params", {{"name", name}, {"arguments", std::move(arguments)}}}
			}.dump()
		));
	}

	bool Listed(engine::control::Surface &surface, std::string_view name) {
		const auto reply = nlohmann::json::parse(
			surface.Answer(nlohmann::json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}}.dump())
		);
		return std::any_of(
			reply["result"]["tools"].begin(), reply["result"]["tools"].end(), [name](const auto &row) {
				return row["name"] == name;
			}
		);
	}

	struct VideoSubsystem {
		bool Started = SDL_Init(SDL_INIT_VIDEO);

		~VideoSubsystem() {
			if (Started) SDL_QuitSubSystem(SDL_INIT_VIDEO);
		}
	};

	struct CaptureHookLeases {
		engine::control::HookLease Lifecycle;
		engine::control::HookLease Capture;
	};

	CaptureHookLeases ActivateCaptureHooks(
		engine::control::Surface &surface,
		engine::world::DataFactorySession &session,
		const std::shared_ptr<engine::render::ScriptDataCaptureBridge> &bridge,
		std::string &failure
	) {
		auto lifecycle = surface.ActivateHook(
			{.Id = "client.data-factory-lifecycle",
			 .Revision = "v1",
			 .Purpose = "test lifecycle",
			 .Dependencies = {},
			 .Limits = {}},
			[&surface, &session](engine::control::HookRegistration &) {
				surface.AddDataFactoryTools(session);
			},
			failure
		);
		REQUIRE(lifecycle.IsValid());
		auto capture = client::ActivateDataCaptureHook(
			{.Surface = surface, .Session = session, .Bridge = bridge}, failure
		);
		REQUIRE(failure.empty());
		REQUIRE(capture.IsValid());
		return {.Lifecycle = std::move(lifecycle), .Capture = std::move(capture)};
	}

	std::string
	PauseAndSnapshot(engine::world::Universe &worlds, engine::world::DataFactorySession &session) {
		const engine::world::WorldId world = worlds.Create({.Name = engine::core::Name("data-world")});
		session.SetPauseParticipant(
			[world](
				engine::world::WorldId candidate, engine::world::DataFactoryPauseScope, bool, std::string &
			) { return candidate == world; }
		);
		REQUIRE(
			session.Pause("data-world", engine::world::DataFactoryPauseScope::AllSystems, 0).Status ==
			engine::world::DataFactoryStatus::Ok
		);
		std::string snapshot;
		REQUIRE(session.Snapshot("data-world", snapshot).Status == engine::world::DataFactoryStatus::Ok);
		return snapshot;
	}

	engine::graph::RenderGraph DataCaptureGraph() {
		engine::graph::RenderGraph graph;
		engine::core::Name offender;
		REQUIRE(
			engine::graph::Build(engine::graph::DefaultPbrDataCaptureDocument(), graph, offender) ==
			engine::graph::PipelineDocumentStatus::Ok
		);
		return graph;
	}

	engine::script::DataCaptureBridgeRequest
	CaptureRequest(std::string_view snapshot, std::string_view pipeline) {
		return {
			.InstanceId = "data-world",
			.SnapshotId = std::string(snapshot),
			.Pipeline = std::string(pipeline),
			.CaptureNode = "data-capture",
			.Channels = {"rgb_linear_hdr"},
			.LocalLightIds = {},
			.TemporalHistory = "preserve",
			.PackedPlanes = {},
		};
	}

	engine::render::View
	CaptureView(std::string_view snapshot, engine::core::Name pipeline, engine::render::SceneTarget &target) {
		engine::render::View view;
		view.WorldName = engine::core::Name("data-world");
		view.Pipeline = pipeline;
		view.Target = &target;
		view.SnapshotId = snapshot;
		return view;
	}
}

TEST_CASE("client visibility observation hook owns its row for the lease lifetime", "[client][control]") {
	engine::control::Surface surface("client", "client hook test");
	std::string failure;
	auto lease = client::ActivateVisibilityObservationHook(
		{.Hooks = surface.Hooks(),
		 .Snapshot =
			 [] {
				 return engine::control::features::VisibilitySnapshotReply{
					 .Frame = 13,
					 .World = {},
					 .Valid = true,
					 .Observations = {},
				 };
			 }},
		failure
	);

	REQUIRE(failure.empty());
	REQUIRE(lease.IsValid());
	const auto visible =
		std::find_if(surface.Registered().begin(), surface.Registered().end(), [](const auto &tool) {
			return tool.Name == "visibility_observations";
		});
	REQUIRE(visible != surface.Registered().end());
	const auto reply = visible->Call(nlohmann::json::object(), failure);
	REQUIRE(failure.empty());
	CHECK(reply["frame"] == 13);
	CHECK(reply["valid"] == true);

	lease.Close();
	CHECK(surface.Hooks().Active().empty());
	CHECK(std::none_of(surface.Registered().begin(), surface.Registered().end(), [](const auto &tool) {
		return tool.Name == "visibility_observations";
	}));
}

TEST_CASE("client visibility observation hook refuses a missing renderer provider", "[client][control]") {
	engine::control::Surface surface("client", "client hook test");
	std::string failure;
	const auto lease =
		client::ActivateVisibilityObservationHook({.Hooks = surface.Hooks(), .Snapshot = {}}, failure);

	CHECK_FALSE(lease.IsValid());
	CHECK(failure == "visibility snapshot provider is required");
	CHECK(surface.Registered().empty());
}

TEST_CASE("client temporal and rig hooks own their session-fenced readers", "[client][control]") {
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	engine::control::Surface surface("client", "client hook test");
	std::string failure;
	auto temporal = client::ActivateTemporalSampleHook(
		{.Hooks = surface.Hooks(), .Universe = worlds, .Session = session}, failure
	);
	REQUIRE(failure.empty());
	REQUIRE(temporal.IsValid());
	auto rig = client::ActivateRigExportHook(
		{.Hooks = surface.Hooks(), .Universe = worlds, .Session = session}, failure
	);
	REQUIRE(failure.empty());
	REQUIRE(rig.IsValid());

	const auto temporalTool =
		std::find_if(surface.Registered().begin(), surface.Registered().end(), [](const auto &tool) {
			return tool.Name == "get_temporal_sample";
		});
	REQUIRE(temporalTool != surface.Registered().end());
	CHECK(temporalTool->Call(nlohmann::json::object(), failure).is_null());
	CHECK_FALSE(failure.empty());
	const auto rigTool =
		std::find_if(surface.Registered().begin(), surface.Registered().end(), [](const auto &tool) {
			return tool.Name == "get_rig_export";
		});
	REQUIRE(rigTool != surface.Registered().end());
	CHECK(rigTool->Call(nlohmann::json::object(), failure).is_null());
	CHECK_FALSE(failure.empty());

	temporal.Close();
	rig.Close();
	CHECK(surface.Hooks().Active().empty());
	CHECK(std::none_of(surface.Registered().begin(), surface.Registered().end(), [](const auto &tool) {
		return tool.Name == "get_temporal_sample" || tool.Name == "get_rig_export";
	}));
}

TEST_CASE("client audio observation hook publishes and removes its coordinated rows", "[client][control]") {
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	engine::control::Surface surface("client", "client hook test");
	std::string failure;
	auto bridge = std::make_shared<client::DataAudioObservationHost>();
	auto lease = client::ActivateDataAudioObservationHook(
		{.Hooks = surface.Hooks(), .Universe = worlds, .Bridge = bridge, .Session = session}, failure
	);

	REQUIRE(failure.empty());
	REQUIRE(lease.IsValid());
	CHECK(surface.Readable().size() == 1);
	CHECK(surface.Registered().size() == 2);
	const auto observation =
		std::find_if(surface.Registered().begin(), surface.Registered().end(), [](const auto &tool) {
			return tool.Name == "get_audio_observation";
		});
	REQUIRE(observation != surface.Registered().end());
	CHECK(observation->Call(nlohmann::json::object(), failure).is_null());
	CHECK_FALSE(failure.empty());

	lease.Close();
	CHECK(surface.Hooks().Active().empty());
	CHECK(surface.Readable().empty());
	CHECK(surface.Registered().empty());
}

TEST_CASE("client audio observation hook refuses a missing bridge", "[client][control]") {
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	engine::control::Surface surface("client", "client hook test");
	std::string failure;
	const auto lease = client::ActivateDataAudioObservationHook(
		{.Hooks = surface.Hooks(), .Universe = worlds, .Bridge = {}, .Session = session}, failure
	);

	CHECK_FALSE(lease.IsValid());
	CHECK(failure == "audio observation bridge is required");
	CHECK(surface.Readable().empty());
	CHECK(surface.Registered().empty());
}

TEST_CASE("client scene-rendering hook owns camera calibration row", "[client][control]") {
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	engine::control::Surface surface("client", "client hook test");
	std::string failure;
	auto lifecycle = surface.ActivateHook(
		{.Id = "client.data-factory-lifecycle",
		 .Revision = "v1",
		 .Purpose = "test lifecycle",
		 .Dependencies = {},
		 .Limits = {}},
		[&surface, &session](engine::control::HookRegistration &) { surface.AddDataFactoryTools(session); },
		failure
	);
	REQUIRE(lifecycle.IsValid());

	auto sceneRendering = surface.ActivateHook(
		{.Id = "client.scene-rendering",
		 .Revision = "v1",
		 .Purpose = "test camera calibration",
		 .Dependencies = {"client.data-factory-lifecycle"},
		 .Limits = {}},
		[&worlds, &session](engine::control::HookRegistration &registration) {
			registration.Add(engine::control::features::CameraRenderingDataTool(worlds, &session));
		},
		failure
	);
	REQUIRE(failure.empty());
	REQUIRE(sceneRendering.IsValid());
	CHECK(std::any_of(surface.Registered().begin(), surface.Registered().end(), [](const auto &tool) {
		return tool.Name == "get_camera_rendering_data";
	}));

	sceneRendering.Close();
	CHECK(std::none_of(surface.Registered().begin(), surface.Registered().end(), [](const auto &tool) {
		return tool.Name == "get_camera_rendering_data";
	}));
	lifecycle.Close();
}

TEST_CASE("scoped client factory hooks retain lifecycle rows until dependants close", "[client][control]") {
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	engine::control::Surface surface("client", "client hook test");
	std::string failure;
	auto lifecycle = surface.ActivateHook(
		{.Id = "client.lifecycle",
		 .Revision = "v1",
		 .Purpose = "test lifecycle",
		 .Dependencies = {},
		 .Limits = {}},
		[&surface, &session](engine::control::HookRegistration &) { surface.AddDataFactoryTools(session); },
		failure
	);
	REQUIRE(lifecycle.IsValid());
	auto scene = surface.ActivateHook(
		{.Id = "client.scene",
		 .Revision = "v1",
		 .Purpose = "test scene",
		 .Dependencies = {"client.lifecycle"},
		 .Limits = {}},
		[&surface](engine::control::HookRegistration &) {
			surface.Add(
				engine::control::Tool{
					"scene_probe", "test", nullptr, [](const nlohmann::json &, std::string &) {
						return nlohmann::json{};
					}
				}
			);
		},
		failure
	);
	REQUIRE(scene.IsValid());
	lifecycle.Close();
	CHECK(surface.Hooks().Active().size() == 2);
	scene.Close();
	CHECK(surface.Hooks().Active().empty());
	CHECK(surface.Registered().empty());
}

TEST_CASE("client capture hook cancels pending tickets before release", "[client][control]") {
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	engine::render::Renderer renderer;
	auto bridge = std::make_shared<engine::render::ScriptDataCaptureBridge>(session, renderer);
	engine::control::Surface surface("client", "client hook test");
	std::string failure;
	auto lifecycle = surface.ActivateHook(
		{.Id = "client.data-factory-lifecycle",
		 .Revision = "v1",
		 .Purpose = "test lifecycle",
		 .Dependencies = {},
		 .Limits = {}},
		[&surface, &session](engine::control::HookRegistration &) { surface.AddDataFactoryTools(session); },
		failure
	);
	REQUIRE(lifecycle.IsValid());
	auto captureHook =
		client::ActivateDataCaptureHook({.Surface = surface, .Session = session, .Bridge = bridge}, failure);
	REQUIRE(failure.empty());
	REQUIRE(captureHook.IsValid());
	CHECK_FALSE(surface.CaptureAvailability().Channels.empty());

	engine::script::DataCaptureBridgeRequest request{
		.InstanceId = "data-world",
		.SnapshotId = "snapshot-1",
		.Pipeline = "pipeline",
		.CaptureNode = "capture",
		.Channels = {"rgb_linear_hdr"},
		.LocalLightIds = {},
		.TemporalHistory = "preserve",
		.PackedPlanes = {},
	};
	uint64_t captureTicket = 0;
	REQUIRE(bridge->Queue("data-world", request, captureTicket, failure));
	engine::script::ViewCameraMutationRequest mutationRequest;
	mutationRequest.InstanceId = "data-world";
	mutationRequest.SnapshotId = "snapshot-1";
	mutationRequest.Pipeline = "pipeline";
	mutationRequest.PipelineRevision = 1;
	mutationRequest.Lens = engine::script::ViewCameraMutationRequest::Camera{
		.FieldOfViewRadians = 0.9f,
		.NearPlane = 0.2f,
		.FarPlane = 100.0f,
	};
	uint64_t mutationTicket = 0;
	REQUIRE(bridge->QueueViewCameraMutation("data-world", mutationRequest, mutationTicket, failure));
	REQUIRE(bridge->HasPending());

	// Closing a dependency first must leave it draining until capture closes.
	lifecycle.Close();
	const auto active = surface.Hooks().Active();
	REQUIRE(active.size() == 2);
	const auto drainingLifecycle = std::find_if(active.begin(), active.end(), [](const auto &hook) {
		return hook.Descriptor.Id == "client.data-factory-lifecycle";
	});
	REQUIRE(drainingLifecycle != active.end());
	CHECK(drainingLifecycle->State == engine::control::HookState::Draining);

	captureHook.Close();
	captureHook.Close();
	surface.PumpHooks();
	CHECK_FALSE(bridge->HasPending());
	CHECK(bridge->HasOutstanding());
	CHECK(surface.Hooks().Active().size() == 2);
	CHECK_FALSE(Listed(surface, "capture"));
	CHECK(Listed(surface, "poll_capture"));
	CHECK(Listed(surface, "release_capture"));
	CHECK_FALSE(surface.CaptureAvailability().Available);
	CHECK(surface.CaptureAvailability().Channels.empty());

	const auto capture =
		McpCall(surface, "poll_capture", {{"instance_id", "data-world"}, {"ticket", captureTicket}});
	CHECK_FALSE(capture["result"]["isError"].get<bool>());
	CHECK(McpCall(surface, "capture", nlohmann::json::object())["result"]["isError"] == true);
	const auto mutation = McpCall(
		surface, "poll_view_camera_mutation", {{"instance_id", "data-world"}, {"ticket", mutationTicket}}
	);
	CHECK_FALSE(mutation["result"]["isError"].get<bool>());
	const auto released = McpCall(
		surface,
		"release_capture",
		{{"instance_id", "data-world"},
		 {"ticket", captureTicket},
		 {"operation_id", "release-draining-capture"}}
	);
	CHECK_FALSE(released["result"]["isError"].get<bool>());
	CHECK_FALSE(bridge->HasOutstanding());
	CHECK(surface.Hooks().Active().empty());
	CHECK(surface.Registered().empty());
}

TEST_CASE("client capture hook drains a full bounded queue across repeated closes", "[client][control]") {
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	engine::render::Renderer renderer;
	auto bridge = std::make_shared<engine::render::ScriptDataCaptureBridge>(session, renderer);
	engine::control::Surface surface("client", "client hook test");
	std::string failure;
	auto lifecycle = surface.ActivateHook(
		{.Id = "client.data-factory-lifecycle",
		 .Revision = "v1",
		 .Purpose = "test lifecycle",
		 .Dependencies = {},
		 .Limits = {}},
		[&surface, &session](engine::control::HookRegistration &) { surface.AddDataFactoryTools(session); },
		failure
	);
	REQUIRE(lifecycle.IsValid());
	const auto request = CaptureRequest("snapshot-1", "pipeline");
	constexpr size_t cycles = 16;
	constexpr size_t ticketLimit = 6;

	for (size_t cycle = 0; cycle < cycles; ++cycle) {
		failure.clear();
		auto capture = client::ActivateDataCaptureHook(
			{.Surface = surface, .Session = session, .Bridge = bridge}, failure
		);
		REQUIRE(failure.empty());
		REQUIRE(capture.IsValid());
		std::array<uint64_t, ticketLimit> tickets{};
		for (uint64_t &ticket : tickets)
			REQUIRE(bridge->Queue("data-world", request, ticket, failure));
		uint64_t refusedTicket = 0;
		CHECK_FALSE(bridge->Queue("data-world", request, refusedTicket, failure));
		CHECK(failure == "capture queue is full; release a terminal capture");

		capture.Close();
		surface.PumpHooks();
		REQUIRE_FALSE(bridge->HasPending());
		REQUIRE(bridge->HasOutstanding());
		CHECK_FALSE(Listed(surface, "capture"));
		CHECK(Listed(surface, "poll_capture"));
		CHECK(Listed(surface, "release_capture"));
		CHECK_FALSE(bridge->Queue("data-world", request, refusedTicket, failure));

		for (size_t index = 0; index < tickets.size(); ++index) {
			const uint64_t ticket = tickets[index];
			engine::script::DataCaptureBridgePoll poll;
			REQUIRE(bridge->Poll("data-world", ticket, poll, failure));
			CHECK(poll.Status == "cancelled");
			const auto mcpPoll =
				McpCall(surface, "poll_capture", {{"instance_id", "data-world"}, {"ticket", ticket}});
			CHECK_FALSE(mcpPoll["result"]["isError"].get<bool>());
			const auto released = McpCall(
				surface,
				"release_capture",
				{{"instance_id", "data-world"},
				 {"ticket", ticket},
				 {"operation_id", "stress-release-" + std::to_string(cycle) + "-" + std::to_string(index)}}
			);
			REQUIRE_FALSE(released["result"]["isError"].get<bool>());
			CHECK(bridge->HasOutstanding() == (index + 1 < tickets.size()));
		}
		surface.PumpHooks();
		CHECK_FALSE(capture.IsValid());
		CHECK_FALSE(Listed(surface, "poll_capture"));
		CHECK_FALSE(Listed(surface, "release_capture"));
		CHECK(surface.Hooks().Active().size() == 1);
	}
	lifecycle.Close();
	CHECK(surface.Hooks().Active().empty());
	CHECK_FALSE(bridge->HasOutstanding());
}

TEST_CASE("client capture hook closes after terminal capture release", "[client][control]") {
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	engine::render::Renderer renderer;
	auto bridge = std::make_shared<engine::render::ScriptDataCaptureBridge>(session, renderer);
	engine::control::Surface surface("client", "client hook test");
	std::string failure;
	auto lifecycle = surface.ActivateHook(
		{.Id = "client.data-factory-lifecycle",
		 .Revision = "v1",
		 .Purpose = "test lifecycle",
		 .Dependencies = {},
		 .Limits = {}},
		[&surface, &session](engine::control::HookRegistration &) { surface.AddDataFactoryTools(session); },
		failure
	);
	REQUIRE(lifecycle.IsValid());
	auto captureHook =
		client::ActivateDataCaptureHook({.Surface = surface, .Session = session, .Bridge = bridge}, failure);
	REQUIRE(failure.empty());
	REQUIRE(captureHook.IsValid());

	engine::script::DataCaptureBridgeRequest request{
		.InstanceId = "data-world",
		.SnapshotId = "snapshot-1",
		.Pipeline = "pipeline",
		.CaptureNode = "capture",
		.Channels = {"rgb_linear_hdr"},
		.LocalLightIds = {},
		.TemporalHistory = "preserve",
		.PackedPlanes = {},
	};
	uint64_t captureTicket = 0;
	REQUIRE(bridge->Queue("data-world", request, captureTicket, failure));
	engine::script::ViewCameraMutationRequest mutationRequest;
	mutationRequest.InstanceId = "data-world";
	mutationRequest.SnapshotId = "snapshot-1";
	mutationRequest.Pipeline = "pipeline";
	mutationRequest.PipelineRevision = 1;
	mutationRequest.Lens = engine::script::ViewCameraMutationRequest::Camera{
		.FieldOfViewRadians = 0.9f,
		.NearPlane = 0.2f,
		.FarPlane = 100.0f,
	};
	uint64_t mutationTicket = 0;
	REQUIRE(bridge->QueueViewCameraMutation("data-world", mutationRequest, mutationTicket, failure));
	bridge->CancelPending();
	bridge->Pump();
	engine::script::DataCaptureBridgePoll capture;
	REQUIRE(bridge->Poll("data-world", captureTicket, capture, failure));
	CHECK(capture.Status == "cancelled");
	REQUIRE(bridge->Release("data-world", captureTicket, failure));

	captureHook.Close();
	captureHook.Close();
	surface.PumpHooks();
	CHECK(surface.Hooks().Active().size() == 2);
	CHECK_FALSE(surface.CaptureAvailability().Available);
	CHECK(surface.CaptureAvailability().Channels.empty());
	CHECK_FALSE(Listed(surface, "capture"));
	CHECK_FALSE(bridge->HasPending());
	CHECK(bridge->HasOutstanding());
	CHECK_FALSE(bridge->Poll("data-world", captureTicket, capture, failure));
	engine::script::ViewCameraMutationPoll mutation;
	REQUIRE(bridge->PollViewCameraMutation("data-world", mutationTicket, mutation, failure));
	CHECK(mutation.Terminal);
	CHECK(mutation.Status == "cancelled");
	surface.PumpHooks();
	CHECK(surface.Hooks().Active().size() == 1);
	lifecycle.Close();
	CHECK(surface.Hooks().Active().empty());
}

TEST_CASE("client capture hook close preserves a failed ticket", "[client][control]") {
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	const std::string snapshot = PauseAndSnapshot(worlds, session);
	engine::render::Renderer renderer;
	auto graph = DataCaptureGraph();
	const engine::core::Name pipeline("client-capture-failed-close");
	REQUIRE(renderer.SetPipeline(pipeline, graph));
	auto bridge = std::make_shared<engine::render::ScriptDataCaptureBridge>(session, renderer);
	engine::control::Surface surface("client", "client hook test");
	std::string failure;
	auto leases = ActivateCaptureHooks(surface, session, bridge, failure);

	uint64_t ticket = 0;
	REQUIRE(bridge->Queue("data-world", CaptureRequest(snapshot, pipeline.Text()), ticket, failure));
	engine::render::SceneTarget target{32, 32};
	auto view = CaptureView(snapshot, pipeline, target);
	engine::render::ScriptDataCaptureBridge::PreparedView prepared;
	CHECK_FALSE(bridge->PrepareView(view, &prepared));
	REQUIRE(prepared.Captures == std::vector<uint64_t>{ticket});
	CHECK_FALSE(bridge->HasPending());
	engine::script::DataCaptureBridgePoll reply;
	REQUIRE(bridge->Poll("data-world", ticket, reply, failure));
	CHECK(reply.Status == "failed");

	leases.Capture.Close();
	leases.Capture.Close();
	surface.PumpHooks();
	CHECK(bridge->HasOutstanding());
	CHECK(Listed(surface, "poll_capture"));
	CHECK_FALSE(Listed(surface, "capture"));
	CHECK_FALSE(surface.CaptureAvailability().Available);
	CHECK(surface.CaptureAvailability().Channels.empty());
	CHECK_FALSE(Listed(surface, "capture"));
	const auto polled = McpCall(surface, "poll_capture", {{"instance_id", "data-world"}, {"ticket", ticket}});
	CHECK_FALSE(polled["result"]["isError"].get<bool>());
	const auto released = McpCall(
		surface,
		"release_capture",
		{{"instance_id", "data-world"}, {"ticket", ticket}, {"operation_id", "release-failed-ticket"}}
	);
	CHECK_FALSE(released["result"]["isError"].get<bool>());
	surface.PumpHooks();
	CHECK_FALSE(bridge->HasOutstanding());
	leases.Lifecycle.Close();
}

TEST_CASE("client capture hook close preserves ready ticket resources", "[client][gpu][data-capture][.]") {
	VideoSubsystem video;
	REQUIRE(video.Started);
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	const std::string snapshot = PauseAndSnapshot(worlds, session);
	engine::render::Renderer renderer;
	REQUIRE(renderer.Initialise(nullptr));
	REQUIRE(renderer.IsHeadless());
	auto graph = DataCaptureGraph();
	const engine::core::Name pipeline("client-capture-ready-close");
	REQUIRE(renderer.SetPipeline(pipeline, graph));
	auto bridge = std::make_shared<engine::render::ScriptDataCaptureBridge>(session, renderer);
	engine::control::Surface surface("client", "client hook test");
	std::string failure;
	auto lifecycle = surface.ActivateHook(
		{.Id = "client.data-factory-lifecycle",
		 .Revision = "v1",
		 .Purpose = "test lifecycle",
		 .Dependencies = {},
		 .Limits = {}},
		[&surface, &session](engine::control::HookRegistration &) { surface.AddDataFactoryTools(session); },
		failure
	);
	REQUIRE(lifecycle.IsValid());

	constexpr size_t CAPTURE_DRAIN_CYCLES = 3;
	size_t retainedBytes = 0;
	for (size_t cycle = 0; cycle < CAPTURE_DRAIN_CYCLES; ++cycle) {
		auto captureHook = client::ActivateDataCaptureHook(
			{.Surface = surface, .Session = session, .Bridge = bridge}, failure
		);
		REQUIRE(failure.empty());
		REQUIRE(captureHook.IsValid());
		REQUIRE(surface.CaptureAvailability().Available);

		uint64_t ticket = 0;
		REQUIRE(bridge->Queue("data-world", CaptureRequest(snapshot, pipeline.Text()), ticket, failure));
		engine::render::SceneTarget target{32, 32};
		auto view = CaptureView(snapshot, pipeline, target);
		engine::render::ScriptDataCaptureBridge::PreparedView prepared;
		CHECK_FALSE(bridge->PrepareView(view, &prepared));
		REQUIRE(prepared.Captures == std::vector<uint64_t>{ticket});
		engine::render::OverlayImage overlay;
		REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Submitted);

		engine::script::DataCaptureBridgePoll reply;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		do {
			bridge->Pump();
			REQUIRE(bridge->Poll("data-world", ticket, reply, failure));
			if (reply.Status == "pending") SDL_Delay(1);
		} while (reply.Status == "pending" && std::chrono::steady_clock::now() < deadline);
		REQUIRE(reply.Status == "ready");
		REQUIRE(reply.Planes.size() == 1);
		CHECK(reply.Profile.RetainedBytes == reply.Planes.front().ByteSize);
		CHECK(reply.Profile.RetainedOperations == reply.Planes.size());
		CHECK(reply.Profile.ReadbackBytes == reply.Profile.RetainedBytes);
		CHECK(reply.Profile.ReadbackOperations == reply.Profile.RetainedOperations);
		retainedBytes += reply.Profile.RetainedBytes;

		captureHook.Close();
		captureHook.Close();
		surface.PumpHooks();
		CHECK_FALSE(bridge->HasPending());
		CHECK(bridge->HasOutstanding());
		CHECK(Listed(surface, "poll_capture"));
		CHECK(Listed(surface, "get_resource"));
		CHECK_FALSE(Listed(surface, "capture"));
		CHECK_FALSE(surface.CaptureAvailability().Available);
		CHECK(surface.CaptureAvailability().Channels.empty());
		if (cycle + 1 == CAPTURE_DRAIN_CYCLES) {
			const auto world = worlds.Find(engine::core::Name("data-world"));
			REQUIRE(world.IsValid());
			REQUIRE(worlds.Destroy(world) == engine::world::WorldStatus::Ok);
			CHECK_FALSE(worlds.Find(engine::core::Name("data-world")).IsValid());
		}

		const auto polled =
			McpCall(surface, "poll_capture", {{"instance_id", "data-world"}, {"ticket", ticket}});
		CHECK_FALSE(polled["result"]["isError"].get<bool>());
		const auto resource = McpCall(
			surface,
			"get_resource",
			{{"id", reply.Planes.front().Resource},
			 {"options",
			  {{"instance_id", "data-world"},
			   {"ticket", ticket},
			   {"offset", 0},
			   {"max_bytes", reply.Planes.front().ByteSize}}}}
		);
		CHECK_FALSE(resource["result"]["isError"].get<bool>());
		const auto released = McpCall(
			surface,
			"release_capture",
			{{"instance_id", "data-world"},
			 {"ticket", ticket},
			 {"operation_id", "release-ready-ticket-" + std::to_string(cycle)}}
		);
		CHECK_FALSE(released["result"]["isError"].get<bool>());
		CHECK_FALSE(bridge->Poll("data-world", ticket, reply, failure));
		surface.PumpHooks();
		CHECK_FALSE(bridge->HasOutstanding());
		CHECK(surface.Hooks().Active().size() == 1);
	}
	CHECK(retainedBytes > 0);
	lifecycle.Close();
	surface.PumpHooks();
	CHECK(surface.Hooks().Active().empty());
}

TEST_CASE("render graph MCP errors retain renderer admission details", "[client][control][render-graph]") {
	using namespace engine::graph;
	using engine::core::Name;

	PipelineDocument document;
	document.Record(
		Edit{
			.Kind = EditKind::AddResource,
			.Name = Name("storage"),
			.Resource = ResourceKind::Storage,
		}
	);
	document.Record(
		Edit{
			.Kind = EditKind::AddNode,
			.Name = Name("bad-dispatch"),
			.NodeKind = Name("compute"),
		}
	);
	document.Record(Edit{.Kind = EditKind::Writes, .Target = Name("storage")});
	document.Record(
		Edit{
			.Kind = EditKind::Set,
			.Key = Name("dispatch.x"),
			.Value = "zero",
		}
	);

	engine::render::Renderer renderer;
	const auto admission = renderer.ValidatePipelineDocument(document);
	REQUIRE_FALSE(admission);
	REQUIRE(admission.Failure.has_value());
	CHECK(admission.Failure->Stage == engine::render::PipelineAdmissionStage::Schedule);
	CHECK(admission.Failure->Offender == Name("bad-dispatch"));
	CHECK(admission.Failure->Reason == "a scheduling hint is not valid");

	const std::string expected =
		"unavailable: refused during schedule: a scheduling hint is not valid at 'bad-dispatch'";
	engine::control::Surface surface("client", "render graph admission test");
	surface.SetRenderGraphProvider([&](const nlohmann::json &, std::string &failure) {
		CHECK(client::RenderGraphAdmissionFailure(renderer, document, failure));
		return nlohmann::json(nullptr);
	});
	engine::control::test::Install(surface, std::array{engine::control::test::RenderGraph()});
	const nlohmann::json request{
		{"jsonrpc", "2.0"},
		{"id", 1},
		{"method", "tools/call"},
		{"params",
		 {{"name", "get_render_graph"},
		  {"arguments",
		   {{"instance_id", "world"},
			{"pipeline", "Invalid"},
			{"view_width", 640u},
			{"view_height", 480u},
			{"expected_world_epoch", 1u},
			{"expected_world_version", 1u}}}}},
	};
	const nlohmann::json response = nlohmann::json::parse(surface.Answer(request.dump()));
	REQUIRE(response.contains("result"));
	CHECK(response["result"]["isError"] == true);
	const nlohmann::json payload =
		nlohmann::json::parse(response["result"]["content"][0]["text"].get<std::string>());
	CHECK(payload["error"] == expected);
}
