#include <engine/graphcore/Compile.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>

TEST_SUITE_ID("engine.graphcore.compile")
TEST_DEPENDS("engine.graphcore.schema")

using namespace engine::graphcore;

namespace {
	Limits GraphLimits() {
		return {
			.MaximumNodes = 4096,
			.MaximumConnections = 8192,
			.MaximumSchemas = 16,
			.MaximumPorts = 64,
			.MaximumIdentityBytes = 1024 * 1024,
		};
	}

	void CheckTopology(const Topology &actual, const Topology &expected) {
		CHECK(actual.Order == expected.Order);
		CHECK(actual.Inputs == expected.Inputs);
		CHECK(actual.Dependents == expected.Dependents);
		CHECK(actual.Waves == expected.Waves);
	}

	void CheckConnections(const CompiledGraph &actual, const CompiledGraph &expected) {
		CheckTopology(actual.Dependencies, expected.Dependencies);
		REQUIRE(actual.Connections.size() == expected.Connections.size());
		for (size_t index = 0; index < actual.Connections.size(); ++index) {
			CHECK(actual.Connections[index].FromNode == expected.Connections[index].FromNode);
			CHECK(actual.Connections[index].FromPort == expected.Connections[index].FromPort);
			CHECK(actual.Connections[index].ToNode == expected.Connections[index].ToNode);
			CHECK(actual.Connections[index].ToPort == expected.Connections[index].ToPort);
		}
	}

	std::vector<NodeSchema> Schemas() {
		return {
			{.Id = "source",
			 .Ports = {{.Id = "image", .ValueType = "image", .Direction = PortDirection::Output}}},
			{.Id = "blend",
			 .Ports = {
				 {.Id = "background", .ValueType = "image"},
				 {.Id = "foreground", .ValueType = "image"},
				 {.Id = "image", .ValueType = "image", .Direction = PortDirection::Output},
			 }},
		};
	}
}

TEST_CASE("serial policies preserve their distinct legacy readiness order", "[graphcore][compile]") {
	const std::array dependencies{Dependency{3, 0}, Dependency{4, 1}};
	Topology swept;
	Topology lowest;
	Diagnostic diagnostic;
	REQUIRE(CompileDependencies(5, dependencies, GraphLimits(), swept, diagnostic) == CompileStatus::Ok);
	REQUIRE(
		CompileDependencies(5, dependencies, GraphLimits(), lowest, diagnostic, OrderPolicy::LowestReady) ==
		CompileStatus::Ok
	);
	CHECK(swept.Order == std::vector<NodeIndex>{2, 3, 4, 0, 1});
	CHECK(lowest.Order == std::vector<NodeIndex>{2, 3, 0, 4, 1});
	CHECK(swept.Waves == std::vector<std::vector<NodeIndex>>{{2, 3, 4}, {0, 1}});
	CHECK(lowest.Waves == swept.Waves);
}

TEST_CASE("dependency diamonds deduplicate edges and retain detached nodes", "[graphcore][compile]") {
	std::vector dependencies{
		Dependency{2, 3}, Dependency{0, 2}, Dependency{1, 3}, Dependency{0, 1}, Dependency{0, 1}
	};
	Topology topology;
	Diagnostic diagnostic;
	REQUIRE(CompileDependencies(5, dependencies, GraphLimits(), topology, diagnostic) == CompileStatus::Ok);
	CHECK(topology.Order == std::vector<NodeIndex>{0, 1, 2, 3, 4});
	CHECK(topology.Inputs[3] == std::vector<NodeIndex>{1, 2});
	CHECK(topology.Dependents[0] == std::vector<NodeIndex>{1, 2});
	CHECK(topology.Waves == std::vector<std::vector<NodeIndex>>{{0, 4}, {1, 2}, {3}});
	const Topology saved = topology;
	std::reverse(dependencies.begin(), dependencies.end());
	REQUIRE(CompileDependencies(5, dependencies, GraphLimits(), topology, diagnostic) == CompileStatus::Ok);
	CheckTopology(topology, saved);
}

