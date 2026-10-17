#pragma once

#include <engine/core/types/Vector3.hpp>
#include <engine/physics/CopiedContacts.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::physics {
	// Stable value identity for one body participating in a cross-world island.
	// It deliberately contains no ECS entity: entity ids are local to one world.
	struct PortalIslandBodyId {
		// High half of the persistent body key.
		uint64_t KeyHigh = 0;
		// Low half of the persistent body key.
		uint64_t KeyLow = 0;
		// Incarnation of the persistent body key.
		uint64_t Generation = 0;
		// Sub-body index within the persistent body.
		uint32_t Part = 0;

		// Orders complete persistent body identities.
		constexpr auto operator<=>(const PortalIslandBodyId &) const = default;
	};

	// One dynamic body expressed in the coordinator's chosen chart.
	// Inverse inertia is diagonal in the already-mapped principal axes.
	struct PortalIslandBody {
		// Persistent identity of this solver body.
		PortalIslandBodyId Id;
		// Centre in the coordinator chart.
		core::Vector3 Centre;
		// Linear velocity in the coordinator chart.
		core::Vector3 LinearVelocity;
		// Angular velocity in the coordinator chart.
		core::Vector3 AngularVelocity;
		// Diagonal inverse inertia in PrincipalAxes.
		core::Vector3 InverseInertia;
		// Reciprocal mass used by the solver.
		float InverseMass = 0.0f;
		// Combined contact friction for this body.
		float Friction = 0.5f;
		// Combined contact restitution for this body.
		float Restitution = 0.0f;
		// Whether this body can participate in the solve.
		bool Available = true;
		// True only in the packet emitted by this body's owning world. A copied
		// far-side witness participates in the solve but receives no reply here.
		bool Owned = true;
		// Orientation of the diagonal inertia basis.
		std::array<core::Vector3, 3> PrincipalAxes{
			core::Vector3::XAxis, core::Vector3::YAxis, core::Vector3::ZAxis
		};
	};

	// A contact already narrowed against the finite aperture. The coordinator
	// supplies one chart, so physics only receives value records and no world.
	struct PortalIslandContact {
		// First body joined by this contact.
		PortalIslandBodyId First;
		// Second body joined by this contact.
		PortalIslandBodyId Second;
		// Contact point in the coordinator chart.
		core::Vector3 Point;
		// Contact normal from First toward Second.
		core::Vector3 Normal;
		// Positive interpenetration depth.
		float Penetration = 0.0f;
		// Contact friction coefficient.
		float Friction = 0.5f;
		// Contact restitution coefficient.
		float Restitution = 0.0f;
	};

	// Result copied back to the owning world at the barrier.
	struct PortalIslandResult {
		// Persistent identity of the updated body.
		PortalIslandBodyId Id;
		// Solved linear velocity.
		core::Vector3 LinearVelocity;
		// Solved angular velocity.
		core::Vector3 AngularVelocity;
	};

	// Why an island was not solved. An unavailable body blocks the crossing at
	// the rim instead of applying a delayed impulse on a later tick.
	enum class PortalIslandStatus : uint8_t { Complete, Unavailable, Invalid };
	// Versioned, bounded value packet used by thread and process seam barriers.
	struct PortalIslandPacket {
		// Solver bodies in this barrier packet.
		std::vector<PortalIslandBody> Bodies;
		// Aperture-clipped contacts between the bodies.
		std::vector<PortalIslandContact> Contacts;
	};
	// Builds one solver body from a copied dynamic contact. The axes preserve
	// the source shape orientation after it has crossed into the chosen chart.
	PortalIslandBody MakePortalIslandBody(const CopiedDynamicContact &body);

	// A deterministic sequential impulse solve over copied cross-world rows.
	// Inputs and outputs are sorted by persistent body identity.
	PortalIslandStatus SolvePortalIsland(
		std::vector<PortalIslandBody> &bodies,
		std::vector<PortalIslandContact> contacts,
		std::vector<PortalIslandResult> &results,
		uint32_t sweeps = 8
	);

	// A uniform chart scale changes inertia by scale squared under the portal
	// convention that keeps mass constant.
	core::Vector3 MapPortalInverseInertia(const core::Vector3 &sourceInverseInertia, float scale);

	// Maps a canonical-chart impulse back through a uniform portal Jacobian.
	// This is J transpose: it preserves virtual work across xB = s R xA.
	core::Vector3 MapPortalImpulseBack(const core::Vector3 &canonicalImpulse, float scale);
	// Encodes a bounded island packet for a seam barrier.
	bool WritePortalIslandPacket(std::vector<std::byte> &out, const PortalIslandPacket &packet);
	// Decodes a bounded island packet transactionally.
	bool ReadPortalIslandPacket(std::span<const std::byte> bytes, PortalIslandPacket &packet);
}
