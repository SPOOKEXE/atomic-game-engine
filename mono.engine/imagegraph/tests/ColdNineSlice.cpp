#include "NodeExecutors.hpp"
#include "PixelBuilderPayload.hpp"
#include "ValueText.hpp"

#include <engine/imagegraph/CacheGroupReplay.hpp>
#include <engine/imagegraph/PcxExpression.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <sstream>

TEST_SUITE_ID("engine.imagegraph.cold_nine_slice")
using namespace engine::imagegraph;
namespace {
	Document ColdGraph() {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"bad",
			 "pc.solid",
			 "",
			 {},
			 {{"dimension", Vector2{1000000, 1000000}}, {"dimension_unit", EnumValue{0}}}},
			{"nine", "pc.9_slice", "", {}, {}},
			{"owner", "pc.cache", "", {}, {}}
		};
		document.Nodes.back().SourceProperties = {
			{"cache_group", ArrayValue{ValueType::Text, {std::string{"nine"}}}}
		};
		document.Links = {{"bad", "surface_out", "nine", "surface_in"}};
		document.Outputs = {
			{"out", "nine", "dyna_surf"}, {"getter", "nine", "surface_out"}, {"bad", "bad", "surface_out"}
		};
		return document;
	}
	DynamicSurfaceValue Cold() {
		DynamicSurfaceValue value;
		auto &data = value.Data.emplace();
		data.OwnerNodeId = "nine";
		data.BaseDimension = {1, 1};
		data.NineSlice.emplace().Cold = true;
		return value;
	}
}
TEST_CASE(
	"Cold Nine Slice freezes before linked producer execution and keeps absent surfaces",
	"[imagegraph][cache_group][source_nine_slice]"
) {
	auto document = ColdGraph();
	Diagnostic diagnostic;
	DataReplayState prior;
	REQUIRE(
		InitializeAuthoredCacheGroupReplay(
			document, {}, prior.CacheGroups, Limits::MaximumEvaluationBytes, diagnostic
		) == Status::Ok
	);
	for (auto &node : prior.CacheGroups.Nodes)
		node.RenderActive = false;
	const auto before = prior;
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	request.DataReplay = &prior;
	EvaluatedValue output;
	REQUIRE(EvaluateValue(document, plan, "out", request, output, diagnostic) == Status::Ok);
	const auto &value = std::get<DynamicSurfaceValue>(output.Data);
	REQUIRE(value.Data);
	REQUIRE(value.Data->NineSlice);
	CHECK(value == Cold());
	CHECK(value.Data->NineSlice->Source == Image{});
	CHECK(value.Data->BaseDimension == Vector2{1, 1});
	REQUIRE(ValueClonePayloadBytes(Value{value}));
	auto cloned = value;
	cloned.Data->OwnerNodeId = "independent";
	CHECK(value.Data->OwnerNodeId == "nine");
	Image getter{1, 1, {11, 22, 33, 44}};
	const auto pixels = getter;
	CHECK(Evaluate(document, plan, "getter", request, getter, diagnostic) != Status::Ok);
	CHECK(getter == pixels);
	CHECK(Evaluate(document, plan, "bad", request, getter, diagnostic) == Status::LimitExceeded);
	CHECK(prior == before);
	CacheGroupReplayState journal = prior.CacheGroups;
	CHECK(InitializeAuthoredCacheGroupReplay(document, {}, journal, 1, diagnostic) == Status::LimitExceeded);
	CHECK(journal == before.CacheGroups);
	StatefulEvaluationResult result;
	REQUIRE(EvaluateStateful(document, plan, "out", request, result, diagnostic) == Status::Ok);
	CHECK(result.Data.CacheGroups == before.CacheGroups);
}
TEST_CASE(
	"Cold Nine Slice native receipt owns absence and rejects malformed or unbounded state atomically",
	"[imagegraph][source_nine_slice][codec]"
) {
	const auto cold = Cold();
	std::ostringstream stream;
	detail::WriteValueText(stream, cold);
	REQUIRE(stream.good());
	Value restored = 9.;
	REQUIRE(detail::ReadValueText(stream.str(), restored));
	CHECK(restored == Value{cold});
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {{"receipt", "pc.string", "", {}, {}}};
	document.Nodes[0].SourceProperties = {{"owned_cold", cold}};
	Document parsed;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), parsed, diagnostic) == Status::Ok);
	CHECK(parsed == document);
	const auto retained = restored;
	CHECK_FALSE(detail::ReadValueText("ninecold 4 \"bad\"", restored));
	CHECK(restored == retained);
	detail::EvaluationBudget budget(1);
	detail::AllocationReservation charge;
	CHECK(detail::ReadValueText(stream.str(), restored, budget, charge) == Status::LimitExceeded);
	CHECK(restored == retained);
	CHECK(budget.Used() == 0);
	CacheGroupReplayState journal;
	const std::array outputs{CacheGroupReplayOutput{"dyna_surf", Value{cold}, {}, {}}};
	REQUIRE(
		RetainCacheGroupReplayNode(journal, "nine", "pc.9_slice", outputs, Limits::MaximumEvaluationBytes)
			.Code == Status::Ok
	);
	const auto original = journal;
	CHECK(
		RetainCacheGroupReplayNode(journal, "nine", "pc.9_slice", outputs, 1).Code == Status::LimitExceeded
	);
	CHECK(journal == original);
	auto malformed = cold;
	malformed.Data->NineSlice->Source = {1, 1, {1, 2, 3, 4}};
	CHECK_FALSE(detail::ValidPixelBuilderPayload(malformed));
	std::ostringstream rejected;
	detail::WriteValueText(rejected, malformed);
	CHECK_FALSE(rejected.good());
}
TEST_CASE(
	"Cold Nine Slice PCX draw preserves destination pixels and restores Normal blend",
	"[imagegraph][source_nine_slice][pcx]"
) {
	PcxExpressionValue program;
	Diagnostic diagnostic;
	REQUIRE(CompilePcxProgram("draw(nine,0,0)\ndraw(raw,1,0)", program, diagnostic) == Status::Ok);
	const std::array parameters{
		AuthoredValue{"nine", Cold()}, AuthoredValue{"raw", SurfaceValue{Image{1, 1, {255, 0, 0, 128}}}}
	};
	Image target{2, 1, {12, 24, 36, 255, 0, 0, 255, 255}};
	EvaluationRequest request;
	request.RequireSourceGpuRasterCoverage = true;
	PcxExecutionContext context{request, parameters, nullptr, {2, 1}, {}};
	context.Target = &target;
	context.DrawBlend = PcxDrawBlend::Override;
	PcxExecutionResult result;
	REQUIRE(ExecutePcxExpression(program, context, result, diagnostic) == Status::Ok);
	CHECK(target.Pixels == std::vector<uint8_t>{12, 24, 36, 255, 128, 0, 127, 191});
	PcxExpressionValue getters;
	REQUIRE(CompilePcxExpression("surface_get_dimension(nine)", getters, diagnostic) == Status::Ok);
	REQUIRE(ExecutePcxExpression(getters, context, result, diagnostic) == Status::Ok);
	CHECK(result.Data == Value{Vector2{1, 1}});
	for (const auto *getter : {"surface_get_width(nine)", "surface_get_height(nine)"}) {
		REQUIRE(CompilePcxExpression(getter, getters, diagnostic) == Status::Ok);
		REQUIRE(ExecutePcxExpression(getters, context, result, diagnostic) == Status::Ok);
		CHECK(result.Data == Value{1.});
	}
	Node drawNode{"draw", "pc.pb_draw_surface", "", {}, {}};
	const auto *entry = FindCatalogueEntry(drawNode.Type);
	REQUIRE(entry);
	detail::NodeContext draw(drawNode, *entry, request);
	draw.ByteBudget = Limits::MaximumEvaluationBytes;
	draw.Project.SurfaceWidth = draw.Project.SurfaceHeight = 1;
	for (const auto &input : entry->Inputs)
		if (input.Id != "surface")
			if (auto value = CatalogueDefault(input)) draw.Values.emplace_back(input.Id, std::move(*value));
	draw.Values.emplace_back("surface", Cold());
	REQUIRE(detail::FindExecutor(drawNode.Type)(draw));
	REQUIRE(draw.FailureCode == Status::Ok);
	REQUIRE(draw.OutputImages.size() == 1);
	const auto &canvas = draw.OutputImages.front().second;
	CHECK(canvas.Width == 1);
	CHECK(canvas.Height == 1);
	CHECK(canvas.Pixels == std::vector<uint8_t>{0, 0, 0, 0});
}
