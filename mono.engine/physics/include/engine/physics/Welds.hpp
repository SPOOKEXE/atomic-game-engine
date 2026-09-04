#pragma once

// arch-waiver public-header: forward physics API. Body assembly hosts use this
// complete weld constraint contract.

// Resolving Weld and WeldConstraint instances into rigid assemblies.
//
// The authored rows live in scene. This pass captures the implicit relative
// frame of WeldConstraint, builds deterministic connected components, and
// projects every simulated member from one stable root before collision.
//
// @tier L8 · shared

namespace engine::ecs {
	class Store;
}

namespace engine::physics {
	void SolveRigidJoints(ecs::Store &store);
}
