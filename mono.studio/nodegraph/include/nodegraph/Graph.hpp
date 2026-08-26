#pragma once

// The model. No drawing, no ImGui, no evaluation: what is there and what is
// wired to what.
//
// Everything an editor needs to be *correct* lives here and is testable with no
// window: link validation, the cycle guard, topological order and the content
// hash the cache is keyed on. Those are the parts that fail **silently**: a
// cycle is a hang, a link validator that lets a type mismatch through is a crash
// inside somebody's evaluator, and a hash that never settles is a graph that
// recomputes for ever.
//
// **A compressed node is a view, not a container.** Folding a selection sets
// `Node::Owner` on its members and derives `Node::Proxies` from the links that
// already crossed the boundary. Nothing is moved and no link is re-pointed, so
// `Ordered`, `Reaches`, `Hash` and the evaluator are all unchanged and none of
// them can be wrong about compression.
//
// **Depth, framing and collapsing are not parameters.** None of `Node::Owner`,
// `Node::Collapsed` or `Graph::Groups` reaches `Hash`, because tidying a graph
// must not recompute it. The JavaScript template's notes record the cost of
// getting this wrong the other way: it hashed evaluation status by accident and
// the cache never settled.

#include <cstdint>
#include <nodegraph/Registry.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace nodegraph {

	// What a node is called inside one graph. Minted by `Graph`, saved, and
	// remapped by `Absorb` when one graph is copied into another.
	using NodeId = uint32_t;

	// The absent node. Zero, so a default-constructed id is already it.
	inline constexpr NodeId NO_NODE = 0;

	// One port a compressed node shows, and the inner port it stands for.
	//
	// **It names the inner port rather than replacing it.** Compression does not
	// re-point a single link: the wires stay between the nodes that made them,
	// and this says where a wire that visually ends on the compressed node
	// actually ends. That is what keeps the evaluator, the cycle guard, the
	// content hash and the save format entirely unaware that compression exists.
	struct Proxy {
		// What the compressed node shows this port as.
		std::string Name;

		// The `DataType::Id` it carries, copied from the inner port so a link
		// can be checked without walking inside.
		std::string Type;

		// Which side it sits on. Outputs are the default because a compressed
		// subtree is usually consumed rather than fed.
		bool Input = false;

		// The node inside that really owns this port, and its port there.
		//@{
		NodeId Inner = NO_NODE;
		std::string InnerPort;
		//@}
	};

	// One knob a compressed node lifted out of a node inside it.
	//
	// **A write goes straight through to the inner node**, so a compressed node
	// is a live view of its contents rather than a copy that drifts from them.
	struct Promotion {
		// `<inner id>/<inner key>`. Unique by construction, and stable across a
		// save because the inner id is.
		std::string Key;

		// What a person reads beside the lifted knob.
		std::string Label;

		// The node inside that really owns this knob, and its key there. A
		// write goes straight to them.
		//@{
		NodeId Inner = NO_NODE;
		std::string InnerKey;
		//@}

		// The inner declaration, re-keyed and re-labelled.
		//
		// **Copied rather than looked up**, so a promoted slider keeps its
		// range, its step and its option list without every reader needing the
		// graph to find the node it came from. Re-derived on load from the inner
		// node's type, so it can never drift from the declaration.
		WidgetSpec Spec;

		// Whether it is shown. A thirty-widget selection compresses into a node
		// that should present the three knobs that matter, and this is how the
		// other twenty-seven are put away without losing them.
		bool Exposed = true;
	};

	// One placed node.
	struct Node {
		// Its name inside this graph. Minted on `Add` and never reused.
		NodeId Id = NO_NODE;

		// A type nobody registered is kept and drawn as broken rather than
		// dropped: a graph saved with something's node type has to survive being
		// opened without it.
		std::string Type;

		// Where it sits on the canvas, top-left, in canvas units.
		//@{
		float X = 0.0f;
		float Y = 0.0f;
		//@}

		// By `WidgetSpec::Key`. A missing key reads as the type's default, so a
		// type that grows a widget does not invalidate every saved graph.
		std::unordered_map<std::string, Value> Widgets;

		// What a person renamed it to, or empty to show the type's title.
		std::string Label;

		// Whether it is drawn as a header and its ports, with the body hidden.
		//
		// **Not in `Graph::Hash`**, because it changes nothing about what the
		// node computes. A collapsed node is a graph somebody has finished
		// reading, and collapsing one must not recompute the branch under it.
		bool Collapsed = false;

		// Which compressed node this one is inside, or `NO_NODE` for the root.
		//
		// **A depth and not a container.** The node stays in the same graph with
		// the same id and the same wires; what changes is which view draws it.
		// A subgraph as a separate `Graph` would make the save format nest, the
		// evaluator recurse and every hash fold in a subgraph signature, which is
		// the trap the reference implementation records falling into.
		NodeId Owner = NO_NODE;

		// A compressed node's derived interface. Empty on every ordinary node,
		// which then reads its type's declaration.
		//@{
		std::vector<nodegraph::Proxy> Proxies;
		std::vector<nodegraph::Promotion> Promoted;
		//@}

		// Whether this node stands for a subtree rather than being one node.
		//
		// @return `true` when it has a derived interface.
		bool Compressed() const {
			return !Proxies.empty() || !Promoted.empty();
		}
	};

	// One connection. Ports are named for the same reason a type id is a string.
	struct Link {
		// Where it starts: a node and one of its output ports.
		//@{
		NodeId From = NO_NODE;
		std::string FromPort;
		//@}

		// Where it ends: a node and one of its input ports. An input takes one
		// link, so this pair is unique across a graph.
		//@{
		NodeId To = NO_NODE;
		std::string ToPort;
		//@}
	};

	// What a frame is called inside one graph.
	using GroupId = uint32_t;

	// The absent group. Zero, for `NO_NODE`'s reason.
	inline constexpr GroupId NO_GROUP = 0;

	// A frame around some nodes, which drags them together.
	//
	// **It holds members and not a rectangle.** A stored rectangle is a second
	// copy of where the nodes are, and it
	// goes wrong the first time one member is moved by anything that forgot to
	// update it. The canvas computes the frame from the members every time it
	// draws, which cannot be stale.
	struct Group {
		// Its name inside this graph.
		GroupId Id = NO_GROUP;

		// What a person reads on the frame's header.
		std::string Title;

		// The frame's colour.
		Colour Tint;

		// The nodes it drags. The frame is computed from these every draw, and
		// a member that has been removed from the graph is skipped rather than
		// keeping the frame open around nothing.
		std::vector<NodeId> Members;
	};

	// Why a link was refused.
	enum class LinkResult : uint8_t { Made, NoSuchPort, TypeMismatch, WouldCycle, SameNode };

	// Why, in words.
	const char *Describe(LinkResult result);

	// A graph of nodes and links.
	class Graph {
	  public:
		// Places a node of a registered type.
		//
		// @param type The type's id.
		// @param x    Where to put it, top-left, in canvas units.
		// @param y    The same, vertically.
		// @return Its new id, or `NO_NODE` when nothing registered that type.
		NodeId Add(const std::string &type, float x, float y);

		// Removes a node and every link touching it.
		//
		// @param id The node.
		// @return `false` when there is no such node.
		bool Remove(NodeId id);

		// Whether a node is still here.
		//
		// @param id The node.
		// @return `true` when it is.
		bool Alive(NodeId id) const;

		// One node.
		//
		// **Valid until the next `Add` or `Remove`**, because the nodes live in
		// a vector: a caller holding one across an edit is holding whatever
		// moved into that slot.
		//
		// @param id The node.
		// @return It, or null when there is no such node.
		//@{
		Node *Find(NodeId id);
		const Node *Find(NodeId id) const;
		//@}

		// **An input takes one link and the newer one wins.** A second link into
		// one input is not a merge. Nothing here knows how to merge two payloads,
		// so it replaces, which is what somebody dragging onto an occupied port
		// meant.
		LinkResult Connect(NodeId from, const std::string &fromPort, NodeId to, const std::string &toPort);

		// The same question, changing nothing. What a drag asks every frame.
		LinkResult
		CanConnect(NodeId from, const std::string &fromPort, NodeId to, const std::string &toPort) const;

		// Removes whatever link ends on an input port.
		//
		// **By the destination, because that is what is unique.** An input takes
		// one link, so naming the far end identifies it; naming the source would
		// not, since one output feeds many.
		//
		// @param to     The node the link ends on.
		// @param toPort Its input port.
		// @return `false` when nothing was connected there.
		bool Disconnect(NodeId to, const std::string &toPort);

		// Everything in the graph, in no particular order.
		//
		// **Handed out whole rather than iterated**, because the canvas draws
		// every node every frame and a callback per node would be a call per
		// node per frame for nothing.
		//@{
		const std::vector<Node> &Nodes() const {
			return Stored;
		}
		std::vector<Node> &Nodes() {
			return Stored;
		}
		const std::vector<Link> &Links() const {
			return Wires;
		}
		//@}

		// Whatever link ends on an input port.
		//
		// @param node The node the link ends on.
		// @param port Its input port.
		// @return The link, or null when nothing is connected there.
		const Link *LinkInto(NodeId node, const std::string &port) const;

		// Every link touching a node, either end.
		std::vector<Link> LinksOf(NodeId node) const;

		// --- groups -----------------------------------------------------------

		// Frames some nodes. A node belongs to at most one group, so a member
		// already in another leaves it: two frames owning one node is two drags
		// moving it twice.
		GroupId Group(std::vector<NodeId> members, std::string title, Colour tint);

		// Removes the frame, and the nodes in it when `withNodes`.
		bool Ungroup(GroupId group, bool withNodes = false);

		// Every frame, in no particular order.
		//@{
		const std::vector<nodegraph::Group> &Groups() const {
			return Frames;
		}
		std::vector<nodegraph::Group> &Groups() {
			return Frames;
		}
		//@}

		// One frame.
		//
		// Valid until the next `Group` or `Ungroup`, for `Find`'s reason.
		//
		// @param group The frame.
		// @return It, or null when there is no such frame.
		//@{
		nodegraph::Group *FindGroup(GroupId group);
		const nodegraph::Group *FindGroup(GroupId group) const;
		//@}

		// Which frame a node is in, or `NO_GROUP`.
		GroupId GroupOf(NodeId node) const;

		// --- compression ------------------------------------------------------

		// Folds a selection into one node whose interface is derived from how it
		// was wired: every inbound target port becomes an input, every outbound
		// source port becomes an output, and every inner knob is promoted.
		//
		// **Nothing is moved and no link is re-pointed.** The members gain an
		// `Owner`, which is what hides them from the outer view; a wire that now
		// appears to end on the compressed node still ends where it always did.
		//
		// @return The node, or `NO_NODE` when the selection cannot be folded:
		//         fewer than two members, or members at different depths.
		NodeId Compress(const std::vector<NodeId> &members, float x, float y);

		// The exact inverse: the members come back to this node's depth and the
		// node is removed. Every wire is already where it should be.
		bool Expand(NodeId node);

		// What is inside a compressed node, in placement order.
		std::vector<NodeId> Contents(NodeId node) const;

		// --- moving pieces between graphs -------------------------------------

		// Takes a node exactly as it is, id and all.
		//
		// **For rebuilding something already valid** (a clipboard, a saved
		// template) and nothing else. It skips every check `Add` makes because
		// what is being rebuilt was checked when it was first made, and because
		// a subtree is adopted in an order where half its references do not
		// exist yet.
		//@{
		NodeId Adopt(const Node &node);
		void Attach(const Link &link);
		//@}

		// Copies another graph in, with fresh ids.
		//
		// **One merge, used by paste and by the custom library.** Both have to
		// remap ids through `Node::Owner`, `Proxy::Inner` and `Promotion::Inner`
		// and re-key the promotions; two implementations of that is two places
		// for a pasted fold to end up pointing at the original's insides.
		//
		// Links go back through `Connect`, so a document that lies about a type
		// or would close a loop loses the link rather than the graph.
		//
		// @param other The graph to copy in. Left untouched.
		// @param dx    How far right to place the copies, in canvas units.
		// @param dy    How far down.
		// @param owner Which depth the roots land at.
		// @return The nodes that landed at `owner`: the copies of what was
		//         top-level in `other`.
		std::vector<NodeId> Absorb(const Graph &other, float dx, float dy, NodeId owner = NO_NODE);

		// --- the custom library -----------------------------------------------

		// A folded node, kept as a document so it can be placed again.
		//
		// **Carried by the graph rather than by the process.** The reference
		// implementation keeps its custom types in browser storage, which makes
		// a saved graph unopenable anywhere else; a document that carries its own
		// types is one file that is the whole thing.
		struct Template {
			// What a person picks it out of the library by. Unique; remembering
			// one under a name already there replaces it.
			std::string Name;

			// What `SaveSubtree` produced. Held as text and not as a `Graph`,
			// because a `Graph` inside a `Graph` is a copy that has to be kept
			// in step with a format that already exists.
			std::string Document;
		};

		// Every folded node this graph carries.
		//
		// @return The library. Valid until the next `Remember` or `Forget`.
		const std::vector<Template> &Templates() const {
			return Library;
		}

		// Adds one, replacing any of the same name.
		void Remember(std::string name, std::string document);

		// Drops one by name.
		//
		// @param name The template to forget.
		// @return `false` when there is no such template.
		bool Forget(const std::string &name);

		// Nodes in dependency order: every node after everything it reads.
		//
		// Kahn's algorithm, seeded in placement order rather than from a hash
		// map, so two runs over one graph produce one order. A cycle stops it,
		// and the orderable prefix is the honest answer.
		std::vector<NodeId> Ordered() const;

		// A hash of everything that can change what a node computes.
		//
		// **Position, label and selection are deliberately not in it.** The
		// reference implementation records hashing evaluation status by
		// accident, which never settled and recomputed the graph for ever.
		uint64_t Hash(NodeId node) const;

		// The whole graph's topology and parameters.
		uint64_t Signature() const;

		// Empties the graph: nodes, links, frames and library.
		//
		// **The id counters go back to one as well**, because a cleared graph is
		// a new document rather than the same one continued. A save written from
		// it should read the same as one written from a fresh `Graph`.
		void Clear();

	  private:
		std::vector<Node> Stored;
		std::vector<Link> Wires;
		std::vector<nodegraph::Group> Frames;
		std::vector<Template> Library;
		NodeId Next = 1;
		GroupId NextGroup = 1;

		bool Reaches(NodeId from, NodeId to) const;
	};
}
