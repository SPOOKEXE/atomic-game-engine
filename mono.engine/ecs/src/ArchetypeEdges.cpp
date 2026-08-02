#include "ArchetypeEdges.hpp"

namespace engine::ecs {

	uint32_t ArchetypeEdges::Lookup(const std::vector<Edge> &edges, uint32_t component) {
		for (const Edge &edge : edges) {
			if (edge.Component == component) {
				return edge.Destination;
			}
		}
		return NO_TABLE;
	}

	void ArchetypeEdges::Record(std::vector<Edge> &edges, uint32_t component, uint32_t destination) {
		for (Edge &edge : edges) {
			if (edge.Component == component) {
				// Already known. Overwritten rather than skipped: the two can
				// only differ if the epoch moved, and in that case the newer
				// answer is the correct one.
				edge.Destination = destination;
				return;
			}
		}
		edges.push_back(Edge{component, destination});
	}

	void ArchetypeEdges::Reconcile(uint64_t epoch) {
		if (epoch == Epoch) {
			return;
		}

		// Observing a component changed which tables carry `DirtyBits`, so an
		// edge recorded before it may now point at a table that does not track
		// changes. Rebuilt on demand rather than repaired, because there are
		// only ever a handful of these and a repair pass would have to redo the
		// interning it exists to avoid.
		ByTable.clear();
		Epoch = epoch;
	}

	ArchetypeEdges::Node &ArchetypeEdges::Reach(uint32_t table) {
		if (table >= ByTable.size()) {
			ByTable.resize(static_cast<size_t>(table) + 1);
		}
		return ByTable[table];
	}

	uint32_t ArchetypeEdges::Added(uint64_t epoch, uint32_t from, ComponentId component) {
		Reconcile(epoch);
		if (from >= ByTable.size()) {
			return NO_TABLE;
		}
		return Lookup(ByTable[from].Additions, component.Index);
	}

	uint32_t ArchetypeEdges::Removed(uint64_t epoch, uint32_t from, ComponentId component) {
		Reconcile(epoch);
		if (from >= ByTable.size()) {
			return NO_TABLE;
		}
		return Lookup(ByTable[from].Removals, component.Index);
	}

	void ArchetypeEdges::RecordAddition(
		uint64_t epoch, uint32_t from, ComponentId component, uint32_t destination
	) {
		Reconcile(epoch);
		Record(Reach(from).Additions, component.Index, destination);
	}

	void ArchetypeEdges::RecordRemoval(
		uint64_t epoch, uint32_t from, ComponentId component, uint32_t destination
	) {
		Reconcile(epoch);
		Record(Reach(from).Removals, component.Index, destination);
	}

	void ArchetypeEdges::Forget() {
		ByTable.clear();
	}

	size_t ArchetypeEdges::Count() const {
		size_t total = 0;
		for (const Node &node : ByTable) {
			total += node.Additions.size() + node.Removals.size();
		}
		return total;
	}
}
