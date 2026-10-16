#include "Schema.hpp"

#include <engine/audio/Graph.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/graphcore/Schema.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.audio.graph")
TEST_DEPENDS("engine.audio.sample")
TEST_DEPENDS("engine.graphcore.compile")
TEST_DEPENDS("engine.graphcore.schema")

using engine::audio::AudioGraph;
using engine::audio::Describe;
using engine::audio::Node;
using engine::audio::NodeId;
using engine::audio::NodeKind;

namespace {
	// Where `id` sits in the run order.
	size_t PositionOf(const AudioGraph &graph, NodeId id) {
		const std::span<const NodeId> order = graph.Order();
		for (size_t index = 0; index < order.size(); ++index) {
			if (order[index] == id) {
				return index;
			}
		}
		return order.size();
	}

	bool RunsBefore(const AudioGraph &graph, NodeId first, NodeId second) {
		return PositionOf(graph, first) < PositionOf(graph, second);
	}
}

TEST_CASE("a fresh graph has exactly its output", "[audio][graph]") {
	// There is never a graph in which "where does the device read from" has no
	// answer.
	const AudioGraph graph;
	CHECK(graph.Count() == 1);
	CHECK(graph.Output().IsValid());
	REQUIRE(graph.Find(graph.Output()) != nullptr);
	CHECK(graph.Find(graph.Output())->Kind == NodeKind::Output);
}

TEST_CASE("a second output is refused", "[audio][graph]") {
	// Two outputs is a graph with no answer to what the device plays.
	AudioGraph graph;
	CHECK_FALSE(graph.Add(NodeKind::Output).IsValid());
	CHECK(graph.Count() == 1);
}

TEST_CASE("the output cannot be removed", "[audio][graph]") {
	AudioGraph graph;
	CHECK_FALSE(graph.Remove(graph.Output()));
	CHECK(graph.Count() == 1);
}

TEST_CASE("nodes are added and found", "[audio][graph]") {
	AudioGraph graph;
	const NodeId player = graph.Add(NodeKind::Player);
	const NodeId fader = graph.Add(NodeKind::Fader);

	REQUIRE(player.IsValid());
	REQUIRE(fader.IsValid());
	CHECK(player != fader);
	CHECK(graph.Count() == 3);

	REQUIRE(graph.Find(player) != nullptr);
	CHECK(graph.Find(player)->Kind == NodeKind::Player);
	CHECK(graph.Find(fader)->Kind == NodeKind::Fader);
	CHECK(graph.Find(NodeId{.Value = 9999}) == nullptr);
	CHECK(graph.Find(NodeId{}) == nullptr);
}

TEST_CASE("a wire connects a source to a target", "[audio][graph]") {
	AudioGraph graph;
	const NodeId player = graph.Add(NodeKind::Player);

	CHECK(graph.Connect(player, graph.Output()));
	CHECK(graph.Connected(player, graph.Output()));
	// One way. A wire is directed, and reading the reverse as connected would
	// make the topological order meaningless.
	CHECK_FALSE(graph.Connected(graph.Output(), player));

	const std::span<const NodeId> inputs = graph.InputsOf(graph.Output());
	REQUIRE(inputs.size() == 1);
	CHECK(inputs[0] == player);
}

TEST_CASE("connecting twice is a no-op that succeeds", "[audio][graph]") {
	// A caller rebuilding a routing should not have to diff it first.
	AudioGraph graph;
	const NodeId player = graph.Add(NodeKind::Player);

	CHECK(graph.Connect(player, graph.Output()));
	CHECK(graph.Connect(player, graph.Output()));
	CHECK(graph.InputsOf(graph.Output()).size() == 1);
}

TEST_CASE("a wire to or from a node that does not exist is refused", "[audio][graph]") {
	AudioGraph graph;
	const NodeId player = graph.Add(NodeKind::Player);
	const NodeId ghost{.Value = 4242};

	CHECK_FALSE(graph.Connect(player, ghost));
	CHECK_FALSE(graph.Connect(ghost, player));
	CHECK_FALSE(graph.Connect(ghost, ghost));
}

