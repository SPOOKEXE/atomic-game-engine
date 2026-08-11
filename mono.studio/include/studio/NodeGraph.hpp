#pragma once

// A typed node graph, and a canvas to edit it in.
//
// **Here to answer what a node editor in this program would be**, ahead of the
// two the roadmap already wants: the render pipeline as a node system with a
// studio editor, and a bake pipeline that `Engine::bakegraph` already describes
// as nodes. Both need the same four things — a registry, a model with a cycle
// guard, a layout, and a canvas — and building either without having built one
// is how an editor ends up shaped by whichever came first.
//
// The standalone reference is `~/Documents/GitHub/node-graph-template`, which
// carries the same design as a library and as a single-file JavaScript version.
// **This is a second implementation and that is a debt, not a pattern.** It is
// worth taking while this is a demo panel; the moment something in the engine
// depends on it, the two become one — a vendored dependency or an engine module
// — and `D00113` carries which.
//
// ## The split, and why it is where it is
//
// Everything above `Canvas` is model: registries, the graph, layout, the
// evaluator, save and load. None of it includes ImGui, and all of it is checked
// by `tests/NodeGraph.cpp` with no window. That is not tidiness — it is that
// these are the parts which fail *silently*. A cycle is a hang, a link validator
// that lets a type mismatch through is a crash inside somebody's evaluator, a
// content hash that never settles is a graph that recomputes for ever, and a
// save format that drops a widget is somebody's work lost. A canvas that draws a
// node in the wrong place is visible in a second.
//
// `Canvas` paints with `ImDrawList` and hit-tests analytically against the same
// `NodeLayout` it draws from. It submits no ImGui widget per node, which is what
// makes zoom work: an ImGui button does not scale, so a canvas built from them
// has text that stays one size while its boxes grow.
//
// ## Three rules the build does not check
//
// **A picture belongs to the wire, not to the node.** `DataType::Preview` is
// what lets a panel draw a node's *inputs*: an input's payload was made upstream
// by a type the reader has never heard of, and the only thing both ends agree on
// is what the wire carries. `NodeType::Preview` overrides it for the rare type
// that wants its own, and `PictureOf` is the one function both go through — the
// canvas's thumbnail and the inspector's strip cannot disagree because there is
// nowhere for them to disagree.
//
// **A compressed node is a view, not a container.** Folding a selection sets
// `Node::Owner` on its members and derives `Node::Proxies` from the links that
// already crossed the boundary. Nothing is moved and no link is re-pointed, so
// `Ordered`, `Reaches`, `Hash` and the evaluator are all unchanged and none of
// them can be wrong about compression. `Standing` maps a port to where it
// appears at a depth and `Actual` maps it back; every model call made from the
// canvas goes through one of them, because a wire dropped on a proxy port has to
// land on the port it names.
//
// **Depth, framing and collapsing are not parameters.** None of `Node::Owner`,
// `Node::Collapsed` or `Graph::Groups` reaches `Hash`, because tidying a graph
// must not recompute it. The reference implementation's own notes record the
// cost of getting this wrong the other way: it hashed evaluation status by
// accident and the cache never settled.
//
// ## Pieces moving between graphs
//
// `Absorb` is the only thing that copies nodes in. Paste, duplicate and placing
// a saved library type are all it, because the part that is easy to get wrong —
// remapping ids through `Node::Owner`, `Proxy::Inner` and `Promotion::Inner`,
// and re-keying the promotions — must exist once. `SaveSubtree` is its
// counterpart and writes through `Save`, so a template is a whole document that
// opens, diffs and hand-edits like any other.
//
// @tier client

