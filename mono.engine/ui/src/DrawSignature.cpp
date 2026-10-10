#include "DrawSignature.hpp"

#include <cstring>
#include <imgui.h>

namespace engine::ui::draw_signature_detail {
	uint64_t FoldBytes(uint64_t hash, const void *data, size_t size) {
		constexpr uint64_t PRIME = 1099511628211ull;
		const auto *bytes = static_cast<const std::byte *>(data);
		hash = (hash ^ size) * PRIME;

		// Independent lanes keep large vertex and index buffers from waiting on
		// one multiply chain. Small command fields retain the serial fold.
		size_t index = 0;
		if (size >= 128) {
			uint64_t lane0 = hash ^ 0x9e3779b97f4a7c15ull;
			uint64_t lane1 = hash ^ 0xbf58476d1ce4e5b9ull;
			uint64_t lane2 = hash ^ 0x94d049bb133111ebull;
			uint64_t lane3 = hash ^ 0xd6e8feb86659fd93ull;
			for (; index + 4 * sizeof(uint64_t) <= size; index += 4 * sizeof(uint64_t)) {
				uint64_t word0{}, word1{}, word2{}, word3{};
				std::memcpy(&word0, bytes + index, sizeof(word0));
				std::memcpy(&word1, bytes + index + sizeof(uint64_t), sizeof(word1));
				std::memcpy(&word2, bytes + index + 2 * sizeof(uint64_t), sizeof(word2));
				std::memcpy(&word3, bytes + index + 3 * sizeof(uint64_t), sizeof(word3));
				lane0 = (lane0 ^ word0) * PRIME;
				lane1 = (lane1 ^ word1) * PRIME;
				lane2 = (lane2 ^ word2) * PRIME;
				lane3 = (lane3 ^ word3) * PRIME;
			}
			// Avalanche each lane and fold it in order, so exchanging lanes or
			// equal-sized chunks does not cancel as it would with a plain XOR.
			const auto mix = [](uint64_t lane) {
				lane = (lane ^ (lane >> 30)) * 0xbf58476d1ce4e5b9ull;
				lane = (lane ^ (lane >> 27)) * 0x94d049bb133111ebull;
				return lane ^ (lane >> 31);
			};
			hash = (hash ^ mix(lane0)) * PRIME;
			hash = (hash ^ mix(lane1)) * PRIME;
			hash = (hash ^ mix(lane2)) * PRIME;
			hash = (hash ^ mix(lane3)) * PRIME;
		}
		for (; index + sizeof(uint64_t) <= size; index += sizeof(uint64_t)) {
			uint64_t word = 0;
			std::memcpy(&word, bytes + index, sizeof(word));
			hash = (hash ^ word) * PRIME;
		}
		if (index < size) {
			uint64_t word = 0;
			std::memcpy(&word, bytes + index, size - index);
			hash = (hash ^ word) * PRIME;
		}
		return hash;
	}

	uint64_t DrawGeometrySignature(const ImDrawData *draw) {
		if (draw == nullptr) {
			return 0;
		}

		uint64_t hash = 1469598103934665603ull;
		hash = FoldBytes(hash, &draw->DisplayPos, sizeof(draw->DisplayPos));
		hash = FoldBytes(hash, &draw->DisplaySize, sizeof(draw->DisplaySize));
		hash = FoldBytes(hash, &draw->FramebufferScale, sizeof(draw->FramebufferScale));
		for (const ImDrawList *list : draw->CmdLists) {
			hash = FoldBytes(hash, list->VtxBuffer.Data, list->VtxBuffer.Size * sizeof(ImDrawVert));
			hash = FoldBytes(hash, list->IdxBuffer.Data, list->IdxBuffer.Size * sizeof(ImDrawIdx));
			for (const ImDrawCmd &command : list->CmdBuffer) {
				hash = FoldBytes(hash, &command.ClipRect, sizeof(command.ClipRect));
				// The unresolved atlas pointer is the texture's identity in a
				// headless frame. GetTexID asserts until a graphics backend uploads
				// it, while this pair is valid in both headed and headless hosts.
				hash = FoldBytes(hash, &command.TexRef, sizeof(command.TexRef));
				hash = FoldBytes(hash, &command.ElemCount, sizeof(command.ElemCount));
				hash = FoldBytes(hash, &command.IdxOffset, sizeof(command.IdxOffset));
				hash = FoldBytes(hash, &command.VtxOffset, sizeof(command.VtxOffset));
				hash = FoldBytes(hash, &command.UserCallback, sizeof(command.UserCallback));
			}
		}
		return hash;
	}
}