TEST_CASE("nothing is downstream of the output", "[audio][graph]") {
	AudioGraph graph;
	const NodeId bus = graph.Add(NodeKind::Bus);
	CHECK_FALSE(graph.Connect(graph.Output(), bus));
}

TEST_CASE("a node cannot wire to itself", "[audio][graph]") {
	AudioGraph graph;
	const NodeId bus = graph.Add(NodeKind::Bus);
	CHECK_FALSE(graph.Connect(bus, bus));
}

// --- cycles, which are the case that matters ------------------------------

TEST_CASE("a two-node loop is refused", "[audio][graph]") {
	// A feedback loop is either infinite recursion inside a callback with a
	// hard deadline, or unbounded gain. Refused at the wire, where it costs a
	// walk.
	AudioGraph graph;
	const NodeId first = graph.Add(NodeKind::Bus);
	const NodeId second = graph.Add(NodeKind::Bus);

	REQUIRE(graph.Connect(first, second));
	CHECK_FALSE(graph.Connect(second, first));
	CHECK_FALSE(graph.Connected(second, first));
}

TEST_CASE("a long loop is refused", "[audio][graph]") {
	// The cheap check catches the two-node case. A chain of ten is what a
	// routing somebody actually built looks like.
	AudioGraph graph;
	std::vector<NodeId> chain;
	for (int index = 0; index < 10; ++index) {
		chain.push_back(graph.Add(NodeKind::Bus));
	}
	for (size_t index = 0; index + 1 < chain.size(); ++index) {
		REQUIRE(graph.Connect(chain[index], chain[index + 1]));
	}

	// Closing the ring at any point is refused.
	CHECK_FALSE(graph.Connect(chain.back(), chain.front()));
	CHECK_FALSE(graph.Connect(chain.back(), chain[4]));
	// And a wire that does not close one is still fine.
	CHECK(graph.Connect(chain.front(), chain.back()));
}

TEST_CASE("a diamond is not a cycle", "[audio][graph]") {
	// One source into two buses into one output. A cycle check that walked
	// without marking what it had seen would either loop or refuse this.
	AudioGraph graph;
	const NodeId player = graph.Add(NodeKind::Player);
	const NodeId left = graph.Add(NodeKind::Bus);
	const NodeId right = graph.Add(NodeKind::Bus);

	REQUIRE(graph.Connect(player, left));
	REQUIRE(graph.Connect(player, right));
	REQUIRE(graph.Connect(left, graph.Output()));
	REQUIRE(graph.Connect(right, graph.Output()));

	CHECK(graph.InputsOf(graph.Output()).size() == 2);
	CHECK(RunsBefore(graph, player, left));
	CHECK(RunsBefore(graph, left, graph.Output()));
	CHECK(RunsBefore(graph, right, graph.Output()));
}

// --- ordering -------------------------------------------------------------

TEST_CASE("a source always runs before its target", "[audio][graph]") {
	AudioGraph graph;
	const NodeId player = graph.Add(NodeKind::Player);
	const NodeId emitter = graph.Add(NodeKind::Emitter);
	const NodeId fader = graph.Add(NodeKind::Fader);
	const NodeId bus = graph.Add(NodeKind::Bus);

	REQUIRE(graph.Connect(player, emitter));
	REQUIRE(graph.Connect(emitter, fader));
	REQUIRE(graph.Connect(fader, bus));
	REQUIRE(graph.Connect(bus, graph.Output()));

	CHECK(RunsBefore(graph, player, emitter));
	CHECK(RunsBefore(graph, emitter, fader));
	CHECK(RunsBefore(graph, fader, bus));
	CHECK(RunsBefore(graph, bus, graph.Output()));
}

