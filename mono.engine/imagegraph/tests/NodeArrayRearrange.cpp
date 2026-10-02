#include "../src/ValuePayload.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.imagegraph.node_array_rearrange")
using namespace engine::imagegraph;
using namespace imagegraph_test;
namespace {
	using Items = std::vector<SourceArrayItem>;
	SourceArrayItem Number(double value) {
		return {ElementValue{value}};
	}
	SourceArrayItem Row(Items children) {
		return {std::move(children)};
	}
	ArrayValue General(Items children) {
		ArrayValue value{ValueType::Any, {}};
		value.Items = std::move(children);
		return value;
	}
	const ArrayValue &RearrangeOutputArray(const NodeRun &run) {
		INFO(run.Message);
		REQUIRE(run.Ok);
		REQUIRE(run.OutputValue("array"));
		return std::get<ArrayValue>(*run.OutputValue("array"));
	}
	// Decode only the lossless publication carriers; expected source shapes are
	// authored independently.
	Items Content(const ArrayValue &value) {
		if (!value.Items.empty()) return value.Items;
		Items result;
		for (const auto &member : value.Elements)
			result.push_back({member});
		for (const auto &nativeRow : value.Nested) {
			Items row;
			for (const auto &member : nativeRow)
				row.push_back({member});
			result.push_back(Row(std::move(row)));
		}
		return result;
	}

} // namespace
TEST_CASE("Rearrange owned leaves preserve data and distinct backing", "[imagegraph][array_rearrange]") {
	MaterialValue3D material;
	material.Edit().Surface = Image{1, 1, {3, 5, 7, 255}, 29};
	material.Edit().Diffuse = 0.75;
	MeshValue3D mesh;
	auto &geometry = mesh.Data.emplace();
	geometry.LocalTransforms.push_back(MeshTransform3D{});
	geometry.Materials.push_back(material);
	geometry.Parts.push_back(
		{{MeshVertex3D{{0, 0, 0}, {0, 0, 1}, {0, 0}, {1, 2, 3, 255}},
		  MeshVertex3D{{1, 0, 0}, {0, 0, 1}, {1, 0}, {4, 5, 6, 255}},
		  MeshVertex3D{{0, 1, 0}, {0, 0, 1}, {0, 1}, {7, 8, 9, 255}}},
		 0}
	);
	geometry.Edges.push_back({{0, 0, 0}, {1, 0, 0}});
	const Gradient gradient{2, {{0, {1, 2, 3, 255}}, {1, {4, 5, 6, 255}}}};
	const Path2D path{true, {{{1, 2, 3, 4, 5, 6}, 7}}, {{0.25, 0.75}}};
	const AudioBit audio{{}, 48000, {{0.25, -0.5}, {0.75, -0.25}}};
	const std::vector<Value> fixtures{gradient, path, audio, mesh, material};
	const Node node{"uniform", "pc.array_rearrange", "", {}, {}};
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	const auto executor = detail::FindExecutor(node.Type);
	REQUIRE(executor);
	const EvaluationRequest request;
	for (const Value &input : fixtures) {
		REQUIRE(detail::ValidValuePayload(input, true));
		const Value before = input;
		const ElementValue expected = std::visit(
			[](const auto &member) -> ElementValue {
				using T = std::decay_t<decltype(member)>;
				if constexpr (std::is_constructible_v<ElementValue, T>)
					return member;
				else
					return int64_t{0};
			},
			input
		);
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		const Value source = ArrayValue{detail::PayloadType(input), {expected, expected, expected}};
		context.ValueViews = {{"array", &source}};
		context.Values = {{"orders", ArrayValue{ValueType::Integer, {int64_t{0}, int64_t{0}, int64_t{0}}}}};
		REQUIRE(executor(context));
		REQUIRE(context.OutputValues.size() == 1);
		auto &output = std::get<ArrayValue>(context.OutputValues.front().Data);
		CHECK(Content(output) == Items(3, SourceArrayItem{expected}));
		REQUIRE(output.Elements.size() == 3);
		std::visit(
			[](auto &member) {
				using T = std::decay_t<decltype(member)>;
				if constexpr (std::is_same_v<T, Gradient>)
					member.Keys.front().Color.Red = 99;
				else if constexpr (std::is_same_v<T, Path2D>)
					member.Anchors.front().Controls[0] = 99;
				else if constexpr (std::is_same_v<T, AudioBit>)
					member.Channels.front().front() = 99;
				else if constexpr (std::is_same_v<T, MeshValue3D>) {
					member.Data->Parts.front().Vertices.front().Position.X = 99;
					member.Data->Materials.front().Edit().Surface->Pixels.front() = 99;
				} else if constexpr (std::is_same_v<T, MaterialValue3D>)
					member.Edit().Surface->Pixels.front() = 99;
			},
			output.Elements.front()
		);
		CHECK(output.Elements[1] == expected);
		CHECK(output.Elements[2] == expected);
		CHECK(input == before);
	}
}
TEST_CASE(
	"Rearrange fallback is pure and safe indices preserve exact integers", "[imagegraph][array_rearrange]"
) {
	const ArrayValue source{
		ValueType::Text, {std::string{"a"}, std::string{"b"}, std::string{"c"}, std::string{"d"}}
	};
	const Items identity = Content(source);
	for (const Value &orders : std::vector<Value>{
			 ArrayValue{ValueType::Integer, {}}, int64_t{7}, ArrayValue{ValueType::Integer, {int64_t{1}}}
		 }) {
		const auto before = orders;
		CHECK(
			Content(RearrangeOutputArray(
				RunNode("pc.array_rearrange", {}, {{"array", source}, {"orders", orders}})
			)) == identity
		);
		CHECK(orders == before);
	}
	CHECK(Content(RearrangeOutputArray(RunNode("pc.array_rearrange", {}, {{"array", source}}))) == identity);
	const std::vector<std::pair<ArrayValue, Items>> schedules{
		{ArrayValue{ValueType::Integer, {int64_t{3}, int64_t{0}, int64_t{3}, int64_t{2}}},
		 {identity[3], identity[0], identity[3], identity[2]}},
		{ArrayValue{ValueType::Scalar, {2.5, 3.5, -0.5, -1.5}},
		 {identity[2], Number(0), identity[0], Number(0)}},
		{ArrayValue{
			 ValueType::Integer,
			 {std::numeric_limits<int64_t>::min(),
			  std::numeric_limits<int64_t>::max(),
			  int64_t{9007199254740993LL},
			  int64_t{4}}
		 },
		 Items(4, Number(0))},
		{ArrayValue{ValueType::Scalar, {1e300, -1e300, 0.5, 1.5}},
		 {Number(0), Number(0), identity[0], identity[2]}},
		{ArrayValue{ValueType::Boolean, {false, true, true, false}},
		 {identity[0], identity[1], identity[1], identity[0]}},
		{ArrayValue{ValueType::Enum, {EnumValue{3}, EnumValue{2}, EnumValue{-1}, EnumValue{0}}},
		 {identity[3], identity[2], Number(0), identity[0]}},
		{ArrayValue{
			 ValueType::Colour,
			 {Colour{2, 0, 0, 0}, Colour{0, 0, 0, 255}, Colour{0, 1, 0, 0}, Colour{3, 0, 0, 0}}
		 },
		 {identity[2], Number(0), Number(0), identity[3]}}
	};
	for (const auto &[orders, expected] : schedules)
		CHECK(
			Content(RearrangeOutputArray(
				RunNode("pc.array_rearrange", {}, {{"array", source}, {"orders", orders}})
			)) == expected
		);
	for (const auto type : {ValueType::Integer, ValueType::Any, ValueType::Text}) {
		const auto run = RunNode("pc.array_rearrange", {}, {{"array", ArrayValue{type, {}}}});
		const auto &out = RearrangeOutputArray(run);
		CHECK(out.ElementType == type);
		CHECK(Content(out).empty());
	}
}
TEST_CASE(
	"Rearrange packed source roots and whole members follow field order", "[imagegraph][array_rearrange]"
) {
	const Curve curve{{1, 2, 3, 4, 5, 6}, {{{7, 8, 9, 10, 11, 12}}, {{13, 14, 15, 16, 17, 18}}}};
	Items curveFields;
	for (double n = 1; n <= 18; ++n)
		curveFields.push_back(Number(n));
	const std::vector<std::pair<Value, Items>> fixtures{
		{Vector2{2, 9}, {Number(2), Number(9)}},
		{Vector3{2, 4, 8}, {Number(2), Number(4), Number(8)}},
		{Vector4{2, 4, 8, 16}, {Number(2), Number(4), Number(8), Number(16)}},
		{Quaternion{3, 5, 7, 11}, {Number(3), Number(5), Number(7), Number(11)}},
		{Area{2, 4, 8, 16, 1, 2}, {Number(2), Number(4), Number(8), Number(16), Number(1), Number(2)}},
		{curve, curveFields}
	};
	for (const auto &[value, fields] : fixtures) {
		CHECK(Content(RearrangeOutputArray(RunNode("pc.array_rearrange", {}, {{"array", value}}))) == fields);
		const ElementValue member = std::visit(
			[](const auto &v) -> ElementValue {
				if constexpr (std::is_constructible_v<ElementValue, std::decay_t<decltype(v)>>)
					return v;
				else
					return int64_t{0};
			},
			value
		);
		const ArrayValue array{detail::PayloadType(value), {member, member}};
		CHECK(
			Content(RearrangeOutputArray(RunNode(
				"pc.array_rearrange",
				{},
				{{"array", array}, {"orders", ArrayValue{ValueType::Integer, {int64_t{1}, int64_t{1}}}}}
			))) == Items{Row(fields), Row(fields)}
		);
	}
}
TEST_CASE(
	"Rearrange recursive whole copies preserve empty rows and "
	"independent images",
	"[imagegraph][array_rearrange]"
) {
	const Image image{1, 1, {1, 2, 3, 255}, 19};
	const Items members{
		Row({{ElementValue{std::string{"keep"}}}, Row({}), {image}}),
		{ElementValue{int64_t{9007199254740993LL}}},
		Row({Number(7), Row({Row({})})})
	};
	const Value input = General(members);
	const Value before = input;
	const Node node{
		"rearrange",
		"pc.array_rearrange",
		"",
		{},
		{{"orders", ArrayValue{ValueType::Integer, {int64_t{0}, int64_t{0}, int64_t{2}}}}}
	};
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	const auto executor = detail::FindExecutor(node.Type);
	REQUIRE(executor);
	const EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.ValueViews = {{"array", &input}};
	context.Values = {{"orders", node.Values.front().Data}};
	const auto authoredBefore = node.Values;
	REQUIRE(executor(context));
	REQUIRE(context.OutputValues.size() == 1);
	auto &out = std::get<ArrayValue>(context.OutputValues.front().Data);
	CHECK(Content(out) == Items{members[0], members[0], members[2]});
	auto &first = std::get<Items>(out.Items[0].Data);
	std::get<std::string>(std::get<ElementValue>(first[0].Data)) = "changed";
	std::get<Image>(first[2].Data).Pixels[0] = 99;
	CHECK(out.Items[1] == members[0]);
	CHECK(input == before);
	CHECK(node.Values == authoredBefore);
}
TEST_CASE(
	"Rearrange executes linked fractional Orders before distinct capture", "[imagegraph][array_rearrange]"
) {
	const ArrayValue source{
		ValueType::Text, {std::string{"a"}, std::string{"b"}, std::string{"c"}, std::string{"d"}}
	};
	const ArrayValue orders{ValueType::Scalar, {2.5, 3.5, -0.5, -1.5}};
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"source",
		 "pc.array",
		 "",
		 {},
		 {{"type", EnumValue{0}}, {"spread_array", true}},
		 {{"value_0", ValueType::Array, Value{source}}}},
		{"orders",
		 "pc.array",
		 "",
		 {},
		 {{"type", EnumValue{0}}, {"spread_array", true}},
		 {{"value_0", ValueType::Array, Value{orders}}}},
		{"rearrange", "pc.array_rearrange", "", {}, {}},
		{"capture", "pc.array_copy", "", {}, {}}
	};
	document.Links = {
		{"source", "array", "rearrange", "array"},
		{"orders", "array", "rearrange", "orders"},
		{"rearrange", "array", "capture", "array"}
	};
	document.Outputs = {{"out", "capture", "array"}};
	const auto authored = document.Nodes;
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationSnapshot snapshot;
	const auto status = EvaluateNodeInputs(document, plan, "capture", {}, snapshot, diagnostic);
	INFO(diagnostic.NodeId << ":" << diagnostic.Port << " " << diagnostic.Message);
	REQUIRE(status == Status::Ok);
	bool found = false;
	for (const auto &value : snapshot.Values())
		if (value.Port == "array") {
			CHECK(
				Content(std::get<ArrayValue>(value.Data)) ==
				Items{Content(source)[2], Number(0), Content(source)[0], Number(0)}
			);
			found = true;
		}
	CHECK(found);
	CHECK(document.Nodes == authored);
}
TEST_CASE("Rearrange exact selected count and unselected expansion bounds", "[imagegraph][array_rearrange]") {
	const ArrayValue exact{ValueType::Integer, std::vector<ElementValue>(4096, int64_t{7})};
	CHECK(
		Content(RearrangeOutputArray(RunNode("pc.array_rearrange", {}, {{"array", exact}}))).size() == 4096
	);
	Curve large;
	large.Anchors.resize(256);
	const Value source = General({Row(Items(8, SourceArrayItem{ElementValue{large}})), Number(3)});
	const ArrayValue small{ValueType::Integer, {int64_t{1}, int64_t{1}}};
	CHECK(
		Content(
			RearrangeOutputArray(RunNode("pc.array_rearrange", {}, {{"array", source}, {"orders", small}}))
		) == Items{Number(3), Number(3)}
	);
	const Value crowded = General({Row(Items(2047, Number(7))), Number(3)});
	CHECK(
		Content(
			RearrangeOutputArray(RunNode(
				"pc.array_rearrange",
				{},
				{{"array", crowded}, {"orders", ArrayValue{ValueType::Integer, {int64_t{0}, int64_t{0}}}}}
			))
		).size() == 2
	);
	const Value tooMany = General({Row(Items(2048, Number(7))), Number(3)});
	const auto rejected = RunNode(
		"pc.array_rearrange",
		{},
		{{"array", tooMany}, {"orders", ArrayValue{ValueType::Integer, {int64_t{0}, int64_t{0}}}}}
	);
	CHECK_FALSE(rejected.Ok);
	CHECK(rejected.Code == Status::LimitExceeded);
	CHECK(rejected.Port == "array");
	CHECK(rejected.Values.empty());
	CHECK(rejected.Images.empty());
}
TEST_CASE("Rearrange shared ledger peak and ownership lifetime", "[imagegraph][array_rearrange]") {
	const Node node{"rearrange", "pc.array_rearrange", "", {}, {}};
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	const auto executor = detail::FindExecutor(node.Type);
	REQUIRE(executor);
	const Value source = General({{ElementValue{std::string(8192, 'x')}}, Row({Number(3), Row({})})});
	const Value before = source;
	const Value orders = ArrayValue{ValueType::Integer, {int64_t{0}, int64_t{0}}};
	const EvaluationRequest request;
	uint64_t peak = 0;
	for (size_t attempt = 0; attempt < 3; ++attempt) {
		const uint64_t limit = attempt == 0 ? Limits::MaximumEvaluationBytes : attempt == 1 ? peak - 1 : peak;
		detail::EvaluationBudget budget(limit);
		auto unrelated = budget.Reserve(512);
		REQUIRE(unrelated);
		{
			detail::AllocationReservation owner;
			std::vector<AuthoredValue> outputs;
			{
				detail::NodeContext context(node, *entry, request, budget);
				context.ByteBudget = limit;
				context.ValueViews = {{"array", &source}, {"orders", &orders}};
				const bool ok = executor(context);
				if (attempt == 1) {
					CHECK_FALSE(ok);
					CHECK(context.FailureCode == Status::LimitExceeded);
					CHECK(context.OutputValues.empty());
					CHECK(context.OutputImages.empty());
					CHECK(context.OutputImageArrays.empty());
				} else {
					REQUIRE(ok);
					if (attempt == 0) peak = budget.Peak();
					CHECK(budget.Peak() == peak);
					REQUIRE(context.OutputValues.size() == 1);
					CHECK(
						Content(std::get<ArrayValue>(context.OutputValues[0].Data)) ==
						Items{
							Content(std::get<ArrayValue>(source))[0], Content(std::get<ArrayValue>(source))[0]
						}
					);
					outputs = std::move(context.OutputValues);
					owner = context.TakeOutputReservation();
				}
				CHECK(budget.Peak() <= limit);
			}
			if (attempt != 1) CHECK(budget.Used() > 512);
		}
		CHECK(budget.Used() == 512);
		CHECK(source == before);
	}
}
TEST_CASE(
	"Rearrange validates unused and zero-root image pools and finite samples", "[imagegraph][array_rearrange]"
) {
	const Node node{"rearrange", "pc.array_rearrange", "", {}, {}};
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	const auto executor = detail::FindExecutor(node.Type);
	REQUIRE(executor);
	const EvaluationRequest request;
	const Image finite{1, 1, {1, 2, 3, 255}, 19};
	const std::vector<Image> invalid{
		{1, 1, {0, 0, 128, 127}, 31, SurfaceFormat::R32Float},
		{1, 1, {0, 0, 192, 127}, 32, SurfaceFormat::R32Float},
		{1, 1, {0, 124}, 33, SurfaceFormat::R16Float},
		{1, 1, {0, 126}, 34, SurfaceFormat::R16Float},
		{1, 1, {1, 2, 3}, 35}
	};
	for (const auto &image : invalid)
		for (bool empty : {false, true})
			for (size_t route = 0; route < 4; ++route) {
				const ImageArray pool{
					{finite, image},
					empty ? std::vector<ImageArrayItem>{} : std::vector<ImageArrayItem>{{size_t{0}}}
				};
				const Value nested = empty ? Value{General({})} : Value{General({Row({{image}}), Number(7)})};
				detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
				auto unrelated = budget.Reserve(512);
				REQUIRE(unrelated);
				{
					detail::NodeContext context(node, *entry, request, budget);
					context.ByteBudget = Limits::MaximumEvaluationBytes;
					if (route == 0)
						context.Images = {{"array", &image}};
					else if (route == 1)
						context.ImageArrays = {{"array", &pool}};
					else if (route == 2) {
						context.Images = {{"orders", &image}};
						context.Values = {{"array", General({Number(7), Number(8)})}};
					} else {
						if (empty) continue;
						context.ValueViews = {{"array", &nested}};
					}
					CHECK_FALSE(executor(context));
					CHECK(context.FailureCode == Status::InvalidValue);
					CHECK(context.FailurePort == (route == 2 ? "orders" : "array"));
					CHECK_FALSE(context.FailureMessage.empty());
					CHECK(context.OutputValues.empty());
					CHECK(context.OutputImages.empty());
					CHECK(context.OutputImageArrays.empty());
				}
				CHECK(budget.Used() == 512);
				CHECK(pool.Images.back() == image);
			}
}
TEST_CASE("Rearrange image duplicates obey exact output byte limits", "[imagegraph][array_rearrange]") {
	const Image image{512, 512, std::vector<uint8_t>(512 * 512 * 4, 7), 21};
	const Node node{"rearrange", "pc.array_rearrange", "", {}, {}};
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	const auto executor = detail::FindExecutor(node.Type);
	REQUIRE(executor);
	const EvaluationRequest request;
	for (size_t count : {size_t{3}, size_t{4}}) {
		const ImageArray source{{image}, std::vector<ImageArrayItem>(count, ImageArrayItem{size_t{0}})};
		const Value orders = ArrayValue{ValueType::Integer, std::vector<ElementValue>(count, int64_t{0})};
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		context.ImageArrays = {{"array", &source}};
		context.ValueViews = {{"orders", &orders}};
		const bool ok = executor(context);
		if (count == 3) {
			REQUIRE(ok);
			REQUIRE(context.OutputImageArrays.size() == 1);
			auto &out = context.OutputImageArrays.front().second;
			REQUIRE(out.Images.size() == 3);
			for (const auto &copy : out.Images)
				CHECK(copy == image);
			out.Images[0].Pixels[0] = 99;
			CHECK(out.Images[1] == image);
			CHECK(out.Images[2] == image);
		} else {
			CHECK_FALSE(ok);
			CHECK(context.FailureCode == Status::LimitExceeded);
			CHECK(context.FailurePort == "array");
			CHECK(context.OutputValues.empty());
			CHECK(context.OutputImages.empty());
			CHECK(context.OutputImageArrays.empty());
		}
		CHECK(source.Images.front() == image);
	}
}
TEST_CASE("Rearrange direct Orders packed roots matrix and surface getter", "[imagegraph][array_rearrange]") {
	const ArrayValue source{ValueType::Integer, {int64_t{10}, int64_t{20}}};
	for (const Value &orders : std::vector<Value>{Vector2{1, 0}, MatrixValue{2, 1, {1, 0}}})
		CHECK(
			Content(RearrangeOutputArray(
				RunNode("pc.array_rearrange", {}, {{"array", source}, {"orders", orders}})
			)) == Items{{ElementValue{int64_t{20}}}, {ElementValue{int64_t{10}}}}
		);
	const Node node{"rearrange", "pc.array_rearrange", "", {}, {}};
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	const auto executor = detail::FindExecutor(node.Type);
	REQUIRE(executor);
	const EvaluationRequest request;
	const Image dimensions{1, 2, std::vector<uint8_t>(8, 255), 17};
	for (bool arrayRoute : {false, true}) {
		const ImageArray surfaces{{dimensions}, {{size_t{0}}}};
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		context.Values = {{"array", source}};
		if (arrayRoute)
			context.ImageArrays = {{"orders", &surfaces}};
		else
			context.Images = {{"orders", &dimensions}};
		REQUIRE(executor(context));
		REQUIRE(context.OutputValues.size() == 1);
		CHECK(
			Content(std::get<ArrayValue>(context.OutputValues[0].Data)) ==
			(arrayRoute ? Items{{ElementValue{int64_t{20}}}, {ElementValue{int64_t{20}}}}
						: Items{{ElementValue{int64_t{20}}}, Number(0)})
		);
	}
}
TEST_CASE("Rearrange named unsupported controls never publish carriers", "[imagegraph][array_rearrange]") {
	const Value source = ArrayValue{ValueType::Integer, {int64_t{10}, int64_t{20}}};
	const std::vector<std::pair<Value, std::string>> controls{
		{std::string{"1"}, "source rearrange Orders text grammar is unrepresented"},
		{Quaternion{1, 0, 0, 0}, "source rearrange Orders quaternion getter is unrepresented"},
		{General({Row({Number(1)}), Number(0)}), "source rearrange Orders comparison is unrepresented"},
		{General({Row({}), Row({Number(0)})}), "source rearrange Orders comparison is unrepresented"},
		{General({Number(1), Row({Number(0)})}), "source rearrange Orders comparison is unrepresented"},
		{ArrayValue{ValueType::Text, {std::string{"1"}, std::string{"0"}}},
		 "source rearrange Orders conversion is unrepresented"}
	};
	for (const auto &[orders, message] : controls) {
		const auto run = RunNode("pc.array_rearrange", {}, {{"array", source}, {"orders", orders}});
		INFO(run.Message);
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::UnsupportedExecution);
		CHECK(run.Port == "orders");
		CHECK(run.Message == message);
		CHECK(run.Values.empty());
		CHECK(run.Images.empty());
	}
	for (const Value &value :
		 std::vector<Value>{int64_t{7}, std::string{"array"}, MatrixValue{2, 1, {1, 0}}}) {
		const auto run = RunNode("pc.array_rearrange", {}, {{"array", value}});
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::UnsupportedExecution);
		CHECK(run.Port == "array");
		CHECK(run.Message == "source rearrange non-array return is history-dependent");
		CHECK(run.Values.empty());
		CHECK(run.Images.empty());
	}
}
TEST_CASE(
	"Rearrange graph delivers direct and typed image-array Orders narrowly", "[imagegraph][array_rearrange]"
) {
	RequestImageSource image{"surface", Image{1, 2, std::vector<uint8_t>(8, 255), 23}};
	const ArrayValue source{ValueType::Integer, {int64_t{10}, int64_t{20}}};
	for (bool typedArray : {false, true}) {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"source",
			 "pc.array",
			 "",
			 {},
			 {{"type", EnumValue{0}}, {"spread_array", true}},
			 {{"value_0", ValueType::Array, Value{source}}}},
			{"surface", "image.captured", "", {}, {{"source_id", std::string{"surface"}}}},
			{"rearrange", "pc.array_rearrange", "", {}, {}},
			{"capture", "pc.array_copy", "", {}, {}}
		};
		document.Links = {
			{"source", "array", "rearrange", "array"}, {"rearrange", "array", "capture", "array"}
		};
		if (typedArray) {
			document.Nodes.push_back(
				{"orders",
				 "pc.array",
				 "",
				 {},
				 {{"type", EnumValue{1}}, {"spread_array", false}},
				 {{"value_0", ValueType::Image, std::nullopt}}}
			);
			document.Links.push_back({"surface", "image", "orders", "value_0"});
			document.Links.push_back({"orders", "array", "rearrange", "orders"});
		} else
			document.Links.push_back({"surface", "image", "rearrange", "orders"});
		document.Outputs = {{"out", "capture", "array"}};
		Plan plan;
		Diagnostic diagnostic;
		const auto compiled = Compile(document, plan, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << " " << diagnostic.Message);
		REQUIRE(compiled == Status::Ok);
		EvaluationRequest request;
		request.ImageSources = std::span<const RequestImageSource>(&image, 1);
		EvaluationSnapshot snapshot;
		const auto status = EvaluateNodeInputs(document, plan, "capture", request, snapshot, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << " " << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		bool found = false;
		for (const auto &value : snapshot.Values())
			if (value.Port == "array") {
				CHECK(
					Content(std::get<ArrayValue>(value.Data)) ==
					(typedArray ? Items{{ElementValue{int64_t{20}}}, {ElementValue{int64_t{20}}}}
								: Items{{ElementValue{int64_t{20}}}, Number(0)})
				);
				found = true;
			}
		CHECK(found);
	}
}
TEST_CASE("Rearrange selected depth includes packed and empty wrappers", "[imagegraph][array_rearrange]") {
	const auto nested = [](SourceArrayItem member, size_t wrappers) {
		for (size_t i = 0; i < wrappers; ++i)
			member = Row({std::move(member)});
		return General({std::move(member)});
	};
	for (const auto &member : Items{{ElementValue{Vector2{2, 9}}}, Row({})}) {
		const auto fits = nested(member, 62);
		const auto run = RunNode("pc.array_rearrange", {}, {{"array", fits}});
		INFO(run.Message);
		REQUIRE(run.Ok);
		const auto exceeds = nested(member, 63);
		const auto bad = RunNode("pc.array_rearrange", {}, {{"array", exceeds}});
		CHECK_FALSE(bad.Ok);
		const bool packedExpansion = std::holds_alternative<ElementValue>(member.Data);
		CHECK(bad.Code == (packedExpansion ? Status::LimitExceeded : Status::InvalidValue));
		CHECK(bad.Port == "array");
		CHECK(bad.Values.empty());
	}
}
TEST_CASE(
	"Rearrange borrowed spare capacities do not replace logical copied cost", "[imagegraph][array_rearrange]"
) {
	std::string text = "small";
	text.reserve(Limits::MaximumArrayBytes + 1024);
	MatrixValue matrix{2, 1, {3, 7}};
	matrix.Values.reserve(Limits::MaximumArrayBytes / sizeof(double) + 1024);
	ArrayValue array{ValueType::Any, {}};
	array.Items.push_back({ElementValue{std::move(text)}});
	array.Items.push_back({ElementValue{std::move(matrix)}});
	const Value source = std::move(array);
	const auto &items = std::get<ArrayValue>(source).Items;
	const auto textCapacity = std::get<std::string>(std::get<ElementValue>(items[0].Data)).capacity();
	const auto matrixCapacity =
		std::get<MatrixValue>(std::get<ElementValue>(items[1].Data)).Values.capacity();
	const Value before = source;
	const Node node{"rearrange", "pc.array_rearrange", "", {}, {}};
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	const auto executor = detail::FindExecutor(node.Type);
	REQUIRE(executor);
	const EvaluationRequest request;
	detail::EvaluationBudget budget(1024 * 1024);
	auto unrelated = budget.Reserve(512);
	REQUIRE(unrelated);
	{
		detail::NodeContext context(node, *entry, request, budget);
		context.ByteBudget = 1024 * 1024;
		context.ValueViews = {{"array", &source}};
		REQUIRE(executor(context));
		REQUIRE(context.OutputValues.size() == 1);
		CHECK(Content(std::get<ArrayValue>(context.OutputValues[0].Data)) == items);
		CHECK(budget.Peak() < 1024 * 1024);
	}
	CHECK(budget.Used() == 512);
	CHECK(source == before);
	CHECK(std::get<std::string>(std::get<ElementValue>(items[0].Data)).capacity() == textCapacity);
	CHECK(std::get<MatrixValue>(std::get<ElementValue>(items[1].Data)).Values.capacity() == matrixCapacity);
}
TEST_CASE(
	"Rearrange linked Matrix and packed Orders execute before capture", "[imagegraph][array_rearrange]"
) {
	const ArrayValue source{ValueType::Integer, {int64_t{10}, int64_t{20}}};
	for (bool matrix : {false, true}) {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"source",
			 "pc.array",
			 "",
			 {},
			 {{"type", EnumValue{0}}, {"spread_array", true}},
			 {{"value_0", ValueType::Array, Value{source}}}},
			{"rearrange", "pc.array_rearrange", "", {}, {}},
			{"capture", "pc.array_copy", "", {}, {}}
		};
		if (matrix)
			document.Nodes.push_back(
				{"orders",
				 "pc.matrix",
				 "",
				 {},
				 {{"size", Vector2{2, 1}}, {"data", MatrixValue{2, 1, {1, 0}}}}}
			);
		else
			document.Nodes.push_back(
				{"orders", "pc.vector2", "", {}, {{"x", 1.0}, {"y", 0.0}, {"integer", false}}}
			);
		document.Links = {
			{"source", "array", "rearrange", "array"},
			{"orders", matrix ? "matrix" : "vector", "rearrange", "orders"},
			{"rearrange", "array", "capture", "array"}
		};
		document.Outputs = {{"out", "capture", "array"}};
		Plan plan;
		Diagnostic diagnostic;
		const auto compiled = Compile(document, plan, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << " " << diagnostic.Message);
		REQUIRE(compiled == Status::Ok);
		EvaluationSnapshot snapshot;
		const auto status = EvaluateNodeInputs(document, plan, "capture", {}, snapshot, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << " " << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		bool found = false;
		for (const auto &value : snapshot.Values())
			if (value.Port == "array") {
				CHECK(
					Content(std::get<ArrayValue>(value.Data)) ==
					Items{{ElementValue{int64_t{20}}}, {ElementValue{int64_t{10}}}}
				);
				found = true;
			}
		CHECK(found);
	}
}
TEST_CASE(
	"Rearrange wrong-length valid nested Orders precede comparison and save authored values",
	"[imagegraph][array_rearrange]"
) {
	const ArrayValue source{ValueType::Integer, {int64_t{10}, int64_t{20}}};
	Curve curve;
	curve.Anchors.resize(256);
	const Value orders =
		General({Row({{ElementValue{curve}}, {ElementValue{curve}}, {ElementValue{curve}}})});
	const auto run = RunNode("pc.array_rearrange", {}, {{"array", source}, {"orders", orders}});
	CHECK(Content(RearrangeOutputArray(run)) == Content(source));
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"rearrange",
		 "pc.array_rearrange",
		 "",
		 {},
		 {{"orders", ArrayValue{ValueType::Integer, {int64_t{1}, int64_t{0}}}}}}
	};
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	REQUIRE(restored.Nodes.size() == 1);
	CHECK(restored.Nodes[0].Values == document.Nodes[0].Values);
}
TEST_CASE(
	"Rearrange empty image category and image plus zero mixed publication", "[imagegraph][array_rearrange]"
) {
	const Node node{"rearrange", "pc.array_rearrange", "", {}, {}};
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	const auto executor = detail::FindExecutor(node.Type);
	REQUIRE(executor);
	const EvaluationRequest request;
	const Image image{1, 1, {1, 2, 3, 255}, 19};
	const ImageArray empty{{image}, {}};
	{
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		context.ImageArrays = {{"array", &empty}};
		REQUIRE(executor(context));
		REQUIRE(context.OutputImageArrays.size() == 1);
		CHECK(context.OutputImageArrays[0].second.Items.empty());
		CHECK(context.OutputValues.empty());
	}
	const ImageArray source{{image}, {{size_t{0}}, {size_t{0}}}};
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.ImageArrays = {{"array", &source}};
	context.Values = {{"orders", ArrayValue{ValueType::Integer, {int64_t{0}, int64_t{-1}}}}};
	REQUIRE(executor(context));
	REQUIRE(context.OutputValues.size() == 1);
	CHECK(context.OutputImageArrays.empty());
	CHECK(Content(std::get<ArrayValue>(context.OutputValues[0].Data)) == Items{{image}, Number(0)});
}
