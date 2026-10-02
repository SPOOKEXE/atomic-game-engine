#pragma once

// Module-private benchmark/test diagnostics for the existing thread-local arena.
// Payload bytes are vector capacity, separate from allocator and profiler overhead.
#include <cstddef>

namespace engine::gui::detail {
	struct ChildArenaScratch {
		size_t LogicalHandles = 0;
		size_t CapacityHandles = 0;
		size_t PayloadBytes = 0;
	};
	ChildArenaScratch ReadChildArenaScratch();
} // namespace engine::gui::detail
