#pragma once
#include <engine/assets/Mesh.hpp>
#include <engine/assets/ProceduralDescription.hpp>
#include <engine/procedural/Geometry.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/script/ProceduralScene.hpp>

namespace engine::scene {
	struct Collider;
}

namespace engine::script::procedural_scene_detail {
	// Document import stages fresh content and derived contact masks before
	// applying the same live target predicate.
	bool CaptureTarget(
		const ecs::Store &,
		procedural::RegionScope,
		ecs::Entity mesh,
		ecs::Entity part,
		core::Name expectedBinding,
		const scene::Collider &candidateCollider,
		ProceduralSceneTarget &,
		procedural::Diagnostic &
	);
	struct StagedRegion {
		scene::EditableMeshUpdate Mesh{};
		ProceduralSceneRegion Region{};
		core::CFrame Frame{};
		core::Vector3 HalfExtent{};
	};
	// Installs the derived marker before nonthrowing part scalar normalization.
	bool ProvisionTarget(ecs::Store &, ProceduralSceneTarget &, procedural::Diagnostic &);
	bool CapturedTargetValid(const ecs::Store &, const ProceduralSceneTarget &);
	bool TargetValid(const ecs::Store &, const ProceduralSceneTarget &);
	// Shared live/frozen aggregate packing. Bounds come from the actual rebased
	// vertices. Mesh handle/revision and regional provenance in out are preserved.
	bool PackRegionGeometry(const procedural::RegionGeometry &, StagedRegion &out, procedural::Diagnostic &);
	// Borrowed only while packing; the decoded placement and mesh storage outlive the call.
	struct BakedGeometryView {
		const assets::ProceduralPlacement *Placement = nullptr;
		const assets::MeshData *Mesh = nullptr;
	};
	bool PackBakedGeometry(
		procedural::Point origin,
		std::span<const BakedGeometryView> objects,
		uint32_t vertices,
		uint32_t indices,
		StagedRegion &out,
		procedural::Diagnostic &
	);
	// All checks/marking/allocations precede the first mesh edit. The scene batch
	// never restructures ECS storage; only noexcept writes and swaps follow it.
	bool CommitRegions(
		ecs::Store &,
		std::span<const ProceduralSceneTarget>,
		std::span<StagedRegion>,
		ProceduralPublished *canonicalCandidate,
		std::span<ProceduralCompletion *> accepted,
		procedural::Diagnostic &
	);
}
