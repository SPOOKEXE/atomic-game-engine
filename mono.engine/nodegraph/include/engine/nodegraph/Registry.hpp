#pragma once

// What kinds of node exist, and what each one computes.
//
// The extension point. Adding a node is one `Register` call and nothing else:
// the palette, the painter, the hit test, the evaluator and the save format all
// read this table, so a node type that appears in one appears in all of them.

#include <engine/nodegraph/Types.hpp>

#include <any>
#include <atomic>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace engine::nodegraph {

	// What an evaluation sees: the node's knobs, and whatever its inputs made.
	struct Inputs {
		// The node's own knobs, borrowed from it. Null is a node with none.
		const std::unordered_map<std::string, Value> *Widgets = nullptr;

		// Whatever the upstream nodes produced, by input port name. A port with
		// nothing connected is absent rather than empty.
		std::unordered_map<std::string, std::any> Ports;

		// Says how far along an async evaluation is.
		//
		// **Called from the worker thread, so it must stay cheap and must not
		// touch the graph.** What it moves is one atomic and one short string
		// behind a lock; the canvas reads them on the frame thread and draws a
		// bar. A node that reported by writing into its own outputs would be
		// publishing a half-computed result, which is the thing a progress bar
		// exists to avoid.
		//
		// Empty for a sync node — calling it is still safe and does nothing, so
		// one implementation can be either.
		std::function<void(size_t step, float fraction, std::string_view note)> Report;

		// Set when the editor is closing and a worker should give up.
		//
		// **Polled rather than enforced.** A task that ignores it finishes and
		// its result is dropped, which costs a moment at shutdown rather than a
		// hang — but a long node that never looks is a long shutdown, so a loop
		// that runs for more than a frame should check it.
		const std::atomic<bool> *Stopping = nullptr;

		// Whether the run wants to stop. Safe with no flag set.
		bool Cancelled() const {
			return Stopping != nullptr && Stopping->load(std::memory_order_relaxed);
		}

		// One of this node's knobs.
		//
		// @param key The knob's `WidgetSpec::Key`.
		// @return Its value, or a default-constructed one when there is no such
		//         knob — which is what a document naming a knob this build's
		//         node type no longer declares produces.
		Value Widget(const std::string &key) const;

		// One of this node's knobs as a number.
		//
		// `Widget(key).Number`, which is what almost every evaluation wants.
		//
		// @param key The knob's `WidgetSpec::Key`.
		// @return Its number, or zero when there is no such knob.
		double Real(const std::string &key) const;

		// A port's payload as `T`, or `fallback` when it is absent or is
		// something else.
		//
		// **`std::any`, because what flows down a wire is the caller's
		// business.** A pipeline graph passes render targets and a bake graph
		// passes images; a library that named either would be a library for one
		// of them. Never throws — an unconnected input is the ordinary case.
		template <typename T> T In(const std::string &name, T fallback = T{}) const {
			const auto found = Ports.find(name);
			if (found == Ports.end()) {
				return fallback;
			}
			const T *held = std::any_cast<T>(&found->second);
			return held != nullptr ? *held : fallback;
		}
	};

	// What one evaluation produced, by output port name.
	using Outputs = std::unordered_map<std::string, std::any>;

	// One kind of node. Registering one is the whole extension point: the
	// palette, the painter, the hit test, the evaluator and the save format all
	// read this table.
	struct NodeType {
		// What a document names this type by. A string for rule 4's reason: it
		// crosses a save file, so it cannot be a registration ordinal.
		std::string Id;

		// What a person reads in the palette and on the node's header.
		std::string Title;

		// Which palette section it appears under.
		std::string Category;

		// The header's colour, so a category is legible at a glance.
		Colour Accent;

		// A second line under the title, or empty for none.
		std::string Subtitle;

		// The ports on each side, top to bottom in this order.
		//@{
		std::vector<PortSpec> Inputs;
		std::vector<PortSpec> Outputs;
		//@}

		// The knobs on the body, in this order.
		std::vector<WidgetSpec> Widgets;

		// How wide a placed node is, in canvas units. The height follows from
		// the ports and knobs above.
		float Width = 180.0f;

		// What it computes, or nothing for a node that is only somewhere a wire
		// ends.
		//
		// **Pure, and that is load-bearing.** A result is cached against a hash
		// of the node's parameters and its inputs' hashes, so a function that
		// read a clock would produce a picture the cache then refuses to
		// recompute.
		//
		// Both names are qualified because this struct has members called
		// `Inputs` and `Outputs`; unqualified, the member wins and the error
		// names the alias rather than the shadowing.
		std::function<engine::nodegraph::Outputs(const engine::nodegraph::Inputs &)> Evaluate;

		// Whether that evaluation runs off the calling thread.
		//
		// **What makes this two kinds of node rather than one slow one.** A sync
		// node is evaluated inside `Run` and its result is there when `Run`
		// returns, which is what a graph of cheap arithmetic wants. An async one
		// is handed to a worker and collected by a later `Run`, so the editor
		// keeps drawing while it works and two branches that do not feed each
		// other run at once.
		//
		// **Everything an async node reads is copied before it is dispatched**,
		// because the graph is edited on the frame thread while the worker runs.
		// That is the same rule the engine applies to a world boundary and for
		// the same reason: a payload crossing a thread is a copy or it is a race.
		bool Async = false;

		// What the stages of an async evaluation are called, in order.
		//
		// Drawn under the progress bar as the node works, so "what is it doing"
		// has an answer that is not a spinner. Ignored by a sync node.
		std::vector<std::string> Steps;

		// Turns this node's output into a picture, or nothing for a node whose
		// output has none.
		//
		// **Given the payload rather than the node**, so a preview cannot depend
		// on a widget the evaluation did not read — a thumbnail that disagreed
		// with the result would be worse than none.
		//
		// Overrides the output port's `DataType::Preview` where both exist. Most
		// types want neither: a wire that knows how to draw itself draws every
		// node on it, and that is one function instead of twelve.
		std::function<bool(const std::any &, PreviewImage &)> Preview;

		// Which output port the preview reads. Empty takes the first declared.
		std::string PreviewPort;

		// Which inspector handler draws this type's panel.
		//
		// **A name and not a function**, so a node type stays a declaration that
		// the model layer can hold without an interface toolkit in it — the
		// handlers themselves are ImGui and live above `Canvas`.
		//
		// Empty is the ordinary case: `Inspectors::For` then infers one from
		// what the node actually produced, so a type nobody thought about gets
		// a panel that is right anyway.
		std::string Inspector;

		// Whether it is kept out of the palette and the library.
		//
		// **On the type rather than on a list in each panel**, so a type a host
		// places itself — the compressed node is the one here — does not have to
		// be taught to two panels separately.
		bool Hidden = false;
	};

	// Every registered node type.
	class NodeTypes {
	  public:
		// Adds a type, or replaces the one already under its id.
		//
		// @param type The type to register.
		static void Register(const NodeType &type);

		// The type under an id.
		//
		// @param id The id to look up.
		// @return The type, or null — which is what a document naming a type
		//         this build does not have produces.
		static const NodeType *Find(const std::string &id);

		// Every registered type, in registration order.
		//
		// @return The types. Valid until the next `Register`.
		static const std::vector<NodeType> &All();

		// The distinct `NodeType::Category` values, for the palette's sections.
		//
		// @return The categories, each once.
		static std::vector<std::string> Categories();

		// Types with an input that would take this type id.
		static std::vector<const NodeType *> AcceptingInput(const std::string &type);
	};
}
