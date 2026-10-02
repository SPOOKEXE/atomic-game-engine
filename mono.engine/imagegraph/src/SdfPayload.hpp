#pragma once
#include <engine/imagegraph/SourceSdf.hpp>
namespace engine::imagegraph::detail {
	uint64_t SdfStorageBytes(const SdfValue &value, bool retained);
	bool ValidSdfPayload(const SdfValue &value);
}
