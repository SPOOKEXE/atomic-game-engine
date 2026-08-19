#include <engine/graph/ExecutionPlan.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <vector>

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

TEST_CASE("the traffic plan buckets nodes into command buffers by queue class", "[graph][execution-plan]") {
	RenderGraph graph;
	const ResourceId camera = Resource(graph, "camera", ResourceKind::Camera, ResourceFormat::R8);
	const ResourceId instances = Resource(graph, "instances", ResourceKind::Buffer, ResourceFormat::R8);
	const ResourceId colour = Resource(graph, "colour", ResourceKind::Colour, ResourceFormat::RGBA8);
	const ResourceId blurred = Resource(graph, "blurred", ResourceKind::Storage, ResourceFormat::R32F);
	const ResourceId shot = Resource(graph, "shot", ResourceKind::Colour, ResourceFormat::RGBA8);

	Node resolve = NodeOf("camera", NodeScope::View);
	resolve.Writes = {camera};
	const NodeId resolveId = graph.AddNode(resolve);
	Node upload = NodeOf("upload", NodeScope::View);
	upload.Writes = {instances};
	upload.Parameters = {{Name("queue"), "transfer"}};
	const NodeId uploadId = graph.AddNode(upload);
	Node draw = NodeOf("draw", NodeScope::View);
	draw.Reads = {camera, instances};
	draw.Writes = {colour};
	draw.Parameters = {{Name("queue"), "graphics"}};
	const NodeId drawId = graph.AddNode(draw);
	Node blur = NodeOf("blur", NodeScope::View);
	blur.Reads = {colour};
	blur.Writes = {blurred};
	blur.Parameters = {{Name("queue"), "compute"}};
	const NodeId blurId = graph.AddNode(blur);
	Node readback = NodeOf("readback", NodeScope::View);
	readback.Reads = {colour};
	readback.Writes = {shot};
	readback.Parameters = {{Name("queue"), "transfer"}};
	const NodeId readbackId = graph.AddNode(readback);

	const ExecutionSchedule schedule = Schedule(graph);
	const std::vector<PlannedCommandBuffer> buffers = PlanCommandBuffers(schedule);

	// The upload leads, the raster work follows, and the dependency-bound
	// readback and compute wave splits by class - transfer before compute.
	REQUIRE(buffers.size() == 4);
	CHECK(buffers[0].Class == CommandBufferClass::Transfer);
	REQUIRE(buffers[0].Nodes.size() == 1);
	CHECK(buffers[0].Nodes[0] == uploadId);
	CHECK(buffers[1].Class == CommandBufferClass::Graphics);
	REQUIRE(buffers[1].Nodes.size() == 1);
	CHECK(buffers[1].Nodes[0] == drawId);
	CHECK(buffers[2].Class == CommandBufferClass::Transfer);
	REQUIRE(buffers[2].Nodes.size() == 1);
	CHECK(buffers[2].Nodes[0] == readbackId);
	CHECK(buffers[3].Class == CommandBufferClass::Compute);
	REQUIRE(buffers[3].Nodes.size() == 1);
	CHECK(buffers[3].Nodes[0] == blurId);
	CHECK(buffers[2].FirstWave == buffers[3].FirstWave);

	// CPU work records no device commands, so no buffer names it.
	for (const PlannedCommandBuffer &buffer : buffers) {
		CHECK(std::find(buffer.Nodes.begin(), buffer.Nodes.end(), resolveId) == buffer.Nodes.end());
	}
}

TEST_CASE("adjacent waves of one queue class share a command buffer", "[graph][execution-plan]") {
	RenderGraph graph;
	const ResourceId clusters = Resource(graph, "clusters", ResourceKind::Storage, ResourceFormat::R32F);
	const ResourceId colour = Resource(graph, "colour", ResourceKind::Colour, ResourceFormat::RGBA8);
	const ResourceId display = Resource(graph, "display", ResourceKind::Colour, ResourceFormat::RGBA8);

	Node build = NodeOf("build-clusters", NodeScope::View);
	build.Writes = {clusters};
	build.Parameters = {{Name("queue"), "compute"}};
	const NodeId buildId = graph.AddNode(build);
	Node opaque = NodeOf("opaque", NodeScope::View);
	opaque.Writes = {colour};
	opaque.Parameters = {{Name("queue"), "graphics"}};
	const NodeId opaqueId = graph.AddNode(opaque);
	Node compose = NodeOf("compose", NodeScope::View);
	compose.Reads = {colour};
	compose.Writes = {display};
	compose.Parameters = {{Name("queue"), "graphics"}};
	const NodeId composeId = graph.AddNode(compose);

	const ExecutionSchedule schedule = Schedule(graph);
	const std::vector<PlannedCommandBuffer> buffers = PlanCommandBuffers(schedule);

	// The independent compute leads its shared wave, and the two graphics
	// waves merge - a boundary with graphics on both sides orders nothing.
	REQUIRE(buffers.size() == 2);
	CHECK(buffers[0].Class == CommandBufferClass::Compute);
	REQUIRE(buffers[0].Nodes.size() == 1);
	CHECK(buffers[0].Nodes[0] == buildId);
	CHECK(buffers[1].Class == CommandBufferClass::Graphics);
	REQUIRE(buffers[1].Nodes.size() == 2);
	CHECK(buffers[1].Nodes[0] == opaqueId);
	CHECK(buffers[1].Nodes[1] == composeId);
	CHECK(buffers[1].FirstWave == 0);
	CHECK(buffers[1].LastWave == 1);
}

TEST_CASE("a seam light capture and the lit pass share one graphics buffer", "[graph][execution-plan]") {
	// The portal light-field arrangement: portal-capture writes the captures,
	// deferred-lighting reads them. Both are graphics work, so the traffic plan
	// must keep them in one buffer with the capture recorded first - the order
	// the renderer's seam-spill binding depends on.
	RenderGraph graph;
	const ResourceId portalLight =
		Resource(graph, "portal-light", ResourceKind::Texture, ResourceFormat::RGBA8);
	const ResourceId lit = Resource(graph, "lit", ResourceKind::Colour, ResourceFormat::RGBA16F);

	Node capture = NodeOf("portal-capture", NodeScope::View);
	capture.Writes = {portalLight};
	capture.Parameters = {{Name("queue"), "graphics"}};
	const NodeId captureId = graph.AddNode(capture);
	Node lighting = NodeOf("deferred-lighting", NodeScope::View);
	lighting.Reads = {portalLight};
	lighting.Writes = {lit};
	lighting.Parameters = {{Name("queue"), "graphics"}};
	const NodeId lightingId = graph.AddNode(lighting);

	const ExecutionSchedule schedule = Schedule(graph);
	const std::vector<PlannedCommandBuffer> buffers = PlanCommandBuffers(schedule);

	REQUIRE(buffers.size() == 1);
	CHECK(buffers[0].Class == CommandBufferClass::Graphics);
	REQUIRE(buffers[0].Nodes.size() == 2);
	CHECK(buffers[0].Nodes[0] == captureId);
	CHECK(buffers[0].Nodes[1] == lightingId);
}

TEST_CASE("a cpu-only schedule plans no command buffers", "[graph][execution-plan]") {
	RenderGraph graph;
	const ResourceId entities = Resource(graph, "entities", ResourceKind::Entities, ResourceFormat::R8);
	Node world = NodeOf("world", NodeScope::World);
	world.Writes = {entities};
	graph.AddNode(world);

	const ExecutionSchedule schedule = Schedule(graph);
	CHECK(PlanCommandBuffers(schedule).empty());
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
