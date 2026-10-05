#pragma once

#include <string_view>

namespace engine::imagegraph::detail {
	enum class SourceFrameCacheInputReads { All, ControlsOnly, None };
	// Auto Cache hits bypass update entirely. Array checks playback and producer activity first.
	inline SourceFrameCacheInputReads SourceFrameCacheReadPolicy(
		std::string_view type, bool playing, bool linked, bool active, bool cacheHit = false
	) {
		if (type == "pc.cache" && cacheHit) return SourceFrameCacheInputReads::None;
		if (type != "pc.cache" && type != "pc.cache_array") return SourceFrameCacheInputReads::All;
		if (playing && linked && active) return SourceFrameCacheInputReads::All;
		return type == "pc.cache_array" ? SourceFrameCacheInputReads::None
										: SourceFrameCacheInputReads::ControlsOnly;
	}
	inline bool SourceFrameCacheReadsPort(SourceFrameCacheInputReads policy, std::string_view port) {
		return policy == SourceFrameCacheInputReads::All ||
			   (policy == SourceFrameCacheInputReads::ControlsOnly && port != "surface_in");
	}
}