#include <any>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace studio::nodes {

	// --- the vocabulary -------------------------------------------------------

	// A colour, 0..1 per channel. Not an ImGui type, so the model layer stays
	// free of the toolkit.
	struct Colour {
		float R = 1.0f;
		float G = 1.0f;
		float B = 1.0f;
		float A = 1.0f;

		// 0xRRGGBB, alpha 1.
		static Colour Hex(uint32_t rgb);
	};

	// A small square picture of whatever a node produced.
	//
	// **The library never learns what a payload is, so somebody else says how to
	// draw one.** A height field becomes a grey ramp, a colour field becomes
	// itself, and a payload carrying a number has no picture at all — which is
	// the honest answer rather than a grey square.
	struct PreviewImage {
		// Pixels a side. Square, because a node's thumbnail slot is.
		uint32_t Side = 0;

		// `Side * Side * 4` bytes, red first, top row first — what
		// `assets::TextureData` takes.
		std::vector<uint8_t> Rgba;

		bool Valid() const {
			return Side > 0 && Rgba.size() == static_cast<size_t>(Side) * Side * 4;
		}
	};

	// A payload read as a square grid of heights.
	//
	// **The one thing a 3-D view needs and a picture cannot give it.** A
	// thumbnail of a height field is already shaded, so re-reading the shading
	// as elevation would put the lighting into the geometry. This is the
	// unshaded numbers.
	struct Surface {
		uint32_t Side = 0;

		// `Side * Side`, row major, top row first. Expected in 0..1; anything
		// else still draws, just taller.
		std::vector<float> Heights;

		bool Valid() const {
			return Side > 1 && Heights.size() == static_cast<size_t>(Side) * Side;
		}

		float At(uint32_t x, uint32_t y) const {
			const uint32_t cx = x < Side ? x : Side - 1;
			const uint32_t cy = y < Side ? y : Side - 1;
			return Heights[static_cast<size_t>(cy) * Side + cx];
		}
	};

	// One kind of thing an edge may carry.
	struct DataType {
		// What a link compares. A **string** and not an ordinal, for AGENTS.md
		// rule 4's reason: this crosses a save file, and an id derived from
		// registration order would connect different things the moment two
		// registrations swapped.
		std::string Id;
		std::string Label;
		Colour Tint;
		std::string Description;

		// Turns a payload carried on a wire of this type into a picture.
		//
		// **On the data type rather than on the node**, which is what makes an
		// inspector able to show a node's *inputs*. An input's payload was made
		// upstream by a node type this one has never heard of; the only thing
		// both ends agree on is the wire, so the wire is where the knowledge
		// belongs. A node type may still override it — see `NodeType::Preview`
		// — for the case where two nodes on one wire type want different
		// pictures.
		std::function<bool(const std::any &, PreviewImage &)> Preview;

		// One line saying what a payload is: `"scalar field 256²"`, `"0.4213"`.
		//
		// Shown wherever a picture will not do, which is most of an inspector's
		// rows. Empty for a type nobody taught, and the inspector then says the
		// only true thing left — that something is there.
		std::function<std::string(const std::any &)> Describe;

		// Reads a payload as elevation, for the 3-D view. Empty for a wire that
		// is not carrying a landscape, which is most of them — and the inspector
		// then offers no 3-D button rather than a flat plane.
		std::function<bool(const std::any &, Surface &)> Heights;
	};

	// The wildcard, spelled once.
	inline constexpr const char *ANY_TYPE = "data.ANY";

	// Every registered data type.
	class DataTypes {
	  public:
		static void Register(const DataType &type);
		static const DataType *Find(const std::string &id);

		// Identical ids, or either side being the wildcard.
		//
		// **Deliberately not a conversion table.** An implicit conversion is a
		// decision taken where nobody can see it, and "the drop went red" only
		// means something while the rule is a plain yes or no.
		static bool CanConnect(const std::string &from, const std::string &to);
		static const std::vector<DataType> &All();
	};

	// What a knob is, before anybody has drawn one.
	enum class WidgetKind : uint8_t { Slider, Number, Text, Toggle, Select, Colour };

	// One value a node carries. A tagged struct rather than a variant, so that
	// saving it is a switch rather than a visitor.
	struct Value {
		WidgetKind Kind = WidgetKind::Number;
		double Number = 0.0;
		bool Flag = false;
		std::string Text;
		nodes::Colour Tint;

		bool operator==(const Value &other) const;
	};

	// A knob's declaration.
	//
	// **One schema, three consumers** — the painter, the hit test and any
	// inspector. A widget drawn where it cannot be clicked is the failure that
	// arrangement makes impossible.
	struct WidgetSpec {
		// Stable key. Saved, so it is a name and never an index.
		std::string Key;
		std::string Label;
		WidgetKind Kind = WidgetKind::Slider;
		double Minimum = 0.0;
		double Maximum = 1.0;
		double Step = 0.01;
		int Precision = 2;
		std::vector<std::string> Options;
		Value Default;
	};

	// A port's declaration.
	struct PortSpec {
		std::string Name;
		std::string Type;
	};

	// Registration helpers, so a node type reads as a declaration.
	//@{
	PortSpec Port(std::string name, std::string type);
	WidgetSpec Slider(std::string key, std::string label, double minimum, double maximum, double value);
	WidgetSpec Toggle(std::string key, std::string label, bool value);
	WidgetSpec Select(std::string key, std::string label, std::vector<std::string> options, int chosen);
	WidgetSpec Number(std::string key, std::string label, double value);
	WidgetSpec Text(std::string key, std::string label, std::string value);
	//@}

	// --- node types -----------------------------------------------------------

	// What an evaluation sees: the node's knobs, and whatever its inputs made.
	struct Inputs {
		const std::unordered_map<std::string, Value> *Widgets = nullptr;
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

		Value Widget(const std::string &key) const;
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
		std::string Id;
		std::string Title;
		std::string Category;
		Colour Accent;
		std::string Subtitle;
		std::vector<PortSpec> Inputs;
		std::vector<PortSpec> Outputs;
		std::vector<WidgetSpec> Widgets;
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
		std::function<nodes::Outputs(const nodes::Inputs &)> Evaluate;

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
		static void Register(const NodeType &type);
		static const NodeType *Find(const std::string &id);
		static const std::vector<NodeType> &All();
		static std::vector<std::string> Categories();

		// Types with an input that would take this type id.
		static std::vector<const NodeType *> AcceptingInput(const std::string &type);
	};

	// --- the model ------------------------------------------------------------

	using NodeId = uint32_t;
	inline constexpr NodeId NO_NODE = 0;

	// One port a compressed node shows, and the inner port it stands for.
	//
	// **It names the inner port rather than replacing it.** Compression does not
	// re-point a single link: the wires stay between the nodes that made them,
	// and this says where a wire that visually ends on the compressed node
	// actually ends. That is what keeps the evaluator, the cycle guard, the
	// content hash and the save format entirely unaware that compression exists.
	struct Proxy {
		std::string Name;
		std::string Type;
		bool Input = false;
		NodeId Inner = NO_NODE;
		std::string InnerPort;
	};

	// One knob a compressed node lifted out of a node inside it.
	//
	// **A write goes straight through to the inner node**, so a compressed node
	// is a live view of its contents rather than a copy that drifts from them.
	struct Promotion {
		// `<inner id>/<inner key>`. Unique by construction, and stable across a
		// save because the inner id is.
		std::string Key;
		std::string Label;
		NodeId Inner = NO_NODE;
		std::string InnerKey;

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
		NodeId Id = NO_NODE;

		// A type nobody registered is kept and drawn as broken rather than
		// dropped: a graph saved with something's node type has to survive being
		// opened without it.
		std::string Type;
		float X = 0.0f;
		float Y = 0.0f;

		// By `WidgetSpec::Key`. A missing key reads as the type's default, so a
		// type that grows a widget does not invalidate every saved graph.
		std::unordered_map<std::string, Value> Widgets;
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
		// evaluator recurse and every hash fold in a subgraph signature — which
		// is the trap the reference implementation records falling into.
		NodeId Owner = NO_NODE;

		// A compressed node's derived interface. Empty on every ordinary node,
		// which then reads its type's declaration.
		//@{
		std::vector<nodes::Proxy> Proxies;
		std::vector<nodes::Promotion> Promoted;
		//@}

		bool Compressed() const {
			return !Proxies.empty() || !Promoted.empty();
		}
	};

	// One connection. Ports are named for the same reason a type id is a string.
	struct Link {
		NodeId From = NO_NODE;
		std::string FromPort;
		NodeId To = NO_NODE;
		std::string ToPort;
	};

	using GroupId = uint32_t;
	inline constexpr GroupId NO_GROUP = 0;

	// A frame around some nodes, which drags them together.
	//
	// **It holds members and not a rectangle.** A stored rectangle is a second
	// copy of where the nodes are — AGENTS.md rule 2's case exactly — and it
	// goes wrong the first time one member is moved by anything that forgot to
	// update it. The canvas computes the frame from the members every time it
	// draws, which cannot be stale.
	struct Group {
		GroupId Id = NO_GROUP;
		std::string Title;
		Colour Tint;
		std::vector<NodeId> Members;
	};

	// Why a link was refused.
	enum class LinkResult : uint8_t { Made, NoSuchPort, TypeMismatch, WouldCycle, SameNode };

	// Why, in words.
	const char *Describe(LinkResult result);

	// A graph of nodes and links.
	class Graph {
	  public:
		NodeId Add(const std::string &type, float x, float y);
		bool Remove(NodeId id);
		bool Alive(NodeId id) const;

		//@{
		Node *Find(NodeId id);
		const Node *Find(NodeId id) const;
		//@}

		// **An input takes one link and the newer one wins.** A second link into
		// one input is not a merge — nothing here knows how to merge two
		// payloads — so it replaces, which is what somebody dragging onto an
		// occupied port meant.
		LinkResult Connect(NodeId from, const std::string &fromPort, NodeId to, const std::string &toPort);

		// The same question, changing nothing. What a drag asks every frame.
		LinkResult
		CanConnect(NodeId from, const std::string &fromPort, NodeId to, const std::string &toPort) const;

		bool Disconnect(NodeId to, const std::string &toPort);

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

		const Link *LinkInto(NodeId node, const std::string &port) const;

		// Every link touching a node, either end.
		std::vector<Link> LinksOf(NodeId node) const;

		// --- groups -----------------------------------------------------------

		// Frames some nodes. A node belongs to at most one group, so a member
		// already in another leaves it — two frames owning one node is two
		// drags moving it twice.
		GroupId Group(std::vector<NodeId> members, std::string title, Colour tint);

		// Removes the frame, and the nodes in it when `withNodes`.
		bool Ungroup(GroupId group, bool withNodes = false);

		const std::vector<nodes::Group> &Groups() const {
			return Frames;
		}
		std::vector<nodes::Group> &Groups() {
			return Frames;
		}

		//@{
		nodes::Group *FindGroup(GroupId group);
		const nodes::Group *FindGroup(GroupId group) const;
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
		// @return The node, or `NO_NODE` when the selection cannot be folded —
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
		// **For rebuilding something already valid** — a clipboard, a saved
		// template — and nothing else. It skips every check `Add` makes because
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
		// @param owner Which depth the roots land at.
		// @return The nodes that landed at `owner` — the copies of what was
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
			std::string Name;

			// What `SaveSubtree` produced. Held as text and not as a `Graph`,
			// because a `Graph` inside a `Graph` is a copy that has to be kept
			// in step with a format that already exists.
			std::string Document;
		};

		const std::vector<Template> &Templates() const {
			return Library;
		}

		// Adds one, replacing any of the same name.
		void Remember(std::string name, std::string document);
		bool Forget(const std::string &name);

		// Nodes in dependency order — every node after everything it reads.
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

		void Clear();

	  private:
		std::vector<Node> Stored;
		std::vector<Link> Wires;
		std::vector<nodes::Group> Frames;
		std::vector<Template> Library;
		NodeId Next = 1;
		GroupId NextGroup = 1;

		bool Reaches(NodeId from, NodeId to) const;
	};

	// --- layout ---------------------------------------------------------------

	// The sizes everything is built from, in graph space.
	struct Metrics {
		float HeaderHeight = 26.0f;
		float RowHeight = 20.0f;
		float WidgetHeight = 22.0f;
		float Padding = 8.0f;
		float PortRadius = 5.0f;
		float Rounding = 6.0f;
		float LabelSize = 13.0f;
		float SmallSize = 11.0f;
	};

	// A port, placed relative to its node's top-left corner.
	struct PlacedPort {
		std::string Name;
		std::string Type;
		bool Input = false;
		float X = 0.0f;
		float Y = 0.0f;
	};

	// A widget, placed. The rectangle is the row it may be dragged in.
	struct PlacedWidget {
		std::string Key;
		size_t Index = 0;
		float X = 0.0f;
		float Y = 0.0f;
		float Width = 0.0f;
		float Height = 0.0f;
	};

	// One node's geometry, and the one answer to "where is that slider".
	struct NodeLayout {
		float Width = 0.0f;
		float Height = 0.0f;
		std::vector<PlacedPort> Ports;
		std::vector<PlacedWidget> Widgets;
		float WidgetsTop = 0.0f;

		// The thumbnail's square, for a type that has a `Preview`. Zero-sided
		// when it has none, which is what a node carrying a number gets: an
		// empty slot would be a picture that never arrives.
		float PreviewTop = 0.0f;
		float PreviewSide = 0.0f;

		// The row a progress bar goes in while the node is working. Always
		// present on an async type, so a node does not change height when it
		// starts — a graph that reflowed as it ran would be one nobody could
		// click in.
		float ProgressTop = 0.0f;
		float ProgressHeight = 0.0f;
	};

	// The interface a node actually has.
	//
	// **A node and not its type**, because a compressed node's ports and knobs
	// come from how its contents were wired rather than from a declaration.
	// Everything downstream — layout, hit testing, the inspector — reads these
	// so that none of them can disagree with what the canvas drew.
	//@{
	std::vector<PortSpec> InputsOf(const Node &node);
	std::vector<PortSpec> OutputsOf(const Node &node);
	std::vector<WidgetSpec> WidgetsOf(const Node &node);
	//@}

	// Where a port's payload really is: itself, or the inner port a compressed
	// node's proxy stands for.
	//
	// **Resolved by whoever reads it rather than republished by the
	// evaluator.** A compressed node's output *is* its inner node's output;
	// copying it forward would be a second field's worth of memory every frame
	// for a node that computed nothing.
	//
	// @return False when the node or port does not exist.
	bool Actual(
		const Graph &graph,
		NodeId node,
		const std::string &port,
		bool input,
		NodeId &outNode,
		std::string &outPort
	);

	// The inverse, and the one a view asks: where does this port *appear* at a
	// given depth? The node itself when it already sits there, or the compressed
	// node standing in for it and the proxy port that does.
	//
	// **One walk, used by both the canvas and by compression itself.** A wire
	// crossing into a folded selection has to be drawn to the fold, and a
	// selection being folded has to know which of its members an outside wire
	// arrives at — those are the same question asked from two sides, and two
	// implementations of it is how a wire ends up drawn to the wrong port.
	//
	// @return False when the port is not visible at `depth` at all.
	bool Standing(
		const Graph &graph,
		NodeId node,
		const std::string &port,
		bool input,
		NodeId depth,
		NodeId &outNode,
		std::string &outPort
	);

	// Where a knob really lives: itself, or the inner node a compressed node
	// promoted it from. A write to a promoted knob is a write to the node
	// inside, which is what makes a compressed node a view of its contents.
	bool ActualWidget(
		const Graph &graph, NodeId node, const std::string &key, NodeId &outNode, std::string &outKey
	);

	// Reading and writing a knob, following a promotion to the node it really
	// lives on. **Everything that touches a value goes through these** — the
	// painter, the hit test and the inspector — because a compressed node that
	// showed one number and wrote another would be worse than one with no knobs.
	//@{
	Value ValueOf(const Graph &graph, NodeId node, const WidgetSpec &spec);
	void SetValue(Graph &graph, NodeId node, const std::string &key, const Value &value);
	//@}

	// The type a compressed node is placed as. Registered on first use, hidden
	// from the palette — one is made by folding a selection and never by
	// picking it out of a list.
	inline constexpr const char *CUSTOM_TYPE = "custom.node";

	// Whether this type's nodes get a thumbnail slot — its own `Preview`, or a
	// preview port whose data type carries one.
	bool HasPicture(const NodeType &type);

	// Lays out one node. A node of an unregistered type still gets a body, so it
	// can be seen, moved and deleted. A collapsed one gets its header and its
	// ports and nothing else.
	NodeLayout LayoutOf(const Node &node, const Metrics &metrics = Metrics{});

	// The port by name, or nullptr.
	const PlacedPort *PortIn(const NodeLayout &layout, const std::string &name, bool input);

	// --- evaluation -----------------------------------------------------------

	// What one run did.
	struct RunReport {
		size_t Evaluated = 0;
		size_t Cached = 0;
		size_t Skipped = 0;

		// Async nodes handed to a worker by this run.
		size_t Started = 0;

		// Async results collected by this run.
		size_t Finished = 0;

		// Async nodes still working when it returned.
		size_t Running = 0;

		// Nodes waiting on an input that is still being computed.
		//
		// **Counted rather than treated as skipped**, because they are two
		// different facts: a node with no evaluation will never produce
		// anything, and this one is about to.
		size_t Waiting = 0;
	};

	// What a node is doing, as the canvas draws it.
	enum class NodeState : uint8_t { Idle, Running, Done, Failed };

	// One node's live state.
	struct NodeStatus {
		NodeState State = NodeState::Idle;

		// 0 to 1, from the node's own reporting.
		float Progress = 0.0f;

		// Which of `NodeType::Steps` it says it is on.
		size_t Step = 0;

		// What it last said it was doing.
		std::string Note;

		// How long the evaluation took, once it has finished.
		double Milliseconds = 0.0;

		// Whether the last result came from the cache rather than a run.
		bool Cached = false;
	};

	// Runs a graph and holds what it produced.
	//
	// **One call a frame, and it never blocks.** `Run` evaluates every sync node
	// it can, hands every ready async one to a worker, collects whatever
	// finished since last time, and returns. A node whose input is still being
	// computed is left for the next call — which is what makes two independent
	// branches run at once without anything here scheduling them: readiness is
	// the schedule.
	class Evaluator {
	  public:
		Evaluator();
		~Evaluator();

		Evaluator(const Evaluator &) = delete;
		Evaluator &operator=(const Evaluator &) = delete;

		RunReport Run(const Graph &graph);

		// Runs until nothing is left working.
		//
		// **For a test and for shutdown, not for a frame.** Blocking the frame
		// thread on a worker is exactly what the async path exists to avoid.
		RunReport RunToCompletion(const Graph &graph);

		// What a node's output port produced in the last run, or nullptr.
		const std::any *Output(NodeId node, const std::string &port) const;

		// Whether a node's last run came from the cache.
		bool WasCached(NodeId node) const;

		// What a node is doing right now.
		NodeStatus Status(NodeId node) const;

		// The hash a node last ran at, or zero.
		//
		// **What a thumbnail is keyed on.** A picture belongs to a result and
		// not to a node, so two nodes with one hash share one texture and an
		// edit makes a new key rather than overwriting the old one.
		uint64_t RanAt(NodeId node) const;

		// Whether any worker is still busy.
		bool Busy() const;

		// Drops every held result. In-flight work is left to finish and its
		// result is kept — it is keyed by a hash that is still correct.
		void Forget();

		size_t Held() const {
			return Results.size();
		}

	  private:
		// One job on its way to a worker, and its result on the way back.
		struct Task;

		// The progress one running node publishes. Shared with its worker, so
		// every field is either atomic or behind the small lock beside them.
		struct Live {
			std::atomic<float> Progress{0.0f};
			std::atomic<size_t> Step{0};
			std::atomic<bool> Finished{false};
			std::atomic<bool> Failed{false};
			std::atomic<double> Milliseconds{0.0};
			std::mutex Words;
			std::string Note;
			Outputs Produced;
			uint64_t Hash = 0;
			NodeId Node = NO_NODE;
		};

		void Begin();
		void Collect(RunReport &report);
		void Worker();

		// **Keyed by hash and not by node.** Undoing an edit, or flipping a
		// value back, then lands on a result that is still there rather than
		// recomputing it — and an async result that arrives after its node has
		// been edited is still correct for the hash it was computed at.
		std::unordered_map<uint64_t, Outputs> Results;
		std::unordered_map<NodeId, uint64_t> Ran;
		std::unordered_map<NodeId, bool> Reused;
		std::unordered_map<NodeId, NodeStatus> States;

		// What is in flight, by the hash it is computing. A second node with the
		// same hash waits on the first rather than starting its own copy.
		std::unordered_map<uint64_t, std::shared_ptr<Live>> Flight;

		std::vector<std::thread> Workers;
		std::deque<std::function<void()>> Queue;
		mutable std::mutex Lock;
		std::condition_variable Waking;
		std::atomic<bool> Stopping{false};
		bool Started = false;
	};

	// --- saving ---------------------------------------------------------------

	// Writes a graph as lines. **Not JSON**: a graph is three flat lists, and a
	// format with no nesting needs no parser and diffs line by line.
	std::string Save(const Graph &graph);

	// The same, for one node and everything inside it.
	//
	// **A whole document and not a fragment**, so filing a fold as a library
	// type and saving a graph produce the same thing — and so a template can be
	// opened, read and hand-edited like anything else here.
	std::string SaveSubtree(const Graph &graph, NodeId root);

	// Reads what `Save` wrote, replacing whatever `graph` held.
	//
	// A node of an unregistered type is kept; a link that cannot be made is
	// dropped. Refusing the whole file for one bad line would make a
	// hand-edited graph unopenable, which is the opposite of what a text format
	// is for.
	bool Load(std::string_view text, Graph &graph, std::string &error);

	// --- the canvas -----------------------------------------------------------

	// What the canvas looks like. ImGui's 0xAABBGGRR packing.
	struct Style {
		uint32_t Background = 0xFF0A0A0A;
		uint32_t GridFine = 0xFF1A1A1A;
		uint32_t GridCoarse = 0xFF262626;
		uint32_t NodeBody = 0xFF1E1E1E;
		uint32_t NodeBorder = 0xFF3A3A3A;
		uint32_t NodeSelected = 0xFF4CA6FF;
		uint32_t Text = 0xFFE6E6E6;
		uint32_t Muted = 0xFF8A8A8A;
		uint32_t Widget = 0xFF2B2B2B;
		uint32_t WidgetFill = 0xFF4A6FA5;
		uint32_t Refused = 0xFF4444EE;
		uint32_t Marquee = 0x224CA6FF;
		Metrics Sizes;
	};

	// Turns a preview into something the canvas can draw.
	//
	// **The library holds no device**, so the host says how a picture becomes a
	// texture. `key` is the hash the payload was computed at: a sink is expected
	// to keep what it made under that key and call `make` only on a miss, which
	// is what stops a thumbnail being rebuilt sixty times a second.
	//
	// @return Whatever the host's ImGui backend takes as a texture id, or null.
	using ImageSink = std::function<void *(uint64_t key, const std::function<bool(PreviewImage &)> &make)>;

	// What a canvas asks its host to do, for the two things it cannot decide.
	//
	// **Callbacks rather than a queue of enums**, because there are two and each
	// has one caller. A canvas that owned an undo stack would be a canvas
	// deciding what a document is, and a second canvas over the same graph would
	// then own a second one.
	struct Hooks {
		// A mutation landed and the host should record it. Called once per
		// gesture, at the end — a slider drag is one undo step and not sixty.
		std::function<void()> Changed;

		// Somebody asked for one node to be computed again from scratch.
		std::function<void(NodeId)> Rerun;
	};

	// A view over one graph. Holds the camera and the drag, and nothing about
	// the model — two canvases over one graph is two views of one thing.
	class Canvas {
	  public:
		// Where thumbnails come from. Without one, a node with a preview draws
		// an empty frame rather than nothing at all, so the slot is still
		// visibly a slot.
		void Images(ImageSink sink) {
			Sink = std::move(sink);
		}

		// Draws and drives the graph inside the current ImGui window.
		void Draw(Graph &graph);

		// Shows what each node last produced. Read only, and when the graph is
		// evaluated stays the caller's decision.
		void Observe(const Evaluator *evaluator) {
			Watching = evaluator;
		}

		const std::vector<NodeId> &Selection() const {
			return Chosen;
		}
		void Select(NodeId node);
		void Select(std::vector<NodeId> nodes);
		void SelectAll(const Graph &graph);

		// The frame the selection is, when it is a frame rather than nodes.
		GroupId SelectedGroup() const {
			return ChosenGroup;
		}

		// Frames every node, or only the selection when there is one.
		void Fit(const Graph &graph);

		// Puts one node in the middle without changing the zoom. What the Types
		// panel does when somebody clicks a row.
		void Centre(const Graph &graph, NodeId node);

		float Zoom() const {
			return Scale;
		}
		void SetZoom(float zoom);

		// --- what the toolbar and the keys drive ------------------------------
		//
		// **Here rather than in the panel**, because each is the selection plus
		// the graph and nothing else, and the selection is the canvas's. A panel
		// re-deriving "which nodes are chosen" would be the second copy rule 2
		// is about.
		//@{
		void Delete(Graph &graph);
		void Copy(const Graph &graph);
		void Paste(Graph &graph);
		void Duplicate(Graph &graph);
		void GroupSelection(Graph &graph);
		void UngroupSelection(Graph &graph);
		void Collapse(Graph &graph, bool collapsed);
		bool CanPaste() const {
			return !Clip.Nodes().empty();
		}

		// Places a saved subtree — a library type — in the middle of the view.
		void Place(Graph &graph, const std::string &document);

		// Folds the selection into one node, and the inverse.
		void CompressSelection(Graph &graph);
		void ExpandSelection(Graph &graph);
		//@}

		// --- depth ------------------------------------------------------------

		// Which compressed node's contents are being drawn, or `NO_NODE` for the
		// root.
		//
		// **A filter over one graph and not a second graph.** Every feature —
		// framing, folding, inserting on a link — therefore works at any depth
		// with nothing here changing, because there is only ever one model.
		NodeId Inside() const {
			return Depth.empty() ? NO_NODE : Depth.back();
		}

		// The stack, outermost first. What a breadcrumb bar reads.
		const std::vector<NodeId> &Path() const {
			return Depth;
		}

		void Enter(const Graph &graph, NodeId node);

		// Back out one level, or to a given depth. `Ascend(0)` is the root.
		//@{
		void Leave(const Graph &graph);
		void Ascend(const Graph &graph, size_t depth);
		//@}

		Style Look;

		// Where the host is told about the things a canvas may not decide.
		Hooks Signals;

		// Whether a dragged node lands on the grid. Off, because a graph is not
		// a diagram and the snap fights small adjustments.
		bool Snap = false;

		// A data type id to pick out, or empty. Everything carrying something
		// else fades.
		//
		// **Set from outside, because the question comes from outside**: it is
		// the Types panel asking "where does a field go", and answering it by
		// dimming the rest is the one view that shows the shape of an answer
		// spread over forty wires.
		std::string Highlight;

		// What the last refused connection was. Cleared by the next one that is
		// accepted.
		std::string LastRefusal;

	  private:
		//@{
		void ToScreen(float x, float y, float &outX, float &outY) const;
		void ToGraph(float x, float y, float &outX, float &outY) const;
		//@}

		void DrawGrid(float x, float y, float width, float height) const;
		void DrawGroups(const Graph &graph) const;
		void DrawLinks(const Graph &graph, size_t hovered) const;
		void DrawNode(const Graph &graph, const Node &node, const NodeLayout &layout) const;
		void DrawWidget(const Graph &graph, const Node &node, const PlacedWidget &placed) const;

		bool HitPort(
			const Graph &graph, float graphX, float graphY, NodeId &node, std::string &port, bool &input
		) const;
		NodeId HitNode(const Graph &graph, float graphX, float graphY) const;

		// The link nearest the cursor, by sampling its curve. `Graph::Links()`'s
		// index, or `npos`.
		//
		// **Sampled rather than solved.** The exact distance to a cubic is a
		// quartic root-find; sixteen points along it is a handful of
		// multiplications and is wrong by less than the grab radius, which is
		// the only accuracy a hit test has to have.
		size_t HitLink(const Graph &graph, float graphX, float graphY) const;

		// The frame whose title bar is under the cursor. A group is grabbed by
		// its bar and not by its body, so a marquee still works inside one.
		GroupId HitGroup(const Graph &graph, float graphX, float graphY) const;

		// Where a link's insert button sits, in graph space.
		bool LinkMiddle(const Graph &graph, size_t link, float &outX, float &outY) const;

		// The frame around a group's members, in graph space.
		bool GroupBounds(
			const Graph &graph,
			const nodes::Group &group,
			float &left,
			float &top,
			float &right,
			float &bottom
		) const;

		void HandleWidget(Graph &graph, NodeId node, const PlacedWidget &placed, float graphX);
		void Palette(Graph &graph);
		void Menu(Graph &graph);

		// Splices a node into a link: the link is removed and two take its
		// place, on the first ports of the new node that will carry the type.
		void InsertOn(Graph &graph, size_t link, const std::string &type);

		float Scale = 1.0f;
		float PanX = 0.0f;
		float PanY = 0.0f;
		float OriginX = 0.0f;
		float OriginY = 0.0f;

		// How big the canvas was when it was last drawn.
		//
		// **Remembered rather than asked for.** `Fit` and `Centre` are pressed
		// from a toolbar and from a panel row, where `GetContentRegionAvail`
		// answers about *that* window — so asking would fit the graph to the
		// width of the button strip, and calling either outside a frame at all
		// would read a null window.
		//@{
		float ViewWidth = 800.0f;
		float ViewHeight = 600.0f;
		//@}

		// Whether a node is drawn in this view: exactly those at the current
		// depth.
		bool Visible(const Node &node) const {
			return node.Owner == Inside();
		}

		std::vector<NodeId> Chosen;
		GroupId ChosenGroup = NO_GROUP;
		std::vector<NodeId> Depth;

		// Exactly one drag is in progress, so this is one enum rather than five
		// booleans — five booleans is five ways to be in two states at once.
		enum class Dragging : uint8_t { None, Nodes, Link, Marquee, Pan, Widget, Group };
		Dragging Drag = Dragging::None;

		NodeId DragNode = NO_NODE;
		std::string DragPort;
		bool DragFromInput = false;
		std::string DragWidget;
		GroupId DragGroup = NO_GROUP;

		// Whether this gesture changed the graph. Reported to `Hooks::Changed`
		// once, when the mouse comes up — a drag is one edit and not one an
		// undo step per frame.
		bool Edited = false;

		float MarqueeX = 0.0f;
		float MarqueeY = 0.0f;
		float PaletteX = 0.0f;
		float PaletteY = 0.0f;
		bool PaletteOpen = false;

		// What the palette will do with the type somebody picks. Splicing into
		// a link, wiring to the port a drag came from, or neither.
		//
		// `PaletteNeeds` is which side of a candidate has to accept
		// `PaletteType`: nothing, an input, an output, or both — which is what a
		// splice into an existing link requires.
		//@{
		size_t PaletteLink = static_cast<size_t>(-1);
		std::string PaletteType;
		enum class Needs : uint8_t { Anything, Input, Output, Both };
		Needs PaletteNeeds = Needs::Anything;
		char PaletteSearch[64] = {};

		// **Asked for on one frame and opened on the next.** The context menu
		// is where "Add node..." is pressed, and opening a popup from inside the
		// popup that is closing puts the two on one another's id stack.
		bool PaletteWanted = false;
		//@}

		// What the context menu was opened over.
		//@{
		bool MenuOpen = false;
		NodeId MenuNode = NO_NODE;
		size_t MenuLink = static_cast<size_t>(-1);
		GroupId MenuGroup = NO_GROUP;
		//@}

		// **A whole graph rather than two loose vectors**, so pasting is the
		// same `Absorb` that places a library type — the remap that pasting a
		// fold needs is not a thing to write twice.
		Graph Clip;

		const Evaluator *Watching = nullptr;
		ImageSink Sink;
	};

	// --- inspectors -----------------------------------------------------------

	// What an inspector handler is handed.
	//
	// **Read only, and it holds no ImGui type.** A handler draws with ImGui, but
	// what it is *given* is the model plus a way to make a texture, which is
	// what lets this declaration live beside the rest of the library.
	struct Inspection {
		const nodes::Node *Node = nullptr;
		const nodes::NodeType *Type = nullptr;
		const nodes::Graph *Graph = nullptr;
		const nodes::Evaluator *Runner = nullptr;

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
		static void Register(const std::string &id, InspectorFn draw);
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

	// The picture a payload on this wire makes, or false when it makes none.
	//
	// The node type's own `Preview` first, then the port's `DataType::Preview`.
	// One function, so the canvas's thumbnail and the inspector's strip can
	// never disagree about what a node looks like.
	bool PictureOf(
		const NodeType *type, const std::string &portType, const std::any &payload, PreviewImage &image
	);

	// What a payload is, in one line. `DataType::Describe` where there is one,
	// and otherwise the only true thing left.
	std::string DescribeValue(const std::string &portType, const std::any &payload);

	// Reads a payload as elevation, through its wire's `DataType::Heights`.
	bool SurfaceOf(const std::string &portType, const std::any &payload, Surface &out);

	// Draws a lit surface into a square picture.
	//
	// **A software rasteriser, and deliberately.** The alternative is a render
	// target, a camera and a mesh upload inside a panel — a device dependency
	// for a 190-pixel square, and a second path for a picture that already has
	// one. This produces a `PreviewImage` like everything else here, so the
	// texture, the caching and the drawing are the code that was already there,
	// and it can be checked with no window.
	//
	// @param colour Optional albedo, sampled across the surface — the same
	//               square the 2-D view shows. Grey ramp without one.
	// @param yaw    Rotation about the vertical, radians.
	// @param pitch  Tilt towards the viewer, radians, clamped to a sane range.
	// @param relief How tall the height range stands, in units of the surface's
	//               width. 0 is flat.
	bool RenderSurface(
		const Surface &surface,
		const PreviewImage *colour,
		float yaw,
		float pitch,
		float relief,
		uint32_t side,
		PreviewImage &out
	);

	// Writes a picture as a PNG.
	//
	// **Stored deflate blocks, so nothing has to be linked.** A real PNG is what
	// somebody expects an exported picture to be, and `mono.engine/bake` only
	// *reads* one — its decoder is a publishing-pipeline concern behind
	// `Engine::bake`, which this program's editor half does not link and should
	// not start linking for a file dialog. The cost is a file about a third
	// larger than a compressed one, for a picture somebody asked to look at
	// rather than ship; `assetc` is where a compressed one belongs.
	std::vector<uint8_t> EncodePng(const PreviewImage &image);

	// The key a picture is held under: the hash its payload was computed at, and
	// which port it left by.
	//
	// **Both, because one run produces every output at once.** A key that was
	// only the hash would hand a node's second port whichever picture its first
	// one made, and the two would then differ for as long as the cache held.
	uint64_t PictureKey(uint64_t ran, const std::string &port);

	// The demo's node types: numbers, fields and an output.
	//
	// **Registered on first use rather than at static-initialisation time.** A
	// registry filled before `main` is a registry whose order depends on link
	// order, and the palette's order is the order things were registered in.
	void RegisterDemoNodes();

	// A wired graph to open the demo on, so the first thing anybody sees is a
	// graph rather than an empty grid.
	void BuildDemoGraph(Graph &graph);
}
