#include "../src/ProcessorBatch.hpp"
#include "NodeHarness.hpp"

#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>

TEST_SUITE_ID("engine.imagegraph.source_generator_dimension")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;

TEST_CASE(
	"Source generator Mask dimensions multiply the authored components", "[source_generator_dimension]"
) {
	const Image mask{3, 4, std::vector<uint8_t>(3 * 4 * 4, 255), 0};
	const std::array types{
		"pc.checker",
		"pc.quasicrystal",
		"pc.wave_interfere",
		"pc.zigzag",
		"pc.box_pattern",
		"pc.fold_noise",
		"pc.noise_aniso"
	};
	for (const std::string_view type : types) {
		const auto run = RunNode(
			type,
			{{"mask", &mask}},
			{{"dimension", Vector2{2, 0.5}},
			 {"dimension_unit", EnumValue{2}},
			 {"seed", 17.0},
			 {"seed_2", 31.0}}
		);
		INFO(type);
		REQUIRE(run.Ok);
		CHECK(run.Output().Width == 6);
		CHECK(run.Output().Height == 2);
	}
}

TEST_CASE(
	"Source generator linked surface Dimension bypasses local Mask units", "[source_generator_dimension]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"source", "pc.solid", "", {}, {{"dimension", Vector2{3, 2}}, {"dimension_unit", EnumValue{0}}}},
		{"checker", "pc.checker", "", {}, {{"dimension_unit", EnumValue{2}}}}
	};
	document.Links = {{"source", "surface_out", "checker", "dimension"}};
	document.Outputs = {{"image", "checker", "surface_out"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image output;
	REQUIRE(Evaluate(document, plan, "image", EvaluationRequest{}, output, diagnostic) == Status::Ok);
	CHECK(output.Width == 3);
	CHECK(output.Height == 2);
}

TEST_CASE(
	"Native general-array Dimension preflight includes scalar rows alongside vectors",
	"[source_generator_dimension]"
) {
	const CatalogueEntry *entry = FindCatalogueEntry("pc.checker");
	const detail::Executor executor = detail::FindExecutor("pc.checker");
	REQUIRE(entry);
	REQUIRE(executor);
	Node authored{"checker", "pc.checker", "", {}, {}};
	EvaluationRequest request;
	detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	detail::NodeContext context(authored, *entry, request, budget);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	ArrayValue dimensions{
		ValueType::Any,
		{},
		{},
		{SourceArrayItem{ElementValue{Vector2{3, 20000}}}, SourceArrayItem{ElementValue{20000.0}}}
	};
	context.Values = {{"dimension", std::move(dimensions)}, {"dimension_unit", EnumValue{2}}};
	ImageArray masks;
	masks.Images = {
		Image{1, 1, std::vector<uint8_t>(4, 255), 0}, Image{2048, 1, std::vector<uint8_t>(2048 * 4, 255), 0}
	};
	masks.Items = {ImageArrayItem{size_t{0}}, ImageArrayItem{size_t{1}}};
	context.ImageArrays.emplace_back("mask", &masks);
	context.ProcessorCount = 2;
	context.InputProvenanceResolved = true;
	CHECK_FALSE(detail::RunProcessorBatch(context, executor, nullptr, nullptr));
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.FailurePort == "dimension");
	CHECK(context.OutputImages.empty());
	CHECK(budget.Peak() < 64 * 1024);
}

TEST_CASE(
	"Source generator preflight rejects negative multiplication overflow before row allocation",
	"[source_generator_dimension]"
) {
	const CatalogueEntry *entry = FindCatalogueEntry("pc.checker");
	const detail::Executor executor = detail::FindExecutor("pc.checker");
	REQUIRE(entry);
	REQUIRE(executor);
	Node authored{"checker", "pc.checker", "", {}, {}};
	EvaluationRequest request;
	detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	detail::NodeContext context(authored, *entry, request, budget);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.Values = {
		{"dimension",
		 ArrayValue{
			 ValueType::Vector2, {Vector2{200, 200}, Vector2{-std::numeric_limits<double>::max(), -1}}
		 }},
		{"dimension_unit", EnumValue{2}}
	};
	ImageArray masks;
	masks.Images = {
		Image{1, 1, std::vector<uint8_t>(4, 255), 0}, Image{2, 1, std::vector<uint8_t>(8, 255), 0}
	};
	masks.Items = {ImageArrayItem{size_t{0}}, ImageArrayItem{size_t{1}}};
	context.ImageArrays.emplace_back("mask", &masks);
	context.ProcessorCount = 2;
	context.InputProvenanceResolved = true;
	CHECK_FALSE(detail::RunProcessorBatch(context, executor, nullptr, nullptr));
	CHECK(context.FailureCode == Status::InvalidValue);
	CHECK(context.FailurePort == "dimension");
	CHECK(context.OutputImages.empty());
	CHECK(budget.Peak() < 64 * 1024);
}

TEST_CASE(
	"Source generator preflight rejects negative project multiplication overflow before row allocation",
	"[source_generator_dimension]"
) {
	const CatalogueEntry *entry = FindCatalogueEntry("pc.checker");
	const detail::Executor executor = detail::FindExecutor("pc.checker");
	REQUIRE(entry);
	REQUIRE(executor);
	Node authored{"checker", "pc.checker", "", {}, {}};
	EvaluationRequest request;
	detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	detail::NodeContext context(authored, *entry, request, budget);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.Project.SurfaceWidth = 2;
	context.Project.SurfaceHeight = 2;
	context.Values = {
		{"dimension",
		 ArrayValue{
			 ValueType::Vector2, {Vector2{200, 200}, Vector2{-std::numeric_limits<double>::max(), -1}}
		 }},
		{"dimension_unit", EnumValue{1}}
	};
	context.ProcessorCount = 2;
	context.InputProvenanceResolved = true;
	CHECK_FALSE(detail::RunProcessorBatch(context, executor, nullptr, nullptr));
	CHECK(context.FailureCode == Status::InvalidValue);
	CHECK(context.FailurePort == "dimension");
	CHECK(context.OutputImages.empty());
	CHECK(budget.Peak() < 64 * 1024);
}

TEST_CASE(
	"Source generator preflights later Mask dimensions before allocating the first row",
	"[source_generator_dimension]"
) {
	const CatalogueEntry *entry = FindCatalogueEntry("pc.checker");
	const detail::Executor executor = detail::FindExecutor("pc.checker");
	REQUIRE(entry);
	REQUIRE(executor);
	Node authored{"checker", "pc.checker", "", {}, {}};
	EvaluationRequest request;
	detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	detail::NodeContext context(authored, *entry, request, budget);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.Values = {
		{"dimension", ArrayValue{ValueType::Vector2, {Vector2{200, 200}, Vector2{3, 1}}}},
		{"dimension_unit", EnumValue{2}}
	};
	ImageArray masks;
	masks.Images = {
		Image{1, 1, std::vector<uint8_t>(4, 255), 0}, Image{2048, 1, std::vector<uint8_t>(2048 * 4, 255), 0}
	};
	masks.Items = {ImageArrayItem{size_t{0}}, ImageArrayItem{size_t{1}}};
	context.ImageArrays.emplace_back("mask", &masks);
	context.ProcessorCount = 2;
	context.InputProvenanceResolved = true;
	CHECK_FALSE(detail::RunProcessorBatch(context, executor, nullptr, nullptr));
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.FailurePort == "dimension");
	CHECK(context.OutputImages.empty());
	CHECK(budget.Peak() < 64 * 1024);
}
