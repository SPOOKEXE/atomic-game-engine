#pragma once

// The panel beside the canvas: what one selected node is, and what it made.
//
// **A registry rather than a switch**, for the reason the node types are one: a
// type added by anybody gets a panel with nothing here changing, and a type that
// wants its own says so by name. `Inspectors::For` falls back to what the node
// actually *produced* rather than to what it declared, so a type nobody thought
// about still gets a panel that is right.
//
// A handler draws the visualisation and nothing else. The title, the knobs and
// the port table are the same for every type and belong to whatever panel this
// is drawn in — a handler that was different about those would be one nobody
// could predict.

#include <engine/nodegraph/Evaluate.hpp>
#include <engine/nodegraph/Graph.hpp>
#include <engine/nodegraph/Preview.hpp>
#include <engine/nodegraph/Registry.hpp>

#include <functional>
#include <string>

namespace engine::nodegraph {

	// What an inspector handler is handed.
	//
	// **Read only, and it holds no ImGui type.** A handler draws with ImGui, but
	// what it is *given* is the model plus a way to make a texture, which is
	// what lets this declaration live beside the rest of the library.
	struct Inspection {
		// The node being inspected, and its type. Both borrowed, and both null
		// only if a handler is called with nothing selected.
		//@{
		const engine::nodegraph::Node *Node = nullptr;
		const engine::nodegraph::NodeType *Type = nullptr;
		//@}

		// The graph it is in, so a handler can follow a link to see what feeds
		// it.
		const engine::nodegraph::Graph *Graph = nullptr;

		// What has been computed, so a handler can show the node's actual
		// result rather than only its settings.
		const engine::nodegraph::Evaluator *Runner = nullptr;

		// Where a picture comes from, on the same terms as `Canvas::Images`.
		ImageSink Images;

		// Where a picture that changes while somebody drags comes from.
		//
		// **Separate from `Images` because it is not content-addressed.**
		// `Images` holds every result for ever under the hash it was computed
		// at, which is right for a thumbnail and ruinous for a view re-rendered
		// sixty times a second: one second of orbiting would evict every node's
		// picture. A host answering this is expected to keep *one* texture and
		// replace what is in it.
		ImageSink Orbit;
	};

	// Draws one node's visualisation. Everything else in the panel — the title,
	// the parameters, the ports — is the same for every type and is drawn around
	// this.
	using InspectorFn = std::function<void(const Inspection &)>;

	// Every registered inspector handler.
	//
	// **A registry rather than a switch**, for the reason the node types are
	// one: a type added by anybody gets a panel with nothing here changing, and
	// a type that wants its own says so by name.
	class Inspectors {
	  public:
		// Adds a handler, or replaces the one already under its id.
		//
		// @param id   What `NodeType::Inspector` names it by.
		// @param draw The handler.
		static void Register(const std::string &id, InspectorFn draw);

		// The handler under an id.
		//
		// @param id The id to look up.
		// @return It, or null when nothing registered that id — which is what a
		//         node type naming a handler this build does not have produces.
		static const InspectorFn *Find(const std::string &id);

		// Which handler a node gets: what its type asked for, and otherwise one
		// inferred from what the node actually produced.
		//
		// **Inferred from the result and not from the declaration**, so a node
		// that has not run yet gets the "nothing yet" panel rather than an empty
		// picture frame, and a node whose output turned out to be a field gets
		// the field panel even though its type never said so.
		static const InspectorFn *For(const Inspection &what);
	};

	// The four that come with the library: a field or image preview with the
	// node's inputs beside it, an async run's stages, a plain value readout, and
	// the honest empty one.
	//
	// Registered on first use, for `RegisterDemoNodes`' reason.
	void RegisterInspectors();
}
