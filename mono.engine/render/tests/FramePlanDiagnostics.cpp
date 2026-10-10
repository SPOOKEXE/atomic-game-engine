#include "../src/FramePlanDiagnostics.hpp"

#include <engine/graph/PipelineDocument.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <utility>

TEST_SUITE_ID("engine.render.frameplan_diagnostics")

namespace {
	using namespace engine;
	using namespace engine::graph;

	ScheduleStatus BuildPipeline(RenderGraph &graph, ExecutionSchedule &schedule, ResourceFormat format) {
		const ResourceId output =
			graph.AddResource({.Name = core::Name("colour"), .Kind = ResourceKind::Colour, .Format = format});
		Node draw{.Name = core::Name("draw"), .Kind = core::Name("draw"), .Scope = NodeScope::View};
		draw.Writes = {output};
		graph.AddNode(draw);
		core::Name offender;
		return CompileSchedule(graph, schedule, offender);
	}

	void CheckMatchesPlan(
		const render::FramePlanDiagnosticTotals &totals,
		const FrameExecutionPlan &plan,
		uint32_t commandBuffers
	) {
		CHECK(totals.ReadBytes == plan.ReadBytes);
		CHECK(totals.WriteBytes == plan.WriteBytes);
		CHECK(totals.QueueTransferBytes == plan.QueueTransferBytes);
		const uint32_t concurrentWaves = static_cast<uint32_t>(
			std::count_if(plan.Waves.begin(), plan.Waves.end(), [](const PlannedWave &wave) {
				return wave.ConcurrentQueues;
			})
		);
		CHECK(totals.ConcurrentWaves == concurrentWaves);
		CHECK(totals.CommandBuffers == commandBuffers);
	}
}

TEST_CASE("frame plan diagnostic hits match direct planning", "[render][frame-plan]") {
	RenderGraph graph;
	ExecutionSchedule schedule;
	REQUIRE(BuildPipeline(graph, schedule, ResourceFormat::RGBA8) == ScheduleStatus::Ok);
	const std::array<uint64_t, 3> worlds{7, 7, 9};

	FrameExecutionPlan direct;
	core::Name directOffender;
	REQUIRE(PlanFrame(graph, schedule, worlds, 32, 24, direct, directOffender) == ExecutionPlanStatus::Ok);

	render::FramePlanDiagnosticCache cache;
	core::Name offender;
	const render::FramePlanDiagnosticResult miss =
		render::ResolveFramePlanDiagnostics(graph, schedule, 1, worlds, 32, 24, 3, cache, offender);
	CHECK(miss.Status == ExecutionPlanStatus::Ok);
	CHECK_FALSE(miss.CacheHit);
	CheckMatchesPlan(miss.Totals, direct, 3);

	offender = core::Name("sentinel");
	const render::FramePlanDiagnosticResult hit =
		render::ResolveFramePlanDiagnostics(graph, schedule, 1, worlds, 32, 24, 3, cache, offender);
	CHECK(hit.Status == ExecutionPlanStatus::Ok);
	CHECK(hit.CacheHit);
	CHECK_FALSE(offender.IsValid());
	CHECK(hit.Totals.ReadBytes == miss.Totals.ReadBytes);
	CHECK(hit.Totals.WriteBytes == miss.Totals.WriteBytes);
	CHECK(hit.Totals.QueueTransferBytes == miss.Totals.QueueTransferBytes);
	CHECK(hit.Totals.ConcurrentWaves == miss.Totals.ConcurrentWaves);
}

TEST_CASE("frame plan cache keys dimensions and the complete ordered world list", "[render][frame-plan]") {
	RenderGraph graph;
	ExecutionSchedule schedule;
	REQUIRE(BuildPipeline(graph, schedule, ResourceFormat::RGBA8) == ScheduleStatus::Ok);
	const std::array<uint64_t, 3> worlds{7, 7, 9};
	const std::array<uint64_t, 3> reordered{9, 7, 7};
	const std::array<uint64_t, 2> fewerDuplicates{7, 9};

	render::FramePlanDiagnosticCache cache;
	core::Name offender;
	const auto resolve = [&](std::span<const uint64_t> inputWorlds, uint32_t width) {
		return render::ResolveFramePlanDiagnostics(
			graph, schedule, 1, inputWorlds, width, 24, 3, cache, offender
		);
	};

	CHECK_FALSE(resolve(worlds, 32).CacheHit);
	CHECK(resolve(worlds, 32).CacheHit);
	CHECK_FALSE(
		render::ResolveFramePlanDiagnostics(graph, schedule, 1, worlds, 32, 25, 3, cache, offender).CacheHit
	);
	CHECK(
		render::ResolveFramePlanDiagnostics(graph, schedule, 1, worlds, 32, 25, 3, cache, offender).CacheHit
	);
	CHECK_FALSE(resolve(worlds, 40).CacheHit);
	CHECK(resolve(worlds, 40).CacheHit);
	CHECK_FALSE(resolve(reordered, 40).CacheHit);
	CHECK(resolve(reordered, 40).CacheHit);
	CHECK_FALSE(resolve(fewerDuplicates, 40).CacheHit);
	CHECK(resolve(fewerDuplicates, 40).CacheHit);
	CHECK_FALSE(
		render::ResolveFramePlanDiagnostics(graph, schedule, 1, fewerDuplicates, 40, 24, 4, cache, offender)
			.CacheHit
	);
}

