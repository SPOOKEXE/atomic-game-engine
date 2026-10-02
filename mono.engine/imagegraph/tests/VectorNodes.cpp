// Computational vector fixtures use source-derived values and persisted real graph links.

#include "../src/ProcessorBatch.hpp"
#include "../src/ValuePayload.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <string>
#include <tuple>

TEST_SUITE_ID("engine.imagegraph.vector_nodes")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;

namespace {
	ArrayValue Numeric(std::initializer_list<double> entries) {
		ArrayValue array{ValueType::Scalar, {}};
		for (double entry : entries)
			array.Elements.emplace_back(entry);
		return array;
	}
	Value EvaluateVector(Document document, std::string output = "out") {
		Document parsed;
		Diagnostic diagnostic;
		Plan plan;
		REQUIRE(Read(Write(document), parsed, diagnostic) == Status::Ok);
		CHECK(parsed == document);
		const Status compile = Compile(parsed, plan, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(compile == Status::Ok);
		EvaluatedValue result;
		const std::string retained = Write(parsed);
		const Status status = EvaluateValue(parsed, plan, output, {}, result, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		CHECK(Write(parsed) == retained);
		return result.Data;
	}
}

TEST_CASE(
	"Vector2 and Vector4 creators publish rounded components with typed packed values",
	"[imagegraph][vector_nodes]"
) {
	const auto two = RunNode("pc.vector2", {}, {{"x", 2.5}, {"y", -3.5}, {"integer", true}});
	REQUIRE(two.Ok);
	CHECK(*two.OutputValue("vector") == Value{Vector2{2, -4}});
	CHECK(*two.OutputValue("x") == Value{2.0});
	CHECK(*two.OutputValue("y") == Value{-4.0});
	const auto four =
		RunNode("pc.vector4", {}, {{"x", 1.5}, {"y", 2.5}, {"z", -1.5}, {"w", -2.5}, {"integer", true}});
	REQUIRE(four.Ok);
	CHECK(*four.OutputValue("vector") == Value{Vector4{2, 2, -2, -2}});
	CHECK(*four.OutputValue("z") == Value{-2.0});
	CHECK(*four.OutputValue("w") == Value{-2.0});
	const auto display = RunNode(
		"pc.vector2",
		{},
		{{"x", 2.5},
		 {"y", 4.0},
		 {"display_type", EnumValue{1}},
		 {"relative_unit", true},
		 {"gizmo_scale", 8.0},
		 {"gizmo_offset", Vector2{10, 20}}}
	);
	REQUIRE(display.Ok);
	CHECK(*display.OutputValue("vector") == Value{Vector2{2.5, 4}});
}

TEST_CASE(
	"Vector Math computes all eight modes and Scalar B changes row interpretation",
	"[imagegraph][vector_nodes]"
) {
	const std::array<Value, 8> expected{
		Value{Numeric({5, 6})},
		Value{Numeric({1, 2})},
		Value{Numeric({6, 8})},
		Value{Numeric({1.5, 2})},
		Value{Numeric({9, 16})},
		Value{Numeric({std::sqrt(3.0), 2})},
		Value{5.0},
		Value{std::sqrt(5.0)}
	};
	for (int64_t mode = 0; mode < 8; ++mode) {
		CAPTURE(mode);
		const auto run =
			RunNode("pc.vector_math", {}, {{"type", EnumValue{mode}}, {"a", Vector2{3, 4}}, {"b", 2.0}});
		REQUIRE(run.Ok);
		CHECK(*run.OutputValue("result") == expected[mode]);
	}
	const auto scalar =
		RunNode("pc.vector_math", {}, {{"a", Numeric({1, 2})}, {"b", Numeric({10, 20})}, {"scalar_b", true}});
	REQUIRE(scalar.Ok);
	ArrayValue rows{ValueType::Scalar, {}};
	rows.Nested = {{11.0, 12.0}, {21.0, 22.0}};
	CHECK(*scalar.OutputValue("result") == Value{rows});
	const auto vector = RunNode(
		"pc.vector_math", {}, {{"a", Numeric({1, 2})}, {"b", Numeric({10, 20})}, {"scalar_b", false}}
	);
	REQUIRE(vector.Ok);
	CHECK(*vector.OutputValue("result") == Value{Numeric({11, 22})});
	const auto fraction = RunNode("pc.vector_math", {}, {{"type", .5}, {"a", Vector2{3, 4}}, {"b", 2.0}});
	REQUIRE(fraction.Ok);
	CHECK(*fraction.OutputValue("result") == Value{Numeric({0, 0})});
}

TEST_CASE(
	"Vector Math resizes independent immutable rows and loops row counts", "[imagegraph][vector_nodes]"
) {
	ArrayValue a{ValueType::Scalar, {}};
	a.Nested = {{1.0, 2.0, 9.0}, {3.0}};
	ArrayValue b{ValueType::Scalar, {}};
	b.Nested = {{10.0, 20.0}};
	const auto savedA = a, savedB = b;
	const auto run = RunNode("pc.vector_math", {}, {{"a", a}, {"b", b}, {"dimension", int64_t{2}}});
	REQUIRE(run.Ok);
	ArrayValue expected{ValueType::Scalar, {}};
	expected.Nested = {{11.0, 22.0}, {13.0, 20.0}};
	CHECK(*run.OutputValue("result") == Value{expected});
	CHECK(a == savedA);
	CHECK(b == savedB);
	const auto padding =
		RunNode("pc.vector_math", {}, {{"a", Numeric({1})}, {"b", 1.0}, {"dimension", int64_t{3}}});
	REQUIRE(padding.Ok);
	CHECK(*padding.OutputValue("result") == Value{Numeric({2, 1, 1})});
	for (const auto &[dimension, expectedCount] :
		 {std::pair{1.5, size_t{2}}, std::pair{2.5, size_t{2}}, std::pair{3.5, size_t{4}}}) {
		const auto rounded =
			RunNode("pc.vector_math", {}, {{"a", 2.0}, {"b", 3.0}, {"dimension", dimension}});
		REQUIRE(rounded.Ok);
		CHECK(std::get<ArrayValue>(*rounded.OutputValue("result")).Elements.size() == expectedCount);
	}
	const auto zero = RunNode("pc.vector_math", {}, {{"dimension", int64_t{0}}});
	REQUIRE(zero.Ok);
	CHECK(*zero.OutputValue("result") == Value{Numeric({})});
	const auto zeroLength =
		RunNode("pc.vector_math", {}, {{"dimension", int64_t{0}}, {"type", EnumValue{6}}});
	REQUIRE(zeroLength.Ok);
	CHECK(*zeroLength.OutputValue("result") == Value{0.0});
}

TEST_CASE(
	"Vector Math diagnoses source invalid shapes domains and bounded work atomically",
	"[imagegraph][vector_nodes]"
) {
	ArrayValue firstEmpty{ValueType::Scalar, {}};
	firstEmpty.Nested = {{}, {1.0}};
	for (const auto &value : {Value{firstEmpty}, Value{std::string("bad")}}) {
		const auto run = RunNode("pc.vector_math", {}, {{"a", value}});
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::UnsupportedExecution);
		CHECK(run.Values.empty());
	}
	const auto emptyB = RunNode("pc.vector_math", {}, {{"b", Numeric({})}, {"scalar_b", true}});
	CHECK_FALSE(emptyB.Ok);
	CHECK(emptyB.Code == Status::UnsupportedExecution);
	const auto vectorEmptyB =
		RunNode("pc.vector_math", {}, {{"a", Numeric({1, 2})}, {"b", Numeric({})}, {"scalar_b", false}});
	REQUIRE(vectorEmptyB.Ok);
	CHECK(*vectorEmptyB.OutputValue("result") == Value{Numeric({1, 2})});
	for (const auto &[mode, a, b] :
		 {std::tuple{4, -1.0, .5},
		  std::tuple{5, -1.0, 2.0},
		  std::tuple{6, std::numeric_limits<double>::max(), 1.0}}) {
		const auto run = RunNode("pc.vector_math", {}, {{"type", EnumValue{mode}}, {"a", a}, {"b", b}});
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::InvalidValue);
		CHECK(run.Values.empty());
	}
	ArrayValue many{ValueType::Scalar, {}};
	many.Nested.resize(65, {1.0});
	const auto expanded = RunNode("pc.vector_math", {}, {{"a", many}, {"dimension", int64_t{65}}});
	CHECK_FALSE(expanded.Ok);
	CHECK(expanded.Code == Status::LimitExceeded);
	CHECK(expanded.Values.empty());
	const auto negative = RunNode("pc.vector_math", {}, {{"dimension", int64_t{-1}}});
	CHECK_FALSE(negative.Ok);
	CHECK(negative.Code == Status::LimitExceeded);
}

