#include "../src/ValuePayload.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>
TEST_SUITE_ID("engine.imagegraph.node_array_unique")
using namespace engine::imagegraph;
using namespace imagegraph_test;
namespace {
	const ArrayValue &UniqueOutput(const NodeRun &run) {
		INFO(run.Message);
		REQUIRE(run.Ok);
		REQUIRE(run.OutputValue("unique_array"));
		return std::get<ArrayValue>(*run.OutputValue("unique_array"));
	}
	ArrayValue Recursive(size_t depth) {
		SourceArrayItem item{ElementValue{double{1}}};
		for (size_t level = 1; level < depth; ++level)
			item = {std::vector<SourceArrayItem>{std::move(item)}};
		ArrayValue array{ValueType::Any, {}};
		array.Items.push_back(std::move(item));
		return array;
	}
} // namespace
TEST_CASE(
	"Unique native policy keeps first scalar occurrence and exact "
	"numeric distinctions",
	"[imagegraph][array_unique]"
) {
	const int64_t adjacent = int64_t{1} << 53;
	ArrayValue input{ValueType::Any, {}};
	input.Items = {
		{ElementValue{adjacent}},
		{ElementValue{adjacent + 1}},
		{ElementValue{double(adjacent)}},
		{ElementValue{true}},
		{ElementValue{int64_t{1}}},
		{ElementValue{EnumValue{1}}},
		{ElementValue{std::string{"1"}}},
		{ElementValue{std::string{"1"}}},
		{ElementValue{std::numeric_limits<int64_t>::min()}},
		{ElementValue{-9223372036854775808.0}},
		{ElementValue{std::numeric_limits<int64_t>::max()}},
		{ElementValue{9223372036854775808.0}}
	};
	const auto run = RunNode("pc.array_unique", {}, {{"array_in", input}});
	CHECK(
		UniqueOutput(run).Items == std::vector<SourceArrayItem>{
									   input.Items[0],
									   input.Items[1],
									   input.Items[3],
									   input.Items[6],
									   input.Items[8],
									   input.Items[10],
									   input.Items[11]
								   }
	);
	CHECK(input.Items.size() == 12);
}
TEST_CASE("Unique accepts empty typed arrays and scalar text deduplication", "[imagegraph][array_unique]") {
	const ArrayValue empty{ValueType::Text, {}};
	CHECK(UniqueOutput(RunNode("pc.array_unique", {}, {{"array_in", empty}})) == empty);
	const ArrayValue input{
		ValueType::Text,
		{std::string{"b"}, std::string{"a"}, std::string{"b"}, std::string{""}, std::string{"a"}}
	};
	CHECK(
		UniqueOutput(RunNode("pc.array_unique", {}, {{"array_in", input}})).Elements ==
		std::vector<ElementValue>{std::string{"b"}, std::string{"a"}, std::string{""}}
	);
	const auto invalid = RunNode("pc.array_unique", {}, {{"array_in", 2.0}});
	CHECK(invalid.Code == Status::InvalidValue);
	CHECK(invalid.Values.empty());
	CHECK(invalid.Port == "array_in");
}
TEST_CASE(
	"Unique does not import Find recursive cutoff or invent array "
	"reference identity",
	"[imagegraph][array_unique]"
) {
	for (size_t depth : {size_t{8}, size_t{9}, Limits::MaximumArrayDepth}) {
		auto input = Recursive(depth);
		REQUIRE(detail::ValidRuntimeValue(Value{input}));
		const auto single = RunNode("pc.array_unique", {}, {{"array_in", input}});
		// A single member needs no equality decision and retains its complete owned
		// shape.
		CHECK(UniqueOutput(single) == input);
		input.Items.push_back(input.Items.front());
		const auto ambiguous = RunNode("pc.array_unique", {}, {{"array_in", input}});
		CHECK(ambiguous.Code == Status::UnsupportedExecution);
		CHECK(ambiguous.Values.empty());
		CHECK(ambiguous.Images.empty());
		CHECK(ambiguous.Port == "array_in");
		CHECK(ambiguous.Message == "source unique array reference comparison is unverified");
	}
}
TEST_CASE(
	"Unique opaque comparisons fail without publishing copied image content", "[imagegraph][array_unique]"
) {
	const Image image{1, 1, std::vector<uint8_t>{1, 2, 3, 255}, 17};
	ArrayValue input{ValueType::Any, {}};
	input.Items = {{image}, {image}};
	const auto ambiguous = RunNode("pc.array_unique", {}, {{"array_in", input}});
	CHECK(ambiguous.Code == Status::UnsupportedExecution);
	CHECK(ambiguous.Values.empty());
	CHECK(ambiguous.Images.empty());
	CHECK(ambiguous.Message == "source unique opaque identity comparison is unavailable");
	CHECK(std::get<Image>(input.Items[0].Data).Pixels == image.Pixels);
	ArrayValue colour{ValueType::Colour, {Colour{1, 0, 0, 1}, Colour{1, 0, 0, 1}}};
	const auto opaque = RunNode("pc.array_unique", {}, {{"array_in", colour}});
	CHECK(opaque.Code == Status::UnsupportedExecution);
	CHECK(opaque.Values.empty());
	CHECK(opaque.Message == "source unique opaque identity comparison is unavailable");
}
TEST_CASE("Unique executes upstream from a distinct snapshot target", "[imagegraph][array_unique]") {
	const ArrayValue source{ValueType::Integer, {int64_t{3}, int64_t{1}, int64_t{3}, int64_t{2}, int64_t{1}}};
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		Node{
			"source",
			"pc.array",
			"",
			{},
			{{"type", EnumValue{0}}, {"spread_array", true}},
			{{"value_0", ValueType::Array, Value{source}}}
		},
		Node{"unique", "pc.array_unique", "", {}, {}},
		Node{"capture", "pc.array_copy", "", {}, {}}
	};
	document.Links = {
		{"source", "array", "unique", "array_in"}, {"unique", "unique_array", "capture", "array"}
	};
	document.Outputs = {{"out", "capture", "array"}};
	Plan plan;
	Diagnostic diagnostic;
	const Status compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.NodeId << ":" << diagnostic.Port << " " << diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	EvaluationSnapshot snapshot;
	REQUIRE(EvaluateNodeInputs(document, plan, "capture", {}, snapshot, diagnostic) == Status::Ok);
	bool found = false;
	const ArrayValue expected{ValueType::Integer, {int64_t{3}, int64_t{1}, int64_t{2}}};
	for (const auto &value : snapshot.Values())
		if (value.Port == "array") {
			CHECK(value.Data == Value{expected});
			found = true;
		}
	CHECK(found);
}
TEST_CASE(
	"Unique output admission respects the actual live workspace boundary", "[imagegraph][array_unique]"
) {
	const Node node{"unique", "pc.array_unique", "", {}, {}};
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	const auto executor = detail::FindExecutor(node.Type);
	REQUIRE(executor);
	const Value source =
		ArrayValue{ValueType::Text, {std::string(1024 * 1024, 'x'), std::string(1024 * 1024, 'x')}};
	const EvaluationRequest request;
	uint64_t peak = 0;
	for (size_t attempt = 0; attempt < 3; ++attempt) {
		const uint64_t limit = attempt == 0 ? Limits::MaximumEvaluationBytes : attempt == 1 ? peak - 1 : peak;
		detail::EvaluationBudget budget(limit);
		auto unrelated = budget.Reserve(512);
		REQUIRE(unrelated);
		{
			detail::NodeContext context(node, *entry, request, budget);
			context.ByteBudget = limit;
			context.ValueViews = {{"array_in", &source}};
			const bool ok = executor(context);
			if (attempt == 1) {
				CHECK_FALSE(ok);
				CHECK(context.FailureCode == Status::LimitExceeded);
				CHECK(context.OutputValues.empty());
				CHECK(context.OutputImages.empty());
			} else {
				REQUIRE(ok);
				if (attempt == 0) peak = budget.Peak();
				CHECK(budget.Peak() == peak);
				REQUIRE(context.OutputValues.size() == 1);
				CHECK(std::get<ArrayValue>(context.OutputValues[0].Data).Elements.size() == 1);
			}
			CHECK(budget.Peak() <= limit);
		}
		CHECK(budget.Used() == 512);
	}
	CHECK(std::get<ArrayValue>(source).Elements.size() == 2);
}

