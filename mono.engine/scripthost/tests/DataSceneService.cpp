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
#include <engine/script/DataSceneService.hpp>
#include <engine/script/EventNarratives.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/scripthost/Runtime.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/DataFactory.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <optional>
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
			return {
				.Available = true,
				.Channels = {"rgb_linear_hdr", "ambient_occlusion", "object_ids", "semantic_ids", "part_ids"},
				.StorageProfiles = {"lossless", "training_compact"},
				.TrainingCompactLimitations = {"linear_depth=float32_to_float16_le"},
				.HookRecords =
					{
						{
							.Name = "data_capture.rgb_linear_hdr",
							.SchemaVersion = 1,
							.NodeKind = "capture",
							.Required = true,
							.Channels = {"rgb_linear_hdr"},
							.Access = "observation",
							.MutatedFields = {},
						},
						{
							.Name = "data_capture.ambient_occlusion",
							.SchemaVersion = 1,
							.NodeKind = "capture",
							.Required = true,
							.Channels = {"ambient_occlusion"},
							.Access = "observation",
							.MutatedFields = {},
						},
						{
							.Name = "data_capture.object_ids",
							.SchemaVersion = 1,
							.NodeKind = "capture",
							.Required = true,
							.Channels = {"object_ids"},
							.Access = "observation",
							.MutatedFields = {},
						},
						{
							.Name = "data_capture.semantic_ids",
							.SchemaVersion = 1,
							.NodeKind = "capture",
							.Required = true,
							.Channels = {"semantic_ids"},
							.Access = "observation",
							.MutatedFields = {},
						},
						{
							.Name = "data_capture.part_ids",
							.SchemaVersion = 1,
							.NodeKind = "capture",
							.Required = true,
							.Channels = {"part_ids"},
							.Access = "observation",
							.MutatedFields = {},
						},
					},
				.MaximumHooks = 14,
				.MaximumConnections = 6,
				.MaximumBatches = 6,
				.MaximumReadbackNodes = 12,
				.MaximumRetainedBytes = 64u * 1024u * 1024u,
				.MaximumPendingPumps = 600,
				.NamedCameraSelection = true,
				.MaximumCameraIdBytes = 256,
				.Detail = "test queue"
			};
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
			LastRequest = request;
			CameraIds.push_back(request.CameraId);
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
				.ByteSize = 4,
				.Scalar = "float16",
				.SourceScalar = "float16",
				.SourceHash = "fixture",
				.SourceRowStride = 4,
				.SourceByteSize = 4,

				.SourceEncoding = {},

				.SourceColourSpace = {},

				.SourceOrigin = {},

				.SourcePacking = {},

				.SourceProvenance = {},
				.ValueClassification = "not_inspected",
				.Encoding = "ieee754_binary16_le",
				.MaximumAbsoluteError = {},
				.ColourSpace = "linear",
				.Origin = "top_left",
				.Packing = "RGBA16F",
				.Provenance = {},
				.AmbientOcclusion = std::nullopt,
			});
			poll.Planes.push_back({
				.Channel = "ambient_occlusion",
				.Status = poll.Status,
				.Resource = "capture/fixture/ambient_occlusion",
				.SourceResource = "occlusion",
				.HashAlgorithm = "blake3-256",
				.Hash = "fixture",
				.Width = 1,
				.Height = 1,
				.RowStride = 1,
				.ByteSize = 1,
				.Scalar = "unorm8",
				.SourceScalar = "unorm8",
				.SourceHash = "fixture",
				.SourceRowStride = 1,
				.SourceByteSize = 1,

				.SourceEncoding = {},

				.SourceColourSpace = {},

				.SourceOrigin = {},

				.SourcePacking = {},

				.SourceProvenance = {},
				.ValueClassification = "not_inspected",
				.Encoding = "unorm8",
				.MaximumAbsoluteError = {},
				.ColourSpace = "not_applicable",
				.Origin = "top_left",
				.Packing = "unorm8",
				.Provenance = "ssao_estimator_visibility_factor_not_ground_truth",
				.AmbientOcclusion = std::nullopt,
			});
			for (const char *channel : {"object_ids", "semantic_ids", "part_ids"})
				poll.Planes.push_back({
					.Channel = channel,
					.Status = poll.Status,
					.Resource = std::string("capture/fixture/") + channel,
					.SourceResource = std::string(channel),
					.HashAlgorithm = "blake3-256",
					.Hash = "fixture",
					.Width = 1,
					.Height = 1,
					.RowStride = 4,
					.ByteSize = 4,
					.Scalar = "uint32",
					.SourceScalar = "uint32",
					.SourceHash = "fixture",
					.SourceRowStride = 4,
					.SourceByteSize = 4,

					.SourceEncoding = {},

					.SourceColourSpace = {},

					.SourceOrigin = {},

					.SourcePacking = {},

					.SourceProvenance = {},
					.ValueClassification = "not_inspected",
					.Encoding = "uint32_le",
					.MaximumAbsoluteError = {},
					.ColourSpace = "not_applicable",
					.Origin = "top_left",
					.Packing = {},
					.Provenance = {},
					.AmbientOcclusion = std::nullopt,
				});
			poll.ObjectLabels = {{1, "fixture/alpha"}, {2, "fixture/packed"}};
			poll.SemanticLabels = {{1, "fixture/box"}};
			poll.PartLabels = {{1, "fixture/alpha"}, {2, "fixture/packed"}};
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

		bool QueueViewCameraMutation(
			std::string_view instanceId,
			const engine::script::ViewCameraMutationRequest &request,
			uint64_t &ticket,
			std::string &detail
		) override {
			if (instanceId != request.InstanceId || request.InstanceId.empty()) {
				detail = "world mismatch";
				return false;
			}
			Mutation = request;
			MutationQueued = true;
			ticket = 303;
			detail = "accepted";
			return true;
		}

		void CancelViewCameraMutation(std::string_view instanceId, uint64_t ticket) override {
			if (MutationQueued && instanceId == Mutation.InstanceId && ticket == 303)
				MutationCancelled = true;
		}

		bool Released = false;
		engine::script::DataCaptureBridgeRequest LastRequest;
		engine::script::ViewCameraMutationRequest Mutation;
		std::vector<std::string> CameraIds;
		bool MutationQueued = false;
		bool MutationCancelled = false;

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
			.SnapshotId = {},
			.TemporalHistory = {},
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

