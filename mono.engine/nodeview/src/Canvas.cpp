#include "Widgets.hpp"

#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/nodeview/Canvas.hpp>

#include <cmath>
#include <unordered_map>
#include <vector>

namespace engine::nodeview {

	namespace {
		// The name the canvas root carries, so a rebuild can find and destroy
		// what the last one made.
		//
		// **A name rather than a handle the caller keeps.** A panel is rebuilt
		// from a document that may have been reloaded from disk, so the handle
		// it held is gone; the name survives that and is what makes `Build`
		// idempotent from a caller's point of view.
		constexpr std::string_view ROOT_NAME = "PipelineCanvas";

		// How thick an edge is drawn, in pixels.
		constexpr float EDGE_THICKNESS = 2.0f;

		// Bands in the order a frame runs them, which is the order they stack.
		constexpr graph::Band BANDS[] = {graph::Band::Shared, graph::Band::PerView, graph::Band::Final};

		// A band's colour, so the three are told apart without reading a label.
		//
		// **Shared work is the same colour at both ends.** They are one idea —
		// work every view shares — and colouring them differently would suggest
		// a distinction that does not exist; what separates them is where they
		// sit, which is already the whole of the layout.
		core::Color3 ColourOf(graph::Band band) {
			switch (band) {
			case graph::Band::Shared:
			case graph::Band::Final:
				return core::Color3{0.16f, 0.22f, 0.30f};
			case graph::Band::PerView:
				return core::Color3{0.20f, 0.26f, 0.22f};
			}
			return core::Color3{0.18f, 0.18f, 0.18f};
		}
	}

	Pick PickAt(const graph::PipelineLayout &layout, const CanvasStyle &style, float x, float y) {
		// **The same walk `Build` makes, in the same order.** Two functions
		// deriving one geometry is the drift this repository files entries
		// about — so this one is written to mirror it line for line, and the
		// tests pick at the centre of a box `Build` placed rather than at a
		// coordinate somebody computed by hand.
		float top = style.Margin;

		for (const graph::Band band : BANDS) {
			uint32_t columns = 0;
			uint32_t rows = 0;
			for (const graph::PlacedNode &placed : layout.Nodes) {
				if (placed.Where == band) {
					columns = std::max(columns, placed.Column + 1);
					rows = std::max(rows, placed.Row + 1);
				}
			}

			if (columns == 0) {
				continue;
			}

			// **The row term is unreachable today and is written anyway.**
			// `graph::LayoutPipeline` leaves every `Row` at zero because a frame
			// is a chain, so `rows` is always one and the gap multiplies out.
			// Dropping it would be correct now and wrong the first time a
			// pipeline branches — and the failure would be a hit-test that
			// selects the wrong pass, which reads as a broken editor rather
			// than as a missing term. `Build` carries the same expression.
			for (const graph::PlacedNode &placed : layout.Nodes) {
				if (placed.Where != band) {
					continue;
				}

				const float left =
					style.Margin + static_cast<float>(placed.Column) * (style.NodeWidth + style.ColumnGap);
				const float down = top + static_cast<float>(placed.Row) * (style.NodeHeight + style.RowGap);

				// Half-open, so two boxes that touch cannot both claim the
				// pixel between them.
				if (x >= left && x < left + style.NodeWidth && y >= down && y < down + style.NodeHeight) {
					return Pick{true, placed.Name};
				}
			}

			top += static_cast<float>(rows) * style.NodeHeight + static_cast<float>(rows - 1) * style.RowGap +
				   style.BandGap;
		}

		return {};
	}