TEST_CASE(
	"Unique singleton images and opaque values retain independent ownership", "[imagegraph][array_unique]"
) {
	const Node node{"unique", "pc.array_unique", "", {}, {}};
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	const auto executor = detail::FindExecutor(node.Type);
	REQUIRE(executor);
	const EvaluationRequest request;
	const Image original{1, 1, std::vector<uint8_t>{1, 2, 3, 255}, 17};
	ArrayValue input{ValueType::Any, {}};
	input.Items = {{original}};
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.Values = {{"array_in", input}};
	REQUIRE(executor(context));
	REQUIRE(context.OutputImageArrays.size() == 1);
	auto &output = context.OutputImageArrays.front();
	CHECK(output.first == "unique_array");
	REQUIRE(output.second.Images.size() == 1);
	CHECK(output.second.Images.front() == original);
	output.second.Images.front().Pixels.front() = 99;
	CHECK(std::get<Image>(input.Items.front().Data).Pixels.front() == 1);
	CHECK(
		std::get<Image>(std::get<ArrayValue>(context.Values.front().second).Items.front().Data)
			.Pixels.front() == 1
	);
	const ArrayValue colour{ValueType::Colour, {Colour{1, 2, 3, 255}}};
	CHECK(UniqueOutput(RunNode("pc.array_unique", {}, {{"array_in", colour}})) == colour);
}
