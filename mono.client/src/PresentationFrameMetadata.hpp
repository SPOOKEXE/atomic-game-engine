#pragma once
#include <engine/gui/SemanticSnapshotCodec.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace client {
	struct PresentationFrameMetadata {
		uint64_t Runtime = 0;
		uint64_t Store = 0;
		std::optional<engine::gui::SemanticSnapshot> Semantics;
	};

	inline std::optional<std::vector<std::byte>> EncodePresentationFrameMetadata(
		uint64_t runtime, uint64_t store, const engine::gui::SemanticSnapshot *semantics, size_t limit
	) {
		constexpr size_t header = 2 * sizeof(uint64_t) + 1;
		if (limit < header) return {};
		try {
			std::vector<std::byte> semanticBytes;
			if (semantics && !engine::gui::EncodeSemanticSnapshot(*semantics, limit - header, semanticBytes))
				return {};
			if (semanticBytes.size() > limit - header) return {};
			std::vector<std::byte> bytes(header + semanticBytes.size());
			std::memcpy(bytes.data(), &runtime, sizeof(runtime));
			std::memcpy(bytes.data() + sizeof(runtime), &store, sizeof(store));
			bytes[2 * sizeof(uint64_t)] = semantics ? std::byte{1} : std::byte{0};
			if (!semanticBytes.empty())
				std::memcpy(bytes.data() + header, semanticBytes.data(), semanticBytes.size());
			return bytes;
		} catch (const std::bad_alloc &) {
			return {};
		}
	}

	inline std::optional<PresentationFrameMetadata>
	DecodePresentationFrameMetadata(std::span<const std::byte> bytes, size_t budget) {
		constexpr size_t header = 2 * sizeof(uint64_t) + 1;
		if (bytes.size() < header || budget < sizeof(PresentationFrameMetadata)) return {};
		PresentationFrameMetadata result;
		std::memcpy(&result.Runtime, bytes.data(), sizeof(result.Runtime));
		std::memcpy(&result.Store, bytes.data() + sizeof(result.Runtime), sizeof(result.Store));
		const auto hasSemantics = std::to_integer<uint8_t>(bytes[2 * sizeof(uint64_t)]);
		if (hasSemantics > 1 || (!hasSemantics && bytes.size() != header)) return {};
		if (hasSemantics) {
			engine::gui::SemanticSnapshot snapshot;
			if (!engine::gui::DecodeSemanticSnapshot(
					bytes.subspan(header), budget - sizeof(result), snapshot
				))
				return {};
			result.Semantics = std::move(snapshot);
		}
		return result;
	}
}