TEST_CASE(
	"Persisted vector creators share processor scheduling and component outputs including Vector3",
	"[imagegraph][vector_nodes]"
) {
	for (const std::string type : {"pc.vector2", "pc.vector3", "pc.vector4"}) {
		Document doc;
		doc.FormatVersion = 8;
		doc.Nodes = {
			{"vector", type, "", {}, {{"x", Numeric({1.5, 2.5})}, {"y", Numeric({10.5})}, {"integer", true}}}
		};
		doc.Outputs = {{"out", "vector", "x"}, {"y", "vector", "y"}};
		CHECK(EvaluateVector(doc) == Value{Numeric({2, 2})});
		CHECK(EvaluateVector(doc, "y") == Value{Numeric({10, 10})});
	}
	Document doc;
	doc.FormatVersion = 8;
	doc.Nodes = {{"vector", "pc.vector2", "", {}, {{"x", Numeric({1, 2})}, {"y", Numeric({10, 20, 30})}}}};
	doc.Outputs = {{"out", "vector", "vector"}};
	const std::array<std::vector<ElementValue>, 4> expected{
		std::vector<ElementValue>{Vector2{1, 10}, Vector2{2, 20}, Vector2{1, 30}},
		std::vector<ElementValue>{Vector2{1, 10}, Vector2{2, 20}, Vector2{2, 30}},
		std::vector<ElementValue>{
			Vector2{1, 10}, Vector2{1, 20}, Vector2{1, 30}, Vector2{2, 10}, Vector2{2, 20}, Vector2{2, 30}
		},
		std::vector<ElementValue>{
			Vector2{1, 10}, Vector2{2, 20}, Vector2{1, 30}, Vector2{2, 10}, Vector2{1, 20}, Vector2{2, 30}
		}
	};
	for (int64_t mode = 0; mode < 4; ++mode) {
		doc.Nodes[0].Values.push_back({"attribute_array_process", EnumValue{mode}});
		CHECK(std::get<ArrayValue>(EvaluateVector(doc)).Elements == expected[mode]);
		doc.Nodes[0].Values.pop_back();
	}
}