TEST_CASE("every node is in the order, including a detached one", "[audio][graph]") {
	// A detached subgraph costs its own mixing and nothing else. Dropping it
	// would make a half-built routing silently stop advancing its players,
	// which reads as a sound that will not start.
	AudioGraph graph;
	const NodeId wired = graph.Add(NodeKind::Player);
	const NodeId loose = graph.Add(NodeKind::Player);
	REQUIRE(graph.Connect(wired, graph.Output()));

	CHECK(graph.Order().size() == 3);
	CHECK(PositionOf(graph, loose) < graph.Order().size());
}

TEST_CASE("the order is deterministic", "[audio][graph]") {
	// Two runs of one graph produce one order, which is what makes a mix
	// reproducible. A sort that popped from a stack would not give this.
	AudioGraph graph;
	std::vector<NodeId> players;
	for (int index = 0; index < 8; ++index) {
		players.push_back(graph.Add(NodeKind::Player));
	}
	const NodeId bus = graph.Add(NodeKind::Bus);
	for (const NodeId player : players) {
		REQUIRE(graph.Connect(player, bus));
	}
	REQUIRE(graph.Connect(bus, graph.Output()));

	const std::vector<NodeId> first(graph.Order().begin(), graph.Order().end());
	const std::vector<NodeId> second(graph.Order().begin(), graph.Order().end());
	REQUIRE(first.size() == second.size());
	for (size_t index = 0; index < first.size(); ++index) {
		CHECK(first[index] == second[index]);
	}
}

TEST_CASE("changing a parameter does not disturb the order", "[audio][graph]") {
	// The common case by a wide margin, and the reason the order is cached
	// rather than recomputed per block.
	AudioGraph graph;
	const NodeId player = graph.Add(NodeKind::Player);
	REQUIRE(graph.Connect(player, graph.Output()));

	const std::vector<NodeId> before(graph.Order().begin(), graph.Order().end());
	graph.Find(player)->Gain = 0.25f;
	graph.Find(player)->Cursor = 128.0;
	const std::vector<NodeId> after(graph.Order().begin(), graph.Order().end());

	REQUIRE(before.size() == after.size());
	for (size_t index = 0; index < before.size(); ++index) {
		CHECK(before[index] == after[index]);
	}
}

TEST_CASE("the order follows a rewire", "[audio][graph]") {
	AudioGraph graph;
	const NodeId first = graph.Add(NodeKind::Bus);
	const NodeId second = graph.Add(NodeKind::Bus);

	REQUIRE(graph.Connect(first, second));
	REQUIRE(graph.Connect(second, graph.Output()));
	CHECK(RunsBefore(graph, first, second));

	// Reverse it, which is only legal once the original wire is gone.
	REQUIRE(graph.Disconnect(first, second));
	REQUIRE(graph.Connect(second, first));
	CHECK(RunsBefore(graph, second, first));
}

// --- removal ---------------------------------------------------------------

TEST_CASE("removing a node takes its wires with it", "[audio][graph]") {
	// A wire to a node that is gone is a lookup that fails once per block, for
	// ever.
	AudioGraph graph;
	const NodeId player = graph.Add(NodeKind::Player);
	const NodeId bus = graph.Add(NodeKind::Bus);

	REQUIRE(graph.Connect(player, bus));
	REQUIRE(graph.Connect(bus, graph.Output()));
	REQUIRE(graph.InputsOf(graph.Output()).size() == 1);

	REQUIRE(graph.Remove(bus));
	CHECK(graph.Find(bus) == nullptr);
	CHECK(graph.InputsOf(graph.Output()).empty());
	CHECK_FALSE(graph.Connected(player, bus));
	// The player survives, detached.
	CHECK(graph.Find(player) != nullptr);
}

TEST_CASE("removing a node that is not there is refused", "[audio][graph]") {
	AudioGraph graph;
	CHECK_FALSE(graph.Remove(NodeId{.Value = 77}));
	CHECK_FALSE(graph.Remove(NodeId{}));
}

