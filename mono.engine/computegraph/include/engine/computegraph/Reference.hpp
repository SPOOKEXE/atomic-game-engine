#pragma once

// CPU oracles for the first GPU consumer chain. These functions submit no GPU work.
// Refusal preserves the previous output, including on checked integer overflow.
// @tier L3 · shared

#include <engine/computegraph/Document.hpp>

#include <span>

namespace engine::computegraph {
	// Stable nonzero compaction with explicit input and output element limits.
	Status CompactNonZeroU32(
		std::span<const uint32_t> input,
		uint32_t maximumInput,
		uint32_t outputCapacity,
		std::vector<uint32_t> &out,
		Diagnostic &diagnostic
	);
	// Checked doubling of the valid compacted prefix, matching the consuming GPU pass.
	Status DoubleU32(
		std::span<const uint32_t> input,
		uint32_t logicalCount,
		uint32_t outputCapacity,
		std::vector<uint32_t> &out,
		Diagnostic &diagnostic
	);
	// Entire reference chain; neither a successful compact nor a partial doubling publishes.
	Status CompactDoubleU32(
		std::span<const uint32_t> input,
		uint32_t maximumInput,
		uint32_t outputCapacity,
		std::vector<uint32_t> &out,
		Diagnostic &diagnostic
	);
}
