#pragma once

// Bounded, exact source geometry retained by a single world-thread scene export.
// @tier L9 · shared

#include <engine/assets/Mesh.hpp>
#include <engine/assets/Texture.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Color3.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::script {
	inline constexpr size_t MAX_GLTF_EXPORT_NODES = 256;
	inline constexpr size_t MAX_GLTF_EXPORT_VERTICES = 262144;
	inline constexpr size_t MAX_GLTF_EXPORT_INDICES = 786432;
	inline constexpr size_t MAX_GLTF_EXPORT_GEOMETRY_BYTES = 48u * 1024u * 1024u;
	inline constexpr size_t MAX_GLTF_EXPORT_TEXTURES = 64;
	inline constexpr size_t MAX_GLTF_EXPORT_SOURCE_TEXTURE_BYTES = 16u * 1024u * 1024u;
	inline constexpr size_t MAX_GLTF_EXPORT_TEXTURE_BYTES = 256u * 1024u * 1024u;
	inline constexpr size_t MAX_GLTF_EXPORT_LIGHTS = 256;

	enum class GltfExportAlphaMode : uint8_t {
		Opaque,
		Mask,
		Blend,
	};

	struct GltfExportTexture {
		std::string Name;
		uint32_t Width = 0;
		uint32_t Height = 0;
		std::vector<uint8_t> Pixels;
	};

	// Original resident image bytes retained for extract-only consumers. This is
	// separate from `Textures`, whose pixels are intentionally converted for GLB.
	struct GltfExportSourceTexture {
		std::string Name;
		uint32_t Width = 0;
		uint32_t Height = 0;
		assets::TextureFormat Format = assets::TextureFormat::RGBA8;
		std::vector<std::byte> Pixels;
	};

	struct GltfExportMaterial {
		core::Color3 BaseColour{1.0f, 1.0f, 1.0f};
		float Alpha = 1.0f;
		GltfExportAlphaMode AlphaMode = GltfExportAlphaMode::Opaque;
		float AlphaCutoff = 0.5f;
		float RoughnessFactor = 0.65f;
		float MetalnessFactor = 0.0f;
		core::Color3 EmissiveFactor{0.0f, 0.0f, 0.0f};
		bool Pixelated = false;
		std::optional<size_t> ColourTexture;
		std::optional<size_t> NormalTexture;
		std::optional<size_t> OcclusionTexture;
		std::optional<size_t> EmissiveTexture;
		std::optional<size_t> MetallicRoughnessTexture;
		std::optional<size_t> SourceColourTexture;
		std::optional<size_t> SourceNormalTexture;
		std::optional<size_t> SourceOcclusionTexture;
		std::optional<size_t> SourceEmissiveTexture;
		std::optional<size_t> SourceRoughnessTexture;
		std::optional<size_t> SourceMetalnessTexture;
		std::optional<size_t> SourceHeightTexture;
		std::string SourceAlphaMode = "opaque";
		std::string SourceShader;
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
		std::vector<std::optional<size_t>> SubmeshColourTextures;
		std::vector<std::optional<size_t>> SourceSubmeshColourTextures;
		bool Visible = true;
	};

	struct GltfExportCamera {
		std::string StableId;
		std::string Name;
		core::CFrame Frame;
		float FieldOfViewRadians = 1.22f;
		float NearPlaneMetres = 0.1f;
		float FarPlaneMetres = 500.0f;
	};

	// Raw source facts for a point light. Brightness is the engine's authored
	// scalar, not a photometric intensity.
	struct GltfExportLight {
		std::string StableId;
		std::string Name;
		core::Vector3 Position;
		core::Color3 Colour{1.0f, 1.0f, 1.0f};
		float Brightness = 1.0f;
		float Range = 8.0f;
	};

	struct GltfExportUnavailable {
		std::string StableId;
		std::string Feature;
		std::string Reason;
	};

	struct GltfSceneExport {
		uint64_t Tick = 0;
		std::vector<GltfExportMesh> Meshes;
		std::vector<GltfExportTexture> Textures;
		std::vector<GltfExportSourceTexture> SourceTextures;
		std::vector<GltfExportNode> Nodes;
		std::vector<GltfExportCamera> Cameras;
		std::vector<GltfExportLight> Lights;
		std::vector<GltfExportUnavailable> Unavailable;
	};

	// A host may expose already resident source geometry without making this
	// shared module depend on a renderer or a content delivery client.
	enum class GltfMeshSourceStatus : uint8_t {
		Available,
		Missing,
		OverLimit,
		Unsupported,
		Invalid,
	};
	using GltfMeshSource = std::function<
		GltfMeshSourceStatus(std::string_view world, std::string_view name, assets::MeshData &out)>;
	enum class GltfTextureSourceStatus : uint8_t {
		Available,
		Missing,
		OverLimit,
		Unsupported,
		Invalid,
	};
	using GltfTextureSource = std::function<
		GltfTextureSourceStatus(std::string_view world, std::string_view name, assets::TextureData &out)>;

	// Captures source geometry available in `store` without deriving triangles
	// from a collider or a bound. The optional host source copies a bounded
	// delivered mesh from its exact content namespace.
	bool CaptureGltfSceneExport(
		ecs::Store &store,
		GltfSceneExport &out,
		std::string &failure,
		const GltfMeshSource &meshSource = {},
		const GltfTextureSource &textureSource = {},
		bool extractOnly = false
	);
}
