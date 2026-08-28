#pragma once

// A render graph as a list of edits, so it can be saved, scripted and undone.
//
// **The same decision as `bake::GraphDocument`, one module over**, and
// deliberately the same shape: an ordered record of edits, replayed to build,
// written to save, truncated to undo. A Render Pipeline widget and an Assets
// Pipeline widget are asked for together, and two editors that agreed about
// nothing would be two sets of bugs - so the operation record, the position
// based references, the text format and the escaping rules are held in common.
// What differs is the vocabulary, because a bake chain and a frame are not the
// same kind of graph.
//
// ## Why the vocabularies differ
//
// `bake::Graph` wires an output into an input, so its document has `connect`.
// `graph::RenderGraph` has no wires: a node names the resources it reads and
// writes, and the order it runs in is the order it was declared in. So this
// document has `resource`, `reads` and `writes` instead - which is the same
// information, stated the way the runtime states it.
//
// **`enable` is here because switching a pass off is an edit like any other.**
// `RenderGraph::SetEnabled` was written as an operation rather than a field for
// exactly this: a pass somebody turned off has to survive a save, and a node
// nobody demanded is a different thing from one somebody disabled.
//
// ## The UI is a consumer of this and not its owner
//
// Everything a Render Pipeline panel does is `Record` with a different
// `Operation`, so a script and a panel reach the same surface and a pipeline
// built either way saves identically. The rule is *"the API comes first and
// the UI is a consumer of it"*, and it is why this file exists before any
// widget does.
//
// @tier L9 · shared

