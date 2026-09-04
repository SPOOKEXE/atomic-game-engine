#pragma once

// In-memory bake graph. Nodes consume at most one input and cycles are rejected
// when connected. Filesystem access stays outside the graph.
//
// **The vocabulary is `Engine::bakegraph`'s and the runtime is this.** `NodeKind`
// and `NodeId` moved out at v0.13 so that `Engine::game` can read a pipeline
// document without linking the PNG, JPEG, glTF and PMX readers this module
// carries - `D00102`. What is left here is everything that touches a payload:
// the importers, the evaluator and the exports.
// @tier L9 · shared

#include <engine/assets/AssetKind.hpp>
#include <engine/assets/Mesh.hpp>
#include <engine/assets/Texture.hpp>
#include <engine/bakegraph/Nodes.hpp>
#include <engine/core/types/Vector3.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::bake {

	// What a node produced.
	//
	// @since v0.9
	enum class PayloadKind : uint8_t {
		// Nothing yet, or a node that failed. Never a valid input.
		None,

		// Undecoded bytes, as a source node holds them.
		Bytes,

		// Geometry.
		Mesh,

		// An image.
		Texture,
	};

	// One node's result; `Kind` selects the populated payload.
	struct Payload {
		// Which of the three below is filled.
		PayloadKind Kind = PayloadKind::None;

		// Source name, carried down the chain for import dispatch.
		std::string Source;

		// The payload itself, in whichever form the last node left it.
		//
		// **Every form rather than a variant**, so a node reads the one it
		// understands and passes the rest through untouched - which is what lets
		// a chain mix a decoder, a resize and a writer without each knowing what
		// the others produce.
		//@{
		std::vector<std::byte> Bytes;
		assets::MeshData Mesh;
		assets::TextureData Texture;
		//@}

		// The material library a model named, for the formats that keep their
		// materials in a second file. Empty for every other payload.
		//
		// **Carried rather than resolved, exactly like `ImportedModel::Textures`
		// and for the same reason:** opening the file is a filesystem operation
		// and this module has none. It rides the payload so the publisher at the
		// end of the chain - which does know where the model came from - can
		// read it. A node that does not understand it passes it through, which
		// is what the note above says every field here does.
		std::string MaterialLibrary;
	};

	// One export node's result.
	//
	// @since v0.9
	struct BakedAsset {
		// The name the export node was given.
		std::string Name;

		// What it is, so a publisher can route it without re-deriving the kind
		// from the name - which `assets::AssetKind` says is a decision taken in
		// exactly one place.
		assets::AssetKind Kind = assets::AssetKind::Unknown;

		// The serialised asset.
		std::vector<std::byte> Bytes;
	};

	// A bake pipeline.
	//
	// Build it, wire it, run it, take the exports. Running twice re-evaluates
	// from the sources, which is what makes a graph a description rather than a
	// one-shot.
	//
	// @since v0.9
	class Graph {
	  public:
		// The most nodes one graph may hold.
		//
		// A bound so that a generated graph - one node per file in a directory
		// somebody uploaded - cannot become an unbounded allocation. Four
		// thousand is far past any hand-built pipeline and is roughly a large
		// asset directory's worth of three-node chains.
		static constexpr uint32_t MAXIMUM_NODES = 4096;

		// Adds an input node holding bytes.
		//
		// @param name  What the bytes came from. Its extension is what `Import`
		//              dispatches on for the formats with no signature.
		// @param bytes The file. Copied, because a graph outlives the call.
		// @return The node, or an invalid id when the graph is full.
		NodeId AddSource(std::string_view name, std::span<const std::byte> bytes);

		// Adds an input node producing one of the engine's built-in meshes.
		//
		// @param name The built-in's name, as `assets::BuiltinName` spells it.
		// @return The node, or an invalid id for an unknown built-in.
		NodeId AddBuiltin(std::string_view name);

		// Adds a processing or export node.
		//
		// @param kind Which bare node. Input and parameterised kinds are refused:
		//             they carry data the other overloads take.
		// @return The node, or an invalid id.
		NodeId Add(NodeKind kind);

		// Adds a `Fit` node.
		//
		// @param size The target measurement of the longest axis, in metres.
		// @return The node, or an invalid id.
		NodeId AddFit(float size);

		// Adds a `Scale` node.
		//
		// @param amount The multiplier on each axis.
		// @return The node, or an invalid id.
		NodeId AddScale(const core::Vector3 &amount);

		// Adds a `Resize` node.
		//
		// @param width  The target width in pixels.
		// @param height The target height in pixels.
		// @return The node, or an invalid id.
		NodeId AddResize(uint32_t width, uint32_t height);

		// Adds a `Rasterize` node.
		//
		// **The size is here rather than in a `Resize` after an `Import`,
		// because a vector drawing has no size to import at.** Rasterising at
		// the target and rasterising large then box-filtering down are different
		// pictures - the second's edges belong to the resampler - so the target
		// is stated where the drawing is first given pixels.
		//
		// Zero on both axes means the size the document itself declares, which
		// is the only size an SVG can be said to have. One zero and one number
		// is refused at evaluation: an aspect this file guessed would be a
		// number nobody wrote.
		//
		// @param width  The target width in pixels.
		// @param height The target height in pixels.
		// @return The node, or an invalid id.
		// @since v0.14
		NodeId AddRasterize(uint32_t width, uint32_t height);

		// Restates an imported flipbook's frame rate.
		//
		// **A node rather than a mutation on the way past**, because everything
		// else that changes a payload is one - and the graph's whole property is
		// that what a bake did is the list of nodes it ran. A rate rewritten by
		// the caller between `Run` calls would be a step nothing records.
		//
		// A rate of zero, or a payload that is not a flipbook, passes through
		// untouched: an override applies to a tree and most of a tree is still
		// images.
		//
		// @param fps Frames a second to stamp on.
		// @return The node.
		// @since v0.10
		NodeId AddRetime(float fps);

		// Adds a `Write` node.
		//
		// @param name The name the asset is published under.
		// @return The node, or an invalid id.
		NodeId AddWrite(std::string_view name);

		// Wires one node's output into another's input.
		//
		// **Refused rather than replaced when the target already has an
		// input.** Silently rewiring would make the order of two `Connect`
		// calls decide what a pipeline does, which is the kind of dependence
		// that only shows up once somebody reorders their setup code.
		//
		// @param from The producing node.
		// @param to   The consuming node.
		// @return `false` for an unknown node, an input-kind target, a target
		//         that already has an input, or a wire that would close a
		//         cycle.
		bool Connect(NodeId from, NodeId to);

		// Evaluates every node.
		//
		// Runs in dependency order and stops at the first failure, so a chain
		// whose import failed does not go on to write an empty asset.
		//
		// @param failure Set to why on failure, naming the node's source.
		//                Untouched on success.
		// @return `false` when any node failed.
		bool Run(std::string &failure);

		// What a node produced by the last `Run`.
		//
		// @param node The node.
		// @return Its payload, or an empty one for an unknown or unevaluated
		//         node.
		const Payload &Output(NodeId node) const;

		// Every export node's result, in the order the nodes were added.
		//
		// @return The baked assets. Empty until a successful `Run`.
		std::span<const BakedAsset> Baked() const;

		// How many nodes the graph holds.
		//
		// @return The count.
		size_t NodeCount() const;

	  private:
		struct Node {
			NodeKind Kind = NodeKind::Source;
			NodeId Input;
			std::string Name;
			core::Vector3 Amount{1.0f, 1.0f, 1.0f};
			float Size = 1.0f;
			uint32_t Width = 0;
			uint32_t Height = 0;
			Payload Result;
		};

		NodeId Append(Node node);
		bool Evaluate(size_t index, std::string &failure);

		std::vector<Node> Nodes;
		std::vector<BakedAsset> Exports;
	};
}
