#pragma once

// Turns a Graph into a Plan: flattened subgraphs, resolved generics, link
// conversions, waves from graphcore and a per-node hash for caching.
//
// A refused compile leaves the caller's previous Plan untouched, so a host can
// keep running the last good graph while the user fixes the new one.
// @tier L3 · shared

#include <engine/ang/Graph.hpp>
#include <engine/ang/Registry.hpp>
#include <engine/graphcore/Schema.hpp>

#include <memory>
#include <unordered_map>

namespace engine::ang {

	enum class DiagnosticCode : uint8_t {
		// graphcore refused; GraphStatus says why (cycle, limits, duplicates).
		Graph,
		UnknownType,
		UnknownPort,
		TypeMismatch,
		GenericConflict,
		AsyncInTick,
		InvalidBypass,
		SubgraphRecursion,
		SubgraphPort,
	};

	const char *Describe(DiagnosticCode code);

	struct Diagnostic {
		DiagnosticCode Code = DiagnosticCode::Graph;
		graphcore::CompileStatus GraphStatus = graphcore::CompileStatus::Ok;
		std::string Node{};
		std::string Port{};
		std::string Message{};
		// Closed cycle of node ids on a cycle refusal.
		std::vector<std::string> Cycle{};
	};

	struct CompileOptions {
		Domain Runs = Domain::Tool;
		size_t MaximumNodes = 4096;
		size_t MaximumLinks = 16384;
		size_t MaximumPorts = 65536;
		size_t MaximumIdentityBytes = 4 * 1024 * 1024;
		size_t MaximumSubgraphDepth = 16;
		// Diagnostics past this count are dropped; compile still refuses.
		size_t MaximumDiagnostics = 32;
	};

	// Where one input reads from.
	struct PlannedSource {
		uint32_t Node = 0;
		uint32_t Output = 0;
		// Set when the link converts the upstream type.
		std::function<Value(const Value &)> Convert{};
	};

	struct PlannedInput {
		std::string Port{};
		TypeId Type{};
		bool Optional = false;
		Value Default{};
		std::vector<PlannedSource> Sources{};
	};

	struct PlannedNode {
		// Flattened id; subgraph members read "outer/inner".
		std::string Id{};
		std::string DefId{};
		// Null for an unknown definition. Such a node never evaluates.
		std::shared_ptr<const NodeDef> Def{};
		// Params with definition defaults filled in. Shared so async jobs can hold them.
		std::shared_ptr<const std::map<std::string, Value>> Params{};
		bool Bypassed = false;
		ExecMode Mode = ExecMode::Sync;
		uint32_t Wave = 0;
		std::vector<PlannedInput> Inputs{};
		std::vector<std::string> OutputNames{};
		std::vector<TypeId> OutputTypes{};
		// DataType::Bytes per output, copied so the plan does not need the registry.
		std::vector<std::function<size_t(const Value &)>> OutputBytes{};
		// Def id, version, params, bypass and upstream hashes. No time, no addresses.
		uint64_t Hash = 0;
	};

	class Plan {
	  public:
		Domain Runs = Domain::Tool;
		std::vector<PlannedNode> Nodes{};
		std::vector<std::vector<uint32_t>> Waves{};
		// Combined hash of every node, for "did anything change".
		uint64_t Hash = 0;

		// Index of a node by flattened id, or -1.
		int64_t Find(std::string_view id) const;

		// Nodes whose hash differs from `previous` or that are new: the dirty
		// closure of whatever was edited, since hashes include upstream hashes.
		std::vector<bool> DirtySince(const Plan &previous) const;

		void Reindex();

	  private:
		std::unordered_map<std::string, uint32_t> Index{};
	};

	struct CompileResult {
		bool Ok = false;
		std::vector<Diagnostic> Diagnostics{};
	};

	// Compiles `graph` against `registry`. On success replaces `out`; on refusal
	// leaves it alone and lists bounded diagnostics.
	CompileResult
	Compile(const Graph &graph, const Registry &registry, const CompileOptions &options, Plan &out);

	// Whether a new link from `from`.`fromPort` to `to`.`toPort` would be accepted:
	// ports exist, types are compatible, the input has room and no cycle forms.
	// Views call this to highlight targets before the edit is applied.
	struct LinkVerdict {
		bool Ok = false;
		std::string Reason{};
		bool Converts = false;
	};
	LinkVerdict CheckNewLink(const Graph &graph, const Registry &registry, const Link &candidate);

	// Whether adding `candidate` would close a cycle, a self link included.
	// Looks only at links, so hosts with their own port rules can still ask.
	bool WouldCycle(const Graph &graph, const Link &candidate);

	// The type of a node port as declared, after resolving generics from the
	// node's current links. Empty when the node or port is unknown.
	TypeId PortType(
		const Graph &graph,
		const Registry &registry,
		std::string_view node,
		std::string_view port,
		Direction dir
	);
}
