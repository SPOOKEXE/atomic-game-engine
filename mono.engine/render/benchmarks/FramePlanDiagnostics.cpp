#include "../src/FramePlanDiagnostics.hpp"

#include <engine/graph/PipelineDocument.hpp>
#include <engine/graph/PipelineProfile.hpp>
#include <engine/testing/Bench.hpp>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

TEST_SUITE_ID("engine.render.bench.frameplan")

namespace {
	using namespace engine::graph;
	using engine::core::Name;
	using engine::render::FramePlanDiagnosticCache;
	using engine::render::FramePlanDiagnosticResult;
	using engine::render::ResolveFramePlanDiagnostics;
	using engine::testing::Consume;

	struct DefaultPipeline {
		RenderGraph Graph;
		ExecutionSchedule Schedule;
		uint32_t CommandBuffers = 0;
	};

	const DefaultPipeline &InstalledDefaultPipeline() {
		static const DefaultPipeline pipeline = [] {
			DefaultPipeline built;
			const PipelineDocument document = DefaultPbrDocument();
			Name offender;
			if (Build(document, built.Graph, offender) != PipelineDocumentStatus::Ok) {
				throw std::runtime_error("default PBR pipeline document did not build");
			}
			if (CompileSchedule(built.Graph, built.Schedule, offender) != ScheduleStatus::Ok) {
				throw std::runtime_error("default PBR pipeline schedule did not compile");
			}
			built.CommandBuffers = static_cast<uint32_t>(PlanCommandBuffers(built.Schedule).size());
			return built;
		}();
		return pipeline;
	}

	const std::vector<uint64_t> &Worlds(size_t viewCount, size_t distinctWorldCount) {
		static std::array<std::vector<uint64_t>, 2> shapes;
		static bool initialised = false;
		if (!initialised) {
			shapes[0] = {1};
			shapes[1] = {1, 2, 3, 4};
			initialised = true;
		}
		if (viewCount == 1 && distinctWorldCount == 1) return shapes[0];
		return shapes[1];
	}

	void ConsumeResult(const FramePlanDiagnosticResult &result) {
		Consume(static_cast<uint64_t>(result.Status));
		Consume(result.Totals.ReadBytes);
		Consume(result.Totals.WriteBytes);
		Consume(result.Totals.QueueTransferBytes);
		Consume(result.Totals.ConcurrentWaves);
		Consume(result.Totals.CommandBuffers);
		Consume(result.CacheHit);
	}
}

BENCH("default PBR diagnostics miss · 1 view · 1 world", 1000) {
	const DefaultPipeline &pipeline = InstalledDefaultPipeline();
	const std::vector<uint64_t> &worlds = Worlds(1, 1);
	FramePlanDiagnosticCache cache;
	engine::core::Name offender;
	for (size_t call = 0; call < 1000; call++) {
		const uint32_t width = call % 2 == 0 ? 1920 : 1921;
		ConsumeResult(ResolveFramePlanDiagnostics(
			pipeline.Graph,
			pipeline.Schedule,
			1,
			worlds,
			width,
			1080,
			pipeline.CommandBuffers,
			cache,
			offender
		));
	}
}

BENCH("default PBR diagnostics hit · 1 view · 1 world", 1000000) {
	const DefaultPipeline &pipeline = InstalledDefaultPipeline();
	const std::vector<uint64_t> &worlds = Worlds(1, 1);
	static FramePlanDiagnosticCache cache;
	engine::core::Name offender;
	for (size_t call = 0; call < 1000000; call++) {
		ConsumeResult(ResolveFramePlanDiagnostics(
			pipeline.Graph, pipeline.Schedule, 1, worlds, 1920, 1080, pipeline.CommandBuffers, cache, offender
		));
	}
}

BENCH("default PBR diagnostics miss · 4 views · 4 worlds", 500) {
	const DefaultPipeline &pipeline = InstalledDefaultPipeline();
	const std::vector<uint64_t> &worlds = Worlds(4, 4);
	FramePlanDiagnosticCache cache;
	engine::core::Name offender;
	for (size_t call = 0; call < 500; call++) {
		const uint32_t width = call % 2 == 0 ? 1920 : 1921;
		ConsumeResult(ResolveFramePlanDiagnostics(
			pipeline.Graph,
			pipeline.Schedule,
			1,
			worlds,
			width,
			1080,
			pipeline.CommandBuffers,
			cache,
			offender
		));
	}
}

BENCH("default PBR diagnostics hit · 4 views · 4 worlds", 1000000) {
	const DefaultPipeline &pipeline = InstalledDefaultPipeline();
	const std::vector<uint64_t> &worlds = Worlds(4, 4);
	static FramePlanDiagnosticCache cache;
	engine::core::Name offender;
	for (size_t call = 0; call < 1000000; call++) {
		ConsumeResult(ResolveFramePlanDiagnostics(
			pipeline.Graph, pipeline.Schedule, 1, worlds, 1920, 1080, pipeline.CommandBuffers, cache, offender
		));
	}
}
