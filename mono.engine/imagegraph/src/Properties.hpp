#pragma once

#include <engine/imagegraph/Document.hpp>

#include <nlohmann/json_fwd.hpp>
#include <string>

namespace engine::imagegraph::detail {
	// Range and per-kind rules for one operation; empty when valid.
	std::string Validate(const Operation &);
	// Strict project JSON for one node's properties. Throws std::runtime_error on refusal.
	Operation DecodeOperation(std::string_view kind, const nlohmann::json &properties, bool live);
	// Canonical project JSON for one node's properties.
	nlohmann::json EncodeOperation(const Operation &, bool live);
	// Bounded relative path or local editable image reference accepted as a source.
	bool SourceValid(std::string_view path);
}
