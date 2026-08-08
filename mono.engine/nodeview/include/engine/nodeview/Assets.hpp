#pragma once

// A bake pipeline drawn as a node canvas — the Assets Pipeline widget's tree.
//
// **The mirror of `Canvas.hpp`, and the differences are the interesting part.**
// A render pipeline is a chain: `RenderGraph` has no wires, its order is
// declaration order, and `graph::PipelineView` has to *derive* what joins two
// boxes. A bake pipeline is a real tree — `bake::Graph::Connect` wires an
// output into an input — so its edges are given rather than inferred, and its
// nodes genuinely branch: one source feeds an import, which feeds a fit and a
// scale, which feed two writes.
//
// So this lays out by **depth**, which is what a branching graph wants: a node
// sits one column right of the furthest-right thing feeding it, and nodes at
// the same depth stack. That is the layout every node editor uses and it is
// what `graph::PlacedNode::Row` was put there for — the render canvas has never
// needed a second row and this one does on its first realistic pipeline.
//
// v0.11 asks for "many node trees in one editor", so a document holding several
// disconnected chains is the ordinary case rather than a corner: each root
// starts its own depth-0 column and they stack down the canvas.
//
// @tier L10 · shared

#include <engine/bake/GraphDocument.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/nodeview/Canvas.hpp>

#include <cstdint>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::nodeview {

	// One bake node, placed.
	//
	// @since v0.11
	struct PlacedAsset {
		// Its position among the document's node operations, one-based — the
		// same numbering `bake::Operation::From` and `To` use, so a panel can
		// turn a click into an edit without a second mapping.
		uint32_t Position = 0;

		// What it is, for the label: `source`, `import`, `fit` and so on.
		std::string_view Kind;

		// Its name, for the ones that carry one — a source's file, a write's
		// asset. Empty otherwise.
		std::string_view Text;

		// How far along the chain it sits, and how far down when several share
		// a depth.
		//@{
		uint32_t Column = 0;
		uint32_t Row = 0;
		//@}
	};

	// A bake document, placed.
	//
	// @since v0.11
	struct AssetLayout {
		// Every node operation, in document order.
		std::vector<PlacedAsset> Nodes;

		// One entry per wire, as positions into `Nodes`' numbering.
		std::vector<bake::Operation> Wires;

		// How wide and tall the canvas needs to be.
		//@{
		uint32_t Columns = 0;
		uint32_t Rows = 0;
		//@}
	};

	// Works out where a bake document's nodes go.
	//
	// **Depth is one past the deepest input**, so a node is always right of
	// everything feeding it and a wire never points backwards. A wire naming a
	// position the document does not hold is ignored rather than refused —
	// `bake::Build` is what refuses a broken document, and a canvas that would
	// not draw one could never show somebody what was wrong with it.
	//
	// @param document The pipeline.
	// @return The layout.
	AssetLayout LayoutAssets(const bake::Document &document);

	// Builds an Assets Pipeline canvas under `parent`.
	//
	// Destroys whatever it built there before, exactly as `nodeview::Build`
	// does, and for the same reason.
	//
	// @param store  The world the instances live in.
	// @param parent What to hang the canvas under. An invalid handle builds
	//               nothing.
	// @param layout Where the nodes go.
	// @param style  Spacing.
	// @return What was built.
	Canvas BuildAssets(
		ecs::Store &store, ecs::Entity parent, const AssetLayout &layout, const CanvasStyle &style = {}
	);
}
