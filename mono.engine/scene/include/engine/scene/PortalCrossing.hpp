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
	// Persistent two-part key for a body crossing world boundaries.
	struct BodyKey {
		// High half of the persistent key.
		uint64_t High = 0;
		// Low half of the persistent key.
		uint64_t Low = 0;

		// Reports whether either key half is nonzero.
		constexpr bool IsValid() const {
			return High != 0 || Low != 0;
		}
		// Compares both persistent key halves.
		constexpr bool operator==(const BodyKey &) const = default;
	};

	// Persistent body key plus its incarnation generation.
	struct BodyIdentity {
		// Stable key allocated by the authority.
		BodyKey Key;
		// Incarnation that rejects recycled identities.
		uint64_t Generation = 1;
	};

	// One authority owns one monotonically assigned key space. This is a world
	// resource so a save, replay, or transferred host continues the sequence
	// instead of minting a process-local replacement identity.
	struct BodyIdentityAuthority {
		// Stable authority namespace for minted body keys.
		uint64_t Namespace = 0;
		// Next sequence in this authority's key space.
		uint64_t NextSequence = 1;
	};

	// Lifecycle phase for a body occupying a portal seam.
	enum class PortalCrossingPhase : uint8_t { Idle, Overlapping, Prepared, Committed };

	// The geometry and route selected on entry. A moving, retargeted, or closing
	// mouth cannot change a body already spanning it; this stays live until the
	// body has fully cleared the original aperture.
	struct PortalSeamPin {
		// Entry portal pane selected on overlap.
		ecs::Entity Pane = ecs::NULL_ENTITY;
		// Paired destination portal endpoint.
		ecs::Entity Far = ecs::NULL_ENTITY;
		// Entry aperture frame at pin time.
		core::CFrame CentreFrame;
		// Destination aperture frame at pin time.
		core::CFrame Destination;
		// Entry aperture normal.
		core::Vector3 Normal;
		// First entry aperture half-axis.
		core::Vector3 First;
		// Second entry aperture half-axis.
		core::Vector3 Second;
		// Stable destination world name.
		core::Name DestinationWorld;
		// Uniform source-to-destination chart scale.
		float Scale = 1.0f;
		// Tags permitted to use this route.
		uint32_t TagFilter = 0;
		// Surface index selected on the portal pane.
		int16_t Surface = 0;
		// Whether the pinned route crosses a world boundary.
		bool Crosses = false;
		// Whether the route permits both crossing directions.
		bool Bidirectional = true;
		// Reserved serialized storage.
		uint8_t Reserved[4] = {};
	};

	// Portal-only state for one canonical body. `Reference` names the entity
	// whose existing Transform decides ownership. There is no copied pose,
	// velocity or animation state here.
	struct PortalCrossingState {
		// Entry route pinned for the duration of the crossing.
		PortalSeamPin Pin;
		// Entity whose transform determines crossing ownership.
		ecs::Entity Reference = ecs::NULL_ENTITY;
		// Current local entry pane.
		ecs::Entity Pane = ecs::NULL_ENTITY;
		// Current paired destination pane.
		ecs::Entity Far = ecs::NULL_ENTITY;
		// Portal topology revision captured at entry.
		uint64_t TopologyRevision = 0;
		// Authority epoch captured at entry.
		uint64_t AuthorityEpoch = 0;
		// Presentation revision captured at entry.
		uint64_t PresentationRevision = 0;
		// Last stable side of the seam.
		int8_t StableSide = 1;
		// Side containing Reference at this tick.
		int8_t ReferenceSide = 1;
		// Current portal-crossing lifecycle phase.
		PortalCrossingPhase Phase = PortalCrossingPhase::Idle;
		// Reserved serialized storage.
		uint8_t Reserved[5] = {};
	};

	// Distance band that stabilizes classification near a portal plane.
	inline constexpr float PORTAL_CROSSING_HYSTERESIS = 0.001f;

	// Sets the stable authority namespace before bodies are allocated. A zero
	// namespace is refused and an authority with allocated keys cannot change
	// namespaces.
	bool ConfigureBodyIdentityAuthority(ecs::Store &store, uint64_t nameSpace, uint64_t nextSequence = 1);
	// Mints the next persistent identity from the world's authority.
	BodyIdentity MintBodyIdentity(ecs::Store &store);
	// Assigns a persistent identity to one local body.
	bool AssignBodyIdentity(ecs::Store &store, ecs::Entity body, const BodyIdentity &identity);
	// Returns the body's identity, minting one when absent.
	bool EnsureBodyIdentity(ecs::Store &store, ecs::Entity body, BodyIdentity &out);
	// Conservative radius of this body tree around its reference anchor. It
	// includes descendant bounds so a trailing limb or equipped tool keeps the
	// old chart alive until it has actually cleared the aperture.
	float
	PortalBodyReach(const ecs::Store &store, ecs::Entity body, ecs::Entity reference = ecs::NULL_ENTITY);
	// Advances one body through portal crossing state for the current tick.
	bool AdvancePortalCrossing(
		ecs::Store &store, ecs::Entity body, std::span<const PortalSeam> seams, float reach
	);
	// Reconstructs the pinned seam from serialized crossing state.
	bool PinnedPortalSeam(const PortalCrossingState &crossing, PortalSeam &out);
	// Resolves a pinned route against the matching live endpoint transforms while
	// retaining its paired destination and entry scale until clearance.
	bool
	PinnedPortalSeam(const PortalCrossingState &crossing, std::span<const PortalSeam> seams, PortalSeam &out);
	// Commits a prepared crossing when its authority epoch still matches.
	bool CommitPortalCrossing(ecs::Store &store, ecs::Entity body, uint64_t authorityEpoch);
	// Cancels a body's active portal crossing state.
	bool CancelPortalCrossing(ecs::Store &store, ecs::Entity body);

	// Encodes a contiguous array of persistent body identities.
	void WriteBodyIdentities(core::ByteWriter &writer, const void *source, size_t count);
	// Decodes a contiguous array of persistent body identities.
	void ReadBodyIdentities(core::ByteReader &reader, void *destination, size_t count);
}
