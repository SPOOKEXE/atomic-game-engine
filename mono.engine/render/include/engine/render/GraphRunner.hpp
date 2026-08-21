#pragma once

// The device-free dispatch seam between a compiled render graph and its GPU
// backend. The table is assembled by the renderer, then RenderGraph owns the
// traversal. A missing implementation is rejected before recording starts.

#include <engine/core/Name.hpp>
#include <engine/graph/RenderGraph.hpp>

#include <functional>
#include <unordered_map>
#include <vector>

namespace engine::render {

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
		// Handlers by interned name id. The id rather than the `core::Name`,
		// because that is what the hash would reduce to anyway.
		std::unordered_map<uint32_t, NodeHandler> Handlers;
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
		explicit GraphRunner(const NodeTable &table) : Table(table) {}

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

	  private:
		// The handlers, borrowed.
		const NodeTable &Table;

		// The first kind found without a handler.
		core::Name Missing;

		// How many nodes have been recorded.
		size_t SubmittedCount = 0;
	};
}
