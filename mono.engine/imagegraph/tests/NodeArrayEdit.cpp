#include "../src/ValuePayload.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>
TEST_SUITE_ID("engine.imagegraph.node_array_edit")
using namespace engine::imagegraph;
using namespace imagegraph_test;
namespace {
	SourceArrayItem N(double n) {
		return {ElementValue{n}};
	}
	SourceArrayItem T(std::string text) {
		return {ElementValue{std::move(text)}};
	}
	SourceArrayItem R(std::initializer_list<SourceArrayItem> values) {
		return {std::vector<SourceArrayItem>{values}};
	}
	ArrayValue A(std::initializer_list<SourceArrayItem> values) {
		ArrayValue a{ValueType::Any, {}};
		a.Items = values;
		return a;
	}
	const ArrayValue &Out(const NodeRun &run, std::string_view port = "array") {
		INFO(run.Message);
		REQUIRE(run.Ok);
		REQUIRE(run.OutputValue(port));
		return std::get<ArrayValue>(*run.OutputValue(port));
	}
	NodeRun Dynamic(std::string type, ArrayValue input, bool spread, std::vector<Value> values) {
		Node node{"operation", type, "", {}, {}};
		const auto *entry = FindCatalogueEntry(type);
		REQUIRE(entry);
		const EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		node.DynamicInputs.reserve(values.size());
		for (size_t i = 0; i < values.size(); ++i)
			node.DynamicInputs.push_back({"value_" + std::to_string(i), ValueType::Any, std::nullopt});
		// Input names stay owned by the node throughout execution.
		context.Values.clear();
		context.Values = {{"array", input}, {"spread_array", spread}, {"spread_content", spread}};
		for (size_t i = 0; i < node.DynamicInputs.size(); ++i)
			context.Values.emplace_back(node.DynamicInputs[i].Id, values[i]);
		NodeRun run;
		run.Ok = detail::FindExecutor(type)(context);
		run.Code = context.FailureCode;
		run.Message = context.FailureMessage;
		run.Values = std::move(context.OutputValues);
		return run;
	}
}
TEST_CASE(
	"Set extends with numeric zero and retains recursive replacement ownership", "[imagegraph][array_edit]"
) {
	const auto input = A({T("start"), R({N(2), R({T("deep")})})});
	const auto replacement = A({T("nested"), R({N(7)})});
	const auto run =
		RunNode("pc.array_set", {}, {{"array", input}, {"index", int64_t{4}}, {"value", replacement}});
	CHECK(
		Out(run).Items ==
		std::vector<SourceArrayItem>{input.Items[0], input.Items[1], N(0), N(0), R({T("nested"), R({N(7)})})}
	);
	CHECK(input.Items.size() == 2);
	const ArrayValue indices{ValueType::Integer, {int64_t{0}, int64_t{-1}, int64_t{3}}};
	const ArrayValue replacements{ValueType::Text, {std::string{"x"}, std::string{"y"}}};
	const auto loop =
		RunNode("pc.array_set", {}, {{"array", input}, {"index", indices}, {"value", replacements}});
	CHECK(Out(loop).Items == std::vector<SourceArrayItem>{T("x"), T("y"), N(0), T("x")});
}
TEST_CASE("Insert uses distinct scalar and array negative-index rules", "[imagegraph][array_edit]") {
	const ArrayValue input{ValueType::Scalar, {1.0, 2.0, 3.0}};
	const auto scalar =
		RunNode("pc.array_insert", {}, {{"array", input}, {"index", int64_t{-1}}, {"value", 9.0}});
	CHECK(Out(scalar).Elements == std::vector<ElementValue>{1.0, 2.0, 9.0, 3.0});
	const ArrayValue indices{ValueType::Integer, {int64_t{-1}, int64_t{-1}}};
	const ArrayValue values{ValueType::Scalar, {8.0, 9.0}};
	const auto array =
		RunNode("pc.array_insert", {}, {{"array", input}, {"index", indices}, {"value", values}});
	CHECK(Out(array).Elements == std::vector<ElementValue>{1.0, 8.0, 9.0, 2.0, 3.0});
	const auto spread = RunNode(
		"pc.array_insert",
		{},
		{{"array", input}, {"index", int64_t{1}}, {"value", A({T("a"), R({N(4)})})}, {"spread_array", true}}
	);
	CHECK(Out(spread).Items == std::vector<SourceArrayItem>{N(1), T("a"), R({N(4)}), N(2), N(3)});
}
TEST_CASE(
	"Get index overflow selects leaves or recursive rows without signed overflow", "[imagegraph][array_edit]"
) {
	const auto input = A({T("a"), R({T("b"), R({N(3)})}), N(4)});
	const auto row = RunNode("pc.array_get", {}, {{"array", input}, {"index", int64_t{-2}}});
	CHECK(Out(row, "value").Items == std::vector<SourceArrayItem>{T("b"), R({N(3)})});
	const ArrayValue indices{ValueType::Integer, {int64_t{-1}, int64_t{3}, int64_t{4}}};
	const auto loop =
		RunNode("pc.array_get", {}, {{"array", input}, {"index", indices}, {"overflow", EnumValue{1}}});
	CHECK(Out(loop, "value").Items == std::vector<SourceArrayItem>{N(4), T("a"), input.Items[1]});
	const auto ping = RunNode(
		"pc.array_get",
		{},
		{{"array", input}, {"index", std::numeric_limits<int64_t>::min()}, {"overflow", EnumValue{2}}}
	);
	REQUIRE(ping.Ok);
	CHECK(std::get<std::string>(*ping.OutputValue("value")) == "a");
	const auto random = RunNode("pc.array_get", {}, {{"array", input}, {"mode", EnumValue{1}}});
	REQUIRE(random.Ok);
	REQUIRE(random.Values.size() == 1);
	const auto repeated = RunNode("pc.array_get", {}, {{"array", input}, {"mode", EnumValue{1}}});
	REQUIRE(repeated.Ok);
	CHECK(random.Values == repeated.Values);
}
TEST_CASE(
	"Remove sorts indexes then resolves negatives against changing length", "[imagegraph][array_edit]"
) {
	const ArrayValue input{ValueType::Scalar, {1.0, 2.0, 3.0, 4.0, 5.0}};
	const ArrayValue indices{ValueType::Integer, {int64_t{-1}, int64_t{1}}};
	const auto run = RunNode("pc.array_remove", {}, {{"array", input}, {"index", indices}});
	CHECK(Out(run).Elements == std::vector<ElementValue>{1.0, 3.0, 4.0});
	const auto recursive = A({R({N(1)}), T("x"), R({N(1)}), N(9)});
	const auto remove =
		RunNode("pc.array_remove", {}, {{"array", recursive}, {"type", EnumValue{1}}, {"value", A({N(1)})}});
	CHECK(Out(remove).Items == std::vector<SourceArrayItem>{T("x"), R({N(1)}), N(9)});
	const auto find = RunNode("pc.array_find", {}, {{"array", recursive}, {"value", A({N(1)})}});
	REQUIRE(find.Ok);
	CHECK(std::get<int64_t>(*find.OutputValue("index")) == 0);
}
TEST_CASE(
	"Dynamic Add and Zip preserve recursive shape and scalar zero padding", "[imagegraph][array_edit]"
) {
	const auto input = A({T("first"), R({N(1)})});
	const auto append =
		Dynamic("pc.array_add", input, true, {A({R({T("nested")}), N(3)}), std::string{"last"}});
	CHECK(
		Out(append, "output").Items ==
		std::vector<SourceArrayItem>{T("first"), R({N(1)}), R({T("nested")}), N(3), T("last")}
	);
	const auto zip = Dynamic("pc.array_zip", {}, false, {input, std::string{"scalar"}});
	CHECK(
		Out(zip, "output").Items ==
		std::vector<SourceArrayItem>{R({T("first"), T("scalar")}), R({R({N(1)}), N(0)})}
	);
}
TEST_CASE(
	"Edit output bounds reject selected subtrees before payload construction", "[imagegraph][array_edit]"
) {
	ArrayValue input{ValueType::Scalar, {}};
	input.Elements.assign(4096, 1.0);
	const auto insert = RunNode("pc.array_insert", {}, {{"array", input}, {"value", 2.0}});
	CHECK(insert.Code == Status::LimitExceeded);
	CHECK(insert.Values.empty());
	const auto deep = A({R({T("deep")})});
	const ArrayValue indexes{ValueType::Integer, {int64_t{0}, int64_t{0}}};
	const auto selected = RunNode("pc.array_get", {}, {{"array", deep}, {"index", indexes}});
	CHECK(
		Out(selected, "value").Nested ==
		std::vector<std::vector<ElementValue>>{{std::string{"deep"}}, {std::string{"deep"}}}
	);
	ArrayValue large{ValueType::Any, {}};
	large.Items.push_back({std::vector<SourceArrayItem>(4095, N(1))});
	REQUIRE(detail::ValidRuntimeValue(Value{large}));
	const auto repeated = RunNode("pc.array_get", {}, {{"array", large}, {"index", indexes}});
	CHECK(repeated.Code == Status::LimitExceeded);
	CHECK(repeated.Values.empty());
}

