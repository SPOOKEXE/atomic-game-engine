#include "../src/ProcessorBatch.hpp"
#include "../src/ValuePayload.hpp"
#include "../src/ValueText.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.imagegraph.node_array_structure")
using namespace engine::imagegraph;
using namespace imagegraph_test;
namespace {
	SourceArrayItem Leaf(double number) {
		return {ElementValue{number}};
	}
	SourceArrayItem Text(std::string text) {
		return {ElementValue{std::move(text)}};
	}
	SourceArrayItem Row(std::initializer_list<SourceArrayItem> items) {
		return {std::vector<SourceArrayItem>{items}};
	}
	ArrayValue General(std::initializer_list<SourceArrayItem> items) {
		ArrayValue array{ValueType::Any, {}};
		array.Items = items;
		return array;
	}
	const ArrayValue &Result(const NodeRun &run, std::string_view port = "array") {
		INFO(run.Message);
		REQUIRE(run.Ok);
		const Value *value = run.OutputValue(port);
		REQUIRE(value);
		REQUIRE(std::holds_alternative<ArrayValue>(*value));
		return std::get<ArrayValue>(*value);
	}
} // namespace
TEST_CASE(
	"Structural arrays preserve mixed recursive shape and input ownership", "[imagegraph][array_structure]"
) {
	const ArrayValue input = General({Text("red"), Row({Leaf(4), Row({Text("deep"), Leaf(8)})}), Leaf(12)});
	const auto reversed = RunNode("pc.array_reverse", {}, {{"array", input}});
	CHECK(Result(reversed).Items == std::vector<SourceArrayItem>{Leaf(12), input.Items[1], Text("red")});
	const auto copied = RunNode(
		"pc.array_copy", {}, {{"array", input}, {"starting_index", int64_t{-1}}, {"size", int64_t{3}}}
	);
	CHECK(Result(copied).Elements == std::vector<ElementValue>{12.0, 0.0, 0.0});
	const auto trimmed = RunNode(
		"pc.array_trim", {}, {{"array", input}, {"trim_start", int64_t{1}}, {"trim_end", int64_t{1}}}
	);
	CHECK(Result(trimmed).Items == std::vector<SourceArrayItem>{input.Items[1]});
	CHECK(input == General({Text("red"), Row({Leaf(4), Row({Text("deep"), Leaf(8)})}), Leaf(12)}));
	const auto length = RunNode("pc.array_length", {}, {{"array", input}});
	REQUIRE(length.Ok);
	CHECK(std::get<int64_t>(*length.OutputValue("size")) == 3);
}
TEST_CASE(
	"Copy and trim follow negative source controls without signed overflow", "[imagegraph][array_structure]"
) {
	const ArrayValue input{ValueType::Scalar, {1.0, 2.0, 3.0}};
	const auto copied = RunNode(
		"pc.array_copy", {}, {{"array", input}, {"starting_index", int64_t{-1}}, {"size", int64_t{-1}}}
	);
	CHECK(Result(copied).Elements == std::vector<ElementValue>{3.0, 0.0});
	const auto extreme = RunNode(
		"pc.array_copy",
		{},
		{{"array", input}, {"starting_index", std::numeric_limits<int64_t>::max()}, {"size", int64_t{2}}}
	);
	CHECK(Result(extreme).Elements == std::vector<ElementValue>{0.0, 0.0});
	const auto negative = RunNode(
		"pc.array_trim", {}, {{"array", input}, {"trim_start", int64_t{-3}}, {"trim_end", int64_t{-3}}}
	);
	CHECK(Result(negative) == input);
	const auto empty = RunNode("pc.array_trim", {}, {{"array", input}, {"trim_start", int64_t{9}}});
	CHECK(Result(empty).Elements.empty());
}
TEST_CASE(
	"Shift wrap zero and ignore preserve order and source numeric padding", "[imagegraph][array_structure]"
) {
	const ArrayValue input{ValueType::Text, {std::string{"a"}, std::string{"b"}, std::string{"c"}}};
	const auto wrap =
		RunNode("pc.array_shift", {}, {{"array", input}, {"shift", int64_t{4}}, {"overflow", EnumValue{0}}});
	CHECK(
		Result(wrap).Elements ==
		std::vector<ElementValue>{std::string{"c"}, std::string{"a"}, std::string{"b"}}
	);
	const auto zero =
		RunNode("pc.array_shift", {}, {{"array", input}, {"shift", int64_t{-1}}, {"overflow", EnumValue{1}}});
	CHECK(Result(zero).Items == std::vector<SourceArrayItem>{Text("b"), Text("c"), Leaf(0)});
	const auto ignore =
		RunNode("pc.array_shift", {}, {{"array", input}, {"shift", int64_t{1}}, {"overflow", EnumValue{2}}});
	CHECK(Result(ignore).Elements == std::vector<ElementValue>{std::string{"a"}, std::string{"b"}});
	const auto extreme = RunNode(
		"pc.array_shift",
		{},
		{{"array", input}, {"shift", std::numeric_limits<int64_t>::min()}, {"overflow", EnumValue{2}}}
	);
	CHECK(Result(extreme).Elements.empty());
}
TEST_CASE(
	"Partition uses accumulated fractional source boundaries and "
	"preserves deeper children",
	"[imagegraph][array_structure]"
) {
	const ArrayValue input{ValueType::Scalar, {1.0, 2.0, 3.0, 4.0, 5.0}};
	const auto length =
		RunNode("pc.array_partition", {}, {{"array", input}, {"type", EnumValue{0}}, {"length", int64_t{2}}});
	CHECK(Result(length).Nested == std::vector<std::vector<ElementValue>>{{1.0, 2.0}, {3.0, 4.0}, {5.0}});
	const auto amount =
		RunNode("pc.array_partition", {}, {{"array", input}, {"type", EnumValue{1}}, {"length", int64_t{3}}});
	CHECK(Result(amount).Nested == std::vector<std::vector<ElementValue>>{{1.0}, {2.0, 3.0}, {4.0, 5.0}});
	const auto many =
		RunNode("pc.array_partition", {}, {{"array", input}, {"type", EnumValue{1}}, {"length", int64_t{7}}});
	CHECK(Result(many).Nested.size() == 7);
	const ArrayValue deep = General({Row({Row({Leaf(1)}), Leaf(2)}), Row({Leaf(3)})});
	const auto nested = RunNode("pc.array_partition", {}, {{"array", deep}, {"length", int64_t{1}}});
	CHECK(Result(nested).Items == std::vector<SourceArrayItem>{Row({deep.Items[0]}), Row({deep.Items[1]})});
}
TEST_CASE(
	"Flatten matches first child depth and raw packed tuple source shape", "[imagegraph][array_structure]"
) {
	const ArrayValue input = General({Row({Leaf(1), Row({Leaf(2)})}), Leaf(3), Row({})});
	const auto all = RunNode("pc.array_flattern", {}, {{"array_in", input}});
	CHECK(Result(all, "flattened_array").Elements == std::vector<ElementValue>{1.0, 2.0, 3.0});
	const auto preserve = RunNode("pc.array_flattern", {}, {{"array_in", input}, {"depth", int64_t{1}}});
	CHECK(
		Result(preserve, "flattened_array").Items ==
		std::vector<SourceArrayItem>{input.Items[0], input.Items[2]}
	);
	const auto beyond = RunNode("pc.array_flattern", {}, {{"array_in", input}, {"depth", int64_t{9}}});
	CHECK(Result(beyond, "flattened_array").Elements.empty());
	const ArrayValue tuples{ValueType::Vector2, {Vector2{4, 7}, Vector2{2, 9}}};
	const auto packed = RunNode("pc.array_flattern", {}, {{"array_in", tuples}});
	CHECK(Result(packed, "flattened_array").Elements == std::vector<ElementValue>{4.0, 7.0, 2.0, 9.0});
}
TEST_CASE(
	"Transpose clips ragged rows while retaining mixed recursive children", "[imagegraph][array_structure]"
) {
	const ArrayValue ragged{ValueType::Scalar, {}, {{1.0, 2.0, 3.0}, {4.0, 5.0}}};
	const auto result = RunNode("pc.array_transpose", {}, {{"array_in", ragged}});
	CHECK(
		Result(result, "transposed_array").Nested ==
		std::vector<std::vector<ElementValue>>{{1.0, 4.0}, {2.0, 5.0}}
	);
	const ArrayValue deep = General({Row({Row({Leaf(1)}), Text("a")}), Row({Row({Leaf(2)}), Text("b")})});
	const auto nested = RunNode("pc.array_transpose", {}, {{"array_in", deep}});
	CHECK(
		Result(nested, "transposed_array").Items ==
		std::vector<SourceArrayItem>{Row({Row({Leaf(1)}), Row({Leaf(2)})}), Row({Text("a"), Text("b")})}
	);
	const auto invalid =
		RunNode("pc.array_transpose", {}, {{"array_in", ArrayValue{ValueType::Scalar, {1.0}}}});
	CHECK_FALSE(invalid.Ok);
	CHECK(invalid.Code == Status::InvalidValue);
}
TEST_CASE(
	"Recursive authored codec round trips mixed arrays and rejects older "
	"versions",
	"[imagegraph][array_structure][document]"
) {
	Value value;
	REQUIRE(detail::ReadValueText("a any 3 s \"a\" a any 2 d 4 a any 1 d 8 d 0", value));
	CHECK(std::get<ArrayValue>(value) == General({Text("a"), Row({Leaf(4), Row({Leaf(8)})}), Leaf(0)}));
	CHECK(detail::ValidValuePayload(value, false));
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {{"array", "value.array", "", {}, {}, {{"source", ValueType::Array, value}}}};
	const std::string encoded = Write(document);
	Document parsed;
	Diagnostic diagnostic;
	REQUIRE(Read(encoded, parsed, diagnostic) == Status::Ok);
	CHECK(*parsed.Nodes.front().DynamicInputs.front().Default == value);
	std::string old = encoded;
	old.replace(0, std::string{"imagegraph 9"}.size(), "imagegraph 8");
	CHECK(Read(old, parsed, diagnostic) != Status::Ok);
	CHECK_FALSE(detail::ReadValueText("a any 4097", value));
}
TEST_CASE(
	"Structural allocations are admitted while earlier output remains live",
	"[imagegraph][array_structure][evaluation_budget]"
) {
	const ArrayValue input =
		General({Text(std::string(1000, 'a')), Row({Text(std::string(500, 'b')), Leaf(2)})});
	const Value value = input;
	Node node{"node", "pc.array_reverse", "", {}, {}};
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	const auto executor = detail::FindExecutor(node.Type);
	REQUIRE(executor);
	const EvaluationRequest request;
	uint64_t peak = 0;
	for (int attempt = 0; attempt < 3; ++attempt) {
		const uint64_t limit = attempt == 0 ? Limits::MaximumEvaluationBytes : attempt == 1 ? peak - 1 : peak;
		detail::EvaluationBudget budget(limit);
		auto previous = budget.Reserve(512);
		REQUIRE(previous);
		{
			detail::NodeContext context(node, *entry, request, budget);
			context.ByteBudget = limit;
			context.ValueViews = {{"array", &value}};
			const bool ok = executor(context);
			if (attempt == 1) {
				CHECK_FALSE(ok);
				CHECK(context.FailureCode == Status::LimitExceeded);
				CHECK(context.OutputValues.empty());
			} else {
				REQUIRE(ok);
				if (attempt == 0) peak = budget.Peak();
				CHECK(budget.Peak() == peak);
			}
			CHECK(budget.Peak() <= limit);
		}
		CHECK(budget.Used() == 512);
		CHECK(std::get<ArrayValue>(value) == input);
	}
}

