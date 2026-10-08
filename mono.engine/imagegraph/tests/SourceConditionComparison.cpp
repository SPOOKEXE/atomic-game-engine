#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>
TEST_SUITE_ID("engine.imagegraph.source_condition_comparison")
using namespace engine::imagegraph;
namespace {
	Document Comparison(Value left, Value right, int64_t mode) {
		Document document;
		document.FormatVersion = 9;
		Node keys{"keys", "pc.array", "", {}, {{"type", EnumValue{0}}}};
		const auto inputType = [](const Value &value) {
			if (std::holds_alternative<int64_t>(value)) return ValueType::Integer;
			if (std::holds_alternative<std::string>(value)) return ValueType::Text;
			return ValueType::Scalar;
		};
		keys.DynamicInputs = {{"input_0", inputType(left), std::move(left)}};
		Node rightKeys{"rightKeys", "pc.array", "", {}, {{"type", EnumValue{0}}}};
		rightKeys.DynamicInputs = {{"input_0", inputType(right), std::move(right)}};
		document.Nodes = {
			std::move(keys),
			{"left", "pc.array_get", "", {}, {{"index", int64_t{0}}}},
			{"right", "pc.array_get", "", {}, {{"index", int64_t{0}}}},
			{"condition", "pc.condition", "", {}, {{"eval_mode", EnumValue{mode}}}},
			{"yes", "pc.string", "", {}, {{"text", std::string{"selected true"}}}},
			{"no", "pc.string", "", {}, {{"text", std::string{"selected false"}}}},
			std::move(rightKeys)
		};
		document.Links = {
			{"keys", "array", "left", "array"},
			{"rightKeys", "array", "right", "array"},
			{"left", "value", "condition", mode == 2 ? "text_1" : "check_value"},
			{"right", "value", "condition", mode == 2 ? "text_2" : "compare_to"},
			{"yes", "text", "condition", "true"},
			{"no", "text", "condition", "false"}
		};
		document.Outputs = {{"bool", "condition", "bool"}, {"result", "condition", "result"}};
		return document;
	}
	void Check(Document document, bool expected) {
		Document restored;
		Diagnostic diagnostic;
		REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
		Plan plan;
		REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
		EvaluatedValue result;
		const auto status = EvaluateValue(restored, plan, "bool", {}, result, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		CHECK(std::get<bool>(result.Data) == expected);
		REQUIRE(EvaluateValue(restored, plan, "result", {}, result, diagnostic) == Status::Ok);
		CHECK(std::get<std::string>(result.Data) == (expected ? "selected true" : "selected false"));
	}
} // namespace
TEST_CASE(
	"Condition Text compares raw linked numbers rather than empty fallback strings", "[source_condition]"
) {
	Check(Comparison(2., 3., 2), false);
	Check(Comparison(2., 2., 2), true);
	Check(Comparison(std::string{"2tail"}, 2., 2), true);
	Check(Comparison(std::string{"2tail"}, 3., 2), false);
	Check(Comparison(std::string{"01"}, std::string{"1"}, 2), false);
}
TEST_CASE("Condition Number uses the pinned comparison epsilon in all six operators", "[source_condition]") {
	const std::array<bool, 6> expected{true, false, false, true, false, true};
	for (size_t index = 0; index < expected.size(); ++index) {
		auto document = Comparison(1., 1.000005, 1);
		document.Nodes[3].Values.push_back({"condition", EnumValue{int64_t(index)}});
		Check(std::move(document), expected[index]);
	}
}
TEST_CASE("Condition preserves adjacent linked integers above double precision", "[source_condition]") {
	const std::array<bool, 6> expected{false, true, false, false, true, true};
	for (size_t index = 0; index < expected.size(); ++index) {
		auto document = Comparison(int64_t{9007199254740993}, int64_t{9007199254740992}, 1);
		document.Nodes[3].Values.push_back({"condition", EnumValue{int64_t(index)}});
		Check(std::move(document), expected[index]);
	}
}

namespace {
	void ReplaceOperand(Document &document, size_t index, Node producer, std::string_view output) {
		const auto id = document.Nodes[index].Id;
		std::erase_if(document.Links, [&](const Link &link) { return link.ToNode == id; });
		for (auto &link : document.Links)
			if (link.FromNode == id) link.FromPort = output;
		document.Nodes[index] = std::move(producer);
	}
}
TEST_CASE("Condition Float getters convert typed Text and reject array comparisons", "[source_condition]") {
	auto document = Comparison(0., 0., 1);
	ReplaceOperand(
		document, 1, {"left", "pc.struct_get", "", {}, {{"key", std::string{"payload"}}}}, "value"
	);
	ReplaceOperand(
		document, 2, {"right", "pc.struct_get", "", {}, {{"key", std::string{"payload"}}}}, "value"
	);
	const auto leftObjectIndex = document.Nodes.size();
	document.Nodes.push_back(
		{"leftObject", "pc.struct_json_parse", "", {}, {{"json_string", std::string{R"({"payload":"01"})"}}}}
	);
	const auto rightObjectIndex = document.Nodes.size();
	document.Nodes.push_back(
		{"rightObject", "pc.struct_json_parse", "", {}, {{"json_string", std::string{R"({"payload":"1"})"}}}}
	);
	document.Links.push_back({"leftObject", "struct", "left", "struct"});
	document.Links.push_back({"rightObject", "struct", "right", "struct"});
	// Struct Get declares Any, then source Auto mode publishes the selected field's Text domain.
	Check(document, true);
	document.Nodes[leftObjectIndex].Values[0].Data = std::string{R"({"payload":"not numeric"})"};
	document.Nodes[rightObjectIndex].Values[0].Data = std::string{R"({"payload":"also not numeric"})"};
	Check(document, true);
	ReplaceOperand(
		document, 1, {"left", "pc.equation", "", {}, {{"equation", std::string{"[1,2]"}}}}, "result"
	);
	for (int64_t operation = 0; operation < 6; ++operation) {
		document.Nodes[3].Values = {{"eval_mode", EnumValue{1}}, {"condition", EnumValue{operation}}};
		Check(document, false);
	}
}
TEST_CASE(
	"Condition raw reference equality refuses without replacing the caller output", "[source_condition]"
) {
	auto document = Comparison(0., 0., 2);
	ReplaceOperand(
		document, 1, {"left", "pc.equation", "", {}, {{"equation", std::string{"[1,2]"}}}}, "result"
	);
	ReplaceOperand(
		document, 2, {"right", "pc.equation", "", {}, {{"equation", std::string{"[1,2]"}}}}, "result"
	);
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluatedValue value;
	value.Data = int64_t{77};
	CHECK(EvaluateValue(document, plan, "result", {}, value, diagnostic) == Status::UnsupportedExecution);
	CHECK(std::get<int64_t>(value.Data) == 77);
}

TEST_CASE(
	"Condition Long overflow ordering refuses atomically while equality remains available",
	"[source_condition]"
) {
	for (bool reverse : {false, true}) {
		const int64_t left =
			reverse ? std::numeric_limits<int64_t>::max() : std::numeric_limits<int64_t>::min();
		const int64_t right =
			reverse ? std::numeric_limits<int64_t>::min() : std::numeric_limits<int64_t>::max();
		for (int64_t operation = 0; operation < 6; ++operation) {
			auto document = Comparison(left, right, 1);
			document.Nodes[3].Values.push_back({"condition", EnumValue{operation}});
			if (operation < 2) {
				Check(document, operation == 1);
				continue;
			}
			Document restored;
			Diagnostic diagnostic;
			REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
			Plan plan;
			REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
			EvaluatedValue value;
			value.Data = int64_t{77};
			CHECK(
				EvaluateValue(restored, plan, "result", {}, value, diagnostic) == Status::UnsupportedExecution
			);
			CHECK(diagnostic.NodeId == "condition");
			CHECK(diagnostic.Port == "check_value");
			CHECK(
				diagnostic.Message ==
				"SDK Long overflow comparison requires captured runtime or profile proof"
			);
			CHECK(std::get<int64_t>(value.Data) == 77);
		}
	}
}

namespace {
	Document TypedTextComparison(std::string text, double number) {
		auto document = Comparison(0., number, 1);
		ReplaceOperand(
			document, 1, {"left", "pc.struct_get", "", {}, {{"key", std::string{"payload"}}}}, "value"
		);
		Node object{"textObject", "pc.struct", ""};
		object.DynamicInputs = {
			{"key_0", ValueType::Text, Value{std::string{"payload"}}},
			{"value_0", ValueType::Any, Value{std::move(text)}}
		};
		document.Nodes.push_back(std::move(object));
		document.Links.push_back({"textObject", "struct", "left", "struct"});
		return document;
	}
}
TEST_CASE("Condition typed Text Float getter uses full real conversion", "[source_condition]") {
	Check(TypedTextComparison("0x10", 16.), true);
	Check(TypedTextComparison("1e123", 1e123), true);
	Check(TypedTextComparison("1e-9999", 0.), true);
	Check(TypedTextComparison("-1e-9999", 0.), true);
}
TEST_CASE("Condition typed Text real overflow refuses atomically", "[source_condition]") {
	auto document = TypedTextComparison("1e309", 0.);
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	Plan plan;
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	EvaluatedValue value;
	value.Data = int64_t{77};
	CHECK(EvaluateValue(restored, plan, "result", {}, value, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.NodeId == "condition");
	CHECK(diagnostic.Port == "check_value");
	CHECK(
		diagnostic.Message == "condition typed Text real conversion requires a bounded finite source value"
	);
	CHECK(std::get<int64_t>(value.Data) == 77);
}

TEST_CASE(
	"Condition Number treats tuple and Matrix Float getter carriers as source arrays", "[source_condition]"
) {
	const std::array<Value, 3> carriers{
		Vector4{1, 2, 3, 4}, Quaternion{0, 0, 0, 1}, MatrixValue{2, 2, {1, 2, 3, 4}}
	};
	for (const auto &carrier : carriers) {
		for (bool right : {false, true}) {
			for (int64_t operation = 0; operation < 6; ++operation) {
				auto document = Comparison(0., 0., 1);
				document.Nodes[3].Values.push_back({"condition", EnumValue{operation}});
				Node object{"carrierObject", "pc.struct", ""};
				object.DynamicInputs = {
					{"key_0", ValueType::Text, Value{std::string{"payload"}}},
					{"value_0", ValueType::Any, carrier}
				};
				const auto id = right ? "right" : "left";
				ReplaceOperand(
					document,
					right ? 2 : 1,
					{id, "pc.struct_get", "", {}, {{"key", std::string{"payload"}}}},
					"value"
				);
				document.Nodes.push_back(std::move(object));
				document.Links.push_back({"carrierObject", "struct", id, "struct"});
				Check(std::move(document), false);
			}
		}
	}
}