TEST_CASE("Array edits execute upstream of a separate graph snapshot target", "[imagegraph][array_edit]") {
	const ArrayValue flat{ValueType::Scalar, {1.0, 2.0, 3.0}};
	struct Fixture {
		std::string Type, Output;
		std::vector<AuthoredValue> Values;
		Value Expected;
	};
	const Fixture fixtures[]{
		{"pc.array_add", "output", {{"array", flat}}, ArrayValue{ValueType::Scalar, {1.0, 2.0, 3.0}}},
		{"pc.array_get", "value", {{"array", flat}, {"index", int64_t{-1}}}, 3.0},
		{"pc.array_set",
		 "array",
		 {{"array", flat}, {"index", int64_t{1}}, {"value", 9.0}},
		 ArrayValue{ValueType::Scalar, {1.0, 9.0, 3.0}}},
		{"pc.array_insert",
		 "array",
		 {{"array", flat}, {"index", int64_t{1}}, {"value", 9.0}},
		 ArrayValue{ValueType::Scalar, {1.0, 9.0, 2.0, 3.0}}},
		{"pc.array_remove",
		 "array",
		 {{"array", flat}, {"index", int64_t{1}}},
		 ArrayValue{ValueType::Scalar, {1.0, 3.0}}},
		{"pc.array_find", "index", {{"array", flat}, {"value", 2.0}}, int64_t{1}},
	};
	for (const auto &fixture : fixtures) {
		INFO(fixture.Type);
		Document document;
		document.FormatVersion = 9;
		Node operation{"operation", fixture.Type, "", {}, {}};
		Node source{
			"source",
			"pc.array",
			"",
			{},
			{{"type", EnumValue{0}}, {"spread_array", true}},
			{{"value_0", ValueType::Array, Value{flat}}}
		};
		document.Nodes = {source, operation, Node{"capture", "pc.array_copy", "", {}, {}}};
		document.Links = {
			{"source", "array", "operation", "array"}, {"operation", fixture.Output, "capture", "array"}
		};
		for (const auto &property : fixture.Values) {
			if (property.Port == "array") continue;
			if (property.Port == "value") {
				document.Nodes.push_back(
					Node{"replacement", "pc.number_simple", "", {}, {{"value", property.Data}}}
				);
				document.Links.push_back({"replacement", "number", "operation", "value"});
			} else
				document.Nodes[1].Values.push_back(property);
		}
		document.Outputs = {{"out", "capture", "array"}};
		Plan plan;
		Diagnostic diagnostic;
		const Status compiled = Compile(document, plan, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << " " << diagnostic.Message);
		REQUIRE(compiled == Status::Ok);
		EvaluationSnapshot snapshot;
		REQUIRE(EvaluateNodeInputs(document, plan, "capture", {}, snapshot, diagnostic) == Status::Ok);
		bool found = false;
		for (const auto &value : snapshot.Values())
			if (value.Port == "array") {
				CHECK(value.Data == fixture.Expected);
				found = true;
			}
		CHECK(found);
	}
	Document zip;
	zip.FormatVersion = 9;
	Node operation{
		"operation",
		"pc.array_zip",
		"",
		{},
		{},
		{{"value_0", ValueType::Array, Value{flat}}, {"value_1", ValueType::Text, Value{std::string{"x"}}}}
	};
	zip.Nodes = {operation, Node{"capture", "pc.array_copy", "", {}, {}}};
	zip.Links = {{"operation", "output", "capture", "array"}};
	zip.Outputs = {{"out", "capture", "array"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(zip, plan, diagnostic) == Status::Ok);
	EvaluationSnapshot snapshot;
	REQUIRE(EvaluateNodeInputs(zip, plan, "capture", {}, snapshot, diagnostic) == Status::Ok);
	bool found = false;
	const auto expected = A({R({N(1), T("x")}), R({N(2), N(0)}), R({N(3), N(0)})});
	for (const auto &value : snapshot.Values())
		if (value.Port == "array") {
			CHECK(value.Data == Value{expected});
			found = true;
		}
	CHECK(found);
}

TEST_CASE("Array Get and Set copy owned surface payloads", "[imagegraph][array_edit]") {
	const Image pixel{1, 1, {10, 20, 30, 255}, 37};
	ArrayValue input{ValueType::Any, {}};
	input.Items = {SourceArrayItem{pixel}, T("text")};
	const auto get = RunNode("pc.array_get", {}, {{"array", input}, {"index", int64_t{0}}});
	REQUIRE(get.Ok);
	REQUIRE(get.Images.size() == 1);
	CHECK(get.Images[0].first == "value");
	CHECK(get.Images[0].second.Pixels == pixel.Pixels);
	CHECK(get.Images[0].second.Pixels.data() != pixel.Pixels.data());
	CHECK(get.Images[0].second.Hash == 37);
	const auto set = RunNode(
		"pc.array_set", {{"value", &pixel}}, {{"array", A({T("first"), T("second")})}, {"index", int64_t{1}}}
	);
	REQUIRE(Out(set).Items.size() == 2);
	const auto &copied = std::get<Image>(Out(set).Items[1].Data);
	CHECK(copied.Pixels == pixel.Pixels);
	CHECK(copied.Pixels.data() != pixel.Pixels.data());
}

TEST_CASE("Find uses source recursive equality depth cutoff", "[imagegraph][array_edit]") {
	for (size_t depth : {size_t{8}, size_t{9}}) {
		SourceArrayItem needle = N(1);
		for (size_t i = 0; i < depth; ++i)
			needle = {std::vector<SourceArrayItem>{std::move(needle)}};
		ArrayValue value{ValueType::Any, {}};
		value.Items = std::get<std::vector<SourceArrayItem>>(needle.Data);
		ArrayValue input{ValueType::Any, {}};
		input.Items.push_back(needle);
		const auto run = RunNode("pc.array_find", {}, {{"array", input}, {"value", value}});
		REQUIRE(run.Ok);
		CHECK(std::get<int64_t>(*run.OutputValue("index")) == (depth == 8 ? 0 : -1));
	}
}

TEST_CASE("Insert diagnoses extreme negative indexes without overflow", "[imagegraph][array_edit]") {
	const ArrayValue empty{ValueType::Integer, {}};
	const ArrayValue indices{ValueType::Integer, {std::numeric_limits<int64_t>::min()}};
	const auto array = RunNode("pc.array_insert", {}, {{"array", empty}, {"index", indices}, {"value", 1.0}});
	CHECK(array.Code == Status::InvalidValue);
	CHECK(array.Values.empty());
	const auto scalar = RunNode(
		"pc.array_insert",
		{},
		{{"array", empty}, {"index", std::numeric_limits<int64_t>::min()}, {"value", 1.0}}
	);
	CHECK(scalar.Code == Status::InvalidValue);
	CHECK(scalar.Values.empty());
	const auto spreadEmpty = RunNode(
		"pc.array_insert",
		{},
		{{"array", empty},
		 {"index", std::numeric_limits<int64_t>::min()},
		 {"value", empty},
		 {"spread_array", true}}
	);
	CHECK(Out(spreadEmpty) == empty);
}
TEST_CASE(
	"Find and Remove compare native integers exactly across floating precision boundaries",
	"[imagegraph][array_edit]"
) {
	const int64_t first = 9007199254740992LL, second = first + 1;
	const ArrayValue input{
		ValueType::Integer,
		{first, second, std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max()}
	};
	const auto find = RunNode("pc.array_find", {}, {{"array", input}, {"value", second}});
	REQUIRE(find.Ok);
	CHECK(std::get<int64_t>(*find.OutputValue("index")) == 1);
	const auto remove =
		RunNode("pc.array_remove", {}, {{"array", input}, {"type", EnumValue{1}}, {"value", second}});
	CHECK(
		Out(remove).Elements ==
		std::vector<ElementValue>{
			first, std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max()
		}
	);
	const auto rounded = RunNode(
		"pc.array_find", {}, {{"array", ArrayValue{ValueType::Integer, {second}}}, {"value", double(first)}}
	);
	REQUIRE(rounded.Ok);
	CHECK(std::get<int64_t>(*rounded.OutputValue("index")) == -1);
	const auto minimum = RunNode("pc.array_find", {}, {{"array", input}, {"value", -9223372036854775808.0}});
	REQUIRE(minimum.Ok);
	CHECK(std::get<int64_t>(*minimum.OutputValue("index")) == 2);
	const auto upper = RunNode("pc.array_find", {}, {{"array", input}, {"value", 9223372036854775808.0}});
	REQUIRE(upper.Ok);
	CHECK(std::get<int64_t>(*upper.OutputValue("index")) == -1);
}
TEST_CASE(
	"Repeated images reject output bytes before copying and source identity remains unavailable",
	"[imagegraph][array_edit]"
) {
	const Image surface{512, 512, std::vector<uint8_t>(512 * 512 * 4, 255), 41};
	ArrayValue input{ValueType::Any, {}};
	input.Items.push_back({surface});
	REQUIRE(detail::ValidRuntimeValue(Value{input}));
	const ArrayValue indexes{ValueType::Integer, {int64_t{0}, int64_t{0}, int64_t{0}, int64_t{0}}};
	const auto repeat = RunNode("pc.array_get", {}, {{"array", input}, {"index", indexes}});
	CHECK(repeat.Code == Status::LimitExceeded);
	CHECK(repeat.Values.empty());
	CHECK(repeat.Images.empty());
	const auto identity = RunNode("pc.array_find", {{"value", &surface}}, {{"array", input}});
	CHECK(identity.Code == Status::UnsupportedExecution);
	CHECK(identity.Values.empty());
	CHECK(identity.Images.empty());
}
TEST_CASE(
	"Insertion and Zip admit added recursive depth before payload construction", "[imagegraph][array_edit]"
) {
	SourceArrayItem branch = N(1);
	for (size_t level = 1; level < Limits::MaximumArrayDepth; ++level)
		branch = {std::vector<SourceArrayItem>{std::move(branch)}};
	ArrayValue deep{ValueType::Any, {}};
	deep.Items.push_back(std::move(branch));
	REQUIRE(detail::ValidRuntimeValue(Value{deep}));
	const ArrayValue empty{ValueType::Integer, {}};
	const auto insert = RunNode("pc.array_insert", {}, {{"array", empty}, {"value", deep}});
	CHECK(insert.Code == Status::LimitExceeded);
	CHECK(insert.Values.empty());
	const auto zip = Dynamic("pc.array_zip", {}, false, {deep});
	CHECK(zip.Code == Status::LimitExceeded);
	CHECK(zip.Values.empty());
	const auto scalarZip = Dynamic("pc.array_zip", {}, false, {1.0, std::string{"scalar"}});
	CHECK(scalarZip.Code == Status::InvalidValue);
	CHECK(scalarZip.Values.empty());
	const auto emptyZip = Dynamic("pc.array_zip", {}, false, {empty});
	CHECK(emptyZip.Code == Status::InvalidValue);
	CHECK(emptyZip.Values.empty());
}
TEST_CASE(
	"Set output publication respects the measured live allocation boundary", "[imagegraph][array_edit]"
) {
	const Node node{"operation", "pc.array_set", "", {}, {}};
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	const auto executor = detail::FindExecutor(node.Type);
	REQUIRE(executor);
	const EvaluationRequest request;
	const Value source = A({T("old"), R({T("nested"), N(2)})});
	const Value index = int64_t{4}, replacement = A({T("new"), R({N(7)})});
	uint64_t peak = 0;
	for (size_t attempt = 0; attempt < 3; ++attempt) {
		const uint64_t limit = attempt == 0 ? Limits::MaximumEvaluationBytes : attempt == 1 ? peak - 1 : peak;
		detail::EvaluationBudget budget(limit);
		auto unrelated = budget.Reserve(512);
		REQUIRE(unrelated);
		{
			detail::NodeContext context(node, *entry, request, budget);
			context.ByteBudget = limit;
			context.ValueViews = {{"array", &source}, {"index", &index}, {"value", &replacement}};
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
			}
			CHECK(budget.Peak() <= limit);
		}
		CHECK(budget.Used() == 512);
	}
	CHECK(std::get<ArrayValue>(source) == A({T("old"), R({T("nested"), N(2)})}));
}

TEST_CASE("signed weighted selector preserves source endpoint check order", "[imagegraph][array_edit]") {
	ArraySelectorValue selector;
	selector.Data.emplace();
	selector.Data->Values = A({N(10), N(20), N(30)});
	selector.Data->CumulativeWeights = {0, 1, -2};
	selector.Data->TotalWeight = -1;
	REQUIRE(detail::ValidRuntimeValue(selector));
	for (int64_t seed : {int64_t{0}, int64_t{1}, int64_t{42}}) {
		const auto result =
			RunNode("pc.array_get", {}, {{"array", selector}, {"mode", EnumValue{1}}, {"seed", seed}});
		INFO(result.Message);
		REQUIRE(result.Ok);
		REQUIRE(result.OutputValue("value"));
		CHECK(std::get<double>(*result.OutputValue("value")) == 10);
	}
}
