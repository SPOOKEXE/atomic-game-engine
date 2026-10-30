#include <engine/graph/Schedule.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <utility>
#include <vector>

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

	RenderGraph AcceptedGraph() {
		RenderGraph graph;
		const auto entities = Resource(graph, "accepted-entities", ResourceKind::Entities);
		const auto storage = Resource(graph, "accepted-storage", ResourceKind::Storage);
		const auto output = Resource(graph, "accepted-output", ResourceKind::Storage);
		auto cpu = NodeOf("accepted-cull", "frustum-cull");
		cpu.Writes = {entities};
		REQUIRE(graph.AddNode(std::move(cpu)).IsValid());
		auto compute = NodeOf("accepted-compute", "compute");
		compute.Writes = {storage};
		compute.Parameters = {
			{Name("queue"), "compute"},
			{Name("async"), "allow"},
			{Name("culling"), "occlusion"},
			{Name("dispatch.x"), "32"},
			{Name("dispatch.y"), "18"},
			{Name("dispatch.z"), "2"}
		};
		REQUIRE(graph.AddNode(std::move(compute)).IsValid());
		auto consumer = NodeOf("accepted-consumer", "compute");
		consumer.Reads = {storage};
		consumer.Writes = {output};
		REQUIRE(graph.AddNode(std::move(consumer)).IsValid());
		return graph;
	}

	void CheckRetained(const ExecutionSchedule &actual, const ExecutionSchedule &accepted) {
		REQUIRE(actual.Waves.size() == accepted.Waves.size());
		for (size_t wave = 0; wave < accepted.Waves.size(); wave++) {
			CHECK(actual.Waves[wave].Concurrent == accepted.Waves[wave].Concurrent);
			REQUIRE(actual.Waves[wave].Nodes.size() == accepted.Waves[wave].Nodes.size());
			for (size_t index = 0; index < accepted.Waves[wave].Nodes.size(); index++) {
				const auto &held = actual.Waves[wave].Nodes[index];
				const auto &expected = accepted.Waves[wave].Nodes[index];
				CHECK(held.Node == expected.Node);
				CHECK(held.Queue == expected.Queue);
				CHECK(held.Async == expected.Async);
				CHECK(held.Culling == expected.Culling);
				CHECK(held.GroupsX == expected.GroupsX);
				CHECK(held.GroupsY == expected.GroupsY);
				CHECK(held.GroupsZ == expected.GroupsZ);
				CHECK(held.AsyncEligible == expected.AsyncEligible);
			}
		}
	}

}

TEST_CASE("the last authored writer supplies a later reader", "[graph][schedule]") {
	RenderGraph graph;
	const ResourceId colour = Resource(graph, "colour", ResourceKind::Colour);
	for (const char *name : {"first", "second"}) {
		Node writer = NodeOf(name, "opaque");
		writer.Writes = {colour};
		graph.AddNode(writer);
	}
	Node reader = NodeOf("reader", "grade");
	reader.Reads = {colour};
	graph.AddNode(reader);

	ExecutionSchedule schedule;
	Name offender;
	REQUIRE(CompileSchedule(graph, schedule, offender) == ScheduleStatus::Ok);
	REQUIRE(schedule.Waves.size() == 3);
	CHECK(graph.Find(schedule.Waves[0].Nodes[0].Node)->Name == Name("first"));
	CHECK(graph.Find(schedule.Waves[1].Nodes[0].Node)->Name == Name("second"));
	CHECK(graph.Find(schedule.Waves[2].Nodes[0].Node)->Name == Name("reader"));

	// Execute the scheduled writes as a tiny value model. The reader must see
	// the second version even when the first writer is also a valid producer.
	std::string colourValue;
	std::string observed;
	for (const ExecutionWave &wave : schedule.Waves)
		for (const ScheduledNode &scheduled : wave.Nodes) {
			const Name name = graph.Find(scheduled.Node)->Name;
			if (name == Name("reader"))
				observed = colourValue;
			else
				colourValue = std::string(name.Text());
		}
	CHECK(observed == "second");
}

