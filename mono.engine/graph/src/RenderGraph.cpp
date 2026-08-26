#include <engine/core/Log.hpp>
#include <engine/graph/RenderGraph.hpp>

#include <algorithm>
#include <cstdlib>
#include <span>
#include <unordered_map>
#include <unordered_set>

namespace engine::graph {
	const char *Describe(ResourceKind kind) {
		switch (kind) {
		case ResourceKind::Colour:
			return "colour";
		case ResourceKind::Depth:
			return "depth";
		case ResourceKind::Texture:
			return "texture";
		case ResourceKind::Storage:
			return "storage";
		case ResourceKind::Buffer:
			return "buffer";
		case ResourceKind::Camera:
			return "camera";
		case ResourceKind::Entities:
			return "entities";
		}
		return "unknown";
	}

	const char *Describe(NodeScope scope) {
		switch (scope) {
		case NodeScope::Frame:
			return "frame";
		case NodeScope::World:
			return "world";
		case NodeScope::View:
			return "view";
		}
		return "?";
	}

	bool RunsPerView(NodeScope scope) {
		return scope == NodeScope::View;
	}

	const char *Describe(ResourceFormat format) {
		switch (format) {
		case ResourceFormat::R8:
			return "R8";
		case ResourceFormat::RG8:
			return "RG8";
		case ResourceFormat::RGBA8:
			return "RGBA8";
		case ResourceFormat::RGBA8_SRGB:
			return "RGBA8_SRGB";
		case ResourceFormat::RGB10A2:
			return "RGB10A2";
		case ResourceFormat::RG11B10F:
			return "RG11B10F";
		case ResourceFormat::R16F:
			return "R16F";
		case ResourceFormat::RG16F:
			return "RG16F";
		case ResourceFormat::RGBA16F:
			return "RGBA16F";
		case ResourceFormat::R32F:
			return "R32F";
		case ResourceFormat::RG32F:
			return "RG32F";
		case ResourceFormat::D24S8:
			return "D24S8";
		case ResourceFormat::D32F:
			return "D32F";
		case ResourceFormat::BC1_SRGB:
			return "BC1_SRGB";
		case ResourceFormat::BC3:
			return "BC3";
		case ResourceFormat::BC5:
			return "BC5";
		case ResourceFormat::BC7_SRGB:
			return "BC7_SRGB";
		}
		return "?";
	}

	uint32_t BitsPerPixel(ResourceFormat format) {
		switch (format) {
		case ResourceFormat::BC1_SRGB:
			// Eight bytes a 4x4 block.
			return 4;
		case ResourceFormat::BC3:
		case ResourceFormat::BC5:
		case ResourceFormat::BC7_SRGB:
			// Sixteen bytes a 4x4 block. **This is the number that makes a BC5
			// normal map expensive** - `PIPELINE_NODES.md` §1.4 notes a single
			// one costing more than the alternatives, and it costs twice what a
			// BC1 does for the same pixels.
			return 8;
		case ResourceFormat::R8:
			return 8;
		case ResourceFormat::RG8:
		case ResourceFormat::R16F:
			return 16;
		case ResourceFormat::RGBA8:
		case ResourceFormat::RGBA8_SRGB:
		case ResourceFormat::RGB10A2:
		case ResourceFormat::RG11B10F:
		case ResourceFormat::RG16F:
		case ResourceFormat::R32F:
		case ResourceFormat::D24S8:
		case ResourceFormat::D32F:
			return 32;
		case ResourceFormat::RGBA16F:
		case ResourceFormat::RG32F:
			return 64;
		}
		return 32;
	}

