#include <engine/imagegraph/BuiltinRandomCaptureCodec.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.array_sample_builtin")
using namespace engine::imagegraph;
namespace {
	Document Fixture(bool nested = false) {
		Document d;
		d.FormatVersion = 9;
		Node source{"source", "pc.array", "", {}, {}};
		source.DynamicInputs = {
			{"input_0", ValueType::Scalar, Value{10.}},
			{"input_1", ValueType::Scalar, Value{20.}},
			{"input_2", ValueType::Scalar, Value{30.}}
		};
		d.Nodes = {
			source,
			{"sample",
			 "pc.array_sample",
			 "",
			 {},
			 {{"mode", EnumValue{1}},
			  {"amount_type", EnumValue{1}},
			  {"amount", int64_t{3}},
			  {"seed", 12345.},
			  {"dimension", int64_t{nested ? 1 : 0}}}}
		};
		if (nested) {
			Node rows{"rows", "pc.array", "", {}, {}};
			rows.DynamicInputs = {
				{"input_0", ValueType::Array, std::nullopt}, {"input_1", ValueType::Array, std::nullopt}
			};
			d.Nodes.push_back(rows);
			d.Links = {
				{"source", "array", "rows", "input_0"},
				{"source", "array", "rows", "input_1"},
				{"rows", "array", "sample", "array"}
			};
		} else
			d.Links = {{"source", "array", "sample", "array"}};
		d.Outputs = {{"result", "sample", "array"}};
		return d;
	}
} // namespace
TEST_CASE(
	"Captured Array Sample calls preserve selected values through "
	"durable replay",
	"[array_sample_builtin]"
) {
	for (bool nested : {false, true}) {
		auto d = Fixture(nested);
		Plan p;
		Diagnostic diag;
		auto s = Compile(d, p, diag);
		INFO(diag.Message);
		REQUIRE(s == Status::Ok);
		EvaluationRequest req;
		SourceBuiltinRandomCapture c;
		s = PrepareSourceBuiltinRandomCapture(d, p, "sample", req, c, diag);
		INFO(diag.Message);
		REQUIRE(s == Status::Ok);
		for (int i = 0; i < (nested ? 2 : 1); ++i)
			for (double v : {1., 1., 0.})
				c.Draws.push_back({SourceBuiltinRandomOperation::IRandom, 0, 2, v});
		std::string text;
		REQUIRE(WriteBuiltinRandomCapture({&c, 1}, text, diag) == Status::Ok);
		std::vector<SourceBuiltinRandomCapture> loaded;
		REQUIRE(ReadBuiltinRandomCapture(text, loaded, diag) == Status::Ok);
		req.BuiltinRandomCaptures = loaded;
		EvaluatedValue result;
		s = EvaluateValue(d, p, "result", req, result, diag);
		INFO(diag.Message);
		REQUIRE(s == Status::Ok);
		const auto &a = std::get<ArrayValue>(result.Data);
		const std::vector<ElementValue> expected = {20., 20., 10.};
		if (nested) {
			REQUIRE(a.Nested.size() == 2);
			CHECK(a.Nested[0] == expected);
			CHECK(a.Nested[1] == expected);
		} else
			CHECK(a.Elements == expected);
		const auto previous = result;
		req.BuiltinRandomCaptures = {};
		EvaluatedValue native;
		REQUIRE(EvaluateValue(d, p, "result", req, native, diag) == Status::Ok);
		CHECK(native.Data != previous.Data);
		req.BuiltinRandomCaptures = loaded;
		loaded[0].Tick = 1;
		CHECK(EvaluateValue(d, p, "result", req, result, diag) == Status::UnsupportedExecution);
		CHECK(result.Data == previous.Data);
		loaded[0] = c;
		loaded[0].Draws.back().Upper = 3;
		CHECK(EvaluateValue(d, p, "result", req, result, diag) == Status::InvalidValue);
		CHECK(result.Data == previous.Data);
		loaded[0] = c;
		loaded[0].Draws.pop_back();
		CHECK(EvaluateValue(d, p, "result", req, result, diag) == Status::UnsupportedExecution);
		CHECK(result.Data == previous.Data);
		loaded[0] = c;
		loaded[0].Draws.push_back(c.Draws[0]);
		CHECK(EvaluateValue(d, p, "result", req, result, diag) == Status::InvalidValue);
		CHECK(result.Data == previous.Data);
		loaded[0] = c;
		loaded[0].Inputs[0].Data = 999.;
		CHECK(EvaluateValue(d, p, "result", req, result, diag) == Status::InvalidValue);
		CHECK(result.Data == previous.Data);
	}
}
