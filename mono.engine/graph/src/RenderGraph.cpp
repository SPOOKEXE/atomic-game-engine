#include <engine/graph/RenderGraph.hpp>

#include <algorithm>
#include <span>
#include <unordered_map>
#include <unordered_set>

namespace engine::graph {

	namespace {
		// Where a resource is written, and by which sort of node.
		struct Producer {
			bool Written = false;
			bool ByShared = false;
			bool ByPerView = false;
		};
	}

	const char *Describe(GraphStatus status) {
		switch (status) {
		case GraphStatus::Ok:
			return "ok";
		case GraphStatus::DuplicateNode:
			return "two nodes share a name";
		case GraphStatus::WritesNothing:
			return "a node writes nothing";
		case GraphStatus::UnknownResource:
			return "a node names a resource this graph does not hold";
		case GraphStatus::ReadsBeforeWrite:
			return "a node reads something nothing earlier wrote";
		case GraphStatus::TooManyNodes:
			return "past the node limit";
		case GraphStatus::SharedBetweenViews:
			return "a shared node sits between two per-view nodes";
		case GraphStatus::SharedWriteConflict:
			return "a per-view node writes what a shared node writes";
		}
		return "unknown";
	}

	ResourceId RenderGraph::AddResource(const ResourceDesc &desc) {
		if (!desc.Name.IsValid()) {
			return {};
		}

		// **A duplicate name is refused rather than merged.** Two declarations of
		// `depth` with different sizes is a graph whose behaviour depends on
		// which one a node happened to be handed, and merging them would pick
		// one silently.
		for (const ResourceDesc &existing : Resources) {
			if (existing.Name == desc.Name) {
				return {};
			}
		}

		Resources.push_back(desc);
		return ResourceId{static_cast<uint32_t>(Resources.size())};
	}

	NodeId RenderGraph::AddNode(Node node) {
		if (!node.Name.IsValid() || Nodes.size() >= MAXIMUM_NODES) {
			return {};
		}
		Nodes.push_back(std::move(node));
		return NodeId{static_cast<uint32_t>(Nodes.size())};
	}

	bool RenderGraph::SetEnabled(NodeId node, bool enabled) {
		if (!node.IsValid() || node.Value > Nodes.size()) {
			return false;
		}
		Nodes[node.Value - 1].Enabled = enabled;
		return true;
	}

	const Node *RenderGraph::Find(NodeId node) const {
		if (!node.IsValid() || node.Value > Nodes.size()) {
			return nullptr;
		}
		return &Nodes[node.Value - 1];
	}

	const ResourceDesc *RenderGraph::FindResource(ResourceId resource) const {
		if (!resource.IsValid() || resource.Value > Resources.size()) {
			return nullptr;
		}
		return &Resources[resource.Value - 1];
	}

	GraphStatus RenderGraph::Validate(core::Name &offender) const {
		std::unordered_set<uint32_t> seen;

		for (const Node &node : Nodes) {
			if (!seen.insert(node.Name.Id()).second) {
				offender = node.Name;
				return GraphStatus::DuplicateNode;
			}

			// **Checked whether or not it is enabled.** A disabled node is still
			// part of the graph an author is editing, and reporting its faults
			// only once it is switched back on is a diagnostic arriving at the
			// least convenient moment.
			if (node.Writes.empty()) {
				offender = node.Name;
				return GraphStatus::WritesNothing;
			}

			for (const ResourceId resource : node.Reads) {
				if (FindResource(resource) == nullptr) {
					offender = node.Name;
					return GraphStatus::UnknownResource;
				}
			}
			for (const ResourceId resource : node.Writes) {
				if (FindResource(resource) == nullptr) {
					offender = node.Name;
					return GraphStatus::UnknownResource;
				}
			}
		}

		// **The partition conflict, and it is checked over the enabled set.**
		// Unlike the faults above this one is about what will actually run: two
		// nodes that never run together cannot fight over a resource.
		std::unordered_map<uint32_t, Producer> producers;
		for (const Node &node : Nodes) {
			if (!node.Enabled) {
				continue;
			}
			for (const ResourceId resource : node.Writes) {
				Producer &producer = producers[resource.Value];
				producer.Written = true;
				(node.PerView ? producer.ByPerView : producer.ByShared) = true;
			}
		}

		for (const auto &[value, producer] : producers) {
			if (producer.ByShared && producer.ByPerView) {
				const ResourceDesc *desc = FindResource(ResourceId{value});
				offender = desc != nullptr ? desc->Name : core::Name{};
				return GraphStatus::SharedWriteConflict;
			}
		}

		return GraphStatus::Ok;
	}

