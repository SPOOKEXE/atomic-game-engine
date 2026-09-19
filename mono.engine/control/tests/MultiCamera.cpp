// The multi-camera row is a protocol contract: it coordinates one checked
// lifecycle revision and one renderer frame without claiming an atomic simulation step.

#include <engine/control/Surface.hpp>
#include <engine/control/features/DataCapture.hpp>
#include <engine/control/features/DataScene.hpp>
#include <engine/script/DataCaptureBridge.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/DataFactory.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.control.multicamera")
TEST_DEPENDS("engine.world.data-factory")

using engine::control::Surface;
using engine::core::Name;
using engine::world::Universe;
using engine::world::WorldSettings;
using nlohmann::json;

namespace {
	json Call(Surface &surface, const char *name, const json &arguments, bool &failed) {
		const json request{
			{"jsonrpc", "2.0"},
			{"id", 1},
			{"method", "tools/call"},
			{"params", {{"name", name}, {"arguments", arguments}}},
		};
		const json reply = json::parse(surface.Answer(request.dump()));
		const json &result = reply.at("result");
		failed = result.value("isError", false);
		return json::parse(result.at("content").at(0).at("text").get<std::string>());
	}

	json Options() {
		return {
			{"schema_version", "data-scene-options/v1"},
			{"channels", {"rgb_linear_hdr"}},
			{"pipeline", "Default PBR"},
			{"capture_node", "data-capture"},
			{"temporal_history", "preserve"},
			{"storage_profile", "lossless"},
			{"output", "raw_planes"},
			{"include_scene_data", false},
			{"include_exact_masks", false},
			{"coordinate_space", "world_camera_image"},
			{"noise_mode", "none"},
			{"noise_seed", 0},
			{"noise_sigma", 0.0},
		};
	}

	class CaptureBridge final : public engine::script::DataCaptureBridge {
	  public:
		engine::script::DataCaptureBridgeCapabilities Capabilities() const override {
			return {
				.Available = true,
				.Channels = {"rgb_linear_hdr"},
				.StorageProfiles = {"lossless"},
				.TrainingCompactLimitations = {},
				.NoiseLimitations = {},
				.HookRecords = {},
				.MaximumCaptureTickets = 6,
				.NamedCameraSelection = true,
				.SameFrameMultiCamera = SameFrameMultiCamera,
				.MaximumSameFrameCameraViews = MaximumSameFrameCameraViews,
				.MaximumCameraIdBytes = 256,
				.Detail = "ready",
			};
		}

		bool Queue(
			std::string_view,
			const engine::script::DataCaptureBridgeRequest &request,
			uint64_t &ticket,
			std::string &detail
		) override {
			if (RejectAfter != 0 && Requests.size() == RejectAfter) {
				detail = "queue full";
				return false;
			}
			Requests.push_back(request);
			ticket =
				TicketSequence.size() >= Requests.size() ? TicketSequence[Requests.size() - 1] : NextTicket++;
			return true;
		}

		bool QueueGroup(
			std::string_view,
			std::span<const engine::script::DataCaptureBridgeRequest> requests,
			std::span<uint64_t> tickets,
			std::string &detail
		) override {
			++GroupCalls;
			if (RejectGroup) {
				detail = "queue full";
				return false;
			}
			if (requests.size() != tickets.size()) {
				detail = "camera ticket count is invalid";
				return false;
			}
			Requests.assign(requests.begin(), requests.end());
			for (size_t index = 0; index < tickets.size(); ++index)
				tickets[index] = TicketSequence.size() > index ? TicketSequence[index] : NextTicket++;
			return true;
		}

		bool
		Poll(std::string_view, uint64_t, engine::script::DataCaptureBridgePoll &, std::string &) override {
			return false;
		}

		bool ReadPlane(
			std::string_view,
			uint64_t,
			std::string_view,
			size_t,
			size_t,
			std::vector<std::byte> &,
			std::string &
		) override {
			return false;
		}

		bool Release(std::string_view, uint64_t, std::string &) override {
			return true;
		}

		void Cancel(std::string_view, uint64_t ticket) override {
			Cancelled.push_back(ticket);
		}

		bool SameFrameMultiCamera = true;
		uint32_t MaximumSameFrameCameraViews = 6;
		bool RejectGroup = false;
		size_t GroupCalls = 0;
		size_t RejectAfter = 0;
		std::vector<engine::script::DataCaptureBridgeRequest> Requests;
		std::vector<uint64_t> Cancelled;
		std::vector<uint64_t> TicketSequence;

