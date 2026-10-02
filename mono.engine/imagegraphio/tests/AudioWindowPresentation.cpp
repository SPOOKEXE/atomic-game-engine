#include <engine/imagegraph/AudioWindowPresentation.hpp>
#include <engine/imagegraphio/PxcxImport.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraphio.audio_window_presentation")
TEST_DEPENDS("engine.imagegraph.audio_window_presentation")
TEST_CASE(
	"Source Audio Window unit attributes survive PXC and native persistence into actual observed bounds",
	"[imagegraphio][audio_window_presentation]"
) {
	using namespace engine::imagegraph;
	engine::bake::PxcxArchive initial;
	initial.MetadataNumber = 121092;
	initial.MetadataText = "1.22.10.201";
	initial.GraphJson =
		R"JSON({"nodes":[{"id":"window","type":"Node_Audio_Window","x":1,"y":2,"inputs":[{"r":{"d":-4}},{"r":{"d":4}},{"r":{"d":0.4},"attri":{"unit":1}},{"r":{"d":0}},{"r":{"d":1}},{"r":{"d":false}}]}]})JSON";
	initial.GraphJson += '\0';
	std::vector<std::byte> bytes;
	std::string failure;
	REQUIRE(engine::bake::WritePxcx(initial, bytes, failure));
	engine::bake::PxcxArchive source;
	REQUIRE(engine::bake::ReadPxcx(bytes, source, failure));
	engine::imagegraphio::PxcxImport imported;
	REQUIRE(engine::imagegraphio::ImportPxcxImageGraph(source, imported, failure));
	CHECK(imported.Source.OriginalBytes == source.OriginalBytes);
	REQUIRE(imported.Graph.Nodes.size() == 1);
	REQUIRE(imported.Graph.Nodes[0].Type == "pc.audio_window");
	imported.Graph.Nodes.push_back(
		{"capture", "image.audio_recording", "", {}, {{"source_id", std::string("mono")}}}
	);
	imported.Graph.Links.push_back({"capture", "audio", "window", "audio_data"});
	imported.Graph.Outputs = {{"samples", "window", "bit_array"}};
	Document parsed;
	Diagnostic error;
	REQUIRE(Read(Write(imported.Graph), parsed, error) == Status::Ok);
	Plan plan;
	REQUIRE(Compile(parsed, plan, error) == Status::Ok);
	const std::vector<AudioCaptureFrame> frames{{"mono", 0, {0, .1, .2, .3, .4, .5, .6, .7, .8, .9}, 10}};
	EvaluationRequest request;
	request.AudioFrames = frames;
	AudioWindowPresentation result;
	REQUIRE(
		ResolveAudioWindowPresentation(parsed, plan, "window", request, 100000, result, error) == Status::Ok
	);
	CHECK(result.Cursor == .4);
	CHECK(result.Start == .4);
	CHECK(result.End == .8);
	CHECK(result.Points.size() == 10);
	EvaluatedValue extracted;
	REQUIRE(EvaluateValue(parsed, plan, "samples", request, extracted, error) == Status::Ok);
	REQUIRE(std::get<ArrayValue>(extracted.Data).Nested.size() == 1);
	CHECK(std::get<ArrayValue>(extracted.Data).Nested[0] == std::vector<ElementValue>{.4, .5, .6, .7});
	CHECK(imported.Source.OriginalBytes == source.OriginalBytes);
}

