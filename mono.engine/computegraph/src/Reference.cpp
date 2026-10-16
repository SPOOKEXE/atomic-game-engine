#include "Validation.hpp"

#include <engine/computegraph/Reference.hpp>

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace engine::computegraph {
	Status CompactNonZeroU32(
		std::span<const uint32_t> input,
		uint32_t maximumInput,
		uint32_t outputCapacity,
		std::vector<uint32_t> &out,
		Diagnostic &diagnostic
	) {
		if (input.size() > maximumInput || input.size() > (64u * 1024u * 1024u) / sizeof(uint32_t))
			return detail::Refuse(diagnostic, Status::LimitExceeded);
		const auto count = static_cast<size_t>(std::count_if(input.begin(), input.end(), [](uint32_t value) {
			return value != 0;
		}));
		if (count > outputCapacity) return detail::Refuse(diagnostic, Status::OutputOverflow);
		try {
			std::vector<uint32_t> result;
			result.reserve(count);
			for (const uint32_t value : input)
				if (value != 0) result.push_back(value);
			out = std::move(result);
			diagnostic = {};
			return Status::Ok;
		} catch (const std::bad_alloc &) {
			return detail::AllocationFailure(diagnostic);
		}
	}
	Status DoubleU32(
		std::span<const uint32_t> input,
		uint32_t logicalCount,
		uint32_t outputCapacity,
		std::vector<uint32_t> &out,
		Diagnostic &diagnostic
	) {
		if (logicalCount > input.size()) return detail::Refuse(diagnostic, Status::InvalidCount);
		if (logicalCount > outputCapacity) return detail::Refuse(diagnostic, Status::OutputOverflow);
		if (logicalCount > (64u * 1024u * 1024u) / sizeof(uint32_t))
			return detail::Refuse(diagnostic, Status::LimitExceeded);
		for (size_t index = 0; index < logicalCount; ++index)
			if (input[index] > std::numeric_limits<uint32_t>::max() / 2)
				return detail::Refuse(diagnostic, Status::OutputOverflow);
		try {
			std::vector<uint32_t> result(logicalCount);
			for (size_t index = 0; index < logicalCount; ++index)
				result[index] = input[index] * 2;
			out = std::move(result);
			diagnostic = {};
			return Status::Ok;
		} catch (const std::bad_alloc &) {
			return detail::AllocationFailure(diagnostic);
		}
	}
	Status CompactDoubleU32(
		std::span<const uint32_t> input,
		uint32_t maximumInput,
		uint32_t outputCapacity,
		std::vector<uint32_t> &out,
		Diagnostic &diagnostic
	) {
		std::vector<uint32_t> compacted;
		const auto status = CompactNonZeroU32(input, maximumInput, outputCapacity, compacted, diagnostic);
		if (status != Status::Ok) return status;
		return DoubleU32(compacted, static_cast<uint32_t>(compacted.size()), outputCapacity, out, diagnostic);
	}
}
