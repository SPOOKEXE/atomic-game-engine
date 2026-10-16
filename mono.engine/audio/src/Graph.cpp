#include "Schema.hpp"

#include <engine/audio/Graph.hpp>

#include <algorithm>

namespace engine::audio {

	const char *Describe(NodeKind kind) {
		const auto *schema = detail::NodeSchemaFor(kind);
		return schema == nullptr ? "unknown" : schema->Id.c_str();
	}

	AudioGraph::AudioGraph()
		: SourceOffsets(MAXIMUM_NODES + 1), SourceCursor(MAXIMUM_NODES), DenseOrder(MAXIMUM_NODES),
		  Workspace(MAXIMUM_NODES, MAXIMUM_WIRES) {
		// Complete and validate the domain schema before this graph can reach a
		// device callback, so no first-use schema allocation occurs there.
		(void)detail::NodeSchemas();
		Ids.reserve(MAXIMUM_NODES);
		Nodes.reserve(MAXIMUM_NODES);
		Wires.reserve(MAXIMUM_WIRES);
		Sources.reserve(MAXIMUM_WIRES);
		DenseIds.reserve(MAXIMUM_NODES);
		Dependencies.reserve(MAXIMUM_WIRES);
		Sorted.reserve(MAXIMUM_NODES);
		// The output exists from the start rather than being added, so there is
		// never a graph in which "where does the device read from" has no
		// answer.
		Sink = NodeId{.Value = OUTPUT_ID};
		NextNode = FIRST_FREE_ID;
		Ids.push_back(Sink);
		Nodes.push_back(Node{.Kind = NodeKind::Output});
	}

	AudioGraph::AudioGraph(const AudioGraph &other) : AudioGraph() {
		*this = other;
	}

	AudioGraph &AudioGraph::operator=(const AudioGraph &other) {
		if (this == &other) return *this;
		Ids.assign(other.Ids.begin(), other.Ids.end());
		Nodes.assign(other.Nodes.begin(), other.Nodes.end());
		Wires.assign(other.Wires.begin(), other.Wires.end());
		Sink = other.Sink;
		NextNode = other.NextNode;
		Ear = other.Ear;
		OrderStale = true;
		return *this;
	}

	AudioGraph::AudioGraph(AudioGraph &&other) : AudioGraph() {
		Swap(other);
	}

	AudioGraph &AudioGraph::operator=(AudioGraph &&other) noexcept {
		if (this != &other) Swap(other);
		return *this;
	}

	void AudioGraph::Swap(AudioGraph &other) noexcept {
		using std::swap;
		swap(Ids, other.Ids);
		swap(Nodes, other.Nodes);
		swap(Wires, other.Wires);
		swap(Sources, other.Sources);
		swap(SourceOffsets, other.SourceOffsets);
		swap(SourceCursor, other.SourceCursor);
		swap(DenseIds, other.DenseIds);
		swap(Dependencies, other.Dependencies);
		swap(DenseOrder, other.DenseOrder);
		swap(Workspace, other.Workspace);
		swap(Sink, other.Sink);
		swap(NextNode, other.NextNode);
		swap(Ear, other.Ear);
		swap(Sorted, other.Sorted);
		swap(OrderStale, other.OrderStale);
	}

	size_t AudioGraph::IndexOf(NodeId id) const {
		for (size_t index = 0; index < Ids.size(); ++index) {
			if (Ids[index] == id) {
				return index;
			}
		}
		return Ids.size();
	}

	NodeId AudioGraph::Add(NodeKind kind) {
		if (kind == NodeKind::Output || detail::NodeSchemaFor(kind) == nullptr) {
			// The output is constructor-owned, and an undeclared kind has no
			// routing schema to enter the graph with.
			return {};
		}
		if (Nodes.size() >= MAXIMUM_NODES) {
			return {};
		}

		const NodeId id{.Value = NextNode++};
		Ids.push_back(id);
		Nodes.push_back(Node{.Kind = kind});
		OrderStale = true;
		return id;
	}

	bool AudioGraph::Adopt(NodeId id, NodeKind kind) {
		if (!id.IsValid() || kind == NodeKind::Output || detail::NodeSchemaFor(kind) == nullptr) {
			return false;
		}
		if (IndexOf(id) != Ids.size()) {
			// Already here. True rather than false: a command replayed or
			// posted twice should leave the graph in the state it asked for
			// rather than reporting a failure nobody can act on.
			return true;
		}
		if (Nodes.size() >= MAXIMUM_NODES) {
			return false;
		}

		Ids.push_back(id);
		Nodes.push_back(Node{.Kind = kind});
		// So a later `Add` on this graph cannot mint an id somebody else's
		// counter has already handed out.
		NextNode = std::max(NextNode, id.Value + 1);
		OrderStale = true;
		return true;
	}

	bool AudioGraph::Remove(NodeId id) {
		if (id == Sink) {
			return false;
		}
		const size_t index = IndexOf(id);
		if (index == Ids.size()) {
			return false;
		}

		// Wires go with the node. One left dangling is a lookup that fails once
		// per block, for ever.
		Wires.erase(
			std::remove_if(
				Wires.begin(),
				Wires.end(),
				[id](const Wire &wire) { return wire.From == id || wire.To == id; }
			),
			Wires.end()
		);

		Ids.erase(Ids.begin() + static_cast<ptrdiff_t>(index));
		Nodes.erase(Nodes.begin() + static_cast<ptrdiff_t>(index));
		OrderStale = true;
		return true;
	}