	GraphStatus RenderGraph::Compile(CompiledGraph &out, core::Name &offender) const {
		const GraphStatus status = Validate(offender);
		if (status != GraphStatus::Ok) {
			return status;
		}

		// **Declaration order is the execution order, and the reads and writes
		// check it rather than derive it.** This is `Pipeline.hpp`'s refusal
		// restated for a graph: a general dependency-resolving frame graph
		// "earns its complexity at twenty passes and costs more than it returns
		// at four".
		//
		// It is also the only model that survives a read-modify-write chain.
		// `opaque` writes colour, `transparent` draws onto it, `overlay` onto
		// that, `interface` onto that — every one of them both reads and writes
		// the same resource, so a producer-to-consumer graph over them is a
		// four-node cycle. That is not a graph to untangle; it is an *authored*
		// order, and no amount of dependency analysis can recover that overlay
		// goes under interface rather than over it.
		//
		// **The shared block runs before the per-view block**, so the effective
		// order is every enabled shared node in declaration order followed by
		// every enabled per-view node in declaration order. That is what the
		// check below walks.
		out.Shared.clear();
		out.PerView.clear();
		out.Final.clear();

		// **Which end a shared node lands at is decided by position alone.**
		// Before the first per-view node it is set-up, after the last it is
		// presentation, and there is no third field to keep in step with the
		// order it is already written in.
		bool seenPerView = false;
		for (size_t index = 0; index < Nodes.size(); index++) {
			if (!Nodes[index].Enabled) {
				continue;
			}

			const NodeId id{static_cast<uint32_t>(index + 1)};
			if (Nodes[index].PerView) {
				seenPerView = true;
				out.PerView.push_back(id);
			} else {
				(seenPerView ? out.Final : out.Shared).push_back(id);
			}
		}

		// A shared node that landed in `Final` with per-view nodes still to come
		// after it is the arrangement three blocks cannot express. Caught here
		// rather than in `Validate` because it is a property of the *enabled*
		// set: switching a per-view node off can make a graph legal.
		if (!out.Final.empty()) {
			const NodeId lastPerView = out.PerView.back();
			for (const NodeId id : out.Final) {
				if (id.Value < lastPerView.Value) {
					offender = Find(id)->Name;
					out.Shared.clear();
					out.PerView.clear();
					out.Final.clear();
					return GraphStatus::SharedBetweenViews;
				}
			}
		}

		std::unordered_set<uint32_t> written;
		const auto walk = [&](const std::vector<NodeId> &block) {
			for (const NodeId id : block) {
				const Node *node = Find(id);

				// Reads are checked before this node's own writes are recorded,
				// which is what makes a self read-modify-write an error unless
				// something earlier produced the resource — `transparent` is
				// legal because `opaque` wrote colour, and would not be first.
				for (const ResourceId resource : node->Reads) {
					if (!written.contains(resource.Value)) {
						offender = node->Name;
						return false;
					}
				}
				for (const ResourceId resource : node->Writes) {
					written.insert(resource.Value);
				}
			}
			return true;
		};

		if (!walk(out.Shared) || !walk(out.PerView) || !walk(out.Final)) {
			out.Shared.clear();
			out.PerView.clear();
			out.Final.clear();
			return GraphStatus::ReadsBeforeWrite;
		}

		return GraphStatus::Ok;
	}

	bool RenderGraph::Execute(const CompiledGraph &compiled, NodeRunner &runner, size_t views) const {
		// Every view in one world, which is what a game and a single-panel
		// editor both are.
		const std::vector<uint64_t> one(views, 0);
		return Execute(compiled, runner, one);
	}