TEST_CASE("disabled writers do not replace the version a reader sees", "[graph][schedule]") {
	RenderGraph graph;
	const ResourceId colour = Resource(graph, "colour", ResourceKind::Colour);
	Node first = NodeOf("first", "opaque");
	first.Writes = {colour};
	graph.AddNode(first);
	Node disabled = NodeOf("disabled", "opaque");
	disabled.Writes = {colour};
	disabled.Enabled = false;
	graph.AddNode(disabled);
	Node reader = NodeOf("reader", "grade");
	reader.Reads = {colour};
	graph.AddNode(reader);

	ExecutionSchedule schedule;
	Name offender;
	REQUIRE(CompileSchedule(graph, schedule, offender) == ScheduleStatus::Ok);
	REQUIRE(schedule.Waves.size() == 2);
	CHECK(graph.Find(schedule.Waves[0].Nodes[0].Node)->Name == Name("first"));
	CHECK(graph.Find(schedule.Waves[1].Nodes[0].Node)->Name == Name("reader"));
}

TEST_CASE("history read before two writers uses the previous generation", "[graph][schedule]") {
	RenderGraph graph;
	const ResourceId history = graph.AddResource({
		.Name = Name("history"),
		.Kind = ResourceKind::Colour,
		.External = true,
		.Lifetime = ResourceLifetime::History,
	});
	Node reader = NodeOf("previous-frame", "grade");
	reader.Reads = {history};
	graph.AddNode(reader);
	for (const char *name : {"first-write", "second-write"}) {
		Node writer = NodeOf(name, "opaque");
		writer.Writes = {history};
		graph.AddNode(writer);
	}

	ExecutionSchedule schedule;
	Name offender;
	REQUIRE(CompileSchedule(graph, schedule, offender) == ScheduleStatus::Ok);
	REQUIRE(schedule.Waves.size() == 2);
	REQUIRE(schedule.Waves[0].Nodes.size() == 2);
	CHECK(graph.Find(schedule.Waves[0].Nodes[0].Node)->Name == Name("previous-frame"));
	CHECK(graph.Find(schedule.Waves[0].Nodes[1].Node)->Name == Name("first-write"));
	CHECK(graph.Find(schedule.Waves[1].Nodes[0].Node)->Name == Name("second-write"));
	std::string previousGeneration = "prior";
	std::string nextGeneration;
	std::string observed;
	for (const ExecutionWave &wave : schedule.Waves)
		for (const ScheduledNode &scheduled : wave.Nodes) {
			const Name name = graph.Find(scheduled.Node)->Name;
			if (name == Name("previous-frame"))
				observed = previousGeneration;
			else
				nextGeneration = std::string(name.Text());
		}
	CHECK(observed == "prior");
	CHECK(nextGeneration == "second-write");
}