TEST_CASE("Recursive image arrays retain pixels and mixed numeric padding", "[imagegraph][array_structure]") {
	const Image first{1, 1, {1, 2, 3, 255}, 17};
	const Image second{1, 1, {4, 5, 6, 255}, 19};
	const ImageArray input{{first, second}, {{size_t{1}}, {std::vector<ImageArrayItem>{{size_t{0}}}}}};
	const auto execute = [&](std::string_view type, auto configure, auto check) {
		Node node{"node", std::string(type), "", {}, {}};
		const auto *entry = FindCatalogueEntry(type);
		REQUIRE(entry);
		const auto executor = detail::FindExecutor(type);
		REQUIRE(executor);
		const EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		context.ImageArrays = {{"array", &input}};
		configure(context);
		REQUIRE(executor(context));
		CHECK(context.FailureCode == Status::Ok);
		check(context);
	};
	execute(
		"pc.array_reverse",
		[](auto &) {},
		[&](const auto &context) {
			REQUIRE(context.OutputImageArrays.size() == 1);
			const auto &result = context.OutputImageArrays.front().second;
			REQUIRE(result.Images.size() == 2);
			CHECK(result.Images[0].Pixels == first.Pixels);
			CHECK(result.Images[0].Hash == 17);
			CHECK(result.Images[1].Pixels == second.Pixels);
			CHECK(result.Images[1].Hash == 19);
			CHECK(result.Images[0].Pixels.data() != first.Pixels.data());
			CHECK(
				result.Items ==
				std::vector<ImageArrayItem>{{std::vector<ImageArrayItem>{{size_t{0}}}}, {size_t{1}}}
			);
		}
	);
	execute(
		"pc.array_copy",
		[](auto &context) { context.Values = {{"starting_index", int64_t{0}}, {"size", int64_t{3}}}; },
		[&](const auto &context) {
			REQUIRE(context.OutputValues.size() == 1);
			const auto &result = std::get<ArrayValue>(context.OutputValues.front().Data);
			REQUIRE(result.Items.size() == 3);
			CHECK(std::get<Image>(result.Items[0].Data).Pixels == second.Pixels);
			const auto &row = std::get<std::vector<SourceArrayItem>>(result.Items[1].Data);
			CHECK(std::get<Image>(row.front().Data).Pixels == first.Pixels);
			CHECK(result.Items.back() == Leaf(0));
		}
	);
	CHECK(input.Images[0].Pixels == first.Pixels);
	CHECK(input.Images[1].Pixels == second.Pixels);
}

