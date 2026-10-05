#pragma once

#include <engine/imagegraph/CacheGroupReplay.hpp>

namespace engine::imagegraph::detail {
	// Reconciliation admits value clone storage beside embedded output records.
	uint64_t CacheGroupReplayCloneBytes(const CacheGroupReplayState &state);
}
