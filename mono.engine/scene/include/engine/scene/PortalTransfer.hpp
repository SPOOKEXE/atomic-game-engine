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

	inline constexpr size_t MAXIMUM_PORTAL_NODES = 128;
	inline constexpr size_t MAXIMUM_PORTAL_BODY_BYTES = 262144;
	inline constexpr size_t MAXIMUM_PORTAL_ANIMATION_BYTES = 128 * 1024;

	struct PortalComponentCopy {
		std::string Type;
		std::vector<std::byte> Bytes;
		std::vector<std::string> References;
	};

	struct PortalNodeCopy {
		std::string Key;
		std::string Parent;
		std::string Class;
		std::string Name;
		std::vector<PortalComponentCopy> Components;
	};

	enum class PortalBodyKind : uint8_t { Player, Object };
	const char *Describe(PortalBodyKind kind);
	std::optional<PortalBodyKind> PortalBodyKindOf(std::string_view name);

	struct PortalBodySweep {
		core::CFrame From;
		core::Vector3 Displacement;
		core::Vector3 AngularDisplacement;
	};

	// arch-crossing: owned component bytes and names, never source-world handles.
	struct PortalBodyCopy {
		PortalBodyKind Kind = PortalBodyKind::Player;
		// Player, Character and Humanoid are empty for an ordinary object.
		std::string Player;
		std::string Character;
		std::string Root;
		std::string Humanoid;
		std::vector<PortalNodeCopy> Nodes;
		std::optional<PortalBodySweep> Sweep;
	};

	// Local handles returned after admission. Objects return only Root.
	struct PortalBodyArrival {
		ecs::Entity Player;
		ecs::Entity Character;
		ecs::Entity Root;
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
	bool CapturePortalObject(
		const ecs::Store &store, ecs::Entity object, PortalBodyCopy &out, std::string &failure
	);
	// Validates transport structure and typed references. Animation buffer bytes
	// remain opaque here; the asset reader validates clips before playback.
	bool ValidatePortalBody(const PortalBodyCopy &body, std::string &failure);
	bool WritePortalBody(core::ByteWriter &writer, const PortalBodyCopy &body);
	bool ReadPortalBody(core::ByteReader &reader, PortalBodyCopy &out);
	bool MapPortalBody(PortalBodyCopy &body, const SeamTransform &through, std::string &failure);
	bool AdmitPortalBody(
		ecs::Store &store, const PortalBodyCopy &body, PortalBodyArrival &out, std::string &failure
	);

}
