#pragma once

// Bounded, exact source geometry retained by a single world-thread scene export.
// @tier L9 · shared

#include <engine/assets/Mesh.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Color3.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::script {
	inline constexpr size_t MAX_GLTF_EXPORT_NODES = 256;
	inline constexpr size_t MAX_GLTF_EXPORT_VERTICES = 4096;
	inline constexpr size_t MAX_GLTF_EXPORT_INDICES = 12288;

	enum class GltfExportAlphaMode : uint8_t {
		Opaque,
		Blend,
	};

	struct GltfExportMaterial {
		core::Color3 BaseColour{1.0f, 1.0f, 1.0f};
		float Alpha = 1.0f;
		GltfExportAlphaMode AlphaMode = GltfExportAlphaMode::Opaque;
	};

	struct GltfExportMesh {
		std::string Name;
		assets::MeshData Data;
	};

	struct GltfExportNode {
		std::string StableId;
		std::string Name;
		core::CFrame Frame;
		core::Vector3 Scale{1.0f, 1.0f, 1.0f};
		size_t Mesh = 0;
		GltfExportMaterial Material;
	};

	struct GltfExportCamera {
		std::string StableId;
		std::string Name;
		core::CFrame Frame;
		float FieldOfViewRadians = 1.22f;
		float NearPlaneMetres = 0.1f;
		float FarPlaneMetres = 500.0f;
	};

	struct GltfExportUnavailable {
		std::string StableId;
		std::string Feature;
		std::string Reason;
	};

	struct GltfSceneExport {
		uint64_t Tick = 0;
		std::vector<GltfExportMesh> Meshes;
		std::vector<GltfExportNode> Nodes;
		std::vector<GltfExportCamera> Cameras;
		std::vector<GltfExportUnavailable> Unavailable;
	};

	// Captures source geometry available in `store` without deriving triangles
	// from a collider or a bound. Built-ins and EditableMesh rows are exact;
	// delivered mesh assets are unavailable until their source bytes have a
	// retained world-visible seam.
	bool CaptureGltfSceneExport(ecs::Store &store, GltfSceneExport &out, std::string &failure);
}
