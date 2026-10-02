#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.array_numeric_nodes")

using namespace engine::imagegraph;
using imagegraph_test::RunNode;

namespace {
	ArrayValue Numbers(std::initializer_list<double> values) {
		ArrayValue array{ValueType::Scalar, {}};
		for (double value : values)
			array.Elements.emplace_back(value);
		return array;
	}
	void
	CheckNumbers(const imagegraph_test::NodeRun &run, std::string_view port, const ArrayValue &expected) {
		INFO(run.Message);
		REQUIRE(run.Ok);
		const auto *value = run.OutputValue(port);
		REQUIRE(value);
		CHECK(std::get<ArrayValue>(*value).Elements == expected.Elements);
	}
}

TEST_CASE("Array Range preserves pinned directed and inclusive arithmetic", "[imagegraph][array_numeric]") {
	CheckNumbers(RunNode("pc.array_range", {}), "array", Numbers({0, 1, 2, 3, 4, 5, 6, 7, 8, 9}));
	CheckNumbers(
		RunNode("pc.array_range", {}, {{"start", 5.0}, {"end", 0.0}, {"step", 2.0}, {"inclusive", true}}),
		"array",
		Numbers({5, 3})
	);
	CheckNumbers(
		RunNode("pc.array_range", {}, {{"type", EnumValue{1}}, {"start", 7.0}, {"step", -3.0}}),
		"array",
		Numbers({7, 7, 7})
	);
	CheckNumbers(
		RunNode("pc.array_range", {}, {{"start", 2.0}, {"end", 2.0}, {"step", 3.0}}),
		"array",
		Numbers({2, 2, 2})
	);
	CHECK_FALSE(RunNode("pc.array_range", {}, {{"step", 0.0}}).Ok);
	CHECK(RunNode("pc.array_range", {}, {{"end", 1e100}}).Code == Status::LimitExceeded);
}

TEST_CASE("Array Cumulative includes or excludes the selected input", "[imagegraph][array_numeric]") {
	CheckNumbers(
		RunNode("pc.array_cumulative", {}, {{"array_in", Numbers({1, 2, -4})}, {"start", 10.0}}),
		"cumulative_array",
		Numbers({11, 13, 9})
	);
	CheckNumbers(
		RunNode(
			"pc.array_cumulative",
			{},
			{{"array_in", Numbers({1, 2, -4})}, {"start", 10.0}, {"include_current", false}}
		),
		"cumulative_array",
		Numbers({10, 11, 13})
	);
}

TEST_CASE("Array Sort keeps source positions and exact large integers", "[imagegraph][array_numeric]") {
	const ArrayValue input{
		ValueType::Integer, {int64_t{9007199254740993}, int64_t{2}, int64_t{9007199254740992}}
	};
	const auto run = RunNode("pc.array_sort", {}, {{"array_in", input}});
	INFO(run.Message);
	REQUIRE(run.Ok);
	const auto &values = std::get<ArrayValue>(*run.OutputValue("sorted_array"));
	CHECK(
		values.Elements ==
		std::vector<ElementValue>{int64_t{2}, int64_t{9007199254740992}, int64_t{9007199254740993}}
	);
	const auto &indices = std::get<ArrayValue>(*run.OutputValue("sorted_index"));
	CHECK(indices.Elements == std::vector<ElementValue>{int64_t{1}, int64_t{2}, int64_t{0}});
	CheckNumbers(
		RunNode("pc.array_sort", {}, {{"array_in", Numbers({3, 1, 2})}, {"order", EnumValue{1}}}),
		"sorted_array",
		Numbers({3, 2, 1})
	);
}

TEST_CASE("Array Convolute matches centered zero wrap and valid windows", "[imagegraph][array_numeric]") {
	const auto input = Numbers({1, 2, 3}), kernel = Numbers({1, 2, 1});
	CheckNumbers(
		RunNode("pc.array_convolute", {}, {{"array", input}, {"kernel", kernel}}), "array", Numbers({4, 8, 8})
	);
	CheckNumbers(
		RunNode("pc.array_convolute", {}, {{"array", input}, {"kernel", kernel}, {"boundary", EnumValue{1}}}),
		"array",
		Numbers({7, 8, 9})
	);
	CheckNumbers(
		RunNode("pc.array_convolute", {}, {{"array", input}, {"kernel", kernel}, {"boundary", EnumValue{2}}}),
		"array",
		Numbers({8})
	);
	ArrayValue rows{ValueType::Scalar, {}};
	rows.Nested = {input.Elements, Numbers({4, 5}).Elements};
	const auto run = RunNode("pc.array_convolute", {}, {{"array", rows}, {"kernel", Numbers({2})}});
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(
		std::get<ArrayValue>(*run.OutputValue("array")).Nested ==
		std::vector<std::vector<ElementValue>>{Numbers({2, 4, 6}).Elements, Numbers({8, 10}).Elements}
	);
}

