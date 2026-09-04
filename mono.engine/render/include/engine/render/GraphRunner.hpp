#pragma once

// arch-waiver public-header: forward renderer API. Rendering hosts use this
// complete compiled-graph execution contract.

// The device-free dispatch seam between a compiled render graph and its GPU
// backend. The table is assembled by the renderer, then RenderGraph owns the
// traversal. A missing implementation is rejected before recording starts.

#include <engine/core/Name.hpp>
#include <engine/graph/RenderGraph.hpp>

#include <functional>
#include <utility>
#include <vector>

namespace engine::render {
	// Runtime instrumentation paid for by graph execution.
	//
	// @since v0.20
	enum class ProfilingTier : uint8_t {
		// No per-node CPU scopes or GPU marks.
		Off,

		// Per-node CPU scopes only.
		Cpu,

		// CPU scopes and backend timestamp marks.
		Full,
	};

	// Backend hooks around one logical graph node.
	//
	// GraphRunner owns assignment and accounting. The device adapter owns the
	// command buffer and reports how many mark writes could not be recorded.
	//
	// @since v0.20
	struct NodeProfileHooks {
		// Optional per-node opt-out. Empty means enabled.
		std::function<bool(const graph::RunContext &)> Enabled;

		// Opens the logical node's device timing range.
		std::function<void(const graph::RunContext &)> Begin;

		// Closes the range and returns dropped timestamp mark writes.
		std::function<size_t(const graph::RunContext &)> End;
	};

	// What one node kind does when the graph reaches it.
	//
	// **A `std::function` rather than a virtual class**, because a handler is
	// usually a lambda closing over one backend object and a class per node kind
	// would be a file per node kind. The context carries everything a handler is
	// allowed to see.
	//
	// @return `false` to fail the frame, which stops recording rather than
	//         leaving a half-built command buffer to submit.
	using NodeHandler = std::function<bool(const graph::RunContext &)>;

	// Which node kinds this backend can record, by name.
	//
	// **Assembled by the renderer and read by the graph**, which is the whole
	// seam: `mono.engine/graph` decides the order and never learns what a device
	// is, and this decides what a device does and never learns the order.
	class NodeTable {
	  public:
		// Keeps the built-in catalogue in one allocation. A renderer builds and
		// replaces this small table per view, so node-based map storage would pay
		// for an allocation per kind before command recording began.
		NodeTable();

		// Registers, or replaces, the handler for one node kind.
		//
		// @param kind    The authored node kind.
		// @param handler What to do when the graph reaches one.
		// @return `false` when the handler is empty, because a registered
		//         nothing is worse than an absence - `Missing` would stop
		//         reporting it.
		bool Set(core::Name kind, NodeHandler handler);

		// Whether a kind has a handler.
		//
		// @param kind The kind to look for.
		// @return `true` when one is registered.
		bool Has(core::Name kind) const;

		// The handler for a kind.
		//
		// @param kind The kind to look up.
		// @return The handler, or null. Valid until the next `Set` or `Clear`.
		const NodeHandler *Find(core::Name kind) const;

		// Every node kind the graph uses that this table cannot record.
		//
		// **Asked before recording starts, not during.** A backend discovering a
		// missing handler halfway through a frame has already submitted part of
		// it, and the failure lands on whichever node happened to be first.
		//
		// @param graph The graph about to run.
		// @return The unhandled kinds, each once. Empty means the graph runs.
		std::vector<core::Name> Missing(const graph::RenderGraph &graph) const;

		// How many kinds are registered.
		//
		// @return The count.
		size_t Count() const;

		// Forgets every handler, which is what a backend teardown calls.
		void Clear();

	  private:
		// A short flat table beats hashing here: the built-in catalogue has only a
		// few dozen entries and a frame visits each kind at most a handful of
		// times. The id keeps both replacement and lookup to integer compares.
		std::vector<std::pair<uint32_t, NodeHandler>> Handlers;
	};

	// Walks a compiled graph and records each node through the table.
	//
	// One per frame, or one reused: it carries the first failure and a count,
	// and nothing else.
	class GraphRunner final : public graph::NodeRunner {
	  public:
		// Binds a runner to a table.
		//
		// @param table The handlers. Borrowed, so it has to outlive the runner -
		//              in practice both belong to the renderer.
		// @param tier The profiling detail enabled for this run.
		// @param profile Hooks that open and close backend measurements.
		explicit GraphRunner(
			const NodeTable &table, ProfilingTier tier = ProfilingTier::Cpu, NodeProfileHooks profile = {}
		)
			: Table(table), Tier(tier), Profile(std::move(profile)) {}

		// Records one node, or refuses.
		//
		// @param context What the graph decided this invocation is.
		// @return `false` when the kind has no handler or the handler failed.
		bool Run(const graph::RunContext &context) override;

		// The node kind that had no handler.
		//
		// @return Its name, or an empty `Name` when nothing was missing. This is
		//         what turns "the frame failed" into a sentence naming what to
		//         register.
		core::Name Unhandled() const {
			return Missing;
		}

		// How many nodes were recorded.
		//
		// @return The count since construction.
		size_t Submitted() const {
			return SubmittedCount;
		}

		// Timestamp mark writes omitted because the backend budget filled.
		size_t DroppedProfileMarks() const {
			return DroppedMarks;
		}

	  private:
		// The handlers, borrowed.
		const NodeTable &Table;

		ProfilingTier Tier = ProfilingTier::Cpu;
		NodeProfileHooks Profile;

		// The first kind found without a handler.
		core::Name Missing;

		// How many nodes have been recorded.
		size_t SubmittedCount = 0;

		size_t DroppedMarks = 0;
	};
}
