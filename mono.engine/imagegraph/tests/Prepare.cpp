#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.prepare")
TEST_DEPENDS("engine.imagegraph.document")
TEST_DEPENDS("engine.imagegraph.inputs")
using namespace engine::imagegraph;

namespace {
	Document Graph() {
		return {
			.Nodes =
				{{"source", Source{"source.png"}, {}, {}},
				 {"resize", Resize{4, 3, Sampling::Bilinear}, {"source"}, {}},
				 {"flip", Flip{}, {"resize"}, {}},
				 {"unused", Source{"missing.png"}, {}, {}}},
			.Outputs = {{"main", "flip"}, {"original", "source"}}
		};
	}
}
TEST_CASE(
	"shared admission returns exact selected order extents and conservative budgets", "[imagegraph][prepare]"
) {
	Document document = Graph();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic));
	std::array<SourceExtent, 1> sources{{{"source", 2, 3}}};
	ExecutionPlan execution;
	REQUIRE(Prepare(document, plan, "main", sources, execution, diagnostic));
	CHECK(execution.Order == std::vector<size_t>{0, 1, 2});
	CHECK(execution.Target == 2);
	CHECK(execution.Output == 0);
	CHECK(execution.Extents[0] == ImageExtent{2, 3, 24});
	CHECK(execution.Extents[1] == ImageExtent{4, 3, 48});
	CHECK(execution.Extents[2] == ImageExtent{4, 3, 48});
	CHECK(execution.Extents[3] == ImageExtent{});
	CHECK(execution.RetainedBytes == 120);
	CHECK(execution.PixelWork == 6 + 48 + 12);
	REQUIRE(Prepare(document, plan, "original", sources, execution, diagnostic));
	CHECK(execution.Order == std::vector<size_t>{0});
	CHECK(execution.RetainedBytes == 24);
}
TEST_CASE(
	"shared admission refuses missing stale incompatible or oversized metadata without altering accepted "
	"plan",
	"[imagegraph][prepare]"
) {
	Document document = Graph();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic));
	std::vector<SourceExtent> sources{{"source", 2, 3}};
	ExecutionPlan execution;
	execution.Target = 99;
	std::string output = "main";
	SECTION("missing source") {
		sources.clear();
	}
	SECTION("zero dimensions") {
		sources[0].Width = 0;
	}
	SECTION("large dimensions") {
		sources[0].Height = 4097;
	}
	SECTION("unknown source") {
		sources[0].Node = "missing";
	}
	SECTION("non source") {
		sources[0].Node = "resize";
	}
	SECTION("duplicate source") {
		sources.push_back(sources[0]);
	}
	SECTION("unknown output") {
		output = "missing";
	}
	SECTION("ambiguous output") {
		output.clear();
	}
	SECTION("stale plan") {
		plan.Order[0] = 100;
	}
	SECTION("blend mismatch") {
		document.Nodes.push_back({"blend", Blend{}, {"source", "resize"}, {}});
		document.Outputs[0].Node = "blend";
		REQUIRE(Compile(document, plan, diagnostic));
	}
	CHECK_FALSE(Prepare(document, plan, output, sources, execution, diagnostic));
	CHECK(execution.Target == 99);
	CHECK_FALSE(diagnostic.Message.empty());
}
TEST_CASE(
	"shared admission checks source dependent work before any composed output allocation",
	"[imagegraph][prepare]"
) {
	Document document{
		.Nodes = {{"source", Source{"source.png"}, {}, {}}, {"flip", Flip{}, {"source"}, {}}},
		.Outputs = {{"main", "flip"}}
	};
	for (size_t index = 2; index <= 4; ++index)
		document.Nodes.push_back(
			{"flip" + std::to_string(index),
			 Flip{},
			 {index == 2 ? "flip" : "flip" + std::to_string(index - 1)},
			 {}}
		);
	document.Outputs[0].Node = "flip4";
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic));
	const std::array<SourceExtent, 1> sources{{{"source", 4096, 4096}}};
	ExecutionPlan execution;
	CHECK_FALSE(Prepare(document, plan, {}, sources, execution, diagnostic));
	CHECK(diagnostic.Message.find("retained") != std::string::npos);
}
TEST_CASE("typed source interpretation and output space preserve numeric bytes", "[imagegraph][prepare]") {
	Document document{
		.Nodes = {{"source", Source{"data.atex", SourceInterpretation::Data}, {}, {}}},
		.Outputs = {{"data", "source", OutputSpace::Linear}}
	};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic));
	Image output;
	REQUIRE(EvaluateTyped(
		document,
		plan,
		{},
		[](const Source &source, Image &image, std::string &) {
			CHECK(source.Interpretation == SourceInterpretation::Data);
			image = {1, 1, {std::byte{128}, std::byte{64}, std::byte{32}, std::byte{255}}};
			return true;
		},
		output,
		diagnostic
	));
	CHECK(output.Space == OutputSpace::Linear);
	CHECK(output.Pixels[0] == std::byte{128});
	CHECK_FALSE(Evaluate(
		document, plan, {}, [](std::string_view, Image &, std::string &) { return true; }, output, diagnostic
	));
	CHECK(output.Pixels[0] == std::byte{128});
	CHECK(diagnostic.Message.find("typed") != std::string::npos);
}

TEST_CASE(
	"shared admission accepts exact memory and work ceilings and rejects the next retained image",
	"[imagegraph][prepare]"
) {
	Document document{.Nodes = {{"source", Source{"source.png"}, {}, {}}}, .Outputs = {{"main", "flip3"}}};
	for (size_t index = 1; index <= 3; ++index)
		document.Nodes.push_back(
			{"flip" + std::to_string(index),
			 Flip{},
			 {index == 1 ? "source" : "flip" + std::to_string(index - 1)},
			 {}}
		);
	Diagnostic diagnostic;
	Plan plan;
	ExecutionPlan execution;
	const std::array<SourceExtent, 1> sources{{{"source", 4096, 4096}}};
	REQUIRE(Compile(document, plan, diagnostic));
	REQUIRE(Prepare(document, plan, {}, sources, execution, diagnostic));
	CHECK(execution.RetainedBytes == Limits::MaximumRetainedBytes);
	CHECK(execution.PixelWork == Limits::MaximumPixelWork);
	const auto accepted = execution;
	document.Nodes.push_back({"flip4", Flip{}, {"flip3"}, {}});
	document.Outputs[0].Node = "flip4";
	REQUIRE(Compile(document, plan, diagnostic));
	CHECK_FALSE(Prepare(document, plan, {}, sources, execution, diagnostic));
	CHECK(execution == accepted);
}