	  private:
		uint64_t NextTicket = 1;
	};

	json Request(const engine::world::DataFactoryReply &current, std::string operation) {
		return {
			{"instance_id", "capture-world"},
			{"snapshot_id", "snapshot-1"},
			{"cameras",
			 {{{"camera_id", "camera/a"}, {"view_slot", 0}}, {{"camera_id", "camera/b"}, {"view_slot", 0}}}},
			{"options", Options()},
			{"operation_id", std::move(operation)},
			{"expected_tick", current.Clock.Tick},
			{"expected_world_epoch", current.WorldEpoch},
			{"expected_world_version", current.WorldVersion},
		};
	}

	struct Fixture {
		Fixture() {
			WorldSettings settings;
			settings.Name = Name("capture-world");
			UniverseRef.Create(settings);
			SurfaceRef.Enable(std::array{engine::control::features::DataCapture(Session, Bridge)});
			SurfaceRef.Enable(std::array{engine::control::features::DataScene(UniverseRef, Bridge)});
		}

		Universe UniverseRef;
		engine::world::DataFactorySession Session{UniverseRef};
		std::shared_ptr<CaptureBridge> Bridge = std::make_shared<CaptureBridge>();
		Surface SurfaceRef{"test", "a suite"};
	};
}

TEST_CASE("motion capture plane keeps its two-component wire shape", "[control][data-capture]") {
	engine::script::DataCaptureBridgePlane plane;
	plane.Channel = "motion_vectors";
	plane.Status = "ready";
	plane.Width = 4;
	plane.Height = 3;
	plane.Scalar = "float16";
	plane.PreviousCameraMotionFrame = 7;
	const json encoded = engine::control::data_capture_detail::Plane(plane, "snapshot-1");
	CHECK(encoded.at("shape") == json::array({3, 4, 2}));
	CHECK(encoded.at("dtype") == "float16");
	CHECK(encoded.at("previous_camera_motion_frame") == 7);
}

TEST_CASE(
	"multi-camera capture shares a renderer frame without atomically stepping", "[control][data-capture]"
) {
	Fixture fixture;
	bool failed = false;
	const json reply = Call(
		fixture.SurfaceRef,
		"capture_multi_camera",
		Request(fixture.Session.Inspect("capture-world"), "multi-camera-ok"),
		failed
	);
	REQUIRE_FALSE(failed);
	CHECK(reply["status"] == "coordinated");
	CHECK_FALSE(reply["atomic"]);
	CHECK(reply["atomicity"] == "same_renderer_frame_not_simulation_atomic");
	CHECK(reply["same_renderer_frame"]);
	CHECK(
		reply["captures"] == json::array({
								 {{"ticket", 1}, {"camera_id", "camera/a"}, {"view_slot", 0}},
								 {{"ticket", 2}, {"camera_id", "camera/b"}, {"view_slot", 0}},
							 })
	);
	REQUIRE(fixture.Bridge->Requests.size() == 2);
	CHECK(fixture.Bridge->GroupCalls == 1);
	CHECK(fixture.Bridge->Requests[0].SnapshotId == "snapshot-1");
	CHECK(fixture.Bridge->Requests[0].CameraId == "camera/a");
	CHECK(fixture.Bridge->Requests[1].CameraId == "camera/b");
	CHECK(fixture.Bridge->Requests[0].ViewSlot == 0);
	CHECK(fixture.Bridge->Requests[1].ViewSlot == 0);
	const json replay = Call(
		fixture.SurfaceRef,
		"capture_multi_camera",
		Request(fixture.Session.Inspect("capture-world"), "multi-camera-ok"),
		failed
	);
	CHECK_FALSE(failed);
	CHECK(replay == reply);
	CHECK(fixture.Bridge->Requests.size() == 2);
	CHECK(fixture.Bridge->GroupCalls == 1);
	const json capabilities = Call(
		fixture.SurfaceRef,
		"get_capture_channels",
		{{"instance_id", "capture-world"}, {"options", json::object()}},
		failed
	);
	REQUIRE_FALSE(failed);
	CHECK(capabilities["limits"]["maximum_capture_tickets"] == 6);
	CHECK(capabilities["limits"]["same_frame_multi_camera"] == true);
	CHECK(capabilities["limits"]["maximum_same_frame_camera_views"] == 6);
}

