// Renderer content and settings: meshes, textures, shaders, portal lens
// programs, post process shaders, and the lighting and surface settings.

#include "DisplayColour.hpp"
#include "RenderTypes.hpp"
#include "RendererState.hpp"
#include "SurfaceScale.hpp"
#include "capture/ViewRecording.hpp"
#include "diagnostics/VulkanTimestamps.hpp"
#include "portal/PortalRendererTerminalObserver.hpp"
#include "presentation/FrameBatch.hpp"
#include "resources/MaterialSamplerAdmission.hpp"
#include "resources/ResourcePreview.hpp"
#include "resources/ShaderBinary.hpp"
#include "resources/StartupCompilation.hpp"

#include <engine/assets/Builtin.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/graph/Cull.hpp>
#include <engine/graph/EntityFlow.hpp>
#include <engine/graph/ExecutionPlan.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/graph/Schedule.hpp>
#include <engine/graph/Shadow.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/capture/DataFactoryHookBind.hpp>
#include <engine/render/pipeline/GraphRunner.hpp>
#include <engine/render/portal/PortalExchange.hpp>
#include <engine/render/resources/MeshTable.hpp>
#include <engine/render/resources/MissingTexture.hpp>
#include <engine/render/resources/ShaderCompiler.hpp>
#include <engine/render/resources/ShaderLibrary.hpp>
#include <engine/render/resources/TextureTable.hpp>
#include <engine/resources/Shaders.hpp>
#include <engine/scene/cameras/ActiveCamera.hpp>
#include <engine/scene/environment/Sunlight.hpp>

