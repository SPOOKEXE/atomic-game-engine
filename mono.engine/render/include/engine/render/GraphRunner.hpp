#pragma once

// The seam between a render graph and the code that submits passes.
//
// **D00002's seam, and deliberately the half with no device in it.**
// `Renderer::Render` used to walk a hand-written list and submit six passes by
// name while the graph only described them. It runs `graph::Execute` now, and
// this is what `Execute` calls: a table from a node's kind to something that
// submits it. Getting there was a rewrite of the largest function in the engine,
// done in increments that each kept `just studio-smoke` byte-identical — and
// this is the part that can be tested without a GPU at all.
//
// ## What it is
//
// A table from a node's `Kind` to something that submits it, plus the
// `graph::NodeRunner` that looks a node up and calls it. `graph::Execute`
// already walks the compiled blocks in the right order, once per world for the
// shared work and once per view for the rest — so the ordering, the partition
// and the world grouping are all *already solved* and none of it needs
// restating here.
//
// ## What it is not
//
// It does not know what a pass *does*. Every handler is registered by whoever
// owns the device state, which is `Renderer`'s private implementation — so this
// header pulls in no SDL and can be exercised by a suite that registers
// handlers which do nothing but write their name down.
//
// **A missing handler is a refusal, not a skip.** A frame that quietly omitted
// a pass nobody had registered would render dark and blame the scene; saying so
// is the difference between a diagnostic and an afternoon.
//
// @tier L12 · client

#include <engine/core/Name.hpp>
#include <engine/graph/RenderGraph.hpp>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::render {

	// Submits one node.
	//
	// @param context Which node, which view, which world — `graph::RunContext`.
	// @return `false` to abandon the frame, which `graph::Execute` propagates.
	using PassHandler = std::function<bool(const graph::RunContext &context)>;

	// What each sort of node does.
	//
	// **Keyed on `Kind` and not on `Name`**, which is the distinction
	// `graph::Node` draws and the reason it draws it: two shadow passes for two
	// lights are one kind and two names, and a table keyed on the name would
	// need an entry per instance of a pass rather than one per sort.
	//
	// @since v0.11
	class PassTable {
	  public:
		// Registers a handler, replacing any of that kind.
		//
		// @param kind    Which sort of node.
		// @param handler What to do. Copied.
		// @return `false` for an unnamed kind or an empty handler.
		bool Set(core::Name kind, PassHandler handler);

		// Whether a kind has a handler.
		//
		// @param kind Which sort.
		// @return Whether `Run` would find something.
		bool Has(core::Name kind) const;

		// How many kinds are registered.
		size_t Count() const {
			return Handlers.size();
		}

		// Forgets everything.
		void Clear();

		// Every kind a graph names that this table cannot submit.
		//
		// **Asked before a frame rather than discovered during one.** A missing
		// handler found halfway through a frame has already submitted half of
		// it, and the half it did submit is the half that gets blamed. This lets
		// a caller check the whole graph up front and say which passes it cannot
		// draw.
		//
		// @param graph The pipeline.
		// @return The kinds with no handler, sorted, without duplicates. Empty
		//         means every enabled node can be submitted.
		std::vector<core::Name> Missing(const graph::RenderGraph &graph) const;

		// The handler for a kind, or null.
		//
		// **Returns a pointer rather than a copy**, because a `std::function`
		// holding a lambda that captured the renderer's state is not something
		// to duplicate once per node per view per frame.
		//
		// @param kind Which sort of node.
		// @return The handler, or null when nothing registered that kind.
		const PassHandler *Find(core::Name kind) const;

	  private:
		std::unordered_map<uint32_t, PassHandler> Handlers;
	};

	// Runs a compiled graph's nodes through a `PassTable`.
	//
	// @since v0.11
	class GraphRunner : public graph::NodeRunner {
	  public:
		// @param table Where the handlers live. Must outlive the runner.
		explicit GraphRunner(const PassTable &table) : Table(table) {}

		// Submits one node, or refuses.
		//
		// @param context What to run.
		// @return `false` when the kind has no handler or the handler failed.
		bool Run(const graph::RunContext &context) override;

		// Which kind was asked for and had no handler, or an invalid name.
		//
		// **Kept so a caller can say *which* pass it could not draw.** Returning
		// `false` alone would make every failure look the same, and the two —
		// "nothing knows how to do this" and "the device refused" — want
		// different answers.
		core::Name Unhandled() const {
			return Missing_;
		}

		// How many nodes were submitted.
		size_t Submitted() const {
			return Count_;
		}

	  private:
		const PassTable &Table;
		core::Name Missing_;
		size_t Count_ = 0;
	};
}
