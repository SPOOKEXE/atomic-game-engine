#include <engine/imagegraphexport/GraphDeviceHost.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
TEST_SUITE_ID("engine.imagegraphexport.graph_device_host")
TEST_DEPENDS("engine.imagegraph.host_capture")

TEST_CASE("MIDI replay retains note state and source watcher normalization", "[assetc][imagegraph]") {
	using namespace engine::imagegraph;
	std::array<engine::imagegraphexport::GraphMidiFrame, 3> frames{
		{{"midi", 2, 0, {144, 60, 127, 176, 7, 64}},
		 {"midi", 2, 1, {128, 60, 0, 144, 61, 0}},
		 {"midi", 2, 2, {}}}
	};
	engine::imagegraphexport::GraphDeviceHost host(frames, {});
	Node node;
	node.Id = "midi";
	node.Type = "pc.midi_in";
	node.DynamicInputs = {
		{"watch.index", ValueType::Integer, int64_t{7}}, {"watch.normalize", ValueType::Boolean, true}
	};
	node.DynamicOutputs = {{"watch.value", ValueType::Scalar}};
	std::array<AuthoredValue, 3> inputs{
		{{"input", EnumValue{2}}, {"watch.index", int64_t{7}}, {"watch.normalize", true}}
	};
	EvaluationRequest request;
	HostNodeCapture capture;
	std::string failure;
	REQUIRE(host.Capture({node, request, inputs, {}, 1048576}, capture, failure));
	REQUIRE(capture.Outputs.size() == 4);
	CHECK(std::get<ArrayValue>(capture.Outputs[0].Data).Elements.size() == 6);
	CHECK(std::get<double>(std::get<ArrayValue>(capture.Outputs[1].Data).Elements[0]) == 60);
	CHECK(std::get<double>(capture.Outputs[3].Data) == Catch::Approx(64.0 / 127));
	request.Tick = 1;
	REQUIRE(host.Capture({node, request, inputs, {}, 1048576}, capture, failure));
	const auto &notes = std::get<ArrayValue>(capture.Outputs[1].Data);
	REQUIRE(notes.Elements.size() == 1);
	CHECK(std::get<double>(notes.Elements[0]) == 61);
	request.Tick = 2;
	REQUIRE(host.Capture({node, request, inputs, {}, 1048576}, capture, failure));
	CHECK(std::get<ArrayValue>(capture.Outputs[0].Data).Elements.empty());
	CHECK(std::get<ArrayValue>(capture.Outputs[1].Data).Elements.size() == 1);
	request.Tick = 3;
	CHECK_FALSE(host.Capture({node, request, inputs, {}, 1048576}, capture, failure));
	frames[1].Messages = {144, 60};
	request.Tick = 1;
	CHECK_FALSE(host.Capture({node, request, inputs, {}, 1048576}, capture, failure));
}

TEST_CASE("Spout recordings bind receiver names and sender pixel receipts", "[assetc][imagegraph]") {
	using namespace engine::imagegraph;
	Image image;
	image.Width = image.Height = 1;
	image.Pixels = {1, 2, 3, 4};
	image.Hash = SurfaceHash(image);
	std::array<engine::imagegraphexport::GraphSpoutFrame, 1> frames{
		{{"spout", "named", 7, image, false, true}}
	};
	engine::imagegraphexport::GraphDeviceHost host({}, frames);
	Node node;
	node.Id = "spout";
	node.Type = "pc.spout_receive";
	EvaluationRequest request;
	request.Tick = 7;
	std::array<AuthoredValue, 1> inputs{{{"receiver_name", std::string{"named"}}}};
	HostNodeCapture capture;
	std::string failure;
	REQUIRE(host.Capture({node, request, inputs, {}, 1048576}, capture, failure));
	REQUIRE(capture.Images.size() == 1);
	CHECK(capture.Images[0].Data == image);
	CHECK_FALSE(capture.SourceUpdateOnFrame.has_value());
	frames[0].SourceUpdateOnFrame = false;
	REQUIRE(host.Capture({node, request, inputs, {}, 1048576}, capture, failure));
	CHECK(capture.SourceUpdateOnFrame == false);
	frames[0].SourceUpdateOnFrame = true;
	REQUIRE(host.Capture({node, request, inputs, {}, 1048576}, capture, failure));
	CHECK(capture.SourceUpdateOnFrame == true);
	inputs[0].Data = std::string{"other"};
	CHECK_FALSE(host.Capture({node, request, inputs, {}, 1048576}, capture, failure));
	inputs[0] = {"sender_name", std::string{"named"}};
	node.Type = "pc.spout_send";
	frames[0].Sending = true;
	std::array<HostResolvedImage, 1> images{{{"surface", &image}}};
	REQUIRE(host.Capture({node, request, inputs, images, 1048576}, capture, failure));
	image.Pixels[0] = 9;
	CHECK_FALSE(host.Capture({node, request, inputs, images, 1048576}, capture, failure));
	image.Pixels[0] = 1;
	frames[0].Accepted = false;
	CHECK_FALSE(host.Capture({node, request, inputs, images, 1048576}, capture, failure));
}

TEST_CASE(
	"DateTime observations retain source token order and exact request binding", "[assetc][imagegraph]"
) {
	using namespace engine::imagegraph;
	std::array<engine::imagegraphexport::GraphDateTimeFrame, 1> frames{};
	auto &frame = frames[0];
	frame.NodeId = "clock";
	frame.Year = 2026;
	frame.Month = 10;
	frame.Day = 2;
	frame.Weekday = 5;
	frame.Hour = 3;
	frame.Minute = 4;
	frame.Second = 5;
	frame.TimerMicroseconds = 123456;
	engine::imagegraphexport::GraphDeviceHost host({}, {}, frames);
	Node node;
	node.Id = "clock";
	node.Type = "pc.datetime_get";
	std::array<AuthoredValue, 2> inputs{
		{{"format", std::string("%y-%m-%dT%h:%n:%s/%w/%tm/%s")}, {"update", false}}
	};
	EvaluationRequest request;
	HostNodeCapture capture;
	std::string failure;
	REQUIRE(host.Capture({node, request, inputs, {}, 1048576}, capture, failure));
	CHECK(std::get<std::string>(capture.Outputs[0].Data) == "2026-10-02T03:04:05/5/123456/05");
	CHECK(capture.SourceUpdateOnFrame == false);
	inputs[1].Data = true;
	REQUIRE(host.Capture({node, request, inputs, {}, 1048576}, capture, failure));
	CHECK(capture.SourceUpdateOnFrame == true);
	request.Subframe = 0.5;
	CHECK_FALSE(host.Capture({node, request, inputs, {}, 1048576}, capture, failure));
	request.Subframe = 0;
	frame.Month = 13;
	CHECK_FALSE(host.Capture({node, request, inputs, {}, 1048576}, capture, failure));
	frame.Month = 10;
	CHECK_FALSE(host.Capture({node, request, inputs, {}, 16}, capture, failure));
}
