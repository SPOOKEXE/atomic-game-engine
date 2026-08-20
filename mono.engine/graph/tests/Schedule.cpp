#include <engine/graph/Schedule.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.graph.schedule")
TEST_DEPENDS("engine.graph.rendergraph")

using engine::core::Name;
using namespace engine::graph;

namespace {
	ResourceId Resource(RenderGraph &graph, const char *name, ResourceKind kind, bool external = false) {
		return graph.AddResource({.Name = Name(name), .Kind = kind, .External = external});
	}

	Node NodeOf(const char *name, const char *kind, NodeScope scope = NodeScope::View) {
		Node node;
		node.Name = Name(name);
		node.Kind = Name(kind);
		node.Scope = scope;
		return node;
	}
}

TEST_CASE("a unique producer is scheduled before a consumer placed above it", "[graph][schedule]") {
	RenderGraph graph;
	const ResourceId colour = Resource(graph, "colour", ResourceKind::Colour);

	Node consumer = NodeOf("grade", "grade");
	consumer.Reads = {colour};
	graph.AddNode(consumer);

	Node producer = NodeOf("draw", "opaque");
	producer.Writes = {colour};
	graph.AddNode(producer);

	ExecutionSchedule schedule;
	Name offender;
	REQUIRE(CompileSchedule(graph, schedule, offender) == ScheduleStatus::Ok);
	REQUIRE(schedule.Waves.size() == 2);
	CHECK(graph.Find(schedule.Waves[0].Nodes[0].Node)->Name == Name("draw"));
	CHECK(graph.Find(schedule.Waves[1].Nodes[0].Node)->Name == Name("grade"));
}

TEST_CASE("independent CPU culling and compute work share an async wave", "[graph][schedule]") {
	RenderGraph graph;
	const ResourceId entities = Resource(graph, "entities", ResourceKind::Entities);
	const ResourceId storage = Resource(graph, "hzb", ResourceKind::Storage);

	Node cull = NodeOf("cull", "frustum-cull");
	cull.Writes = {entities};
	graph.AddNode(cull);

	Node compute = NodeOf("depth-pyramid", "compute");
	compute.Writes = {storage};
	graph.AddNode(compute);

	ExecutionSchedule schedule;
	Name offender;
	REQUIRE(CompileSchedule(graph, schedule, offender) == ScheduleStatus::Ok);
	REQUIRE(schedule.Waves.size() == 1);
	REQUIRE(schedule.Waves[0].Nodes.size() == 2);
	CHECK(schedule.Waves[0].Nodes[0].Queue == ExecutionQueue::Cpu);
	CHECK(schedule.Waves[0].Nodes[1].Queue == ExecutionQueue::Compute);
	CHECK(schedule.Waves[0].Concurrent);
}

TEST_CASE("world view and frame scopes form execution boundaries", "[graph][schedule]") {
	RenderGraph graph;
	const ResourceId worldOutput = Resource(graph, "world-output", ResourceKind::Depth);
	const ResourceId viewOutput = Resource(graph, "view-output", ResourceKind::Colour);
	const ResourceId frameOutput = Resource(graph, "frame-output", ResourceKind::Colour, true);

	Node frame = NodeOf("interface", "interface", NodeScope::Frame);
	frame.Writes = {frameOutput};
	graph.AddNode(frame);

	Node view = NodeOf("opaque", "opaque", NodeScope::View);
	view.Writes = {viewOutput};
	graph.AddNode(view);

	Node world = NodeOf("shadow", "shadow", NodeScope::World);
	world.Writes = {worldOutput};
	graph.AddNode(world);

	ExecutionSchedule schedule;
	Name offender;
	REQUIRE(CompileSchedule(graph, schedule, offender) == ScheduleStatus::Ok);
	REQUIRE(schedule.Waves.size() == 3);
	REQUIRE(schedule.Waves[0].Nodes.size() == 1);
	REQUIRE(schedule.Waves[1].Nodes.size() == 1);
	REQUIRE(schedule.Waves[2].Nodes.size() == 1);
	CHECK(graph.Find(schedule.Waves[0].Nodes[0].Node)->Name == Name("shadow"));
	CHECK(graph.Find(schedule.Waves[1].Nodes[0].Node)->Name == Name("opaque"));
	CHECK(graph.Find(schedule.Waves[2].Nodes[0].Node)->Name == Name("interface"));
}

TEST_CASE("several writers of one resource remain explicitly ordered", "[graph][schedule]") {
	RenderGraph graph;
	const ResourceId colour = Resource(graph, "colour", ResourceKind::Colour, true);

	Node first = NodeOf("opaque", "opaque");
	first.Reads = {colour};
	first.Writes = {colour};
	graph.AddNode(first);

	Node second = NodeOf("transparent", "transparent");
	second.Reads = {colour};
	second.Writes = {colour};
	graph.AddNode(second);

	ExecutionSchedule schedule;
	Name offender;
	REQUIRE(CompileSchedule(graph, schedule, offender) == ScheduleStatus::Ok);
	REQUIRE(schedule.Waves.size() == 2);
	CHECK(graph.Find(schedule.Waves[0].Nodes[0].Node)->Name == Name("opaque"));
	CHECK(graph.Find(schedule.Waves[1].Nodes[0].Node)->Name == Name("transparent"));
}