// `scene::ReflectCamera` and `scene::SurfacePane`: where a mirror's camera goes
// for a viewer that is not the eye, which is what makes the levels below the
// first a recursion rather than a repeat.
#include <engine/scene/Tagging.hpp>
#include <engine/scene/cameras/SurfaceCameras.hpp>

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_video.h>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <spirv-tools/libspirv.hpp>
#include <spirv_cross.hpp>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace engine::render {
	core::Name Renderer::Impl::MeshContentOwner(core::Name name, core::Name owner) {
		if (!owner.IsValid()) return {};
		static const auto builtins = [] {
			std::array<core::Name, assets::BUILTIN_MESH_COUNT> names;
			for (uint8_t index = 0; index < names.size(); ++index) {
				names[index] = core::Name(assets::BuiltinName(static_cast<assets::BuiltinMesh>(index)));
			}
			return names;
		}();
		return std::find(builtins.begin(), builtins.end(), name) != builtins.end() ? core::Name{} : owner;
	}

	core::Name Renderer::Impl::TextureContentOwner(core::Name name, core::Name owner, core::Name world) {
		static const core::Name checker(assets::BuiltinName(assets::BuiltinTexture::Checker));
		// Generated images belong to their world; Studio keeps imported assets shared.
		const auto text = name.Text();
		if (world.IsValid() &&
			(text.starts_with("imagegraph-instance://") || text.starts_with("editable-image://")))
			return world;
		return name == checker ? core::Name{} : owner;
	}

	bool Renderer::MeshExtentOf(const core::Name &name, core::Vector3 &out, core::Name owner) const {
		if (State == nullptr || !State->Meshes.Has(name, Impl::MeshContentOwner(name, owner))) {
			return false;
		}
		out = State->Meshes.Resolve(name, Impl::MeshContentOwner(name, owner)).Extent;
		return true;
	}

	bool Renderer::AddMesh(const core::Name &name, const assets::MeshData &mesh, core::Name owner) {
		if (State == nullptr || State->Device == nullptr) {
			return false;
		}

		// **Accumulated here and uploaded by `Render`**, which is what makes a
		// burst of arrivals cost one transfer. It used to upload on the spot,
		// and because a copy pass cannot write part of a cycled buffer that
		// meant re-sending the whole table per mesh: admitting N meshes moved
		// O(N^2) bytes over the bus and froze the frame that a game finished
		// loading in. The entry is registered immediately either way, so
		// `MeshExtentOf` and the parts waiting to be sized by it are unaffected.
		//
		if (!State->Meshes.Add(name, mesh, owner)) return false;
		++State->ResourceEpoch;
		return true;
	}

	bool Renderer::PatchMesh(const core::Name &name, const MeshVertexPatch &patch, core::Name owner) {
		if (State == nullptr || !State->Meshes.Patch(name, patch, owner)) return false;
		++State->ResourceEpoch;
		return true;
	}

	uint64_t Renderer::MeshRevision(const core::Name &name, core::Name owner) const {
		return State == nullptr ? 0 : State->Meshes.RevisionOf(name, Impl::MeshContentOwner(name, owner));
	}

	bool Renderer::DropMesh(const core::Name &name, core::Name owner) {
		if (State == nullptr || !State->Meshes.Drop(name, owner)) return false;
		++State->ResourceEpoch;
		return true;
	}

	MeshCopyStatus Renderer::CopyMesh(
		const core::Name &name, assets::MeshData &out, size_t vertexLimit, size_t indexLimit, core::Name owner
	) const {
		return State != nullptr ? State->Meshes.Copy(name, out, vertexLimit, indexLimit, owner)
								: MeshCopyStatus::Missing;
	}

	TextureCopyStatus Renderer::CopyTexture(
		const core::Name &name, assets::TextureData &out, size_t byteLimit, core::Name owner
	) const {
		return State != nullptr ? State->Textures.Copy(name, out, byteLimit, owner)
								: TextureCopyStatus::Missing;
	}

	bool Renderer::AddPackedMesh(const core::Name &name, const PackedMeshData &mesh, core::Name owner) {
		if (State == nullptr || State->Device == nullptr) return false;
		if (!State->Meshes.AddPacked(name, mesh, owner)) return false;
		++State->ResourceEpoch;
		return true;
	}

	void Renderer::DropContentOwner(core::Name owner) {
		if (State == nullptr || !owner.IsValid()) return;
		for (auto graph = State->ImageGraphs.Documents.begin();
			 graph != State->ImageGraphs.Documents.end();) {
			if (graph->second.Owner != owner) {
				++graph;
				continue;
			}
			State->ImageGraphs.ReleaseDocument(State->Device, State->Textures, graph->second);
			graph = State->ImageGraphs.Documents.erase(graph);
			++State->ResourceEpoch;
		}
		for (auto &refusals : State->ShaderRefusals)
			std::erase_if(refusals, [&](const auto &entry) {
				return static_cast<uint32_t>(entry.first >> 32) == owner.Id();
			});
		bool shaders = false;
		for (auto entry = State->ShaderVariants.begin(); entry != State->ShaderVariants.end();) {
			const uint64_t key = (entry++)->first;
			if (static_cast<uint32_t>(key >> 32) != owner.Id()) continue;
			if (!shaders) (void)WaitForFrame();
			State->DropShaderVariant(core::Name::FromId(static_cast<uint32_t>(key)), owner);
			shaders = true;
		}
		for (auto entry = State->LensPipelines.begin(); entry != State->LensPipelines.end();) {
			if (static_cast<uint32_t>(entry->first >> 32) != owner.Id()) {
				++entry;
				continue;
			}
			if (!shaders) (void)WaitForFrame();
			SDL_ReleaseGPUGraphicsPipeline(State->Device, entry->second.Pipeline);
			entry = State->LensPipelines.erase(entry);
			shaders = true;
		}
		if (const auto found = State->PostProcessPipelines.find(owner.Id());
			found != State->PostProcessPipelines.end()) {
			if (!shaders) (void)WaitForFrame();
			SDL_ReleaseGPUGraphicsPipeline(State->Device, found->second.Pipeline);
			State->PostProcessPipelines.erase(found);
			shaders = true;
		}
		const size_t awaited = State->Textures.Awaited();
		const size_t textures = State->Textures.DropOwner(owner);
		const size_t meshes = State->Meshes.DropOwner(owner);
		if (shaders || textures != 0 || meshes != 0 || awaited != State->Textures.Awaited()) {
			++State->ResourceEpoch;
		}
	}

	bool Renderer::AddTexture(const core::Name &name, const assets::TextureData &image, core::Name owner) {
		if (State == nullptr || State->Device == nullptr) {
			return false;
		}
		if (!State->Textures.Add(name, image, owner)) return false;
		++State->ResourceEpoch;
		return true;
	}

	void Renderer::ExpectTexture(const core::Name &name, core::Name owner) {
		if (State != nullptr && name.IsValid() && !State->Textures.Expecting(name, owner)) {
			State->Textures.Expect(name, owner);
			++State->ResourceEpoch;
		}
	}

	void Renderer::StopExpectingTexture(const core::Name &name, core::Name owner) {
		if (State != nullptr && State->Textures.Expecting(name, owner)) {
			State->Textures.StopExpecting(name, owner);
			++State->ResourceEpoch;
		}
	}

	bool Renderer::ExpectingTexture(const core::Name &name, core::Name owner) const {
		return State != nullptr && State->Textures.Expecting(name, owner);
	}

	bool Renderer::AddTextureSequence(
		core::Name name, assets::TextureSequenceData sequence, core::Name owner
	) try {
		RequireOwningThread("AddTextureSequence");
		if (!State || !State->Device || !sequence.IsValid() || sequence.FrameDurations.size() <= 256)
			return false;
		const uint32_t side = sequence.FrameDurations.size() <= 1024 ? 32 : 64;
		const uint64_t width = uint64_t(sequence.Width) * side, height = uint64_t(sequence.Height) * side;
		const uint64_t bytes = width * height * 4;
		if (width > assets::Texture::MAXIMUM_DIMENSION || height > assets::Texture::MAXIMUM_DIMENSION ||
			bytes > assets::TextureSequence::MAXIMUM_PIXEL_BYTES)
			return false;
		ENGINE_PROFILE_CAT("sequence atlas upload", core::ProfileCategory::Assets);
		assets::TextureData image;
		image.Width = static_cast<uint32_t>(width);
		image.Height = static_cast<uint32_t>(height);
		image.Format = sequence.Format;
		image.FlipbookSide = static_cast<uint8_t>(side);
		image.FlipbookFrames = static_cast<uint16_t>(sequence.FrameDurations.size());
		image.FlipbookFrameDurations = std::move(sequence.FrameDurations);
		image.Pixels.resize(static_cast<size_t>(bytes));
		for (size_t frame = 0; frame < image.FlipbookFrames; ++frame) {
			const size_t rowBytes = size_t(sequence.Width) * 4;
			for (size_t row = 0; row < sequence.Height; ++row) {
				const size_t source = (frame * sequence.Height + row) * rowBytes;
				const size_t destination =
					((frame / side * sequence.Height + row) * image.Width + frame % side * sequence.Width) *
					4;
				std::copy_n(sequence.Pixels.begin() + source, rowBytes, image.Pixels.begin() + destination);
			}
		}
		if (!State->Textures.Add(name, image, owner)) return false;
		++State->ResourceEpoch;
		core::Metrics::Count("render.sequence.upload_bytes", bytes);
		return true;
	} catch (const std::bad_alloc &) {
		return false;
	}

	void Renderer::SetAnimationTime(double seconds) {
		if (State != nullptr) {
			State->AnimationSeconds = seconds;
		}
	}

	void Renderer::SetSurfaceBounces(uint32_t bounces) {
		if (State != nullptr) {
			// **Zero is kept rather than floored, and that is the change.** It
			// used to mean "no surface pass at all", which nobody wanted and
			// which has a clearer spelling; it is
			// `scene::AUTOMATIC_SURFACE_BOUNCES` now, and it is the default. A
			// stated number still floors at one for the original reason.
			//
			// **Capped**, which it did not need to be while this counted
			// iterations of one pass. It counts levels of a recursion - see
			// `MAX_SURFACE_DEPTH` for why that turns a large number from
			// wasteful into unfinishable.
			State->SurfaceBounces = bounces == 0 ? 0u : std::clamp<uint32_t>(bounces, 1u, MAX_SURFACE_DEPTH);
		}
	}

	uint32_t Renderer::SurfaceBounces() const {
		return State == nullptr ? 0u : State->SurfaceBounces;
	}

	void Renderer::SetSurfaceLimit(uint32_t panes) {
		if (State != nullptr) {
			// **Clamped here rather than at every use.** `scene::MAX_SURFACES`
			// is how far the per-viewport slot arrays reach, and a world may
			// state a larger number - `scene::SurfaceLimit` says in as many
			// words that the ceiling belongs to whoever draws. Zero is kept: a
			// world that wants its mirrors off has to be able to say so.
			State->SurfaceLimit = std::min<uint32_t>(panes, scene::MAX_SURFACES);
		}
	}

	uint32_t Renderer::SurfaceLimit() const {
		return State == nullptr ? static_cast<uint32_t>(scene::DEFAULT_SURFACE_LIMIT) : State->SurfaceLimit;
	}

	void Renderer::SetWireframe(bool enabled) {
		if (State != nullptr) {
			State->WireframeMode = enabled;
		}
	}

	bool Renderer::Wireframe() const {
		return State != nullptr && State->WireframeMode;
	}

	void Renderer::SetShaderOwnersFollowWorlds(bool follow) {
		if (State != nullptr) State->ShaderOwnersFollowWorlds = follow;
	}

	void Renderer::SetUntextured(bool enabled) {
		if (State != nullptr) {
			State->UntexturedMode = enabled;
		}
	}

	bool Renderer::Untextured() const {
		return State != nullptr && State->UntexturedMode;
	}

	void Renderer::SetPortalDepth(uint32_t depth) {
		if (State != nullptr) {
			// **Clamped rather than floored**, which is the opposite of the
			// bounce knob above and is right for the opposite reason: zero here
			// is a meaningful setting - every hole draws flat and nothing
			// recurses - while the ceiling exists because the pool is full-screen
			// targets per level per slot.
			State->PortalDepth = std::min(depth, MAX_PORTAL_DEPTH);
		}
	}

	void Renderer::SetSun(
		const core::Vector3 &direction, const core::Color3 &ambient, const core::Color3 &direct
	) {
		if (State == nullptr) {
			return;
		}

		// **Normalised by the caller and checked here.** `scene::SunOf` already
		// does it, and a direction of zero arriving from anywhere else would
		// shade every surface by `dot(n, 0)` - a world that goes flat grey with
		// nothing in the log to say why.
		const float length = direction.Magnitude();
		if (length > 0.0f) {
			State->Sun = glm::vec3{direction.X / length, direction.Y / length, direction.Z / length};
		}

		State->Ambient = glm::vec4{ambient.R, ambient.G, ambient.B, 1.0f};
		State->Direct = glm::vec4{direct.R, direct.G, direct.B, 1.0f};
	}

	void Renderer::SetLighting(const scene::WorldLighting &lighting) {
		if (State == nullptr) {
			return;
		}

		SetSun(lighting.Direction, lighting.Ambient, lighting.Direct);
		State->OutdoorAmbient = glm::vec4{
			lighting.OutdoorAmbient.R,
			lighting.OutdoorAmbient.G,
			lighting.OutdoorAmbient.B,
			1.0f,
		};
		State->FogColour = glm::vec4{lighting.FogColor.R, lighting.FogColor.G, lighting.FogColor.B, 1.0f};
		State->FogStart = std::max(lighting.FogStart, 0.0f);
		State->FogEnd = std::max(lighting.FogEnd, State->FogStart);
		State->BloomThreshold = std::max(lighting.BloomThreshold, 0.0f);
		State->BloomIntensity = std::max(lighting.BloomIntensity, 0.0f);
		State->BloomRadius = std::max(lighting.BloomRadius, 0.0f);
		State->DepthOfFieldIntensity = std::clamp(lighting.DepthOfFieldIntensity, 0.0f, 1.0f);
		State->DepthOfFieldFocusDistance = std::max(lighting.DepthOfFieldFocusDistance, 0.0f);
		State->DepthOfFieldFocusRange = std::max(lighting.DepthOfFieldFocusRange, 0.0f);
		State->DepthOfFieldRadius = std::max(lighting.DepthOfFieldRadius, 0.0f);
		State->GodRayIntensity = std::max(lighting.GodRayIntensity, 0.0f);
		State->GodRayThreshold = std::max(lighting.GodRayThreshold, 0.0f);
		State->GodRayRadius = std::max(lighting.GodRayRadius, 0.0f);
		State->Stars = lighting.Stars;
		State->EnvironmentState = lighting.EnvironmentState;
		State->Volumes = lighting.Volumes;
		State->VolumeCount = std::min(lighting.VolumeCount, State->Volumes.size());
		State->ShaderLenses = lighting.ShaderLenses;
		State->ShaderLensCount = std::min(lighting.ShaderLensCount, State->ShaderLenses.size());
		State->WorldRenderFeatures = lighting.RenderFeatures;
		State->HalfResolutionAmbientOcclusion = lighting.HalfResolutionAmbientOcclusion;
	}

	scene::WorldLighting Renderer::CurrentLighting() const {
		if (State == nullptr) {
			return {};
		}

		scene::WorldLighting lighting;
		lighting.Direction = core::Vector3{State->Sun.x, State->Sun.y, State->Sun.z};
		lighting.Ambient = core::Color3{State->Ambient.x, State->Ambient.y, State->Ambient.z};
		lighting.OutdoorAmbient = core::Color3{
			State->OutdoorAmbient.x,
			State->OutdoorAmbient.y,
			State->OutdoorAmbient.z,
		};
		lighting.Direct = core::Color3{State->Direct.x, State->Direct.y, State->Direct.z};
		lighting.FogColor = core::Color3{State->FogColour.x, State->FogColour.y, State->FogColour.z};
		lighting.FogStart = State->FogStart;
		lighting.FogEnd = State->FogEnd;
		lighting.BloomThreshold = State->BloomThreshold;
		lighting.BloomIntensity = State->BloomIntensity;
		lighting.BloomRadius = State->BloomRadius;
		lighting.DepthOfFieldIntensity = State->DepthOfFieldIntensity;
		lighting.DepthOfFieldFocusDistance = State->DepthOfFieldFocusDistance;
		lighting.DepthOfFieldFocusRange = State->DepthOfFieldFocusRange;
		lighting.DepthOfFieldRadius = State->DepthOfFieldRadius;
		lighting.GodRayIntensity = State->GodRayIntensity;
		lighting.GodRayThreshold = State->GodRayThreshold;
		lighting.GodRayRadius = State->GodRayRadius;
		lighting.Stars = State->Stars;
		lighting.EnvironmentState = State->EnvironmentState;
		lighting.Volumes = State->Volumes;
		lighting.VolumeCount = State->VolumeCount;
		lighting.ShaderLenses = State->ShaderLenses;
		lighting.ShaderLensCount = State->ShaderLensCount;
		lighting.RenderFeatures = State->WorldRenderFeatures;
		lighting.HalfResolutionAmbientOcclusion = State->HalfResolutionAmbientOcclusion;
		return lighting;
	}

	core::Vector3 Renderer::SunDirection() const {
		return core::Vector3{State->Sun.x, State->Sun.y, State->Sun.z};
	}

	core::Color3 Renderer::SunAmbient() const {
		return core::Color3{State->Ambient.x, State->Ambient.y, State->Ambient.z};
	}

	core::Color3 Renderer::SunColor() const {
		return core::Color3{State->Direct.x, State->Direct.y, State->Direct.z};
	}

	uint32_t Renderer::PortalDepth() const {
		return State == nullptr ? 0u : State->PortalDepth;
	}

	FlipbookCell Renderer::TextureCell(const core::Name &name, double seconds, core::Name owner) const {
		if (State == nullptr) {
			return {};
		}
		return State->Textures.CellOf(name, seconds, Impl::TextureContentOwner(name, owner));
	}

	uint64_t Renderer::TextureAnimationSignature(double seconds) const {
		return State == nullptr ? 0 : State->Textures.AnimationSignature(seconds);
	}

	uint64_t Renderer::ResourceRevision() const {
		return State == nullptr ? 0 : State->ResourceEpoch;
	}

	bool Renderer::HasPendingWeatherWork(uint64_t world) const {
		return State != nullptr && State->HasPendingWeatherWork(world);
	}

	void *Renderer::TextureHandle(const core::Name &name, core::Name owner) const {
		if (State == nullptr) {
			return nullptr;
		}
		return State->Textures.Find(name, Impl::TextureContentOwner(name, owner));
	}

	bool Renderer::TextureSamplesSRGB(const core::Name &name, core::Name owner) const {
		assets::TextureFormat format{};
		return State != nullptr &&
			   State->Textures.FormatOf(name, format, Impl::TextureContentOwner(name, owner)) &&
			   assets::IsSRGB(format);
	}

	bool
	Renderer::TextureSize(const core::Name &name, uint32_t &width, uint32_t &height, core::Name owner) const {
		if (State == nullptr) {
			return false;
		}
		return State->Textures.SizeOf(name, width, height, Impl::TextureContentOwner(name, owner));
	}

	bool Renderer::DropTexture(const core::Name &name, core::Name owner) {
		if (State == nullptr) {
			return false;
		}
		if (!State->Textures.Drop(name, owner)) return false;
		++State->ResourceEpoch;
		return true;
	}

	bool Renderer::AddShader(const core::Name &name, std::span<const uint32_t> spirv, core::Name owner) {
		if (State == nullptr || State->Device == nullptr || !name.IsValid() || spirv.empty()) {
			return false;
		}

		const auto hash = assets::Hasher::Of(std::as_bytes(spirv));
		const auto held = State->ShaderVariants.find(Impl::ShaderVariantKey(name, owner));
		if (held != State->ShaderVariants.end() && held->second.CodeHash == hash) return true;

		// **Built on the spot rather than at the next frame's barrier**, which
		// is `AddMesh`'s rule and its reason: a caller has already arranged for
		// this to be a moment it controls - a content pump or a
		// `ShaderLibrary::Refresh` - so deferring would add a second
		// synchronisation point for a caller that already had one.
		//
		// **A frame is waited for first, because replacing releases.** An author
		// editing a `ShaderScript` replaces a pipeline that the frame in flight
		// may still be drawing through, and releasing that is a use after free
		// inside the driver rather than an error here.
		if (State->ShaderVariants.contains(Impl::ShaderVariantKey(name, owner))) {
			(void)WaitForFrame();
		}
		if (!State->AddShaderVariant(name, spirv, owner)) return false;
		++State->ResourceEpoch;
		return true;
	}

	bool
	Renderer::AddMaterialShader(const core::Name &name, std::span<const uint32_t> spirv, core::Name owner) {
		if (const auto failure = AdmitMaterialSamplers(spirv)) {
			ENGINE_ERROR("material shader '{}': {}", name.Text(), *failure);
			return false;
		}
		return AddShader(name, spirv, owner);
	}

	bool Renderer::DropShader(const core::Name &name, core::Name owner) {
		if (State == nullptr || State->Device == nullptr) {
			return false;
		}
		if (!State->ShaderVariants.contains(Impl::ShaderVariantKey(name, owner))) {
			return false;
		}

		(void)WaitForFrame();
		State->DropShaderVariant(name, owner);
		++State->ResourceEpoch;
		return true;
	}

	bool Renderer::HasShader(const core::Name &name, core::Name owner) const {
		return State != nullptr && name.IsValid() &&
			   State->ShaderVariants.contains(Impl::ShaderVariantKey(name, owner));
	}

	namespace {
		std::atomic<uint64_t> NextPortalLensToken{1};

		uint64_t TakePortalLensToken() {
			auto token = NextPortalLensToken.load(std::memory_order_relaxed);
			while (token != 0) {
				if (NextPortalLensToken.compare_exchange_weak(token, token + 1, std::memory_order_relaxed))
					return token;
			}
			return 0;
		}

		bool ValidCapturedLensProgram(std::span<const uint32_t> words) {
			spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_0);
			if (!validator.Validate(words.data(), words.size())) return false;
			try {
				spirv_cross::Compiler module(words.data(), words.size());
				const auto entries = module.get_entry_points_and_stages();
				if (entries.size() != 1 || entries[0].execution_model != spv::ExecutionModelFragment ||
					entries[0].name != "main")
					return false;
				const auto resources = module.get_shader_resources();
				if (!resources.storage_buffers.empty() || !resources.storage_images.empty() ||
					!resources.subpass_inputs.empty() || !resources.atomic_counters.empty() ||
					!resources.acceleration_structures.empty() || !resources.gl_plain_uniforms.empty() ||
					!resources.tensors.empty() || !resources.push_constant_buffers.empty() ||
					!resources.shader_record_buffers.empty() || !resources.separate_images.empty() ||
					!resources.separate_samplers.empty() || !resources.builtin_outputs.empty() ||
					resources.sampled_images.size() > 2 || resources.uniform_buffers.size() > 1 ||
					resources.stage_inputs.size() > 1 || resources.stage_outputs.size() != 1)
					return false;
				const auto vector = [](const spirv_cross::SPIRType &type, uint32_t count) {
					return type.basetype == spirv_cross::SPIRType::Float && type.width == 32 &&
						   type.vecsize == count && type.columns == 1 && type.array.empty();
				};
				for (const auto &input : resources.stage_inputs)
					if (module.get_decoration(input.id, spv::DecorationLocation) != 0 ||
						!vector(module.get_type(input.type_id), 2))
						return false;
				const auto &output = resources.stage_outputs.front();
				if (module.get_decoration(output.id, spv::DecorationLocation) != 0 ||
					module.get_decoration(output.id, spv::DecorationIndex) != 0 ||
					!vector(module.get_type(output.type_id), 4))
					return false;
				uint32_t samplerBindings = 0;
				for (const auto &sampler : resources.sampled_images) {
					const auto binding = module.get_decoration(sampler.id, spv::DecorationBinding);
					const auto &type = module.get_type(sampler.type_id);
					if (module.get_decoration(sampler.id, spv::DecorationDescriptorSet) != 2 || binding > 1 ||
						(samplerBindings & (1u << binding)) != 0 || !type.array.empty() ||
						type.image.dim != spv::Dim2D || type.image.arrayed || type.image.ms ||
						type.image.depth ||
						module.get_type(type.image.type).basetype != spirv_cross::SPIRType::Float)
						return false;
					samplerBindings |= 1u << binding;
				}
				for (const auto &uniform : resources.uniform_buffers) {
					const auto &type = module.get_type(uniform.base_type_id);
					if (module.get_decoration(uniform.id, spv::DecorationDescriptorSet) != 3 ||
						module.get_decoration(uniform.id, spv::DecorationBinding) != 0 ||
						!module.get_type(uniform.type_id).array.empty() || type.member_types.size() > 7 ||
						module.get_declared_struct_size(type) > sizeof(LensPassUniforms))
						return false;
					constexpr std::array<uint32_t, 7> offsets{
						0,
						64,
						128,
						144,
						160,
						176,
						static_cast<uint32_t>(offsetof(LensPassUniforms, CameraDepth))
					};
					for (uint32_t member = 0; member < type.member_types.size(); ++member) {
						const auto &field = module.get_type(type.member_types[member]);
						if (module.type_struct_member_offset(type, member) != offsets[member]) return false;
						if (member < 2) {
							if (field.basetype != spirv_cross::SPIRType::Float || field.width != 32 ||
								field.vecsize != 4 || field.columns != 4 || !field.array.empty() ||
								module.type_struct_member_matrix_stride(type, member) != 16 ||
								module.has_member_decoration(type.self, member, spv::DecorationRowMajor))
								return false;
						} else if (member < 5) {
							if (!vector(field, 4)) return false;
						} else if (member == 5) {
							if (field.basetype != spirv_cross::SPIRType::Struct || field.array.size() != 1 ||
								!field.array_size_literal[0] ||
								field.array[0] != scene::MAX_SCENE_SHADER_LENSES ||
								field.member_types.size() != 5 ||
								module.type_struct_member_array_stride(type, member) !=
									sizeof(LensPassUniforms::LensUniform))
								return false;
							for (uint32_t component = 0; component < 5; ++component)
								if (!vector(module.get_type(field.member_types[component]), 4) ||
									module.type_struct_member_offset(field, component) != component * 16)
									return false;
						} else if (!vector(field, 4)) {
							return false;
						}
					}
				}
				return true;
			} catch (const spirv_cross::CompilerError &) {
				return false;
			}
		}
	}

	uint64_t Renderer::RetainPortalLensPrograms(const PortalCaptureLenses &lenses) {
		ENGINE_PROFILE_CAT("captured lens programs retain", core::ProfileCategory::Render);
		RequireOwningThread("RetainPortalLensPrograms");
		if (!State || !State->Device || lenses.Programs.empty() || !ValidPortalCaptureLenses(lenses))
			return 0;
		std::vector<Impl::PortalLensProgramBinding> bindings;
		for (const auto &lens : lenses.Entries) {
			const core::Name name(lens.Shader);
			const auto held = std::find_if(bindings.begin(), bindings.end(), [&](const auto &binding) {
				return binding.Shader == name;
			});
			if (held != bindings.end()) {
				if (held->Hash != lens.ProgramHash) return 0;
				continue;
			}
			bindings.push_back({name, lens.ProgramHash});
		}
		std::sort(bindings.begin(), bindings.end(), [](const auto &left, const auto &right) {
			return left.Shader.Id() < right.Shader.Id();
		});
		for (auto &group : State->PortalLensPrograms) {
			if (group.Token != 0 && group.Bindings == bindings) {
				if (group.References == std::numeric_limits<size_t>::max()) return 0;
				++group.References;
				return group.Token;
			}
		}
		const auto free = std::find_if(
			State->PortalLensPrograms.begin(), State->PortalLensPrograms.end(), [](const auto &group) {
				return group.Token == 0;
			}
		);
		if (free == State->PortalLensPrograms.end()) return 0;
		for (const auto &binding : bindings) {
			const auto program =
				std::find_if(lenses.Programs.begin(), lenses.Programs.end(), [&](const auto &candidate) {
					return candidate.Hash == binding.Hash;
				});
			if (program == lenses.Programs.end() ||
				assets::Hasher::Of(std::as_bytes(std::span(program->SpirV))) != binding.Hash ||
				!ValidCapturedLensProgram(program->SpirV))
				return 0;
		}
		const auto token = TakePortalLensToken();
		if (token == 0) return 0;
		for (auto &binding : bindings) {
			const auto program =
				std::find_if(lenses.Programs.begin(), lenses.Programs.end(), [&](const auto &candidate) {
					return candidate.Hash == binding.Hash;
				});
			binding.Pipeline = State->CreateLensPipeline(binding.Shader, program->SpirV);
			if (!binding.Pipeline) {
				for (const auto &installed : bindings)
					if (installed.Pipeline) SDL_ReleaseGPUGraphicsPipeline(State->Device, installed.Pipeline);
				return 0;
			}
		}
		*free = {.Token = token, .References = 1, .Bindings = std::move(bindings)};
		++State->ResourceEpoch;
		return free->Token;
	}

	void Renderer::ReleasePortalLensPrograms(uint64_t token) {
		RequireOwningThread("ReleasePortalLensPrograms");
		if (!State || token == 0) return;
		for (auto &group : State->PortalLensPrograms) {
			if (group.Token != token) continue;
			if (--group.References != 0) return;
			(void)WaitForFrame();
			for (const auto &binding : group.Bindings)
				SDL_ReleaseGPUGraphicsPipeline(State->Device, binding.Pipeline);
			group = {};
			++State->ResourceEpoch;
			return;
		}
	}

	SDL_GPUGraphicsPipeline *
	Renderer::Impl::CreateLensPipeline(core::Name name, std::span<const uint32_t> spirv) {
		ENGINE_PROFILE_CAT("lens pipeline create", core::ProfileCategory::Render);
		const bool toMsl = Binary.Form == resources::ShaderForm::Msl;
		std::string translated;
		if (toMsl) {
			msl::Translation result = msl::Translate(spirv);
			if (result.Failed) {
				ENGINE_ERROR("lens shader '{}' cannot be translated to MSL: {}", name.Text(), result.Error);
				return nullptr;
			}
			translated = std::move(result.Source);
		}

		SDL_GPUShaderCreateInfo fragmentInfo{};
		fragmentInfo.code = toMsl ? reinterpret_cast<const Uint8 *>(translated.data())
								  : reinterpret_cast<const Uint8 *>(spirv.data());
		fragmentInfo.code_size = toMsl ? translated.size() : spirv.size() * sizeof(uint32_t);
		fragmentInfo.entrypoint = Binary.EntryPoint;
		fragmentInfo.format = Binary.Format;
		fragmentInfo.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
		fragmentInfo.num_samplers = 2;
		fragmentInfo.num_uniform_buffers = 1;

		SDL_GPUShader *fragment = SDL_CreateGPUShader(Device, &fragmentInfo);
		if (fragment == nullptr) {
			ENGINE_ERROR("lens shader '{}': {}", name.Text(), SDL_GetError());
			return nullptr;
		}
		SDL_GPUShader *vertex = LoadShader("overlay.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
		if (vertex == nullptr) {
			SDL_ReleaseGPUShader(Device, fragment);
			return nullptr;
		}

		SDL_GPUColorTargetDescription target{};
		target.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
		SDL_GPUGraphicsPipelineCreateInfo info{};
		info.vertex_shader = vertex;
		info.fragment_shader = fragment;
		info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
		info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
		info.target_info.color_target_descriptions = &target;
		info.target_info.num_color_targets = 1;
		SDL_GPUGraphicsPipeline *pipeline = SDL_CreateGPUGraphicsPipeline(Device, &info);
		SDL_ReleaseGPUShader(Device, vertex);
		SDL_ReleaseGPUShader(Device, fragment);
		if (pipeline == nullptr) {
			ENGINE_ERROR("lens shader '{}' pipeline: {}", name.Text(), SDL_GetError());
			return nullptr;
		}

		return pipeline;
	}

	bool Renderer::AddLensShader(const core::Name &name, std::span<const uint32_t> spirv, core::Name owner) {
		if (State == nullptr || State->Device == nullptr || !name.IsValid() || spirv.empty()) {
			return false;
		}

		const auto hash = assets::Hasher::Of(std::as_bytes(spirv));
		const auto held = State->LensPipelines.find(Impl::ShaderVariantKey(name, owner));
		if (held != State->LensPipelines.end() && held->second.CodeHash == hash) return true;

		SDL_GPUGraphicsPipeline *pipeline = State->CreateLensPipeline(name, spirv);
		if (!pipeline) return false;

		const auto previous = State->LensPipelines.find(Impl::ShaderVariantKey(name, owner));
		if (previous != State->LensPipelines.end()) {
			(void)WaitForFrame();
			SDL_ReleaseGPUGraphicsPipeline(State->Device, previous->second.Pipeline);
			previous->second = {hash, pipeline};
		} else {
			State->LensPipelines.emplace(
				Impl::ShaderVariantKey(name, owner), Impl::LensVariant{hash, pipeline}
			);
		}
		++State->ResourceEpoch;
		return true;
	}

	bool Renderer::DropLensShader(const core::Name &name, core::Name owner) {
		if (State == nullptr || State->Device == nullptr) {
			return false;
		}
		const auto found = State->LensPipelines.find(Impl::ShaderVariantKey(name, owner));
		if (found == State->LensPipelines.end()) {
			return false;
		}
		(void)WaitForFrame();
		SDL_ReleaseGPUGraphicsPipeline(State->Device, found->second.Pipeline);
		State->LensPipelines.erase(found);
		++State->ResourceEpoch;
		return true;
	}

	bool Renderer::HasLensShader(const core::Name &name, core::Name owner) const {
		return State != nullptr && name.IsValid() &&
			   State->LensPipelines.contains(Impl::ShaderVariantKey(name, owner));
	}

	assets::ContentHash Renderer::LensShaderHash(const core::Name &name, core::Name owner) const {
		if (!State || !name.IsValid()) return {};
		const auto found = State->LensPipelines.find(Impl::ShaderVariantKey(name, owner));
		return found == State->LensPipelines.end() ? assets::ContentHash{} : found->second.CodeHash;
	}

	bool Renderer::PrepareShaders(
		const ShaderLibrary &library,
		std::span<const core::Name> materials,
		std::span<const core::Name> lenses,
		core::Name postProcess,
		core::Name owner
	) {
		if (State == nullptr || State->Device == nullptr) return false;
		const auto before = State->ResourceEpoch;
		const auto wanted = [](std::span<const core::Name> names, core::Name name) {
			return std::find(names.begin(), names.end(), name) != names.end();
		};
		const auto accepted = [](const ShaderModule *module) {
			return module != nullptr && !module->CodeHash.IsZero();
		};
		for (auto entry = State->ShaderVariants.begin(); entry != State->ShaderVariants.end();) {
			const auto key = (entry++)->first;
			if (static_cast<uint32_t>(key >> 32) != owner.Id()) continue;
			const auto name = core::Name::FromId(static_cast<uint32_t>(key));
			if (!wanted(materials, name) || !accepted(library.Find(name, owner))) DropShader(name, owner);
		}
		for (auto entry = State->LensPipelines.begin(); entry != State->LensPipelines.end();) {
			const auto key = (entry++)->first;
			if (static_cast<uint32_t>(key >> 32) != owner.Id()) continue;
			const auto name = core::Name::FromId(static_cast<uint32_t>(key));
			if (!wanted(lenses, name) || !accepted(library.FindLens(name, owner)))
				DropLensShader(name, owner);
		}
		const std::array gradeDemand{postProcess};
		const std::array<std::span<const core::Name>, 3> demands{materials, lenses, gradeDemand};
		for (size_t family = 0; family < demands.size(); ++family) {
			auto &refusals = State->ShaderRefusals[family];
			std::erase_if(refusals, [&](const auto &entry) {
				return static_cast<uint32_t>(entry.first >> 32) == owner.Id() &&
					   !wanted(demands[family], core::Name::FromId(static_cast<uint32_t>(entry.first)));
			});
			for (const auto name : demands[family]) {
				const auto key = Impl::ShaderVariantKey(name, owner);
				const auto *module = family == 1 ? library.FindLens(name, owner) : library.Find(name, owner);
				if (!accepted(module)) {
					refusals.erase(key);
					if (family == 2) ClearPostProcessShader(owner);
					continue;
				}
				assets::ContentHash held;
				if (family == 0) {
					const auto found = State->ShaderVariants.find(key);
					if (found != State->ShaderVariants.end()) held = found->second.CodeHash;
				} else if (family == 1) {
					const auto found = State->LensPipelines.find(key);
					if (found != State->LensPipelines.end()) held = found->second.CodeHash;
				} else {
					const auto found = State->PostProcessPipelines.find(owner.Id());
					if (found != State->PostProcessPipelines.end() && found->second.Shader == name)
						held = found->second.CodeHash;
				}
				if (held == module->CodeHash) {
					refusals.erase(key);
					continue;
				}
				const auto refused = refusals.find(key);
				if (refused != refusals.end() && refused->second == module->CodeHash) continue;
				const bool installed = family == 0	 ? AddMaterialShader(name, module->SpirV, owner)
									   : family == 1 ? AddLensShader(name, module->SpirV, owner)
													 : SetPostProcessShader(name, module->SpirV, owner);
				if (installed)
					refusals.erase(key);
				else
					refusals[key] = module->CodeHash;
			}
		}
		return State->ResourceEpoch != before;
	}

	bool Renderer::SetPostProcessShader(
		const core::Name &name, std::span<const uint32_t> spirv, core::Name owner
	) {
		if (State == nullptr || State->Device == nullptr || spirv.empty()) {
			return false;
		}

		const auto hash = assets::Hasher::Of(std::as_bytes(spirv));
		const auto held = State->PostProcessPipelines.find(owner.Id());
		if (held != State->PostProcessPipelines.end() && held->second.Shader == name &&
			held->second.CodeHash == hash)
			return true;

		const bool toMsl = State->Binary.Form == resources::ShaderForm::Msl;
		std::string translated;
		if (toMsl) {
			msl::Translation result = msl::Translate(spirv);
			if (result.Failed) {
				ENGINE_ERROR(
					"postprocess shader '{}' cannot be translated to MSL: {}", name.Text(), result.Error
				);
				return false;
			}
			translated = std::move(result.Source);
		}

		SDL_GPUShaderCreateInfo fragmentInfo{};
		fragmentInfo.code = toMsl ? reinterpret_cast<const Uint8 *>(translated.data())
								  : reinterpret_cast<const Uint8 *>(spirv.data());
		fragmentInfo.code_size = toMsl ? translated.size() : spirv.size() * sizeof(uint32_t);
		fragmentInfo.entrypoint = State->Binary.EntryPoint;
		fragmentInfo.format = State->Binary.Format;
		fragmentInfo.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;

		// **One sampler and no bound uniform buffer - `tonemap.frag`'s own
		// shape**, matching what `CreatePipelines` declares for it. See
		// `SetPostProcessShader`'s own header for the contract a
		// `ShaderScript` used here is written against.
		fragmentInfo.num_samplers = 1;
		fragmentInfo.num_uniform_buffers = 0;

		SDL_GPUShader *fragment = SDL_CreateGPUShader(State->Device, &fragmentInfo);
		if (fragment == nullptr) {
			ENGINE_ERROR("postprocess shader '{}': {}", name.Text(), SDL_GetError());
			return false;
		}

		// Reloaded rather than kept - SDL_GPU pipelines own what they need
		// from the shader objects that built them, `InterfacePass::
		// AddShaderVariant`'s own reason.
		SDL_GPUShader *vertex = State->LoadShader("overlay.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
		if (vertex == nullptr) {
			SDL_ReleaseGPUShader(State->Device, fragment);
			return false;
		}

		SDL_GPUColorTargetDescription target{};
		target.format = State->ColourFormat();

		SDL_GPUGraphicsPipelineCreateInfo info{};
		info.vertex_shader = vertex;
		info.fragment_shader = fragment;
		info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
		info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
		info.target_info.color_target_descriptions = &target;
		info.target_info.num_color_targets = 1;

		SDL_GPUGraphicsPipeline *pipeline = SDL_CreateGPUGraphicsPipeline(State->Device, &info);

		SDL_ReleaseGPUShader(State->Device, vertex);
		SDL_ReleaseGPUShader(State->Device, fragment);

		if (pipeline == nullptr) {
			ENGINE_ERROR("postprocess shader '{}' pipeline: {}", name.Text(), SDL_GetError());
			return false;
		}

		// **Waited for before replacing**, `AddShader`'s own reason: the
		// frame in flight may still be reading through whatever pipeline
		// this is about to release.
		const auto previous = State->PostProcessPipelines.find(owner.Id());
		if (previous != State->PostProcessPipelines.end()) {
			(void)WaitForFrame();
			SDL_ReleaseGPUGraphicsPipeline(State->Device, previous->second.Pipeline);
			previous->second = {name, hash, pipeline};
		} else {
			State->PostProcessPipelines.emplace(owner.Id(), Impl::PostProcessVariant{name, hash, pipeline});
		}
		++State->ResourceEpoch;
		return true;
	}

	void Renderer::ClearPostProcessShader(core::Name owner) {
		if (State == nullptr) return;
		const auto found = State->PostProcessPipelines.find(owner.Id());
		if (found == State->PostProcessPipelines.end()) return;
		if (State->Device != nullptr) {
			(void)WaitForFrame();
			SDL_ReleaseGPUGraphicsPipeline(State->Device, found->second.Pipeline);
		}
		State->PostProcessPipelines.erase(found);
		++State->ResourceEpoch;
	}

	core::Name Renderer::PostProcessShaderName(core::Name owner) const {
		if (State == nullptr) return {};
		const auto found = State->PostProcessPipelines.find(owner.Id());
		return found == State->PostProcessPipelines.end() ? core::Name{} : found->second.Shader;
	}
}
