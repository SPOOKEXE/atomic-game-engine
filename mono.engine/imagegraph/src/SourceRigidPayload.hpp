#pragma once
#include <engine/imagegraph/Document.hpp>
namespace engine::imagegraph::detail {
	template <bool Retained> uint64_t RigidStorageBytes(const RigidValue &value) {
		if (!value.Data) return 0;
		const auto &data = *value.Data;
		return sizeof(RigidObjectData) + (Retained ? data.OwnerId.capacity() : data.OwnerId.size()) +
			   (Retained ? data.BodyId.capacity() : data.BodyId.size());
	}
	inline bool ValidRigidPayload(const RigidValue &value) {
		if (!value.Data) return true;
		return !value.Data->OwnerId.empty() && value.Data->OwnerId.size() <= Limits::MaximumTextBytes &&
			   !value.Data->BodyId.empty() && value.Data->BodyId.size() <= 256;
	}
} // namespace engine::imagegraph::detail
