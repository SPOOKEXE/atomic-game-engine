#pragma once

#include <string_view>

namespace engine::imagegraph::detail {
	enum class SourceFrameCacheInputReads { All, ControlsOnly, None };
	// Complete receipts retain the native auto-cache shortcut. Source step loading runs Cache update
	// controls before recovery, while Cache Array loading returns before every getter.
	inline SourceFrameCacheInputReads SourceFrameCacheReadPolicy(
		std::string_view type,
		bool playing,
		bool linked,
		bool active,
		bool cacheHit = false,
		bool sourceStepLoading = false,
		bool loading = false
	) {
		if (sourceStepLoading && type == "pc.cache_array" && loading) return SourceFrameCacheInputReads::None;
		if (type == "pc.cache" && cacheHit)
			return sourceStepLoading ? SourceFrameCacheInputReads::ControlsOnly
									 : SourceFrameCacheInputReads::None;
		if (type != "pc.cache" && type != "pc.cache_array") return SourceFrameCacheInputReads::All;
		if (playing && linked && active && !loading) return SourceFrameCacheInputReads::All;
		return type == "pc.cache_array" ? SourceFrameCacheInputReads::None
										: SourceFrameCacheInputReads::ControlsOnly;
	}
	inline bool SourceFrameCacheReadsPort(SourceFrameCacheInputReads policy, std::string_view port) {
		return policy == SourceFrameCacheInputReads::All ||
			   (policy == SourceFrameCacheInputReads::ControlsOnly && port != "surface_in");
	}
}