	uint32_t ChannelCount(ResourceFormat format) {
		switch (format) {
		case ResourceFormat::R8:
		case ResourceFormat::R16F:
		case ResourceFormat::R32F:
		case ResourceFormat::D24S8:
		case ResourceFormat::D32F:
			return 1;
		case ResourceFormat::RG8:
		case ResourceFormat::RG16F:
		case ResourceFormat::RG32F:
		case ResourceFormat::BC5:
			return 2;
		case ResourceFormat::RG11B10F:
		case ResourceFormat::BC1_SRGB:
			return 3;
		case ResourceFormat::RGBA8:
		case ResourceFormat::RGBA8_SRGB:
		case ResourceFormat::RGB10A2:
		case ResourceFormat::RGBA16F:
		case ResourceFormat::BC3:
		case ResourceFormat::BC7_SRGB:
			return 4;
		}
		return 4;
	}

	bool HasAlpha(ResourceFormat format) {
		return ChannelCount(format) == 4;
	}

	void ResourceDesc::Resolve(
		uint32_t viewWidth, uint32_t viewHeight, uint32_t &outWidth, uint32_t &outHeight
	) const {
		if (Width != 0 && Height != 0) {
			outWidth = Width;
			outHeight = Height;
			return;
		}

		// **Zero reads as one**, so a resource written before `Divisor` existed
		// still means "the view" rather than dividing by nothing.
		const uint32_t by = Divisor == 0 ? 1u : Divisor;
		outWidth = std::max(1u, viewWidth / by);
		outHeight = std::max(1u, viewHeight / by);
	}

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
			return "a node neither reads nor writes";
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
				// The caller gets an invalid id and usually reports only the
				// name it asked for, so the reason it was refused is here.
				ENGINE_WARN("resource '{}' is already declared; refusing the second one", desc.Name.Text());
				return {};
			}
		}

		Resources.push_back(desc);
		return ResourceId{static_cast<uint32_t>(Resources.size())};
	}

	NodeId RenderGraph::AddNode(Node node) {
		if (!node.Name.IsValid() || Nodes.size() >= MAXIMUM_NODES) {
			// Two causes, one invalid id. A pipeline that quietly loses its last
			// pass at the cap looks nothing like one with a blank name in it.
			ENGINE_WARN(
				"node refused: {}", node.Name.IsValid() ? "the graph is at its node cap" : "the name is empty"
			);
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

	const std::string *Node::Parameter(core::Name key) const {
		for (const NodeParameter &parameter : Parameters) {
			if (parameter.Key == key) {
				return &parameter.Value;
			}
		}
		return nullptr;
	}

	float Node::Number(core::Name key, float fallback) const {
		const std::string *text = Parameter(key);
		if (text == nullptr) {
			return fallback;
		}

		// **`strtof` and not `stof`.** The second throws on a half-typed number,
		// and a half-typed number in an editor is a state somebody is passing
		// through rather than a pipeline to reject.
		char *end = nullptr;
		const float value = std::strtof(text->c_str(), &end);
		return end == text->c_str() ? fallback : value;
	}

	uint32_t Node::Integer(core::Name key, uint32_t fallback) const {
		const std::string *text = Parameter(key);
		if (text == nullptr) {
			return fallback;
		}

		// Base zero, so `0x1f` and `31` both read - a tag mask is written in hex
		// far more often than in decimal.
		char *end = nullptr;
		const unsigned long value = std::strtoul(text->c_str(), &end, 0);
		return end == text->c_str() ? fallback : static_cast<uint32_t>(value);
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
			// **A sink is not a pointless node.** `viewer` and `capture` write
			// nothing by definition - their whole purpose is to consume, and
			// what they produce is a panel or a file, which is outside the
			// graph. Refusing every node with no writes made both of them
			// catalogue entries that could never be placed, which nothing
			// noticed until one was.
			//
			// What is still a fault is a node that neither reads nor writes:
			// that one cannot affect the frame and cannot be observed either.
			if (node.Writes.empty() && node.Reads.empty()) {
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
				(RunsPerView(node.Scope) ? producer.ByPerView : producer.ByShared) = true;
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
		// check it rather than derive it.** A general dependency sort cannot
		// recover the authored order of read-modify-write passes.
		//
		// It is also the only model that survives a read-modify-write chain.
		// `opaque` writes colour, `transparent` draws onto it, `overlay` onto
		// that, `interface` onto that - every one of them both reads and writes
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
			if (RunsPerView(Nodes[index].Scope)) {
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
					ENGINE_WARN(
						"'{}' is shared but sits between per-view nodes; the three blocks cannot express "
						"that",
						offender.Text()
					);
					out.Shared.clear();
					out.PerView.clear();
					out.Final.clear();
					return GraphStatus::SharedBetweenViews;
				}
			}
		}

		std::unordered_set<uint32_t> written;
		for (size_t index = 0; index < Resources.size(); index++) {
			if (Resources[index].External) {
				written.insert(static_cast<uint32_t>(index + 1));
			}
		}
		const auto walk = [&](const std::vector<NodeId> &block) {
			for (const NodeId id : block) {
				const Node *node = Find(id);

				// Reads are checked before this node's own writes are recorded,
				// which is what makes a self read-modify-write an error unless
				// something earlier produced the resource - `transparent` is
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

		// **Shared first, and that ordering is the whole partition.** Every
		// shared node produces something the per-view nodes may read - a shadow
		// map is the case this was built for - so running them after would have
		// each view sampling a target written for the frame after it.
		// **Distinct worlds, in first-appearance order.** Views of one world do
		// not have to be adjacent, so this is a scan rather than a run-length
		// walk - and it keeps the shared work in the order the caller listed its
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
			bool shared = true;

			// **This world's views, immediately after its shared work.** The
			// other grouping - every world's shared block, then every view -
			// would have the second world's shadow pass overwrite the first's
			// before the first's views had sampled it.
			for (size_t view = 0; view < worlds.size(); view++) {
				if (worlds[view] != distinct[world]) {
					continue;
				}
				if (!ExecuteView(compiled, runner, view, world, shared)) {
					return false;
				}
				shared = false;
			}

			// A world with no views still runs its shared work: a headless host
			// presenting nothing still has a world to light.
			if (shared && !ExecuteView(compiled, runner, RunContext::WHOLE_FRAME, world, true)) {
				return false;
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
		return ExecuteFinal(compiled, runner);
	}

	bool RenderGraph::ExecuteView(
		const CompiledGraph &compiled, NodeRunner &runner, size_t view, size_t world, bool shared
	) const {
		// **Shared first, and that ordering is the whole partition.** Every
		// shared node produces something the per-view nodes may read - a shadow
		// map is the case this was built for - so running them after would have
		// each view sampling a target written for the frame after it.
		if (shared && !RunBlock(compiled.Shared, runner, RunContext::WHOLE_FRAME, world)) {
			return false;
		}

		if (view == RunContext::WHOLE_FRAME) {
			return true;
		}
		return RunBlock(compiled.PerView, runner, view, world);
	}

	bool RenderGraph::ExecuteFinal(const CompiledGraph &compiled, NodeRunner &runner) const {
		// `WHOLE_FRAME` for both, for the reason the shared block gets it for
		// the view: there is no view or world these belong to, and handing over
		// the last one's index would invite a runner to use it.
		return RunBlock(compiled.Final, runner, RunContext::WHOLE_FRAME, RunContext::WHOLE_FRAME);
	}

	bool RenderGraph::RunBlock(
		const std::vector<NodeId> &block, NodeRunner &runner, size_t view, size_t world
	) const {
		for (const NodeId id : block) {
			const Node *node = Find(id);
			if (node == nullptr) {
				// A compiled block naming a node the graph no longer holds. The
				// pass is dropped from every frame and the picture is simply
				// missing whatever it drew.
				ENGINE_WARN_EVERY(
					5.0, "compiled node {} is gone from the graph; the pass is skipped", id.Value
				);
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
	}

}
