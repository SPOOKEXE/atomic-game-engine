#include "CaptureRecordValidation.hpp"

#include <engine/assets/ContentHash.hpp>
#include <engine/core/Name.hpp>
#include <engine/render/DataCapture.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.render.opticalflow")

namespace {
	using namespace engine::render;

	constexpr std::string_view CAMERA_MOTION_PROVENANCE =
		"camera_reprojection/v1;components=delta_x_delta_y;units=pixels;"
		"surface=visible_static_builtin_opaque_or_masked;object_motion=false;"
		"disocclusion=unavailable;camera_history=verified";
}

TEST_CASE("camera motion capture has a distinct graph capture node", "[render][data-capture][motion]") {
	const engine::core::Name capture("data-capture");
	CHECK(
		DataCaptureNode(capture, DataCaptureChannel::MotionVectors) ==
		engine::core::Name("data-capture-motion-vectors")
	);
	CHECK(
		DataCaptureNode(capture, DataCaptureChannel::OpticalFlow) ==
		engine::core::Name("data-capture-optical-flow")
	);
}

TEST_CASE(
	"camera motion validation requires verified prior producer frame", "[render][data-capture][motion]"
) {
	DataCaptureTicket ticket;
	ticket.CaptureNode = engine::core::Name("data-capture");
	ticket.Channels = {DataCaptureChannel::MotionVectors};
	DataCapturePlane plane;
	plane.Channel = DataCaptureChannel::MotionVectors;
	plane.Status = DataCaptureStatus::Ready;
	plane.CaptureNode = ticket.CaptureNode;
	plane.Resource = engine::core::Name("camera-motion-vectors");
	plane.Width = 1;
	plane.Height = 1;
	plane.RowStride = 4;
	plane.Scalar = DataCaptureScalar::Float16;
	plane.ColourSpace = DataCaptureColourSpace::NotApplicable;
	plane.Provenance = CAMERA_MOTION_PROVENANCE;
	plane.PreviousCameraMotionFrame = 4;
	plane.Bytes = {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};
	plane.Hash = engine::assets::Hasher::Of(plane.Bytes);
	capture_record_validation::State state;
	CHECK(capture_record_validation::Plane(ticket, plane, 9, state));

	plane.PreviousCameraMotionFrame.reset();
	capture_record_validation::State missingHistory;
	CHECK_FALSE(capture_record_validation::Plane(ticket, plane, 9, missingHistory));
}