TEST_CASE("a reader finishes before the next writer replaces its version", "[graph][schedule]") {
	RenderGraph graph;
	const ResourceId colour = Resource(graph, "colour", ResourceKind::Colour, true);
	const ResourceId sampled = Resource(graph, "sampled", ResourceKind::Colour);

	Node first = NodeOf("opaque", "opaque");
	first.Writes = {colour};
	graph.AddNode(first);

	Node reader = NodeOf("capture", "capture");
	reader.Reads = {colour};
	reader.Writes = {sampled};
	graph.AddNode(reader);

	Node replacement = NodeOf("transparent", "transparent");
	replacement.Reads = {colour};
	replacement.Writes = {colour};
	graph.AddNode(replacement);

	ExecutionSchedule schedule;
	Name offender;
	REQUIRE(CompileSchedule(graph, schedule, offender) == ScheduleStatus::Ok);
	REQUIRE(schedule.Waves.size() == 3);
	CHECK(graph.Find(schedule.Waves[0].Nodes[0].Node)->Name == Name("opaque"));
	CHECK(graph.Find(schedule.Waves[1].Nodes[0].Node)->Name == Name("capture"));
	CHECK(graph.Find(schedule.Waves[2].Nodes[0].Node)->Name == Name("transparent"));
}

TEST_CASE("queue async culling and compute dispatch hints reach the schedule", "[graph][schedule]") {
	RenderGraph graph;
	const ResourceId output = Resource(graph, "output", ResourceKind::Storage);

	Node compute = NodeOf("cluster", "compute");
	compute.Writes = {output};
	compute.Parameters = {
		{Name("queue"), "compute"},
		{Name("async"), "allow"},
		{Name("culling"), "occlusion"},
		{Name("dispatch.x"), "32"},
		{Name("dispatch.y"), "18"},
		{Name("dispatch.z"), "2"},
	};
	graph.AddNode(compute);

	ExecutionSchedule schedule;
	Name offender;
	REQUIRE(CompileSchedule(graph, schedule, offender) == ScheduleStatus::Ok);
	REQUIRE(schedule.Waves.size() == 1);
	REQUIRE(schedule.Waves[0].Nodes.size() == 1);
	const ScheduledNode &scheduled = schedule.Waves[0].Nodes[0];
	CHECK(scheduled.Queue == ExecutionQueue::Compute);
	CHECK(scheduled.Async == AsyncPolicy::Allow);
	CHECK(scheduled.Culling == CullingMode::Occlusion);
	CHECK(scheduled.GroupsX == 32);
	CHECK(scheduled.GroupsY == 18);
	CHECK(scheduled.GroupsZ == 2);
	CHECK(scheduled.AsyncEligible);
}

TEST_CASE("a serial hint prevents otherwise independent queue overlap", "[graph][schedule]") {
	RenderGraph graph;
	const ResourceId entities = Resource(graph, "entities", ResourceKind::Entities);
	const ResourceId storage = Resource(graph, "storage", ResourceKind::Storage);

	Node cpu = NodeOf("cull", "frustum-cull");
	cpu.Writes = {entities};
	graph.AddNode(cpu);

	Node compute = NodeOf("compute", "compute");
	compute.Writes = {storage};
	compute.Parameters = {{Name("async"), "serial"}};
	graph.AddNode(compute);

	ExecutionSchedule schedule;
	Name offender;
	REQUIRE(CompileSchedule(graph, schedule, offender) == ScheduleStatus::Ok);
	REQUIRE(schedule.Waves.size() == 1);
	CHECK_FALSE(schedule.Waves[0].Concurrent);
}

TEST_CASE("invalid scheduling hints are refused by node name", "[graph][schedule]") {
	RenderGraph graph;
	const ResourceId storage = Resource(graph, "storage", ResourceKind::Storage);
	Node compute = NodeOf("bad-dispatch", "compute");
	compute.Writes = {storage};
	compute.Parameters = {{Name("dispatch.x"), "zero"}};
	graph.AddNode(compute);

	ExecutionSchedule schedule;
	Name offender;
	CHECK(CompileSchedule(graph, schedule, offender) == ScheduleStatus::InvalidHint);
	CHECK(offender == Name("bad-dispatch"));
}

TEST_CASE("a world node cannot depend on a per-view result", "[graph][schedule]") {
	RenderGraph graph;
	const ResourceId depth = Resource(graph, "depth", ResourceKind::Depth);

	Node world = NodeOf("world-read", "shadow", NodeScope::World);
	world.Reads = {depth};
	graph.AddNode(world);

	Node view = NodeOf("view-write", "opaque", NodeScope::View);
	view.Writes = {depth};
	graph.AddNode(view);

	ExecutionSchedule schedule;
	Name offender;
	CHECK(CompileSchedule(graph, schedule, offender) == ScheduleStatus::ScopeDependency);
	CHECK(offender == Name("world-read"));
}

TEST_CASE("every schedule status and queue has a stable name", "[graph][schedule]") {
	for (const ScheduleStatus status :
		 {ScheduleStatus::Ok,
		  ScheduleStatus::InvalidGraph,
		  ScheduleStatus::MissingProducer,
		  ScheduleStatus::ScopeDependency,
		  ScheduleStatus::Cycle,
		  ScheduleStatus::InvalidHint}) {
		CHECK(std::string(Describe(status)) != "unknown");
	}
	for (const ExecutionQueue queue :
		 {ExecutionQueue::Cpu, ExecutionQueue::Graphics, ExecutionQueue::Compute, ExecutionQueue::Transfer}) {
		CHECK(std::string(Describe(queue)) != "unknown");
	}
	for (const AsyncPolicy policy : {AsyncPolicy::Automatic, AsyncPolicy::Allow, AsyncPolicy::Serial}) {
		CHECK(std::string(Describe(policy)) != "unknown");
	}
	for (const CullingMode mode :
		 {CullingMode::Inherit, CullingMode::None, CullingMode::Frustum, CullingMode::Occlusion}) {
		CHECK(std::string(Describe(mode)) != "unknown");
	}
}
