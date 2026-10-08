#include "nodes/Path.hpp"

#include <engine/core/FrameGraph.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>

TEST_SUITE_ID("engine.imagegraph.source_smooth_path_workloads")
using namespace engine::imagegraph;
namespace {
	struct ProfileFrame {
		bool PreviouslyEnabled = engine::core::FrameGraph::IsEnabled();
		ProfileFrame() {
			engine::core::FrameGraph::SetEnabled(true);
			engine::core::FrameGraph::BeginFrame();
		}
		~ProfileFrame() {
			engine::core::FrameGraph::EndFrame();
			engine::core::FrameGraph::SetEnabled(PreviouslyEnabled);
		}
	};
	Document DensePath() {
		ArrayValue anchors;
		anchors.ElementType = ValueType::Vector2;
		for (size_t i = 0; i < Limits::MaximumPathAnchors; ++i)
			anchors.Elements.push_back(Vector2{32. * i, 0});
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"smooth", "pc.path_smooth", "", {}, {{"normalized_length", false}, {"sample_path", .5}}}
		};
		document.Nodes[0].DynamicInputs = {{"anchor_0", ValueType::Vector2, Value{std::move(anchors)}}};
		document.Outputs = {{"path", "smooth", "path_data"}, {"position", "smooth", "position_out"}};
		return document;
	}
}

TEST_CASE(
	"Smooth dense source geometry stays bounded and repeat sampling retains its exact metrics",
	"[imagegraph][path_smooth][workload]"
) {
	ProfileFrame frame;
	ENGINE_PROFILE("imagegraph.path_smooth.workload");
	auto document = DensePath();
	Plan plan;
	Diagnostic diagnostic;
	const auto compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	StatefulEvaluationResult result;
	EvaluationRequest request;
	request.DataReplay = &result.Data;
	auto status = EvaluateStateful(document, plan, "path", request, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	const auto path = std::get<Path2D>(std::get<EvaluatedValue>(result.Output).Data);
	REQUIRE(path.Anchors.size() == Limits::MaximumPathAnchors);
	REQUIRE(path.SourceSmooth);
	CHECK_FALSE(path.SourceSmooth->NormalizedLength);
	REQUIRE(result.Data.Entries.size() == 1);
	REQUIRE(result.Data.Entries[0].Values.size() == 1);
	const auto position = std::get<Vector2>(result.Data.Entries[0].Values[0].Data);
	CHECK(position.X == 32. * Limits::MaximumPathAnchors / 2);
	CHECK(position.Y == 0);
	Node sample{"sample", "pc.path_sample", "", {}, {}};
	detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	{
		detail::NodeContext context(sample, *FindCatalogueEntry(sample.Type), request, budget);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		context.Values = {{"path", path}};
		detail::SourcePathShiftMemo memo;
		context.PathShiftMemo = &memo;
		REQUIRE(detail::StampSourcePathShiftInputs(context));
		detail::PathRuntime runtime;
		REQUIRE(runtime.Init(context, std::get<Path2D>(context.Values[0].second)));
		const auto t = 31. / 32;
		const double expected = 31. * (Limits::MaximumPathAnchors - 3) + 32. * (2 * t * t - t * t * t) +
								32. * (t + t * t - t * t * t);
		CHECK(runtime.Length() == Catch::Approx(expected).margin(1e-7));
		CHECK(runtime.SegmentCount() == Limits::MaximumPathAnchors - 1);
		std::array<detail::PathPoint, 128> points;
		for (size_t i = 0; i < points.size(); ++i) {
			points[i] = runtime.PointRatio((double(i) + .5) / points.size());
			REQUIRE(std::isfinite(points[i].X));
			CHECK(points[i].Y == 0);
			CHECK(points[i].Weight == 1);
		}
		for (size_t i = 0; i < points.size(); ++i) {
			const auto repeated = runtime.PointRatio((double(i) + .5) / points.size());
			CHECK(repeated.X == points[i].X);
			CHECK(repeated.Y == points[i].Y);
		}
		CHECK(context.FailureCode == Status::Ok);
		CHECK(budget.Used() > 0);
	}
	CHECK(budget.Used() == 0);
	const auto prior = result.Data;
	const auto priorOutput = std::get<EvaluatedValue>(result.Output);
	request.Tick = 1;
	CHECK(
		EvaluateStateful(document, plan, "position", request, result, diagnostic, 1) == Status::LimitExceeded
	);
	CHECK(result.Data == prior);
	CHECK(std::get<EvaluatedValue>(result.Output) == priorOutput);
	REQUIRE(EvaluateStateful(document, plan, "position", request, result, diagnostic) == Status::Ok);
	CHECK(std::get<Vector2>(std::get<EvaluatedValue>(result.Output).Data) == position);
}
