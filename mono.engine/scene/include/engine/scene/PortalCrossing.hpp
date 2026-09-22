#pragma once

// Persistent body identity and the portal facts needed while one body occupies
// a seam. Pose, motion and animation remain in their existing components.
//
// @tier L7 · shared

#include <engine/core/Bytes.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/scene/SurfaceCameras.hpp>

#include <cstdint>
#include <span>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {
	struct BodyKey {
		uint64_t High = 0;
		uint64_t Low = 0;

		constexpr bool IsValid() const {
			return High != 0 || Low != 0;
		}
		constexpr bool operator==(const BodyKey &) const = default;
	};

	struct BodyIdentity {
		BodyKey Key;
		uint64_t Generation = 1;
	};

	// One authority owns one monotonically assigned key space. This is a world
	// resource so a save, replay, or transferred host continues the sequence
	// instead of minting a process-local replacement identity.
	struct BodyIdentityAuthority {
		uint64_t Namespace = 0;
		uint64_t NextSequence = 1;
	};

	enum class PortalCrossingPhase : uint8_t { Idle, Overlapping, Prepared, Committed };

	// The geometry and route selected on entry. A moving, retargeted, or closing
	// mouth cannot change a body already spanning it; this stays live until the
	// body has fully cleared the original aperture.
	struct PortalSeamPin {
		ecs::Entity Pane = ecs::NULL_ENTITY;
		ecs::Entity Far = ecs::NULL_ENTITY;
		core::CFrame CentreFrame;
		core::CFrame Destination;
		core::Vector3 Normal;
		core::Vector3 First;
		core::Vector3 Second;
		core::Name DestinationWorld;
		float Scale = 1.0f;
		uint32_t TagFilter = 0;
		int16_t Surface = 0;
		bool Crosses = false;
		bool Bidirectional = true;
		uint8_t Reserved[4] = {};
	};

	// Portal-only state for one canonical body. `Reference` names the entity
	// whose existing Transform decides ownership. There is no copied pose,
	// velocity or animation state here.
	struct PortalCrossingState {
		PortalSeamPin Pin;
		ecs::Entity Reference = ecs::NULL_ENTITY;
		ecs::Entity Pane = ecs::NULL_ENTITY;
		ecs::Entity Far = ecs::NULL_ENTITY;
		uint64_t TopologyRevision = 0;
		uint64_t AuthorityEpoch = 0;
		uint64_t PresentationRevision = 0;
		int8_t StableSide = 1;
		int8_t ReferenceSide = 1;
		PortalCrossingPhase Phase = PortalCrossingPhase::Idle;
		uint8_t Reserved[5] = {};
	};

	inline constexpr float PORTAL_CROSSING_HYSTERESIS = 0.001f;

	// Sets the stable authority namespace before bodies are allocated. A zero
	// namespace is refused and an authority with allocated keys cannot change
	// namespaces.
	bool ConfigureBodyIdentityAuthority(ecs::Store &store, uint64_t nameSpace, uint64_t nextSequence = 1);
	BodyIdentity MintBodyIdentity(ecs::Store &store);
	bool AssignBodyIdentity(ecs::Store &store, ecs::Entity body, const BodyIdentity &identity);
	bool EnsureBodyIdentity(ecs::Store &store, ecs::Entity body, BodyIdentity &out);
	// Conservative radius of this body tree around its reference anchor. It
	// includes descendant bounds so a trailing limb or equipped tool keeps the
	// old chart alive until it has actually cleared the aperture.
	float
	PortalBodyReach(const ecs::Store &store, ecs::Entity body, ecs::Entity reference = ecs::NULL_ENTITY);
	bool AdvancePortalCrossing(
		ecs::Store &store, ecs::Entity body, std::span<const PortalSeam> seams, float reach
	);
	bool PinnedPortalSeam(const PortalCrossingState &crossing, PortalSeam &out);
	// Resolves a pinned route against the matching live endpoint transforms while
	// retaining its paired destination and entry scale until clearance.
	bool
	PinnedPortalSeam(const PortalCrossingState &crossing, std::span<const PortalSeam> seams, PortalSeam &out);
	bool CommitPortalCrossing(ecs::Store &store, ecs::Entity body, uint64_t authorityEpoch);
	bool CancelPortalCrossing(ecs::Store &store, ecs::Entity body);

	void WriteBodyIdentities(core::ByteWriter &writer, const void *source, size_t count);
	void ReadBodyIdentities(core::ByteReader &reader, void *destination, size_t count);
}
