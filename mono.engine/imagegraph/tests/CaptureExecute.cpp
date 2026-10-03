#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.capture_execute")
using namespace engine::imagegraph;
namespace {
	Document Graph(bool feedback = false) {
		Document d;
		d.FormatVersion = 9;
		d.Project = ProjectSettings{};
		d.Project->SurfaceWidth = d.Project->SurfaceHeight = 2;
		d.Nodes = {
			{"source",
			 "image.captured",
			 "",
			 {},
			 {{"source_id", std::string(feedback ? "feedback:out" : "source")}}},
			{"invert", "image.invert", "", {}, {{"include_alpha", false}}}
		};
		d.Links = {{"source", "image", "invert", "image"}};
		d.Outputs = {{"out", "invert", "image"}};
		return d;
	}
	Plan Compiled(const Document &d) {
		Plan p;
		Diagnostic diagnostic;
		const auto status = Compile(d, p, diagnostic);
		INFO(diagnostic.Message << " " << diagnostic.NodeId << ":" << diagnostic.Port);
		REQUIRE(status == Status::Ok);
		return p;
	}
	const Image &Captured(const EvaluationSnapshot &snapshot) {
		REQUIRE(snapshot.Images().size() == 1);
		CHECK(snapshot.Images().front().Port == "image");
		return snapshot.Images().front().Data;
	}
	void Pixel(const Image &image, uint8_t rgb, uint8_t alpha) {
		REQUIRE(image.Width == 2);
		REQUIRE(image.Height == 2);
		REQUIRE(image.Pixels.size() == 16);
		for (size_t i = 0; i < 16; i += 4) {
			CHECK(image.Pixels[i] == rgb);
			CHECK(image.Pixels[i + 1] == rgb);
			CHECK(image.Pixels[i + 2] == rgb);
			CHECK(image.Pixels[i + 3] == alpha);
		}
	}
}
TEST_CASE(
	"Input capture also executes its selected output and preserves both owned payloads", "[capture_execute]"
) {
	auto d = Graph();
	const auto p = Compiled(d);
	Image source{2, 2, std::vector<uint8_t>(16, 40), 0};
	const std::array sources{RequestImageSource{"source", source}};
	EvaluationRequest request;
	request.ImageSources = sources;
	const std::array<std::string, 1> outputs{"out"};
	StatefulInputEvaluationResult result;
	Diagnostic diagnostic;
	const auto status = EvaluateStatefulNodeInputs(
		d, p, "invert", request, result, diagnostic, Limits::MaximumEvaluationBytes, outputs
	);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(result.Outputs.size() == 1);
	Pixel(std::get<Image>(result.Outputs.front().Output), 215, 40);
	Pixel(Captured(result.Inputs), 40, 40);
	source.Pixels.assign(16, 0);
	Pixel(Captured(result.Inputs), 40, 40);
	const auto snapshotBytes = result.Inputs.RetainedBytes();
	CHECK(
		EvaluateStatefulNodeInputs(d, p, "invert", request, result, diagnostic, 1, outputs) ==
		Status::LimitExceeded
	);
	Pixel(Captured(result.Inputs), 40, 40);
	Pixel(std::get<Image>(result.Outputs.front().Output), 215, 40);
	CHECK(result.Inputs.RetainedBytes() == snapshotBytes);
}
TEST_CASE(
	"Input capture executes once when a downstream selected output needs the captured producer",
	"[capture_execute]"
) {
	auto d = Graph();
	d.Nodes.push_back({"again", "image.invert", "", {}, {{"include_alpha", false}}});
	d.Links.push_back({"invert", "image", "again", "image"});
	d.Outputs.push_back({"downstream", "again", "image"});
	const auto p = Compiled(d);
	const Image source{2, 2, std::vector<uint8_t>(16, 40), 0};
	const std::array sources{RequestImageSource{"source", source}};
	EvaluationRequest request;
	request.ImageSources = sources;
	const std::array<std::string, 1> outputs{"downstream"};
	StatefulInputEvaluationResult result;
	Diagnostic diagnostic;
	const auto status = EvaluateStatefulNodeInputs(
		d, p, "invert", request, result, diagnostic, Limits::MaximumEvaluationBytes, outputs
	);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(result.Outputs.size() == 1);
	Pixel(std::get<Image>(result.Outputs.front().Output), 40, 40);
	Pixel(Captured(result.Inputs), 40, 40);
}
TEST_CASE(
	"Same-target feedback captures the prior generation and publishes deterministic seek and reset pixels",
	"[capture_execute]"
) {
	const auto d = Graph(true);
	const auto p = Compiled(d);
	CapturedFeedbackHost host;
	Diagnostic diagnostic;
	const auto prepare = [&](uint64_t tick) {
		EvaluationRequest request;
		request.Tick = tick;
		const bool ready = host.PrepareNodeInputs(d, p, 1, 0, "invert", request, diagnostic);
		INFO("tick=" << tick << " code=" << int(diagnostic.Code) << " " << diagnostic.Message);
		REQUIRE(ready);
		REQUIRE(host.Output("out"));
	};
	prepare(0);
	Pixel(Captured(host.Snapshot()), 0, 0);
	Pixel(*host.Output("out"), 255, 0);
	prepare(1);
	Pixel(Captured(host.Snapshot()), 255, 0);
	Pixel(*host.Output("out"), 0, 0);
	prepare(3);
	Pixel(Captured(host.Snapshot()), 255, 0);
	Pixel(*host.Output("out"), 0, 0);
	EvaluationRequest refused;
	refused.Tick = 4;
	CHECK_FALSE(host.PrepareNodeInputs(d, p, 1, 0, "invert", refused, diagnostic, 1));
	Pixel(Captured(host.Snapshot()), 255, 0);
	Pixel(*host.Output("out"), 0, 0);
	prepare(1);
	Pixel(Captured(host.Snapshot()), 255, 0);
	Pixel(*host.Output("out"), 0, 0);
	prepare(0);
	Pixel(Captured(host.Snapshot()), 0, 0);
	Pixel(*host.Output("out"), 255, 0);
}
TEST_CASE(
	"Capture and execute copies linked audio before the selected kernel reads it", "[capture_execute]"
) {
	Document d;
	d.FormatVersion = 9;
	d.Nodes = {
		{"file", "pc.wav_file_read", "", {}, {{"path", std::string("tone.wav")}}},
		{"window",
		 "pc.audio_window",
		 "",
		 {},
		 {{"width", int64_t{4}},
		  {"location", 0.0},
		  {"location_unit", EnumValue{0}},
		  {"cursor_location", EnumValue{0}},
		  {"step", int64_t{1}},
		  {"match_timeline", false}}}
	};
	d.Links = {{"file", "data", "window", "audio_data"}};
	d.Outputs = {{"out", "window", "bit_array"}};
	const auto p = Compiled(d);
	const std::array clips{AudioClipSource{"tone.wav", AudioBit{{1, 2, 3, 4, 5}, 8}}};
	EvaluationRequest request;
	request.AudioClips = clips;
	const std::array<std::string, 1> outputs{"out"};
	StatefulInputEvaluationResult result;
	Diagnostic diagnostic;
	const auto status = EvaluateStatefulNodeInputs(
		d, p, "window", request, result, diagnostic, Limits::MaximumEvaluationBytes, outputs
	);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	const auto values = result.Inputs.Values();
	const auto audio = std::find_if(values.begin(), values.end(), [](const auto &input) {
		return input.Port == "audio_data";
	});
	REQUIRE(audio != values.end());
	CHECK(std::get<AudioBit>(audio->Data) == clips[0].Data);
	REQUIRE(result.Outputs.size() == 1);
	const auto &array = std::get<ArrayValue>(std::get<EvaluatedValue>(result.Outputs[0].Output).Data);
	REQUIRE(array.Nested.size() == 1);
	REQUIRE(array.Nested[0].size() == 4);
	for (size_t i = 0; i < 4; ++i)
		CHECK(std::get<double>(array.Nested[0][i]) == double(i + 1));
	CHECK(clips[0].Data.Samples == std::vector<double>{1, 2, 3, 4, 5});
}
