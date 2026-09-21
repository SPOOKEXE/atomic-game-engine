#pragma once

#include <engine/scene/SurfaceCameras.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::scene {
	// Maximum foreign portal crossings advertised by one world snapshot.
	inline constexpr size_t MAX_CAMERA_PORTAL_SEAMS = 256;
	// Maximum encoded bytes accepted for one world topology snapshot.
	inline constexpr size_t MAX_CAMERA_PORTAL_TOPOLOGY_BYTES = 512 * 1024;

	// Owned crossing data. Local pane, camera and surface identities stay home.
	struct CameraPortalMouth {
		// Authored seam name within the source world.
		std::string Name;
		// Stable destination-world name carried across hosts.
		std::string DestinationWorld;
		// Full instance paths identify the exact two mouths within worlds that
		// contain several portals with the same destination.
		std::string PanePath;
		// Full instance path of the paired mouth in the destination world.
		std::string FarPath;
		// Source mouth centre in world-space XYZ metres.
		std::array<float, 3> Centre{};
		// Unit outward normal of the source mouth plane.
		std::array<float, 3> Normal{0, 0, 1};
		// First unit in-plane basis vector for source mouth geometry.
		std::array<float, 3> First{1, 0, 0};
		// Second unit in-plane basis vector for source mouth geometry.
		std::array<float, 3> Second{0, 1, 0};
		// Source mouth up vector used to preserve camera orientation.
		std::array<float, 3> Up{};
		// Destination position XYZ, then unit quaternion XYZW.
		std::array<float, 7> Destination{0, 0, 0, 0, 0, 0, 1};
		// Positive rigid-seam scale applied to position and lens distances.
		float Scale = 1;
		// Whether the destination world may traverse this seam in reverse.
		bool Bidirectional = true;
		// Compares all copied seam geometry and destination identity.
		bool operator==(const CameraPortalMouth &) const = default;
	};
	// Complete foreign-camera crossing snapshot for one source world.
	struct CameraPortalTopology {
		// Stable source-world name that owns this topology.
		std::string World;
		// Monotonic topology revision for this world.
		uint64_t Revision = 0;
		// Empty means a complete world with no foreign crossings, not unavailable.
		std::vector<CameraPortalMouth> Mouths;
		// Compares source identity, revision, and complete mouth set.
		bool operator==(const CameraPortalTopology &) const = default;
	};

	// Copies one live seam into owned host-transport geometry.
	bool CopyCameraPortalMouth(
		std::string_view name, const PortalSeam &seam, CameraPortalMouth &out, std::string &error
	);
	// Codecs validate complete snapshots before replacing output, without interning
	// text. The host must authenticate world and incarnation before resolving names.
	// Encodes one bounded topology snapshot without interning names.
	bool EncodeCameraPortalTopology(
		const CameraPortalTopology &topology, std::vector<std::byte> &out, std::string &error
	);
	// Decodes a complete topology snapshot without replacing output on failure.
	bool DecodeCameraPortalTopology(
		std::span<const std::byte> bytes, CameraPortalTopology &out, std::string &error
	);
	// Resolves copied seam geometry into local camera portal seams.
	bool ResolveCameraPortalTopology(
		const CameraPortalTopology &topology, std::vector<PortalSeam> &out, std::string &error
	);
}