	Canvas Build(
		ecs::Store &store, ecs::Entity parent, const graph::PipelineLayout &layout, const CanvasStyle &style
	) {
		Canvas canvas;

		if (parent == ecs::NULL_ENTITY || !store.Alive(parent)) {
			return canvas;
		}

		// **Registered here rather than required of the caller.** One that
		// forgot would get an empty canvas and no diagnostic, which reads as the
		// pipeline being empty rather than the classes being absent.
		gui::RegisterGuiClasses();

		// The previous canvas, if this is a rebuild. Destroyed whole: its
		// children go with it, which is what makes a rebuild one call.
		// **Collected before any of it is destroyed.** `EachChild` walks the
		// sibling links `Destroy` is about to rewrite, so destroying from inside
		// the walk is a rebuild that drops whatever came after the first match.
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

		// Each node's centre in canvas space, so an edge can be drawn between
		// two of them without walking back up the parents.
		std::unordered_map<uint32_t, std::pair<float, float>> centres;

		float top = style.Margin;

		for (const graph::Band band : BANDS) {
			// How many columns this band needs, so its frame is only as tall and
			// wide as it has to be.
			uint32_t columns = 0;
			uint32_t rows = 0;
			for (const graph::PlacedNode &placed : layout.Nodes) {
				if (placed.Where == band) {
					columns = std::max(columns, placed.Column + 1);
					rows = std::max(rows, placed.Row + 1);
				}
			}

			if (columns == 0) {
				// **A band with nothing in it is not drawn.** A frame that draws
				// no world has no per-view band, and an empty box labelled "per
				// view" would be reporting a stage that is not there.
				continue;
			}

			const float height =
				static_cast<float>(rows) * style.NodeHeight + static_cast<float>(rows - 1) * style.RowGap;

			const ecs::Entity bandFrame = MakeFrame(
				store,
				canvas.Root,
				graph::Describe(band),
				core::UDim2{0.0f, style.Margin, 0.0f, top},
				core::UDim2{
					0.0f,
					static_cast<float>(columns) * style.NodeWidth +
						static_cast<float>(columns - 1) * style.ColumnGap,
					0.0f,
					height,
				},
				core::Color3{0.12f, 0.13f, 0.16f}
			);
			canvas.Bands.push_back(bandFrame);

			for (const graph::PlacedNode &placed : layout.Nodes) {
				if (placed.Where != band) {
					continue;
				}

				const float left = static_cast<float>(placed.Column) * (style.NodeWidth + style.ColumnGap);
				const float down = static_cast<float>(placed.Row) * (style.NodeHeight + style.RowGap);

				const ecs::Entity box = MakeFrame(
					store,
					bandFrame,
					placed.Name.Text(),
					core::UDim2{0.0f, left, 0.0f, down},
					core::UDim2{0.0f, style.NodeWidth, 0.0f, style.NodeHeight},
					ColourOf(band)
				);

				centres[placed.Node.Value] = {
					style.Margin + left + style.NodeWidth * 0.5f,
					top + down + style.NodeHeight * 0.5f,
				};

				CanvasNode made;
				made.Name = placed.Name;
				made.Box = box;
				made.Label = MakeLabel(
					store, box, "Name", std::string(placed.Name.Text()), core::Color3{0.86f, 0.88f, 0.92f}
				);
				canvas.Nodes.push_back(made);
			}

			top += height + style.BandGap;
		}

		// --- the edges --------------------------------------------------------
		//
		// **Drawn from the positions this function just chose**, rather than read
		// back out of the components. A box's `Element` is its offset inside its
		// band, so recovering a canvas-space centre would mean walking back up
		// the parents and adding — which is the layout computed a second time
		// and a second chance to disagree with itself.
		for (const graph::PlacedEdge &edge : layout.Edges) {
			const auto from = centres.find(edge.From.Value);
			const auto to = centres.find(edge.To.Value);
			if (from == centres.end() || to == centres.end()) {
				continue;
			}

			const float dx = to->second.first - from->second.first;
			const float dy = to->second.second - from->second.second;
			const float length = std::sqrt(dx * dx + dy * dy);
			if (length <= 0.0f) {
				continue;
			}

			// Degrees, which is what `gui::Element::Rotation` is in, and about
			// the frame's own centre — so the line is placed by its midpoint.
			const float degrees = std::atan2(dy, dx) * 180.0f / 3.14159265358979f;

			const ecs::Entity line = MakeFrame(
				store,
				canvas.Root,
				edge.Resource.IsValid() ? edge.Resource.Text() : "edge",
				core::UDim2{
					0.0f,
					(from->second.first + to->second.first) * 0.5f - length * 0.5f,
					0.0f,
					(from->second.second + to->second.second) * 0.5f - EDGE_THICKNESS * 0.5f,
				},
				core::UDim2{0.0f, length, 0.0f, EDGE_THICKNESS},
				core::Color3{0.42f, 0.47f, 0.55f}
			);

			gui::Element *element = store.GetMutable<gui::Element>(line);
			if (element != nullptr) {
				element->Rotation = degrees;
			}
			canvas.Edges.push_back(line);
		}

		return canvas;
	}
}
