#include <engine/audio/Graph.hpp>

#include <algorithm>

namespace engine::audio {

	const char *Describe(NodeKind kind) {
		switch (kind) {
		case NodeKind::Player:
			return "player";
		case NodeKind::Fader:
			return "fader";
		case NodeKind::Emitter:
			return "emitter";
		case NodeKind::Bus:
			return "bus";
		case NodeKind::Output:
			return "output";
		}
		return "unknown";
	}

	AudioGraph::AudioGraph() {
		// The output exists from the start rather than being added, so there is
		// never a graph in which "where does the device read from" has no
		// answer.
		Sink = NodeId{.Value = OUTPUT_ID};
		NextNode = FIRST_FREE_ID;
		Ids.push_back(Sink);
		Nodes.push_back(Node{.Kind = NodeKind::Output});
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
		if (kind == NodeKind::Output) {
			// Two outputs is a graph with no answer to "what does the device
			// play". The one that exists was made by the constructor.
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
		if (!id.IsValid() || kind == NodeKind::Output) {
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

	bool AudioGraph::CanReach(NodeId start, NodeId target) const {
		// Depth-first *forwards* from `start`, looking for `target`. Iterative
		// rather than recursive: this runs against a graph somebody is
		// building, and a deep chain should not be a stack overflow.
		//
		// **The direction is the whole check and it is easy to invert**, which
		// is why the parameters are named for what the walk does rather than
		// for the wire being considered. Written the other way round it
		// refuses every shortcut across a chain and admits every loop, which
		// is exactly backwards and reads plausibly either way.
		std::vector<NodeId> pending{start};
		std::vector<NodeId> seen;

		while (!pending.empty()) {
			const NodeId at = pending.back();
			pending.pop_back();
			if (at == target) {
				return true;
			}
			if (std::find(seen.begin(), seen.end(), at) != seen.end()) {
				continue;
			}
			seen.push_back(at);

			for (const Wire &wire : Wires) {
				if (wire.From == at) {
					pending.push_back(wire.To);
				}
			}
		}
		return false;
	}

	bool AudioGraph::Connect(NodeId from, NodeId to) {
		if (from == to) {
			return false;
		}
		if (IndexOf(from) == Ids.size() || IndexOf(to) == Ids.size()) {
			return false;
		}
		if (from == Sink) {
			// Nothing is downstream of the output.
			return false;
		}
		if (Connected(from, to)) {
			// A caller rebuilding a routing should not have to diff it first.
			return true;
		}
		// A wire `from -> to` closes a loop exactly when `to` can already reach
		// `from` by following wires forwards.
		if (CanReach(to, from)) {
			// Refused here, where it costs a walk, rather than discovered on
			// the device thread where it is an infinite recursion or unbounded
			// gain.
			return false;
		}

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
		if (OrderStale) {
			Rebuild();
		}
		const size_t index = IndexOf(id);
		if (index >= Sources.size()) {
			return {};
		}
		return Sources[index];
	}

	void AudioGraph::Rebuild() const {
		Sorted.clear();
		Sorted.reserve(Ids.size());

		// The adjacency list, rebuilt with the order because both are derived
		// from the same wires and go stale together.
		Sources.assign(Ids.size(), {});
		for (const Wire &wire : Wires) {
			const size_t target = IndexOf(wire.To);
			if (target < Sources.size()) {
				Sources[target].push_back(wire.From);
			}
		}

		// Kahn's algorithm over in-degree. Deterministic because the ready set
		// is walked in node order rather than popped from a stack - two runs of
		// one graph produce one order, which is what makes a mix reproducible.
		std::vector<size_t> remaining(Ids.size(), 0);
		for (const Wire &wire : Wires) {
			const size_t target = IndexOf(wire.To);
			if (target < remaining.size()) {
				++remaining[target];
			}
		}

		std::vector<bool> emitted(Ids.size(), false);
		bool progressed = true;
		while (progressed) {
			progressed = false;
			for (size_t index = 0; index < Ids.size(); ++index) {
				if (emitted[index] || remaining[index] != 0) {
					continue;
				}
				emitted[index] = true;
				Sorted.push_back(Ids[index]);
				progressed = true;

				for (const Wire &wire : Wires) {
					if (wire.From != Ids[index]) {
						continue;
					}
					const size_t target = IndexOf(wire.To);
					if (target < remaining.size() && remaining[target] > 0) {
						--remaining[target];
					}
				}
			}
		}

		// Anything left is in a cycle, which `Connect` refuses - so this cannot
		// happen. Appending rather than dropping is the safe answer if it ever
		// does: a node missing from the order silently stops advancing, which
		// is far harder to find than one mixed in the wrong order.
		for (size_t index = 0; index < Ids.size(); ++index) {
			if (!emitted[index]) {
				Sorted.push_back(Ids[index]);
			}
		}

		OrderStale = false;
	}

	std::span<const NodeId> AudioGraph::Order() const {
		if (OrderStale) {
			Rebuild();
		}
		return Sorted;
	}
}
