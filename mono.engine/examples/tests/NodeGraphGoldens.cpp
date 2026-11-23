// Checked-in render pipelines run through the shared document reader and the
// ang-backed engine executor, compared with output recorded before either moved.

#include <engine/core/Paths.hpp>
#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/graph/RenderStage.hpp>
#include <engine/graph/Schedule.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.examples.nodegraphgoldens")
TEST_DEPENDS("engine.graph.pipelinedocument")
TEST_DEPENDS("engine.graph.renderstage")

namespace {
	using engine::core::Name;
	using namespace engine::graph;

	struct StagedAssets {
		std::filesystem::path Previous = engine::core::Paths::Assets();

		StagedAssets() {
			engine::core::Paths::SetAssetsOverride(engine::core::Paths::Base().parent_path() / "assets");
		}

		~StagedAssets() {
			engine::core::Paths::SetAssetsOverride(Previous);
		}
	};

	uint64_t Fnv(std::string_view text) {
		uint64_t hash = 0xcbf29ce484222325ull;
		for (const char character : text) {
			hash ^= static_cast<unsigned char>(character);
			hash *= 0x100000001b3ull;
		}
		return hash;
	}

	std::string LoadText(std::string_view name) {
		const std::filesystem::path path =
			engine::core::Paths::Assets() / "examples" / "pipelines" / std::string(name);
		std::ifstream input(path, std::ios::binary);
		REQUIRE(input);
		return std::string(std::istreambuf_iterator<char>(input), {});
	}

	// Every invocation as "name@view/world", in executor order.
	struct Trace final : NodeRunner {
		std::string Text;
		size_t Calls = 0;
		bool Run(const RunContext &context) override {
			Text += context.Name.Text();
			Text += '@' + std::to_string(static_cast<int64_t>(context.View)) + '/' +
					std::to_string(static_cast<int64_t>(context.World)) + ';';
			++Calls;
			return true;
		}
	};

	struct Golden {
		std::string_view File;
		size_t Edits = 0;
		uint64_t Written = 0;
		size_t Calls = 0;
		uint64_t Executed = 0;
	};

	// Recorded from the pre-migration reader and EngineGraph executor.
	constexpr Golden GOLDENS[] = {
		{"RenderFeatures.pipeline", 219, 0x9de0f27c07b2f063ull, 178, 0xe5d8e7a239d171a5ull},
		{"CompositorDemo.pipeline", 253, 0x444661df73d2f7baull, 196, 0x515d8a240946f085ull},
		{"RaytraceDemo.pipeline", 61, 0x1c4327df3a070163ull, 38, 0x54c3e80dfeb4c0f1ull},
		{"PathtraceDemo.pipeline", 70, 0x8e209a4af2b75478ull, 44, 0x3e952f8e03315ea9ull},
	};
}

TEST_CASE("checked-in render pipelines read, write and execute as recorded", "[examples][graph][golden]") {
	const StagedAssets assets;
	RegisterRenderNodeKinds();
	for (const Golden &golden : GOLDENS) {
		INFO(golden.File);
		const std::string text = LoadText(golden.File);
		PipelineDocument document;
		Name offender;
		REQUIRE(Read(text, document, offender) == PipelineDocumentStatus::Ok);
		const std::string written = Write(document);
		CHECK(written == text);

		RenderGraph graph;
		REQUIRE(Build(document, graph, offender) == PipelineDocumentStatus::Ok);
		CompiledGraph compiled;
		REQUIRE(graph.Compile(compiled, offender) == GraphStatus::Ok);
		ExecutionSchedule schedule;
		REQUIRE(CompileSchedule(graph, schedule, offender) == ScheduleStatus::Ok);
		RenderStageProjection authored;
		RenderStageProjection scheduled;
		REQUIRE(ProjectRenderStage(graph, compiled, authored, offender) == EngineGraphStatus::Ok);
		REQUIRE(ProjectRenderStage(graph, compiled, schedule, scheduled, offender) == EngineGraphStatus::Ok);

		const std::vector<uint64_t> worlds{7, 7, 9};
		Trace trace;
		REQUIRE(ExecuteRenderStage(graph, authored, trace, worlds));
		REQUIRE(ExecuteRenderStage(graph, scheduled, trace, worlds));
		CHECK(document.Count() == golden.Edits);
		CHECK(Fnv(written) == golden.Written);
		CHECK(trace.Calls == golden.Calls);
		CHECK(Fnv(trace.Text) == golden.Executed);
	}
}