TEST_CASE("multi-camera capture refuses a group without admitting any view", "[control][data-capture]") {
	Fixture fixture;
	fixture.Bridge->RejectGroup = true;
	bool failed = false;
	const json reply = Call(
		fixture.SurfaceRef,
		"capture_multi_camera",
		Request(fixture.Session.Inspect("capture-world"), "multi-camera-reject"),
		failed
	);
	CHECK(failed);
	CHECK(reply["error"] == "capture_refused: queue full");
	CHECK(fixture.Bridge->Requests.empty());
	CHECK(fixture.Bridge->Cancelled.empty());
	CHECK(fixture.Bridge->GroupCalls == 1);
}

TEST_CASE("multi-camera capture requires same-frame bridge support", "[control][data-capture]") {
	Fixture fixture;
	fixture.Bridge->SameFrameMultiCamera = false;
	bool failed = false;
	const json reply = Call(
		fixture.SurfaceRef,
		"capture_multi_camera",
		Request(fixture.Session.Inspect("capture-world"), "multi-camera-no-same-frame"),
		failed
	);
	CHECK(failed);
	CHECK(reply["error"] == "capability_unsupported: same-frame multi-camera capture is unavailable");
	CHECK(fixture.Bridge->Requests.empty());
	CHECK(fixture.Bridge->GroupCalls == 0);
}

TEST_CASE(
	"multi-camera capture refuses duplicate camera identity before queueing", "[control][data-capture]"
) {
	Fixture fixture;
	json request = Request(fixture.Session.Inspect("capture-world"), "multi-camera-duplicate");
	request["cameras"][1]["camera_id"] = "camera/a";
	bool failed = false;
	const json reply = Call(fixture.SurfaceRef, "capture_multi_camera", request, failed);
	CHECK(failed);
	CHECK(reply["error"] == "validation_failed: cameras must use distinct camera_id values");
	CHECK(fixture.Bridge->Requests.empty());
	CHECK(fixture.Bridge->GroupCalls == 0);
}

TEST_CASE(
	"multi-camera capture refuses stale lifecycle revisions before queueing", "[control][data-capture]"
) {
	Fixture fixture;
	json request = Request(fixture.Session.Inspect("capture-world"), "multi-camera-stale");
	request["expected_tick"] = request["expected_tick"].get<uint64_t>() + 1;
	bool failed = false;
	const json reply = Call(fixture.SurfaceRef, "capture_multi_camera", request, failed);
	CHECK(failed);
	CHECK(reply["current_tick"] == fixture.Session.Inspect("capture-world").Clock.Tick);
	CHECK(fixture.Bridge->Requests.empty());
	CHECK(fixture.Bridge->GroupCalls == 0);
}

TEST_CASE(
	"multi-camera capture rejects a reused operation id with different work", "[control][data-capture]"
) {
	Fixture fixture;
	const auto current = fixture.Session.Inspect("capture-world");
	bool failed = false;
	const json accepted =
		Call(fixture.SurfaceRef, "capture_multi_camera", Request(current, "multi-camera-conflict"), failed);
	REQUIRE_FALSE(failed);
	CHECK(accepted["status"] == "coordinated");
	json conflict = Request(current, "multi-camera-conflict");
	conflict["cameras"][1]["view_slot"] = 3;
	const json rejected = Call(fixture.SurfaceRef, "capture_multi_camera", conflict, failed);
	CHECK(failed);
	CHECK(rejected["error"].get<std::string>().starts_with("operation_id_conflict:"));
	CHECK(fixture.Bridge->Requests.size() == 2);
	CHECK(fixture.Bridge->GroupCalls == 1);
}

TEST_CASE("multi-camera capture cancels a malformed admitted group", "[control][data-capture]") {
	Fixture fixture;
	fixture.Bridge->TicketSequence = {1, 0};
	bool failed = false;
	const json reply = Call(
		fixture.SurfaceRef,
		"capture_multi_camera",
		Request(fixture.Session.Inspect("capture-world"), "multi-camera-invalid-ticket"),
		failed
	);
	CHECK(failed);
	CHECK(reply["error"] == "capture_refused: capture bridge returned an invalid ticket");
	CHECK(fixture.Bridge->Cancelled == std::vector<uint64_t>{1});
	CHECK(fixture.Bridge->GroupCalls == 1);
}
