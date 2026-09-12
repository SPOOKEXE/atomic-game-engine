#include <engine/render/DataCapture.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/ScriptDataCaptureBridge.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/DataFactory.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.render.datacapture")

using namespace engine::render;

TEST_CASE("data capture channel names are stable", "[render][data-capture]") {
	CHECK(std::string_view(DataCaptureChannelName(DataCaptureChannel::RgbLinearHdr)) == "rgb_linear_hdr");
	CHECK(std::string_view(DataCaptureChannelName(DataCaptureChannel::LinearDepth)) == "linear_depth");
	CHECK(std::string_view(DataCaptureChannelName(DataCaptureChannel::OpticalFlow)) == "optical_flow");
}

TEST_CASE("data capture camera convention records the engine projection", "[render][data-capture]") {
	const DataCaptureCameraConvention convention = DataCaptureCameraConventions();
	CHECK(convention.RightHandedWorld);
	CHECK(convention.CameraLooksNegativeZ);
	CHECK(convention.ClipYUp);
	CHECK(convention.DepthZeroToOne);
	CHECK(convention.ProjectionIsColumnMajor);
	CHECK(convention.MetresPerWorldUnit == 1.0f);
}

TEST_CASE("data capture refuses a non-rendering history policy before queueing", "[render][data-capture]") {
	Renderer renderer;
	DataCaptureRequest request{
		.SnapshotId = "snapshot-1",
		.Pipeline = engine::core::Name("capture-pipeline"),
		.CaptureNode = engine::core::Name("capture"),
		.Channels = {DataCaptureChannel::RgbLinearHdr},
		.TemporalHistory = DataCaptureTemporalHistory::Reset,
	};
	DataCaptureTicket ticket;
	CHECK_FALSE(renderer.QueueDataCapture(request, ticket));
	CHECK(ticket.ResourceTokens.empty());

	request.TemporalHistory = DataCaptureTemporalHistory::Preserve;
	request.SnapshotId.clear();
	CHECK_FALSE(renderer.QueueDataCapture(request, ticket));
}

namespace {
	engine::script::DataCaptureBridgeRequest Request(std::string_view instanceId = "data-world") {
		return {
			.InstanceId = std::string(instanceId),
			.SnapshotId = "snapshot-1",
			.Pipeline = "pipeline",
			.CaptureNode = "capture",
			.Channels = {"rgb_linear_hdr"},
			.TemporalHistory = "preserve",
		};
	}
}

TEST_CASE("script capture retains terminal tickets until release", "[render][data-capture]") {
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	Renderer renderer;
	ScriptDataCaptureBridge bridge(session, renderer);
	const auto request = Request();
	std::array<uint64_t, 6> tickets{};
	std::string detail;
	for (uint64_t &ticket : tickets)
		REQUIRE(bridge.Queue("data-world", request, ticket, detail));
	uint64_t refused = 0;
	CHECK_FALSE(bridge.Queue("data-world", request, refused, detail));
	bridge.Cancel("data-world", tickets[2]);
	bridge.Pump();
	engine::script::DataCaptureBridgePoll reply;
	REQUIRE(bridge.Poll("data-world", tickets[2], reply, detail));
	CHECK(reply.Status == "cancelled");
	std::vector<std::byte> bytes;
	CHECK_FALSE(bridge.ReadPlane("data-world", tickets[2], "capture/3/rgb_linear_hdr", 0, 8, bytes, detail));
	REQUIRE(bridge.Release("data-world", tickets[2], detail));
	uint64_t reused = 0;
	CHECK(bridge.Queue("data-world", request, reused, detail));
}

TEST_CASE("script capture validates requests and isolates ticket owners", "[render][data-capture]") {
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	Renderer renderer;
	ScriptDataCaptureBridge first(session, renderer);
	ScriptDataCaptureBridge second(session, renderer);
	std::string detail;
	uint64_t ticket = 0;

	auto malformed = Request();
	malformed.Channels = {"rgb_linear_hdr", "rgb_linear_hdr"};
	CHECK_FALSE(first.Queue("data-world", malformed, ticket, detail));
	malformed = Request();
	malformed.CaptureNode = std::string("capture\0node", 12);
	CHECK_FALSE(first.Queue("data-world", malformed, ticket, detail));
	malformed = Request();
	malformed.Channels = {"object_ids"};
	CHECK_FALSE(first.Queue("data-world", malformed, ticket, detail));
	CHECK_FALSE(first.Queue("another-world", Request(), ticket, detail));

	uint64_t firstTicket = 0;
	uint64_t secondTicket = 0;
	REQUIRE(first.Queue("data-world", Request(), firstTicket, detail));
	REQUIRE(second.Queue("data-world", Request(), secondTicket, detail));
	first.Cancel("another-world", firstTicket);
	first.Pump();
	engine::script::DataCaptureBridgePoll reply;
	REQUIRE(first.Poll("data-world", firstTicket, reply, detail));
	CHECK(reply.Status == "pending");
	CHECK_FALSE(first.Poll("another-world", firstTicket, reply, detail));
	second.Cancel("data-world", secondTicket);
	second.Pump();
	REQUIRE(second.Poll("data-world", secondTicket, reply, detail));
	CHECK(reply.Status == "cancelled");
	REQUIRE(second.Release("data-world", secondTicket, detail));
	CHECK_FALSE(second.Poll("data-world", firstTicket, reply, detail));
}

TEST_CASE("script capture reuses capacity after sequential terminal releases", "[render][data-capture]") {
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	Renderer renderer;
	ScriptDataCaptureBridge bridge(session, renderer);
	std::string detail;

	for (size_t index = 0; index < 8; ++index) {
		uint64_t ticket = 0;
		REQUIRE(bridge.Queue("data-world", Request(), ticket, detail));
		bridge.Cancel("data-world", ticket);
		bridge.Pump();
		engine::script::DataCaptureBridgePoll reply;
		REQUIRE(bridge.Poll("data-world", ticket, reply, detail));
		CHECK(reply.Status == "cancelled");
		std::vector<std::byte> bytes;
		CHECK_FALSE(bridge.ReadPlane("data-world", ticket, "capture/invalid", 0, 16, bytes, detail));
		REQUIRE(bridge.Release("data-world", ticket, detail));
	}
}
