#pragma once

// A bake graph as a list of edits, so it can be saved, scripted and undone.
//
// **The operations are the document.** `bake::Graph` is a runtime — you build
// it, wire it, run it, and what it holds afterwards is payloads. This is the
// other half: an ordered record of the *edits* that produce such a graph.
// Replaying the record builds the graph; writing the record saves it; dropping
// the last entry is undo. One representation, four features, and none of them
// is a second description of the other.
//
// The alternative — a panel that calls `Graph::AddFit` directly and separately
// remembers what it did for the save file — is two descriptions of one pipeline
// kept in step by whoever remembers, which is the failure `DEFERRED.md` D00016
// is about in another module.
//
// ## The UI is a consumer of this and not its owner
//
// v0.11 §4.4 settles the order: *"the API comes first and the UI is a consumer
// of it, which also means a graph can be edited from a script with no UI at
// all."* Everything an Assets Pipeline widget does — add a node, wire two,
// change a number — is `Record` with a different `Operation`, so a script and a
// panel reach the same surface and a graph built either way saves identically.
//
// ## A recipe, not a payload
//
// **A source is a name here and bytes at build time**, which is the one place
// this deliberately differs from `Graph::AddSource`. A document that embedded
// its inputs would be a save file the size of the assets it describes, and it
// would go stale the moment the file on disk changed. So `Build` takes a
// `SourceResolver` and asks it for each name.
//
// That also keeps this module's rule intact: `bake` touches no filesystem, so
// all of it is exercised headlessly. The resolver is where a caller's storage
// enters, and a test supplies one backed by a map.
//
//
// ## Why this is not in `bake`
//
// **So that `Engine::game` can read a pipeline out of a save file without
// linking a JPEG decoder.** `bake` carries the PNG, JPEG, GIF, BMP, OBJ, glTF
// and PMX readers, and its own build file states that nothing a shipped game
// links may link it — a save format that named `bake` to parse *text* would put
// every one of those decoders into `server`, which has no reason to decode a
// JPEG. `D00102` weighed three ways out and settled on the split.
//
// So the document lives here and the runtime stays there. `Build` is the one
// function that needs both, and it is in `bake` for that reason —
// `bake/GraphDocument.hpp`.
//
// @tier L9 · shared

