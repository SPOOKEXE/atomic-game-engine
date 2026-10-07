#pragma once
#include "SourceParticle3DState.hpp"
namespace engine::imagegraph::detail {
	// A single DataReplay value owns this constructor-bound receipt; no private persistent cache exists.
	struct SourceParticle3DReceipt {
		SourceParticle3DState State;
		AllocationReservation BaseCharge;
		MeshValue3D BaseMesh;
	};
	bool EncodeSourceParticle3DReceipt(
		NodeContext &,
		const SourceParticle3DState &,
		const MeshValue3D &,
		StructValue &,
		AllocationReservation &
	);
	bool DecodeSourceParticle3DReceipt(NodeContext &, const StructValue &, SourceParticle3DReceipt &);
}
