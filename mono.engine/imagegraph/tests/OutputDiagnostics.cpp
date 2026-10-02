#include "../src/NodeExecutors.hpp"

#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

TEST_SUITE_ID("engine.imagegraph.output_diagnostics")
using namespace engine::imagegraph;
namespace {
	Document RefusedHeight() {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"grid",
			 "pc.grid",
			 "",
			 {},
			 {{"dimension", Vector2{4, 4}},
			  {"dimension_unit", EnumValue{0}},
			  {"seed", 1.},
			  {"grid_size", Vector2{2, 2}},
			  {"grid_size_unit", EnumValue{0}},
			  {"render_type", EnumValue{1}},
			  {"gap_width", 1.},
			  {"tile_color", Gradient{0, {{0, {255, 255, 255, 255}}}}}}}
		};
		document.Outputs = {{"colour", "grid", "surface_out"}, {"height", "grid", "heightmap"}};
		return document;
	}
}
TEST_CASE("Output refusals preserve siblings and own bounded diagnostic storage", "[imagegraph][outputs]") {
	const Node node{"grid", "pc.grid", "", {}, {}};
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	const EvaluationRequest request;
	uint64_t needed = 0;
	{
		detail::EvaluationBudget budget{65536};
		detail::NodeContext context{node, *entry, request, budget};
		context.ByteBudget = 65536;
		REQUIRE(context.NewImage("surface_out", 1, 1));
		const auto before = budget.Used();
		CHECK(context.OutputDiagnostics.capacity() == 0);
		REQUIRE(
			context.SetOutputDiagnostic("heightmap", Status::UnsupportedExecution, "source has no height")
		);
		needed = budget.Used();
		CHECK(needed > before);
		CHECK(context.FailureCode == Status::Ok);
		CHECK(context.OutputImages.size() == 1);
		REQUIRE(context.OutputDiagnostics.size() == 1);
		CHECK(context.OutputDiagnostic("heightmap")->NodeId == "grid");
		REQUIRE(
			context.SetOutputDiagnostic("heightmap", Status::UnsupportedExecution, "source has no height")
		);
		CHECK(budget.Used() == needed);
		context.ClearOutputs();
		CHECK(context.OutputDiagnostics.empty());
		CHECK(context.OutputDiagnostics.capacity() == 0);
		CHECK(budget.Used() < before);
	}
	for (const uint64_t maximum : {needed, needed - 1}) {
		detail::EvaluationBudget budget{maximum + 512};
		auto unrelated = budget.Reserve(512);
		REQUIRE(unrelated);
		{
			detail::NodeContext context{node, *entry, request, budget};
			context.ByteBudget = maximum;
			REQUIRE(context.NewImage("surface_out", 1, 1));
			const bool published = context.SetOutputDiagnostic(
				"heightmap", Status::UnsupportedExecution, "source has no height"
			);
			CHECK(published == (maximum == needed));
			CHECK(context.FailureCode == (published ? Status::Ok : Status::LimitExceeded));
			CHECK(context.OutputDiagnostics.size() == (published ? 1 : 0));
		}
		CHECK(budget.Used() == 512);
	}
}
TEST_CASE("Output diagnostics enforce declared-port and payload exclusivity", "[imagegraph][outputs]") {
	const Node node{"grid", "pc.grid", "", {}, {}};
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	const EvaluationRequest request;
	for (int route = 0; route < 3; ++route) {
		INFO(route);
		detail::NodeContext context{node, *entry, request};
		context.ByteBudget = 65536;
		if (route == 0) {
			CHECK_FALSE(context.SetOutputDiagnostic("unknown", Status::UnsupportedExecution, "refused"));
		} else if (route == 1) {
			REQUIRE(context.NewImage("heightmap", 1, 1));
			CHECK_FALSE(context.SetOutputDiagnostic("heightmap", Status::UnsupportedExecution, "refused"));
		} else {
			REQUIRE(context.SetOutputDiagnostic("heightmap", Status::UnsupportedExecution, "refused"));
			CHECK(context.NewImage("heightmap", 1, 1) == nullptr);
		}
		CHECK(context.FailureCode == Status::InvalidOutput);
	}
}
TEST_CASE(
	"Input snapshots preserve their prior payload when a linked output is refused", "[imagegraph][outputs]"
) {
	auto document = RefusedHeight();
	document.Nodes.push_back({"consumer", "pc.invert", "", {}, {}});
	document.Links = {{"grid", "surface_out", "consumer", "surface_in"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationSnapshot snapshot;
	REQUIRE(EvaluateNodeInputs(document, plan, "consumer", {}, snapshot, diagnostic) == Status::Ok);
	REQUIRE(snapshot.Images().size() == 1);
	const auto image = snapshot.Images()[0].Data;
	const auto bytes = snapshot.RetainedBytes();
	document.Links[0].FromPort = "heightmap";
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CHECK(
		EvaluateNodeInputs(document, plan, "consumer", {}, snapshot, diagnostic) ==
		Status::UnsupportedExecution
	);
	CHECK(diagnostic.NodeId == "grid");
	CHECK(diagnostic.Port == "heightmap");
	CHECK(snapshot.RetainedBytes() == bytes);
	REQUIRE(snapshot.Images().size() == 1);
	CHECK(snapshot.Images()[0].Data == image);
}
TEST_CASE(
	"Output batches publish valid siblings and refuse a mixed selection atomically", "[imagegraph][outputs]"
) {
	const auto document = RefusedHeight();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	StatefulOutputEvaluationResult result;
	const std::array<std::string, 1> colour{"colour"};
	const std::array<std::string, 2> mixed{"colour", "height"};
	REQUIRE(EvaluateStatefulOutputs(document, plan, colour, {}, result, diagnostic) == Status::Ok);
	REQUIRE(result.Outputs.size() == 1);
	const Image previous = std::get<Image>(result.Outputs[0].Output);
	CHECK(
		EvaluateStatefulOutputs(document, plan, mixed, {}, result, diagnostic) == Status::UnsupportedExecution
	);
	CHECK(diagnostic.NodeId == "grid");
	CHECK(diagnostic.Port == "heightmap");
	REQUIRE(result.Outputs.size() == 1);
	CHECK(result.Outputs[0].Id == "colour");
	CHECK(std::get<Image>(result.Outputs[0].Output) == previous);
}
TEST_CASE("PCX named output lookup propagates the original refused port", "[imagegraph][outputs]") {
	auto document = RefusedHeight();
	document.Nodes[0].SourceInternalName = "source";
	document.Nodes.push_back(
		{"equation", "pc.equation", "", {}, {{"equation", std::string{"source.outputs.heightmap"}}}}
	);
	document.Outputs = {{"value", "equation", "result"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluatedValue result;
	result.Data = 17.;
	CHECK(EvaluateValue(document, plan, "value", {}, result, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.NodeId == "grid");
	CHECK(diagnostic.Port == "heightmap");
	CHECK(std::get<double>(result.Data) == 17.);
}