TEST_CASE(
	"Persisted vector rows chain into distances and a real image parameter", "[imagegraph][vector_nodes]"
) {
	Document doc;
	doc.FormatVersion = 8;
	doc.Nodes = {
		{"vectors", "pc.vector4", "", {}, {{"x", Numeric({3, 6})}, {"y", Numeric({4, 8})}}},
		{"length", "pc.vector_math", "", {}, {{"type", EnumValue{6}}, {"dimension", int64_t{4}}}}
	};
	doc.Links = {{"vectors", "vector", "length", "a"}};
	doc.Outputs = {{"out", "length", "result"}};
	CHECK(EvaluateVector(doc) == Value{Numeric({5, 10})});
	Document image;
	image.FormatVersion = 8;
	image.Nodes = {
		{"vector", "pc.vector2", "", {}, {{"x", 3.0}, {"y", 4.0}}},
		{"length", "pc.vector_math", "", {}, {{"type", EnumValue{6}}}},
		{"fill",
		 "pc.solid",
		 "",
		 {},
		 {{"dimension", Vector2{1, 1}}, {"dimension_unit", EnumValue{0}}, {"color", Colour{10, 20, 30, 255}}}}
	};
	image.Links = {{"vector", "vector", "length", "a"}, {"length", "result", "fill", "dimension"}};
	image.Outputs = {{"out", "fill", "surface_out"}};
	Document parsed;
	Diagnostic diagnostic;
	Plan plan;
	REQUIRE(Read(Write(image), parsed, diagnostic) == Status::Ok);
	REQUIRE(Compile(parsed, plan, diagnostic) == Status::Ok);
	Image result;
	REQUIRE(Evaluate(parsed, plan, "out", result, diagnostic) == Status::Ok);
	CHECK(result.Width == 5);
	CHECK(result.Height == 5);
	CHECK(result.Pixels.size() == 100);
}