TEST_CASE("a removed node's id is never reissued", "[audio][graph]") {
	// A recycled id makes a stale handle name somebody else's node, which is
	// the failure `NodeId` being a counter rather than an index exists to
	// avoid.
	AudioGraph graph;
	const NodeId first = graph.Add(NodeKind::Player);
	REQUIRE(graph.Remove(first));
	const NodeId second = graph.Add(NodeKind::Player);

	CHECK(second != first);
	CHECK(graph.Find(first) == nullptr);
}

TEST_CASE("inputs survive a rebuild of the order", "[audio][graph]") {
	// The adjacency list and the order are derived from the same wires and go
	// stale together. A shared scratch buffer here would be invalidated by the
	// second call, which is the bug this arrangement avoids.
	AudioGraph graph;
	const NodeId left = graph.Add(NodeKind::Player);
	const NodeId right = graph.Add(NodeKind::Player);
	const NodeId bus = graph.Add(NodeKind::Bus);

	REQUIRE(graph.Connect(left, bus));
	REQUIRE(graph.Connect(right, bus));
	REQUIRE(graph.Connect(bus, graph.Output()));

	// Hold one node's inputs while asking about another.
	const std::span<const NodeId> busInputs = graph.InputsOf(bus);
	const std::span<const NodeId> outputInputs = graph.InputsOf(graph.Output());

	REQUIRE(busInputs.size() == 2);
	REQUIRE(outputInputs.size() == 1);
	CHECK(busInputs[0] == left);
	CHECK(busInputs[1] == right);
	CHECK(outputInputs[0] == bus);
}

TEST_CASE("the graph is bounded", "[audio][graph]") {
	// The topological walk is per block on a thread with a hard deadline, so
	// the amount of work has a ceiling.
	AudioGraph graph;
	while (graph.Count() < AudioGraph::MAXIMUM_NODES) {
		REQUIRE(graph.Add(NodeKind::Player).IsValid());
	}
	CHECK_FALSE(graph.Add(NodeKind::Player).IsValid());
	CHECK(graph.Count() == AudioGraph::MAXIMUM_NODES);
}

TEST_CASE("every node kind has a name", "[audio][graph]") {
	CHECK(std::string(Describe(NodeKind::Player)) == "player");
	CHECK(std::string(Describe(NodeKind::Fader)) == "fader");
	CHECK(std::string(Describe(NodeKind::Emitter)) == "emitter");
	CHECK(std::string(Describe(NodeKind::Bus)) == "bus");
	CHECK(std::string(Describe(NodeKind::Output)) == "output");
}

TEST_CASE("audio node declarations define the routing vocabulary", "[audio][graph]") {
	const auto schemas = engine::audio::detail::NodeSchemas();
	REQUIRE(schemas.size() == 5);
	const std::array kinds{
		NodeKind::Player, NodeKind::Fader, NodeKind::Emitter, NodeKind::Bus, NodeKind::Output
	};
	for (size_t index = 0; index < kinds.size(); ++index)
		CHECK(engine::audio::detail::NodeSchemaFor(kinds[index]) == &schemas[index]);
	const auto playerInput = engine::audio::detail::RouteFor(NodeKind::Bus, NodeKind::Player).Input;
	REQUIRE(playerInput != nullptr);
	CHECK(playerInput->Id == "input");
	CHECK(playerInput->ValueType == "audio.block");
	CHECK(playerInput->MaximumConnections == 0);
	CHECK(playerInput->Optional);
	CHECK(engine::audio::detail::RouteFor(NodeKind::Output, NodeKind::Bus).Input == nullptr);
	const engine::graphcore::Limits limits{
		.MaximumSchemas = 5, .MaximumPorts = 9, .MaximumIdentityBytes = 256
	};
	engine::graphcore::Diagnostic diagnostic;
	CHECK(
		engine::graphcore::ValidateSchemas(schemas, limits, diagnostic) ==
		engine::graphcore::CompileStatus::Ok
	);
}

