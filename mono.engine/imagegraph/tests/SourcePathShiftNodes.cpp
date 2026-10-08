#include "SourcePathShiftMemo.hpp"
#include "SourcePathShiftVisit.hpp"
#include "nodes/Path.hpp"

#include <engine/imagegraph/HostCapture.hpp>
#include <engine/imagegraph/SourceBuiltinRandom.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.source_path_shift")
using namespace engine::imagegraph;
namespace {
	const DataReplayEntry &ShiftReplayOwner(const DataReplayState &state, std::string_view id) {
		const auto matches = [&](const auto &entry) { return entry.NodeId == id && entry.ProcessorRow == 0; };
		REQUIRE(std::count_if(state.Entries.begin(), state.Entries.end(), matches) == 1);
		const auto &entry = *std::find_if(state.Entries.begin(), state.Entries.end(), matches);
		REQUIRE(entry.Initialized);
		REQUIRE(entry.Values.size() == 1);
		return entry;
	}
	Path2D ShiftLine() {
		Path2D path;
		path.Anchors = {{{0, 0, 0, 0, 0, 0}, 0}, {{10, 0, 0, 0, 0, 0}, 0}};
		path.Weights = {{0, 2}, {100, 4}};
		return path;
	}
	Document ShiftGraph() {
		Document document;
		document.FormatVersion = 9;
		Node collect{"collect", "pc.array", "", {}, {{"type", EnumValue{0}}}};
		collect.DynamicInputs = {
			{"input_0", ValueType::Vector2, std::nullopt}, {"input_1", ValueType::Vector2, std::nullopt}
		};
		document.Nodes = {
			{"shift", "pc.path_shift", "", {}, {{"path", ShiftLine()}, {"distance", 2.}}},
			{"first", "pc.path_sample", "", {}, {{"ratio", .2500001}}},
			{"second", "pc.path_sample", "", {}, {{"ratio", .2500004}}},
			std::move(collect)
		};
		document.Links = {
			{"shift", "path", "first", "path"},
			{"shift", "path", "second", "path"},
			{"first", "position", "collect", "input_0"},
			{"second", "position", "collect", "input_1"}
		};
		document.Outputs = {
			{"points", "collect", "array"},
			{"path", "shift", "path"},
			{"first", "first", "position"},
			{"second", "second", "position"}
		};
		return document;
	}
	Plan ShiftCompile(const Document &document) {
		Plan plan;
		Diagnostic error;
		const auto status = Compile(document, plan, error);
		INFO(error.Message);
		REQUIRE(status == Status::Ok);
		return plan;
	}
	Value ShiftValue(
		const Document &document,
		const Plan &plan,
		std::string output = "points",
		EvaluationRequest request = {}
	) {
		Diagnostic error;
		EvaluatedValue result;
		const auto status = EvaluateValue(document, plan, output, request, result, error);
		INFO(error.Message);
		REQUIRE(status == Status::Ok);
		return result.Data;
	}
	Vector2 ShiftPoint(const ArrayValue &value, size_t index) {
		REQUIRE(value.Elements.size() == 2);
		return std::get<Vector2>(value.Elements[index]);
	}
	Path2D ShiftBlob() {
		Path2D output;
		auto &op = output.SourceOperation.emplace();
		op.Kind = SourcePathOperationKind::Shift;
		op.Inputs.push_back(ShiftLine());
		op.ShiftDistance = 2;
		return output;
	}
}
TEST_CASE(
	"Shift shares the first six-decimal sample across actual linked consumers and resets next evaluation",
	"[source_path_shift]"
) {
	auto document = ShiftGraph();
	auto plan = ShiftCompile(document);
	const auto values = std::get<ArrayValue>(ShiftValue(document, plan));
	CHECK(ShiftPoint(values, 0).X == Catch::Approx(2.500001).margin(1e-12));
	CHECK(ShiftPoint(values, 1) == ShiftPoint(values, 0));
	CHECK(ShiftPoint(values, 0).Y == -2);
	const auto path = std::get<Path2D>(ShiftValue(document, plan, "path"));
	REQUIRE(path.SourceOperation);
	CHECK(path.SourceOperation->EvaluationMemoId == 0);
	std::swap(document.Nodes[1], document.Nodes[2]);
	plan = ShiftCompile(document);
	const auto reversed = std::get<ArrayValue>(ShiftValue(document, plan));
	CHECK(ShiftPoint(reversed, 0).X == Catch::Approx(2.500004).margin(1e-12));
	CHECK(ShiftPoint(reversed, 1) == ShiftPoint(reversed, 0));
	Document restored;
	Diagnostic error;
	REQUIRE(Read(Write(document), restored, error) == Status::Ok);
	CHECK(ShiftValue(restored, ShiftCompile(restored)) == Value(reversed));
}
TEST_CASE(
	"Shift assigns separate identities to authored blobs even if their stale numeric stamps match",
	"[source_path_shift]"
) {
	auto document = ShiftGraph();
	auto first = ShiftBlob(), second = ShiftBlob();
	first.SourceOperation->EvaluationMemoId = 1;
	second.SourceOperation->EvaluationMemoId = 1;
	document.Nodes[1].Values.push_back({"path", first});
	document.Nodes[2].Values.push_back({"path", second});
	document.Links.erase(document.Links.begin(), document.Links.begin() + 2);
	const auto values = std::get<ArrayValue>(ShiftValue(document, ShiftCompile(document)));
	CHECK(ShiftPoint(values, 0).X == Catch::Approx(2.500001).margin(1e-12));
	CHECK(ShiftPoint(values, 1).X == Catch::Approx(2.500004).margin(1e-12));
	CHECK(document.Nodes[1].Values.back().Data == Value(first));
	CHECK(std::get<Path2D>(document.Nodes[1].Values.back().Data).SourceOperation->EvaluationMemoId == 1);
}
TEST_CASE(
	"Shift preserves inclusive source range, signed distance, weight and original metadata",
	"[source_path_shift]"
) {
	Node node{"sample", "pc.path_sample", "", {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(node, *FindCatalogueEntry("pc.path_sample"), request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	detail::SourcePathShiftMemo memo;
	context.PathShiftMemo = &memo;
	auto path = ShiftBlob();
	path.SourceOperation->ShiftDistance = -2;
	path.SourceOperation->ShiftRange = {.25, .75};
	context.Values = {{"path", path}};
	REQUIRE(detail::StampSourcePathShiftInputs(context));
	detail::PathRuntime runtime;
	REQUIRE(runtime.Init(context, std::get<Path2D>(context.Values[0].second)));
	CHECK(runtime.Length() == 10);
	CHECK(runtime.LineCount() == 1);
	CHECK(runtime.SegmentCount() == 1);
	CHECK(runtime.MinX == 0);
	CHECK(runtime.MaxX == 10);
	CHECK(runtime.MinY == 0);
	CHECK(runtime.MaxY == 0);
	CHECK(runtime.PointRatio(.2).Y == 0);
	CHECK(runtime.PointRatio(.25).Y == 2);
	CHECK(runtime.PointRatio(.75).Y == 2);
	CHECK(runtime.PointRatio(.8).Y == 0);
	CHECK(runtime.PointRatio(.5).Weight == Catch::Approx(3));
	CHECK(runtime.PointDistance(5).X == Catch::Approx(5));
	CHECK(runtime.PointDistance(5).Y == 2);
	CHECK(context.FailureCode == Status::Ok);
}
TEST_CASE(
	"Shift keeps first-sample range-boundary decisions and line caches independent", "[source_path_shift]"
) {
	Node node{"sample", "pc.path_sample", "", {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(node, *FindCatalogueEntry("pc.path_sample"), request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	detail::SourcePathShiftMemo memo;
	context.PathShiftMemo = &memo;
	auto path = ShiftBlob();
	path.SourceOperation->ShiftRange = {.25, .75};
	context.Values = {{"path", path}};
	REQUIRE(detail::StampSourcePathShiftInputs(context));
	detail::PathRuntime runtime;
	REQUIRE(runtime.Init(context, std::get<Path2D>(context.Values[0].second)));
	const auto outside = runtime.PointRatio(.2499998), collision = runtime.PointRatio(.2500001);
	CHECK(outside.Y == 0);
	CHECK(collision.X == outside.X);
	CHECK(collision.Y == 0);
	CHECK(collision.Weight == outside.Weight);
	CHECK(runtime.PointRatio(.2500001, 1).Y == -2);
}
TEST_CASE(
	"Shift clears memo identities from snapshots, host captures, builtin captures and saved authored paths",
	"[source_path_shift]"
) {
	auto document = ShiftGraph();
	const auto plan = ShiftCompile(document);
	Diagnostic error;
	EvaluationSnapshot snapshot;
	REQUIRE(EvaluateNodeInputs(document, plan, "first", {}, snapshot, error) == Status::Ok);
	const auto path = std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &value) {
		return value.Port == "path";
	});
	REQUIRE(path != snapshot.Values().end());
	CHECK(std::get<Path2D>(path->Data).SourceOperation->EvaluationMemoId == 0);
	HostNodeCapture capture;
	REQUIRE(PrepareHostCapture(document, plan, "first", {}, capture, error) == Status::Ok);
	const auto captured = std::find_if(capture.Inputs.begin(), capture.Inputs.end(), [](const auto &value) {
		return value.Port == "path";
	});
	REQUIRE(captured != capture.Inputs.end());
	CHECK(std::get<Path2D>(captured->Data).SourceOperation->EvaluationMemoId == 0);
	SourceBuiltinRandomCapture builtin;
	REQUIRE(PrepareSourceBuiltinRandomCapture(document, plan, "first", {}, builtin, error) == Status::Ok);
	const auto recorded = std::find_if(builtin.Inputs.begin(), builtin.Inputs.end(), [](const auto &value) {
		return value.Port == "path";
	});
	REQUIRE(recorded != builtin.Inputs.end());
	CHECK(std::get<Path2D>(recorded->Data).SourceOperation->EvaluationMemoId == 0);
	auto authored = ShiftBlob();
	authored.SourceOperation->EvaluationMemoId = 9821;
	document.Nodes[0].Values[0].Data = authored;
	Document restored;
	REQUIRE(Read(Write(document), restored, error) == Status::Ok);
	const auto saved = std::get<Path2D>(restored.Nodes[0].Values[0].Data);
	CHECK(saved == authored);
	CHECK(saved.SourceOperation->EvaluationMemoId == 0);
}
TEST_CASE("Shift animated controls survive native save and fresh seeks", "[source_path_shift]") {
	auto document = ShiftGraph();
	document.Nodes.resize(1);
	document.Links.clear();
	document.Outputs = {{"path", "shift", "path"}};
	document.Nodes[0].Values[1].Data = 1.;
	document.Keyframes = {{"shift", "distance", 0, 1., "linear"}, {"shift", "distance", 2, -2., "step"}};
	Document restored;
	Diagnostic error;
	REQUIRE(Read(Write(document), restored, error) == Status::Ok);
	EvaluationRequest request;
	request.Tick = 2;
	const auto end = ShiftValue(restored, ShiftCompile(restored), "path", request);
	CHECK(std::get<Path2D>(end).SourceOperation->ShiftDistance == -2);
	request.Tick = 0;
	CHECK(
		std::get<Path2D>(ShiftValue(restored, ShiftCompile(restored), "path", request))
			.SourceOperation->ShiftDistance == 1
	);
	request.Tick = 2;
	CHECK(ShiftValue(restored, ShiftCompile(restored), "path", request) == end);
}
TEST_CASE(
	"Shift memo and payload budget refusal preserve prior outputs and caller authored trees",
	"[source_path_shift]"
) {
	auto document = ShiftGraph();
	const auto plan = ShiftCompile(document);
	Diagnostic error;
	StatefulEvaluationResult output;
	output.Output = EvaluatedValue{"old", int64_t{17}, {}};
	const auto before = std::get<EvaluatedValue>(output.Output);
	CHECK(EvaluateStateful(document, plan, "points", {}, output, error, 32) == Status::LimitExceeded);
	CHECK(std::get<EvaluatedValue>(output.Output) == before);
	Node node{"sample", "pc.path_sample", "", {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(node, *FindCatalogueEntry("pc.path_sample"), request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	detail::SourcePathShiftMemo memo;
	context.PathShiftMemo = &memo;
	context.Values = {{"path", ShiftBlob()}};
	REQUIRE(detail::StampSourcePathShiftInputs(context));
	detail::PathRuntime runtime;
	REQUIRE(runtime.Init(context, std::get<Path2D>(context.Values[0].second)));
	context.ByteBudget = 0;
	(void)runtime.PointRatio(.5);
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(memo.Entries.empty());
	CHECK(document == ShiftGraph());
}
TEST_CASE(
	"Shift six-decimal key matches exact HTML5 half-away ties and negative zero classes",
	"[source_path_shift]"
) {
	const auto key = [](double ratio) {
		const auto k = detail::SourceShiftRatioKey(ratio);
		REQUIRE(k);
		return std::string(k->Text.data(), k->Size);
	};
	CHECK(key(.0078125) == "0.007813");
	CHECK(key(-.0078125) == "-0.007813");
	CHECK(key(.0234375) == "0.023438");
	CHECK(key(-.0234375) == "-0.023438");
	CHECK(key(-0.) == key(0.));
	CHECK(key(-.0000001) != key(.0000001));
	CHECK(key(.2500001) == key(.2500004));
}

TEST_CASE(
	"Shift native constructor validation leaves the first real sample fresh while Skew seeds zero",
	"[source_path_shift]"
) {
	for (const std::string type : {"pc.path_transform", "pc.path_redistribute", "pc.path_skew"}) {
		auto document = ShiftGraph();
		document.Nodes[1].Values = {{"ratio", .0000004}};
		document.Nodes[2].Values = {{"ratio", .0000003}};
		Node wrapper{"wrapper", type, "", {}, {}};
		if (type == "pc.path_skew") wrapper.Values = {{"strength", 0.}};
		document.Nodes.insert(document.Nodes.begin() + 1, std::move(wrapper));
		document.Links[0] = {"wrapper", "path", "first", "path"};
		document.Links.push_back({"shift", "path", "wrapper", "path"});
		const auto values = std::get<ArrayValue>(ShiftValue(document, ShiftCompile(document)));
		INFO(type);
		const double expected = type == "pc.path_skew" ? 0. : .000004;
		CHECK(ShiftPoint(values, 0).X == Catch::Approx(expected).margin(1e-12));
		CHECK(ShiftPoint(values, 1).X == Catch::Approx(expected).margin(1e-12));
	}
}
TEST_CASE(
	"Shift validation memo suppression is nested and restores on scope unwinding", "[source_path_shift]"
) {
	Node node{"sample", "pc.path_sample", "", {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(node, *FindCatalogueEntry("pc.path_sample"), request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	detail::SourcePathShiftMemo memo;
	context.PathShiftMemo = &memo;
	context.Values = {{"path", ShiftBlob()}};
	REQUIRE(detail::StampSourcePathShiftInputs(context));
	detail::PathRuntime runtime;
	REQUIRE(runtime.Init(context, std::get<Path2D>(context.Values[0].second)));
	try {
		detail::SourcePathShiftValidationScope outer(context);
		(void)runtime.PointRatio(0);
		{
			detail::SourcePathShiftValidationScope inner(context);
			CHECK(memo.ValidationProbe);
		}
		CHECK(memo.ValidationProbe);
		throw 17;
	} catch (int) {}
	CHECK_FALSE(memo.ValidationProbe);
	CHECK(memo.Entries.empty());
	CHECK(runtime.PointRatio(.0000004).X == Catch::Approx(.000004).margin(1e-12));
	CHECK(memo.Entries.size() == 1);
}
TEST_CASE(
	"Shift inherited tangent wrappers and Node fallback objects preserve their source class distinction",
	"[source_path_shift]"
) {
	Node node{"sample", "pc.path_sample", "", {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(node, *FindCatalogueEntry("pc.path_sample"), request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	for (auto kind :
		 {SourcePathOperationKind::Reverse,
		  SourcePathOperationKind::Trim,
		  SourcePathOperationKind::Offset,
		  SourcePathOperationKind::Blend,
		  SourcePathOperationKind::Redistribute,
		  SourcePathOperationKind::Skew,
		  SourcePathOperationKind::Transform,
		  SourcePathOperationKind::AreaMap,
		  SourcePathOperationKind::Combine,
		  SourcePathOperationKind::Join,
		  SourcePathOperationKind::Shift}) {
		Path2D path;
		auto &operation = path.SourceOperation.emplace();
		operation.Kind = kind;
		operation.Inputs = {ShiftLine()};
		if (kind == SourcePathOperationKind::Blend) operation.Inputs.push_back(ShiftLine());
		if (kind == SourcePathOperationKind::Join) operation.Reversed = {0};
		if (kind == SourcePathOperationKind::Redistribute) {
			operation.RedistributeMap.emplace();
			for (size_t i = 0; i <= 32; ++i)
				(*operation.RedistributeMap)[i] = double(i) / 32;
		}
		detail::PathRuntime runtime;
		REQUIRE(runtime.Init(context, path));
		const bool inherited = kind != SourcePathOperationKind::Combine &&
							   kind != SourcePathOperationKind::Join &&
							   kind != SourcePathOperationKind::Shift;
		CHECK(runtime.HasSourceTangent() == inherited);
	}
}

TEST_CASE(
	"Shift strips nested recipe authoring recordings and group overlays without persisting ephemeral IDs",
	"[source_path_shift]"
) {
	auto blob = ShiftBlob();
	blob.SourceOperation->EvaluationMemoId = 19;
	DynamicSurfaceValue surface;
	auto &recipe = surface.Data.emplace();
	recipe.Authored.FormatVersion = 9;
	recipe.Authored.Nodes.push_back({"source", "pc.path_shift", "", {}, {{"path", blob}}});
	recipe.PcxObservations = {{"path", blob}};
	HostNodeCapture host;
	host.Authored = recipe.Authored.Nodes.front();
	host.Inputs = {{"path", blob}};
	host.Outputs = {{"path", blob}};
	recipe.HostCaptures.push_back(host);
	SourceBuiltinRandomCapture builtin;
	builtin.Authored = host.Authored;
	builtin.Inputs = host.Inputs;
	recipe.BuiltinRandomCaptures.push_back(builtin);
	recipe.DataHistory.emplace().Entries.push_back({"cache", 0, 0, 0, false, true, 0, 0, false, {{0, blob}}});
	recipe.Groups.emplace();
	auto owner = std::make_unique<detail::GroupReplayAccess::Owner>(Limits::MaximumEvaluationBytes);
	GroupReplayEntry entry;
	entry.NodeId = "group";
	entry.ParentReset = blob;
	entry.SubtypeStatic = blob;
	owner->Entries.push_back(std::move(entry));
	owner->SharedSubtypes.push_back({"group", blob, {}, "subtype"});
	detail::GroupReplayAccess::Install(recipe.Groups->Replay, std::move(owner));
	Value value = std::move(surface);
	detail::StripSourcePathShiftIdentities(value);
	detail::SourcePathShiftRoute route;
	size_t visited = 0;
	auto check = [&](const auto &operation, const detail::SourcePathShiftRoute &) {
		CHECK(operation.EvaluationMemoId == 0);
		++visited;
		return true;
	};
	REQUIRE(detail::VisitSourcePathShift(std::get<DynamicSurfaceValue>(value), route, check));
	CHECK(visited == 11);
	CHECK(blob.SourceOperation->EvaluationMemoId == 19);
}
TEST_CASE(
	"Shift final stateful publication strips preserved unselected DataReplay paths", "[source_path_shift]"
) {
	auto document = ShiftGraph();
	document.Nodes.push_back({"unused", "pc.path_shift", "", {}, {}});
	auto blob = ShiftBlob();
	blob.SourceOperation->EvaluationMemoId = 124;
	DataReplayState prior;
	prior.Entries.push_back({"unused", 0, 0, 0, false, true, 0, 0, false, {{0, blob}}});
	EvaluationRequest request;
	request.DataReplay = &prior;
	StatefulEvaluationResult result;
	Diagnostic error;
	const auto status = EvaluateStateful(document, ShiftCompile(document), "points", request, result, error);
	INFO(error.Message);
	REQUIRE(status == Status::Ok);
	const auto &unused = ShiftReplayOwner(result.Data, "unused");
	CHECK(std::get<Path2D>(unused.Values[0].Data).SourceOperation->EvaluationMemoId == 0);
	for (const auto id : {"first", "second"}) {
		const auto &sampler = ShiftReplayOwner(result.Data, id);
		const auto &buffers = std::get<ArrayValue>(sampler.Values[0].Data);
		CHECK(buffers.ElementType == ValueType::Vector4);
		REQUIRE(buffers.Elements.size() == 6);
	}

	CHECK(std::get<Path2D>(prior.Entries[0].Values[0].Data).SourceOperation->EvaluationMemoId == 124);
}

TEST_CASE(
	"Shift typed path arrays roundtrip and select through the actual source array getter",
	"[source_path_shift]"
) {
	auto a = ShiftBlob(), b = ShiftBlob();
	b.SourceOperation->ShiftDistance = -2;
	Document document;
	document.FormatVersion = 9;
	Node pool{"pool", "pc.array", "", {}, {{"type", EnumValue{0}}}};
	pool.DynamicInputs = {{"input_0", ValueType::Path2D, Value{a}}, {"input_1", ValueType::Path2D, Value{b}}};
	document.Nodes = {
		std::move(pool),
		{"pick", "pc.array_get", "", {}, {{"index", int64_t{1}}}},
		{"sample", "pc.path_sample", "", {}, {{"ratio", .5}}}
	};
	document.Links = {{"pool", "array", "pick", "array"}, {"pick", "value", "sample", "path"}};
	document.Outputs = {
		{"path", "pick", "value"}, {"point", "sample", "position"}, {"array", "pool", "array"}
	};
	const auto plan = ShiftCompile(document);
	const auto selected = std::get<Path2D>(ShiftValue(document, plan, "path"));
	CHECK(selected.SourceOperation->ShiftDistance == -2);
	CHECK(selected.SourceOperation->EvaluationMemoId == 0);
	CHECK(std::get<Vector2>(ShiftValue(document, plan, "point")) == Vector2{5, 2});
	Diagnostic error;
	Document restored;
	REQUIRE(Read(Write(document), restored, error) == Status::Ok);
	CHECK(ShiftValue(restored, ShiftCompile(restored), "point") == ShiftValue(document, plan, "point"));
	auto invalid = ShiftGraph();
	invalid.Nodes[0].Values[1].Data = ArrayValue{ValueType::Scalar, {1., -2.}};
	Plan prior = plan;
	CHECK(Compile(invalid, prior, error) == Status::TypeMismatch);
	CHECK(prior == plan);
}

TEST_CASE(
	"Shift animated linked producers refresh wrapper samples with caller-owned prior data and fresh seeks",
	"[source_path_shift]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"shift", "pc.path_shift", "", {}, {{"path", ShiftLine()}, {"distance", 2.}}},
		{"redistribute", "pc.path_redistribute", "", {}, {}},
		{"sample", "pc.path_sample", "", {}, {{"ratio", .5}}}
	};
	document.Links = {{"shift", "path", "redistribute", "path"}, {"redistribute", "path", "sample", "path"}};
	document.Outputs = {{"point", "sample", "position"}};
	document.Keyframes = {{"shift", "distance", 0, 2., "linear"}, {"shift", "distance", 1, -2., "step"}};
	const auto plan = ShiftCompile(document);
	Diagnostic error;
	StatefulEvaluationResult first, next, fresh;
	REQUIRE(EvaluateStateful(document, plan, "point", {}, first, error) == Status::Ok);
	CHECK(std::get<Vector2>(std::get<EvaluatedValue>(first.Output).Data) == Vector2{5, -2});
	EvaluationRequest request;
	request.Tick = 1;
	request.DataReplay = &first.Data;
	REQUIRE(EvaluateStateful(document, plan, "point", request, next, error) == Status::Ok);
	CHECK(std::get<Vector2>(std::get<EvaluatedValue>(next.Output).Data) == Vector2{5, 2});
	request.DataReplay = nullptr;
	REQUIRE(EvaluateStateful(document, plan, "point", request, fresh, error) == Status::Ok);
	CHECK(std::get<EvaluatedValue>(next.Output) == std::get<EvaluatedValue>(fresh.Output));
	const auto &redistribute = ShiftReplayOwner(next.Data, "redistribute");
	const auto &path = std::get<Path2D>(redistribute.Values[0].Data);
	const auto &sampler = ShiftReplayOwner(next.Data, "sample");
	const auto &buffers = std::get<ArrayValue>(sampler.Values[0].Data);
	CHECK(buffers.ElementType == ValueType::Vector4);
	REQUIRE(buffers.Elements.size() == 6);
	REQUIRE(path.SourceOperation);
	REQUIRE(path.SourceOperation->Inputs[0].SourceOperation);
	CHECK(path.SourceOperation->Inputs[0].SourceOperation->EvaluationMemoId == 0);
}

TEST_CASE(
	"Shift loop controls apply only to the Node fallback and inherited Path tangent keeps its own loop",
	"[source_path_shift]"
) {
	for (const bool fallback : {false, true}) {
		Node node{"sample", "pc.path_sample", "", {}, {}};
		EvaluationRequest request;
		detail::NodeContext context(node, *FindCatalogueEntry("pc.path_sample"), request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		detail::SourcePathShiftMemo memo;
		context.PathShiftMemo = &memo;
		auto shifted = ShiftBlob();
		shifted.SourceOperation->ShiftLoop = true;
		if (fallback) {
			Path2D combined;
			auto &operation = combined.SourceOperation.emplace();
			operation.Kind = SourcePathOperationKind::Combine;
			operation.Inputs = {ShiftLine()};
			shifted.SourceOperation->Inputs = {std::move(combined)};
		}
		context.Values = {{"path", shifted}};
		REQUIRE(detail::StampSourcePathShiftInputs(context));
		detail::PathRuntime runtime;
		REQUIRE(runtime.Init(context, std::get<Path2D>(context.Values[0].second)));
		const auto point = runtime.PointRatio(0);
		CHECK(point.X == 0);
		CHECK(point.Y == (fallback ? 2 : -2));
		CHECK(point.Weight == 2);
		CHECK(context.FailureCode == Status::Ok);
	}
}
