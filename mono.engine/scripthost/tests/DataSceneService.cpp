// The data observation surface is tested through both VMs because the service
// description is neutral and a map that only one adapter can return is not a
// usable data-factory boundary.

#include <engine/core/Bytes.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/script/DataCaptureBridge.hpp>
#include <engine/script/DataCaptureDriver.hpp>
#include <engine/script/DataLifecycleBridge.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/scripthost/Runtime.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/DataFactory.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <tuple>
#include <vector>

TEST_SUITE_ID("engine.scripthost.datasceneservice")

namespace {
	std::unique_ptr<engine::script::Runtime> Runtime(
		engine::ecs::Store &store,
		engine::script::Language language,
		std::shared_ptr<engine::script::DataCaptureBridge> capture = nullptr,
		std::shared_ptr<engine::script::DataLifecycleBridge> lifecycle = nullptr
	) {
		engine::script::RegisterScriptComponents();
		engine::script::RuntimeLimits limits;
		limits.Role = engine::script::HostRole::OfBoth();
		limits.DataCapture = std::move(capture);
		limits.DataLifecycle = std::move(lifecycle);
		return engine::script::MakeRuntime(store, language, limits);
	}

	class FakeCaptureBridge final : public engine::script::DataCaptureBridge {
	  public:
		explicit FakeCaptureBridge(uint64_t ticket) : Ticket(ticket) {}

		engine::script::DataCaptureBridgeCapabilities Capabilities() const override {
			return {.Available = true, .Channels = {"rgb_linear_hdr"}, .Detail = "test queue"};
		}

		bool Queue(
			std::string_view instanceId,
			const engine::script::DataCaptureBridgeRequest &request,
			uint64_t &ticket,
			std::string &detail
		) override {
			if (instanceId != request.InstanceId || request.InstanceId.empty()) {
				detail = "world mismatch";
				return false;
			}
			Owner = request.InstanceId;
			Queued = true;
			ticket = Ticket;
			detail = "accepted";
			return true;
		}

		bool Poll(
			std::string_view instanceId,
			uint64_t ticket,
			engine::script::DataCaptureBridgePoll &poll,
			std::string &detail
		) override {
			if (!Queued || ticket != Ticket || instanceId != Owner) {
				detail = "unknown capture ticket";
				return false;
			}
			poll.Status = Cancelled ? "cancelled" : "ready";
			poll.SnapshotId = "fixture/snapshot";
			poll.CaptureFrame = 7;
			poll.Planes.push_back({
				.Channel = "rgb_linear_hdr",
				.Status = poll.Status,
				.Resource = "capture/fixture/rgb_linear_hdr",
				.SourceResource = "lit",
				.HashAlgorithm = "blake3-256",
				.Hash = "fixture",
				.Width = 1,
				.Height = 1,
				.RowStride = 4,
				.Scalar = "float16",
				.ColourSpace = "linear",
				.Origin = "top_left",
				.Packing = "RGBA16F",
			});
			detail = "ready copy retained";
			return true;
		}

		bool ReadPlane(
			std::string_view instanceId,
			uint64_t ticket,
			std::string_view resource,
			size_t offset,
			size_t maximumBytes,
			std::vector<std::byte> &bytes,
			std::string &detail
		) override {
			if (!Queued || ticket != Ticket || instanceId != Owner ||
				resource != "capture/fixture/rgb_linear_hdr" || offset != 0 || maximumBytes < 4) {
				detail = "unknown capture plane";
				return false;
			}
			bytes = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
			return true;
		}

		bool Release(std::string_view instanceId, uint64_t ticket, std::string &detail) override {
			if (!Queued || ticket != Ticket || instanceId != Owner) {
				detail = "unknown capture ticket";
				return false;
			}
			Released = true;
			return true;
		}

		void Cancel(std::string_view instanceId, uint64_t ticket) override {
			if (Queued && ticket == Ticket && instanceId == Owner) Cancelled = true;
		}

		bool Released = false;

