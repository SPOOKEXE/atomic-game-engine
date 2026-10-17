#pragma once

// A bounded copy of an actual character or movable part subtree, including
// owned animation definitions. Packet-local names replace source-world handles;
// component bytes use the registered lossless same-build codecs.

#include <engine/core/Bytes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/scene/SurfaceCameras.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace engine::scene {

	// Maximum copied nodes admitted in one portal body.
	inline constexpr size_t MAXIMUM_PORTAL_NODES = 128;
	// Maximum encoded bytes admitted in one portal body.
	inline constexpr size_t MAXIMUM_PORTAL_BODY_BYTES = 262144;
	// Maximum owned animation bytes admitted in one portal body.
	inline constexpr size_t MAXIMUM_PORTAL_ANIMATION_BYTES = 128 * 1024;

	// Lossless same-build copy of one component on a portal node.
	struct PortalComponentCopy {
		// Registered stable component type name.
		std::string Type;
		// Owned bytes emitted by the component's registered codec.
		std::vector<std::byte> Bytes;
		// Packet-local names referenced by the encoded component.
		std::vector<std::string> References;
	};

	// One node in a copied portal subtree.
	struct PortalNodeCopy {
		// Packet-local key used to connect parent and component references.
		std::string Key;
		// Packet-local key of this node's parent.
		std::string Parent;
		// Registered scene class used when admitting the node.
		std::string Class;
		// Authored node name reproduced in the destination world.
		std::string Name;
		// Owned component copies attached to this node.
		std::vector<PortalComponentCopy> Components;
	};

	// Transfer categories with different required character records.
	enum class PortalBodyKind : uint8_t { Player, Object };
	// Returns the persistent text name for a body category.
	const char *Describe(PortalBodyKind kind);
	// Parses a body category from its persistent text name.
	std::optional<PortalBodyKind> PortalBodyKindOf(std::string_view name);

	// Motion preserved when a body crosses a portal seam.
	struct PortalBodySweep {
		// Source pose at the crossing point.
		core::CFrame From;
		// Linear movement accumulated before the crossing.
		core::Vector3 Displacement;
		// Angular movement accumulated before the crossing.
		core::Vector3 AngularDisplacement;
	};

	// arch-crossing: owned component bytes and names, never source-world handles.
	struct PortalBodyCopy {
		// Character or ordinary-object transfer category.
		PortalBodyKind Kind = PortalBodyKind::Player;
		// Player, Character and Humanoid are empty for an ordinary object.
		std::string Player;
		// Packet-local key for the transferred character model.
		std::string Character;
		// Packet-local key for the transferred character root.
		std::string Root;
		// Packet-local key for the transferred humanoid.
		std::string Humanoid;
		// Nodes in parent-before-child admission order.
		std::vector<PortalNodeCopy> Nodes;
		// Optional motion to transform through the portal seam.
		std::optional<PortalBodySweep> Sweep;
		// The root was resting in the authoritative physics world. A sleeping body
		// has no Motion component, so this must travel beside the copied rows.
		bool RootSleeping = false;
		// Latest client input tick whose effect this copy's state already includes.
		// The destination skips forwarded input at or before it. Zero is unknown.
		uint64_t AppliedInputTick = 0;
	};

	// Local handles returned after admission. Objects return only Root.
	struct PortalBodyArrival {
		// Destination player entity for character transfers.
		ecs::Entity Player;
		// Destination character entity for character transfers.
		ecs::Entity Character;
		// Destination root entity for every transfer.
		ecs::Entity Root;
		// Destination humanoid entity for character transfers.
		ecs::Entity Humanoid;
	};

	// The host may explicitly retain components that belong only to this world's control state.
	bool CapturePortalBody(
		const ecs::Store &store,
		ecs::Entity player,
		PortalBodyCopy &out,
		std::string &failure,
		std::span<const ecs::ComponentId> localComponents = {}
	);
	// Captures one movable object subtree into an owned transport copy.
	bool CapturePortalObject(
		const ecs::Store &store, ecs::Entity object, PortalBodyCopy &out, std::string &failure
	);
	// Validates transport structure and typed references. Animation buffer bytes
	// remain opaque here; the asset reader validates clips before playback.
	bool ValidatePortalBody(const PortalBodyCopy &body, std::string &failure);
	// Encodes a validated portal body into the bounded wire form.
	bool WritePortalBody(core::ByteWriter &writer, const PortalBodyCopy &body);
	// Decodes one bounded portal body without partial output on failure.
	bool ReadPortalBody(core::ByteReader &reader, PortalBodyCopy &out);
	// Transforms copied poses and motion through a portal seam.
	bool MapPortalBody(PortalBodyCopy &body, const SeamTransform &through, std::string &failure);
	// Creates a destination subtree and returns its local entity handles.
	bool AdmitPortalBody(
		ecs::Store &store, const PortalBodyCopy &body, PortalBodyArrival &out, std::string &failure
	);

}
