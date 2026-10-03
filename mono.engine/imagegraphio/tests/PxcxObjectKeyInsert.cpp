#include <engine/imagegraphio/PxcxEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <nlohmann/json.hpp>

TEST_SUITE_ID("engine.imagegraphio.pxcx_object_key_insert")
TEST_DEPENDS("engine.imagegraphio.pxcxedit")
using namespace engine::imagegraph;
using namespace engine::imagegraphio;
namespace {
	using Json = nlohmann::ordered_json;
	Json GradientData(uint32_t colour) {
		return Json{
			{"type", 1},
			{"keys", Json::array({Json{{"time", 0}, {"value", colour}, {"future_key", "keep"}}})},
			{"future_gradient", 19}
		}
			.dump();
	}
	Json MatrixData(double value) {
		return Json{
			{"size", Json::array({2, 3})},
			{"isize", 6},
			{"raw", Json::array({value, value, value, value, value, value})},
			{"future_matrix", 23}
		};
	}
	Json Record(double frame, Json data) {
		return Json::array(
			{Json::array({0, frame}),
			 std::move(data),
			 Json::array({0, 1}),
			 Json::array({0, 0}),
			 0,
			 0,
			 true,
			 0,
			 4294967295u,
			 "retained-tail"}
		);
	}
	PxcxImport Fixture(bool matrix) {
		Json animated{
			{"anim", true},
			{"r",
			 Json::array(
				 {Record(0, matrix ? MatrixData(1) : GradientData(0xff000000u)),
				  Record(10, matrix ? MatrixData(9) : GradientData(0xffffffffu))}
			 )},
			{"future_input", 7}
		};
		Json inputs = matrix ? Json::array({Json{{"r", {{"d", Json::array({2, 3})}}}}, animated})
							 : Json::array({animated, Json{{"r", {{"d", 0.}}}}});
		Json node{
			{"id", "object"},
			{"type", matrix ? "Node_Matrix" : "Node_Gradient_Out"},
			{"x", 0},
			{"y", 0},
			{"inputs", inputs},
			{"future_node", 11}
		};
		engine::bake::PxcxArchive source;
		source.MetadataNumber = 121092;
		source.MetadataText = "1.22.10.201";
		source.GraphJson =
			Json{
				{"animator", {{"frames_total", 30}, {"playback", 0}, {"framerate", 30}}},
				{"nodes", Json::array({node})},
				{"future_project", 13}
			}
				.dump();
		source.GraphJson.push_back('\0');
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(source, bytes, failure));
		engine::bake::PxcxArchive checked;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		PxcxImport result;
		REQUIRE(ImportPxcxImageGraph(checked, result, failure));
		REQUIRE(
			std::count_if(result.Graph.Keyframes.begin(), result.Graph.Keyframes.end(), [&](const auto &key) {
				return key.NodeId == "object" && key.Port == (matrix ? "data" : "gradient");
			}) == 2
		);
		REQUIRE(result.Graph.Nodes[0].Type == (matrix ? "pc.matrix" : "pc.gradient_out"));
		return result;
	}
	Keyframe NewKey(bool matrix, int time) {
		Keyframe key;
		key.NodeId = "object";
		key.Port = matrix ? "data" : "gradient";
		key.Interpolation = "source";
		key.Ease = KeyframeEase{};
		key.Data = matrix
					   ? Value{MatrixValue{2, 3, {3, 4, 5, 6, 7, 8}}}
					   : Value{Gradient{1, {{0, Colour{10, 20, 30, 255}}, {1, Colour{110, 120, 130, 255}}}}};
		FrameTime frame;
		REQUIRE(SplitFrameTime(time, frame));
		REQUIRE(SetFrameTime(key, frame));
		return key;
	}
}
TEST_CASE(
	"PXC inserted Gradient and rectangular Matrix values use exact fresh source descriptors",
	"[imagegraphio][pxcx_object_key_insert]"
) {
	for (bool matrix : {false, true})
		for (int time : {-2, 5, 12}) {
			INFO("matrix=" << matrix << " time=" << time);
			const auto original = Fixture(matrix);
			const auto key = NewKey(matrix, time);
			const std::array<PxcxEdit, 1> edits{PxcxKeyframeInsertEdit{"object", key.Port, key}};
			std::vector<std::byte> bytes;
			Diagnostic diagnostic;
			const auto written =
				WritePxcxEdits(original, original.Source.OriginalBytes, edits, bytes, diagnostic);
			INFO(diagnostic.Message);
			REQUIRE(written);
			engine::bake::PxcxArchive archive;
			std::string failure;
			REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
			PxcxImport imported;
			REQUIRE(ImportPxcxImageGraph(archive, imported, failure));
			REQUIRE(
				std::count_if(
					imported.Graph.Keyframes.begin(), imported.Graph.Keyframes.end(), [&](const auto &key) {
						return key.NodeId == "object" && key.Port == (matrix ? "data" : "gradient");
					}
				) == 3
			);
			const auto found = std::find_if(
				imported.Graph.Keyframes.begin(), imported.Graph.Keyframes.end(), [&](const auto &value) {
					return value.NodeId == key.NodeId && value.Port == key.Port &&
						   GetFrameTime(value) == GetFrameTime(key);
				}
			);
			REQUIRE(found != imported.Graph.Keyframes.end());
			CHECK(found->Data == key.Data);
			const auto source = Json::parse(archive.GraphJson.c_str());
			CHECK(source["future_project"] == 13);
			CHECK(source["nodes"][0]["future_node"] == 11);
			const auto &input = source["nodes"][0]["inputs"][matrix ? 1 : 0];
			CHECK(input["future_input"] == 7);
			const auto &records = input["r"];
			REQUIRE(records.size() == 3);
			for (const auto &record : records) {
				if (record[0][1].get<int>() != time) {
					CHECK(record.back() == "retained-tail");
					if (matrix)
						CHECK(record[1]["future_matrix"] == 23);
					else {
						const auto retained = Json::parse(record[1].get<std::string>());
						CHECK(retained["future_gradient"] == 19);
						CHECK(retained["keys"][0]["future_key"] == "keep");
					}
					continue;
				}
				const auto &data = record[1];
				if (matrix) {
					REQUIRE(data.is_object());
					CHECK(data.size() == 3);
					CHECK(data["size"] == Json::array({2, 3}));
					CHECK(data["isize"] == 6);
					CHECK(data["raw"] == Json::array({3, 4, 5, 6, 7, 8}));
				} else {
					REQUIRE(data.is_string());
					const auto gradient = Json::parse(data.get<std::string>());
					CHECK(gradient.size() == 2);
					REQUIRE(gradient["keys"].size() == 2);
					CHECK(gradient["keys"][0].size() == 2);
					CHECK(gradient["keys"][1].size() == 2);
				}
			}
			auto document = imported.Graph;
			document.Outputs = {{"value", "object", matrix ? "matrix" : "gradient"}};
			Plan plan;
			REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
			EvaluationRequest request;
			REQUIRE(SetFrameTime(request, GetFrameTime(key)));
			EvaluatedValue value{"previous", int64_t{91}, std::nullopt};
			const auto previous = value;
			const auto evaluated = EvaluateValue(document, plan, "value", request, value, diagnostic);
			INFO(diagnostic.Message);
			if (matrix && time == 5) {
				CHECK(evaluated == Status::UnsupportedExecution);
				CHECK(value == previous);
			} else {
				REQUIRE(evaluated == Status::Ok);
				CHECK(value.Data == key.Data);
			}
			Document restored;
			REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
			CHECK(restored == document);
		}
}
TEST_CASE(
	"PXC object key insertion bounds payloads and preserves caller bytes on refusal",
	"[imagegraphio][pxcx_object_key_insert]"
) {
	for (bool matrix : {false, true}) {
		const auto original = Fixture(matrix);
		auto key = NewKey(matrix, 5);
		if (matrix)
			std::get<MatrixValue>(key.Data).Values[0] = std::numeric_limits<double>::infinity();
		else
			std::get<Gradient>(key.Data).Keys[1].Time = -1;
		const std::array<PxcxEdit, 1> edits{PxcxKeyframeInsertEdit{"object", key.Port, key}};
		std::vector<std::byte> output{std::byte{42}};
		const auto previous = output;
		Diagnostic diagnostic;
		CHECK_FALSE(WritePxcxEdits(original, original.Source.OriginalBytes, edits, output, diagnostic));
		CHECK(output == previous);
		key = NewKey(matrix, 5);
		if (matrix) {
			auto &oversizedMatrix = std::get<MatrixValue>(key.Data);
			oversizedMatrix.Columns = 1;
			oversizedMatrix.Rows = Limits::MaximumArrayElements + 1;
			oversizedMatrix.Values.resize(Limits::MaximumArrayElements + 1);
		} else
			std::get<Gradient>(key.Data).Keys.resize(
				Limits::MaximumGradientKeys + 1, std::get<Gradient>(key.Data).Keys.back()
			);
		const std::array<PxcxEdit, 1> oversized{PxcxKeyframeInsertEdit{"object", key.Port, key}};
		CHECK_FALSE(WritePxcxEdits(original, original.Source.OriginalBytes, oversized, output, diagnostic));
		CHECK(output == previous);
	}
}