TEST_CASE("Source structural shape bounds reject invalid recursive inputs", "[imagegraph][array_structure]") {
	ArrayValue excessive{ValueType::Any, {}};
	excessive.Items.resize(Limits::MaximumArrayElements + 1, Leaf(2));
	const auto count = RunNode("pc.array_reverse", {}, {{"array", excessive}});
	CHECK_FALSE(count.Ok);
	CHECK(count.Code == Status::InvalidValue);
	SourceArrayItem deep = Leaf(2);
	for (size_t index = 0; index < Limits::MaximumArrayDepth + 1; ++index)
		deep = Row({deep});
	const auto depth = RunNode("pc.array_reverse", {}, {{"array", General({deep})}});
	CHECK_FALSE(depth.Ok);
	CHECK(depth.Code == Status::InvalidValue);
	CHECK_FALSE(
		detail::ValidValuePayload(Value{General({SourceArrayItem{Image{1, 1, {1, 2, 3, 4}, 0}}})}, false)
	);
}

TEST_CASE(
	"Array Shift processor controls retain complete recursive output rows",
	"[imagegraph][array_structure][processor]"
) {
	const ArrayValue input = General({Text("a"), Row({Leaf(1)}), Leaf(2)});
	Node node{"node", "pc.array_shift", "", {}, {}};
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	const auto *inputMetadata = FindCatalogueInput(*entry, "array");
	REQUIRE(inputMetadata);
	CHECK(inputMetadata->ArrayDepthKnown);
	CHECK(inputMetadata->ArrayDepth == 99);
	const auto executor = detail::FindExecutor(node.Type);
	REQUIRE(executor);
	const EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.Values = {
		{"array", input},
		{"shift", ArrayValue{ValueType::Integer, {int64_t{0}, int64_t{1}}}},
		{"overflow", EnumValue{1}}
	};
	INFO(context.FailureMessage);
	REQUIRE(detail::RunProcessorBatch(context, executor));
	REQUIRE(context.OutputValues.size() == 1);
	const auto &output = std::get<ArrayValue>(context.OutputValues.front().Data);
	CHECK(
		output.Items ==
		std::vector<SourceArrayItem>{
			Row({Text("a"), Row({Leaf(1)}), Leaf(2)}), Row({Leaf(0), Text("a"), Row({Leaf(1)})})
		}
	);
}

