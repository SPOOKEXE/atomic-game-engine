#include "StrandCodec.hpp"
#include "nodes/SourceStrandNodes.hpp"

#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/SourceBuiltinRandom.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <sstream>
TEST_SUITE_ID("engine.imagegraph.strand_graph")
using namespace engine::imagegraph;
namespace {
	Document Scene() {
		Document d;
		d.FormatVersion = 9;
		d.Nodes = {
			{"create",
			 "pc.strand_create",
			 "",
			 {},
			 {{"strands", int64_t{1}},
			  {"segment", int64_t{1}},
			  {"position_unit", EnumValue{0}},
			  {"position", Vector2{0, 0}},
			  {"elasticity", 1.},
			  {"spring", 1.},
			  {"structure", 0.}}},
			{"gravity", "pc.strand_gravity", "", {}, {{"gravity", 1.}}},
			{"second", "pc.strand_gravity", "", {}, {{"gravity", 2.}}},
			{"update", "pc.strand_update", "", {}, {{"step", int64_t{1}}}}
		};
		d.Links = {
			{"create", "strands", "gravity", "input_0"},
			{"gravity", "strands", "second", "input_0"},
			{"second", "strands", "update", "input_0"}
		};
		d.Outputs = {{"strands", "update", "strands"}, {"alias", "create", "strands"}};
		return d;
	}
	SourceBuiltinRandomCapture Capture(Document &d, Plan &p, EvaluationRequest request, uint32_t count) {
		SourceBuiltinRandomCapture capture;
		Diagnostic diag;
		REQUIRE(PrepareSourceBuiltinRandomCapture(d, p, "create", request, capture, diag) == Status::Ok);
		for (uint32_t i = 0; i < count; ++i) {
			capture.Draws.push_back({SourceBuiltinRandomOperation::RandomRange, 4, 4, 4});
			capture.Draws.push_back(
				{SourceBuiltinRandomOperation::IRandomRange, 100000, 999999, 123456. + i}
			);
		}
		return capture;
	}
	const StrandValue &Leaf(const StatefulEvaluationResult &r) {
		return std::get<StrandValue>(std::get<EvaluatedValue>(r.Output).Data);
	}
} // namespace
TEST_CASE(
	"Registered strand graph orders mutations and transactionally "
	"replays owned aliases",
	"[strand_graph]"
) {
	auto d = Scene();
	Plan p;
	Diagnostic diag;
	INFO(diag.Message);
	REQUIRE(Compile(d, p, diag) == Status::Ok);
	EvaluationRequest request;
	request.SimulationAuthoringRevision = 4;
	auto capture = Capture(d, p, request, 1);
	request.BuiltinRandomCaptures = {&capture, 1};
	StatefulEvaluationResult result;
	REQUIRE(EvaluateStateful(d, p, "strands", request, result, diag) == Status::Ok);
	REQUIRE(result.Data.Entries.size() == 1);
	CHECK(result.Data.Entries[0].NodeId == "create");
	const auto &state = Leaf(result).Data->State;
	REQUIRE(state.Hairs.size() == 1);
	CHECK(state.Hairs[0].Points[0].Position == std::array<double, 2>{0, 0});
	CHECK(state.Hairs[0].Points[1].Position[1] == Catch::Approx(3));
	const auto initial = result;
	request.BuiltinRandomCaptures = {};
	request.DataReplay = &result.Data;
	request.Tick = 1;
	REQUIRE(EvaluateStateful(d, p, "strands", request, result, diag) == Status::Ok);
	CHECK(Leaf(result).Data->State.Hairs[0].Points[1].Position[1] == Catch::Approx(12));
	const auto checkpoint = result;
	CHECK(EvaluateStateful(d, p, "strands", request, result, diag) == Status::InvalidValue);
	CHECK(result.Data == checkpoint.Data);
	CHECK(Leaf(result) == Leaf(checkpoint));
	request.ReuseSimulationFrame = true;
	REQUIRE(EvaluateStateful(d, p, "strands", request, result, diag) == Status::Ok);
	CHECK(result.Data == checkpoint.Data);
	CHECK(Leaf(result) == Leaf(checkpoint));
	request.ReuseSimulationFrame = false;
	request.Tick = 2;
	CHECK(EvaluateStateful(d, p, "strands", request, result, diag, 1) == Status::LimitExceeded);
	CHECK(result.Data == checkpoint.Data);
	CHECK(Leaf(result) == Leaf(checkpoint));
	request = {};
	request.SimulationAuthoringRevision = 4;
	request.BuiltinRandomCaptures = {&capture, 1};
	REQUIRE(EvaluateStateful(d, p, "strands", request, result, diag) == Status::Ok);
	CHECK(result.Data == initial.Data);
	CHECK(Leaf(result) == Leaf(initial));
}
TEST_CASE(
	"Strand constructor grows from exact draws and density shrink "
	"retains source hairs",
	"[strand_graph]"
) {
	auto d = Scene();
	d.Outputs[0] = {"strands", "create", "strands"};
	Plan p;
	Diagnostic diag;
	INFO(diag.Message);
	REQUIRE(Compile(d, p, diag) == Status::Ok);
	EvaluationRequest request;
	request.SimulationAuthoringRevision = 9;
	auto capture = Capture(d, p, request, 1);
	request.BuiltinRandomCaptures = {&capture, 1};
	StatefulEvaluationResult result;
	REQUIRE(EvaluateStateful(d, p, "strands", request, result, diag) == Status::Ok);
	auto first = Leaf(result).Data->State.Hairs[0];
	d.Nodes[0].Values[0].Data = int64_t{2};
	REQUIRE(Compile(d, p, diag) == Status::Ok);
	request.Tick = 1;
	request.DataReplay = &result.Data;
	request.BuiltinRandomCaptures = {};
	capture = Capture(d, p, request, 1);
	request.BuiltinRandomCaptures = {&capture, 1};
	REQUIRE(EvaluateStateful(d, p, "strands", request, result, diag) == Status::Ok);
	REQUIRE(Leaf(result).Data->State.Hairs.size() == 2);
	CHECK(Leaf(result).Data->State.Hairs[0] == first);
	CHECK(Leaf(result).Data->State.Hairs[1].Direction == 180);
	d.Nodes[0].Values[0].Data = int64_t{0};
	REQUIRE(Compile(d, p, diag) == Status::Ok);
	request.Tick = 2;
	request.BuiltinRandomCaptures = {};
	REQUIRE(EvaluateStateful(d, p, "strands", request, result, diag) == Status::Ok);
	CHECK(Leaf(result).Data->State.Hairs.size() == 2);
	auto checkpoint = result;
	d.Nodes[0].Values.push_back({"distribution", EnumValue{1}});
	REQUIRE(Compile(d, p, diag) == Status::Ok);
	request.Tick = 3;
	CHECK(EvaluateStateful(d, p, "strands", request, result, diag) == Status::UnsupportedExecution);
	CHECK(result.Data == checkpoint.Data);
	CHECK(Leaf(result) == Leaf(checkpoint));
}
TEST_CASE(
	"Strand native geometry and typed array codec preserve data with "
	"bounded admission",
	"[strand_graph]"
) {
	auto d = Scene();
	Plan p;
	Diagnostic diag;
	INFO(diag.Message);
	REQUIRE(Compile(d, p, diag) == Status::Ok);
	EvaluationRequest request;
	auto capture = Capture(d, p, request, 1);
	request.BuiltinRandomCaptures = {&capture, 1};
	StatefulEvaluationResult result;
	REQUIRE(EvaluateStateful(d, p, "strands", request, result, diag) == Status::Ok);
	Document stored;
	stored.FormatVersion = 9;
	stored.Nodes = {
		{"value", "value.array", "", {}, {{"value", ArrayValue{ValueType::Strand, {Leaf(result)}}}}}
	};
	stored.Outputs = {{"array", "value", "value"}};
	const auto text = Write(stored);
	Document restored;
	REQUIRE(Read(text, restored, diag) == Status::Ok);
	CHECK(restored == stored);
	auto before = restored;
	CHECK(Read(text.substr(0, text.size() / 2), restored, diag) != Status::Ok);
	CHECK(restored == before);
}
TEST_CASE(
	"Strand selected output union resolves sibling aliases and ignores "
	"unrelated scoped creators",
	"[strand_graph]"
) {
	auto d = Scene();
	d.Nodes.insert(d.Nodes.begin(), {"scope", "pc.strand_group_inline", "", {}, {}});
	Group owner;
	owner.Id = "scope/inline";
	owner.OwnerNodeId = "scope";
	d.Groups.push_back(owner);
	for (auto &node : d.Nodes)
		if (node.Id != "scope") node.GroupId = owner.Id;
	d.Nodes.push_back({"unvisited", "pc.strand_create", "", {}, {{"distribution", EnumValue{1}}}});
	Plan p;
	Diagnostic diag;
	INFO(diag.Message);
	REQUIRE(Compile(d, p, diag) == Status::Ok);
	EvaluationRequest request;
	request.SimulationAuthoringRevision = 3;
	auto capture = Capture(d, p, request, 1);
	request.BuiltinRandomCaptures = {&capture, 1};
	const std::array<std::string, 2> ids{"strands", "alias"};
	StatefulOutputEvaluationResult result;
	REQUIRE(EvaluateStatefulOutputs(d, p, ids, request, result, diag) == Status::Ok);
	REQUIRE(result.Outputs.size() == 2);
	REQUIRE(result.Data.Entries.size() == 1);
	const auto &updated = std::get<StrandValue>(std::get<EvaluatedValue>(result.Outputs[0].Output).Data);
	const auto &alias = std::get<StrandValue>(std::get<EvaluatedValue>(result.Outputs[1].Output).Data);
	CHECK(alias == updated);
	CHECK(alias.Data->State.Hairs[0].Points[1].Position[1] == Catch::Approx(3));
	const auto checkpoint = result.Data;
	d.Nodes[4].Values[0].Data = int64_t{4097};
	REQUIRE(Compile(d, p, diag) == Status::Ok);
	request.Tick = 1;
	request.DataReplay = &result.Data;
	request.BuiltinRandomCaptures = {};
	CHECK(EvaluateStatefulOutputs(d, p, ids, request, result, diag) == Status::InvalidValue);
	CHECK(result.Data == checkpoint);
	d.Nodes[4].Values[0].Data = int64_t{1};
	REQUIRE(Compile(d, p, diag) == Status::Ok);
	REQUIRE(EvaluateStatefulOutputs(d, p, ids, request, result, diag) == Status::Ok);
	CHECK(result.Data.Entries[0].Tick == 1);
}
TEST_CASE(
	"Strand fresh seek reconstruction equals a contiguous run and "
	"rejects stale capture revision",
	"[strand_graph]"
) {
	auto d = Scene();
	Plan p;
	Diagnostic diag;
	INFO(diag.Message);
	REQUIRE(Compile(d, p, diag) == Status::Ok);
	EvaluationRequest request;
	request.SimulationAuthoringRevision = 6;
	auto capture = Capture(d, p, request, 1);
	const auto run = [&](uint64_t target, StatefulEvaluationResult &result) {
		EvaluationRequest clock;
		clock.SimulationAuthoringRevision = 6;
		for (uint64_t tick = 0; tick <= target; ++tick) {
			clock.Tick = tick;
			clock.BuiltinRandomCaptures = tick == 0 ? std::span<const SourceBuiltinRandomCapture>{&capture, 1}
													: std::span<const SourceBuiltinRandomCapture>{};
			clock.DataReplay = tick ? &result.Data : nullptr;
			REQUIRE(EvaluateStateful(d, p, "strands", clock, result, diag) == Status::Ok);
		}
	};
	StatefulEvaluationResult contiguous, seek;
	run(3, contiguous);
	run(1, seek);
	seek = {};
	run(3, seek);
	CHECK(Leaf(seek) == Leaf(contiguous));
	CHECK(seek.Data == contiguous.Data);
	const auto checkpoint = seek.Data;
	request.Tick = 4;
	request.SimulationAuthoringRevision = 7;
	request.DataReplay = &seek.Data;
	CHECK(EvaluateStateful(d, p, "strands", request, seek, diag) == Status::InvalidValue);
	CHECK(seek.Data == checkpoint);
	request = {};
	auto stale = capture;
	stale.Draws.pop_back();
	request.BuiltinRandomCaptures = {&stale, 1};
	CHECK(EvaluateStateful(d, p, "strands", request, seek, diag) == Status::UnsupportedExecution);
	CHECK(seek.Data == checkpoint);
}
TEST_CASE(
	"Native Strand arrays execute generic length and selection without "
	"flattening geometry",
	"[strand_graph]"
) {
	auto d = Scene();
	Plan p;
	Diagnostic diag;
	INFO(diag.Message);
	REQUIRE(Compile(d, p, diag) == Status::Ok);
	EvaluationRequest request;
	auto capture = Capture(d, p, request, 1);
	request.BuiltinRandomCaptures = {&capture, 1};
	StatefulEvaluationResult sample;
	REQUIRE(EvaluateStateful(d, p, "strands", request, sample, diag) == Status::Ok);
	ArrayValue array{ValueType::Strand, {Leaf(sample), Leaf(sample)}};
	std::get<StrandValue>(array.Elements[1]).Data->State.Hairs[0].Points[0].Position[0] = 99;
	Document transported;
	transported.FormatVersion = 9;
	transported.Nodes = {
		{"get", "pc.array_get", "", {}, {{"index", int64_t{1}}}}, {"length", "pc.array_length", "", {}, {}}
	};
	transported.Junctions = {{"array", "", ValueType::Array, Value{array}}};
	transported.Links = {{"array", "value", "get", "array"}, {"array", "value", "length", "array"}};
	transported.Outputs = {{"selected", "get", "value"}, {"count", "length", "size"}};
	Document restored;
	REQUIRE(Read(Write(transported), restored, diag) == Status::Ok);
	const auto compiled = Compile(restored, p, diag);
	INFO(diag.Message);
	INFO(diag.NodeId);
	INFO(diag.Port);
	REQUIRE(compiled == Status::Ok);
	EvaluatedValue output;
	REQUIRE(EvaluateValue(restored, p, "selected", {}, output, diag) == Status::Ok);
	REQUIRE(std::holds_alternative<StrandValue>(output.Data));
	CHECK(std::get<StrandValue>(output.Data).Data->State.Hairs[0].Points[0].Position[0] == 99);
	REQUIRE(EvaluateValue(restored, p, "count", {}, output, diag) == Status::Ok);
	CHECK(std::get<int64_t>(output.Data) == 2);
}
TEST_CASE(
	"Strand native codec refuses malformed provenance and allocation "
	"before replacing geometry",
	"[strand_graph]"
) {
	auto d = Scene();
	Plan p;
	Diagnostic diag;
	INFO(diag.Message);
	REQUIRE(Compile(d, p, diag) == Status::Ok);
	EvaluationRequest request;
	auto capture = Capture(d, p, request, 1);
	request.BuiltinRandomCaptures = {&capture, 1};
	StatefulEvaluationResult result;
	REQUIRE(EvaluateStateful(d, p, "strands", request, result, diag) == Status::Ok);
	StrandValue retained = Leaf(result), before = retained;
	std::stringstream invalid("1 4096 0 0 \"\" 1 0");
	CHECK_FALSE(engine::imagegraph::detail::ReadStrandValue(invalid, retained, [](uint64_t) {
		return true;
	}));
	CHECK(retained == before);
	std::stringstream denied;
	engine::imagegraph::detail::WriteStrandValue(denied, retained);
	CHECK_FALSE(engine::imagegraph::detail::ReadStrandValue(denied, retained, [](uint64_t) {
		return false;
	}));
	CHECK(retained == before);
	auto oversized = retained;
	oversized.Data->State.Hairs[0].Points.push_back({});
	CHECK_FALSE(engine::imagegraph::detail::ValidStrandPayload(oversized));
}
TEST_CASE(
	"Strand array mutation preserves independent origin rows and "
	"replacement admission",
	"[strand_graph]"
) {
	auto d = Scene();
	d.Outputs = {{"one", "create", "strands"}};
	d.Nodes.resize(1);
	d.Nodes.push_back(d.Nodes[0]);
	d.Nodes[1].Id = "other";
	d.Outputs.push_back({"two", "other", "strands"});
	d.Links.clear();
	Plan p;
	Diagnostic diag;
	INFO(diag.Message);
	REQUIRE(Compile(d, p, diag) == Status::Ok);
	EvaluationRequest request;
	auto one = Capture(d, p, request, 1);
	SourceBuiltinRandomCapture two;
	REQUIRE(PrepareSourceBuiltinRandomCapture(d, p, "other", request, two, diag) == Status::Ok);
	two.Draws = one.Draws;
	two.Draws[1].Result = 654321;
	const std::array<SourceBuiltinRandomCapture, 2> captures{one, two};
	request.BuiltinRandomCaptures = captures;
	StatefulOutputEvaluationResult initial;
	const std::array<std::string, 2> outputs{"one", "two"};
	REQUIRE(EvaluateStatefulOutputs(d, p, outputs, request, initial, diag) == Status::Ok);
	ArrayValue array{
		ValueType::Strand,
		{std::get<StrandValue>(std::get<EvaluatedValue>(initial.Outputs[0].Output).Data),
		 std::get<StrandValue>(std::get<EvaluatedValue>(initial.Outputs[1].Output).Data)}
	};
	d.Junctions = {{"array", "", ValueType::Array, Value{array}}};
	d.Nodes.push_back({"gravity", "pc.strand_gravity", "", {}, {{"gravity", 3.}}});
	d.Links = {{"array", "value", "gravity", "input_0"}};
	d.Outputs = {{"mutated", "gravity", "strands"}};
	REQUIRE(Compile(d, p, diag) == Status::Ok);
	request.BuiltinRandomCaptures = {};
	request.DataReplay = &initial.Data;
	StatefulEvaluationResult result;
	REQUIRE(EvaluateStateful(d, p, "mutated", request, result, diag) == Status::Ok);
	REQUIRE(result.Data.Entries.size() == 2);
	const auto &updated = std::get<ArrayValue>(std::get<EvaluatedValue>(result.Output).Data);
	REQUIRE(updated.Elements.size() == 2);
	CHECK(std::get<StrandValue>(updated.Elements[0]).Data->State.Hairs[0].SourceId == 123456);
	CHECK(std::get<StrandValue>(updated.Elements[1]).Data->State.Hairs[0].SourceId == 654321);
	for (const auto &leaf : updated.Elements)
		CHECK(std::get<StrandValue>(leaf).Data->State.Hairs[0].Points[1].Position[1] == 3);
	const auto before = result.Data;
	CHECK(EvaluateStateful(d, p, "mutated", request, result, diag, 1) == Status::LimitExceeded);
	CHECK(result.Data == before);
}
TEST_CASE(
	"Strand executor refuses malformed flat-array leaves with an explicit diagnostic", "[strand_graph]"
) {
	Node node;
	node.Id = "gravity";
	node.Type = "pc.strand_gravity";
	EvaluationRequest request;
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	engine::imagegraph::detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.Values = {{"input_0", ArrayValue{ValueType::Any, {UndefinedValue{}}}}};
	CHECK_FALSE(engine::imagegraph::detail::StrandGravity(context));
	CHECK(context.FailureCode == Status::InvalidValue);
	CHECK(context.OutputValues.empty());
	CHECK(context.DataUpdates.empty());
}
TEST_CASE(
	"Strand array update admits aggregate constraint visits before copying geometry", "[strand_graph]"
) {
	Node node;
	node.Id = "step";
	node.Type = "pc.strand_update";
	EvaluationRequest request;
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	engine::imagegraph::detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	StrandValue geometry;
	auto &data = geometry.Data.emplace();
	SourceStrandHair hair;
	hair.SourceId = 123456;
	hair.Points.resize(256);
	hair.Lengths.resize(256);
	hair.RestAngles.resize(256);
	data.State.Hairs.push_back(hair);
	CHECK(engine::imagegraph::detail::ValidStrandPayload(geometry));
	context.Values = {
		{"input_0", ArrayValue{ValueType::Strand, {geometry, geometry}}}, {"step", int64_t{4096}}
	};
	CHECK_FALSE(engine::imagegraph::detail::StrandUpdate(context));
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.DataUpdates.empty());
	CHECK(context.OutputValues.empty());
	CHECK(context.OutputCharge.Bytes() == 0);
}
