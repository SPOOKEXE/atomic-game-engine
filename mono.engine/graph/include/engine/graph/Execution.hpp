#pragma once

// arch-waiver public-header: forward graph API. Hosts consume this device-free
// execution contract without a renderer or script VM.

// Cached, device-free execution for authored node graphs.
//
// Scheduling and node evaluation are deliberately separate. A dispatcher may
// hand a request to a job system, but only CompleteAsync mutates this runtime,
// so a worker never writes graph state while a tick is evaluating it.
//
// @tier L9 · shared

#include <engine/core/Name.hpp>
#include <engine/graph/NodeSchema.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace engine::graph {

	// The lifecycle of one evaluated node instance.
	enum class NodeExecutionState : uint8_t { Idle, Queued, Running, Complete, Error };

	// Stable diagnostic label for a node execution state.
	const char *Describe(NodeExecutionState state);

	// The result of a graph execution operation.
	enum class NodeExecutionStatus : uint8_t {
		Ok,
		InvalidNode,
		DuplicateNode,
		UnknownSchema,
		InvalidConnection,
		TypeMismatch,
		Cycle,
		Pending,
		StaleCompletion,
		EvaluationFailed,
	};

	// Stable diagnostic label for an execution result.
	const char *Describe(NodeExecutionStatus status);

	// An authored node instance. Id and Schema are stable names, while port names
	// remain strings because they are part of the serialized node contract.
	struct NodeExecutionNode {
		// Stable authored node name used by wires and serialized graphs.
		core::Name Id;
		// Stable schema name used to resolve this node's ports and evaluator.
		core::Name Schema;
		// Whether this node evaluates normally instead of applying BypassMode.
		bool Enabled = true;
		// How a node routes values while bypassed.
		NodeBypassMode BypassMode = NodeBypassMode::None;

		// A delay or feedback node supplies its cached output to its dependants.
		// Only this explicit boundary permits an otherwise cyclic graph.
		bool BreaksCycle = false;
	};

	// One output-to-input wire.
	struct NodeExecutionConnection {
		// Authored source node name for this wire.
		core::Name FromNode;
		// Output port name on FromNode that supplies this wire.
		std::string FromPort;
		// Authored destination node name for this wire.
		core::Name ToNode;
		// Input port name on ToNode that receives this wire.
		std::string ToPort;
	};

	// Public state owned by one node instance. Inputs are authored constants or
	// externally supplied values. Outputs are retained until invalidation or a
	// later successful evaluation replaces them.
	struct NodeExecutionRecord {
		// Cached evaluation state that controls scheduling and output validity.
		NodeExecutionState State = NodeExecutionState::Idle;
		// Whether inputs changed since the last evaluation.
		bool Dirty = true;
		// Revision assigned to the latest input set accepted for evaluation.
		uint64_t Revision = 0;
		// Inputs retained to compare a future request against this cached record.
		NodeValues Inputs;
		// Latest evaluated output values for the node.
		NodeValues Outputs;
		// Failure text from the node evaluation.
		std::string Error;
	};

	// The complete work handed to an asynchronous adapter. It contains copies,
	// so it can cross a worker or process boundary without graph pointers.
	struct NodeExecutionRequest {
		// Authored node name whose evaluator receives this request.
		core::Name Node;
		// Schema name expected by the evaluator receiving this request.
		core::Name Schema;
		// Input revision that a synchronous or asynchronous completion must match.
		uint64_t Revision = 0;
		// Copy of inputs supplied to the evaluator across the adapter boundary.
		NodeValues Inputs;
	};

	// The adapter result. Pending leaves the node Running until CompleteAsync;
	// Complete and Error finish inside Evaluate.
	enum class NodeDispatchResult : uint8_t { Complete, Pending, Error };

	// Adapter seam for compute backends and job systems. Implementations may run
	// synchronously or enqueue copied requests. They must not mutate NodeRuntime.
	class NodeExecutionDispatcher {
	  public:
		virtual ~NodeExecutionDispatcher() = default;

		// Evaluates or queues copied node work and returns its synchronous completion state.
		virtual NodeDispatchResult
		Dispatch(const NodeExecutionRequest &request, NodeValues &outputs, std::string &error) = 0;
	};

	// Owns one authored graph's cache, dirty propagation, and evaluation states.
	class NodeRuntime {
	  public:
		// Builds runtime state against a schema registry that must outlive it.
		explicit NodeRuntime(const NodeSchemaRegistry &schemas) : Schemas(schemas) {}

		// Adds an instance, refusing invalid names, duplicate ids, and unknown schemas.
		NodeExecutionStatus AddNode(NodeExecutionNode node);

		// Adds one validated output-to-input wire. A source `any` output is allowed
		// here, then checked against the actual value while evaluating.
		NodeExecutionStatus Connect(NodeExecutionConnection connection);

		// Changes an authored input and marks its dependent closure dirty.
		NodeExecutionStatus SetInput(core::Name node, std::string_view port, NodeValue input);

		// Changes node enable or bypass mode and marks its dependent closure dirty.
		NodeExecutionStatus SetEnabled(core::Name node, bool enabled);
		// Changes bypass routing and invalidates every downstream cached result.
		NodeExecutionStatus SetBypassMode(core::Name node, NodeBypassMode bypassMode);

		// Marks one node and every node fed from it dirty. Cached outputs remain
		// readable until a later successful evaluation replaces them.
		NodeExecutionStatus MarkDirty(core::Name node);

		// Evaluates the dirty closure in dependency order. Without a dispatcher it
		// invokes the schema evaluator. A Pending dispatch leaves the node Running.
		NodeExecutionStatus Evaluate(NodeExecutionDispatcher *dispatcher = nullptr);

		// Closes a request previously returned Pending. A completion with an older
		// revision is refused after the node's inputs have been invalidated. This is
		// the only mutation door intended for a worker completion callback.
		NodeExecutionStatus
		CompleteAsync(core::Name node, uint64_t revision, NodeValues outputs, std::string error = {});

		// Borrows the cached execution record for a node, or null when absent.
		const NodeExecutionRecord *Find(core::Name node) const;
		// Borrows the authored node declaration for a node, or null when absent.
		const NodeExecutionNode *FindNode(core::Name node) const;
		// Borrows the validated wires in their authored insertion order.
		const std::vector<NodeExecutionConnection> &Connections() const {
			return Wires;
		}

	  private:
		NodeExecutionStatus ValidateConnection(const NodeExecutionConnection &connection) const;
		NodeExecutionStatus
		BuildInputs(const NodeExecutionNode &node, NodeValues &inputs, std::string &error) const;
		NodeExecutionStatus Finish(core::Name node, NodeValues outputs, std::string error);
		NodeExecutionStatus BuildOrder(std::vector<size_t> &order) const;

		const NodeSchemaRegistry &Schemas;
		std::vector<NodeExecutionNode> Nodes;
		std::vector<NodeExecutionRecord> Records;
		std::vector<NodeExecutionConnection> Wires;
		std::unordered_map<core::Name, size_t> NodeIndexes;
	};
}
