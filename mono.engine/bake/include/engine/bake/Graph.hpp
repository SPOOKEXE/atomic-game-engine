#pragma once

// The bake pipeline: input nodes, processing nodes, export nodes.
//
// `ROADMAP.md` v0.9 asks for exactly those three words, and this is them. The
// shape is `audio::Graph`'s deliberately — nodes, wires, a topological run and
// cycles refused at the wire rather than found at execution — because a second
// unrelated node model in one engine is two things to learn and two to debug.
//
// **The graph touches no filesystem, and that is the whole design.** A source
// node holds bytes somebody handed it and an export node hands bytes back; the
// reading and writing is the caller's. That is what `audio::NullDevice` is to
// the mixer: it makes every importer, every processing step and every export
// testable in a suite that opens no file, and it means a studio baking into
// memory and a CLI baking onto a disk run the identical code.
//
// **A node has at most one input.** Not a limitation waiting to be lifted — it
// is what makes "what produced this asset" answerable by walking a chain rather
// than by reasoning about a fold. Fan-*out* is allowed and is the useful
// direction: one decoded texture resized to three sizes is three chains sharing
// a head.
//
// **Cycles are refused when the wire is made.** A cycle here is not the audio
// module's unbounded gain; it is an evaluation that never terminates, and the
// check is a walk up the input chain at connect time, which is not on any hot
// path because there is no hot path.
//
// @tier L9 · shared

#include <engine/assets/AssetKind.hpp>
#include <engine/assets/Mesh.hpp>
#include <engine/assets/Texture.hpp>
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

	// One node's result.
	//
	// **Three fields of which at most one is filled, rather than a variant.**
	// A variant would be tidier and would cost every consumer a visit; these
	// are large, movable, ordinary types and `Kind` is the discriminator that
	// every path already has to switch on.
	//
	// @since v0.9
	struct Payload {
		// Which of the three below is filled.
		PayloadKind Kind = PayloadKind::None;

		// What this came from, carried down the chain from the source node.
		//
		// **The reason an import node needs no configuration**: it dispatches
		// on this, so a chain that reads `hair.png` and one that reads
		// `body.pmx` are the same three nodes with different bytes at the top.
		std::string Source;

		std::vector<std::byte> Bytes;
		assets::MeshData Mesh;
		assets::TextureData Texture;
	};

	// What a node does.
	//
	// A closed list, for `audio::NodeKind`'s reason: an open set would make
	// "what does this graph do" unanswerable without running it.
	//
	// @since v0.9
	enum class NodeKind : uint8_t {
		// **Input.** Bytes the caller supplied, under the name they came from.
		Source,

		// **Input.** One of `assets::MakeBuiltin`'s meshes, by name. No bytes,
		// because a built-in is generated rather than read.
		Builtin,

		// **Processor.** Bytes to a mesh or a texture, chosen by what the bytes
		// are and — for the formats with no signature — by the source name.
		Import,

		// **Processor.** Scales a mesh uniformly so its longest axis measures
		// the given size, and recentres it on the origin. `FitMesh`.
		Fit,

		// **Processor.** Multiplies a mesh's positions per axis. The escape
		// hatch for a model that is the right proportions and the wrong size,
		// where `Fit` would be the wrong tool because it changes proportions
		// relative to other assets.
		Scale,

		// **Processor.** Replaces a mesh's normals with area-weighted vertex
		// normals. `SmoothNormals`.
		Smooth,

		// **Processor.** Box-filters a texture to a given size. `ResizeImage`.
		Resize,

		// **Processor.** Forces a texture's alpha to fully opaque.
		//
		// Narrow on purpose: it exists because a sphere map or a toon ramp
		// arrives with a zeroed alpha channel it never meant as transparency,
		// and a blended pass would then draw nothing at all.
		Opaque,

		// **Export.** Serialises its input into the engine's format under a
		// published name. A mesh becomes `assets::Mesh`'s bytes and a texture
		// becomes `assets::Texture`'s.
		Write,
	};

	// A node's handle.
	//
	// A number rather than a pointer, so a graph may be copied and a node may
	// be named in a log line.
	//
	// @since v0.9
	struct NodeId {
		// The value meaning "no node". Zero is never issued.
		static constexpr uint32_t NONE = 0;

		uint32_t Value = NONE;

		constexpr bool IsValid() const {
			return Value != NONE;
		}
		constexpr bool operator==(const NodeId &other) const = default;
	};

	// One export node's result.
	//
	// @since v0.9
	struct BakedAsset {
		// The name the export node was given.
		std::string Name;

		// What it is, so a publisher can route it without re-deriving the kind
		// from the name — which `assets::AssetKind` says is a decision taken in
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
		// A bound so that a generated graph — one node per file in a directory
		// somebody uploaded — cannot become an unbounded allocation. Four
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
		// @param kind Which. `Source` and `Builtin` are refused: they carry
		//             data the other overloads take.
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
