#pragma once
#include <engine/assets/Texture.hpp>
#include <engine/graph/RenderGraph.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/HostCapture.hpp>
#include <engine/render/CookedComposer.hpp>

#include <array>

namespace engine::render::hlsl {
	inline constexpr uint64_t MAXIMUM_SURFACE_JOB_BYTES = 64ull * 1024 * 1024;
	struct CapturedDefinition {
		std::string Vertex, Main, Global, Libraries;
		std::vector<Argument> Arguments;
		Definition View() const {
			return {Vertex, Main, Global, Libraries, Arguments};
		}
	};
	// Source-slot texture order is durable for this request; backend compaction
	// happens at record time.
	struct SurfaceRequest {
		core::Name Shader;
		uint64_t ShaderRevision = 0;
		CookedPair Pair;
		std::vector<engine::imagegraph::Image> Textures;
		std::vector<std::byte> Uniforms;
		assets::TextureFormat Format = assets::TextureFormat::RGBA8_LINEAR;
		bool LinearSampling = false;
	};
	struct SurfaceLiveRequest {
		core::Name Owner, Name;
		uint64_t Generation = 0;
		SurfaceRequest Request;
	};
	struct SurfaceJob {
		graph::RenderGraph Graph;
		graph::CompiledGraph Schedule;
		graph::ResourceId Output;
	};
	std::optional<std::string> CaptureDefinition(
		const engine::imagegraph::Node &, const engine::imagegraph::EvaluationSnapshot &, CapturedDefinition &
	);
	// Authoring makes a separate bounded native document copy; no source archive
	// field is invented.
	std::optional<std::string> CookNode(
		const engine::imagegraph::Document &,
		const engine::imagegraph::Node &,
		const engine::imagegraph::EvaluationSnapshot &,
		std::span<const Library>,
		std::string_view compilerIdentity,
		bool includeMsl,
		core::Name shaderAsset,
		engine::imagegraph::Document &,
		assets::ShaderData &,
		std::string_view translatorIdentity = {},
		uint64_t maximumBytes = MAXIMUM_COOKED_OPERATION_BYTES
	);
	// Captured inputs are inspected once, with no upstream reevaluation or runtime
	// compilation.
	std::optional<std::string> BuildSurfaceRequest(
		const engine::imagegraph::Node &,
		const engine::imagegraph::EvaluationSnapshot &,
		const CookedComposerLibrary &,
		core::Name owner,
		bool displayColorSpace,
		SurfaceRequest &
	);
	std::optional<std::string> BuildSurfaceRequest(
		const engine::imagegraph::HostNodeInvocation &,
		const CookedComposerLibrary &,
		core::Name owner,
		bool displayColorSpace,
		SurfaceRequest &
	);
	std::optional<std::string> ValidateSurfaceRequest(const SurfaceRequest &);
	uint64_t SurfaceSourceBytes(const SurfaceRequest &);
	uint64_t SurfaceScratchBytes(const SurfaceRequest &);
	// Exact fixed-vertex block layout: three matrices with reflected major
	// ordering.
	std::array<float, 48> VertexMatrices(const CookedPair &, uint32_t width, uint32_t height);
	// Upload and raster are real frame-scoped graph nodes; output escapes through
	// fence publication.
	std::optional<std::string>
	BuildSurfaceJob(const SurfaceRequest &, SurfaceJob &, bool captureReadback = false);
	// The second fence stage consumes exact completed RGBA8 bytes and uploads an
	// interpreted display texture.
	std::optional<std::string>
	BuildSurfaceDisplayUploadJob(uint32_t width, uint32_t height, uint64_t bytes, SurfaceJob &);
} // namespace engine::render::hlsl
