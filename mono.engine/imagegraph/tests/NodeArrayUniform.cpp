#include "../src/ValuePayload.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.imagegraph.node_array_uniform")
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
	const ArrayValue &UniformOutputArray(const NodeRun &run) {
		INFO(run.Message);
		REQUIRE(run.Ok);
		REQUIRE(run.OutputValue("array_out"));
		return std::get<ArrayValue>(*run.OutputValue("array_out"));
	}
	// Decode only the lossless publication carriers; expected source shapes are authored independently.
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
	void Rejected(const NodeRun &run, Status status, std::string_view port) {
		INFO(run.Message);
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == status);
		CHECK(run.Port == port);
		const std::string expected = status == Status::InvalidValue ? "source uniform length is negative"
									 : status == Status::UnsupportedExecution
										 ? "source uniform noone default is unavailable"
									 : port == "length" ? "source uniform length exceeds bounds"
														: "source uniform output exceeds bounds";
		CHECK(run.Message == expected);
		CHECK(run.Values.empty());
		CHECK(run.Images.empty());
	}
	ArrayValue Chain(size_t depth) {
		SourceArrayItem member = Number(17);
		for (size_t level = 1; level < depth; ++level)
			member = Row({std::move(member)});
		return General({std::move(member)});
	}
}
TEST_CASE(
	"Uniform repeats complete scalar values and preserves empty categories", "[imagegraph][array_uniform]"
) {
	const std::vector<std::pair<Value, ElementValue>> fixtures{
		{int64_t{9007199254740993LL}, int64_t{9007199254740993LL}},
		{2.75, 2.75},
		{std::string{"whole text"}, std::string{"whole text"}},
		{true, true},
		{EnumValue{7}, EnumValue{7}},
		{Colour{1, 2, 3, 4}, Colour{1, 2, 3, 4}},
		{MatrixValue{2, 2, {1, 2, 3, 4}}, MatrixValue{2, 2, {1, 2, 3, 4}}}
	};
	for (const auto &[data, expected] : fixtures) {
		CHECK(
			Content(UniformOutputArray(RunNode("pc.array_uniform", {}, {{"data", data}}))) ==
			Items{{expected}}
		);
		for (int64_t length : {int64_t{0}, int64_t{1}, int64_t{3}}) {
			const auto run = RunNode("pc.array_uniform", {}, {{"data", data}, {"length", length}});
			const auto &output = UniformOutputArray(run);
			CHECK(Content(output) == Items(size_t(length), SourceArrayItem{expected}));
			if (length == 0) CHECK(output.ElementType == detail::PayloadType(data));
		}
	}
}
TEST_CASE(
	"Uniform packed tuples and curves repeat complete source component rows", "[imagegraph][array_uniform]"
) {
	const Curve curve{{1, 2, 3, 4, 5, 6}, {{{7, 8, 9, 10, 11, 12}}, {{13, 14, 15, 16, 17, 18}}}};
	Items fields;
	for (double field = 1; field <= 18; ++field)
		fields.push_back(Number(field));
	CHECK(
		Content(
			UniformOutputArray(RunNode("pc.array_uniform", {}, {{"data", curve}, {"length", int64_t{2}}}))
		) == Items{Row(fields), Row(fields)}
	);
	CHECK(
		Content(UniformOutputArray(
			RunNode("pc.array_uniform", {}, {{"data", Vector2{2, 9}}, {"length", int64_t{2}}})
		)) == Items{Row({Number(2), Number(9)}), Row({Number(2), Number(9)})}
	);
	const ArrayValue packed{ValueType::Vector2, {Vector2{2, 9}, Vector2{4, 8}}};
	const Items source{Row({Number(2), Number(9)}), Row({Number(4), Number(8)})};
	CHECK(
		Content(
			UniformOutputArray(RunNode("pc.array_uniform", {}, {{"data", packed}, {"length", int64_t{2}}}))
		) == Items{Row(source), Row(source)}
	);
}
TEST_CASE("Uniform recursive mixed copies own every member independently", "[imagegraph][array_uniform]") {
	const Image image{1, 1, {1, 2, 3, 255}, 19};
	const Items source{
		{ElementValue{std::string{"keep"}}},
		{ElementValue{int64_t{9007199254740993LL}}},
		Row({Number(5), Row({})}),
		{image}
	};
	const Value input = General(source);
	const Value before = input;
	const Node node{"uniform", "pc.array_uniform", "", {}, {}};
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	const auto executor = detail::FindExecutor(node.Type);
	REQUIRE(executor);
	const EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.ValueViews = {{"data", &input}};
	context.Values = {{"length", int64_t{3}}};
	REQUIRE(executor(context));
	REQUIRE(context.OutputValues.size() == 1);
	auto &output = std::get<ArrayValue>(context.OutputValues.front().Data);
	CHECK(Content(output) == Items{Row(source), Row(source), Row(source)});
	REQUIRE(output.Items.size() == 3);
	auto &first = std::get<Items>(output.Items.front().Data);
	std::get<std::string>(std::get<ElementValue>(first[0].Data)) = "changed";
	std::get<Image>(first[3].Data).Pixels[0] = 99;
	CHECK(std::get<Items>(output.Items[1].Data) == source);
	CHECK(std::get<Items>(output.Items[2].Data) == source);
	CHECK(input == before);
}
TEST_CASE(
	"Uniform empty array members retain wrappers and their count boundary", "[imagegraph][array_uniform]"
) {
	for (const auto type : {ValueType::Integer, ValueType::Text, ValueType::Any}) {
		const ArrayValue empty{type, {}};
		const auto zero = RunNode("pc.array_uniform", {}, {{"data", empty}, {"length", int64_t{0}}});
		CHECK(UniformOutputArray(zero).ElementType == type);
		CHECK(Content(UniformOutputArray(zero)).empty());
		for (int64_t length : {int64_t{1}, int64_t{4096}}) {
			const auto run = RunNode("pc.array_uniform", {}, {{"data", empty}, {"length", length}});
			CHECK(Content(UniformOutputArray(run)) == Items(size_t(length), Row({})));
			CHECK(UniformOutputArray(run).ElementType == type);
			CHECK(detail::ValidValuePayload(*run.OutputValue("array_out"), true));
			if (type == ValueType::Any)
				CHECK(UniformOutputArray(run).Items.size() == size_t(length));
			else
				CHECK(UniformOutputArray(run).Nested.size() == size_t(length));
		}
	}
	Rejected(
		RunNode(
			"pc.array_uniform", {}, {{"data", ArrayValue{ValueType::Integer, {}}}, {"length", int64_t{4097}}}
		),
		Status::LimitExceeded,
		"length"
	);
}
TEST_CASE("Uniform admits exact recursive count and depth boundaries", "[imagegraph][array_uniform]") {
	const auto depth64 = RunNode("pc.array_uniform", {}, {{"data", Chain(63)}, {"length", int64_t{1}}});
	CHECK(Content(UniformOutputArray(depth64)) == Items{Row(Chain(63).Items)});
	Rejected(
		RunNode("pc.array_uniform", {}, {{"data", Chain(64)}, {"length", int64_t{1}}}),
		Status::LimitExceeded,
		"array_out"
	);
	const ArrayValue exact{ValueType::Integer, std::vector<ElementValue>(2047, int64_t{7})};
	const auto fits = RunNode("pc.array_uniform", {}, {{"data", exact}, {"length", int64_t{2}}});
	CHECK(Content(UniformOutputArray(fits)) == Items{Row(Content(exact)), Row(Content(exact))});
	const ArrayValue excessive{ValueType::Integer, std::vector<ElementValue>(2048, int64_t{7})};
	Rejected(
		RunNode("pc.array_uniform", {}, {{"data", excessive}, {"length", int64_t{2}}}),
		Status::LimitExceeded,
		"array_out"
	);
}
TEST_CASE("Uniform zero length avoids expanding a valid large packed input", "[imagegraph][array_uniform]") {
	Curve curve;
	curve.Anchors.resize(256);
	const Value input = ArrayValue{ValueType::Curve, std::vector<ElementValue>(64, curve)};
	const Value before = input;
	const auto run = RunNode("pc.array_uniform", {}, {{"data", input}, {"length", int64_t{0}}});
	CHECK(UniformOutputArray(run).ElementType == ValueType::Curve);
	CHECK(Content(UniformOutputArray(run)).empty());
	CHECK(input == before);
	Rejected(
		RunNode("pc.array_uniform", {}, {{"data", input}, {"length", int64_t{1}}}),
		Status::LimitExceeded,
		"array_out"
	);
}
TEST_CASE(
	"Uniform rejects invalid resolved lengths and the unavailable source noone default",
	"[imagegraph][array_uniform]"
) {
	for (const auto length : {int64_t{-1}, std::numeric_limits<int64_t>::min()})
		Rejected(
			RunNode("pc.array_uniform", {}, {{"data", int64_t{5}}, {"length", length}}),
			Status::InvalidValue,
			"length"
		);
	Rejected(
		RunNode(
			"pc.array_uniform", {}, {{"data", int64_t{5}}, {"length", std::numeric_limits<int64_t>::max()}}
		),
		Status::LimitExceeded,
		"length"
	);
	for (const Value &length : std::vector<Value>{
			 2.5,
			 std::numeric_limits<double>::infinity(),
			 std::string{"2"},
			 ArrayValue{ValueType::Integer, {int64_t{2}}}
		 }) {
		const auto run = RunNode("pc.array_uniform", {}, {{"data", int64_t{5}}, {"length", length}});
		CHECK_FALSE(run.Ok);
		CHECK(run.Port == "length");
		CHECK(run.Code == Status::UnsupportedExecution);
		CHECK(run.Message == "source uniform length requires one resolved integer");
		CHECK(run.Values.empty());
		CHECK(run.Images.empty());
	}
	Rejected(RunNode("pc.array_uniform", {}, {}), Status::UnsupportedExecution, "data");
	const auto empty = RunNode("pc.array_uniform", {}, {{"length", int64_t{0}}});
	CHECK(UniformOutputArray(empty).ElementType == ValueType::Any);
	CHECK(Content(UniformOutputArray(empty)).empty());
}
TEST_CASE("Uniform surface copies preserve HDR bytes and image-array shape", "[imagegraph][array_uniform]") {
	const Node node{"uniform", "pc.array_uniform", "", {}, {}};
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	const auto executor = detail::FindExecutor(node.Type);
	REQUIRE(executor);
	const EvaluationRequest request;
	Image image{1, 1, std::vector<uint8_t>(16, 0), 31, SurfaceFormat::RGBA32Float};
	REQUIRE(StoreSurfacePixel(image, 0, 0, {2.0, -0.5, 0.25, 1.0}));
	for (const int64_t length : {int64_t{0}, int64_t{3}}) {
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		context.Images = {{"data", &image}};
		context.Values = {{"length", length}};
		REQUIRE(executor(context));
		REQUIRE(context.OutputImageArrays.size() == 1);
		auto &output = context.OutputImageArrays.front().second;
		REQUIRE(output.Images.size() == size_t(length));
		CHECK(output.Items.size() == size_t(length));
		for (size_t index = 0; index < output.Images.size(); ++index) {
			CHECK(output.Images[index] == image);
			CHECK(output.Items[index].Data == std::variant<size_t, std::vector<ImageArrayItem>>{index});
		}
		if (length) {
			output.Images.front().Pixels.front() = 99;
			CHECK(output.Images[1] == image);
			CHECK(image.Pixels.front() == 0);
		}
	}
	const Image second{1, 1, {4, 5, 6, 255}, 42};
	const ImageArray input{{image, second}, {{size_t{1}}, {std::vector<ImageArrayItem>{{size_t{0}}}}}};
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.ImageArrays = {{"data", &input}};
	context.Values = {{"length", int64_t{2}}};
	REQUIRE(executor(context));
	REQUIRE(context.OutputValues.size() == 1);
	auto &output = std::get<ArrayValue>(context.OutputValues.front().Data);
	const Items expected{{second}, Row({{image}})};
	CHECK(Content(output) == Items{Row(expected), Row(expected)});
	REQUIRE(output.Items.size() == 2);
	auto &firstRow = std::get<Items>(output.Items[0].Data);
	std::get<Image>(firstRow[0].Data).Pixels[0] = 99;
	CHECK(std::get<Items>(output.Items[1].Data) == expected);
	CHECK(input.Images[1] == second);
}
TEST_CASE(
	"Uniform image byte limits are checked with recursive wrapper overhead", "[imagegraph][array_uniform]"
) {
	const Image image{512, 512, std::vector<uint8_t>(512 * 512 * 4, 7), 21};
	const Node node{"uniform", "pc.array_uniform", "", {}, {}};
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	const auto executor = detail::FindExecutor(node.Type);
	REQUIRE(executor);
	const EvaluationRequest request;
	for (int64_t length : {int64_t{3}, int64_t{4}}) {
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		context.Images = {{"data", &image}};
		context.Values = {{"length", length}};
		const bool ok = executor(context);
		if (length == 3) {
			REQUIRE(ok);
			REQUIRE(context.OutputImageArrays.size() == 1);
			REQUIRE(context.OutputImageArrays.front().second.Images.size() == 3);
			for (const auto &copy : context.OutputImageArrays.front().second.Images)
				CHECK(copy == image);
		} else {
			CHECK_FALSE(ok);
			CHECK(context.FailureCode == Status::LimitExceeded);
			CHECK(context.FailurePort == "array_out");
			CHECK(context.OutputValues.empty());
			CHECK(context.OutputImages.empty());
			CHECK(context.OutputImageArrays.empty());
		}
	}
}
TEST_CASE(
	"Uniform live ledger retains output ownership until its owner dies", "[imagegraph][array_uniform]"
) {
	const Node node{"uniform", "pc.array_uniform", "", {}, {}};
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	const auto executor = detail::FindExecutor(node.Type);
	REQUIRE(executor);
	const Value input = General({{ElementValue{std::string(8192, 'x')}}, Row({Number(3), Row({})})});
	const Value before = input;
	const EvaluationRequest request;
	uint64_t peak = 0;
	for (size_t attempt = 0; attempt < 3; ++attempt) {
		const uint64_t limit = attempt == 0 ? Limits::MaximumEvaluationBytes : attempt == 1 ? peak - 1 : peak;
		detail::EvaluationBudget budget(limit);
		auto unrelated = budget.Reserve(512);
		REQUIRE(unrelated);
		{
			detail::AllocationReservation outputOwner;
			std::vector<AuthoredValue> ownedOutputs;
			{
				detail::NodeContext context(node, *entry, request, budget);
				context.ByteBudget = limit;
				context.ValueViews = {{"data", &input}};
				context.Values = {{"length", int64_t{3}}};
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
						Content(std::get<ArrayValue>(context.OutputValues.front().Data)) ==
						Items(3, Row(std::get<ArrayValue>(input).Items))
					);
					ownedOutputs = std::move(context.OutputValues);
					outputOwner = context.TakeOutputReservation();
				}
				CHECK(budget.Peak() <= limit);
			}
			if (attempt != 1) CHECK(budget.Used() > 512);
		}
		CHECK(budget.Used() == 512);
		CHECK(input == before);
	}
}
TEST_CASE(
	"Uniform runs upstream and linked integer getters round half to even", "[imagegraph][array_uniform]"
) {
	const ArrayValue source = General({{ElementValue{std::string{"whole"}}}, Row({Number(4), Row({})})});
	for (const auto &[fractional, expectedLength] :
		 std::vector<std::pair<double, int64_t>>{{0.5, 0}, {2.5, 2}, {3.5, 4}, {-0.5, 0}, {-1.5, -2}}) {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"source",
			 "pc.array",
			 "",
			 {},
			 {{"type", EnumValue{0}}, {"spread_array", true}},
			 {{"value_0", ValueType::Array, Value{source}}}},
			{"length", "pc.number_simple", "", {}, {{"value", fractional}}},
			{"uniform", "pc.array_uniform", "", {}, {}},
			{"capture", "pc.array_copy", "", {}, {}}
		};
		document.Links = {
			{"source", "array", "uniform", "data"},
			{"length", "number", "uniform", "length"},
			{"uniform", "array_out", "capture", "array"}
		};
		document.Outputs = {{"out", "capture", "array"}};
		Plan plan;
		Diagnostic diagnostic;
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		EvaluationSnapshot snapshot;
		const auto status = EvaluateNodeInputs(document, plan, "capture", {}, snapshot, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << " " << diagnostic.Message);
		if (expectedLength < 0) {
			CHECK(status == Status::InvalidValue);
			CHECK(diagnostic.NodeId == "uniform");
			CHECK(diagnostic.Port == "length");
			CHECK(snapshot.Values().empty());
		} else {
			REQUIRE(status == Status::Ok);
			bool found = false;
			for (const auto &value : snapshot.Values())
				if (value.Port == "array") {
					CHECK(
						Content(std::get<ArrayValue>(value.Data)) ==
						Items(size_t(expectedLength), Row(source.Items))
					);
					found = true;
				}
			CHECK(found);
		}
	}
}
TEST_CASE(
	"Uniform borrowed preflight includes packed and empty branch depths", "[imagegraph][array_uniform]"
) {
	const auto nested = [](SourceArrayItem member, size_t wrappers) {
		for (size_t level = 0; level < wrappers; ++level)
			member = Row({std::move(member)});
		return General({std::move(member)});
	};
	const auto packedFits = nested({ElementValue{Vector2{2, 9}}}, 61);
	CHECK(
		Content(
			UniformOutputArray(
				RunNode("pc.array_uniform", {}, {{"data", packedFits}, {"length", int64_t{1}}})
			)
		).size() == 1
	);
	Rejected(
		RunNode(
			"pc.array_uniform",
			{},
			{{"data", nested({ElementValue{Vector2{2, 9}}}, 62)}, {"length", int64_t{1}}}
		),
		Status::LimitExceeded,
		"array_out"
	);
	const auto emptyFits = nested(Row({}), 61);
	CHECK(
		Content(
			UniformOutputArray(RunNode("pc.array_uniform", {}, {{"data", emptyFits}, {"length", int64_t{1}}}))
		) == Items{Row(emptyFits.Items)}
	);
	Rejected(
		RunNode("pc.array_uniform", {}, {{"data", nested(Row({}), 62)}, {"length", int64_t{1}}}),
		Status::LimitExceeded,
		"array_out"
	);
}
TEST_CASE(
	"Uniform validates every surface pool entry even when producing no members", "[imagegraph][array_uniform]"
) {
	const Node node{"uniform", "pc.array_uniform", "", {}, {}};
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	const auto executor = detail::FindExecutor(node.Type);
	REQUIRE(executor);
	const EvaluationRequest request;
	const Image image{1, 1, {1, 2, 3, 255}, 9};
	const ImageArray duplicated{{image}, {{size_t{0}}, {size_t{0}}}};
	{
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		context.ImageArrays = {{"data", &duplicated}};
		context.Values = {{"length", int64_t{2}}};
		REQUIRE(executor(context));
		REQUIRE(context.OutputImageArrays.size() == 1);
		auto &output = context.OutputImageArrays.front().second;
		REQUIRE(output.Images.size() == 4);
		for (const auto &copy : output.Images)
			CHECK(copy == image);
		output.Images[0].Pixels[0] = 99;
		CHECK(output.Images[1] == image);
		CHECK(output.Images[2] == image);
		CHECK(output.Images[3] == image);
		CHECK(duplicated.Images.front() == image);
	}
	const std::vector<ImageArray> invalid{
		{{image, Image{1, 1, {1, 2, 3}, 0}}, {{size_t{0}}}},
		{{image}, {{size_t{1}}}},
		{{Image{1, 1, {0, 0, 128, 127}, 0, SurfaceFormat::R32Float}}, {{size_t{0}}}}
	};
	for (size_t invalidIndex = 0; invalidIndex < invalid.size(); ++invalidIndex) {
		const auto &input = invalid[invalidIndex];
		for (int64_t length : {int64_t{0}, int64_t{1}}) {
			detail::NodeContext context(node, *entry, request);
			context.ByteBudget = Limits::MaximumEvaluationBytes;
			context.ImageArrays = {{"data", &input}};
			context.Values = {{"length", length}};
			CHECK_FALSE(executor(context));
			CHECK(context.FailureCode == Status::InvalidValue);
			CHECK(context.FailurePort == "data");
			CHECK(
				context.FailureMessage == (invalidIndex == 1   ? "source uniform image shape is invalid"
										   : invalidIndex == 2 ? "source uniform image samples are nonfinite"
															   : "input surface layout is invalid")
			);
			CHECK(context.OutputValues.empty());
			CHECK(context.OutputImages.empty());
			CHECK(context.OutputImageArrays.empty());
		}
	}
}
TEST_CASE(
	"Uniform image transport executes between a captured producer and distinct consumer",
	"[imagegraph][array_uniform]"
) {
	RequestImageSource source{
		"surface", Image{1, 1, std::vector<uint8_t>(16), 23, SurfaceFormat::RGBA32Float}
	};
	REQUIRE(StoreSurfacePixel(source.Data, 0, 0, {-2, 4, .25, 1}));
	const Image before = source.Data;
	for (int64_t length : {int64_t{0}, int64_t{2}}) {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"source", "image.captured", "", {}, {{"source_id", std::string{"surface"}}}},
			{"uniform", "pc.array_uniform", "", {}, {{"length", length}}},
			{"capture", length ? "pc.array_reverse" : "pc.array_length", "", {}, {}}
		};
		document.Links = {
			{"source", "image", "uniform", "data"}, {"uniform", "array_out", "capture", "array"}
		};
		document.Outputs = {
			{"out", "capture", length ? "array" : "size"}, {"uniform_out", "uniform", "array_out"}
		};
		Plan plan;
		Diagnostic diagnostic;
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		EvaluationRequest request;
		request.ImageSources = std::span<const RequestImageSource>(&source, 1);
		ImageArray output;
		if (!length) {
			EvaluatedValue size;
			REQUIRE(EvaluateValue(document, plan, "out", request, size, diagnostic) == Status::Ok);
			CHECK(size.Data == Value{int64_t{0}});
		}
		const auto status =
			EvaluateArray(document, plan, length ? "out" : "uniform_out", request, output, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << " " << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		REQUIRE(output.Images.size() == size_t(length));
		CHECK(output.Items.size() == size_t(length));
		for (const auto &image : output.Images) {
			CHECK(image.Width == before.Width);
			CHECK(image.Height == before.Height);
			CHECK(image.Pixels == before.Pixels);
			CHECK(image.Format == before.Format);
		}
		if (length) {
			output.Images.front().Pixels.front() = 99;
			CHECK(output.Images.back().Pixels == before.Pixels);
		}
		CHECK(source.Data == before);
	}
}
TEST_CASE("Uniform direct failures publish no carrier collection", "[imagegraph][array_uniform]") {
	const Node node{"uniform", "pc.array_uniform", "", {}, {}};
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	const auto executor = detail::FindExecutor(node.Type);
	REQUIRE(executor);
	const EvaluationRequest request;
	for (const Value &length : std::vector<Value>{
			 int64_t{-1},
			 int64_t{4097},
			 2.5,
			 std::numeric_limits<double>::quiet_NaN(),
			 std::numeric_limits<double>::max(),
			 ArrayValue{ValueType::Integer, {int64_t{2}}}
		 }) {
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		context.Values = {{"data", int64_t{7}}, {"length", length}};
		CHECK_FALSE(executor(context));
		CHECK(context.FailurePort == "length");
		CHECK_FALSE(context.FailureMessage.empty());
		CHECK(context.OutputValues.empty());
		CHECK(context.OutputImages.empty());
		CHECK(context.OutputImageArrays.empty());
	}
	const Value malformed = ArrayValue{ValueType::Integer, {std::string{"wrong"}}};
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.ValueViews = {{"data", &malformed}};
	context.Values = {{"length", int64_t{0}}};
	CHECK_FALSE(executor(context));
	CHECK(context.FailureCode == Status::InvalidValue);
	CHECK(context.FailurePort == "data");
	CHECK(context.FailureMessage == "source uniform data is invalid");
	CHECK(context.OutputValues.empty());
	CHECK(context.OutputImages.empty());
	CHECK(context.OutputImageArrays.empty());
}
TEST_CASE(
	"Uniform copies logical payload without charging borrowed spare capacity", "[imagegraph][array_uniform]"
) {
	std::string text = "small";
	text.reserve(Limits::MaximumArrayBytes + 1024);
	MatrixValue matrix{2, 1, {3, 7}};
	matrix.Values.reserve(Limits::MaximumArrayBytes / sizeof(double) + 1024);
	const std::vector<Value> inputs = [&] {
		std::vector<Value> values;
		values.emplace_back(std::move(text));
		values.emplace_back(std::move(matrix));
		return values;
	}();
	const Node node{"uniform", "pc.array_uniform", "", {}, {}};
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	const auto executor = detail::FindExecutor(node.Type);
	REQUIRE(executor);
	const EvaluationRequest request;
	for (const Value &input : inputs) {
		const Value before = input;
		const size_t capacity = std::holds_alternative<std::string>(input)
									? std::get<std::string>(input).capacity()
									: std::get<MatrixValue>(input).Values.capacity();
		detail::EvaluationBudget budget(1024 * 1024);
		auto unrelated = budget.Reserve(512);
		REQUIRE(unrelated);
		{
			detail::NodeContext context(node, *entry, request, budget);
			context.ByteBudget = 1024 * 1024;
			context.ValueViews = {{"data", &input}};
			context.Values = {{"length", int64_t{3}}};
			REQUIRE(executor(context));
			REQUIRE(context.OutputValues.size() == 1);
			const ElementValue expected = std::holds_alternative<std::string>(input)
											  ? ElementValue{std::get<std::string>(input)}
											  : ElementValue{std::get<MatrixValue>(input)};
			CHECK(
				Content(std::get<ArrayValue>(context.OutputValues.front().Data)) ==
				Items(3, SourceArrayItem{expected})
			);
			CHECK(budget.Peak() < 1024 * 1024);
		}
		CHECK(budget.Used() == 512);
		CHECK(input == before);
		CHECK(
			capacity == (std::holds_alternative<std::string>(input)
							 ? std::get<std::string>(input).capacity()
							 : std::get<MatrixValue>(input).Values.capacity())
		);
	}
}
TEST_CASE(
	"Uniform expands every remaining packed type in source field order", "[imagegraph][array_uniform]"
) {
	const std::vector<std::pair<Value, Items>> fixtures{
		{Vector3{2, 4, 8}, {Number(2), Number(4), Number(8)}},
		{Vector4{2, 4, 8, 16}, {Number(2), Number(4), Number(8), Number(16)}},
		{Quaternion{3, 5, 7, 11}, {Number(3), Number(5), Number(7), Number(11)}},
		{Area{2, 4, 8, 16, 1, 2}, {Number(2), Number(4), Number(8), Number(16), Number(1), Number(2)}}
	};
	for (const auto &[input, expected] : fixtures) {
		const Value before = input;
		const auto run = RunNode("pc.array_uniform", {}, {{"data", input}, {"length", int64_t{3}}});
		CHECK(Content(UniformOutputArray(run)) == Items(3, Row(expected)));
		CHECK(input == before);
	}
}
TEST_CASE(
	"Uniform remaining owned leaves preserve data and distinct backing", "[imagegraph][array_uniform]"
) {
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
	const Node node{"uniform", "pc.array_uniform", "", {}, {}};
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
		context.ValueViews = {{"data", &input}};
		context.Values = {{"length", int64_t{3}}};
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
	"Uniform rejects nonfinite surface samples before zero or positive construction",
	"[imagegraph][array_uniform]"
) {
	const std::vector<Image> samples{
		{1, 1, {0, 0, 128, 127}, 31, SurfaceFormat::R32Float},
		{1, 1, {0, 0, 192, 127}, 32, SurfaceFormat::R32Float},
		{1, 1, {0, 124}, 33, SurfaceFormat::R16Float},
		{1, 1, {0, 126}, 34, SurfaceFormat::R16Float}
	};
	const Image finite{1, 1, {1, 2, 3, 255}, 19};
	const Node node{"uniform", "pc.array_uniform", "", {}, {}};
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	const auto executor = detail::FindExecutor(node.Type);
	REQUIRE(executor);
	const EvaluationRequest request;
	for (const auto &nonfinite : samples) {
		REQUIRE(ValidSurfaceLayout(nonfinite, Limits::MaximumDimension, Limits::MaximumArrayBytes));
		REQUIRE_FALSE(FiniteSurfaceSamples(nonfinite));
		const Image before = nonfinite;
		const ImageArray selected{{nonfinite}, {{size_t{0}}}};
		const ImageArray unused{{finite, nonfinite}, {{size_t{0}}}};
		const Value nested = General({Row({Row({{nonfinite}})}), {ElementValue{int64_t{7}}}});
		const Value nestedBefore = nested;
		for (int64_t length : {int64_t{0}, int64_t{1}}) {
			for (size_t route = 0; route < 4; ++route) {
				detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
				auto unrelated = budget.Reserve(512);
				REQUIRE(unrelated);
				{
					detail::NodeContext context(node, *entry, request, budget);
					context.ByteBudget = Limits::MaximumEvaluationBytes;
					context.Values = {{"length", length}};
					if (route == 0)
						context.Images = {{"data", &nonfinite}};
					else if (route == 1)
						context.ImageArrays = {{"data", &selected}};
					else if (route == 2)
						context.ImageArrays = {{"data", &unused}};
					else
						context.ValueViews = {{"data", &nested}};
					INFO("route=" << route << " length=" << length << " format=" << int(nonfinite.Format));
					CHECK_FALSE(executor(context));
					CHECK(context.FailureCode == Status::InvalidValue);
					CHECK(context.FailurePort == "data");
					CHECK(
						context.FailureMessage == (route == 3 ? "source uniform data is invalid"
															  : "source uniform image samples are nonfinite")
					);
					CHECK(context.OutputValues.empty());
					CHECK(context.OutputImages.empty());
					CHECK(context.OutputImageArrays.empty());
				}
				CHECK(budget.Used() == 512);
			}
		}
		CHECK(nonfinite == before);
		CHECK(selected.Images.front() == before);
		CHECK(unused.Images.back() == before);
		CHECK(nested == nestedBefore);
	}
}