TEST_CASE(
	"Persisted PXC Window and captured loudness drive a registered Solid image output",
	"[imagegraphio][audio_window_presentation]"
) {
	using namespace engine::imagegraph;
	engine::bake::PxcxArchive initial;
	initial.MetadataNumber = 121092;
	initial.MetadataText = "1.22.10.201";
	initial.GraphJson =
		R"JSON({"nodes":[{"id":"window","type":"Node_Audio_Window","x":1,"y":2,"inputs":[{"r":{"d":-4}},{"r":{"d":2}},{"r":{"d":0},"attri":{"unit":0}},{"r":{"d":0}},{"r":{"d":1}},{"r":{"d":false}}]}]})JSON";
	initial.GraphJson += '\0';
	std::vector<std::byte> bytes;
	std::string failure;
	REQUIRE(engine::bake::WritePxcx(initial, bytes, failure));
	engine::bake::PxcxArchive archive;
	REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
	engine::imagegraphio::PxcxImport imported;
	REQUIRE(engine::imagegraphio::ImportPxcxImageGraph(archive, imported, failure));
	REQUIRE(imported.Graph.Nodes.size() == 1);
	REQUIRE(imported.Graph.Nodes.front().Type == "pc.audio_window");
	imported.Graph.Nodes.push_back(
		{"capture", "image.audio_recording", "", {}, {{"source_id", std::string("mono")}}}
	);
	imported.Graph.Nodes.push_back({"volume", "pc.audio_loudness", "", {}, {}});
	imported.Graph.Nodes.push_back(
		{"solid",
		 "pc.solid",
		 "",
		 {},
		 {{"dimension", Vector2{1, 1}},
		  {"dimension_unit", EnumValue{0}},
		  {"color", Colour{9, 8, 7, 255}},
		  {"use_mask_dimension", false}}}
	);
	imported.Graph.Links = {
		{"capture", "audio", "window", "audio_data"},
		{"window", "bit_array", "volume", "audio_data"},
		{"volume", "loudness", "solid", "empty"}
	};
	imported.Graph.Outputs = {{"pixels", "solid", "surface_out"}};
	Document persisted;
	Diagnostic error;
	REQUIRE(Read(Write(imported.Graph), persisted, error) == Status::Ok);
	Plan plan;
	const auto compiled = Compile(persisted, plan, error);
	INFO(error.Message);
	REQUIRE(compiled == Status::Ok);
	// RMS amplitudes 1 and 2 yield 0 and 10*log10(2); the source Bool getter selects Empty.
	// Capture selection is exact unsigned Tick, independently of the authored Window location.
	const std::vector<AudioCaptureFrame> frames{
		{"mono", 7, {1, -1, 99}, 10}, {"mono", 8, {2, -2, 99}, 10}, {"mono", 9, {0, 0, 99}, 10}
	};
	EvaluationRequest request;
	request.AudioFrames = frames;
	request.Tick = 7;
	Image image, replay;
	const auto evaluated = Evaluate(persisted, plan, "pixels", request, image, error);
	INFO(error.Message);
	REQUIRE(evaluated == Status::Ok);
	CHECK(image.Width == 1);
	CHECK(image.Height == 1);
	CHECK(image.Pixels == std::vector<uint8_t>{9, 8, 7, 255});
	REQUIRE(Evaluate(persisted, plan, "pixels", request, replay, error) == Status::Ok);
	CHECK(replay == image);
	request.Tick = 8;
	REQUIRE(Evaluate(persisted, plan, "pixels", request, image, error) == Status::Ok);
	CHECK(image.Pixels == std::vector<uint8_t>{0, 0, 0, 0});
	const auto lastGood = image;
	// Nonempty silence is the existing bounded native nonfinite-loudness refusal.
	request.Tick = 9;
	CHECK(Evaluate(persisted, plan, "pixels", request, image, error) == Status::InvalidValue);
	CHECK(error.NodeId == "volume");
	CHECK(image == lastGood);
	request.Tick = 10;
	CHECK(Evaluate(persisted, plan, "pixels", request, image, error) == Status::InvalidValue);
	CHECK(error.NodeId == "capture");
	CHECK(image == lastGood);
	request.Tick = 7;
	REQUIRE(Evaluate(persisted, plan, "pixels", request, image, error) == Status::Ok);
	CHECK(image == replay);
	CHECK(imported.Source.OriginalBytes == archive.OriginalBytes);
}