TEST_CASE("invalid node kinds cannot enter or route through an audio graph", "[audio][graph]") {
	const auto invalid = static_cast<NodeKind>(255);
	AudioGraph graph;
	CHECK(std::string(Describe(invalid)) == "unknown");
	CHECK(engine::audio::detail::NodeSchemaFor(invalid) == nullptr);
	CHECK_FALSE(graph.Add(invalid).IsValid());
	CHECK_FALSE(graph.Adopt(NodeId{.Value = 400}, invalid));
	const NodeId player = graph.Add(NodeKind::Player);
	REQUIRE(player.IsValid());
	REQUIRE(graph.Connect(player, graph.Output()));
	graph.Find(player)->Kind = invalid;
	CHECK(graph.Order().empty());
	CHECK_FALSE(graph.Connect(player, graph.Output()));
	CHECK(graph.Connected(player, graph.Output()));
}

TEST_CASE("players retain optional incoming wires in authored order", "[audio][graph]") {
	AudioGraph graph;
	const NodeId target = graph.Add(NodeKind::Player);
	const NodeId first = graph.Add(NodeKind::Bus);
	const NodeId second = graph.Add(NodeKind::Bus);
	REQUIRE(graph.Connect(second, target));
	REQUIRE(graph.Connect(first, target));
	REQUIRE(graph.Connect(target, graph.Output()));
	const auto inputs = graph.InputsOf(target);
	REQUIRE(inputs.size() == 2);
	CHECK(inputs[0] == second);
	CHECK(inputs[1] == first);
	CHECK(RunsBefore(graph, second, target));
	CHECK(RunsBefore(graph, first, target));
}

TEST_CASE(
	"routing keeps authored sweeps and input summation sequence after shared compilation", "[audio][graph]"
) {
	AudioGraph graph;
	const NodeId first = graph.Add(NodeKind::Bus);
	const NodeId second = graph.Add(NodeKind::Bus);
	const NodeId loose = graph.Add(NodeKind::Player);
	const NodeId left = graph.Add(NodeKind::Player);
	const NodeId right = graph.Add(NodeKind::Player);
	REQUIRE(graph.Connect(left, first));
	REQUIRE(graph.Connect(right, second));
	const std::array expected{graph.Output(), loose, left, right, first, second};
	CHECK(std::equal(graph.Order().begin(), graph.Order().end(), expected.begin(), expected.end()));
	REQUIRE(graph.Connect(right, graph.Output()));
	REQUIRE(graph.Connect(left, graph.Output()));
	const auto inputs = graph.InputsOf(graph.Output());
	REQUIRE(inputs.size() == 2);
	CHECK(inputs[0] == right);
	CHECK(inputs[1] == left);
	const std::vector<NodeId> savedOrder(graph.Order().begin(), graph.Order().end());
	REQUIRE(graph.Connect(first, second));
	const std::vector<NodeId> acceptedOrder(graph.Order().begin(), graph.Order().end());
	const auto acceptedInputs = graph.InputsOf(graph.Output());
	CHECK_FALSE(graph.Connect(second, first));
	CHECK(std::equal(graph.Order().begin(), graph.Order().end(), acceptedOrder.begin(), acceptedOrder.end()));
	CHECK(graph.InputsOf(graph.Output()).data() == acceptedInputs.data());
	CHECK(graph.InputsOf(graph.Output())[0] == right);
	REQUIRE(graph.Disconnect(first, second));
	CHECK(std::equal(graph.Order().begin(), graph.Order().end(), savedOrder.begin(), savedOrder.end()));
}