	Node *AudioGraph::Find(NodeId id) {
		const size_t index = IndexOf(id);
		return index == Ids.size() ? nullptr : &Nodes[index];
	}

	const Node *AudioGraph::Find(NodeId id) const {
		const size_t index = IndexOf(id);
		return index == Ids.size() ? nullptr : &Nodes[index];
	}

	bool AudioGraph::PrepareDependencies() const {
		DenseIds.resize(Ids.size());
		for (size_t index = 0; index < Ids.size(); ++index)
			DenseIds[index] = {Ids[index].Value, static_cast<graphcore::NodeIndex>(index)};
		std::sort(DenseIds.begin(), DenseIds.end());
		auto denseIndex = [&](NodeId id) {
			const auto found = std::lower_bound(
				DenseIds.begin(), DenseIds.end(), id.Value, [](const auto &entry, uint32_t value) {
					return entry.first < value;
				}
			);
			return found != DenseIds.end() && found->first == id.Value ? found->second : graphcore::NO_NODE;
		};
		Dependencies.clear();
		for (const Wire &wire : Wires) {
			const auto from = denseIndex(wire.From);
			const auto to = denseIndex(wire.To);
			if (from == graphcore::NO_NODE || to == graphcore::NO_NODE) return false;
			if (detail::RouteFor(Nodes[from].Kind, Nodes[to].Kind).Input == nullptr) return false;
			Dependencies.push_back({from, to});
		}
		return true;
	}

	bool AudioGraph::Connect(NodeId from, NodeId to) {
		if (from == to) {
			return false;
		}
		const size_t fromIndex = IndexOf(from);
		const size_t toIndex = IndexOf(to);
		if (fromIndex == Ids.size() || toIndex == Ids.size()) {
			return false;
		}
		const detail::RoutingPorts route = detail::RouteFor(Nodes[fromIndex].Kind, Nodes[toIndex].Kind);
		if (route.Input == nullptr) return false;
		if (Connected(from, to)) {
			// A caller rebuilding a routing should not have to diff it first.
			return true;
		}
		if (route.Input->MaximumConnections != 0) {
			size_t inputs = 0;
			for (const Wire &wire : Wires)
				inputs += wire.To == to;
			if (inputs >= route.Input->MaximumConnections) return false;
		}
		if (Wires.size() == MAXIMUM_WIRES || !PrepareDependencies()) return false;
		Dependencies.push_back(
			{static_cast<graphcore::NodeIndex>(fromIndex), static_cast<graphcore::NodeIndex>(toIndex)}
		);
		const graphcore::Limits limits{.MaximumNodes = MAXIMUM_NODES, .MaximumConnections = MAXIMUM_WIRES};
		const auto result = graphcore::CompileOrder(Ids.size(), Dependencies, limits, Workspace, DenseOrder);
		if (result.Status != graphcore::CompileStatus::Ok) return false;

		Wires.push_back(Wire{.From = from, .To = to});
		OrderStale = true;
		return true;
	}

	bool AudioGraph::Disconnect(NodeId from, NodeId to) {
		const auto found = std::find_if(Wires.begin(), Wires.end(), [from, to](const Wire &wire) {
			return wire.From == from && wire.To == to;
		});
		if (found == Wires.end()) {
			return false;
		}
		Wires.erase(found);
		OrderStale = true;
		return true;
	}

	bool AudioGraph::Connected(NodeId from, NodeId to) const {
		return std::any_of(Wires.begin(), Wires.end(), [from, to](const Wire &wire) {
			return wire.From == from && wire.To == to;
		});
	}

	std::span<const NodeId> AudioGraph::InputsOf(NodeId id) const {
		if (OrderStale && !Rebuild()) return {};
		const size_t index = IndexOf(id);
		if (index == Ids.size()) return {};
		return std::span<const NodeId>(Sources).subspan(
			SourceOffsets[index], SourceOffsets[index + 1] - SourceOffsets[index]
		);
	}

	bool AudioGraph::Rebuild() const {
		if (!PrepareDependencies()) return false;
		const graphcore::Limits limits{.MaximumNodes = MAXIMUM_NODES, .MaximumConnections = MAXIMUM_WIRES};
		const auto result = graphcore::CompileOrder(Ids.size(), Dependencies, limits, Workspace, DenseOrder);
		if (result.Status != graphcore::CompileStatus::Ok) return false;

		std::fill_n(SourceOffsets.begin(), Ids.size() + 1, size_t{0});
		for (const auto &edge : Dependencies)
			++SourceOffsets[edge.After + 1];
		for (size_t index = 0; index < Ids.size(); ++index)
			SourceOffsets[index + 1] += SourceOffsets[index];
		std::copy_n(SourceOffsets.begin(), Ids.size(), SourceCursor.begin());
		Sources.resize(Wires.size());
		for (size_t index = 0; index < Wires.size(); ++index)
			Sources[SourceCursor[Dependencies[index].After]++] = Wires[index].From;
		Sorted.resize(Ids.size());
		for (size_t index = 0; index < Ids.size(); ++index)
			Sorted[index] = Ids[DenseOrder[index]];
		OrderStale = false;
		return true;
	}

	std::span<const NodeId> AudioGraph::Order() const {
		if (OrderStale && !Rebuild()) return {};
		return Sorted;
	}
}