TEST_CASE("FIFO readiness retains initial sources and authored outgoing order", "[graphcore][compile]") {
	std::vector dependencies{Dependency{0, 2}, Dependency{0, 1}};
	Topology fifo;
	Topology swept;
	Topology lowest;
	Diagnostic diagnostic;
	REQUIRE(
		CompileDependencies(4, dependencies, GraphLimits(), fifo, diagnostic, OrderPolicy::FifoReady) ==
		CompileStatus::Ok
	);
	CHECK(fifo.Order == std::vector<NodeIndex>{0, 3, 2, 1});
	CHECK(fifo.Dependents[0] == std::vector<NodeIndex>{1, 2});
	CHECK(fifo.Inputs[1] == std::vector<NodeIndex>{0});
	CHECK(fifo.Inputs[2] == std::vector<NodeIndex>{0});
	CHECK(fifo.Waves == std::vector<std::vector<NodeIndex>>{{0, 3}, {1, 2}});
	REQUIRE(CompileDependencies(4, dependencies, GraphLimits(), swept, diagnostic) == CompileStatus::Ok);
	REQUIRE(
		CompileDependencies(4, dependencies, GraphLimits(), lowest, diagnostic, OrderPolicy::LowestReady) ==
		CompileStatus::Ok
	);
	CHECK(swept.Order == std::vector<NodeIndex>{0, 1, 2, 3});
	CHECK(lowest.Order == swept.Order);
	const Topology savedFifo = fifo;
	const Topology savedSwept = swept;
	const Topology savedLowest = lowest;
	dependencies = {{0, 1}, {0, 2}, {0, 2}};
	REQUIRE(
		CompileDependencies(4, dependencies, GraphLimits(), fifo, diagnostic, OrderPolicy::FifoReady) ==
		CompileStatus::Ok
	);
	CHECK(fifo.Order == std::vector<NodeIndex>{0, 3, 1, 2});
	CHECK(fifo.Waves == savedFifo.Waves);
	CHECK(fifo.Inputs == savedFifo.Inputs);
	CHECK(fifo.Dependents == savedFifo.Dependents);
	REQUIRE(CompileDependencies(4, dependencies, GraphLimits(), swept, diagnostic) == CompileStatus::Ok);
	REQUIRE(
		CompileDependencies(4, dependencies, GraphLimits(), lowest, diagnostic, OrderPolicy::LowestReady) ==
		CompileStatus::Ok
	);
	CheckTopology(swept, savedSwept);
	CheckTopology(lowest, savedLowest);
	REQUIRE(
		CompileDependencies(
			3,
			std::array{Dependency{0, 2}, Dependency{0, 1}},
			GraphLimits(),
			fifo,
			diagnostic,
			OrderPolicy::FifoReady
		) == CompileStatus::Ok
	);
	CHECK(fifo.Order == std::vector<NodeIndex>{0, 2, 1});
}

TEST_CASE(
	"FIFO repeated prerequisites release consumers at their last authored occurrence", "[graphcore][compile]"
) {
	const std::array dependencies{Dependency{0, 1}, Dependency{0, 2}, Dependency{0, 1}};
	Topology fifo;
	Topology swept;
	Topology lowest;
	Diagnostic diagnostic;
	REQUIRE(
		CompileDependencies(3, dependencies, GraphLimits(), fifo, diagnostic, OrderPolicy::FifoReady) ==
		CompileStatus::Ok
	);
	CHECK(fifo.Order == std::vector<NodeIndex>{0, 2, 1});
	CHECK(fifo.Inputs[1] == std::vector<NodeIndex>{0});
	CHECK(fifo.Dependents[0] == std::vector<NodeIndex>{1, 2});
	CHECK(fifo.Waves == std::vector<std::vector<NodeIndex>>{{0}, {1, 2}});
	REQUIRE(CompileDependencies(3, dependencies, GraphLimits(), swept, diagnostic) == CompileStatus::Ok);
	REQUIRE(
		CompileDependencies(3, dependencies, GraphLimits(), lowest, diagnostic, OrderPolicy::LowestReady) ==
		CompileStatus::Ok
	);
	CHECK(swept.Order == std::vector<NodeIndex>{0, 1, 2});
	CHECK(lowest.Order == swept.Order);
	CHECK(fifo.Waves == swept.Waves);
}

TEST_CASE("deep reversed chains do not recurse and retain one wave per dependency", "[graphcore][compile]") {
	constexpr NodeIndex COUNT = 4096;
	std::vector<Dependency> dependencies;
	for (NodeIndex index = 1; index < COUNT; ++index)
		dependencies.push_back({index, index - 1});
	Topology topology;
	Diagnostic diagnostic;
	REQUIRE(
		CompileDependencies(COUNT, dependencies, GraphLimits(), topology, diagnostic) == CompileStatus::Ok
	);
	REQUIRE(topology.Order.size() == COUNT);
	REQUIRE(topology.Waves.size() == COUNT);
	for (NodeIndex index = 0; index < COUNT; ++index) {
		CHECK(topology.Order[index] == COUNT - index - 1);
		CHECK(topology.Waves[index] == std::vector<NodeIndex>{COUNT - index - 1});
	}
}