TEST_CASE(
	"prepared routing mutators and rebuilds allocate nothing through the full node capacity",
	"[audio][graph][heap]"
) {
	using engine::core::HeapProfile;
	if (!HeapProfile::IsCompiledIn()) SKIP("allocator hooks are required for the routing allocation check");
	uint64_t constructorBytes = 0;
	uint64_t constructorBlocks = 0;
	const uint32_t previous = HeapProfile::Push("audio.graph.construction.check");
	const uint32_t constructorTag = HeapProfile::Current();
	const auto beforeConstructor = HeapProfile::Node(constructorTag);
	AudioGraph graph;
	const auto afterConstructor = HeapProfile::Node(constructorTag);
	HeapProfile::Pop(previous);
	constructorBytes = afterConstructor.TotalBytes - beforeConstructor.TotalBytes;
	constructorBlocks = afterConstructor.TotalBlocks - beforeConstructor.TotalBlocks;
	CAPTURE(constructorBytes, constructorBlocks);

	std::array<NodeId, AudioGraph::MAXIMUM_NODES - 1> nodes{};
	engine::core::HeapNodeView before;
	engine::core::HeapNodeView after;
	bool accepted = true;
	bool fullRefused = false;
	bool cycleRefused = false;
	size_t orderedCount = 0;
	size_t inputCount = 0;
	{
		ENGINE_HEAP_SCOPE("audio.graph.routing.check");
		const uint32_t tag = HeapProfile::Current();
		before = HeapProfile::Node(tag);
		for (NodeId &node : nodes) {
			node = graph.Add(NodeKind::Bus);
			accepted = accepted && node.IsValid();
		}
		fullRefused = !graph.Add(NodeKind::Player).IsValid();
		for (const NodeId node : nodes)
			accepted = graph.Connect(node, graph.Output()) && accepted;
		orderedCount = graph.Order().size();
		inputCount = graph.InputsOf(graph.Output()).size();
		accepted = graph.Connect(nodes[0], nodes[1]) && accepted;
		cycleRefused = !graph.Connect(nodes[1], nodes[0]);
		accepted = graph.Disconnect(nodes[0], nodes[1]) && accepted;
		accepted = graph.Remove(nodes.back()) && accepted;
		accepted = graph.Adopt(NodeId{5000}, NodeKind::Player) && accepted;
		accepted = graph.Connect(NodeId{5000}, graph.Output()) && accepted;
		accepted = graph.Order().size() == AudioGraph::MAXIMUM_NODES && accepted;
		const auto inputs = graph.InputsOf(graph.Output());
		accepted = !inputs.empty() && inputs.back() == NodeId{5000} && accepted;
		after = HeapProfile::Node(tag);
	}
	CHECK(constructorBytes > 0);
	CHECK(constructorBlocks > 0);
	CHECK(accepted);
	CHECK(fullRefused);
	CHECK(cycleRefused);
	CHECK(orderedCount == AudioGraph::MAXIMUM_NODES);
	CHECK(inputCount == AudioGraph::MAXIMUM_NODES - 1);
	CHECK(after.TotalBlocks == before.TotalBlocks);
	CHECK(after.TotalBytes == before.TotalBytes);
}

TEST_CASE("copied and moved routing retain prepared mutation capacity", "[audio][graph][heap]") {
	using engine::core::HeapProfile;
	if (!HeapProfile::IsCompiledIn())
		SKIP("allocator hooks are required for the copied routing allocation check");
	AudioGraph original;
	const NodeId player = original.Add(NodeKind::Player);
	REQUIRE(original.Connect(player, original.Output()));
	AudioGraph copied(original);
	AudioGraph assigned;
	assigned = original;
	AudioGraph moved(std::move(copied));
	CHECK(copied.Count() == 1);
	CHECK(moved.InputsOf(moved.Output()).size() == 1);
	engine::core::HeapNodeView before;
	engine::core::HeapNodeView after;
	bool accepted = true;
	{
		ENGINE_HEAP_SCOPE("audio.graph.copy.routing.check");
		const uint32_t tag = HeapProfile::Current();
		before = HeapProfile::Node(tag);
		for (AudioGraph *graph : {&copied, &assigned, &moved}) {
			const size_t initialCount = graph->Count();
			for (size_t index = 0; index < 32; ++index) {
				const NodeId node = graph->Add(NodeKind::Bus);
				accepted = node.IsValid() && graph->Connect(node, graph->Output()) && accepted;
			}
			accepted = graph->Order().size() == initialCount + 32 && accepted;
		}
		after = HeapProfile::Node(tag);
	}
	CHECK(accepted);
	CHECK(after.TotalBlocks == before.TotalBlocks);
	CHECK(after.TotalBytes == before.TotalBytes);
}