TEST_CASE("default PBR cached diagnostics match multiworld planning", "[render][frame-plan]") {
	RenderGraph graph;
	ExecutionSchedule schedule;
	core::Name offender;
	const PipelineDocument document = DefaultPbrDocument();
	REQUIRE(Build(document, graph, offender) == PipelineDocumentStatus::Ok);
	REQUIRE(CompileSchedule(graph, schedule, offender) == ScheduleStatus::Ok);
	const std::array<uint64_t, 4> worlds{12, 12, 19, 23};
	FrameExecutionPlan direct;
	REQUIRE(PlanFrame(graph, schedule, worlds, 1920, 1080, direct, offender) == ExecutionPlanStatus::Ok);
	REQUIRE(direct.QueueTransferBytes > 0);

	render::FramePlanDiagnosticCache cache;
	const uint32_t commandBuffers = static_cast<uint32_t>(PlanCommandBuffers(schedule).size());
	const auto miss = render::ResolveFramePlanDiagnostics(
		graph, schedule, 1, worlds, 1920, 1080, commandBuffers, cache, offender
	);
	CHECK(miss.Status == ExecutionPlanStatus::Ok);
	CHECK_FALSE(miss.CacheHit);
	CheckMatchesPlan(miss.Totals, direct, commandBuffers);
	const auto hit = render::ResolveFramePlanDiagnostics(
		graph, schedule, 1, worlds, 1920, 1080, commandBuffers, cache, offender
	);
	CHECK(hit.Status == ExecutionPlanStatus::Ok);
	CHECK(hit.CacheHit);
	CheckMatchesPlan(hit.Totals, direct, commandBuffers);
}

TEST_CASE("frame plan cache revision separates changed installed pipelines", "[render][frame-plan]") {
	RenderGraph rgba8;
	RenderGraph rgba16f;
	ExecutionSchedule schedule8;
	ExecutionSchedule schedule16f;
	REQUIRE(BuildPipeline(rgba8, schedule8, ResourceFormat::RGBA8) == ScheduleStatus::Ok);
	REQUIRE(BuildPipeline(rgba16f, schedule16f, ResourceFormat::RGBA16F) == ScheduleStatus::Ok);
	const std::array<uint64_t, 1> worlds{5};

	render::FramePlanDiagnosticCache cache;
	core::Name offender;
	const auto first =
		render::ResolveFramePlanDiagnostics(rgba8, schedule8, 10, worlds, 64, 48, 2, cache, offender);
	const auto replaced =
		render::ResolveFramePlanDiagnostics(rgba16f, schedule16f, 11, worlds, 64, 48, 2, cache, offender);
	CHECK_FALSE(first.CacheHit);
	CHECK_FALSE(replaced.CacheHit);
	CHECK(replaced.Totals.WriteBytes > first.Totals.WriteBytes);
	CHECK(
		render::ResolveFramePlanDiagnostics(rgba16f, schedule16f, 11, worlds, 64, 48, 2, cache, offender)
			.CacheHit
	);
}

TEST_CASE("frame plan failures retain planner status and offender without caching", "[render][frame-plan]") {
	RenderGraph graph;
	Node broken{.Name = core::Name("missing-resource"), .Kind = core::Name("draw"), .Scope = NodeScope::View};
	broken.Reads = {ResourceId{99}};
	const NodeId brokenId = graph.AddNode(broken);
	ExecutionSchedule schedule;
	ExecutionWave wave;
	wave.Nodes.push_back({.Node = brokenId});
	schedule.Waves.push_back(std::move(wave));
	const std::array<uint64_t, 1> worlds{5};
	render::FramePlanDiagnosticCache cache;
	core::Name offender;

	const auto first =
		render::ResolveFramePlanDiagnostics(graph, schedule, 1, worlds, 64, 48, 2, cache, offender);
	CHECK(first.Status == ExecutionPlanStatus::MissingResource);
	CHECK_FALSE(first.CacheHit);
	CHECK(offender == core::Name("missing-resource"));
	const auto second =
		render::ResolveFramePlanDiagnostics(graph, schedule, 1, worlds, 64, 48, 2, cache, offender);
	CHECK(second.Status == ExecutionPlanStatus::MissingResource);
	CHECK_FALSE(second.CacheHit);
	CHECK(offender == core::Name("missing-resource"));

	offender = core::Name("stale");
	const auto invalidDimensions =
		render::ResolveFramePlanDiagnostics(graph, schedule, 2, worlds, 0, 48, 2, cache, offender);
	CHECK(invalidDimensions.Status == ExecutionPlanStatus::InvalidDimensions);
	CHECK_FALSE(invalidDimensions.CacheHit);
	CHECK_FALSE(offender.IsValid());
}
