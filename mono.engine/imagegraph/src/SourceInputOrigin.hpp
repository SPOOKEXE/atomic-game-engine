#pragma once
#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <charconv>
#include <optional>
#include <span>
namespace engine::imagegraph::detail {
	inline std::optional<uint64_t> SourceInputOrdinal(std::string_view name) {
		constexpr std::string_view prefix = "pxc:input:";
		if (name.size() > Limits::MaximumSourceInputIdBytes || !name.starts_with(prefix)) return std::nullopt;
		name.remove_prefix(prefix.size());
		if (name.empty() || (name.size() > 1 && name.front() == '0')) return std::nullopt;
		uint64_t ordinal = 0;
		const auto parsed = std::from_chars(name.data(), name.data() + name.size(), ordinal);
		if (parsed.ec != std::errc{} || parsed.ptr != name.data() + name.size()) return std::nullopt;
		return ordinal;
	}
	// Caller supplies fixed or preadmitted scratch; only nonempty canonical origins participate.
	inline Status ValidateSourceInputOrigins(
		std::span<const DynamicInput> inputs, std::span<uint64_t> scratch, std::string_view &failedPort
	) {
		failedPort = {};
		size_t count = 0;
		for (const auto &input : inputs) {
			if (input.SourceInputId.empty()) continue;
			failedPort = input.Id;
			if (input.SourceInputId.size() > Limits::MaximumSourceInputIdBytes) return Status::LimitExceeded;
			const auto ordinal = SourceInputOrdinal(input.SourceInputId);
			if (!ordinal) return Status::InvalidValue;
			if (count == scratch.size()) return Status::LimitExceeded;
			scratch[count++] = *ordinal;
		}
		const auto used = scratch.first(count);
		std::sort(used.begin(), used.end());
		const auto duplicate = std::adjacent_find(used.begin(), used.end());
		if (duplicate != used.end()) {
			bool first = false;
			for (const auto &input : inputs) {
				const auto ordinal = SourceInputOrdinal(input.SourceInputId);
				if (ordinal && *ordinal == *duplicate) {
					if (first) {
						failedPort = input.Id;
						break;
					}
					first = true;
				}
			}
			return Status::DuplicateId;
		}
		failedPort = {};
		return Status::Ok;
	}

}
