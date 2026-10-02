#include <engine/audio/Wav.hpp>
#include <engine/imagegraph/WavExport.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.imagegraph.wav_export")
using namespace engine::imagegraph;
namespace {
	ArrayValue Channels(std::initializer_list<std::initializer_list<double>> rows) {
		ArrayValue result;
		result.ElementType = ValueType::Scalar;
		for (auto row : rows) {
			auto &channel = result.Nested.emplace_back();
			for (double value : row)
				channel.emplace_back(value);
		}
		return result;
	}
	uint32_t Word(const std::vector<std::byte> &bytes, size_t offset, size_t count) {
		uint32_t value = 0;
		for (size_t i = 0; i < count; ++i)
			value |= uint32_t(std::to_integer<uint8_t>(bytes[offset + i])) << (8 * i);
		return value;
	}
}
TEST_CASE("WAV export canonical headers and native quantization", "[imagegraph][wav_export]") {
	Diagnostic error;
	std::vector<std::byte> bytes;
	REQUIRE(EncodeWavExport(Channels({{-1, .5, 1.5, 2.5, 256}}), {8000}, 1000, bytes, error) == Status::Ok);
	CHECK(bytes.size() == 50);
	CHECK(Word(bytes, 4, 4) == 42);
	CHECK(Word(bytes, 28, 4) == 8000);
	CHECK(Word(bytes, 32, 2) == 1);
	CHECK(Word(bytes, 40, 4) == 5);
	CHECK(Word(bytes, 44, 1) == 0);
	CHECK(Word(bytes, 45, 1) == 0);
	CHECK(Word(bytes, 46, 1) == 2);
	CHECK(Word(bytes, 47, 1) == 2);
	CHECK(Word(bytes, 48, 1) == 255);
	CHECK(Word(bytes, 49, 1) == 0);
	REQUIRE(engine::audio::InspectWav(bytes).has_value());
	REQUIRE(engine::audio::DecodeWav(bytes).has_value());
	bytes = {};
	REQUIRE(
		EncodeWavExport(
			Channels({{-1.5, -2.5}, {32768, -32769}}), {48000, WavExportFormat::Signed16}, 1000, bytes, error
		) == Status::Ok
	);
	CHECK(Word(bytes, 28, 4) == 192000);
	CHECK(Word(bytes, 32, 2) == 4);
	CHECK(Word(bytes, 40, 4) == 8);
	CHECK(Word(bytes, 44, 2) == 65534);
	CHECK(Word(bytes, 46, 2) == 32767);
	CHECK(Word(bytes, 48, 2) == 65534);
	CHECK(Word(bytes, 50, 2) == 32768);
	CHECK(engine::audio::DecodeWav(bytes).has_value());
}
TEST_CASE("WAV remap and replacement admission are atomic", "[imagegraph][wav_export]") {
	Diagnostic error;
	std::vector<std::byte> bytes;
	const auto channels = Channels({{0, .5, 1}});
	REQUIRE(
		EncodeWavExport(channels, {8, WavExportFormat::Unsigned8, true, {0, 1}}, 48, bytes, error) ==
		Status::Ok
	);
	CHECK(Word(bytes, 45, 1) == 128);
	const auto original = bytes;
	const auto exact = bytes.capacity() + 48;
	CHECK(EncodeWavExport(channels, {8}, exact - 1, bytes, error) == Status::LimitExceeded);
	CHECK(bytes == original);
	CHECK(EncodeWavExport(channels, {8}, exact, bytes, error) == Status::Ok);
	CHECK(
		EncodeWavExport(channels, {8, WavExportFormat::Unsigned8, true, {1, 1}}, 1000, bytes, error) ==
		Status::InvalidValue
	);
	CHECK(
		EncodeWavExport(channels, {8, WavExportFormat::Unsigned8, false, {1, 1}}, 1000, bytes, error) ==
		Status::Ok
	);
	CHECK(
		EncodeWavExport(channels, {8, WavExportFormat::Unsigned8, true, {1, 0}}, 1000, bytes, error) ==
		Status::Ok
	);
	CHECK(Word(bytes, 44, 1) == 255);
	CHECK(Word(bytes, 46, 1) == 0);
	CHECK(EncodeWavExport(Channels({{1}, {}}), {8}, 1000, bytes, error) == Status::InvalidValue);
	CHECK(
		EncodeWavExport(Channels({{std::numeric_limits<double>::infinity()}}), {8}, 1000, bytes, error) ==
		Status::InvalidValue
	);
	CHECK(
		EncodeWavExport(
			Channels({{1e308}}), {8, WavExportFormat::Unsigned8, true, {-1e308, 1e308}}, 1000, bytes, error
		) == Status::InvalidValue
	);
	CHECK(EncodeWavExport(Channels({{}}), {8}, 1000, bytes, error) == Status::Ok);
	CHECK(bytes.size() == 44);
	CHECK(EncodeWavExport(Channels({}), {8}, 1000, bytes, error) == Status::InvalidValue);
}
TEST_CASE("WAV sink observes source getters and captured multichannel graph", "[imagegraph][wav_export]") {
	Document doc;
	doc.FormatVersion = 9;
	doc.Nodes = {
		{"file", "pc.wav_file_read", "", {}, {{"path", std::string("capture.wav")}}},
		{"window",
		 "pc.audio_window",
		 "",
		 {},
		 {{"width", int64_t(3)},
		  {"step", int64_t(1)},
		  {"cursor_location", EnumValue{0}},
		  {"match_timeline", false}}},
		{"sink",
		 "pc.wav_file_write",
		 "",
		 {},
		 {{"path", std::string("render.WAV")},
		  {"sample", 6.5},
		  {"remap_data", .75},
		  {"data_range", Vector2{0, 1}}}}
	};
	doc.Links = {{"file", "data", "window", "audio_data"}, {"window", "bit_array", "sink", "audio_data"}};
	doc.Outputs = {{"observed", "window", "bit_array"}};
	Diagnostic error;
	Document parsed;
	REQUIRE(Read(Write(doc), parsed, error) == Status::Ok);
	Plan plan;
	const auto compileStatus = Compile(parsed, plan, error);
	INFO(error.NodeId << ":" << error.Port << " " << error.Message);
	REQUIRE(compileStatus == Status::Ok);
	std::vector<AudioClipSource> clips{{"capture.wav", {{}, 8, {{0, .5, 1, 0}, {1, .5, 0, 1}}}}};
	EvaluationRequest request;
	request.AudioClips = clips;
	WavExport result;
	INFO(error.Message);
	REQUIRE(PrepareWavExport(parsed, plan, "sink", request, 100000, result, error) == Status::Ok);
	CHECK(result.Path == "render.WAV.wav");
	CHECK(Word(result.Bytes, 24, 4) == 6);
	CHECK(Word(result.Bytes, 22, 2) == 2);
	CHECK(Word(result.Bytes, 44, 1) == 0);
	CHECK(Word(result.Bytes, 45, 1) == 255);
	CHECK(Word(result.Bytes, 46, 1) == 128);
	CHECK(Word(result.Bytes, 47, 1) == 128);
	const auto retained = result;
	CHECK(PrepareWavExport(parsed, plan, "sink", request, 1, result, error) == Status::LimitExceeded);
	CHECK(result.Path == retained.Path);
	CHECK(result.Bytes == retained.Bytes);
	parsed.Nodes.back().Values.push_back({"bit_depth", .5});
	REQUIRE(Compile(parsed, plan, error) == Status::Ok);
	CHECK(
		PrepareWavExport(parsed, plan, "sink", request, 100000, result, error) == Status::UnsupportedExecution
	);
	CHECK(result.Bytes == retained.Bytes);
}
TEST_CASE(
	"WAV input capture projects linked animated getters once and refuses atomically",
	"[imagegraph][wav_export]"
) {
	Document doc;
	doc.FormatVersion = 9;
	doc.Nodes = {
		{"rate", "pc.number_simple", "", {}, {{"value", 6.5}}},
		{"sink", "pc.wav_file_write", "", {}, {{"path", std::string("result.wav")}, {"remap_data", .75}}}
	};
	doc.Links = {{"rate", "number", "sink", "sample"}};
	doc.Outputs = {{"observed", "rate", "number"}};
	doc.Keyframes = {{"rate", "value", 0, 6.5, "linear"}, {"rate", "value", 2, 10.5, "linear"}};
	Plan plan;
	Diagnostic error;
	REQUIRE(Compile(doc, plan, error) == Status::Ok);
	EvaluationSnapshot snapshot;
	EvaluationRequest request;
	request.Tick = 1;
	request.Subframe = .5;
	REQUIRE(EvaluateNodeInputs(doc, plan, "sink", request, snapshot, error, 100000) == Status::Ok);
	const auto find = [&](std::string_view port) -> const EvaluationInputValue * {
		for (const auto &value : snapshot.Values())
			if (value.Port == port) return &value;
		return nullptr;
	};
	REQUIRE(find("sample"));
	CHECK(find("sample")->Data == Value{int64_t(10)});
	CHECK(find("sample")->Linked);
	REQUIRE(find("remap_data"));
	CHECK(find("remap_data")->Data == Value{true});
	const auto bytes = snapshot.RetainedBytes();
	CHECK(EvaluateNodeInputs(doc, plan, "sink", request, snapshot, error, 1) == Status::LimitExceeded);
	CHECK(snapshot.RetainedBytes() == bytes);
	CHECK(find("sample")->Data == Value{int64_t(10)});
	request.Tick = 0;
	request.Subframe = 0;
	request.NegativeFrame = true;
	// Negative zero is invalid, and must not replace the retained positive-time snapshot.
	CHECK(EvaluateNodeInputs(doc, plan, "sink", request, snapshot, error, 100000) == Status::InvalidValue);
	CHECK(find("sample")->Data == Value{int64_t(10)});
}
TEST_CASE("WAV defaults and hostile channel geometry remain bounded", "[imagegraph][wav_export]") {
	Document doc;
	doc.FormatVersion = 9;
	doc.Nodes = {{"sink", "pc.wav_file_write", "", {}, {{"path", std::string("empty.wav")}}}};
	doc.Nodes.push_back({"observed", "value.number", "", {}, {{"value", 0.}}});
	doc.Outputs = {{"observed", "observed", "number"}};
	Plan plan;
	Diagnostic error;
	REQUIRE(Compile(doc, plan, error) == Status::Ok);
	WavExport result;
	REQUIRE(PrepareWavExport(doc, plan, "sink", {}, 100000, result, error) == Status::Ok);
	CHECK(result.Bytes.size() == 44);
	CHECK(Word(result.Bytes, 24, 4) == 44100);
	CHECK(Word(result.Bytes, 22, 2) == 1);
	ArrayValue excessive;
	excessive.ElementType = ValueType::Scalar;
	excessive.Nested.resize(Limits::MaximumAudioChannels + 1);
	std::vector<std::byte> bytes;
	CHECK(EncodeWavExport(excessive, {8}, 100000, bytes, error) == Status::LimitExceeded);
	excessive = Channels({{0}});
	CHECK(
		EncodeWavExport(
			excessive, {std::numeric_limits<uint32_t>::max(), WavExportFormat::Signed16}, 100000, bytes, error
		) == Status::InvalidValue
	);
	excessive.Elements.emplace_back(0.);
	CHECK(EncodeWavExport(excessive, {8}, 100000, bytes, error) == Status::InvalidValue);
}
