#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace engine::imagegraphexport::detail {
	// grug vendor heap shares one fixed reservation, including transient realloc copies.
	struct PxcxCodecHeap {
		struct alignas(std::max_align_t) Block {
			size_t Bytes;
		};
		static constexpr size_t MaximumBytes = 1024 * 1024;
		size_t Remaining = MaximumBytes;
		static void *Allocate(void *opaque, size_t count, size_t width) noexcept {
			auto &heap = *static_cast<PxcxCodecHeap *>(opaque);
			if (!width || count > (SIZE_MAX - sizeof(Block)) / width) return nullptr;
			const size_t bytes = count * width + sizeof(Block);
			if (bytes > heap.Remaining) return nullptr;
			auto *block = static_cast<Block *>(std::malloc(bytes));
			if (!block) return nullptr;
			block->Bytes = bytes;
			heap.Remaining -= bytes;
			return block + 1;
		}
		static void Release(void *opaque, void *address) noexcept {
			if (!address) return;
			auto *block = static_cast<Block *>(address) - 1;
			static_cast<PxcxCodecHeap *>(opaque)->Remaining += block->Bytes;
			std::free(block);
		}
		static void *Resize(void *opaque, void *address, size_t count, size_t width) noexcept {
			void *replacement = Allocate(opaque, count, width);
			if (!replacement) return nullptr;
			if (address) {
				const auto *block = static_cast<Block *>(address) - 1;
				std::memcpy(replacement, address, std::min(block->Bytes - sizeof(Block), count * width));
				Release(opaque, address);
			}
			return replacement;
		}
	};
}
