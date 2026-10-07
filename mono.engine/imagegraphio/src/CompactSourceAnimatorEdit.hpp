#pragma once

// Source compact animators carry one value; future fields remain owned by the archive.
#include <nlohmann/json.hpp>
#include <utility>

namespace engine::imagegraphio::detail {
	inline bool
	WriteCompactSourceAnimatorValue(nlohmann::ordered_json &input, nlohmann::ordered_json encoded) {
		if (!input.is_object()) return false;
		const auto animator = input.find("r");
		if (animator != input.end() && (!animator->is_object() || !animator->contains("d"))) return false;
		// node_keyframe.deserialize reads only d for compact records. Replacing r loses retained fields.
		if (animator == input.end()) input["r"] = nlohmann::ordered_json::object();
		input["r"]["d"] = std::move(encoded);
		return true;
	}
}