TEST_CASE("cycle refusal reports an actual closed path and the lowest blocked node", "[graphcore][compile]") {
	Topology topology;
	Diagnostic diagnostic;
	REQUIRE(CompileDependencies(2, {}, GraphLimits(), topology, diagnostic) == CompileStatus::Ok);
	const Topology saved = topology;
	std::vector dependencies{Dependency{2, 3}, Dependency{3, 2}, Dependency{2, 0}};
	SECTION("cycle with a lower downstream blocked declaration") {}
	SECTION("self cycle") {
		dependencies = {{0, 0}};
	}
	REQUIRE(
		CompileDependencies(4, dependencies, GraphLimits(), topology, diagnostic) == CompileStatus::Cycle
	);
	CHECK(diagnostic.Blocked == 0);
	REQUIRE(diagnostic.Cycle.size() >= 2);
	CHECK(diagnostic.Cycle.front() == diagnostic.Cycle.back());
	CHECK(diagnostic.Cycle.size() <= 5);
	for (size_t index = 1; index < diagnostic.Cycle.size(); ++index) {
		CHECK(std::any_of(dependencies.begin(), dependencies.end(), [&](const Dependency &edge) {
			return edge.Before == diagnostic.Cycle[index - 1] && edge.After == diagnostic.Cycle[index];
		}));
	}
	CheckTopology(topology, saved);
}

TEST_CASE(
	"dependency bounds and endpoints refuse without replacing an accepted plan", "[graphcore][compile]"
) {
	Topology topology;
	Diagnostic diagnostic;
	REQUIRE(CompileDependencies(2, {}, GraphLimits(), topology, diagnostic) == CompileStatus::Ok);
	const Topology saved = topology;
	Limits limits = GraphLimits();
	std::vector<Dependency> dependencies;
	CompileStatus expected = CompileStatus::LimitExceeded;
	SECTION("node count") {
		limits.MaximumNodes = 1;
	}
	SECTION("connection count counts duplicates before deduplication") {
		limits.MaximumConnections = 1;
		dependencies = {{0, 1}, {0, 1}};
	}
	SECTION("endpoint") {
		dependencies = {{0, 2}};
		expected = CompileStatus::InvalidDependency;
	}
	CHECK(CompileDependencies(2, dependencies, limits, topology, diagnostic) == expected);
	CheckTopology(topology, saved);
	CHECK(
		CompileDependencies(2, {}, GraphLimits(), topology, diagnostic, static_cast<OrderPolicy>(255)) ==
		CompileStatus::InvalidDependency
	);
	CheckTopology(topology, saved);
	REQUIRE(CompileDependencies(0, {}, {}, topology, diagnostic) == CompileStatus::Ok);
	CHECK(topology.Order.empty());
	CHECK(topology.Inputs.empty());
	CHECK(topology.Waves.empty());
}

TEST_CASE(
	"typed compilation preserves authored pin order while sharing dependency edges", "[graphcore][compile]"
) {
	const std::array nodes{Node{"mixed", "blend"}, Node{"solid", "source"}};
	const std::array connections{
		Connection{"solid", "image", "mixed", "foreground"},
		Connection{"solid", "image", "mixed", "background"}
	};
	CompiledGraph graph;
	Diagnostic diagnostic;
	REQUIRE(
		CompileGraph(nodes, connections, Schemas(), GraphLimits(), graph, diagnostic) == CompileStatus::Ok
	);
	CHECK(graph.Dependencies.Order == std::vector<NodeIndex>{1, 0});
	CHECK(graph.Dependencies.Inputs[0] == std::vector<NodeIndex>{1});
	REQUIRE(graph.Connections.size() == 2);
	CHECK(graph.Connections[0].FromNode == 1);
	CHECK(graph.Connections[0].FromPort == 0);
	CHECK(graph.Connections[0].ToNode == 0);
	CHECK(graph.Connections[0].ToPort == 1);
	CHECK(graph.Connections[1].ToPort == 0);
}