#include <engine/bakegraph/Nodes.hpp>
#include <engine/core/types/Vector3.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::bake {

	// Which edit an `Operation` is.
	//
	// **One per way of adding a node, rather than one `AddNode` carrying every
	// parameter.** `Graph` already draws these lines — `AddFit` takes a size and
	// `AddResize` takes two integers — and collapsing them here would mean a
	// reader could not tell which fields of an operation are meaningful.
	//
	// @since v0.11
	enum class OperationKind : uint8_t {
		// An input node naming bytes a resolver supplies.
		AddSource,

		// An input node naming a built-in mesh.
		AddBuiltin,

		// A node whose kind carries no parameters — `Import`, `Smooth`,
		// `Opaque`.
		AddNode,

		// A `Fit` node.
		AddFit,

		// A `Scale` node.
		AddScale,

		// A `Resize` node.
		AddResize,

		// A `Retime` node.
		AddRetime,

		// A `Write` node.
		AddWrite,

		// A wire from one node's output to another's input.
		Connect,
	};

	// A stable, human-readable name for an operation kind.
	//
	// **The same spelling the text format uses**, so a diagnostic and a saved
	// document never disagree about what something is called.
	//
	// @param kind The kind to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(OperationKind kind);

	// One edit.
	//
	// **Every field rather than a variant**, which is `Payload`'s arrangement in
	// this module and is here for the same reason: a plain struct copies,
	// compares and serialises without a visitor, and the fields a kind does not
	// use cost eight bytes rather than a type hierarchy.
	//
	// @since v0.11
	struct Operation {
		// Which edit this is. Decides which fields below are read.
		OperationKind Kind = OperationKind::AddNode;

		// For `AddNode`, which node. Ignored otherwise.
		//
		// `Source`, `Builtin`, `Fit`, `Scale`, `Resize`, `Retime` and `Write`
		// are refused here — each has its own operation kind, because each
		// carries a parameter this field cannot.
		NodeKind Node = NodeKind::Import;

		// A source's name, a built-in's name, or a written asset's name.
		std::string Text;

		// `AddScale`'s per-axis multiplier.
		core::Vector3 Amount{1.0f, 1.0f, 1.0f};

		// `AddFit`'s target size in metres, or `AddRetime`'s frames a second.
		float Number = 0.0f;

		// `AddResize`'s target, in pixels.
		//@{
		uint32_t Width = 0;
		uint32_t Height = 0;
		//@}

		// `Connect`'s endpoints, as one-based positions among the *node*
		// operations of this document.
		//
		// **A position in the document and not a `NodeId`**, and the difference
		// is what makes a document portable. A `NodeId` is whatever the runtime
		// happened to issue, so a saved graph carrying one would only reload
		// into a `Graph` that had been built in exactly the same order by
		// exactly the same code. Counting node operations is a property of the
		// document itself.
		//@{
		uint32_t From = 0;
		uint32_t To = 0;
		//@}
	};

	// Why a document would not build or parse.
	//
	// @since v0.11
	enum class DocumentStatus : uint8_t {
		// It built, or it parsed.
		Ok,

		// An operation names a node position this document does not hold.
		UnknownNode,

		// `AddNode` was given a kind that carries a parameter, or `Source` or
		// `Builtin`, which have their own operations.
		WrongNodeKind,

		// The graph refused the edit — a full graph, an unknown built-in, a
		// second input on one node, or a wire that would close a cycle.
		//
		// **Not split further on purpose.** `Graph::Connect` documents four
		// distinct refusals behind one `false`, and inventing separate statuses
		// here would be this file claiming to know which one happened.
		Refused,

		// Text that is not a document: a bad header, an unknown operation, or a
		// number that would not parse.
		Malformed,

		// Past `Graph::MAXIMUM_NODES`, checked before anything is built.
		TooManyOperations,
	};

	// A stable, human-readable name for a status.
	//
	// @param status The status to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(DocumentStatus status);

	// Where a source's bytes come from at build time.
	//
	// **The seam that keeps a filesystem out of L9.** A studio hands one backed
	// by the content store; a test hands one backed by a map. Returning an empty
	// span is how a resolver says it has never heard of a name, and `Build`
	// reports that as `Refused` against the source's own name rather than
	// baking nothing and succeeding.
	//
	// @since v0.11
	using SourceResolver = std::function<std::span<const std::byte>(std::string_view)>;

	// An ordered list of edits.
	//
	// @since v0.11
	class Document {
	  public:
		// Appends an edit.
		//
		// **Nothing is validated here, and that is deliberate.** An editor lets
		// somebody wire two nodes and then delete one; a record that refused
		// halfway through an edit would be a panel that could not be used in the
		// order people work in. `Build` is where a document meets a `Graph` and
		// where it can be wrong.
		//
		// @param operation The edit. Copied.
		void Record(Operation operation);

		// Drops the last edit.
		//
		// @return `false` when there was nothing to drop.
		bool Undo();

		// Every edit, in order.
		//
		// @return A view valid until the next `Record`, `Undo` or `Clear`.
		std::span<const Operation> Operations() const {
			return Edits;
		}

		// How many edits there are.
		size_t Count() const {
			return Edits.size();
		}

		// How many of them add a node.
		//
		// **What `Operation::From` and `To` are counted against**, so an editor
		// wiring the node it just added names `NodeCount()` without tracking a
		// parallel index.
		//
		// @return The number of node operations.
		size_t NodeCount() const;

		// Forgets everything.
		void Clear();

	  private:
		std::vector<Operation> Edits;
	};

	// Writes a document as text.
	//
	// **Line oriented and human readable**, which is the same argument this
	// repository makes for every format a person might have to diff. One
	// operation per line, in order, so a saved pipeline reviews as a list of
	// what somebody did.
	//
	// Names are quoted and the five characters that would break a line —
	// backslash, quote, newline, carriage return and tab — are escaped, so a
	// name is never able to forge an operation.
	//
	// @param document The edits.
	// @return The text, ending in a newline.
	std::string Write(const Document &document);

	// Reads a document back.
	//
	// **Round trips exactly**, which is the property the tests are built on:
	// `Read(Write(d))` is `d` for every document, including the ones that would
	// not build.
	//
	// @param text     What `Write` produced.
	// @param document Filled in. Cleared first.
	// @param offender Set to the offending line's text. Untouched on success.
	// @return `Ok`, or why not.
	DocumentStatus Read(std::string_view text, Document &document, std::string &offender);
}