#include <engine/graph/RenderGraph.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine::graph {

	// Which edit an `Operation` is.
	//
	// @since v0.11
	enum class EditKind : uint8_t {
		// Declares a resource.
		AddResource,

		// Declares a node.
		AddNode,

		// Adds a resource to a node's reads.
		Reads,

		// Adds a resource to a node's writes.
		Writes,

		// Turns a node on or off.
		Enable,

		// Configures the node being built.
		//
		// **Applies to the node above it**, exactly as `reads` and `writes` do,
		// so a node and everything about it is one contiguous run of lines. See
		// `Node::Parameters` for why a parameter is text.
		Set,

		// Places a node on an editor's canvas.
		//
		// **Editor metadata, and `Build` ignores it.** Where a box sits changes
		// how a pipeline reads and never what it computes, so moving one must
		// not be able to produce a different frame - the same exclusion
		// `rl-pipeline`'s `GraphSpec.content_hash` makes, and for the same
		// reason: a cosmetic drag that invalidated a compile would make the
		// editor unusable for the thing it is for.
		//
		// **An edit rather than a field on `AddNode`**, because a node is placed
		// far more often than it is added and a document is a list of what
		// somebody did. `PositionsOf` replays them; the last one for a name
		// wins.
		Move,
	};

	// A stable, human-readable name for an edit kind.
	//
	// **The same spelling the text format uses**, so a diagnostic and a saved
	// document never disagree about what something is called.
	//
	// @param kind The kind to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(EditKind kind);

	// One edit.
	//
	// **Every field rather than a variant**, matching `bake::Operation` and
	// `graph::Payload` for the reason both give: a plain struct copies, compares
	// and serialises without a visitor.
	//
	// @since v0.11
	struct Edit {
		// Which edit this is. Decides which fields below are read.
		EditKind Kind = EditKind::AddNode;

		// A resource's or a node's name.
		//
		// **A name and not an index, which is rule 4.** A document is exactly
		// the sort of thing a game file or an editor carries, and an index means
		// a different thing the moment a list is reordered.
		core::Name Name{};

		// `AddNode`'s kind - what an executor switches on. Apart from `Name`
		// because two shadow passes for two lights are one kind and two names.
		core::Name NodeKind{};

		// `AddResource`'s kind.
		ResourceKind Resource = ResourceKind::Colour;

		// `AddResource`'s pixel format.
		ResourceFormat Format = ResourceFormat::RGBA8;

		// `AddResource`'s size, or zero to follow the view.
		//@{
		uint32_t Width = 0;
		uint32_t Height = 0;
		//@}

		// `AddResource`'s fraction of the view, when the size is not absolute.
		uint32_t Divisor = 1;

		// Whether an `AddResource` survives this graph's transient lifetime.
		bool External = false;

		// `Reads` and `Writes`: which resource, by name.
		core::Name Target{};

		// `AddNode`'s partition, and the field the whole version turns on.
		NodeScope Scope = NodeScope::View;

		// `AddNode`'s skippability.
		bool Optional = false;

		// `Enable`'s new state.
		bool Enabled = true;

		// For `Set`: which parameter, and what to.
		//@{
		core::Name Key{};
		std::string Value{};
		//@}

		// `Move`'s destination, in canvas pixels.
		//
		// **Pixels and not columns.** `PipelineView` places a node by which
		// band and column it runs in, which is the right answer for a diagram
		// that draws itself; an editor is one somebody drags boxes around, and
		// the two coexist - `LayoutPipeline` is what an "arrange" button calls
		// to produce these.
		//@{
		float X = 0.0f;
		float Y = 0.0f;
		//@}
	};

	// Why a document would not build or parse.
	//
	// @since v0.11
	enum class PipelineDocumentStatus : uint8_t {
		// It built, or it parsed.
		Ok,

		// An edit names a node or resource this document has not declared.
		UnknownName,

		// The graph refused the edit - a full graph, a duplicate resource name,
		// or an unnamed one.
		Refused,

		// Text that is not a document.
		Malformed,

		// The built graph does not compile. **Reported by `Build` rather than
		// left to the caller**, because a document that produces an
		// uncompilable graph is a broken save file and saying so at load is the
		// difference between a diagnostic and a black screen.
		Invalid,
	};

	// A stable, human-readable name for a status.
	//
	// @param status The status to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(PipelineDocumentStatus status);

	// An ordered list of edits.
	//
	// @since v0.11
	class PipelineDocument {
	  public:
		// Appends an edit.
		//
		// **Nothing is validated here**, matching `bake::Document::Record`: an
		// editor lets somebody declare a read and then rename the resource, and
		// a record that refused mid-edit would be a panel nobody could use in
		// the order people work in. `Build` is where a document can be wrong.
		//
		// @param edit The edit. Copied.
		void Record(Edit edit);

		// Drops the last edit.
		//
		// @return `false` when there was nothing to drop.
		bool Undo();

		// Every edit, in order.
		//
		// @return A view valid until the next `Record`, `Undo` or `Clear`.
		std::span<const Edit> Edits() const {
			return Records;
		}

		// How many edits there are.
		size_t Count() const {
			return Records.size();
		}

		// Forgets everything.
		void Clear();

	  private:
		std::vector<Edit> Records;
	};

	// Replays a document into a graph.
	//
	// **Into an empty graph**, for `bake::Build`'s reason: replaying onto one
	// that already holds nodes would make a name resolve to something the
	// document did not declare.
	//
	// @param document The edits.
	// @param graph    Filled in. Must be empty.
	// @param offender Set to the name or position of what failed. Untouched on
	//                 success.
	// @return `Ok`, or why not.
	PipelineDocumentStatus Build(const PipelineDocument &document, RenderGraph &graph, core::Name &offender);

	// Writes a document as text.
	//
	// **The same line-oriented shape `bake::Write` produces**, one edit per
	// line, so a saved pipeline reviews as a list of what somebody did and the
	// two editors' files look like each other.
	//
	// @param document The edits.
	// @return The text, ending in a newline.
	std::string Write(const PipelineDocument &document);

	// Reads a document back.
	//
	// **Round trips exactly**: `Read(Write(d))` is `d` for every document,
	// including the ones that would not build.
	//
	// @param text     What `Write` produced.
	// @param document Filled in. Cleared first.
	// @param offender Set to the offending line's text, interned. Untouched on
	//                 success.
	// @return `Ok`, or why not.
	PipelineDocumentStatus Read(std::string_view text, PipelineDocument &document, core::Name &offender);

	// Several named pipelines, which is what one editor holds.
	//
	// **The requirement: many node trees in one editor, and render "as
	// different subpipelines".** A world does not have *a* pipeline any more
	// than it has *a* script: a frame may be described by a main pass chain, a
	// cheaper one for a reflection, and a debug one somebody switches to. This
	// is the container for that, and it is what a save file carries.
	//
	// **Names are sorted on the way out.** A set written twice with the same
	// contents produces byte-identical text whatever order the editor happened
	// to add things in, which is what makes a save file diffable and a
	// round-trip test meaningful.
	//
	// @since v0.11
	class PipelineSet {
	  public:
		// Adds a pipeline, or replaces one of that name.
		//
		// **Replaces rather than refuses**, unlike `RenderGraph::AddResource`.
		// A duplicate resource inside one graph is an authoring mistake with no
		// sensible reading; saving over a pipeline is what "save" means.
		//
		// @param name     What it is called. An invalid name is refused.
		// @param document The pipeline. Copied.
		// @return `false` for an unnamed pipeline.
		bool Set(core::Name name, PipelineDocument document);

		// One pipeline.
		//
		// @param name Which.
		// @return The document, or null when there is no such name.
		const PipelineDocument *Find(core::Name name) const;

		// Drops one.
		//
		// @param name Which.
		// @return `false` when there was no such name.
		bool Remove(core::Name name);

		// Every name it holds, sorted.
		//
		// @return The names. Valid until the next `Set` or `Remove`.
		std::span<const core::Name> Names() const {
			return Order;
		}

		// How many pipelines it holds.
		size_t Count() const {
			return Order.size();
		}

		// Forgets everything.
		void Clear();

	  private:
		// Parallel to `Order` and kept in step by every mutator, so `Names` can
		// hand back a span rather than building one per call.
		std::vector<core::Name> Order;
		std::vector<PipelineDocument> Documents;
	};

	// Writes a set as text.
	//
	// **One `pipeline` line then that pipeline's edits**, repeated, so the
	// single-document format is a strict substring of this one and a reader of
	// either can be understood from the other.
	//
	// @param set The pipelines.
	// @return The text, ending in a newline.
	std::string Write(const PipelineSet &set);

	// Reads a set back.
	//
	// **Edits before the first `pipeline` line are refused**, because a set is
	// not a document with extra parts - an edit belonging to no named pipeline
	// is a file somebody hand-edited into a shape this cannot represent.
	//
	// @param text     What `Write` produced.
	// @param set      Filled in. Cleared first.
	// @param offender Set to the offending line, interned. Untouched on success.
	// @return `Ok`, or why not.
	PipelineDocumentStatus Read(std::string_view text, PipelineSet &set, core::Name &offender);

	// Registers `PipelineSet` under a stable name.
	//
	// **Called before a world's pipelines are read or written**, and idempotent.
	// A resource is keyed by a component id, so one that is never registered
	// here would be minted by the first `SetResource` under the compiler's
	// spelling of the type - a world that saves, loads, and quietly has no
	// pipelines because the two spellings never met.
	//
	// Here rather than in `game` because a module names its own types. Naming
	// another module's is exactly the mistake `scene::RegisterSceneComponents`
	// exists to stop `physics` making.
	void RegisterPipelineComponents();

	// Where a document's nodes have been dragged to.
	//
	// **Replayed rather than stored**, because a document is the record and a
	// position is one more thing somebody did to it. The last `Move` for a name
	// wins, so undoing a drag is `PipelineDocument::Undo` with no special case.
	//
	// A node that has never been moved is absent rather than at the origin: a
	// caller that stacked every unplaced node on top of itself would be worse
	// than one that arranged them, and only the caller knows which it wants.
	//
	// @param document The edits.
	// @return Name to position, for every node a `Move` names - including names
	//         the document never declared, which is what makes a position
	//         survive deleting and re-adding a node under the same name.
	std::unordered_map<uint32_t, std::pair<float, float>> PositionsOf(const PipelineDocument &document);

	// The default physically based frame authored for the node editor.
	//
	// This is the engine's complete runnable graph document. G-buffer material
	// data and emissive are produced once,
	// ambient occlusion is derived from depth and normals, deferred lighting
	// consumes all three, and tone mapping produces the display image.
	//
	// @return The PBR document. `Build`ing it produces a graph that compiles.
	PipelineDocument DefaultPbrDocument();
}
