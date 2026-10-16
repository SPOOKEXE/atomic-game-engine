#pragma once

// Shared dependency decisions without execution, domain payloads or device state.
// All indices are derived from the supplied declaration order and stay process-local.
// @tier L2 · shared

#include <engine/graphcore/Schema.hpp>

namespace engine::graphcore {

	// Serial readiness policy; dependency waves are independent of this choice.
	enum class OrderPolicy : uint8_t { AuthoredSweep, LowestReady, FifoReady };

	// One authored node instance referencing a domain schema by stable text.
	struct Node {
		// Durable document-local identity.
		std::string Id{};
		// Durable identity of a supplied node schema.
		std::string Schema{};
	};

	// One directed output-to-input authored connection.
	struct Connection {
		// Identity of the producing node.
		std::string FromNode{};
		// Output identity within the producing node's schema.
		std::string FromPort{};
		// Identity of the consuming node.
		std::string ToNode{};
		// Input identity within the consuming node's schema.
		std::string ToPort{};
	};

	// One prerequisite edge using dense caller-supplied node indices.
	struct Dependency {
		// Node that must finish first.
		NodeIndex Before = 0;
		// Node that consumes the prerequisite.
		NodeIndex After = 0;
	};

	// Derived immutable-by-convention decisions shared by separate domain runtimes.
	struct Topology {
		// Stable serial order under OrderPolicy; do not substitute flattened Waves.
		std::vector<NodeIndex> Order{};
		// Deduplicated upstream nodes per node, in ascending declaration order.
		std::vector<std::vector<NodeIndex>> Inputs{};
		// Deduplicated downstream nodes per node, in ascending declaration order.
		std::vector<std::vector<NodeIndex>> Dependents{};
		// Dependency-depth waves; each wave follows declaration order.
		std::vector<std::vector<NodeIndex>> Waves{};
	};

	// One authored connection resolved into declaration-local node and port indices.
	struct ResolvedConnection {
		// Producing node index.
		NodeIndex FromNode = 0;
		// Index in the producing schema's Ports vector, including both directions.
		uint32_t FromPort = 0;
		// Consuming node index.
		NodeIndex ToNode = 0;
		// Index in the consuming schema's Ports vector, including both directions.
		uint32_t ToPort = 0;
	};

	// Complete typed compilation, with no domain runtime storage.
	struct CompiledGraph {
		// Dependency order, adjacency and waves.
		Topology Dependencies{};
		// Resolved pins in authored connection order; topology edges may be shared.
		std::vector<ResolvedConnection> Connections{};
	};

	// Allocation-free ordering result; failures leave caller-owned output unchanged.
	struct OrderResult {
		// Admission, endpoint or cycle result.
		CompileStatus Status = CompileStatus::Ok;
		// Number of order entries written on success, zero on refusal.
		size_t Count = 0;
		// Lowest blocked declaration on cycle refusal.
		NodeIndex Blocked = NO_NODE;
	};

	// Fixed-capacity scratch owned by a caller and allocated before realtime execution.
	// Construction and copying may allocate; CompileOrder never resizes these arrays.
	class OrderWorkspace {
	  public:
		// Prepares node and raw dependency capacity. Allocation failures escape setup.
		OrderWorkspace(size_t maximumNodes, size_t maximumDependencies);

	  private:
		std::vector<size_t> Offsets;
		std::vector<size_t> Incoming;
		std::vector<size_t> CursorDepth;
		std::vector<NodeIndex> Targets;
		std::vector<NodeIndex> Ready;
		std::vector<NodeIndex> NextReady;
		std::vector<NodeIndex> Candidate;

		friend OrderResult CompileOrder(
			size_t,
			std::span<const Dependency>,
			const Limits &,
			OrderWorkspace &,
			std::span<NodeIndex>,
			OrderPolicy
		) noexcept;
		friend CompileStatus CompileDependencies(
			size_t, std::span<const Dependency>, const Limits &, Topology &, Diagnostic &, OrderPolicy
		);
	};

	// Compiles serial order using only prepared workspace and a caller-owned output span.
	// Raw dependency multiplicity matters for FifoReady. All other policies preserve
	// their unique-edge ordering. Refusal writes neither partial order nor heap diagnostics.
	OrderResult CompileOrder(
		size_t nodeCount,
		std::span<const Dependency> dependencies,
		const Limits &limits,
		OrderWorkspace &workspace,
		std::span<NodeIndex> out,
		OrderPolicy policy = OrderPolicy::AuthoredSweep
	) noexcept;

	// Validates bounds and endpoints, deduplicates public adjacency and compiles topology.
	// FifoReady retains raw edge order and multiplicity for readiness transitions.
	// Empty graphs succeed. Cycles produce a closed path. Refusal preserves out.
	CompileStatus CompileDependencies(
		size_t nodeCount,
		std::span<const Dependency> dependencies,
		const Limits &limits,
		Topology &out,
		Diagnostic &diagnostic,
		OrderPolicy policy = OrderPolicy::AuthoredSweep
	);

	// Validates bounded declarations and typed pins, then compiles their dependencies.
	// Exact duplicate connections and disconnected required inputs refuse. Outputs fan
	// out regardless of MaximumConnections. Refusal preserves out; success resets diagnostic.
	CompileStatus CompileGraph(
		std::span<const Node> nodes,
		std::span<const Connection> connections,
		std::span<const NodeSchema> schemas,
		const Limits &limits,
		CompiledGraph &out,
		Diagnostic &diagnostic,
		OrderPolicy policy = OrderPolicy::AuthoredSweep
	);
}
