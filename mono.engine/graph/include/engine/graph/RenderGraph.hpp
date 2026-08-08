#pragma once

// A frame as nodes and resources, compiled to an order and then run.
//
// **This replaced `graph::Pipeline`, which is deleted.** That type was a flat
// list of stage records checked for one property. This holds resources as well
// as nodes, partitions them into what runs once a frame and what runs once a
// view, and hands the result to something that owns a device. Everything
// `Pipeline` did is here, so keeping it would have been a second description of
// the same frame — see below.
//
// **`DEFERRED.md` D00016 is closed by that deletion rather than by a check.**
// The frame used to be described twice, in `render::PassOrder` and in
// `graph::StandardPipeline`, with a test keeping the copies in step. A test
// that compares two descriptions can say they disagree; it cannot say which is
// right, and it only runs after somebody has already edited one. `PassOrder`
// now reads its names out of `StandardGraph`, so there is one description and a
// view of it. D00002 — running the frame from the description rather than
// merely agreeing with it — is the part still open.
//
// ## Declaration order is the order, and the resources check it
//
// **A node names what it reads and writes and never names another node**, and
// the order it runs in is the order it was declared in. The reads and writes are
// a *check* on that order rather than a derivation of it.
//
// **Deriving it was tried first and a read-modify-write chain kills it.**
// `opaque` writes colour, `transparent` draws onto it, `overlay` onto that,
// `interface` onto that — every one of them both reads and writes the same
// resource, so a producer-to-consumer graph over the four is a four-node cycle.
// The first version of this compiled the standard frame into exactly that and
// refused it. And the cycle is not a tangle to be undone: which of overlay and
// interface goes on top is *authored*, and no dependency analysis recovers it.
//
// So this is `Pipeline.hpp`'s refusal restated rather than reversed — a general
// dependency-resolving frame graph "earns its complexity at twenty passes and
// costs more than it returns at four". What the resources buy is that an author
// who puts a pass above the thing it samples is told which pass, at compile,
// rather than seeing a frame lit by whatever was in that memory.
//
// ## Per view, and it is the point of the version
//
// `Node::PerView` is `Stage::PerView` and it finally has a reader. A shadow map
// is per light and shareable by every view of a world; a colour pass is per view
// and is not. `Compile` partitions on it, so four split-screen views of one world
// pay for **one** shadow pass and four colour passes — which is what "handle
// multiple worlds in parallel" resolves to once it stops being a slogan.
//
// ## No device, and that is what makes it testable
//
// This module is L9 `shared` and links no GPU. Execution calls back into a
// `NodeRunner` the caller supplies, so `render` implements one over SDL and a
// test implements one that records what it was asked to do. That is
// `bake::Graph`'s arrangement — it touches no filesystem for the same reason —
// and it is why the whole of this is exercised on a machine with no GPU.
//
// @tier L9 · shared

