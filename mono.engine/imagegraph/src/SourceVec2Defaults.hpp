#pragma once
#include "SourceSeparatedVec2.hpp"
namespace engine::imagegraph::detail {
	Status ValidateSourceVec2Defaults(const Node &node, Diagnostic &diagnostic);
	std::optional<uint64_t> SourceVec2DefaultsBytes(const Node &node, bool retained);
	std::optional<Vector2> SourceVec2ConstructorDefault(const Node &node, std::string_view port);
}