TEST_CASE("Flatten stops first-child depth at empty source arrays", "[imagegraph][array_structure]") {
	const ArrayValue input = General({Row({})});
	const auto run = RunNode("pc.array_flattern", {}, {{"array_in", input}, {"depth", int64_t{1}}});
	CHECK(Result(run, "flattened_array").Items == std::vector<SourceArrayItem>{Row({Row({})})});
}
TEST_CASE("Structural executors run upstream of a snapshot target", "[imagegraph][array_structure]") {
	struct Fixture {
		std::string Type, Input, Output;
		std::vector<AuthoredValue> Controls;
		Value Expected;
		ArrayValue Source;
	};
	const ArrayValue flat{ValueType::Scalar, {1.0, 2.0, 3.0}};
	const ArrayValue reversed{ValueType::Scalar, {3.0, 2.0, 1.0}};
	const ArrayValue rows{ValueType::Scalar, {}, {{1.0, 2.0}, {3.0, 4.0}}};
	const Fixture fixtures[]{
		{"pc.array_reverse", "array", "array", {}, reversed, flat},
		{"pc.array_copy",
		 "array",
		 "array",
		 {{"starting_index", int64_t{1}}, {"size", int64_t{2}}},
		 ArrayValue{ValueType::Scalar, {2.0, 3.0}},
		 flat},
		{"pc.array_trim",
		 "array",
		 "array",
		 {{"trim_start", int64_t{1}}, {"trim_end", int64_t{1}}},
		 ArrayValue{ValueType::Scalar, {2.0}},
		 flat},
		{"pc.array_shift",
		 "array",
		 "array",
		 {{"shift", int64_t{1}}, {"overflow", EnumValue{0}}},
		 ArrayValue{ValueType::Scalar, {3.0, 1.0, 2.0}},
		 flat},
		{"pc.array_partition",
		 "array",
		 "array",
		 {{"type", EnumValue{1}}, {"length", int64_t{2}}},
		 ArrayValue{ValueType::Scalar, {}, {{1.0}, {2.0, 3.0}}},
		 flat},
		{"pc.array_flattern",
		 "array_in",
		 "flattened_array",
		 {{"depth", int64_t{0}}},
		 ArrayValue{ValueType::Scalar, {1.0, 2.0, 3.0, 4.0}},
		 rows},
		{"pc.array_transpose",
		 "array_in",
		 "transposed_array",
		 {},
		 ArrayValue{ValueType::Scalar, {}, {{1.0, 3.0}, {2.0, 4.0}}},
		 rows},
		{"pc.array_length", "array", "size", {}, int64_t{3}, flat},
	};
	for (const auto &fixture : fixtures) {
		INFO(fixture.Type);
		Document document;
		document.FormatVersion = 9;
		Node operation{"operation", fixture.Type, "", {}, fixture.Controls};
		ArrayValue sourceValue = fixture.Source;
		if (!sourceValue.Nested.empty()) {
			sourceValue.ElementType = ValueType::Any;
			for (const auto &row : sourceValue.Nested) {
				std::vector<SourceArrayItem> members;
				for (const auto &leaf : row)
					members.push_back({leaf});
				sourceValue.Items.push_back({std::move(members)});
			}
			sourceValue.Nested.clear();
		}
		Node source{
			"source",
			"pc.array",
			"",
			{},
			{{"type", EnumValue{0}}, {"spread_array", true}},
			{{"value_0", ValueType::Array, Value{sourceValue}}}
		};
		document.Nodes = {source, operation, Node{"capture", "pc.array_add", "", {}, {}}};
		document.Links = {
			{"source", "array", "operation", fixture.Input}, {"operation", fixture.Output, "capture", "array"}
		};
		document.Outputs = {{"out", "capture", "output"}};
		Plan plan;
		Diagnostic diagnostic;
		const Status compiled = Compile(document, plan, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << " " << diagnostic.Message);
		REQUIRE(compiled == Status::Ok);
		EvaluationSnapshot snapshot;
		REQUIRE(EvaluateNodeInputs(document, plan, "capture", {}, snapshot, diagnostic) == Status::Ok);
		bool found = false;
		for (const auto &entry : snapshot.Values())
			if (entry.Port == "array") {
				CHECK(entry.Data == fixture.Expected);
				found = true;
			}
		CHECK(found);
	}
}