TEST_CASE(
	"typed graph refusals preserve accepted plans and identify offending pins", "[graphcore][compile]"
) {
	std::vector nodes{Node{"solid", "source"}, Node{"mixed", "blend"}};
	std::vector connections{
		Connection{"solid", "image", "mixed", "background"},
		Connection{"solid", "image", "mixed", "foreground"}
	};
	std::vector schemas = Schemas();
	CompiledGraph graph;
	Diagnostic diagnostic;
	REQUIRE(CompileGraph(nodes, connections, schemas, GraphLimits(), graph, diagnostic) == CompileStatus::Ok);
	const CompiledGraph saved = graph;
	CompileStatus expected = CompileStatus::UnknownPort;
	SECTION("source port direction") {
		connections[0].FromPort = "background";
	}
	SECTION("destination port direction") {
		connections[0].ToPort = "image";
	}
	SECTION("missing source node") {
		connections[0].FromNode = "absent";
		expected = CompileStatus::UnknownNode;
	}
	SECTION("missing destination node") {
		connections[0].ToNode = "absent";
		expected = CompileStatus::UnknownNode;
	}
	SECTION("unknown schema") {
		nodes[0].Schema = "absent";
		expected = CompileStatus::UnknownSchema;
	}
	SECTION("duplicate node") {
		nodes.push_back(nodes[0]);
		expected = CompileStatus::DuplicateNode;
	}
	SECTION("incompatible types") {
		schemas[0].Ports[0].ValueType = "number";
		expected = CompileStatus::TypeMismatch;
	}
	SECTION("exact duplicate connection") {
		connections.push_back(connections[0]);
		expected = CompileStatus::DuplicateConnection;
	}
	SECTION("excess input connections") {
		nodes.push_back({"other", "source"});
		connections.push_back({"other", "image", "mixed", "background"});
		expected = CompileStatus::TooManyConnections;
	}
	SECTION("missing required input") {
		connections.pop_back();
		expected = CompileStatus::MissingInput;
	}
	CHECK(CompileGraph(nodes, connections, schemas, GraphLimits(), graph, diagnostic) == expected);
	CHECK(diagnostic.Status == expected);
	CHECK_FALSE(diagnostic.Node.empty());
	CheckConnections(graph, saved);
}

TEST_CASE(
	"optional inputs and unlimited fan-in keep domain-defined connection semantics", "[graphcore][compile]"
) {
	std::vector schemas = Schemas();
	schemas[1].Ports[0].MaximumConnections = 0;
	schemas[1].Ports[1].Optional = true;
	const std::array nodes{Node{"one", "source"}, Node{"two", "source"}, Node{"mixed", "blend"}};
	const std::array connections{
		Connection{"one", "image", "mixed", "background"}, Connection{"two", "image", "mixed", "background"}
	};
	CompiledGraph graph;
	Diagnostic diagnostic;
	REQUIRE(CompileGraph(nodes, connections, schemas, GraphLimits(), graph, diagnostic) == CompileStatus::Ok);
	CHECK(graph.Dependencies.Inputs[2] == std::vector<NodeIndex>{0, 1});
	SECTION("source any") {
		schemas[0].Ports[0].ValueType = "any";
	}
	SECTION("destination any") {
		schemas[1].Ports[0].ValueType = "any";
	}
	REQUIRE(CompileGraph(nodes, connections, schemas, GraphLimits(), graph, diagnostic) == CompileStatus::Ok);
}

TEST_CASE(
	"typed cycles identify a blocked authored node and retain the last good plan", "[graphcore][compile]"
) {
	std::vector schemas = Schemas();
	schemas[1].Ports[1].Optional = true;
	const std::array nodes{Node{"a", "blend"}, Node{"b", "blend"}};
	const std::array connections{
		Connection{"b", "image", "a", "background"}, Connection{"a", "image", "b", "background"}
	};
	CompiledGraph graph;
	Diagnostic diagnostic;
	REQUIRE(CompileGraph({}, {}, {}, {}, graph, diagnostic) == CompileStatus::Ok);
	const CompiledGraph saved = graph;
	CHECK(
		CompileGraph(nodes, connections, schemas, GraphLimits(), graph, diagnostic) == CompileStatus::Cycle
	);
	CHECK(diagnostic.Node == "a");
	CHECK(diagnostic.Blocked == 0);
	CHECK(diagnostic.Cycle == std::vector<NodeIndex>{0, 1, 0});
	CheckConnections(graph, saved);
}

TEST_CASE("typed graph budgets include node and pin text as well as schema text", "[graphcore][compile]") {
	const std::array nodes{Node{"solid", "source"}, Node{"mixed", "blend"}};
	const std::array connections{
		Connection{"solid", "image", "mixed", "background"},
		Connection{"solid", "image", "mixed", "foreground"}
	};
	const std::vector schemas = Schemas();
	CompiledGraph graph;
	Diagnostic diagnostic;
	REQUIRE(CompileGraph(nodes, connections, schemas, GraphLimits(), graph, diagnostic) == CompileStatus::Ok);
	const CompiledGraph saved = graph;
	Limits limits = GraphLimits();
	SECTION("node budget") {
		limits.MaximumNodes = 1;
	}
	SECTION("connection budget") {
		limits.MaximumConnections = 1;
	}
	SECTION("schema budget") {
		limits.MaximumSchemas = 1;
	}
	SECTION("port budget") {
		limits.MaximumPorts = 3;
	}
	SECTION("identity byte budget") {
		limits.MaximumIdentityBytes = 20;
	}
	CHECK(
		CompileGraph(nodes, connections, schemas, limits, graph, diagnostic) == CompileStatus::LimitExceeded
	);
	CheckConnections(graph, saved);
}
