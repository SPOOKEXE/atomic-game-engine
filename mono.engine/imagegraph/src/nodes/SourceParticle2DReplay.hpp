#pragma once
#include "SourceParticle2DState.hpp"
namespace engine::imagegraph::detail {
	bool EncodeSourceParticle2DReceipt(
		NodeContext &, const SourceParticle2DState &, StructValue &, AllocationReservation &
	);
	bool DecodeSourceParticle2DReceipt(NodeContext &, const StructValue &, SourceParticle2DState &);
}
