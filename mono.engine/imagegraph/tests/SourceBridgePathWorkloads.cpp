#include "nodes/Path.hpp"

#include <engine/core/FrameGraph.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>

TEST_SUITE_ID("engine.imagegraph.source_bridge_path_workloads")
using namespace engine::imagegraph;
namespace {
	struct ProfileFrame {
		bool Enabled = engine::core::FrameGraph::IsEnabled();
		ProfileFrame() {
			engine::core::FrameGraph::SetEnabled(true);
			engine::core::FrameGraph::BeginFrame();
		}
		~ProfileFrame() {
			engine::core::FrameGraph::EndFrame();
			engine::core::FrameGraph::SetEnabled(Enabled);
		}
	};
	Document DenseBridge() {
		Path2D source;
		auto &combined = source.SourceOperation.emplace();
		combined.Kind = SourcePathOperationKind::Combine;
		for (size_t i = 0; i < 8; ++i) {
			Path2D line;
			line.Anchors = {{{0, 8. * i, 0, 0, 0, 0}, 0}, {{32, 8. * i, 0, 0, 0, 0}, 0}};
			line.Weights = {{0, double(i + 1)}, {100, double(i + 1)}};
			combined.Inputs.push_back(std::move(line));
		}
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"bridge", "pc.path_bridge", "", {}, {{"path", source}, {"amount", int64_t{64}}, {"seed", 17.}}}
		};
		document.Outputs = {{"path", "bridge", "path"}};
		return document;
	}
	void HashNumber(uint64_t &hash, double number) {
		REQUIRE(std::isfinite(number));
		const auto quantized = uint64_t(std::llround(number * 1000000));
		for (unsigned shift = 0; shift < 64; shift += 8)
			hash = (hash ^ uint8_t(quantized >> shift)) * 1099511628211ull;
	}
}
TEST_CASE(
	"Bridge dense line grid preserves source metadata and its independently derived sample hash",
	"[imagegraph][path_bridge][workload]"
) {
	ProfileFrame frame;
	ENGINE_PROFILE("imagegraph.path_bridge.workload");
	const auto document = DenseBridge();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	StatefulEvaluationResult result;
	EvaluationRequest request;
	request.DataReplay = &result.Data;
	const auto status = EvaluateStateful(document, plan, "path", request, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	const auto path = std::get<Path2D>(std::get<EvaluatedValue>(result.Output).Data);
	REQUIRE(path.SourceOperation);
	REQUIRE(path.SourceOperation->Bridge);
	CHECK(path.SourceOperation->Bridge->LineCount == 64);
	REQUIRE(path.SourceOperation->Bridge->Lines.size() == 64);
	Node node{"sample", "pc.path_sample", "", {}, {}};
	detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	uint64_t hash = 14695981039346656037ull;
	{
		detail::NodeContext context(node, *FindCatalogueEntry(node.Type), request, budget);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		detail::SourcePathShiftMemo memo;
		context.PathShiftMemo = &memo;
		context.Values = {{"path", path}};
		REQUIRE(detail::StampSourcePathShiftInputs(context));
		detail::PathRuntime runtime;
		REQUIRE(runtime.Init(context, std::get<Path2D>(context.Values[0].second)));
		CHECK(runtime.LineCount() == 64);
		for (size_t line = 0; line < 64; ++line) {
			CHECK(runtime.SegmentCount(line) == 8);
			CHECK(runtime.Length(line) == 56);
			CHECK(runtime.AccumulatedCount(line) == 7);
			for (size_t segment = 0; segment < 7; ++segment)
				CHECK(runtime.AccumulatedAt(segment, line) == 8. * (segment + 1));
			for (size_t sample = 0; sample <= 16; ++sample) {
				const auto point = runtime.PointRatio(double(sample) / 16, line);
				HashNumber(hash, point.X);
				HashNumber(hash, point.Y);
				HashNumber(hash, point.Weight);
			}
		}
		CHECK(context.FailureCode == Status::Ok);
		CHECK(budget.Used() == 0);
		CHECK(memo.LookupWork == 64 * 67);
		CHECK(memo.Entries.empty());
	}
	CHECK(budget.Used() == 0);
	// Closed-form source grid: X=32*min(row/63,.999), Y=56*t, weight=1+7*t.
	CHECK(hash == 15941574929047459686ull);
	const auto data = result.Data;
	const auto output = std::get<EvaluatedValue>(result.Output);
	request.Tick = 1;
	CHECK(EvaluateStateful(document, plan, "path", request, result, diagnostic, 1) == Status::LimitExceeded);
	CHECK(result.Data == data);
	CHECK(std::get<EvaluatedValue>(result.Output) == output);
	REQUIRE(EvaluateStateful(document, plan, "path", request, result, diagnostic) == Status::Ok);
	CHECK(std::get<Path2D>(std::get<EvaluatedValue>(result.Output).Data) == path);
}
