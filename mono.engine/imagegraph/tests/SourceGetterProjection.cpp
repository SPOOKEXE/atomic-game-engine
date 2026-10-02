#include "../src/SourceGetterProjection.hpp"

#include "../src/ProcessorBatch.hpp"
#include "../src/ValuePayload.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>

TEST_SUITE_ID("engine.imagegraph.source_getter_projection")
using namespace engine::imagegraph;
namespace {
	Value Evaluated(Document graph, const EvaluationRequest &request = {}) {
		Plan plan;
		Diagnostic diagnostic;
		const auto compiled = Compile(graph, plan, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(compiled == Status::Ok);
		Document restored;
		REQUIRE(Read(Write(graph), restored, diagnostic) == Status::Ok);
		CHECK(restored == graph);
		EvaluatedValue result;
		const auto status = EvaluateValue(restored, plan, "out", request, result, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return result.Data;
	}
	Document Vector(double boolean) {
		Document document;
		document.FormatVersion = 8;
		document.Nodes = {{"vector", "pc.vector2", "", {}, {{"x", 5.5}, {"integer", boolean}}, {}}};
		document.Outputs = {{"out", "vector", "x"}};
		return document;
	}
	Value Projected(std::string_view type, std::string_view port, const Value &raw) {
		const auto *entry = FindCatalogueEntry(type);
		REQUIRE(entry);
		Node node{"node", std::string(type), "", {}, {}, {}};
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		context.ValueViews.emplace_back(port, &raw);
		Value result;
		{
			detail::SourceGetterProjection projection(context);
			REQUIRE(projection.Prepare());
			REQUIRE(context.Find(port));
			result = *context.Find(port);
		}
		CHECK(context.Find(port) == &raw);
		return result;
	}
}
TEST_CASE(
	"Source raw real admission is limited to proven ordinary constructor domains",
	"[imagegraph][source_getter]"
) {
	const auto &width = *FindCatalogueInput(*FindCatalogueEntry("pc.audio_window"), "width");
	const auto &integer = *FindCatalogueInput(*FindCatalogueEntry("pc.vector2"), "integer");
	CHECK(CatalogueSourceRawValue(width, Value{6.5}));
	CHECK(CatalogueSourceRawValue(integer, Value{.25}));
	CHECK_FALSE(CatalogueSourceRawValue(width, Value{std::numeric_limits<double>::infinity()}));
	CHECK_FALSE(CatalogueSourceRawValue(width, Value{int64_t{6}}));
	for (std::string_view kind : {"Toggle", "SeedInt", "Float"}) {
		auto excluded = width;
		excluded.SourceKind = kind;
		CHECK_FALSE(CatalogueSourceRawValue(excluded, Value{6.5}));
	}
	auto attribute = integer;
	attribute.SourceIndex = -1;
	CHECK_FALSE(CatalogueSourceRawValue(attribute, Value{.25}));
	auto retag = width;
	retag.Type = ValueType::Vector4;
	CHECK_FALSE(CatalogueSourceRawValue(retag, Value{6.5}));
	bool slider = false, active = false;
	for (const auto &entry : Catalogue())
		for (const auto &input : entry.Inputs) {
			if (input.SourceIndex >= 0 && input.SourceKind == "ISlider" && input.Type == ValueType::Integer) {
				slider = true;
				CHECK(CatalogueSourceRawValue(input, Value{6.5}));
			}
			if (input.SourceIndex >= 0 && input.SourceKind == "Active" && input.Type == ValueType::Boolean) {
				active = true;
				CHECK(CatalogueSourceRawValue(input, Value{.25}));
			}
		}
	CHECK(slider);
	CHECK(active);
}
TEST_CASE(
	"Durable raw Boolean fractions use source greater-than-half at actual vector evaluation",
	"[imagegraph][source_getter]"
) {
	for (double raw : {-1., .25, .5, .75}) {
		const auto graph = Vector(raw);
		CHECK(std::get<double>(Evaluated(graph)) == (raw > .5 ? 6. : 5.5));
		CHECK(std::get<double>(graph.Nodes.front().Values.back().Data) == raw);
	}
	for (const auto &[port, raw] : std::array{std::pair{"width", 6.5}, std::pair{"empty", .25}}) {
		Document legacy;
		legacy.Nodes = {{"legacy", "image.solid", "", {}, {{port, raw}}, {}}};
		legacy.Outputs = {{"out", "legacy", "image"}};
		Plan plan;
		Diagnostic diagnostic;
		CHECK(Compile(legacy, plan, diagnostic) == Status::TypeMismatch);
		CHECK(diagnostic.Port == port);
	}
}
TEST_CASE(
	"Source integer fractions persist raw and round at the actual Audio Window getter",
	"[imagegraph][source_getter]"
) {
	Document graph;
	graph.FormatVersion = 8;
	graph.Nodes = {
		{"capture", "image.audio_recording", "", {}, {{"source_id", std::string{"mono"}}}, {}},
		{"window",
		 "pc.audio_window",
		 "",
		 {},
		 {{"width", 6.5}, {"step", int64_t{1}}, {"cursor_location", EnumValue{0}}, {"match_timeline", false}},
		 {}}
	};
	graph.Links = {{"capture", "audio", "window", "audio_data"}};
	graph.Outputs = {{"out", "window", "bit_array"}};
	const std::array frames{AudioCaptureFrame{"mono", 0, {1, 2, 3, 4, 5, 6, 7, 8}, 10}};
	EvaluationRequest request;
	request.AudioFrames = frames;
	const auto result = std::get<ArrayValue>(Evaluated(graph, request));
	REQUIRE(result.Nested.size() == 1);
	REQUIRE(result.Nested.front().size() == 6);
	CHECK(std::get<double>(result.Nested.front().back()) == 6.);
	CHECK(std::get<double>(graph.Nodes[1].Values[0].Data) == 6.5);
}
TEST_CASE(
	"Source integer ties and depth bypass use original whole input shape", "[imagegraph][source_getter]"
) {
	for (auto [raw, expected] : std::array{
			 std::pair{2.5, int64_t{2}},
			 std::pair{3.5, int64_t{4}},
			 std::pair{-2.5, int64_t{-2}},
			 std::pair{-3.5, int64_t{-4}}
		 })
		CHECK(std::get<int64_t>(Projected("pc.audio_window", "width", Value{raw})) == expected);
	ArrayValue flat{ValueType::Scalar, {2.5, 3.5, -2.5, -3.5}};
	CHECK(
		std::get<ArrayValue>(Projected("pc.audio_window", "width", flat)) ==
		(ArrayValue{ValueType::Integer, {int64_t{2}, int64_t{4}, int64_t{-2}, int64_t{-4}}})
	);
	ArrayValue nested{ValueType::Scalar, {}, {{2.5, 3.5}, {-2.5, -3.5}}};
	CHECK(Projected("pc.audio_window", "width", nested) == Value{nested});
	CHECK(
		Projected("pc.vector2", "integer", nested) ==
		Value{ArrayValue{ValueType::Boolean, {}, {{true, true}, {false, false}}}}
	);
	CHECK(
		Projected("pc.audio_window", "width", Vector2{6.5, -2.5}) ==
		Value{ArrayValue{ValueType::Integer, {int64_t{6}, int64_t{-2}}}}
	);
	CHECK(
		Projected("pc.audio_window", "width", MatrixValue{2, 1, {6.5, -2.5}}) ==
		Value{ArrayValue{ValueType::Integer, {int64_t{6}, int64_t{-2}}}}
	);
	CHECK(Projected("pc.vector2", "integer", Colour{1, 0, 0, 0}) == Value{true});
	CHECK(Projected("pc.audio_window", "width", Colour{5, 0, 0, 0}) == Value{int64_t{5}});
}
TEST_CASE(
	"Typed vector links invoke recursive source Bool before processor expansion",
	"[imagegraph][source_getter]"
) {
	Document graph = Vector(.25);
	graph.Nodes.insert(graph.Nodes.begin(), {"control", "pc.vector2", "", {}, {{"x", .5}, {"y", .75}}, {}});
	graph.Links = {{"control", "vector", "vector", "integer"}};
	CHECK(Evaluated(graph) == Value{ArrayValue{ValueType::Scalar, {5.5, 6.}}});
	ArrayValue vectors{ValueType::Vector2, {Vector2{.25, .75}, Vector2{.5, -1}}};
	CHECK(
		Projected("pc.vector2", "integer", vectors) ==
		Value{ArrayValue{ValueType::Boolean, {}, {{false, true}, {false, false}}}}
	);
}
TEST_CASE(
	"Source constructor validators bind exact indices before integer rounding", "[imagegraph][source_getter]"
) {
	CHECK(Projected("pc.convolution", "size", 2.5) == Value{int64_t{3}});
	CHECK(Projected("pc.convolution", "size", 17.5) == Value{int64_t{16}});
	const auto *entry = FindCatalogueSource("Node_MK_Pile");
	REQUIRE(entry);
	for (auto [index, expected] : {std::pair{6, int64_t{1}}, std::pair{11, int64_t{0}}}) {
		const auto *input = FindCatalogueInputIndex(*entry, index);
		REQUIRE(input);
		CHECK(Projected(entry->Type, input->Id, -1.) == Value{expected});
	}
}
TEST_CASE(
	"Getter overlays preserve cast and row precedence across every scalar reader",
	"[imagegraph][source_getter]"
) {
	const auto *entry = FindCatalogueEntry("pc.vector2");
	REQUIRE(entry);
	Node node{"node", "pc.vector2", "", {}, {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.Values.emplace_back("integer", .75);
	Value cast = .25;
	context.ValueViews.emplace_back("integer", &cast);
	{
		detail::SourceGetterProjection getter(context);
		REQUIRE(getter.Prepare());
		CHECK_FALSE(context.Boolean("integer"));
		CHECK(context.Integer("integer") == 0);
		CHECK(context.Scalar("integer") == 0);
		CHECK_FALSE(context.Get<bool>("integer"));
		CHECK(*context.Find("integer") == Value{false});
	}
	CHECK(context.Find("integer") == &cast);
	Value array = ArrayValue{ValueType::Scalar, {.25, .75}};
	context.ValueViews.back().second = &array;
	const auto executor = +[](detail::NodeContext &row) {
		const bool flag = std::get<bool>(*row.Find("integer"));
		if (row.Boolean("integer") != flag || row.Integer("integer", 23) != 23 ||
			row.Scalar("integer", 17) != 17 || row.Get<bool>("integer") != flag)
			return false;
		row.SetValue("x", double(flag));
		return row.FailureCode == Status::Ok;
	};
	REQUIRE(detail::RunProcessorBatch(context, executor));
	REQUIRE(context.OutputValues.size() == 1);
	CHECK(context.OutputValues.front().Data == Value{ArrayValue{ValueType::Scalar, {0., 1.}}});
	CHECK(context.Find("integer") == &array);
	CHECK(context.Values.front().second == Value{.75});
}
TEST_CASE(
	"Getter workspace admits prior live payload plus exact headroom and refuses one byte short",
	"[imagegraph][source_getter]"
) {
	Value raw = ArrayValue{ValueType::Scalar, {6.5, 7.5}};
	const uint64_t resident =
		detail::ValuePayloadBytes(raw) - sizeof(Value) + sizeof(std::pair<std::string_view, const Value *>);
	const uint64_t needed = detail::ValuePayloadBytes(raw) - sizeof(Value) +
							sizeof(std::pair<std::string_view, Value>) +
							2 * sizeof(std::pair<std::string_view, const Value *>);
	for (bool exact : {true, false}) {
		detail::EvaluationBudget budget(resident + needed - (exact ? 0 : 1));
		auto prior = budget.Reserve(resident);
		REQUIRE(prior);
		const auto *entry = FindCatalogueEntry("pc.audio_window");
		REQUIRE(entry);
		Node node{"node", "pc.audio_window", "", {}, {}, {}};
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request, budget);
		context.ByteBudget = resident + needed;
		context.ValueViews.emplace_back("width", &raw);
		{
			detail::SourceGetterProjection getter(context);
			CHECK(getter.Prepare() == exact);
			if (exact)
				CHECK(context.Find("width") != &raw);
			else {
				CHECK(context.FailureCode == Status::LimitExceeded);
				CHECK(context.Find("width") == &raw);
			}
		}
		CHECK(context.Find("width") == &raw);
		CHECK(budget.Used() == resident);
	}
}
TEST_CASE(
	"Unrepresented source struct getters refuse atomically without numeric coercion",
	"[imagegraph][source_getter]"
) {
	const auto *entry = FindCatalogueEntry("pc.vector2");
	REQUIRE(entry);
	Node node{"node", "pc.vector2", "", {}, {}, {}};
	EvaluationRequest request;
	for (Value value :
		 {Value{MatrixValue{1, 1, {1.}}},
		  Value{Quaternion{}},
		  Value{MeshValue3D{}},
		  Value{MaterialValue3D{}}}) {
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		context.ValueViews.emplace_back("integer", &value);
		// Failure occurs before an executor can consume a fabricated fallback.
		const auto executor = +[](detail::NodeContext &row) {
			row.SetValue("x", row.Scalar("integer"));
			return row.FailureCode == Status::Ok;
		};
		CHECK_FALSE(detail::RunProcessorBatch(context, executor));
		CHECK(context.FailureCode == Status::UnsupportedExecution);
		CHECK(context.OutputValues.empty());
		CHECK(context.Find("integer") == &value);
	}
}

TEST_CASE(
	"Source integer range narrowing rejects unrepresentable rounded reals atomically",
	"[imagegraph][source_getter]"
) {
	const auto *entry = FindCatalogueEntry("pc.audio_window");
	REQUIRE(entry);
	Node node{"node", "pc.audio_window", "", {}, {}, {}};
	EvaluationRequest request;
	for (double raw :
		 {-0x1p63,
		  0x1p63,
		  std::nextafter(-0x1p63, -std::numeric_limits<double>::infinity()),
		  std::nextafter(0x1p63, 0.)}) {
		for (Value value : {Value{raw}, Value{ArrayValue{ValueType::Scalar, {raw}}}}) {
			detail::NodeContext context(node, *entry, request);
			context.ByteBudget = Limits::MaximumEvaluationBytes;
			context.ValueViews.emplace_back("width", &value);
			detail::SourceGetterProjection projection(context);
			const bool representable = raw >= -0x1p63 && raw < 0x1p63;
			CHECK(projection.Prepare() == representable);
			if (representable) {
				const Value expected =
					std::holds_alternative<ArrayValue>(value)
						? Value{ArrayValue{ValueType::Integer, {static_cast<int64_t>(raw)}}}
						: Value{static_cast<int64_t>(raw)};
				CHECK(*context.Find("width") == expected);
			} else {
				CHECK(context.FailureCode == Status::InvalidValue);
				CHECK(context.FailureMessage == "integer input must be finite and within int64 range");
				CHECK(context.Find("width") == &value);
			}
		}
	}
}
TEST_CASE(
	"Source getter overlays retain enum array shape for source choice bypass", "[imagegraph][source_getter]"
) {
	const auto *vector = FindCatalogueEntry("pc.vector2");
	const auto *palette = FindCatalogueEntry("pc.gradient_palette");
	REQUIRE(vector);
	REQUIRE(palette);
	std::vector<CatalogueInput> inputs(vector->Inputs.begin(), vector->Inputs.end());
	const auto *mode = FindCatalogueInput(*palette, "interpolation");
	REQUIRE(mode);
	inputs.push_back(*mode);
	CatalogueEntry entry = *vector;
	entry.Inputs = inputs;
	Node node{"node", "pc.vector2", "", {}, {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(node, entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	Value raw = .75;
	Value choices = ArrayValue{ValueType::Scalar, {-1., 8.}};
	context.ValueViews = {{"integer", &raw}, {"interpolation", &choices}};
	const auto executor = +[](detail::NodeContext &row) {
		row.SetValue("x", row.SourceChoice("interpolation"));
		return row.FailureCode == Status::Ok;
	};
	REQUIRE(detail::RunProcessorBatch(context, executor));
	CHECK(context.OutputValues.front().Data == Value{ArrayValue{ValueType::Scalar, {-1., 8.}}});
	CHECK(context.Find("interpolation") == &choices);
	CHECK(context.ProcessorOriginalValues.empty());
}
TEST_CASE(
	"Dynamic source getter inputs use their constructor template domain", "[imagegraph][source_getter]"
) {
	const CatalogueEntry *found = nullptr;
	const CatalogueInput *field = nullptr;
	for (const auto &entry : Catalogue()) {
		for (const auto &input : entry.DynamicTemplate) {
			if (input.SourceKind == "Bool" && input.SourceIndex >= 0 && input.Type == ValueType::Boolean) {
				found = &entry;
				field = &input;
				break;
			}
		}
		if (found) break;
	}
	REQUIRE(found);
	REQUIRE(field);
	const std::string port = std::string(field->Id) + "_2";
	Node node{"node", std::string(found->Type), "", {}, {}, {{port, ValueType::Boolean, std::nullopt}}};
	EvaluationRequest request;
	detail::NodeContext context(node, *found, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	Value raw = .75;
	context.ValueViews.emplace_back(port, &raw);
	detail::SourceGetterProjection projection(context);
	REQUIRE(projection.Prepare());
	CHECK(*context.Find(port) == Value{true});
}