	  private:
		uint64_t Ticket;
		bool Queued = false;
		bool Cancelled = false;
		std::string Owner;
	};

	void Run(engine::script::Runtime &runtime, const char *source) {
		INFO(source);
		const bool ran = runtime.Run(source);
		INFO(runtime.LastError());
		REQUIRE(ran);
	}

	engine::world::WorldId MakeWorld(engine::world::Universe &universe, const char *name) {
		engine::world::WorldSettings settings;
		settings.Name = engine::core::Name(name);
		return universe.Create(settings);
	}

	engine::script::DataLifecycleBridgeRequest Request(
		std::string_view instanceId,
		std::string operation,
		std::string operationId,
		uint64_t tick,
		uint64_t epoch,
		uint64_t version
	) {
		return {
			.InstanceId = std::string(instanceId),
			.Operation = std::move(operation),
			.OperationId = std::move(operationId),
			.ExpectedTick = tick,
			.ExpectedWorldEpoch = epoch,
			.ExpectedWorldVersion = version,
			.DtNumeratorNanoseconds = 0,
			.DtDenominator = 0,
			.Scope = {},
			.CheckpointId = {},
		};
	}
}

TEST_CASE("DataSceneService retains and replaces one capture driver in both VMs", "[scripting][data]") {
	for (const auto language : {engine::script::Language::Luau, engine::script::Language::JavaScript}) {
		engine::scene::EnsureClassTree();
		engine::scene::RegisterSceneComponents();
		engine::ecs::Store store("capture_driver");
		const auto runtime = Runtime(store, language);
		REQUIRE(runtime != nullptr);
		if (language == engine::script::Language::Luau) {
			Run(*runtime,
				"local s=game:GetService('DataSceneService'); assert(s:SetCaptureDriver(function() return "
				"{status='queued',ticket='1'} end).status=='registered'); "
				"local ok=pcall(function() s:SetCaptureDriver(42) end); assert(not ok); "
				"assert(s:SetCaptureDriver(function() return {status='pending'} end).status=='registered')");
		} else {
			Run(*runtime,
				"const s=game.GetService('DataSceneService'); "
				"if(s.SetCaptureDriver(()=>({status:'queued',ticket:'1'})).status!=='registered') throw new "
				"Error('register'); try{s.SetCaptureDriver(42)}catch(_){ } "
				"if(s.SetCaptureDriver(()=>({status:'pending'})).status!=='registered') "
				"throw new Error('replace');");
		}
		const auto *driver = store.Resource<engine::script::DataCaptureDriver>();
		REQUIRE(driver != nullptr);
		CHECK(driver->Callback.Valid());
		if (language == engine::script::Language::Luau)
			Run(*runtime,
				"assert(game:GetService('DataSceneService'):SetCaptureDriver(nil).status=='released')");
		else
			Run(*runtime,
				"if(game.GetService('DataSceneService').SetCaptureDriver(null).status!=='released') throw "
				"new Error('release');");
		CHECK(store.Resource<engine::script::DataCaptureDriver>() == nullptr);
	}
}

TEST_CASE("capture driver snapshots restore without a stale VM callback", "[scripting][data]") {
	engine::script::RegisterScriptComponents();
	engine::ecs::Store source("capture_driver_snapshot_source");
	source.SetResource(engine::script::DataCaptureDriver{engine::script::HostCallback{17}});
	engine::core::ByteWriter writer;
	REQUIRE(source.Save(writer));

	engine::ecs::Store restored("capture_driver_snapshot_restored");
	engine::core::ByteReader reader(writer.Bytes());
	REQUIRE(restored.Load(reader));
	const auto *driver = restored.Resource<engine::script::DataCaptureDriver>();
	REQUIRE(driver != nullptr);
	CHECK_FALSE(driver->Callback.Valid());
}