	bool RenderGraph::Execute(
		const CompiledGraph &compiled, NodeRunner &runner, std::span<const uint64_t> worlds
	) const {
		const auto runBlock = [&](const std::vector<NodeId> &block, size_t view, size_t world) {
			for (const NodeId id : block) {
				const Node *node = Find(id);
				if (node == nullptr) {
					continue;
				}

				RunContext context;
				context.Node = id;
				context.Name = node->Name;
				context.Kind = node->Kind;
				context.View = view;
				context.World = world;
				context.Reads = node->Reads;
				context.Writes = node->Writes;

				if (!runner.Run(context)) {
					return false;
				}
			}
			return true;
		};

		// **Shared first, and that ordering is the whole partition.** Every
		// shared node produces something the per-view nodes may read — a shadow
		// map is the case this was built for — so running them after would have
		// each view sampling a target written for the frame after it.
		// **Distinct worlds, in first-appearance order.** Views of one world do
		// not have to be adjacent, so this is a scan rather than a run-length
		// walk — and it keeps the shared work in the order the caller listed its
		// worlds rather than in whatever order a set would produce.
		std::vector<uint64_t> distinct;
		for (const uint64_t world : worlds) {
			if (std::find(distinct.begin(), distinct.end(), world) == distinct.end()) {
				distinct.push_back(world);
			}
		}

		// A frame with no views still runs its shared block once. A headless
		// host presenting nothing still has a world to light.
		if (distinct.empty()) {
			distinct.push_back(0);
		}

		for (size_t world = 0; world < distinct.size(); world++) {
			if (!runBlock(compiled.Shared, RunContext::WHOLE_FRAME, world)) {
				return false;
			}

			// **This world's views, immediately after its shared work.** The
			// other grouping — every world's shared block, then every view —
			// would have the second world's shadow pass overwrite the first's
			// before the first's views had sampled it.
			for (size_t view = 0; view < worlds.size(); view++) {
				if (worlds[view] != distinct[world]) {
					continue;
				}
				if (!runBlock(compiled.PerView, view, world)) {
					return false;
				}
			}
		}

		// **View outermost, node innermost**, so one view's passes are adjacent
		// in the command stream. The other nesting would interleave two views'
		// work in one buffer, which every backend allows and no profiler makes
		// legible.
		// **And the shared work that belongs at the other end.** An overlay and
		// an editor's chrome are the window's rather than a view's, so they are
		// drawn once, over whatever the views produced. Running these with the
		// block above would draw the panels once per viewport; running them with
		// the block below it would draw them under the world.
		//
		// `WHOLE_FRAME` for the same reason the first block gets it: there is no
		// view these belong to, and handing over the last one's index would
		// invite a runner to use it.
		return runBlock(compiled.Final, RunContext::WHOLE_FRAME, RunContext::WHOLE_FRAME);
	}

	RenderGraph StandardGraph() {
		RenderGraph graph;

		const ResourceId shadow = graph.AddResource({core::Name("shadow"), ResourceKind::Depth, 0, 0});
		const ResourceId surface = graph.AddResource({core::Name("surface"), ResourceKind::Colour, 0, 0});
		const ResourceId colour = graph.AddResource({core::Name("colour"), ResourceKind::Colour, 0, 0});
		const ResourceId depth = graph.AddResource({core::Name("depth"), ResourceKind::Depth, 0, 0});

		// **The window, which is not a view's colour target.** Every per-view
		// pass draws into whatever that view was given — a slot's texture for an
		// editor's panel, the swapchain for a game with one view — and the
		// overlay and the editor's chrome draw onto the window itself, once,
		// over whatever the views produced. They were one resource until the
		// three-block partition made the difference matter: a shared node and a
		// per-view node writing one resource is `SharedWriteConflict`, and it
		// fired here, correctly, on a model that said the overlay fought the
		// world for the same memory.
		const ResourceId window = graph.AddResource({core::Name("window"), ResourceKind::Colour, 0, 0});

		// **Shared, and it is the reason this type exists.** A shadow map is per
		// light: every view of one world samples the same one, so four
		// split-screen views pay for one of these rather than four.
		graph.AddNode({
			.Name = core::Name("shadow"),
			.Kind = core::Name("shadow"),
			.Reads = {},
			.Writes = {shadow},
			.PerView = false,
			.Optional = true,
		});

		graph.AddNode({
			.Name = core::Name("surface"),
			.Kind = core::Name("surface"),
			.Reads = {shadow},
			.Writes = {surface},
			.PerView = true,
			.Optional = true,
		});

		graph.AddNode({
			.Name = core::Name("opaque"),
			.Kind = core::Name("opaque"),
			.Reads = {shadow, surface},
			.Writes = {colour, depth},
			.PerView = true,
		});

		// Reads the colour it also writes, which is the load that makes a second
		// pass draw *onto* the first rather than over it.
		graph.AddNode({
			.Name = core::Name("transparent"),
			.Kind = core::Name("transparent"),
			.Reads = {colour, depth},
			.Writes = {colour},
			.PerView = true,
			.Optional = true,
		});

		graph.AddNode({
			.Name = core::Name("overlay"),
			.Kind = core::Name("overlay"),

			// Reads nothing: it composites its own texture onto the window and
			// never samples what the world drew.
			.Reads = {},
			.Writes = {window},
			.PerView = false,
			.Optional = true,
		});

		graph.AddNode({
			.Name = core::Name("interface"),
			.Kind = core::Name("interface"),

			// **Reads nothing, for the overlay's reason and one more.** Panels
			// are composited onto the window rather than sampled from it, and
			// the window is guaranteed to hold something whether or not any
			// earlier pass drew — the renderer clears it when nothing did, so
			// its initialisation is the frame's job and not a node's output.
			//
			// Declaring a read here made switching the overlay off a
			// `ReadsBeforeWrite`, which said the editor's chrome could not be
			// drawn without the debug panels. That is a modelling mistake and
			// the check was right to catch it.
			//
			// What keeps the chrome above the overlay is declaration order,
			// which is the execution order — not a dependency edge.
			.Reads = {},
			.Writes = {window},
			.PerView = false,
			.Optional = true,
		});

		return graph;
	}
}
