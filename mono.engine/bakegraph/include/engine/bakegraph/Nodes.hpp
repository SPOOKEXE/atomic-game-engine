#pragma once

// The words a bake pipeline is described in, and nothing that runs one.
//
// **This module exists so that `Engine::game` can read a pipeline document
// without linking a JPEG decoder.** `bake` carries the PNG, JPEG, GIF, BMP, OBJ,
// glTF and PMX readers - it is the only thing in the engine that parses a
// foreign format, and `bake/CMakeLists.txt` states at length that nothing a
// shipped game links may link it. A save format that named `bake` to parse a
// *text document* would put every one of those decoders into `server`, which has
// no reason to decode a JPEG.
//
// So the vocabulary and the document format live here and the evaluator stays
// there. `D00102` weighed three ways out and settled on this one: carrying
// pipelines as opaque text the save format never parses would cost the load-time
// validation the render half already has, and putting them on the universe
// rather than the world trades a dependency question for an ownership question
// nobody has answered.
//
// **The namespace is still `engine::bake`.** A module boundary is a link-line
// fact and these are the same words they always were; renaming them would be
// churn at every call site to express something `expected_graph.json` already
// states. What changed is which archive they are in and what that archive is
// allowed to depend on.
//
// @tier L9 · shared

#include <cstdint>

namespace engine::bake {

	// Closed list of node operations.
	enum class NodeKind : uint8_t {
		// Input bytes supplied by the caller.
		Source,

		// Input built-in mesh by name.
		Builtin,

		// Import bytes to a mesh or texture.
		Import,

		// Fit a mesh to a size and recenter it.
		Fit,

		// Scale mesh positions per axis.
		Scale,

		// Recompute area-weighted vertex normals.
		Smooth,

		// Box-filter a texture to a size.
		Resize,

		// Force a texture's alpha opaque.
		Opaque,

		// Build a texture's mip chain.
		//
		// Last of the texture nodes in a chain, because every other one changes
		// the pixels the levels are filtered from.
		//
		// @since v0.14
		Mipmap,

		// Restate a flipbook's frame rate.
		//
		// @since v0.10
		Retime,

		// Serialize input into the engine format.
		Write,

		// Rasterise a vector source into a texture at a stated pixel size.
		//
		// **A separate kind rather than a parameter on `Import`, because it is
		// the one import that cannot be decided from the bytes.** An SVG states
		// a coordinate system and no pixels, so the size is the pipeline's
		// decision - and a `Resize` afterwards is not the same picture, because
		// it resamples a rasterisation that was never the right sharpness. An
		// `Import` handed an SVG says so and names this node.
		//
		// **Appended, because the ordinals here are on the wire.** A document
		// saved by an older build names its nodes by these numbers.
		//
		// @since v0.14
		Rasterize,
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

		// The handle itself, an index into the graph's node list.
		uint32_t Value = NONE;

		// Whether this names a node.
		//
		// @return `false` for a default handle, which is what `AddX` returns
		//         when it refuses.
		constexpr bool IsValid() const {
			return Value != NONE;
		}

		// @param other The handle to compare.
		// @return `true` when both name the same node.
		constexpr bool operator==(const NodeId &other) const = default;
	};

	// Whether a node kind carries no parameter of its own.
	//
	// **A closed list rather than the complement of the parameterised ones.** A
	// node kind added later arrives here as "not bare" and has to be thought
	// about, rather than silently becoming legal with its parameter dropped.
	//
	// **Public because two modules need the same answer.** The document format
	// asks it to decide how an operation is spelled, and `bake::Build` asks it
	// to decide which `Graph::Add` overload to call. It was a private helper
	// beside the first until the split; a copy beside the second is exactly the
	// drift a closed list exists to prevent.
	//
	// @param kind The kind.
	// @return `true` for a kind that `Graph::Add(NodeKind)` takes whole.
	// @since v0.13
	bool IsBareNode(NodeKind kind);
}