TEST_CASE(
	"event narrative snapshots round trip the default bundle and reject invalid bundles", "[scripting][data]"
) {
	engine::script::RegisterScriptComponents();
	engine::ecs::Store source("event_narrative_snapshot_source");
	const auto runtime = Runtime(source, engine::script::Language::Luau);
	REQUIRE(runtime != nullptr);
	Run(*runtime, R"(
		local service = game:GetService("DataSceneService")
		assert(service:SetEventNarratives({version=1, records={{event_time_ns="1", narration_time_ns="1", subject_id="fixture", speaker_id="script", text="saved", knowledge_state="script_declared", evidence_ids={"fixture"}, provenance_ids={"fixture"}, temporal_reference="observation"}}}).status == "ok")
	)");
	engine::core::ByteWriter writer;
	REQUIRE(source.Save(writer));
	const std::vector<std::byte> valid(writer.Bytes().begin(), writer.Bytes().end());

	engine::ecs::Store restored("event_narrative_snapshot_restored");
	engine::core::ByteReader validReader(valid);
	REQUIRE(restored.Load(validReader));
	const auto *restoredNarratives = restored.Resource<engine::script::EventNarratives>();
	REQUIRE(restoredNarratives != nullptr);
	CHECK(restoredNarratives->Bundle.Tag == engine::script::ValueTag::Map);

	engine::ecs::Store emptySource("event_narrative_empty_snapshot_source");
	emptySource.SetResource(engine::script::EventNarratives{});
	engine::core::ByteWriter emptyWriter;
	REQUIRE(emptySource.Save(emptyWriter));
	engine::ecs::Store emptyRestored("event_narrative_empty_snapshot_restored");
	engine::core::ByteReader emptyReader(emptyWriter.Bytes());
	REQUIRE(emptyRestored.Load(emptyReader));
	const auto *emptyNarratives = emptyRestored.Resource<engine::script::EventNarratives>();
	REQUIRE(emptyNarratives != nullptr);
	engine::script::ScriptValue emptyCanonical;
	CHECK(engine::script::CanonicalEventNarratives(emptyNarratives->Bundle, emptyCanonical));
	CHECK(emptyCanonical.Entries.at(1).first == "records");
	CHECK(emptyCanonical.Entries.at(1).second.Items.empty());

	std::vector<std::byte> corrupt = valid;
	corrupt.back() ^= std::byte{0xff};
	engine::ecs::Store corruptStore("event_narrative_corrupt_snapshot");
	engine::core::ByteReader corruptReader(corrupt);
	CHECK_FALSE(corruptStore.Load(corruptReader));

	engine::ecs::Store invalidSource("event_narrative_invalid_snapshot_source");
	invalidSource.SetResource(engine::script::EventNarratives{engine::script::ScriptValue{}});
	engine::core::ByteWriter invalidWriter;
	REQUIRE(invalidSource.Save(invalidWriter));
	engine::ecs::Store invalidStore("event_narrative_invalid_snapshot");
	engine::core::ByteReader invalidReader(invalidWriter.Bytes());
	CHECK_FALSE(invalidStore.Load(invalidReader));
}

