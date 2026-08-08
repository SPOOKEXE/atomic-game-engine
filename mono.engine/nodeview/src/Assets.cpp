#include <engine/nodeview/Assets.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace engine::nodeview {

	namespace {
		// Whether an operation adds a node, which is what positions count.
		bool AddsNode(bake::OperationKind kind) {
			return kind != bake::OperationKind::Connect;
		}
	}

	AssetLayout LayoutAssets(const bake::Document &document) {
		AssetLayout layout;

		for (const bake::Operation &operation : document.Operations()) {
			if (!AddsNode(operation.Kind)) {
				layout.Wires.push_back(operation);
				continue;
			}

			PlacedAsset placed;
			placed.Position = static_cast<uint32_t>(layout.Nodes.size() + 1);
			placed.Kind = bake::Describe(operation.Kind);
			placed.Text = operation.Text;
			layout.Nodes.push_back(placed);
		}

		// **Depth is one past the deepest input.** Walked in document order,
		// which is enough because `bake::Graph::Connect` refuses a wire that
		// would close a cycle and `bake::Build` resolves positions as it reaches
		// them — so a wire's source is always a node already placed.
		for (const bake::Operation &wire : layout.Wires) {
			if (wire.From == 0 || wire.From > layout.Nodes.size() || wire.To == 0 ||
				wire.To > layout.Nodes.size()) {
				// A canvas that would not draw a broken document could never
				// show somebody what was wrong with it.
				continue;
			}

			PlacedAsset &to = layout.Nodes[wire.To - 1];
			to.Column = std::max(to.Column, layout.Nodes[wire.From - 1].Column + 1);
		}

		// Stacked within a column, in document order, so two chains that never
		// meet do not draw on top of each other.
		std::vector<uint32_t> used;
		for (PlacedAsset &placed : layout.Nodes) {
			if (used.size() <= placed.Column) {
				used.resize(placed.Column + 1, 0);
			}
			placed.Row = used[placed.Column]++;

			layout.Columns = std::max(layout.Columns, placed.Column + 1);
			layout.Rows = std::max(layout.Rows, placed.Row + 1);
		}

		return layout;
	}
}
