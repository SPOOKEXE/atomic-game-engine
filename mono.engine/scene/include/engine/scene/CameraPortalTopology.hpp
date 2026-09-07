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
	inline constexpr size_t MAX_CAMERA_PORTAL_SEAMS = 256;
	inline constexpr size_t MAX_CAMERA_PORTAL_TOPOLOGY_BYTES = 256 * 1024;

	// Owned crossing data. Local pane, camera and surface identities stay home.
	struct CameraPortalMouth {
		std::string Name;
		std::string DestinationWorld;
		std::array<float, 3> Centre{};
		std::array<float, 3> Normal{0, 0, 1};
		std::array<float, 3> First{1, 0, 0};
		std::array<float, 3> Second{0, 1, 0};
		std::array<float, 3> Up{};
		// Destination position XYZ, then unit quaternion XYZW.
		std::array<float, 7> Destination{0, 0, 0, 0, 0, 0, 1};
		float Scale = 1;
		bool Bidirectional = true;
		bool operator==(const CameraPortalMouth &) const = default;
	};
	struct CameraPortalTopology {
		std::string World;
		uint64_t Revision = 0;
		// Empty means a complete world with no foreign crossings, not unavailable.
		std::vector<CameraPortalMouth> Mouths;
		bool operator==(const CameraPortalTopology &) const = default;
	};

	bool CopyCameraPortalMouth(
		std::string_view name, const PortalSeam &seam, CameraPortalMouth &out, std::string &error
	);
	// Codecs validate complete snapshots before replacing output, without interning
	// text. The host must authenticate world and incarnation before resolving names.
	bool EncodeCameraPortalTopology(
		const CameraPortalTopology &topology, std::vector<std::byte> &out, std::string &error
	);
	bool DecodeCameraPortalTopology(
		std::span<const std::byte> bytes, CameraPortalTopology &out, std::string &error
	);
	bool ResolveCameraPortalTopology(
		const CameraPortalTopology &topology, std::vector<PortalSeam> &out, std::string &error
	);
}