TEST_CASE(
	"Vector output publication sums every Value and refuses before materializing",
	"[imagegraph][vector_nodes]"
) {
	const auto invoke = [](std::string_view type, uint64_t bytes) {
		const auto *entry = FindCatalogueEntry(type);
		REQUIRE(entry);
		Node node{"vector", std::string(type), "", {}, {}};
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = bytes;
		for (const auto &input : entry->Inputs)
			if (auto value = CatalogueDefault(input)) context.Values.emplace_back(input.Id, *value);
		const auto executor = detail::FindExecutor(type);
		REQUIRE(executor);
		const bool ok = executor(context);
		if (!ok) CHECK(context.FailureCode == Status::LimitExceeded);
		return std::pair{ok, std::move(context.OutputValues)};
	};
	const auto outputSlots = [](std::string_view type) {
		const auto *entry = FindCatalogueEntry(type);
		REQUIRE(entry);
		return entry->Outputs.size() * (sizeof(std::pair<std::string, Image>) + sizeof(AuthoredValue) +
										(sizeof(std::pair<std::string, ImageArray>) +
										 sizeof(std::pair<std::string_view, SourceSocketDomain>)));
	};
	const uint64_t nameBytes = std::string{}.capacity();
	for (const auto &[type, count] :
		 {std::pair{"pc.vector2", uint64_t{3}}, std::pair{"pc.vector4", uint64_t{5}}}) {
		const auto exact = invoke(type, outputSlots(type) + count * nameBytes);
		REQUIRE(exact.first);
		CHECK(exact.second.size() == count);
		const auto shortBudget = invoke(type, outputSlots(type) + count * nameBytes - 1);
		CHECK_FALSE(shortBudget.first);
		CHECK(shortBudget.second.empty());
	}
	const uint64_t vectorBytes = outputSlots("pc.vector_math") + nameBytes + 2 * sizeof(ElementValue);
	const auto exactMath = invoke("pc.vector_math", vectorBytes);
	REQUIRE(exactMath.first);
	CHECK(exactMath.second.front().Data == Value{Numeric({0, 0})});
	const auto shortMath = invoke("pc.vector_math", vectorBytes - 1);
	CHECK_FALSE(shortMath.first);
	CHECK(shortMath.second.empty());
}

TEST_CASE(
	"Source Boolean controls use the documented real threshold on compiled links",
	"[imagegraph][vector_nodes]"
) {
	for (const std::string type : {"pc.vector2", "pc.vector3", "pc.vector4"}) {
		for (const auto &[flag, expected] :
			 {std::pair{-1.0, 2.5},
			  std::pair{.49999, 2.5},
			  std::pair{.5, 2.5},
			  std::pair{.50001, 2.0},
			  std::pair{1.0, 2.0}}) {
			Document doc;
			doc.FormatVersion = 8;
			doc.Nodes = {
				{"flag", "pc.number", "", {}, {{"value", flag}}}, {"vector", type, "", {}, {{"x", 2.5}}}
			};
			doc.Links = {{"flag", "number", "vector", "integer"}};
			doc.Outputs = {{"out", "vector", "x"}};
			CAPTURE(type, flag);
			CHECK(EvaluateVector(doc) == Value{expected});
		}
	}
	const auto vectorB =
		RunNode("pc.vector_math", {}, {{"a", Numeric({1, 2})}, {"b", Numeric({10, 20})}, {"scalar_b", .5}});
	REQUIRE(vectorB.Ok);
	CHECK(*vectorB.OutputValue("result") == Value{Numeric({11, 22})});
}