TEST_CASE(
	"Valid source inputs reject oversized output shapes before construction", "[imagegraph][array_structure]"
) {
	const auto rejected = [](const NodeRun &run) {
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::LimitExceeded);
		CHECK(run.Message == "source array output exceeds bounds");
		CHECK(run.Values.empty());
		CHECK(run.Images.empty());
	};
	ArrayValue flat{ValueType::Scalar, {}};
	flat.Elements.assign(Limits::MaximumArrayElements, 1.0);
	REQUIRE(detail::ValidRuntimeValue(Value{flat}));
	rejected(RunNode(
		"pc.array_partition", {}, {{"array", flat}, {"type", EnumValue{1}}, {"length", int64_t{4096}}}
	));

	ArrayValue rows{ValueType::Scalar, {}};
	rows.Nested.assign(2, std::vector<ElementValue>(2047, 1.0));
	REQUIRE(detail::ValidRuntimeValue(Value{rows}));
	rejected(RunNode("pc.array_transpose", {}, {{"array_in", rows}}));

	ArrayValue largeSubtree{ValueType::Any, {}};
	std::vector<SourceArrayItem> leaves(4095, Leaf(1));
	largeSubtree.Items.push_back({std::move(leaves)});
	REQUIRE(detail::ValidRuntimeValue(Value{largeSubtree}));
	rejected(RunNode("pc.array_copy", {}, {{"array", largeSubtree}, {"size", int64_t{4096}}}));
	rejected(RunNode("pc.array_flattern", {}, {{"array_in", largeSubtree}, {"depth", int64_t{2}}}));

	SourceArrayItem branch = Leaf(1);
	for (size_t level = 1; level < Limits::MaximumArrayDepth; ++level)
		branch = {std::vector<SourceArrayItem>{std::move(branch)}};
	ArrayValue deep{ValueType::Any, {}};
	deep.Items.push_back(std::move(branch));
	REQUIRE(detail::ValidRuntimeValue(Value{deep}));
	rejected(RunNode("pc.array_flattern", {}, {{"array_in", deep}, {"depth", int64_t{64}}}));

	ArrayValue surfaces{ValueType::Any, {}};
	const Image pixel{1, 1, {1, 2, 3, 255}, 17};
	surfaces.Items.assign(4096, SourceArrayItem{pixel});
	REQUIRE(detail::ValidRuntimeValue(Value{surfaces}));
	rejected(RunNode(
		"pc.array_partition", {}, {{"array", surfaces}, {"type", EnumValue{1}}, {"length", int64_t{4096}}}
	));

	ArrayValue packed{ValueType::Vector2, {}};
	packed.Elements.assign(1365, Vector2{1, 2});
	REQUIRE(detail::ValidRuntimeValue(Value{packed}));
	rejected(RunNode(
		"pc.array_partition", {}, {{"array", packed}, {"type", EnumValue{1}}, {"length", int64_t{1365}}}
	));
}

