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
// @tier client

#include <any>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
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
		template <typename T>
		T In(const std::string &name, T fallback = T{}) const {
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

	// A small square picture of whatever a node produced.
	//
	// **The library never learns what a payload is, so a node type says how to
	// draw one.** A height field becomes a grey ramp, a colour field becomes
	// itself, and a node carrying a number has no picture at all — which is the
	// honest answer rather than a grey square.
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
		std::function<bool(const std::any &, PreviewImage &)> Preview;

		// Which output port the preview reads. Empty takes the first declared.
		std::string PreviewPort;
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
	};

	// One connection. Ports are named for the same reason a type id is a string.
	struct Link {
		NodeId From = NO_NODE;
		std::string FromPort;
		NodeId To = NO_NODE;
		std::string ToPort;
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
		LinkResult CanConnect(NodeId from, const std::string &fromPort, NodeId to, const std::string &toPort)
			const;

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
		NodeId Next = 1;

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

	// Lays out one node. A node of an unregistered type still gets a body, so it
	// can be seen, moved and deleted.
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

		// Frames every node.
		void Fit(const Graph &graph);

		float Zoom() const {
			return Scale;
		}
		void SetZoom(float zoom);

		Style Look;

		// What the last refused connection was. Cleared by the next one that is
		// accepted.
		std::string LastRefusal;

	  private:
		//@{
		void ToScreen(float x, float y, float &outX, float &outY) const;
		void ToGraph(float x, float y, float &outX, float &outY) const;
		//@}

		void DrawGrid(float x, float y, float width, float height) const;
		void DrawLinks(const Graph &graph) const;
		void DrawNode(const Node &node, const NodeLayout &layout) const;
		void DrawWidget(const Node &node, const NodeType &type, const PlacedWidget &placed) const;

		bool HitPort(
			const Graph &graph, float graphX, float graphY, NodeId &node, std::string &port, bool &input
		) const;
		NodeId HitNode(const Graph &graph, float graphX, float graphY) const;
		void HandleWidget(Graph &graph, NodeId node, const PlacedWidget &placed, float graphX);
		void Palette(Graph &graph);

		float Scale = 1.0f;
		float PanX = 0.0f;
		float PanY = 0.0f;
		float OriginX = 0.0f;
		float OriginY = 0.0f;

		std::vector<NodeId> Chosen;

		// Exactly one drag is in progress, so this is one enum rather than four
		// booleans — four booleans is four ways to be in two states at once.
		enum class Dragging : uint8_t { None, Nodes, Link, Marquee, Pan, Widget };
		Dragging Drag = Dragging::None;

		NodeId DragNode = NO_NODE;
		std::string DragPort;
		bool DragFromInput = false;
		std::string DragWidget;

		float MarqueeX = 0.0f;
		float MarqueeY = 0.0f;
		float PaletteX = 0.0f;
		float PaletteY = 0.0f;
		bool PaletteOpen = false;

		const Evaluator *Watching = nullptr;
		ImageSink Sink;
	};

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