TEST_CASE("DataSceneService keeps validated event narratives in both VMs", "[scripting][data]") {
	for (const auto language : {engine::script::Language::Luau, engine::script::Language::JavaScript}) {
		engine::scene::EnsureClassTree();
		engine::ecs::Store store("event_narratives");
		const auto runtime = Runtime(store, language);
		REQUIRE(runtime != nullptr);
		if (language == engine::script::Language::Luau)
			Run(*runtime, R"(
				local service = game:GetService("DataSceneService")
				assert(service:GetEventNarratives().status == "unavailable")
				local result = service:SetEventNarratives({version = 1, records = {
					{event_time_ns="18446744073709551615", narration_time_ns="18446744073709551615", subject_id="crate", speaker_id="camera", text="seen", knowledge_state="observed", belief=1, certainty=1, evidence_ids={"capture/1"}, provenance_ids={"demo"}, temporal_reference="observation"},
					{event_time_ns="3", narration_time_ns="4", subject_id="crate", speaker_id="sim", text="hidden", knowledge_state="simulator_hidden", evidence_ids={}, provenance_ids={"demo"}, temporal_reference="flashback"},
					{event_time_ns="5", narration_time_ns="4", subject_id="crate", speaker_id="model", text="next", knowledge_state="inferred", belief=.8, certainty=.7, evidence_ids={"capture/1"}, provenance_ids={"demo"}, temporal_reference="prediction"},
				}})
				assert(result.status == "ok", result.reason)
				local records = service:GetEventNarratives()
				assert(records.status == "ok" and records.schema_version == "event-narrative/v1")
				assert(records.records[1].event_time_ns == "18446744073709551615")
				assert(records.records[2].belief == nil and #records.records[2].evidence_ids == 0)
				assert(service:SetEventNarratives({version=1, records={{event_time_ns="1", narration_time_ns="1", subject_id="x", speaker_id="x", text="bad", knowledge_state="observed", evidence_ids={}, provenance_ids={"demo"}, temporal_reference="observation"}}}).status == "invalid_event_narratives")
				assert(service:GetEventNarratives().records[1].text == "seen")
				assert(service:SetEventNarratives({version=1, records={}}).status == "ok")
				assert(#service:GetEventNarratives().records == 0)
				local near = {}
				for index = 1, 3 do
					near[index] = {event_time_ns="1", narration_time_ns="1", subject_id="fixture/" .. index, speaker_id="script", text=string.rep("\1", 2800), knowledge_state="script_declared", evidence_ids={"fixture"}, provenance_ids={"fixture"}, temporal_reference="observation"}
				end
				local nearResult = service:SetEventNarratives({version=1, records=near})
				assert(nearResult.status == "ok", nearResult.reason)
				assert(#service:GetEventNarratives().records == 3)
				local oversized = {}
				for index = 1, 16 do
					oversized[index] = {event_time_ns="1", narration_time_ns="1", subject_id="fixture/" .. index, speaker_id="script", text=string.rep("\1", 4096), knowledge_state="script_declared", evidence_ids={"fixture"}, provenance_ids={"fixture"}, temporal_reference="observation"}
				end
				assert(service:SetEventNarratives({version=1, records=oversized}).status == "invalid_event_narratives")
			)");
		else
			Run(*runtime, R"(
				const service = game.GetService("DataSceneService");
				if (service.GetEventNarratives().status !== "unavailable") throw new Error("initial state");
				const result = service.SetEventNarratives({version: 1, records: [
					{event_time_ns:"18446744073709551615", narration_time_ns:"18446744073709551615", subject_id:"crate", speaker_id:"camera", text:"seen", knowledge_state:"observed", belief:1, certainty:1, evidence_ids:["capture/1"], provenance_ids:["demo"], temporal_reference:"observation"},
					{event_time_ns:"3", narration_time_ns:"4", subject_id:"crate", speaker_id:"sim", text:"hidden", knowledge_state:"simulator_hidden", evidence_ids:[], provenance_ids:["demo"], temporal_reference:"flashback"},
					{event_time_ns:"5", narration_time_ns:"4", subject_id:"crate", speaker_id:"model", text:"next", knowledge_state:"inferred", belief:.8, certainty:.7, evidence_ids:["capture/1"], provenance_ids:["demo"], temporal_reference:"prediction"},
				]});
				if (result.status !== "ok") throw new Error("set");
				const records = service.GetEventNarratives();
				if (records.status !== "ok" || records.schema_version !== "event-narrative/v1" || records.records[0].event_time_ns !== "18446744073709551615" || records.records[1].belief !== null || records.records[1].evidence_ids.length !== 0) throw new Error("stored");
				if (service.SetEventNarratives({version:1, records:[{event_time_ns:"1", narration_time_ns:"1", subject_id:"x", speaker_id:"x", text:"bad", knowledge_state:"observed", evidence_ids:[], provenance_ids:["demo"], temporal_reference:"observation"}]}).status !== "invalid_event_narratives") throw new Error("reject");
				if (service.GetEventNarratives().records[0].text !== "seen") throw new Error("atomic");
				if (service.SetEventNarratives({version:1, records:[]}).status !== "ok" || service.GetEventNarratives().records.length !== 0) throw new Error("clear");
			)");
	}
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
				assert(not snapshot.entities[1].physics.assembly_available)
				assert(not snapshot.entities[1].physics.has_rigid_assembly)
				assert(not snapshot.entities[1].physics.assembly_root_has_stable_id)
				assert(snapshot.entities[1].physics.assembly_root_id == "")
				assert(snapshot.physics_observations.schema_version == "physics-observation/v1")
				assert(not snapshot.physics_observations.world_prepared)
				assert(not snapshot.physics_observations.contacts.available)
				assert(not snapshot.physics_observations.impulses.available)
				snapshot.entities[1].id = "mutated"
				assert(game:GetService("DataSceneService"):GetSceneSnapshot().entities[1].id == "fixture/observed")
				local capabilities = game:GetService("DataSceneService"):GetCapabilities()
				assert(capabilities.scene_snapshot and not capabilities.render_capture)
				assert(capabilities.camera_metadata_schema_version == "camera-rendering-data/v1")
				assert(capabilities.physics_observation_schema_version == "physics-observation/v1")
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
					 snapshot.entities[0].physics.mass_kg !== 1 || snapshot.entities[0].physics.assembly_available ||
					 snapshot.entities[0].physics.has_rigid_assembly ||
					 snapshot.entities[0].physics.assembly_root_has_stable_id ||
					 snapshot.entities[0].physics.assembly_root_id !== "" ||
					 snapshot.physics_observations.schema_version !== "physics-observation/v1" ||
					 snapshot.physics_observations.world_prepared || snapshot.physics_observations.contacts.available ||
					 snapshot.physics_observations.impulses.available) throw new Error("snapshot mismatch");
				snapshot.entities[0].id = "mutated";
				if (game.GetService("DataSceneService").GetSceneSnapshot().entities[0].id !== "fixture/observed") throw new Error("snapshot aliases ECS state");
				const capabilities = game.GetService("DataSceneService").GetCapabilities();
				if (!capabilities.scene_snapshot || capabilities.render_capture || capabilities.camera_metadata_schema_version !== "camera-rendering-data/v1" || capabilities.physics_observation_schema_version !== "physics-observation/v1" || !capabilities.spatial_queries || capabilities.spatial_query_kinds[2] !== "obb_overlap" || capabilities.max_raycast_distance_metres !== 100000) throw new Error("capabilities mismatch");
				const capture = game.GetService("DataSceneService").GetCaptureChannels();
				if (capture.status !== "capability_unsupported" || capture.channels.length !== 0) throw new Error("capture mismatch");
			)");
		}
	}
}