TEST_CASE("a world version reaches views before a frame writer replaces it", "[graph][schedule]") {
	RenderGraph graph;
	const ResourceId colour = Resource(graph, "colour", ResourceKind::Colour, true);
	Node world = NodeOf("world-write", "shadow", NodeScope::World);
	world.Writes = {colour};
	graph.AddNode(world);
	Node view = NodeOf("view-read", "grade", NodeScope::View);
	view.Reads = {colour};
	graph.AddNode(view);
	const ResourceId overlay = Resource(graph, "overlay", ResourceKind::Colour);
	Node frame = NodeOf("frame-write", "overlay", NodeScope::Frame);
	frame.Writes = {overlay};
	graph.AddNode(frame);

	ExecutionSchedule schedule;
	Name offender;
	REQUIRE(CompileSchedule(graph, schedule, offender) == ScheduleStatus::Ok);
	REQUIRE(schedule.Waves.size() == 3);
	CHECK(graph.Find(schedule.Waves[0].Nodes[0].Node)->Name == Name("world-write"));
	CHECK(graph.Find(schedule.Waves[1].Nodes[0].Node)->Name == Name("view-read"));
	CHECK(graph.Find(schedule.Waves[2].Nodes[0].Node)->Name == Name("frame-write"));
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

	Node view = NodeOf("opaque", "opaque", NodeScope::View);
	view.Writes = {viewOutput};
	graph.AddNode(view);

	Node world = NodeOf("shadow", "shadow", NodeScope::World);
	world.Writes = {worldOutput};
	graph.AddNode(world);
	graph.AddNode(frame);

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

TEST_CASE("writer lists are independent across resources", "[graph][schedule]") {
	bool external = false;
	SECTION("external input stays independent") {
		external = true;
	}
	SECTION("missing input is refused") {}
	RenderGraph graph;
	const ResourceId written = Resource(graph, "written", ResourceKind::Colour);
	const ResourceId input = Resource(graph, "independent-input", ResourceKind::Colour, external);
	for (const char *name : {"first-writer", "second-writer"}) {
		Node writer = NodeOf(name, "opaque");
		writer.Writes = {written};
		REQUIRE(graph.AddNode(std::move(writer)).IsValid());
	}
	Node reader = NodeOf("independent-reader", "capture");
	reader.Reads = {input};
	REQUIRE(graph.AddNode(std::move(reader)).IsValid());
	ExecutionSchedule schedule;
	Name offender;
	const auto status = CompileSchedule(graph, schedule, offender);
	if (!external) {
		CHECK(status == ScheduleStatus::MissingProducer);
		CHECK(offender == Name("independent-input"));
		return;
	}
	REQUIRE(status == ScheduleStatus::Ok);
	REQUIRE(schedule.Waves.size() == 2);
	REQUIRE(schedule.Waves[0].Nodes.size() == 2);
	REQUIRE(schedule.Waves[1].Nodes.size() == 1);
	CHECK(graph.Find(schedule.Waves[0].Nodes[0].Node)->Name == Name("first-writer"));
	CHECK(graph.Find(schedule.Waves[0].Nodes[1].Node)->Name == Name("independent-reader"));
	CHECK(graph.Find(schedule.Waves[1].Nodes[0].Node)->Name == Name("second-writer"));
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
		  ScheduleStatus::InvalidHint,
		  ScheduleStatus::LimitExceeded,
		  ScheduleStatus::AllocationFailed}) {
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

TEST_CASE(
	"reachable invalid hints precede a disconnected cycle while blocked hints do not", "[graph][schedule]"
) {
	RenderGraph graph;
	const auto first = Resource(graph, "cycle-first", ResourceKind::Storage);
	const auto second = Resource(graph, "cycle-second", ResourceKind::Storage);
	const auto spare = Resource(graph, "spare", ResourceKind::Storage);
	const auto output = Resource(graph, "blocked-output", ResourceKind::Storage);
	Node blocked = NodeOf("blocked-low", "compute");
	blocked.Reads = {first};
	blocked.Writes = {output};
	Node left = NodeOf("cycle-left", "compute");
	left.Reads = {second};
	left.Writes = {first};
	Node right = NodeOf("cycle-right", "compute");
	right.Reads = {first};
	right.Writes = {second};
	Node independent = NodeOf("independent", "compute");
	independent.Writes = {spare};
	ScheduleStatus expected = ScheduleStatus::Cycle;
	Name expectedOffender("blocked-low");
	SECTION("invalid hint on an emitted node precedes the cycle") {
		independent.Parameters = {{Name("dispatch.x"), "zero"}};
		expected = ScheduleStatus::InvalidHint;
		expectedOffender = independent.Name;
	}
	SECTION("invalid hint on a blocked node is never evaluated") {
		blocked.Parameters = {{Name("dispatch.x"), "zero"}};
	}
	SECTION("cycle without invalid hints reports the first blocked descendant") {}
	graph.AddNode(blocked);
	graph.AddNode(left);
	graph.AddNode(right);
	graph.AddNode(independent);
	ExecutionSchedule schedule;
	Name offender;
	REQUIRE(CompileSchedule(AcceptedGraph(), schedule, offender) == ScheduleStatus::Ok);
	const auto accepted = schedule;
	CHECK(CompileSchedule(graph, schedule, offender) == expected);
	CHECK(offender == expectedOffender);
	CheckRetained(schedule, accepted);
}

TEST_CASE(
	"hint refusal follows complete declaration-ordered waves before a later cycle", "[graph][schedule]"
) {
	RenderGraph graph;
	const auto prefix = Resource(graph, "prefix", ResourceKind::Storage);
	const auto first = Resource(graph, "first", ResourceKind::Storage);
	const auto second = Resource(graph, "second", ResourceKind::Storage);
	const auto a = Resource(graph, "cycle-a", ResourceKind::Storage);
	const auto b = Resource(graph, "cycle-b", ResourceKind::Storage);
	Node later = NodeOf("later-wave-low-index", "compute");
	later.Reads = {prefix};
	later.Writes = {first};
	later.Parameters = {{Name("dispatch.x"), "zero"}};
	graph.AddNode(later);
	Node earlier = NodeOf("first-wave-higher-index", "compute");
	earlier.Writes = {second};
	earlier.Parameters = {{Name("dispatch.x"), "zero"}};
	graph.AddNode(earlier);
	Node source = NodeOf("source", "compute");
	source.Writes = {prefix};
	graph.AddNode(source);
	Node left = NodeOf("left", "compute");
	left.Reads = {b};
	left.Writes = {a};
	graph.AddNode(left);
	Node right = NodeOf("right", "compute");
	right.Reads = {a};
	right.Writes = {b};
	graph.AddNode(right);
	ExecutionSchedule schedule;
	Name offender;
	CHECK(CompileSchedule(graph, schedule, offender) == ScheduleStatus::InvalidHint);
	CHECK(offender == earlier.Name);
	CHECK(schedule.Waves.empty());
}

TEST_CASE("unregistered render kinds retain resource-based scheduling fallbacks", "[graph][schedule]") {
	RenderGraph graph;
	const auto entities = Resource(graph, "entities", ResourceKind::Entities);
	const auto storage = Resource(graph, "storage", ResourceKind::Storage);
	Node cpu = NodeOf("unregistered-cpu", "schedule-custom-cpu");
	cpu.Writes = {entities};
	graph.AddNode(cpu);
	Node compute = NodeOf("unregistered-compute", "schedule-custom-compute");
	compute.Writes = {storage};
	graph.AddNode(compute);
	ExecutionSchedule schedule;
	Name offender;
	REQUIRE(CompileSchedule(graph, schedule, offender) == ScheduleStatus::Ok);
	REQUIRE(schedule.Waves.size() == 1);
	REQUIRE(schedule.Waves[0].Nodes.size() == 2);
	CHECK(schedule.Waves[0].Nodes[0].Node == NodeId{1});
	CHECK(schedule.Waves[0].Nodes[0].Queue == ExecutionQueue::Cpu);
	CHECK(schedule.Waves[0].Nodes[1].Node == NodeId{2});
	CHECK(schedule.Waves[0].Nodes[1].Queue == ExecutionQueue::Compute);
	CHECK(schedule.Waves[0].Concurrent);
	CHECK_FALSE(offender.IsValid());
}

TEST_CASE("render schedule refusals preserve accepted waves and execution metadata", "[graph][schedule]") {
	const auto acceptedGraph = AcceptedGraph();
	ExecutionSchedule schedule;
	Name offender;
	REQUIRE(CompileSchedule(acceptedGraph, schedule, offender) == ScheduleStatus::Ok);
	REQUIRE(schedule.Waves.size() == 2);
	REQUIRE(schedule.Waves[0].Concurrent);
	const auto accepted = schedule;
	RenderGraph refused;
	ScheduleStatus expected = ScheduleStatus::MissingProducer;
	Name expectedOffender("missing");
	SECTION("missing producer") {
		const auto missing = Resource(refused, "missing", ResourceKind::Storage);
		auto reader = NodeOf("missing-reader", "compute");
		reader.Reads = {missing};
		refused.AddNode(std::move(reader));
	}
	SECTION("invalid graph") {
		auto node = NodeOf("invalid-resource", "compute");
		node.Reads = {ResourceId{999}};
		refused.AddNode(std::move(node));
		expected = ScheduleStatus::InvalidGraph;
		expectedOffender = Name("invalid-resource");
	}
	SECTION("scope regression") {
		const auto depth = Resource(refused, "late", ResourceKind::Depth);
		auto reader = NodeOf("early-world", "shadow", NodeScope::World);
		reader.Reads = {depth};
		refused.AddNode(std::move(reader));
		auto writer = NodeOf("late-view", "opaque");
		writer.Writes = {depth};
		refused.AddNode(std::move(writer));
		expected = ScheduleStatus::ScopeDependency;
		expectedOffender = Name("early-world");
	}
	SECTION("late invalid hint after an accepted prefix") {
		const auto storage = Resource(refused, "prefix", ResourceKind::Storage);
		const auto output = Resource(refused, "bad-output", ResourceKind::Storage);
		auto prefix = NodeOf("valid-prefix", "compute");
		prefix.Writes = {storage};
		refused.AddNode(std::move(prefix));
		auto invalid = NodeOf("bad-dispatch", "compute");
		invalid.Reads = {storage};
		invalid.Writes = {output};
		invalid.Parameters = {{Name("dispatch.x"), "zero"}};
		refused.AddNode(std::move(invalid));
		expected = ScheduleStatus::InvalidHint;
		expectedOffender = Name("bad-dispatch");
	}
	SECTION("cycle") {
		const auto left = Resource(refused, "left", ResourceKind::Storage);
		const auto right = Resource(refused, "right", ResourceKind::Storage);
		auto first = NodeOf("first-cycle", "compute");
		first.Reads = {right};
		first.Writes = {left};
		refused.AddNode(std::move(first));
		auto second = NodeOf("second-cycle", "compute");
		second.Reads = {left};
		second.Writes = {right};
		refused.AddNode(std::move(second));
		expected = ScheduleStatus::Cycle;
		expectedOffender = Name("first-cycle");
	}
	CHECK(CompileSchedule(refused, schedule, offender) == expected);
	CHECK(offender == expectedOffender);
	CheckRetained(schedule, accepted);
	std::vector<Name> ran;
	for (const auto &wave : schedule.Waves)
		for (const auto &node : wave.Nodes) {
			REQUIRE(acceptedGraph.Find(node.Node) != nullptr);
			ran.push_back(acceptedGraph.Find(node.Node)->Name);
		}
	CHECK(
		ran == std::vector<Name>{Name("accepted-cull"), Name("accepted-compute"), Name("accepted-consumer")}
	);
}

TEST_CASE("scope and duplicate resource dependencies preserve boundary-node waves", "[graph][schedule]") {
	RenderGraph graph;
	const auto external = Resource(graph, "external-input", ResourceKind::Storage, true);
	std::array<Node, 256> nodes;
	for (size_t index = 0; index < nodes.size(); ++index) {
		const std::string name = "boundary-node-" + std::to_string(index);
		const auto scope = index < 64 ? NodeScope::World : index < 128 ? NodeScope::View : NodeScope::Frame;
		nodes[index] = NodeOf(name.c_str(), "compute", scope);
		nodes[index].Reads = {external};
	}
	// Duplicate resource prerequisites also coincide with the world/view/frame boundaries.
	// The late frame producer must run before its earlier-authored frame consumer.
	const std::array<std::pair<size_t, size_t>, 6> links{
		{{0, 63}, {63, 64}, {64, 127}, {127, 128}, {255, 128}, {128, 129}}
	};
	for (size_t link = 0; link < links.size(); ++link)
		for (size_t duplicate = 0; duplicate < 2; ++duplicate) {
			const std::string name =
				"boundary-link-" + std::to_string(link) + "-" + std::to_string(duplicate);
			const auto resource = Resource(graph, name.c_str(), ResourceKind::Storage);
			REQUIRE(resource.IsValid());
			nodes[links[link].first].Writes.push_back(resource);
			nodes[links[link].second].Reads.push_back(resource);
		}
	std::array<NodeId, 256> ids;
	for (size_t index = 0; index < nodes.size(); ++index) {
		ids[index] = graph.AddNode(std::move(nodes[index]));
		REQUIRE(ids[index].IsValid());
	}
	ExecutionSchedule schedule;
	Name offender;
	REQUIRE(CompileSchedule(graph, schedule, offender) == ScheduleStatus::Ok);
	CHECK_FALSE(offender.IsValid());
	// Independent work stays in authored order within each scope. Only the six
	// declared prerequisites split these otherwise concurrent-ready scope groups.
	std::array<std::vector<NodeId>, 7> expected;
	for (size_t index = 0; index < 63; ++index)
		expected[0].push_back(ids[index]);
	expected[1] = {ids[63]};
	for (size_t index = 64; index < 127; ++index)
		expected[2].push_back(ids[index]);
	expected[3] = {ids[127]};
	for (size_t index = 130; index < 256; ++index)
		expected[4].push_back(ids[index]);
	expected[5] = {ids[128]};
	expected[6] = {ids[129]};
	REQUIRE(schedule.Waves.size() == expected.size());
	for (size_t wave = 0; wave < expected.size(); ++wave) {
		CAPTURE(wave);
		CHECK_FALSE(schedule.Waves[wave].Concurrent);
		REQUIRE(schedule.Waves[wave].Nodes.size() == expected[wave].size());
		for (size_t index = 0; index < expected[wave].size(); ++index) {
			CAPTURE(index);
			CHECK(schedule.Waves[wave].Nodes[index].Node == expected[wave][index]);
			CHECK(schedule.Waves[wave].Nodes[index].Queue == ExecutionQueue::Compute);
		}
	}
}
