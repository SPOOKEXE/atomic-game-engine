#pragma once
#include <engine/imagegraph/Document.hpp>

namespace engine::imagegraph::detail {
	uint64_t AtlasStorageBytes(const AtlasValue &value, bool retained);
	bool ValidAtlasPayload(const AtlasValue &value);
}