TEST_CASE("DataSceneService reports source-stage lighting metadata in both VMs", "[scripting][data]") {
	for (const auto language : {engine::script::Language::Luau, engine::script::Language::JavaScript}) {
		engine::scene::EnsureClassTree();
		engine::scene::RegisterSceneComponents();
		engine::scene::RegisterSceneClasses();
		engine::ecs::Store store("data_scene_lighting");
		const auto runtime = Runtime(store, language);
		REQUIRE(runtime != nullptr);

		if (language == engine::script::Language::Luau) {
			Run(*runtime, R"(
				local parent = Instance.new("Part")
				parent.CFrame = CFrame.new(2, 3, 4)
				local point = Instance.new("PointLight")
				point:SetAttribute("DataFactoryId", "light/point")
				point.Color = Color3.new(.5, .25, .125)
				point.Brightness = 4
				point.Parent = parent
				local spot = Instance.new("SpotLight")
				spot:SetAttribute("DataFactoryId", "light/spot")
				spot.Angle = 360
				spot.Parent = parent
				local surface = Instance.new("SurfaceLight")
				surface:SetAttribute("DataFactoryId", "light/surface")
				surface.Angle = 0
				surface.Parent = parent
				local rejected = Instance.new("PointLight")
				rejected:SetAttribute("DataFactoryId", "light/rejected")
				rejected.Enabled = false
				rejected.Parent = parent
				local snapshot = game:GetService("DataSceneService"):GetSceneSnapshot()
				local lighting = snapshot.lighting_observation
				assert(lighting.schema_version == "lighting-observation/v1", tostring(lighting.schema_version))
				assert(lighting.local_light_coverage == "identified_source_rows_before_portal_copies_and_camera_cap")
				assert(lighting.identified_local_light_count == 4 and lighting.omitted_unidentified_local_light_count == 0)
				assert(lighting.local_lights[1].id == "light/point" and lighting.local_lights[1].renderer_rgb.R == 2)
				assert(lighting.local_lights[1].authored_color_rgb.R == .5 and lighting.local_lights[1].authored_brightness_renderer_relative == 4 and lighting.local_lights[1].authored_enabled and not lighting.local_lights[1].authored_shadows_requested)
				assert(not lighting.local_lights[1].resolved_direction_available and lighting.local_lights[1].resolved_direction_world == nil and lighting.local_lights[1].resolved_direction_reason == "point_is_omnidirectional")
				assert(lighting.local_lights[2].id == "light/rejected" and not lighting.local_lights[2].source_stage_eligible and lighting.local_lights[2].source_stage_rejection == "disabled")
				assert(lighting.local_lights[3].kind == "spot" and math.abs(lighting.local_lights[3].cone_cosine) < .0001)
				assert(lighting.local_lights[4].kind == "surface" and lighting.local_lights[4].cone_cosine == 1)
				assert(not lighting.view_selection.available and not lighting.portal_copies.available)
				assert(not lighting.per_pixel_contribution.available and not lighting.shadow_factor.available)
				assert(not lighting.shadow_caster.available and not lighting.shadow_receiver.available and not lighting.photometric_units.available)
				local capabilities = game:GetService("DataSceneService"):GetCapabilities()
				assert(capabilities.lighting_source_metadata and not capabilities.lighting_contribution)
			)");
		} else {
			Run(*runtime, R"(
				const parent = Instance.new("Part");
				parent.CFrame = CFrame.new(2, 3, 4);
				const point = Instance.new("PointLight");
				point.SetAttribute("DataFactoryId", "light/point");
				point.Color = Color3.new(.5, .25, .125);
				point.Brightness = 4;
				point.Parent = parent;
				const spot = Instance.new("SpotLight");
				spot.SetAttribute("DataFactoryId", "light/spot");
				spot.Angle = 360;
				spot.Parent = parent;
				const surface = Instance.new("SurfaceLight");
				surface.SetAttribute("DataFactoryId", "light/surface");
				surface.Angle = 0;
				surface.Parent = parent;
				const rejected = Instance.new("PointLight");
				rejected.SetAttribute("DataFactoryId", "light/rejected");
				rejected.Enabled = false;
				rejected.Parent = parent;
				const lighting = game.GetService("DataSceneService").GetSceneSnapshot().lighting_observation;
				if (lighting.schema_version !== "lighting-observation/v1" || lighting.local_light_coverage !== "identified_source_rows_before_portal_copies_and_camera_cap" || lighting.identified_local_light_count !== 4 || lighting.omitted_unidentified_local_light_count !== 0 || lighting.local_lights[0].renderer_rgb.R !== 2 || lighting.local_lights[0].authored_brightness_renderer_relative !== 4 || !lighting.local_lights[0].authored_enabled || lighting.local_lights[0].resolved_direction_available || lighting.local_lights[0].resolved_direction_world !== null || lighting.local_lights[1].source_stage_rejection !== "disabled" || Math.abs(lighting.local_lights[2].cone_cosine) > .0001 || lighting.local_lights[3].cone_cosine !== 1 || lighting.view_selection.available || lighting.portal_copies.available || lighting.per_pixel_contribution.available || lighting.shadow_factor.available || lighting.shadow_caster.available || lighting.shadow_receiver.available || lighting.photometric_units.available) throw new Error("lighting metadata mismatch");
				const capabilities = game.GetService("DataSceneService").GetCapabilities();
				if (!capabilities.lighting_source_metadata || capabilities.lighting_contribution) throw new Error("lighting capabilities mismatch");
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
				local captureCapabilities = service:GetCaptureChannels()
				assert(captureCapabilities.status == "ok")
				assert(captureCapabilities.schema_version == "data-capture-hooks/v1")
				assert(#captureCapabilities.hooks == 5)
				assert(captureCapabilities.hooks[1].name == "data_capture.rgb_linear_hdr")
				assert(captureCapabilities.hooks[1].schema_version == 1)
				assert(captureCapabilities.hooks[1].node_kind == "capture")
				assert(captureCapabilities.hooks[1].required)
				assert(captureCapabilities.hooks[1].channels[1] == "rgb_linear_hdr")
				assert(captureCapabilities.limits.maximum_hooks == 14)
				assert(captureCapabilities.limits.maximum_retained_bytes == 67108864)
				assert(captureCapabilities.limits.maximum_pending_pumps == 600)
				local mutation = service:SubmitViewCameraMutation({snapshot_id = "fixture/snapshot", pipeline = "main", pipeline_revision = 1, view_slot = 0, camera = {field_of_view_radians = 0.9, near_plane = 0.2, far_plane = 200}})
				assert(mutation.status == "queued" and mutation.ticket == "303")
				assert(service:CancelViewCameraMutation(mutation.ticket).status == "cancellation_requested")
				assert(service:Capture({snapshot_id = "fixture/snapshot", pipeline = "main", capture_node = "lit", view_slot = 4294967296, channels = {"rgb_linear_hdr"}, temporal_history = "preserve"}).status == "invalid_capture_request")
				local queued = service:Capture({snapshot_id = "fixture/snapshot", pipeline = "main", capture_node = "lit", view_slot = 0, channels = {"rgb_linear_hdr", "ambient_occlusion", "object_ids", "semantic_ids", "part_ids"}, temporal_history = "preserve"})
				assert(queued.status == "queued")
				assert(service:PollCapture("202").status == "unknown_capture_ticket")
				local poll = service:PollCapture(queued.ticket)
				assert(poll.status == "ready" and poll.planes[1].source_resource == "lit")
				assert(poll.planes[2].channel == "ambient_occlusion" and poll.planes[2].packing == "unorm8")
				assert(poll.planes[2].provenance == "ssao_estimator_visibility_factor_not_ground_truth")
				assert(#poll.object_labels == 2 and poll.object_labels[1].label == 1)
				assert(poll.object_labels[1].stable_id == "fixture/alpha")
				assert(poll.object_labels[2].label == 2 and poll.object_labels[2].stable_id == "fixture/packed")
				assert(#poll.semantic_labels == 1 and poll.semantic_labels[1].stable_id == "fixture/box")
				assert(#poll.part_labels == 2 and poll.part_labels[2].stable_id == "fixture/packed")
				assert(buffer.len(service:GetCaptureBuffer(queued.ticket, poll.planes[1].resource, 0, 4)) == 4)
				assert(service:CancelCapture(queued.ticket).status == "cancellation_requested")
				assert(service:PollCapture(queued.ticket).status == "cancelled")
				assert(service:ReleaseCapture(queued.ticket).status == "released")
			)");
		} else {
			Run(*runtime, R"(
				const service = game.GetService("DataSceneService");
				if (!service.GetCapabilities().render_capture) throw new Error("capture missing");
				const captureCapabilities = service.GetCaptureChannels();
				if (captureCapabilities.status !== "ok" || captureCapabilities.schema_version !== "data-capture-hooks/v1" || captureCapabilities.hooks.length !== 5 || captureCapabilities.hooks[0].name !== "data_capture.rgb_linear_hdr" || captureCapabilities.hooks[0].schema_version !== 1 || captureCapabilities.hooks[0].node_kind !== "capture" || !captureCapabilities.hooks[0].required || captureCapabilities.hooks[0].channels[0] !== "rgb_linear_hdr" || captureCapabilities.limits.maximum_connections !== 6 || captureCapabilities.limits.maximum_batches !== 6 || captureCapabilities.limits.maximum_readback_nodes !== 12) throw new Error("capture capability mismatch");
				const mutation = service.SubmitViewCameraMutation({snapshot_id: "fixture/snapshot", pipeline: "main", pipeline_revision: 1, view_slot: 0, camera: {field_of_view_radians: .9, near_plane: .2, far_plane: 200}});
				if (mutation.status !== "queued" || mutation.ticket !== "303" || service.CancelViewCameraMutation(mutation.ticket).status !== "cancellation_requested") throw new Error("mutation bridge failed");
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
		CHECK(bridge->MutationQueued);
		CHECK(bridge->MutationCancelled);
	}
}

TEST_CASE("DataSceneService copies typed capture bundle options in both VMs", "[scripting][data]") {
	for (const auto &[language, worldName, ticket] : {
			 std::tuple{engine::script::Language::Luau, "options_luau", uint64_t{401}},
			 std::tuple{engine::script::Language::JavaScript, "options_javascript", uint64_t{402}},
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
				local options = service:CreateOptions()
				assert(options.SchemaVersion == "data-scene-options/v1")
				assert(options.Pipeline == "Default PBR")
				options.Pipeline = "main"
				options.CaptureNode = "lit"
				options.Channels = {"rgb_linear_hdr", "object_ids"}
				local queued = service:CaptureBundle("fixture/snapshot", options)
				assert(queued.status == "queued" and queued.options.CameraId == "current_view")
				local invalid = service:CreateOptions()
				invalid.CameraId = "fixture/camera"
				assert(service:CaptureBundle("fixture/snapshot", invalid).status == "queued")
				options.IncludeSceneData = true
				assert(service:CaptureBundle("fixture/snapshot", options).status == "queued")
				invalid = service:CreateOptions()
				invalid.IncludeExactMasks = true
				assert(service:CaptureBundle("fixture/snapshot", invalid).status == "unsupported_data_scene_options")
				invalid = service:CreateOptions()
				invalid.StorageProfile = "training_compact"
				assert(service:CaptureBundle("fixture/snapshot", invalid).status == "queued")
				invalid = service:CreateOptions()
				invalid.NoiseMode = "gaussian"
				assert(service:CaptureBundle("fixture/snapshot", invalid).status == "invalid_data_scene_options")
				invalid = service:CreateOptions()
				invalid.NoiseSeed = 1
				assert(service:CaptureBundle("fixture/snapshot", invalid).status == "invalid_data_scene_options")
				invalid = service:CreateOptions()
				invalid.Channels = {"rgb_linear_hdr", "rgb_linear_hdr"}
				assert(service:CaptureBundle("fixture/snapshot", invalid).status == "invalid_data_scene_options")
				invalid = service:CreateOptions()
				invalid.Channels = {"second_surface_depth"}
				assert(service:CaptureBundle("fixture/snapshot", invalid).status == "invalid_data_scene_options")
				invalid = service:CreateOptions()
				invalid.ViewSlot = 4294967296
				assert(service:CaptureBundle("fixture/snapshot", invalid).status == "invalid_data_scene_options")
				assert(service:CaptureBundle("fixture/snapshot", options).status == "queued")
			)");
		} else {
			Run(*runtime, R"(
				const service = game.GetService("DataSceneService");
				const options = service.CreateOptions();
				if (options.SchemaVersion !== "data-scene-options/v1") throw new Error("schema");
				if (options.Pipeline !== "Default PBR") throw new Error("pipeline");
				options.Pipeline = "main";
				options.CaptureNode = "lit";
				options.Channels = ["rgb_linear_hdr", "object_ids"];
				const queued = service.CaptureBundle("fixture/snapshot", options);
				if (queued.status !== "queued" || queued.options.CameraId !== "current_view") throw new Error("queue");
				let invalid = service.CreateOptions();
				invalid.CameraId = "fixture/camera";
				if (service.CaptureBundle("fixture/snapshot", invalid).status !== "queued") throw new Error("camera");
				options.IncludeSceneData = true;
				if (service.CaptureBundle("fixture/snapshot", options).status !== "queued") throw new Error("scene data");
				invalid = service.CreateOptions(); invalid.IncludeExactMasks = true;
				if (service.CaptureBundle("fixture/snapshot", invalid).status !== "unsupported_data_scene_options") throw new Error("exact masks");
				invalid = service.CreateOptions(); invalid.StorageProfile = "training_compact";
				if (service.CaptureBundle("fixture/snapshot", invalid).status !== "queued") throw new Error("storage");
				invalid = service.CreateOptions(); invalid.NoiseMode = "gaussian";
				if (service.CaptureBundle("fixture/snapshot", invalid).status !== "invalid_data_scene_options") throw new Error("noise mode");
				invalid = service.CreateOptions(); invalid.NoiseSeed = 1;
				if (service.CaptureBundle("fixture/snapshot", invalid).status !== "invalid_data_scene_options") throw new Error("noise seed");
				invalid = service.CreateOptions(); invalid.Channels = ["rgb_linear_hdr", "rgb_linear_hdr"];
				if (service.CaptureBundle("fixture/snapshot", invalid).status !== "invalid_data_scene_options") throw new Error("duplicates");
				invalid = service.CreateOptions(); invalid.Channels = ["second_surface_depth"];
				if (service.CaptureBundle("fixture/snapshot", invalid).status !== "invalid_data_scene_options") throw new Error("pair");
				invalid = service.CreateOptions(); invalid.ViewSlot = 4294967296;
				if (service.CaptureBundle("fixture/snapshot", invalid).status !== "invalid_data_scene_options") throw new Error("view slot");
				if (service.CaptureBundle("fixture/snapshot", options).status !== "queued") throw new Error("final queue");
			)");
		}
		CHECK(bridge->LastRequest.SnapshotId == "fixture/snapshot");
		CHECK(bridge->LastRequest.Pipeline == "main");
		CHECK(bridge->LastRequest.CaptureNode == "lit");
		CHECK(bridge->LastRequest.CameraId == "current_view");
		CHECK(
			std::find(bridge->CameraIds.begin(), bridge->CameraIds.end(), "fixture/camera") !=
			bridge->CameraIds.end()
		);
		CHECK(bridge->LastRequest.Channels == std::vector<std::string>{"rgb_linear_hdr", "object_ids"});
		CHECK(bridge->LastRequest.IncludeSceneData);
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

TEST_CASE(
	"queued lifecycle PumpOne keeps consecutive paused steps as separate boundaries", "[scripting][data]"
) {
	using engine::script::DataLifecycleBridgeReply;
	using engine::script::QueuedDataLifecycleBridge;
	using engine::world::DataFactoryPauseScope;
	using engine::world::DataFactorySession;
	using engine::world::Universe;

	Universe universe;
	const auto world = MakeWorld(universe, "lifecycle.one");
	DataFactorySession session(universe);
	session.SetPauseParticipant(
		[&](engine::world::WorldId candidate, DataFactoryPauseScope scope, bool, std::string &) {
			return candidate == world && scope == DataFactoryPauseScope::AllSystems;
		}
	);
	QueuedDataLifecycleBridge bridge(session);
	std::string detail;
	uint64_t pauseTicket = 0;
	REQUIRE(bridge.Queue(
		"lifecycle.one", Request("lifecycle.one", "pause", "pause", 0, 1, 0), pauseTicket, detail
	));
	const auto paused = bridge.PumpOne();
	CHECK(paused.Processed);
	CHECK(paused.WorldMutated);
	CHECK_FALSE(paused.CompletedTick);

	auto first = Request("lifecycle.one", "step", "step-1", 0, 1, 1);
	first.DtNumeratorNanoseconds = 1'000'000'000;
	first.DtDenominator = 60;
	auto second = Request("lifecycle.one", "step", "step-2", 1, 1, 2);
	second.DtNumeratorNanoseconds = 1'000'000'000;
	second.DtDenominator = 60;
	uint64_t firstTicket = 0;
	uint64_t secondTicket = 0;
	REQUIRE(bridge.Queue("lifecycle.one", first, firstTicket, detail));
	REQUIRE(bridge.Queue("lifecycle.one", second, secondTicket, detail));

	const auto firstBoundary = bridge.PumpOne();
	CHECK(firstBoundary.Processed);
	CHECK(firstBoundary.CompletedTick);
	CHECK(firstBoundary.InstanceId == "lifecycle.one");
	CHECK(session.Inspect("lifecycle.one").Clock.Tick == 1);
	DataLifecycleBridgeReply firstReply;
	REQUIRE(bridge.Poll("lifecycle.one", firstTicket, firstReply, detail));
	CHECK(firstReply.Status == "ok");
	CHECK(firstReply.Tick == "1");

	const auto secondBoundary = bridge.PumpOne();
	CHECK(secondBoundary.Processed);
	CHECK(secondBoundary.CompletedTick);
	CHECK(session.Inspect("lifecycle.one").Clock.Tick == 2);
	DataLifecycleBridgeReply secondReply;
	REQUIRE(bridge.Poll("lifecycle.one", secondTicket, secondReply, detail));
	CHECK(secondReply.Status == "ok");
	CHECK(secondReply.Tick == "2");

	auto conflicting = Request("lifecycle.one", "step", "step-conflict", 2, 1, 99);
	conflicting.DtNumeratorNanoseconds = 1'000'000'000;
	conflicting.DtDenominator = 60;
	uint64_t conflictingTicket = 0;
	REQUIRE(bridge.Queue("lifecycle.one", conflicting, conflictingTicket, detail));
	const auto refused = bridge.PumpOne();
	CHECK(refused.Processed);
	CHECK_FALSE(refused.WorldMutated);
	CHECK_FALSE(refused.CompletedTick);
	CHECK_FALSE(refused.Restored);
}

TEST_CASE(
	"queued lifecycle bridge reports render-only command submission without readback", "[scripting][data]"
) {
	using engine::script::DataLifecycleBridgeReply;
	using engine::script::QueuedDataLifecycleBridge;
	using engine::world::DataFactoryPauseScope;
	using engine::world::DataFactorySession;
	using engine::world::Universe;

	Universe universe;
	MakeWorld(universe, "lifecycle.render-only");
	DataFactorySession session(universe);
	session.SetPauseParticipant([](engine::world::WorldId, DataFactoryPauseScope, bool, std::string &) {
		return true;
	});
	REQUIRE(
		session.Pause("lifecycle.render-only", DataFactoryPauseScope::AllSystems, 0).Status ==
		engine::world::DataFactoryStatus::Ok
	);
	std::string snapshot;
	REQUIRE(
		session.Snapshot("lifecycle.render-only", snapshot).Status == engine::world::DataFactoryStatus::Ok
	);
	const auto current = session.Inspect("lifecycle.render-only");
	session.SetRenderOnlyPresenter([](const engine::world::DataFactoryRenderOnlyRequest &, std::string &) {
		return true;
	});
	QueuedDataLifecycleBridge bridge(session);
	auto request = Request("lifecycle.render-only", "render_only", "render-1", 0, 1, current.WorldVersion);
	request.SnapshotId = snapshot;
	request.TemporalHistory = "preserve";
	uint64_t ticket = 0;
	std::string detail;
	REQUIRE(bridge.Queue("lifecycle.render-only", request, ticket, detail));
	bridge.Pump();
	DataLifecycleBridgeReply pending;
	REQUIRE(bridge.Poll("lifecycle.render-only", ticket, pending, detail));
	CHECK(pending.Status == "pending");
	CHECK(pending.TemporalHistory == "preserve");
	REQUIRE_FALSE(pending.OperationId.empty());
	const uint64_t operation = std::stoull(pending.OperationId);
	REQUIRE(
		session
			.CompleteRenderOnly({
				.InstanceId = "lifecycle.render-only",
				.OperationId = operation,
				.Submitted = true,
				.Detail = {},
			})
			.Status == engine::world::DataFactoryStatus::Ok
	);
	// Ticket A is terminal in the session but still retained by the bridge.
	// Queue B before the next pump. The bridge must collect A before it can
	// dequeue B, otherwise a pending B can hide A indefinitely.
	auto secondRequest = request;
	secondRequest.OperationId = "render-2";
	uint64_t secondTicket = 0;
	REQUIRE(bridge.Queue("lifecycle.render-only", secondRequest, secondTicket, detail));
	const auto completedFirst = bridge.PumpOne();
	CHECK(completedFirst.Processed);
	CHECK_FALSE(completedFirst.PendingRenderOnly);
	DataLifecycleBridgeReply submitted;
	REQUIRE(bridge.Poll("lifecycle.render-only", ticket, submitted, detail));
	CHECK(submitted.Status == "submitted");
	CHECK(submitted.Detail.find("readback readiness is not tracked") != std::string::npos);

	const auto acceptedSecond = bridge.PumpOne();
	CHECK(acceptedSecond.Processed);
	CHECK_FALSE(acceptedSecond.PendingRenderOnly);
	DataLifecycleBridgeReply second;
	REQUIRE(bridge.Poll("lifecycle.render-only", secondTicket, second, detail));
	CHECK(second.Status == "pending");
	const auto waiting = bridge.PumpOne();
	CHECK(waiting.Processed);
	CHECK(waiting.PendingRenderOnly);
	CHECK_FALSE(waiting.WorldMutated);
	bridge.Pump();
	DataLifecycleBridgeReply stillPending;
	REQUIRE(bridge.Poll("lifecycle.render-only", secondTicket, stillPending, detail));
	CHECK(stillPending.Status == "pending");
	uint64_t repeated = 0;
	REQUIRE(bridge.Queue("lifecycle.render-only", request, repeated, detail));
	CHECK(repeated == ticket);
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
				local bev = service:GetColliderBev({xz_bounds_metres = {minimum = {-1, -1}, maximum = {1, 1}}, y_minimum_metres = -1, y_maximum_metres = 1, rows = 1, columns = 1})
				assert(bev.status == "ok" and bev.row_order == "z_major_then_x" and bev.cells[1].state == "occupied")
				assert(service:Raycast({origin = Vector3.new(0, 0, 0), direction = Vector3.new(1, 0, 0), max_distance_metres = -1}).status == "invalid_raycast_query")
				assert(service:Raycast({origin = Vector3.new(0, 0, 0), direction = Vector3.new(0, 0, 0), max_distance_metres = 1}).status == "invalid_raycast_query")
			)");
		} else {
			Run(*runtime, R"(
				const service = game.GetService("DataSceneService");
				if (service.Raycast({origin: Vector3.new(-3, 0, 0), direction: Vector3.new(1, 0, 0), max_distance_metres: 10}).id !== "query/box") throw new Error("raycast mismatch");
				if (service.OverlapAABB({minimum: Vector3.new(-1, -1, -1), maximum: Vector3.new(1, 1, 1)}).ids[0] !== "query/box") throw new Error("aabb mismatch");
				if (service.OverlapOBB({frame: CFrame.new(0, 0, 0), half_extent: Vector3.new(1, 1, 1)}).ids[0] !== "query/box") throw new Error("obb mismatch");
				const bev = service.GetColliderBev({xz_bounds_metres: {minimum: [-1, -1], maximum: [1, 1]}, y_minimum_metres: -1, y_maximum_metres: 1, rows: 1, columns: 1});
				if (bev.status !== "ok" || bev.row_order !== "z_major_then_x" || bev.cells[0].state !== "occupied") throw new Error("BEV mismatch");
				if (service.OverlapAABB({minimum: Vector3.new(1, 1, 1), maximum: Vector3.new(-1, -1, -1)}).status !== "invalid_aabb_query") throw new Error("invalid bounds accepted");
				if (service.Raycast({origin: Vector3.new(0, 0, 0), direction: Vector3.new(0, 0, 0), max_distance_metres: 1}).status !== "invalid_raycast_query") throw new Error("zero ray accepted");
			)");
		}
	}
}
