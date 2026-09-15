#pragma once

#include <cstdint>
#include <limits>

namespace engine::world::detail {

	inline bool CanAdvanceDataFactoryRevision(uint64_t epoch, uint64_t version, bool freshEpoch) {
		return version != std::numeric_limits<uint64_t>::max() &&
			   (!freshEpoch || epoch != std::numeric_limits<uint64_t>::max());
	}
}
