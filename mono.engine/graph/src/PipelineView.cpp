#include <engine/graph/PipelineView.hpp>

#include <unordered_map>

namespace engine::graph {

	const char *Describe(Band band) {
		switch (band) {
		case Band::Shared:
			return "shared";
		case Band::PerView:
			return "per view";
		case Band::Final:
			return "final";
		}
		return "unknown";
	}

	PipelineLayout LayoutPipeline(const RenderGraph &graph, const CompiledGraph &compiled) {
		PipelineLayout layout;

		// **Walked in execution order, which is the order a reader reasons in.**
		// Shared, then one view's worth, then the window's - the same sequence
		// `Execute` runs and the same one a profiler's trace shows, so a canvas
		// and a capture can be read side by side.
		const auto place = [&layout, &graph](const std::vector<NodeId> &block, Band band) {
			uint32_t column = 0;
			for (const NodeId id : block) {
				const Node *node = graph.Find(id);
				if (node == nullptr) {
					continue;
				}

				PlacedNode placed;
				placed.Node = id;
				placed.Name = node->Name;
				placed.Where = band;
				placed.Column = column++;
				layout.Nodes.push_back(placed);
			}
			layout.Columns = std::max(layout.Columns, column);
		};

		place(compiled.Shared, Band::Shared);
		place(compiled.PerView, Band::PerView);
		place(compiled.Final, Band::Final);

		// **The last writer of each resource, updated as the walk proceeds.**
		// That is what makes an edge mean "this is where the data you read came
		// from" rather than "you both mention the same name": when `overlay`
		// reads colour after both `opaque` and `transparent` wrote it, the line
		// comes from `transparent`, because that is what it will see.
		//
		// Reads are resolved before this node's own writes are recorded, which
		// is what keeps a read-modify-write node - `transparent` reads the
		// colour it also writes - joined to the pass in front of it rather than
		// to itself.
		std::unordered_map<uint32_t, NodeId> lastWriter;

		for (const PlacedNode &placed : layout.Nodes) {
			const Node *node = graph.Find(placed.Node);
			if (node == nullptr) {
				continue;
			}

			for (const ResourceId resource : node->Reads) {
				const auto found = lastWriter.find(resource.Value);
				if (found == lastWriter.end()) {
					// Nothing wrote it. `Compile` refuses that as
					// `ReadsBeforeWrite`, so a layout only sees it when a caller
					// laid out a compile it did not check - drawn as no edge
					// rather than as an edge from nowhere.
					continue;
				}

				PlacedEdge edge;
				edge.From = found->second;
				edge.To = placed.Node;

				const ResourceDesc *desc = graph.FindResource(resource);
				edge.Resource = desc != nullptr ? desc->Name : core::Name{};
				layout.Edges.push_back(edge);
			}

			for (const ResourceId resource : node->Writes) {
				lastWriter[resource.Value] = placed.Node;
			}
		}

		return layout;
	}
}