TEST_CASE("DataSceneService reports a bounded stable-id subset in both VMs", "[scripting][data]") {
	for (const auto language : {engine::script::Language::Luau, engine::script::Language::JavaScript}) {
		engine::scene::EnsureClassTree();
		engine::scene::RegisterSceneComponents();
		engine::ecs::Store store("data_scene_service");
		const auto runtime = Runtime(store, language);
		REQUIRE(runtime != nullptr);

		if (language == engine::script::Language::Luau) {
			Run(*runtime, R"(
				local part = Instance.new("Part")
				part.Name = "Observed"
				part:SetAttribute("DataFactoryId", "fixture/observed")
				part.Position = Vector3.new(1, 2, 3)
				local snapshot = game:GetService("DataSceneService"):GetSceneSnapshot()
				assert(snapshot.status == "ok")
				assert(snapshot.coverage == "explicitly_identified_subset")
				assert(snapshot.entities[1].id == "fixture/observed")
				assert(snapshot.entities[1].transform.Position.X == 1)
				assert(snapshot.entities[1].physics.mass_kg == 1)
				snapshot.entities[1].id = "mutated"
				assert(game:GetService("DataSceneService"):GetSceneSnapshot().entities[1].id == "fixture/observed")
				local capabilities = game:GetService("DataSceneService"):GetCapabilities()
				assert(capabilities.scene_snapshot and not capabilities.render_capture)
				assert(capabilities.camera_metadata_schema_version == "camera-rendering-data/v1")
				assert(capabilities.spatial_queries and capabilities.spatial_query_kinds[3] == "obb_overlap")
				assert(capabilities.max_raycast_distance_metres == 100000)
				local capture = game:GetService("DataSceneService"):GetCaptureChannels()
				assert(capture.status == "capability_unsupported" and #capture.channels == 0)
			)");
		} else {
			Run(*runtime, R"(
				const part = Instance.new("Part");
				part.Name = "Observed";
				part.SetAttribute("DataFactoryId", "fixture/observed");
				part.Position = Vector3.new(1, 2, 3);
				const snapshot = game.GetService("DataSceneService").GetSceneSnapshot();
				if (snapshot.status !== "ok" || snapshot.coverage !== "explicitly_identified_subset" ||
					 snapshot.entities[0].id !== "fixture/observed" || snapshot.entities[0].transform.Position.X !== 1 ||
					 snapshot.entities[0].physics.mass_kg !== 1) throw new Error("snapshot mismatch");
				snapshot.entities[0].id = "mutated";
				if (game.GetService("DataSceneService").GetSceneSnapshot().entities[0].id !== "fixture/observed") throw new Error("snapshot aliases ECS state");
				const capabilities = game.GetService("DataSceneService").GetCapabilities();
				if (!capabilities.scene_snapshot || capabilities.render_capture || capabilities.camera_metadata_schema_version !== "camera-rendering-data/v1" || !capabilities.spatial_queries || capabilities.spatial_query_kinds[2] !== "obb_overlap" || capabilities.max_raycast_distance_metres !== 100000) throw new Error("capabilities mismatch");
				const capture = game.GetService("DataSceneService").GetCaptureChannels();
				if (capture.status !== "capability_unsupported" || capture.channels.length !== 0) throw new Error("capture mismatch");
			)");
		}
	}
}

TEST_CASE("DataSceneService reports complete authored camera calibration in both VMs", "[scripting][data]") {
	for (const auto language : {engine::script::Language::Luau, engine::script::Language::JavaScript}) {
		engine::scene::EnsureClassTree();
		engine::scene::RegisterSceneComponents();
		engine::ecs::Store store("data_scene_camera");
		const auto runtime = Runtime(store, language);
		REQUIRE(runtime != nullptr);

		if (language == engine::script::Language::Luau) {
			Run(*runtime, R"(
				local camera = Instance.new("Camera")
				camera:SetAttribute("DataFactoryId", "fixture/camera")
				camera.CFrame = CFrame.new(1, 2, 3)
				camera.FieldOfView = 60
				camera.NearPlaneZ = 0.25
				camera.FarPlaneZ = 400
				camera.ImageWidth = 640
				camera.ImageHeight = 360
				local data = game:GetService("DataSceneService"):GetCameraRenderingData(camera)
				assert(data.status == "ok" and data.id == "fixture/camera")
				assert(data.world_from_camera.Position.Z == 3 and data.camera_from_world.Position.Z == -3)
				assert(data.requested_resolution_available and data.requested_width == 640 and data.requested_height == 360)
				assert(not data.projection_available and data.projection_reason == "exact_projection_available_after_render_capture")
				assert(data.crop.width == 1 and data.crop.convention == "normalized_full_view_left_top_width_height")
				assert(not data.lens_distortion.available and not data.temporal_jitter.available)
				assert(data.units.world == "metres" and data.units.angle == "radians")
				assert(data.camera_axes == "x_right_y_up_negative_z_forward" and data.clip_depth_range == "zero_to_one")
			)");
		} else {
			Run(*runtime, R"(
				const camera = Instance.new("Camera");
				camera.SetAttribute("DataFactoryId", "fixture/camera");
				camera.CFrame = CFrame.new(1, 2, 3);
				camera.FieldOfView = 60;
				camera.NearPlaneZ = 0.25;
				camera.FarPlaneZ = 400;
				camera.ImageWidth = 640;
				camera.ImageHeight = 360;
				const data = game.GetService("DataSceneService").GetCameraRenderingData(camera);
				if (data.status !== "ok" || data.id !== "fixture/camera") throw new Error("identity");
				if (data.world_from_camera.Position.Z !== 3 || data.camera_from_world.Position.Z !== -3) throw new Error("extrinsics");
				if (!data.requested_resolution_available || data.requested_width !== 640 || data.requested_height !== 360) throw new Error("resolution");
				if (data.projection_available || data.projection_reason !== "exact_projection_available_after_render_capture") throw new Error("projection");
				if (data.crop.width !== 1 || data.crop.convention !== "normalized_full_view_left_top_width_height") throw new Error("crop");
				if (data.lens_distortion.available || data.temporal_jitter.available) throw new Error("unsupported calibration");
				if (data.units.world !== "metres" || data.units.angle !== "radians") throw new Error("units");
				if (data.camera_axes !== "x_right_y_up_negative_z_forward" || data.clip_depth_range !== "zero_to_one") throw new Error("coordinates");
			)");
		}
	}
}

TEST_CASE("DataSceneService refuses duplicate stable IDs", "[scripting][data]") {
	engine::scene::EnsureClassTree();
	engine::scene::RegisterSceneComponents();
	engine::ecs::Store store("data_scene_duplicate");
	const auto runtime = Runtime(store, engine::script::Language::Luau);
	REQUIRE(runtime != nullptr);
	Run(*runtime, R"(
		local first = Instance.new("Part")
		local second = Instance.new("Part")
		first:SetAttribute("DataFactoryId", "fixture/duplicate")
		second:SetAttribute("DataFactoryId", "fixture/duplicate")
		local snapshot = game:GetService("DataSceneService"):GetSceneSnapshot()
		assert(snapshot.status == "identity_conflict")
	)");
}

TEST_CASE("DataSceneService capture bridges remain runtime-local", "[scripting][data]") {
	for (const auto &[language, worldName, ticket] : {
			 std::tuple{engine::script::Language::Luau, "capture_luau", uint64_t{101}},
			 std::tuple{engine::script::Language::JavaScript, "capture_javascript", uint64_t{202}},
		 }) {
		engine::scene::EnsureClassTree();
		engine::scene::RegisterSceneComponents();
		engine::ecs::Store store(worldName);
		auto bridge = std::make_shared<FakeCaptureBridge>(ticket);
		const auto runtime = Runtime(store, language, bridge);
		REQUIRE(runtime != nullptr);
		if (language == engine::script::Language::Luau) {
			Run(*runtime, R"(
				local service = game:GetService("DataSceneService")
				assert(service:GetCapabilities().render_capture)
				assert(service:Capture({snapshot_id = "fixture/snapshot", pipeline = "main", capture_node = "lit", view_slot = 4294967296, channels = {"rgb_linear_hdr"}, temporal_history = "preserve"}).status == "invalid_capture_request")
				local queued = service:Capture({snapshot_id = "fixture/snapshot", pipeline = "main", capture_node = "lit", view_slot = 0, channels = {"rgb_linear_hdr"}, temporal_history = "preserve"})
				assert(queued.status == "queued")
				assert(service:PollCapture("202").status == "unknown_capture_ticket")
				local poll = service:PollCapture(queued.ticket)
				assert(poll.status == "ready" and poll.planes[1].source_resource == "lit")
				assert(buffer.len(service:GetCaptureBuffer(queued.ticket, poll.planes[1].resource, 0, 4)) == 4)
				assert(service:CancelCapture(queued.ticket).status == "cancellation_requested")
				assert(service:PollCapture(queued.ticket).status == "cancelled")
				assert(service:ReleaseCapture(queued.ticket).status == "released")
			)");
		} else {
			Run(*runtime, R"(
				const service = game.GetService("DataSceneService");
				if (!service.GetCapabilities().render_capture) throw new Error("capture missing");
				if (service.Capture({snapshot_id: "fixture/snapshot", pipeline: "main", capture_node: "lit", view_slot: Infinity, channels: ["rgb_linear_hdr"], temporal_history: "preserve"}).status !== "invalid_capture_request") throw new Error("invalid request accepted");
				const queued = service.Capture({snapshot_id: "fixture/snapshot", pipeline: "main", capture_node: "lit", view_slot: 0, channels: ["rgb_linear_hdr"], temporal_history: "preserve"});
				if (queued.status !== "queued") throw new Error("queue failed");
				if (service.PollCapture("101").status !== "unknown_capture_ticket") throw new Error("other runtime ticket accepted");
				const poll = service.PollCapture(queued.ticket);
				if (poll.status !== "ready" || poll.planes[0].source_resource !== "lit") throw new Error("poll failed");
				if (service.GetCaptureBuffer(queued.ticket, poll.planes[0].resource, 0, 4).byteLength !== 4) throw new Error("bytes failed");
				if (service.CancelCapture(queued.ticket).status !== "cancellation_requested") throw new Error("cancel failed");
				if (service.PollCapture(queued.ticket).status !== "cancelled") throw new Error("cancel state missing");
				if (service.ReleaseCapture(queued.ticket).status !== "released") throw new Error("release failed");
			)");
		}
		CHECK(bridge->Released);
	}
}

TEST_CASE("retained capture driver copies a ready plane before terminal release", "[scripting][data]") {
	engine::scene::EnsureClassTree();
	engine::scene::RegisterSceneComponents();
	engine::ecs::Store store("capture_driver_payload");
	auto bridge = std::make_shared<FakeCaptureBridge>(303);
	const auto runtime = Runtime(store, engine::script::Language::Luau, bridge);
	REQUIRE(runtime != nullptr);
	Run(*runtime, R"(
		local service = game:GetService("DataSceneService")
		assert(service:SetCaptureDriver(function(snapshot, ticket)
			if ticket == nil then return service:Capture({snapshot_id=snapshot, pipeline="main", capture_node="lit", view_slot=0, channels={"rgb_linear_hdr"}, temporal_history="preserve"}) end
			local poll=service:PollCapture(ticket)
			if poll.status == "ready" then poll.copied_payload_bytes=buffer.len(service:GetCaptureBuffer(ticket, poll.planes[1].resource, 0, 4)) end
			return poll
		end).status == "registered")
	)");
	const auto *driver = store.Resource<engine::script::DataCaptureDriver>();
	REQUIRE(driver != nullptr);
	engine::script::HostValue snapshot(engine::script::HostTag::String);
	snapshot.Text = "fixture/snapshot";
	engine::script::HostValue none(engine::script::HostTag::Nil);
	engine::script::HostValue queued;
	REQUIRE(runtime->Invoke(driver->Callback, std::array{snapshot, none}, queued));
	engine::script::HostValue ticket(engine::script::HostTag::String);
	ticket.Text = "303";
	engine::script::HostValue ready;
	REQUIRE(runtime->Invoke(driver->Callback, std::array{snapshot, ticket}, ready));
	bool copied = false;
	for (const auto &[name, value] : ready.Entries)
		if (name == "copied_payload_bytes" && value.Tag == engine::script::HostTag::Number &&
			value.Number == 4)
			copied = true;
	CHECK(copied);
	std::string detail;
	CHECK(bridge->Release("capture_driver_payload", 303, detail));
	CHECK(bridge->Released);
}

TEST_CASE("queued lifecycle bridge mutates only at Pump and retains released retries", "[scripting][data]") {
	using engine::script::DataLifecycleBridgeReply;
	using engine::script::QueuedDataLifecycleBridge;
	using engine::world::DataFactoryPauseScope;
	using engine::world::DataFactorySession;
	using engine::world::DataFactoryStatus;
	using engine::world::Universe;

	Universe universe;
	const auto world = MakeWorld(universe, "lifecycle.direct");
	DataFactorySession session(universe);
	bool paused = false;
	session.SetPauseParticipant(
		[&](engine::world::WorldId candidate, DataFactoryPauseScope scope, bool value, std::string &) {
			if (candidate != world || scope != DataFactoryPauseScope::AllSystems) return false;
			paused = value;
			return true;
		}
	);
	QueuedDataLifecycleBridge bridge(session);

	uint64_t pauseTicket = 0;
	uint64_t resumeTicket = 0;
	std::string detail;
	REQUIRE(bridge.Queue(
		"lifecycle.direct", Request("lifecycle.direct", "pause", "pause-1", 0, 1, 0), pauseTicket, detail
	));
	REQUIRE(bridge.Queue(
		"lifecycle.direct", Request("lifecycle.direct", "resume", "resume-1", 0, 1, 1), resumeTicket, detail
	));
	CHECK_FALSE(paused);
	CHECK(session.Inspect("lifecycle.direct").WorldVersion == 0);
	DataLifecycleBridgeReply pending;
	REQUIRE(bridge.Poll("lifecycle.direct", pauseTicket, pending, detail));
	CHECK(pending.Status == "pending");

	bridge.Pump();
	DataLifecycleBridgeReply pauseReply;
	DataLifecycleBridgeReply resumeReply;
	REQUIRE(bridge.Poll("lifecycle.direct", pauseTicket, pauseReply, detail));
	REQUIRE(bridge.Poll("lifecycle.direct", resumeTicket, resumeReply, detail));
	CHECK(pauseReply.Status == "ok");
	CHECK(resumeReply.Status == "ok");
	CHECK_FALSE(paused);
	CHECK(session.Inspect("lifecycle.direct").WorldVersion == 2);

	uint64_t pausedTicket = 0;
	REQUIRE(bridge.Queue(
		"lifecycle.direct", Request("lifecycle.direct", "pause", "pause-2", 0, 1, 2), pausedTicket, detail
	));
	bridge.Pump();
	REQUIRE(paused);
	uint64_t stepTicket = 0;
	auto step = Request("lifecycle.direct", "step", "step-1", 0, 1, 3);
	step.DtNumeratorNanoseconds = 1'000'000'000;
	step.DtDenominator = 60;
	const auto submittedStep = step;
	REQUIRE(bridge.Queue("lifecycle.direct", step, stepTicket, detail));
	step.DtDenominator = 30;
	bridge.Pump();
	DataLifecycleBridgeReply stepped;
	REQUIRE(bridge.Poll("lifecycle.direct", stepTicket, stepped, detail));
	CHECK(stepped.Status == "ok");
	CHECK(stepped.Tick == "1");
	CHECK(stepped.TimeNanoseconds == "16666666");
	uint64_t beforeReleaseRetry = 0;
	REQUIRE(bridge.Queue("lifecycle.direct", submittedStep, beforeReleaseRetry, detail));
	CHECK(beforeReleaseRetry == stepTicket);
	CHECK(session.Inspect("lifecycle.direct").Clock.Tick == 1);
	REQUIRE(bridge.Release("lifecycle.direct", stepTicket, detail));
	uint64_t retriedTicket = 0;
	REQUIRE(bridge.Queue("lifecycle.direct", submittedStep, retriedTicket, detail));
	CHECK(retriedTicket == stepTicket);
	DataLifecycleBridgeReply retained;
	REQUIRE(bridge.Poll("lifecycle.direct", retriedTicket, retained, detail));
	CHECK(retained.Status == "ok");
	CHECK(retained.Tick == "1");
	retained.Detail = "caller mutation";
	REQUIRE(bridge.Poll("lifecycle.direct", retriedTicket, retained, detail));
	CHECK(retained.Detail != "caller mutation");
	CHECK(session.Inspect("lifecycle.direct").Clock.Tick == 1);

	auto conflicting = submittedStep;
	conflicting.DtDenominator = 30;
	uint64_t ignored = 0;
	CHECK_FALSE(bridge.Queue("lifecycle.direct", conflicting, ignored, detail));
	CHECK(detail.find("different arguments") != std::string::npos);
	CHECK_FALSE(bridge.Poll("other-instance", stepTicket, retained, detail));

	uint64_t staleTicket = 0;
	REQUIRE(bridge.Queue(
		"lifecycle.direct", Request("lifecycle.direct", "resume", "stale-1", 0, 1, 3), staleTicket, detail
	));
	bridge.Pump();
	DataLifecycleBridgeReply stale;
	REQUIRE(bridge.Poll("lifecycle.direct", staleTicket, stale, detail));
	CHECK(stale.Status == "version_conflict");
}

TEST_CASE("DataSceneService lifecycle schema and queue work in both VMs", "[scripting][data]") {
	using engine::script::QueuedDataLifecycleBridge;
	using engine::world::DataFactorySession;
	using engine::world::Universe;

	for (const auto &[language, name] :
		 {std::tuple{engine::script::Language::Luau, "lifecycle_luau"},
		  std::tuple{engine::script::Language::JavaScript, "lifecycle_javascript"}}) {
		engine::scene::EnsureClassTree();
		engine::scene::RegisterSceneComponents();
		Universe universe;
		MakeWorld(universe, name);
		DataFactorySession session(universe);
		auto bridge = std::make_shared<QueuedDataLifecycleBridge>(session);
		engine::ecs::Store store(name);
		const auto runtime = Runtime(store, language, nullptr, bridge);
		REQUIRE(runtime != nullptr);
		if (language == engine::script::Language::Luau) {
			Run(*runtime, R"(
				local service = game:GetService("DataSceneService")
				assert(service:RequestLifecycle({operation = "inspect", extra = "no"}).status == "invalid_lifecycle_request")
				assert(service:RequestLifecycle({operation = "step", operation_id = "bad", expected_tick = "0", expected_world_epoch = "1", expected_world_version = "0", dt_ns = {numerator = "1000000000"}}).status == "invalid_lifecycle_request")
				local queued = service:RequestLifecycle({operation = "inspect"})
				assert(queued.status == "queued")
				assert(service:PollLifecycle(queued.ticket).status == "pending")
			)");
		} else {
			Run(*runtime, R"(
				const service = game.GetService("DataSceneService");
				if (service.RequestLifecycle({operation: "inspect", extra: "no"}).status !== "invalid_lifecycle_request") throw new Error("unknown field accepted");
				if (service.RequestLifecycle({operation: "step", operation_id: "bad", expected_tick: "0", expected_world_epoch: "1", expected_world_version: "0", dt_ns: {numerator: "1000000000"}}).status !== "invalid_lifecycle_request") throw new Error("malformed interval accepted");
				const queued = service.RequestLifecycle({operation: "inspect"});
				if (queued.status !== "queued" || service.PollLifecycle(queued.ticket).status !== "pending") throw new Error("lifecycle queue failed");
			)");
		}
		bridge->Pump();
		if (language == engine::script::Language::Luau) {
			Run(*runtime, R"(
				local service = game:GetService("DataSceneService")
				local reply = service:PollLifecycle("1")
				assert(reply.status == "ok" and reply.instance_id == "lifecycle_luau" and reply.tick == "0")
			)");
		} else {
			Run(*runtime, R"(
				const reply = game.GetService("DataSceneService").PollLifecycle("1");
				if (reply.status !== "ok" || reply.instance_id !== "lifecycle_javascript" || reply.tick !== "0") throw new Error("lifecycle reply mismatch");
			)");
		}
	}
}

TEST_CASE("DataSceneService queries exact prepared collider geometry in both VMs", "[scripting][data]") {
	for (const auto language : {engine::script::Language::Luau, engine::script::Language::JavaScript}) {
		engine::scene::EnsureClassTree();
		engine::scene::RegisterSceneComponents();
		engine::ecs::Store store(language == engine::script::Language::Luau ? "query_luau" : "query_js");
		engine::physics::PreparePhysicsWorld(store);
		const auto runtime = Runtime(store, language);
		REQUIRE(runtime != nullptr);
		if (language == engine::script::Language::Luau) {
			Run(*runtime, R"(
				local part = Instance.new("Part")
				part:SetAttribute("DataFactoryId", "query/box")
				part.Position = Vector3.new(0, 0, 0)
				part.Size = Vector3.new(2, 2, 2)
			)");
		} else {
			Run(*runtime, R"(
				const part = Instance.new("Part");
				part.SetAttribute("DataFactoryId", "query/box");
				part.Position = Vector3.new(0, 0, 0);
				part.Size = Vector3.new(2, 2, 2);
			)");
		}
		engine::ecs::Scheduler scheduler;
		engine::physics::RegisterPhysicsSystems(scheduler);
		scheduler.Tick(store, 1.0 / 60.0);
		if (language == engine::script::Language::Luau) {
			Run(*runtime, R"(
				local service = game:GetService("DataSceneService")
				assert(service:Raycast({origin = Vector3.new(-3, 0, 0), direction = Vector3.new(1, 0, 0), max_distance_metres = 10}).id == "query/box")
				assert(service:OverlapAABB({minimum = Vector3.new(-1, -1, -1), maximum = Vector3.new(1, 1, 1)}).ids[1] == "query/box")
				assert(service:OverlapOBB({frame = CFrame.new(0, 0, 0), half_extent = Vector3.new(1, 1, 1)}).ids[1] == "query/box")
				assert(service:Raycast({origin = Vector3.new(0, 0, 0), direction = Vector3.new(1, 0, 0), max_distance_metres = -1}).status == "invalid_raycast_query")
				assert(service:Raycast({origin = Vector3.new(0, 0, 0), direction = Vector3.new(0, 0, 0), max_distance_metres = 1}).status == "invalid_raycast_query")
			)");
		} else {
			Run(*runtime, R"(
				const service = game.GetService("DataSceneService");
				if (service.Raycast({origin: Vector3.new(-3, 0, 0), direction: Vector3.new(1, 0, 0), max_distance_metres: 10}).id !== "query/box") throw new Error("raycast mismatch");
				if (service.OverlapAABB({minimum: Vector3.new(-1, -1, -1), maximum: Vector3.new(1, 1, 1)}).ids[0] !== "query/box") throw new Error("aabb mismatch");
				if (service.OverlapOBB({frame: CFrame.new(0, 0, 0), half_extent: Vector3.new(1, 1, 1)}).ids[0] !== "query/box") throw new Error("obb mismatch");
				if (service.OverlapAABB({minimum: Vector3.new(1, 1, 1), maximum: Vector3.new(-1, -1, -1)}).status !== "invalid_aabb_query") throw new Error("invalid bounds accepted");
				if (service.Raycast({origin: Vector3.new(0, 0, 0), direction: Vector3.new(0, 0, 0), max_distance_metres: 1}).status !== "invalid_raycast_query") throw new Error("zero ray accepted");
			)");
		}
	}
}
