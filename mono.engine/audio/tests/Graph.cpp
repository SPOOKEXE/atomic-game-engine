#include <engine/audio/Graph.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.audio.graph")
TEST_DEPENDS("engine.audio.sample")

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
