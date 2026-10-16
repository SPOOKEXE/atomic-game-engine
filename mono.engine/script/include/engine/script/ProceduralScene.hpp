#pragma once

// Fixed-barrier materialization of canonical regional artifacts.
// @tier L9 · shared
#include <engine/ecs/Entity.hpp>
#include <engine/scene/Components.hpp>
#include <engine/script/ProceduralJobs.hpp>

#include <span>
#include <string>
#include <vector>

namespace engine::script {
	// Stable object identity and its canonical aggregate triangle-index run.
	// These ranges describe ECS geometry, before any presentation repacking.
	struct ProceduralObjectRange {
		std::string Id{};		 // Stable artifact placement or road identity.
		std::string Kind{};		 // Named geometry kind from the shared pure builder.
		uint32_t FirstIndex = 0; // First aggregate triangle-list index.
		uint32_t IndexCount = 0; // Complete triangle-list run length.
		uint16_t Material = 0;	 // Artifact material choice.
		uint16_t Variant = 0;	 // Artifact variant choice.
	};
	// Derived regional view on the EditableMesh row. Cleared when loaded.
	struct ProceduralSceneRegion {
		procedural::RegionScope Scope{};			  // Region owned by this materialization.
		procedural::Digest ArtifactDigest{};		  // Exact canonical artifact backing this view.
		std::vector<ProceduralObjectRange> Objects{}; // Stable identity table in aggregate order.
	};
	// Owner-local binding captured before generation. Never serialized or sent
	// between worlds. Any intervening target edit refuses publication.
	struct ProceduralSceneTarget {
		procedural::RegionScope Scope{}; // Region owned by this materialization.
		ecs::Entity Mesh{};				 // Full generation-bearing EditableMesh handle.
		ecs::Entity Part{};				 // Full generation-bearing static MeshPart handle.
		uint64_t StoreIdentity = 0;		 // Exact owner-world incarnation fence.
		uint32_t MeshRevision = 0;		 // Geometry revision observed before generation.
		core::Name Content{};			 // Existing Mesh and Fitted content binding.
		core::CFrame Frame{};			 // Captured transform; intervening edits refuse publication.
		core::CFrame Previous{};		 // Captured interpolation history.
		core::Vector3 HalfExtent{};		 // Captured display bounds.
	};
	// Called by RegisterProceduralComponents before the component registry seals.
	void RegisterProceduralSceneComponents();
	// Requires a static MeshPart naming the mesh. Establishes Fitted and the
	// derived component before work starts. Aggregate output is visual-only:
	// collider queries and contact masks must already be disabled by the author.
	bool BindProceduralSceneTarget(
		ecs::Store &store,
		procedural::RegionScope scope,
		ecs::Entity mesh,
		ecs::Entity part,
		ProceduralSceneTarget &out,
		procedural::Diagnostic &diagnostic
	);
	// Recreates visible geometry from accepted canonical RGA1 artifacts without
	// compiling recipes or running GenerateRegion. All targets publish together.
	// Ordinary baked .amesh assets use the separate asset loading path.
	bool MaterializePublishedProceduralRegions(
		ecs::Store &store, std::span<const ProceduralSceneTarget> targets, procedural::Diagnostic &diagnostic
	);
}