TEST_CASE("Array Sample preserves step shift wrapping and nested dimension", "[imagegraph][array_numeric]") {
	CheckNumbers(
		RunNode("pc.array_sample", {}, {{"array", Numbers({0, 1, 2, 3, 4, 5})}}), "array", Numbers({0, 2, 4})
	);
	CHECK_FALSE(
		RunNode("pc.array_sample", {}, {{"array", Numbers({0, 1, 2, 3, 4, 5})}, {"shift", int64_t{-1}}}).Ok
	);
	CheckNumbers(
		RunNode(
			"pc.array_sample",
			{},
			{{"array", Numbers({0, 1, 2})}, {"amount_type", EnumValue{1}}, {"amount", int64_t{5}}}
		),
		"array",
		Numbers({0, 2, 1, 0, 2})
	);
	ArrayValue rows{ValueType::Scalar, {}};
	rows.Nested = {Numbers({1, 2, 3}).Elements, Numbers({4, 5, 6, 7}).Elements};
	const auto run = RunNode("pc.array_sample", {}, {{"array", rows}, {"dimension", int64_t{1}}});
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(
		std::get<ArrayValue>(*run.OutputValue("array")).Nested ==
		std::vector<std::vector<ElementValue>>{Numbers({1, 3}).Elements, Numbers({4, 6}).Elements}
	);
	CHECK_FALSE(RunNode("pc.array_sample", {}, {{"array", Numbers({1})}, {"step", 0.0}}).Ok);
}

TEST_CASE(
	"Array Boolean Opr preserves multiset subtraction and unique set membership",
	"[imagegraph][array_numeric]"
) {
	const auto a = Numbers({1, 1, 2, 3}), b = Numbers({1, 2, 2, 4});
	CheckNumbers(
		RunNode("pc.array_boolean_opr", {}, {{"array_1", a}, {"array_2", b}}),
		"array_out",
		Numbers({1, 2, 3, 4})
	);
	CheckNumbers(
		RunNode("pc.array_boolean_opr", {}, {{"array_1", a}, {"array_2", b}, {"operation", EnumValue{1}}}),
		"array_out",
		Numbers({1, 3})
	);
	CheckNumbers(
		RunNode("pc.array_boolean_opr", {}, {{"array_1", a}, {"array_2", b}, {"operation", EnumValue{2}}}),
		"array_out",
		Numbers({1, 2})
	);
	CheckNumbers(
		RunNode("pc.array_boolean_opr", {}, {{"array_1", a}, {"array_2", b}, {"operation", EnumValue{3}}}),
		"array_out",
		Numbers({1, 3, 2, 4})
	);
}

TEST_CASE("Array Pin includes connected inputs and retains whole rows", "[imagegraph][array_numeric]") {
	const auto *entry = FindCatalogueEntry("pc.array_pin");
	const auto executor = detail::FindExecutor("pc.array_pin");
	REQUIRE(entry);
	REQUIRE(executor);
	Node node{"pin", "pc.array_pin", "", {}, {}};
	node.DynamicInputs = {
		{"first", ValueType::Any, {}}, {"unlinked", ValueType::Any, {}}, {"last", ValueType::Any, {}}
	};
	const EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.Values = {{"first", Numbers({1, 2})}, {"unlinked", 99.0}, {"last", Numbers({3, 4})}};
	context.LinkedValues = {"first", "last"};
	REQUIRE(executor(context));
	REQUIRE(context.OutputValues.size() == 1);
	CHECK(
		std::get<ArrayValue>(context.OutputValues.front().Data).Nested ==
		std::vector<std::vector<ElementValue>>{Numbers({1, 2}).Elements, Numbers({3, 4}).Elements}
	);
}

TEST_CASE(
	"Array Composite retains source outer products and nested row depth", "[imagegraph][array_numeric]"
) {
	const auto run =
		RunNode("pc.array_composite", {}, {{"array", Numbers({2, 3})}, {"compose", Numbers({4, 5})}});
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(
		std::get<ArrayValue>(*run.OutputValue("array")).Nested ==
		std::vector<std::vector<ElementValue>>{Numbers({8, 10}).Elements, Numbers({12, 15}).Elements}
	);
	ArrayValue rows{ValueType::Scalar, {}};
	rows.Nested = {Numbers({2}).Elements, Numbers({3}).Elements};
	const auto nested = RunNode("pc.array_composite", {}, {{"array", rows}, {"compose", Numbers({4, 5})}});
	INFO(nested.Message);
	REQUIRE(nested.Ok);
	const auto &items = std::get<ArrayValue>(*nested.OutputValue("array")).Items;
	REQUIRE(items.size() == 2);
	const auto &firstRow = std::get<std::vector<SourceArrayItem>>(items[0].Data);
	REQUIRE(firstRow.size() == 1);
	const auto &products = std::get<std::vector<SourceArrayItem>>(firstRow[0].Data);
	REQUIRE(products.size() == 2);
	CHECK(std::get<double>(std::get<ElementValue>(products[0].Data)) == 8);
	CHECK(std::get<double>(std::get<ElementValue>(products[1].Data)) == 10);
}

