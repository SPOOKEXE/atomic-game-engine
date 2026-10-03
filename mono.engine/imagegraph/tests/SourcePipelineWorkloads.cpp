#include "fixtures/SourcePipelineWorkloads.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.source_pipeline_workloads")
using Fixture = engine::imagegraph::testing::SourcePipelineFixture;
TEST_CASE(
	"Source pipeline workloads match independent full-output hashes after fresh replay",
	"[imagegraph][source_pipeline_workloads]"
) {
	for (const auto kind : {Fixture::Kind::Fft, Fixture::Kind::Path, Fixture::Kind::Strand}) {
		Fixture graph(kind);
		graph.Run();
		graph.Validate();
		const auto first = graph.Actual;
		const auto state = graph.Evaluated.Data;
		graph.Run();
		graph.Validate();
		CHECK(graph.Actual == first);
		CHECK(graph.Evaluated.Data == state);
		const auto before = graph.Evaluated;
		engine::imagegraph::EvaluationRequest request;
		request.SimulationAuthoringRevision = 1;
		request.BuiltinRandomCaptures = {
			&graph.Capture, kind == Fixture::Kind::Strand ? size_t{1} : size_t{0}
		};
		CHECK(
			engine::imagegraph::EvaluateStateful(
				graph.Authored, graph.Compiled, "out", request, graph.Evaluated, graph.Failure, 1
			) == engine::imagegraph::Status::LimitExceeded
		);
		CHECK(graph.Evaluated.Data == before.Data);
		CHECK(
			std::get<engine::imagegraph::EvaluatedValue>(graph.Evaluated.Output).Data ==
			std::get<engine::imagegraph::EvaluatedValue>(before.Output).Data
		);
	}
}
