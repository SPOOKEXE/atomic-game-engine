#include "Widgets.hpp"

#include <engine/nodeview/Assets.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace engine::nodeview {

	namespace {
		constexpr std::string_view ROOT_NAME = "AssetsCanvas";

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

	Canvas
	BuildAssets(ecs::Store &store, ecs::Entity parent, const AssetLayout &layout, const CanvasStyle &style) {
		Canvas canvas;

		if (parent == ecs::NULL_ENTITY || !store.Alive(parent)) {
			return canvas;
		}

		gui::RegisterGuiClasses();

		// Collected before any of it is destroyed, for `Canvas.cpp`'s reason:
		// `EachChild` walks the sibling links `Destroy` rewrites.
		std::vector<ecs::Entity> stale;
		store.EachChild(parent, [&store, &stale](ecs::Entity child) {
			if (store.InstanceNameOf(child) == core::Name(ROOT_NAME)) {
				stale.push_back(child);
			}
		});
		for (const ecs::Entity child : stale) {
			store.Destroy(child);
		}

		canvas.Root = MakeFrame(
			store,
			parent,
			ROOT_NAME,
			core::UDim2{0.0f, 0.0f, 0.0f, 0.0f},
			core::UDim2{1.0f, 0.0f, 1.0f, 0.0f},
			core::Color3{0.09f, 0.10f, 0.12f}
		);

		// **No bands.** A render frame has three because a frame does; a bake
		// chain has none, and inventing one to hold everything would be a box
		// that means nothing.
		for (const PlacedAsset &placed : layout.Nodes) {
			const float left =
				style.Margin + static_cast<float>(placed.Column) * (style.NodeWidth + style.ColumnGap);
			const float down =
				style.Margin + static_cast<float>(placed.Row) * (style.NodeHeight + style.RowGap);

			// Named by position rather than by kind, because a chain may hold
			// four `import` nodes and an instance name is how a panel finds the
			// one somebody clicked.
			const std::string name = std::to_string(placed.Position);

			const ecs::Entity box = MakeFrame(
				store,
				canvas.Root,
				name,
				core::UDim2{0.0f, left, 0.0f, down},
				core::UDim2{0.0f, style.NodeWidth, 0.0f, style.NodeHeight},
				core::Color3{0.24f, 0.20f, 0.28f}
			);

			// The kind, and the name under it when there is one — a `source`
			// box that did not say which file it read would be six identical
			// boxes in a directory bake.
			std::string text{placed.Kind};
			if (!placed.Text.empty()) {
				text += "\n";
				text += placed.Text;
			}

			CanvasNode made;
			made.Name = core::Name(name);
			made.Box = box;
			made.Label = MakeLabel(store, box, "Name", std::move(text), core::Color3{0.88f, 0.86f, 0.92f});
			canvas.Nodes.push_back(made);
		}

		return canvas;
	}
}
