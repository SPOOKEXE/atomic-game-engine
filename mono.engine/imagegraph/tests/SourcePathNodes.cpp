#include "../src/nodes/Path.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.source_path_nodes")
using namespace engine::imagegraph;
namespace {
	Path2D Line(double offset = 0) {
		Path2D path;
		path.Anchors = {{{offset, 0, 0, 0, 0, 0}, 0}, {{offset + 10, 0, 0, 0, 0, 0}, 0}};
		return path;
	}
}
TEST_CASE(
	"Reverse path preserves lazy source ratio endpoint and owned input history", "[imagegraph][source_path]"
) {
	const auto run = imagegraph_test::RunNode("pc.path_reverse", {}, {{"path", Line()}});
	INFO(run.Message);
	REQUIRE(run.Ok);
	auto reversed = std::get<Path2D>(*run.OutputValue("path"));
	REQUIRE(reversed.SourceOperation);
	CHECK(reversed.Anchors.empty());
	CHECK(reversed.SourceOperation->Kind == SourcePathOperationKind::Reverse);
	const auto sample = imagegraph_test::RunNode("pc.path_sample", {}, {{"path", reversed}, {"ratio", .25}});
	INFO(sample.Message);
	REQUIRE(sample.Ok);
	CHECK(std::get<Vector2>(*sample.OutputValue("position")).X == Catch::Approx(7.5));
	CHECK(std::get<double>(*sample.OutputValue("direction")) == Catch::Approx(180));
	auto copy = reversed;
	copy.SourceOperation->Inputs[0].Anchors[0].Controls[0] = 99;
	CHECK(reversed.SourceOperation->Inputs[0].Anchors[0].Controls[0] == 0);
	const auto endpoint =
		imagegraph_test::RunNode("pc.path_sample", {}, {{"path", reversed}, {"ratio", 0.0}});
	REQUIRE(endpoint.Ok);
	CHECK(std::get<Vector2>(*endpoint.OutputValue("position")).X == Catch::Approx(9.999));
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {{"reverse", "pc.path_reverse", "", {}, {{"path", reversed}}}};
	document.Outputs = {{"out", "reverse", "path"}};
	Diagnostic diagnostic;
	Document parsed;
	REQUIRE(Read(Write(document), parsed, diagnostic) == Status::Ok);
	CHECK(parsed == document);
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	document.FormatVersion = 8;
	CHECK(Compile(document, plan, diagnostic) == Status::UnsupportedVersion);
}
TEST_CASE(
	"Combined paths keep independent lines rather than connecting anchors", "[imagegraph][source_path]"
) {
	Path2D combined;
	auto &op = combined.SourceOperation.emplace();
	op.Kind = SourcePathOperationKind::Combine;
	op.Inputs = {Line(), Line(20)};
	const auto first = imagegraph_test::RunNode(
		"pc.path_sample", {}, {{"path", combined}, {"ratio", .5}, {"path_index", int64_t{0}}}
	);
	REQUIRE(first.Ok);
	CHECK(std::get<Vector2>(*first.OutputValue("position")).X == 5);
	const auto second = imagegraph_test::RunNode(
		"pc.path_sample", {}, {{"path", combined}, {"ratio", .5}, {"path_index", int64_t{1}}}
	);
	REQUIRE(second.Ok);
	CHECK(std::get<Vector2>(*second.OutputValue("position")).X == 25);
	const auto missing =
		imagegraph_test::RunNode("pc.path_sample", {}, {{"path", combined}, {"path_index", int64_t{2}}});
	REQUIRE(missing.Ok);
	CHECK(std::get<Vector2>(*missing.OutputValue("position")) == Vector2{});
	CHECK(std::get<double>(*missing.OutputValue("weight")) == 1);
}