#include <engine/core/Name.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::graph {

	// What a resource is, so an executor knows what to make.
	//
	// @since v0.11
	enum class ResourceKind : uint8_t {
		// A colour attachment.
		Colour,

		// A depth attachment.
		Depth,

		// A texture a pass samples rather than writes.
		Texture,
	};

	// One thing a node reads or writes.
	//
	// @since v0.11
	struct ResourceDesc {
		// What it is called — `colour`, `depth`, `shadow`.
		//
		// **A name and not an index**, which is rule 4 and the same reason
		// `Attachment` carries one: a graph is the sort of thing a game file or
		// an editor will carry, and an index means a different resource the
		// moment the list is reordered.
		core::Name Name;

		// What kind of thing it is.
		ResourceKind Kind = ResourceKind::Colour;

		// Its size, or zero to mean "whatever the view is".
		//
		// **Zero rather than a separate flag**, because a resource that follows
		// the view is the ordinary case and a flag would be a second field to
		// keep in step with the numbers beside it.
		//@{
		uint32_t Width = 0;
		uint32_t Height = 0;
		//@}
	};

	// A handle to a resource in one graph.
	//
	// @since v0.11
	struct ResourceId {
		// The value meaning "no resource". Zero is never issued.
		static constexpr uint32_t NONE = 0;

		// The handle itself.
		uint32_t Value = NONE;

		// Whether this names a resource.
		//
		// @return `false` for a default handle.
		constexpr bool IsValid() const {
			return Value != NONE;
		}

		// @param other The handle to compare.
		// @return `true` when both name the same resource.
		constexpr bool operator==(const ResourceId &other) const = default;
	};

	// A handle to a node in one graph.
	//
	// @since v0.11
	struct NodeId {
		// The value meaning "no node". Zero is never issued.
		static constexpr uint32_t NONE = 0;

		// The handle itself.
		uint32_t Value = NONE;

		// Whether this names a node.
		//
		// @return `false` for a default handle, which is what `AddNode` returns
		//         when it refuses.
		constexpr bool IsValid() const {
			return Value != NONE;
		}

		// @param other The handle to compare.
		// @return `true` when both name the same node.
		constexpr bool operator==(const NodeId &other) const = default;
	};

	// One pass of a frame.
	//
	// @since v0.11
	struct Node {
		// What this node is called, for a profiler and for a diagnostic.
		core::Name Name;

		// What it does, which is what an executor switches on.
		//
		// **Apart from `Name` on purpose.** Two shadow passes for two lights are
		// one kind and two names, and an executor that switched on the name
		// would need one case per instance of a pass rather than one per sort of
		// pass.
		core::Name Kind;

		// What it must be able to read.
		std::vector<ResourceId> Reads;

		// What it writes.
		std::vector<ResourceId> Writes;

		// Whether the work is per view.
		//
		// **The flag that decides how many worlds cost**, and the reason this
		// type exists rather than a longer function. False means the node runs
		// once for the whole frame however many views there are.
		bool PerView = true;

		// Whether it may be skipped when there is nothing for it to do.
		bool Optional = false;

		// Whether it runs at all.
		//
		// **Distinct from being dead, and an editor must keep them distinct.**
		// A node nobody demanded is dead; a node somebody switched off is
		// disabled, and a black screen whose cause is one presented as the other
		// is unexplainable.
		bool Enabled = true;
	};

	// Why a graph is not runnable.
	//
	// @since v0.11
	enum class GraphStatus : uint8_t {
		// It is runnable.
		Ok,

		// Two nodes share a name, so a profiler cannot tell them apart.
		DuplicateNode,

		// A node writes nothing, which is a node that cannot be observed.
		WritesNothing,

		// A node names a resource this graph does not hold.
		UnknownResource,

		// A node reads something nothing earlier wrote.
		//
		// **Earlier is the whole of it.** Declaration order is the execution
		// order, so this covers a producer that was deleted, one that was never
		// added, and one that is simply below the node reading it — which are
		// three causes with one symptom and one fix.
		ReadsBeforeWrite,

		// Past `MAXIMUM_NODES`.
		TooManyNodes,

		// A shared node sits between two per-view nodes.
		//
		// **The one arrangement a three-block frame cannot express.** Shared
		// work runs either before every view or after every one of them; a
		// shared node declared in the middle would mean "every view's pass A,
		// then this once, then every view's pass B", which is four blocks and
		// then six and then however many the author writes. Refused rather than
		// silently hoisted to one end, because either end changes what it reads.
		SharedBetweenViews,

		// A per-view node writes something a shared node also writes.
		//
		// **The one failure this partition can produce and nothing else can
		// catch.** A shared node runs once and a per-view node runs per view, so
		// both writing one resource means the last view wins and the shared
		// node's work is silently discarded — which looks like the shared pass
		// not running at all.
		SharedWriteConflict,
	};

	// A stable, human-readable name for a status.
	//
	// @param status The status to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(GraphStatus status);

	// A graph compiled to an order.
	//
	// **Three lists rather than one with a flag**, so an executor loops rather
	// than branches. Holding one list and testing a bool per node per view would
	// put the partition decision inside the hot loop, where it is re-answered
	// every frame for an answer that changes only when the graph is edited.
	//
	// The frame is `Shared`, then `PerView` once for each view, then `Final`.
	//
	// **Why there is a third list and not two.** A real frame has shared work at
	// both ends. A shadow map is shared and must be written *before* any view
	// samples it; an overlay and an editor's chrome are the window's rather than
	// a view's and must be drawn *after* every view has finished with it. Two
	// lists can only put shared work at one end, and putting the overlay first
	// would draw it under the world.
	//
	// **Which end a shared node lands at is its declaration position**, not a
	// second flag. Declaration order is already the execution order, so a shared
	// node before the first per-view node runs before and one after the last
	// runs after — and nothing has to be kept in step with anything.
	//
	// @since v0.11
	struct CompiledGraph {
		// The nodes that run once, before any view.
		std::vector<NodeId> Shared;

		// The nodes that run once per view, in order.
		std::vector<NodeId> PerView;

		// The nodes that run once, after every view.
		std::vector<NodeId> Final;
	};

	// What an executor is told about the node it is running.
	//
	// @since v0.11
	struct RunContext {
		// Which node.
		NodeId Node;

		// Its name and kind, so a runner needs no second lookup.
		//@{
		core::Name Name;
		core::Name Kind;
		//@}

		// Which view this is for, or `WHOLE_FRAME` for a shared node.
		size_t View = 0;

		// Which world this is for, as a position among the frame's distinct
		// worlds — not the caller's identifier.
		//
		// **The field that makes "shared" mean what it says.** A shadow map is
		// one per world per light, so a frame showing two worlds needs two of
		// them; a shared node therefore runs once per distinct world rather than
		// once per frame, and this is how a runner tells which it is being asked
		// for. `WHOLE_FRAME` on a node in `Final`, which belongs to the window
		// rather than to any world.
		size_t World = 0;

		// The value `View` takes for a node that is not per view.
		static constexpr size_t WHOLE_FRAME = static_cast<size_t>(-1);

		// What it reads and writes, as the graph holds them.
		//@{
		std::span<const ResourceId> Reads;
		std::span<const ResourceId> Writes;
		//@}
	};

	// What runs a node.
	//
	// **The seam that keeps a device out of L9.** `render` implements this over
	// SDL; a test implements one that records what it was asked to do, which is
	// how every case in `graph/tests/RenderGraph.cpp` runs on a machine with no
	// GPU.
	//
	// @since v0.11
	class NodeRunner {
	  public:
		virtual ~NodeRunner() = default;

		// Runs one node.
		//
		// @param context What to run and for which view.
		// @return `false` to abandon the frame. An executor stops rather than
		//         carrying on, because a pass that failed leaves its writes
		//         undefined and everything downstream reads them.
		virtual bool Run(const RunContext &context) = 0;
	};

	// A frame, as nodes over resources.
	//
	// @since v0.11
	class RenderGraph {
	  public:
		// The most nodes one graph may hold.
		//
		// A bound so an editor cannot make a graph that takes unbounded time to
		// compile. `bake::Graph` takes the same shape for the same reason; this
		// is smaller because a frame with a thousand passes is a mistake rather
		// than a large scene.
		static constexpr uint32_t MAXIMUM_NODES = 256;

		// Declares a resource.
		//
		// @param desc What it is.
		// @return The handle, or an invalid one for an unnamed resource or a
		//         duplicate name.
		ResourceId AddResource(const ResourceDesc &desc);

		// Adds a node.
		//
		// @param node The node. Copied.
		// @return The handle, or an invalid one when the graph is full or the
		//         node is unnamed.
		NodeId AddNode(Node node);

		// Turns a node on or off.
		//
		// **An operation rather than a field somebody reaches into**, because
		// this is the first of `Editor`'s mutations and every one of them has to
		// be expressible as data for a graph to be saved or scripted.
		//
		// @param node The node.
		// @param enabled Whether it runs.
		// @return `false` for an unknown node.
		bool SetEnabled(NodeId node, bool enabled);

		// Checks everything that can be checked without running.
		//
		// @param offender Filled in with the node or resource that failed.
		// @return `Ok`, or why not.
		GraphStatus Validate(core::Name &offender) const;

		// Works out the order.
		//
		// **Disabled nodes are left out of the compile rather than skipped at
		// run time**, so the cost of switching a pass off is zero per frame
		// rather than a branch per node per view.
		//
		// @param out Filled in on success, left alone otherwise.
		// @param offender Filled in with what failed.
		// @return `Ok`, or why not. Validates first, so a caller need not.
		GraphStatus Compile(CompiledGraph &out, core::Name &offender) const;

		// Runs a compiled order.
		//
		// @param compiled What `Compile` produced.
		// @param runner What actually does the work.
		// @param views How many views to run the per-view nodes for. Zero runs
		//              the shared nodes once and the final nodes, which is what
		//              a headless host presenting no view does.
		// @return `false` when the runner abandoned the frame.
		bool Execute(const CompiledGraph &compiled, NodeRunner &runner, size_t views) const;

		// Runs a compiled order over views that may belong to different worlds.
		//
		// **The shared block runs once per distinct world, not once per frame**,
		// which is what `Node::PerView = false` has always claimed and could not
		// deliver while a frame had only one world in it. A shadow map is per
		// world per light: two views of one world share one, and two views of
		// two worlds need two, fitted to two sets of bounds. Running one for the
		// frame would light one world's geometry with the other's sun.
		//
		// **Worlds are identified by value and grouped in first-appearance
		// order**, so a caller passes whatever it already uses to tell worlds
		// apart and this never learns what a world is. Views of one world do not
		// have to be adjacent.
		//
		// `Final` runs once at the end however many worlds there were, because
		// the overlay and the editor's chrome are the window's.
		//
		// @param compiled What `Compile` produced.
		// @param runner   What actually does the work.
		// @param worlds   One entry per view, naming that view's world. Its size
		//                 is the view count.
		// @return `false` when the runner abandoned the frame.
		bool
		Execute(const CompiledGraph &compiled, NodeRunner &runner, std::span<const uint64_t> worlds) const;

		// A node, or null.
		//
		// @param node The handle.
		// @return The node, or null for an unknown handle.
		const Node *Find(NodeId node) const;

		// A resource, or null.
		//
		// @param resource The handle.
		// @return The descriptor, or null for an unknown handle.
		const ResourceDesc *FindResource(ResourceId resource) const;

		// How many nodes the graph holds, enabled or not.
		size_t Count() const {
			return Nodes.size();
		}

		// How many resources it declares.
		size_t ResourceCount() const {
			return Resources.size();
		}

	  private:
		std::vector<Node> Nodes;
		std::vector<ResourceDesc> Resources;
	};

	// The frame this engine draws today, as a graph.
	//
	// **The same six passes `render::PassOrder` submits**, in the same order, so
	// the first thing this can be checked against is the renderer it replaces.
	// What it adds is the partition, and the frame has shared work at both ends:
	//
	// - `shadow` is shared and declared first, so it runs once *before* any
	//   view. A shadow map is per light and every view of the world samples the
	//   same one — four split-screen views pay for one.
	// - `surface`, `opaque` and `transparent` are per view.
	// - `overlay` and `interface` are shared and declared last, so they run once
	//   *after* every view. Both draw onto the window rather than into a view's
	//   target: one CPU overlay image and one editor's chrome, however many
	//   worlds the frame shows. Marking them per view would draw the editor's
	//   panels once per viewport, into whichever target that viewport used.
	//
	// @return The graph.
	RenderGraph StandardGraph();
}
