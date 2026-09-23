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
	// Maximum scene nodes captured in one bounded export.
	inline constexpr size_t MAX_GLTF_EXPORT_NODES = 256;
	// Maximum mesh vertices retained across one export.
	inline constexpr size_t MAX_GLTF_EXPORT_VERTICES = 262144;
	// Maximum mesh indices retained across one export.
	inline constexpr size_t MAX_GLTF_EXPORT_INDICES = 786432;
	// Maximum encoded mesh geometry bytes retained across one export.
	inline constexpr size_t MAX_GLTF_EXPORT_GEOMETRY_BYTES = 48u * 1024u * 1024u;
	// Maximum converted textures retained across one export.
	inline constexpr size_t MAX_GLTF_EXPORT_TEXTURES = 64;
	// Maximum original resident-image bytes retained for extract-only export.
	inline constexpr size_t MAX_GLTF_EXPORT_SOURCE_TEXTURE_BYTES = 16u * 1024u * 1024u;
	// Maximum converted image bytes retained for GLB export.
	inline constexpr size_t MAX_GLTF_EXPORT_TEXTURE_BYTES = 256u * 1024u * 1024u;
	// Maximum point lights captured in one bounded export.
	inline constexpr size_t MAX_GLTF_EXPORT_LIGHTS = 256;

	// glTF material alpha treatment selected by the captured source material.
	enum class GltfExportAlphaMode : uint8_t {
		Opaque,
		Mask,
		Blend,
	};

	// Converted RGBA image retained for GLB image emission.
	struct GltfExportTexture {
		// Stable texture name used by exported material references.
		std::string Name;
		// Image width in pixels.
		uint32_t Width = 0;
		// Image height in pixels.
		uint32_t Height = 0;
		// Converted RGBA pixel bytes in row-major image order.
		std::vector<uint8_t> Pixels;
	};

	// Original resident image bytes retained for extract-only consumers. This is
	// separate from `Textures`, whose pixels are intentionally converted for GLB.
	struct GltfExportSourceTexture {
		// Script-visible name.
		std::string Name;
		// Image width in pixels.
		uint32_t Width = 0;
		// Image height in pixels.
		uint32_t Height = 0;
		// Original resident image format preserved for extract-only consumers.
		assets::TextureFormat Format = assets::TextureFormat::RGBA8;
		// Raw pixel payload.
		std::vector<std::byte> Pixels;
	};

	// Captured material inputs used to build one glTF material.
	struct GltfExportMaterial {
		// Base colour.
		core::Color3 BaseColour{1.0f, 1.0f, 1.0f};
		// Base-colour alpha multiplier captured from the source material.
		float Alpha = 1.0f;
		// Alpha mode.
		GltfExportAlphaMode AlphaMode = GltfExportAlphaMode::Opaque;
		// Alpha cutoff.
		float AlphaCutoff = 0.5f;
		// Roughness factor.
		float RoughnessFactor = 0.65f;
		// Metalness factor.
		float MetalnessFactor = 0.0f;
		// Fraction of incoming light transmitted through the material.
		float TransmissionFactor = 0.0f;
		// Dielectric index of refraction used by transmission shading.
		float IndexOfRefraction = 1.5f;
		// Authored surface thickness used by volume transmission.
		float Thickness = 0.0f;
		// Emissive factor.
		core::Color3 EmissiveFactor{0.0f, 0.0f, 0.0f};
		// Whether texture sampling requests nearest-neighbour filtering.
		bool Pixelated = false;
		// Colour texture.
		std::optional<size_t> ColourTexture;
		// Normal texture.
		std::optional<size_t> NormalTexture;
		// Occlusion texture.
		std::optional<size_t> OcclusionTexture;
		// Emissive texture.
		std::optional<size_t> EmissiveTexture;
		// Metallic roughness texture.
		std::optional<size_t> MetallicRoughnessTexture;
		// Source colour texture.
		std::optional<size_t> SourceColourTexture;
		// Source normal texture.
		std::optional<size_t> SourceNormalTexture;
		// Source occlusion texture.
		std::optional<size_t> SourceOcclusionTexture;
		// Source emissive texture.
		std::optional<size_t> SourceEmissiveTexture;
		// Source roughness texture.
		std::optional<size_t> SourceRoughnessTexture;
		// Source metalness texture.
		std::optional<size_t> SourceMetalnessTexture;
		// Source height texture.
		std::optional<size_t> SourceHeightTexture;
		// Source alpha mode.
		std::string SourceAlphaMode = "opaque";
		// Source shader.
		std::string SourceShader;
	};

	// Named mesh geometry retained for one glTF mesh entry.
	struct GltfExportMesh {
		// Script-visible name.
		std::string Name;
		// Exact mesh geometry copied from the host content namespace.
		assets::MeshData Data;
	};

	// One visible scene instance referencing captured mesh and material data.
	struct GltfExportNode {
		// Stable authored identity.
		std::string StableId;
		// Script-visible name.
		std::string Name;
		// World-space transform.
		core::CFrame Frame;
		// Authored per-axis scale applied after Frame.
		core::Vector3 Scale{1.0f, 1.0f, 1.0f};
		// Index into GltfSceneExport::Meshes.
		size_t Mesh = 0;
		// Captured material inputs for this mesh instance.
		GltfExportMaterial Material;
		// Submesh colour textures.
		std::vector<std::optional<size_t>> SubmeshColourTextures;
		// Source submesh colour textures.
		std::vector<std::optional<size_t>> SourceSubmeshColourTextures;
		// Whether the authored node was visible when captured.
		bool Visible = true;
	};

	// Captured authored camera with glTF-compatible projection values.
	struct GltfExportCamera {
		// Stable authored identity.
		std::string StableId;
		// Script-visible name.
		std::string Name;
		// World-space transform.
		core::CFrame Frame;
		// Vertical camera field of view in radians.
		float FieldOfViewRadians = 1.22f;
		// Near plane metres.
		float NearPlaneMetres = 0.1f;
		// Far plane metres.
		float FarPlaneMetres = 500.0f;
	};

	// Raw source facts for a point light. Brightness is the engine's authored
	// scalar, not a photometric intensity.
	struct GltfExportLight {
		// Stable authored identity.
		std::string StableId;
		// Script-visible name.
		std::string Name;
		// World-space position.
		core::Vector3 Position;
		// Authored point-light colour.
		core::Color3 Colour{1.0f, 1.0f, 1.0f};
		// Engine-authored non-photometric point-light brightness.
		float Brightness = 1.0f;
		// Authored point-light attenuation range in metres.
		float Range = 8.0f;
	};

	// Captured feature that could not be represented by this export.
	struct GltfExportUnavailable {
		// Stable authored identity.
		std::string StableId;
		// Name of the scene feature omitted from this export.
		std::string Feature;
		// Reason the value is unavailable.
		std::string Reason;
	};

	// Complete bounded scene snapshot consumed by the glTF encoder.
	struct GltfSceneExport {
		// World tick for this record.
		uint64_t Tick = 0;
		// Named mesh geometry referenced by captured nodes.
		std::vector<GltfExportMesh> Meshes;
		// Converted images referenced by material texture indices.
		std::vector<GltfExportTexture> Textures;
		// Original resident images returned only to extract-only callers.
		std::vector<GltfExportSourceTexture> SourceTextures;
		// Captured visible scene instances in stable traversal order.
		std::vector<GltfExportNode> Nodes;
		// Captured authored cameras available to export consumers.
		std::vector<GltfExportCamera> Cameras;
		// Captured authored point lights.
		std::vector<GltfExportLight> Lights;
		// Features omitted from export with their reasons.
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
	// Host callback that copies named resident mesh geometry for export.
	using GltfMeshSource = std::function<
		GltfMeshSourceStatus(std::string_view world, std::string_view name, assets::MeshData &out)>;
	// Outcome of a host request for named resident image data.
	enum class GltfTextureSourceStatus : uint8_t {
		Available,
		Missing,
		OverLimit,
		Unsupported,
		Invalid,
	};
	// Host callback that copies named resident texture data for export.
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
