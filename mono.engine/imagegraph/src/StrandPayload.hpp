#pragma once
#include "MeshPayload.hpp"
#include "SourceStrandState.hpp"
namespace engine::imagegraph::detail {
	template <bool Retained> uint64_t StrandStorageBytes(const StrandValue &value) {
		if (!value.Data) return 0;
		const auto &data = *value.Data;
		uint64_t bytes = MeshAddBytes(
			sizeof(StrandData2D), (Retained ? data.OriginNodeId.capacity() : data.OriginNodeId.size())
		);
		bytes = MeshAddBytes(bytes, MeshVectorBytes<Retained>(data.State.Hairs));
		for (const auto &hair : data.State.Hairs) {
			bytes = MeshAddBytes(bytes, MeshVectorBytes<Retained>(hair.Points));
			bytes = MeshAddBytes(bytes, MeshVectorBytes<Retained>(hair.Lengths));
			bytes = MeshAddBytes(bytes, MeshVectorBytes<Retained>(hair.RestAngles));
		}
		return bytes;
	}
	inline bool ValidStrandPayload(const StrandValue &value) {
		return !value.Data || (value.Data->OriginNodeId.size() <= Limits::MaximumTextBytes &&
							   value.Data->OriginProcessorRow < Limits::MaximumArrayElements &&
							   ValidSourceStrand(value.Data->State) &&
							   StrandStorageBytes<true>(value) <= Limits::MaximumEvaluationBytes);
	}
} // namespace engine::imagegraph::detail
