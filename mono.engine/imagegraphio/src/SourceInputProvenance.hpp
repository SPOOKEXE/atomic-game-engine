#pragma once
#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <charconv>
#include <optional>
namespace engine::imagegraphio::detail {
	inline std::optional<uint64_t> SourceInputOrdinal(std::string_view name) {
		constexpr std::string_view prefix = "pxc:input:";
		if (name.size() > imagegraph::Limits::MaximumSourceInputIdBytes || !name.starts_with(prefix))
			return std::nullopt;
		name.remove_prefix(prefix.size());
		if (name.empty() || (name.size() > 1 && name.front() == '0')) return std::nullopt;
		uint64_t ordinal = 0;
		const auto parsed = std::from_chars(name.data(), name.data() + name.size(), ordinal);
		if (parsed.ec != std::errc{} || parsed.ptr != name.data() + name.size()) return std::nullopt;
		return ordinal;
	}
	// Checked reimport changes physical source positions; only the writer's comparison copy rebases.
	inline void RebaseInputProvenance(imagegraph::Document &comparison, const imagegraph::Document &saved) {
		for (auto &node : comparison.Nodes) {
			if (node.Type != "pc.hlsl") continue;
			const auto source =
				std::find_if(saved.Nodes.begin(), saved.Nodes.end(), [&](const auto &candidate) {
					return candidate.Id == node.Id;
				});
			if (source == saved.Nodes.end()) continue;
			for (auto &input : node.DynamicInputs) {
				const auto found = std::find_if(
					source->DynamicInputs.begin(), source->DynamicInputs.end(), [&](const auto &candidate) {
						return candidate.Id == input.Id;
					}
				);
				if (found != source->DynamicInputs.end()) input.SourceInputId = found->SourceInputId;
			}
		}
	}

}