TEST_CASE(
	"Trim and shift admit exact small outputs from valid large legacy payloads",
	"[imagegraph][array_structure]"
) {
	const size_t count = Limits::MaximumArrayElements;
	const size_t textBytes = (Limits::MaximumArrayBytes - count * sizeof(ElementValue)) / count - 1;
	ArrayValue input{ValueType::Text, {}};
	input.Elements.assign(count, std::string(textBytes, 'x'));
	REQUIRE(detail::ValidRuntimeValue(Value{input}));
	REQUIRE(count * (sizeof(SourceArrayItem) + textBytes) > Limits::MaximumArrayBytes);
	const auto empty = RunNode("pc.array_trim", {}, {{"array", input}, {"trim_start", int64_t(count)}});
	CHECK(Result(empty).Elements.empty());
	const auto one = RunNode("pc.array_trim", {}, {{"array", input}, {"trim_start", int64_t(count - 1)}});
	CHECK(Result(one).Elements == std::vector<ElementValue>{std::string(textBytes, 'x')});
	const auto ignored = RunNode(
		"pc.array_shift", {}, {{"array", input}, {"shift", int64_t(count)}, {"overflow", EnumValue{2}}}
	);
	CHECK(Result(ignored).Elements.empty());
	const auto zeroed = RunNode(
		"pc.array_shift", {}, {{"array", input}, {"shift", int64_t(count)}, {"overflow", EnumValue{1}}}
	);
	CHECK(Result(zeroed).Elements == std::vector<ElementValue>(count, 0.0));
	CHECK(input.Elements.size() == count);
	CHECK(std::get<std::string>(input.Elements.front()).size() == textBytes);
}

