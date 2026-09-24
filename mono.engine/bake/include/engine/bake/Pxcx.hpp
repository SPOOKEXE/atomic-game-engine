#pragma once

// Bounded byte-span reader for the supplied Pixel Composer PXCX container.
//
// The reader preserves the source archive and decoded graph verbatim. Its
// node and link records are validated observations of the five supplied
// projects, not a node registry and not an execution plan.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace engine::bake {

	// Resource ceilings for one imported PXCX project.
	struct PxcxLimits {
		// Total input bytes accepted by the reader.
		static constexpr size_t MaximumArchiveBytes = 64u * 1024u * 1024u;
		// Compressed thumbnail stream bytes.
		static constexpr size_t MaximumThumbnailCompressedBytes = 1024u * 1024u;
		// Decoded thumbnail is 256 by 256 RGBA8 in all five supplied projects.
		static constexpr size_t ThumbnailRgbaBytes = 256u * 256u * 4u;
		// Compressed graph stream bytes.
		static constexpr size_t MaximumGraphCompressedBytes = 32u * 1024u * 1024u;
		// Decoded UTF-8 graph document, including its terminal NUL byte.
		static constexpr size_t MaximumGraphJsonBytes = 32u * 1024u * 1024u;
		// Opaque META payload bytes.
		static constexpr size_t MaximumMetadataBytes = 64u * 1024u;
		// Validated records extracted from the graph JSON.
		static constexpr size_t MaximumNodes = 4096;
		static constexpr size_t MaximumLinks = 262144;
		// The five supplied projects use at most 84 positional inputs per node.
		static constexpr size_t MaximumInputsPerNode = 8192;
		// Maximum UTF-8 bytes in one imported node identifier or type name.
		static constexpr size_t MaximumNodeTextBytes = 1024;
		// Maximum nesting accepted by the graph JSON parser callback.
		static constexpr size_t MaximumJsonDepth = 128;
	};

	// One validated node identity and canvas position from the PXC graph JSON.
	struct PxcxNodeFact {
		// Durable string identifier exactly as stored in the project.
		std::string Id;
		// Foreign node type string. It does not imply a native implementation.
		std::string Type;
		// Authored horizontal coordinate.
		double X = 0.0;
		// Authored vertical coordinate.
		double Y = 0.0;
		bool operator==(const PxcxNodeFact &) const = default;
	};

	// One validated connection between durable node identifiers.
	struct PxcxLinkFact {
		// Source node identifier.
		std::string FromNode;
		// Source format's positional output index, not a native port name.
		uint32_t FromIndex = 0;
		// Destination node identifier.
		std::string ToNode;
		// Index into the destination node's authored inputs array.
		uint32_t ToInputIndex = 0;
		bool operator==(const PxcxLinkFact &) const = default;
	};

	// A parsed PXCX archive with source data and validated structural facts.
	struct PxcxArchive {
		// Original container bytes, retained exactly.
		std::vector<std::byte> OriginalBytes;
		// Whether the source header carried THMB, including a zero-length block.
		bool HasThumbnailBlock = false;
		// Decoded RGBA8 thumbnail bytes.
		std::vector<uint8_t> ThumbnailRgba;
		// Opaque META payload, retained exactly.
		std::vector<std::byte> MetadataPayload;
		// SAVE_VERSION written by Pixel Composer, as observed in upstream SAVE_AT.
		uint32_t MetadataNumber = 0;
		// VERSION_STRING written by Pixel Composer, excluding its terminal NUL.
		std::string MetadataText;
		// Full decoded graph stream, including its required trailing NUL byte.
		std::string GraphJson;
		// Validated node identity and position facts.
		std::vector<PxcxNodeFact> Nodes;
		// Validated endpoint references and source positional indices.
		std::vector<PxcxLinkFact> Links;
	};

	// Reads a PXCX archive from memory without opening files or executing nodes.
	// `out` is replaced only after the entire container and graph structure pass.
	bool ReadPxcx(std::span<const std::byte> bytes, PxcxArchive &out, std::string &failure);

	// Writes the upstream PXCX envelope. An unchanged imported archive is emitted
	// byte for byte; changed content is validated again before replacing `out`.
	bool WritePxcx(const PxcxArchive &archive, std::vector<std::byte> &out, std::string &failure);

}