TEST_CASE("Array Sample seeded random mode resets at each sampled row", "[imagegraph][array_numeric]") {
	const auto input = Numbers({0, 1, 2, 3, 4, 5});
	const std::initializer_list<std::pair<std::string_view, Value>> controls{
		{"array", input},
		{"mode", EnumValue{1}},
		{"seed", int64_t{12345}},
		{"amount_type", EnumValue{1}},
		{"amount", int64_t{8}}
	};
	CheckNumbers(RunNode("pc.array_sample", {}, controls), "array", Numbers({4, 0, 5, 1, 2, 0, 3, 0}));
	ArrayValue rows{ValueType::Scalar, {}};
	rows.Nested = {input.Elements, input.Elements};
	const auto run = RunNode(
		"pc.array_sample",
		{},
		{{"array", rows}, {"dimension", int64_t{1}}, {"mode", EnumValue{1}}, {"seed", int64_t{12345}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	const auto &output = std::get<ArrayValue>(*run.OutputValue("array"));
	REQUIRE(output.Nested.size() == 2);
	CHECK(output.Nested[0] == Numbers({4, 0, 5, 1, 2, 0}).Elements);
	CHECK(output.Nested[0] == output.Nested[1]);
}

TEST_CASE(
	"Array Get random mode ignores index arrays and selects one seeded value", "[imagegraph][array_numeric]"
) {
	const auto run = RunNode(
		"pc.array_get",
		{},
		{{"array", Numbers({0, 1, 2, 3, 4, 5})},
		 {"index", Numbers({1, 2})},
		 {"mode", EnumValue{1}},
		 {"seed", int64_t{12345}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	REQUIRE(run.OutputValue("value"));
	CHECK(std::get<double>(*run.OutputValue("value")) == 4);
}

TEST_CASE(
	"Native seeded Shuffle preserves rows and repeats its deterministic permutation",
	"[imagegraph][array_numeric]"
) {
	const auto input = Numbers({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11});
	const auto first = RunNode("pc.array_shuffle", {}, {{"array_in", input}, {"seed", int64_t{12345}}});
	const auto repeat = RunNode("pc.array_shuffle", {}, {{"array_in", input}, {"seed", int64_t{12345}}});
	const auto other = RunNode("pc.array_shuffle", {}, {{"array_in", input}, {"seed", int64_t{12346}}});
	REQUIRE(first.Ok);
	REQUIRE(repeat.Ok);
	REQUIRE(other.Ok);
	auto values = std::get<ArrayValue>(*first.OutputValue("shuffled_array")).Elements;
	CHECK(values == std::get<ArrayValue>(*repeat.OutputValue("shuffled_array")).Elements);
	CHECK(values != std::get<ArrayValue>(*other.OutputValue("shuffled_array")).Elements);
	std::sort(values.begin(), values.end(), [](const auto &a, const auto &b) {
		return std::get<double>(a) < std::get<double>(b);
	});
	CHECK(values == input.Elements);
	ArrayValue rows{ValueType::Scalar, {}};
	rows.Nested = {{0.0, 1.0}, {2.0, 3.0}, {4.0, 5.0}};
	const auto nested = RunNode("pc.array_shuffle", {}, {{"array_in", rows}, {"seed", int64_t{1}}});
	REQUIRE(nested.Ok);
	auto shuffled = std::get<ArrayValue>(*nested.OutputValue("shuffled_array")).Nested;
	REQUIRE(shuffled.size() == 3);
	std::sort(shuffled.begin(), shuffled.end(), [](const auto &a, const auto &b) {
		return std::get<double>(a[0]) < std::get<double>(b[0]);
	});
	CHECK(shuffled == rows.Nested);
}

TEST_CASE(
	"Numeric array nodes preserve general mixed numeric leaves and source nonnumeric skip",
	"[imagegraph][array_numeric]"
) {
	ArrayValue input{ValueType::Any, {}};
	input.Items = {{ElementValue{int64_t{2}}}, {ElementValue{1.0}}, {ElementValue{int64_t{3}}}};
	const auto sort = RunNode("pc.array_sort", {}, {{"array_in", input}});
	INFO(sort.Message);
	REQUIRE(sort.Ok);
	const auto &sorted = std::get<ArrayValue>(*sort.OutputValue("sorted_array"));
	REQUIRE(sorted.Items.size() == 3);
	CHECK(std::get<double>(std::get<ElementValue>(sorted.Items[0].Data)) == 1);
	CHECK(std::get<int64_t>(std::get<ElementValue>(sorted.Items[1].Data)) == 2);
	CheckNumbers(
		RunNode("pc.array_convolute", {}, {{"array", input}, {"kernel", Numbers({1})}}),
		"array",
		Numbers({2, 1, 3})
	);
	input.Items = {
		{ElementValue{1.0}},
		{std::vector<SourceArrayItem>{{ElementValue{4.0}}}},
		{ElementValue{std::string{"text"}}},
		{ElementValue{2.0}}
	};
	CheckNumbers(
		RunNode("pc.array_cumulative", {}, {{"array_in", input}}), "cumulative_array", Numbers({1, 1, 1, 3})
	);
}