TEST_CASE(
	"Transpose reserves columns while preserving recursive rows and empty policy",
	"[imagegraph][array_structure]"
) {
	ArrayValue input{ValueType::Any, {}};
	std::vector<SourceArrayItem> firstColumn, secondColumn;
	for (size_t index = 0; index < 64; ++index) {
		firstColumn.push_back(Leaf(double(index)));
		secondColumn.push_back(Text("row-" + std::to_string(index)));
		std::vector<SourceArrayItem> row{firstColumn.back(), secondColumn.back()};
		if (index % 2) row.push_back(Row({Leaf(1000)}));
		input.Items.push_back({std::move(row)});
	}
	const auto before = input;
	const auto result = RunNode("pc.array_transpose", {}, {{"array_in", input}});
	const auto &output = Result(result, "transposed_array");
	CHECK(
		output.Items ==
		std::vector<SourceArrayItem>{SourceArrayItem{firstColumn}, SourceArrayItem{secondColumn}}
	);
	CHECK(input == before);
	const auto empty = RunNode("pc.array_transpose", {}, {{"array_in", ArrayValue{ValueType::Scalar, {}}}});
	CHECK(Result(empty, "transposed_array").Elements.empty());
	const auto emptyRow =
		RunNode("pc.array_transpose", {}, {{"array_in", General({Row({Leaf(1)}), Row({})})}});
	CHECK(Result(emptyRow, "transposed_array").Elements.empty());
}

TEST_CASE(
	"Transpose item count boundary remains independent of reserve size", "[imagegraph][array_structure]"
) {
	ArrayValue input{
		ValueType::Scalar, {}, std::vector<std::vector<ElementValue>>(2, std::vector<ElementValue>(1365, 1.0))
	};
	REQUIRE(detail::ValidRuntimeValue(Value{input}));
	const auto result = RunNode("pc.array_transpose", {}, {{"array_in", input}});
	CHECK(
		Result(result, "transposed_array").Nested ==
		std::vector<std::vector<ElementValue>>(1365, std::vector<ElementValue>(2, 1.0))
	);
	for (auto &row : input.Nested)
		row.push_back(1.0);
	REQUIRE(detail::ValidRuntimeValue(Value{input}));
	const auto invalid = RunNode("pc.array_transpose", {}, {{"array_in", input}});
	CHECK_FALSE(invalid.Ok);
	CHECK(invalid.Code == Status::LimitExceeded);
	CHECK(invalid.Message == "source array output exceeds bounds");
	CHECK(invalid.Values.empty());
}

TEST_CASE(
	"Transpose column storage obeys exact live budget with earlier output retained",
	"[imagegraph][array_structure][evaluation_budget]"
) {
	ArrayValue input{ValueType::Any, {}};
	for (size_t row = 0; row < 64; ++row)
		input.Items.push_back(Row({Leaf(double(row)), Text(std::string(32, 'x'))}));
	const Value value = input;
	Node node{"node", "pc.array_transpose", "", {}, {}};
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	const auto executor = detail::FindExecutor(node.Type);
	REQUIRE(executor);
	const EvaluationRequest request;
	uint64_t peak = 0;
	for (int attempt = 0; attempt < 3; ++attempt) {
		const uint64_t limit = attempt == 0 ? Limits::MaximumEvaluationBytes : attempt == 1 ? peak - 1 : peak;
		detail::EvaluationBudget budget(limit);
		auto earlier = budget.Reserve(512);
		REQUIRE(earlier);
		{
			detail::NodeContext context(node, *entry, request, budget);
			context.ByteBudget = limit;
			context.ValueViews = {{"array_in", &value}};
			const bool ok = executor(context);
			if (attempt == 1) {
				CHECK_FALSE(ok);
				CHECK(context.FailureCode == Status::LimitExceeded);
				CHECK(context.OutputValues.empty());
			} else {
				REQUIRE(ok);
				REQUIRE(context.OutputValues.size() == 1);
				const auto &output = std::get<ArrayValue>(context.OutputValues.front().Data);
				CHECK(output.Items.size() == 2);
				if (attempt == 0) peak = budget.Peak();
				CHECK(budget.Peak() == peak);
			}
			CHECK(budget.Peak() <= limit);
		}
		CHECK(budget.Used() == 512);
		CHECK(std::get<ArrayValue>(value) == input);
	}
}
