#pragma once
#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <optional>
namespace engine::imagegraph::detail {
	inline std::optional<uint64_t> SourceRegionOrdinal(std::string_view identity) {
		constexpr std::string_view prefix = "pxc:region:";
		if (identity.size() > Limits::MaximumSourceRegionIdBytes || !identity.starts_with(prefix)) return {};
		identity.remove_prefix(prefix.size());
		if (identity.empty() || (identity.size() > 1 && identity.front() == '0')) return {};
		uint64_t ordinal = 0;
		const auto parsed = std::from_chars(identity.data(), identity.data() + identity.size(), ordinal);
		if (parsed.ec != std::errc{} || parsed.ptr != identity.data() + identity.size()) return {};
		return ordinal;
	}
	inline bool UniqueSourceRegionOrigins(std::span<const AnimationRegion> regions) {
		if (regions.size() > Limits::MaximumAnimationRegions) return false;
		std::array<uint64_t, Limits::MaximumAnimationRegions> scratch{};
		size_t count = 0;
		for (const auto &region : regions) {
			if (region.SourceRegionId.empty()) continue;
			const auto ordinal = SourceRegionOrdinal(region.SourceRegionId);
			if (!ordinal) return false;
			scratch[count++] = *ordinal;
		}
		auto used = std::span(scratch).first(count);
		std::sort(used.begin(), used.end());
		return std::adjacent_find(used.begin(), used.end()) == used.end();
	}
}
