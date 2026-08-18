#include <engine/graph/ExecutionPlan.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

TEST_SUITE_ID("engine.graph.executionplan")
TEST_DEPENDS("engine.graph.schedule")

using engine::core::Name;
using namespace engine::graph;

namespace {
	ResourceId Resource(RenderGraph &graph, const char *name, ResourceKind kind, ResourceFormat format) {
		return graph.AddResource({.Name = Name(name), .Kind = kind, .Format = format});
	}

	Node NodeOf(const char *name, NodeScope scope) {
		return {.Name = Name(name), .Kind = Name(name), .Scope = scope};
	}

	ExecutionSchedule Schedule(RenderGraph &graph) {
		ExecutionSchedule schedule;
		Name offender;
		REQUIRE(CompileSchedule(graph, schedule, offender) == ScheduleStatus::Ok);
		return schedule;
	}
}

TEST_CASE("a frame plan expands each execution scope", "[graph][execution-plan]") {
	RenderGraph graph;
	const ResourceId shadow = Resource(graph, "shadow", ResourceKind::Depth, ResourceFormat::D32F);
	const ResourceId colour = Resource(graph, "colour", ResourceKind::Colour, ResourceFormat::RGBA8);
	const ResourceId window = Resource(graph, "window", ResourceKind::Colour, ResourceFormat::RGBA8);

	Node world = NodeOf("shadow", NodeScope::World);
	world.Writes = {shadow};
	graph.AddNode(world);
	Node view = NodeOf("opaque", NodeScope::View);
	view.Reads = {shadow};
	view.Writes = {colour};
	graph.AddNode(view);
	Node frame = NodeOf("present", NodeScope::Frame);
	frame.Reads = {colour};
	frame.Writes = {window};
	graph.AddNode(frame);

	const ExecutionSchedule schedule = Schedule(graph);
	const std::array<uint64_t, 3> worlds = {7, 7, 9};
	FrameExecutionPlan plan;
	Name offender;
	REQUIRE(PlanFrame(graph, schedule, worlds, 100, 50, plan, offender) == ExecutionPlanStatus::Ok);
	REQUIRE(plan.Waves.size() == 3);
	REQUIRE(plan.Waves[0].Invocations.size() == 2);
	CHECK(plan.Waves[0].IndependentWorlds);
	CHECK(plan.Waves[0].Invocations[0].WorldKey == 7);
	CHECK(plan.Waves[0].Invocations[1].WorldKey == 9);
	REQUIRE(plan.Waves[1].Invocations.size() == 3);
	CHECK(plan.Waves[1].IndependentViews);
	CHECK(plan.Waves[1].Invocations[2].View == 2);
	CHECK(plan.Waves[1].Invocations[2].World == 1);
	REQUIRE(plan.Waves[2].Invocations.size() == 1);
	CHECK(plan.Waves[2].Invocations[0].View == RunContext::WHOLE_FRAME);
	CHECK(plan.Waves[2].Invocations[0].World == RunContext::WHOLE_FRAME);

	CHECK(plan.WriteBytes == 120000);
	CHECK(plan.ReadBytes == 120000);
}

TEST_CASE("queue transfers are tracked per world resource instance", "[graph][execution-plan]") {
	RenderGraph graph;
	const ResourceId clusters = Resource(graph, "clusters", ResourceKind::Storage, ResourceFormat::R32F);
	const ResourceId colour = Resource(graph, "colour", ResourceKind::Colour, ResourceFormat::RGBA8);

	Node build = NodeOf("build-clusters", NodeScope::World);
	build.Writes = {clusters};
	build.Parameters = {{Name("queue"), "compute"}};
	graph.AddNode(build);
	Node draw = NodeOf("opaque", NodeScope::View);
	draw.Reads = {clusters};
	draw.Writes = {colour};
	draw.Parameters = {{Name("queue"), "graphics"}};
	graph.AddNode(draw);

	const ExecutionSchedule schedule = Schedule(graph);
	const std::array<uint64_t, 3> worlds = {4, 4, 8};
	FrameExecutionPlan plan;
	Name offender;
	REQUIRE(PlanFrame(graph, schedule, worlds, 16, 16, plan, offender) == ExecutionPlanStatus::Ok);
	REQUIRE(plan.Transfers.size() == 2);
	CHECK(plan.Transfers[0].World == 0);
	CHECK(plan.Transfers[1].World == 1);
	CHECK(plan.Transfers[0].From == ExecutionQueue::Compute);
	CHECK(plan.Transfers[0].To == ExecutionQueue::Graphics);
	CHECK(plan.QueueTransferBytes == 2048);
}

TEST_CASE("a headless plan still invokes world and frame work", "[graph][execution-plan]") {
	RenderGraph graph;
	const ResourceId shadow = Resource(graph, "shadow", ResourceKind::Depth, ResourceFormat::D32F);
	const ResourceId window = Resource(graph, "window", ResourceKind::Colour, ResourceFormat::RGBA8);
	Node world = NodeOf("shadow", NodeScope::World);
	world.Writes = {shadow};
	graph.AddNode(world);
	Node frame = NodeOf("interface", NodeScope::Frame);
	frame.Writes = {window};
	graph.AddNode(frame);

	const ExecutionSchedule schedule = Schedule(graph);
	FrameExecutionPlan plan;
	Name offender;
	REQUIRE(PlanFrame(graph, schedule, {}, 10, 10, plan, offender) == ExecutionPlanStatus::Ok);
	REQUIRE(plan.Waves.size() == 2);
	REQUIRE(plan.Waves[0].Invocations.size() == 1);
	CHECK(plan.Waves[0].Invocations[0].WorldKey == 0);
	REQUIRE(plan.Waves[1].Invocations.size() == 1);
}

TEST_CASE("zero view dimensions are refused without a partial plan", "[graph][execution-plan]") {
	RenderGraph graph;
	const ResourceId colour = Resource(graph, "colour", ResourceKind::Colour, ResourceFormat::RGBA8);
	Node view = NodeOf("opaque", NodeScope::View);
	view.Writes = {colour};
	graph.AddNode(view);

	const ExecutionSchedule schedule = Schedule(graph);
	FrameExecutionPlan plan;
	Name offender;
	CHECK(PlanFrame(graph, schedule, {}, 0, 10, plan, offender) == ExecutionPlanStatus::InvalidDimensions);
	CHECK(plan.Waves.empty());
}