TEST_CASE(
	"Combine executor spreads nested authored path inputs in source order", "[imagegraph][source_path]"
) {
	const auto *entry = FindCatalogueEntry("pc.path_array");
	REQUIRE(entry);
	Node node{
		"combine",
		"pc.path_array",
		"",
		{},
		{},
		{{"path_0", ValueType::Path2D, std::nullopt}, {"path_1", ValueType::Path2D, std::nullopt}}
	};
	EvaluationRequest request;
	engine::imagegraph::detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	ArrayValue array;
	array.ElementType = ValueType::Path2D;
	array.Elements = {Line(), Line(20)};
	context.Values = {{"path_0", array}, {"path_1", Line(40)}};
	const auto executor = engine::imagegraph::detail::FindExecutor(node.Type);
	REQUIRE(executor);
	REQUIRE(executor(context));
	INFO(context.FailureMessage);
	REQUIRE(context.FailureCode == Status::Ok);
	REQUIRE(context.OutputValues.size() == 1);
	const auto &path = std::get<Path2D>(context.OutputValues[0].Data);
	REQUIRE(path.SourceOperation);
	REQUIRE(path.SourceOperation->Inputs.size() == 3);
	for (size_t index = 0; index < 3; ++index) {
		const auto sample = imagegraph_test::RunNode(
			"pc.path_sample", {}, {{"path", path}, {"ratio", .5}, {"path_index", int64_t(index)}}
		);
		REQUIRE(sample.Ok);
		CHECK(std::get<Vector2>(*sample.OutputValue("position")).X == 5 + 20 * index);
	}
}

TEST_CASE(
	"Source path anchors retain six coordinates and explicit mirror controls", "[imagegraph][source_path]"
) {
	for (const bool mirrored : {true, false}) {
		const auto run = imagegraph_test::RunNode(
			"pc.path_anchor",
			{},
			{{"postion", Vector2{7, 9}},
			 {"control_point_1", Vector2{-3, 4}},
			 {"control_point_2", Vector2{5, 6}},
			 {"mirror_control_point", mirrored}}
		);
		REQUIRE(run.Ok);
		const auto &output = std::get<ArrayValue>(*run.OutputValue("anchor"));
		REQUIRE(output.Elements.size() == 6);
		const std::array expected{7.0, 9.0, -3.0, 4.0, mirrored ? 3.0 : 5.0, mirrored ? -4.0 : 6.0};
		for (size_t index = 0; index < expected.size(); ++index)
			CHECK(std::get<double>(output.Elements[index]) == expected[index]);
	}
}

TEST_CASE(
	"Trim path preserves signed range length, shift, clamp and native operation encoding",
	"[imagegraph][source_path]"
) {
	const auto run = imagegraph_test::RunNode(
		"pc.path_trim",
		{},
		{{"path", Line()},
		 {"range", Vector2{.2, .8}},
		 {"shift", .3},
		 {"clamp", true},
		 {"range_2", Vector2{0, 1}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	const auto path = std::get<Path2D>(*run.OutputValue("path"));
	REQUIRE(path.SourceOperation);
	CHECK(path.SourceOperation->TrimRange == Vector2{.5, 1});
	const auto sample = imagegraph_test::RunNode("pc.path_sample", {}, {{"path", path}, {"ratio", .5}});
	REQUIRE(sample.Ok);
	CHECK(std::get<Vector2>(*sample.OutputValue("position")).X == Catch::Approx(7.5));
	const auto reversed =
		imagegraph_test::RunNode("pc.path_trim", {}, {{"path", Line()}, {"range", Vector2{.8, .2}}});
	REQUIRE(reversed.Ok);
	Node node{"sample", "pc.path_sample", "", {}, {}};
	EvaluationRequest request;
	engine::imagegraph::detail::NodeContext context(node, *FindCatalogueEntry(node.Type), request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	engine::imagegraph::detail::PathRuntime runtime;
	REQUIRE(runtime.Init(context, std::get<Path2D>(*reversed.OutputValue("path"))));
	CHECK(runtime.Length() == Catch::Approx(-6));
	CHECK(runtime.PointRatio(.25).X == Catch::Approx(6.5));
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {{"trim", "pc.path_trim", "", {}, {{"path", path}}}};
	document.Outputs = {{"out", "trim", "path"}};
	Document parsed;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), parsed, diagnostic) == Status::Ok);
	CHECK(parsed == document);
}
