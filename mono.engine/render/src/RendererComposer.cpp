#include "ComposerCaptureIdentity.hpp"
#include "ImageGraphGpuHelpers.hpp"
#include "RendererState.hpp"
#include "nodes/ComposerNodes.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/SourceTransformImage3D.hpp>

#include <algorithm>
#include <new>
#include <type_traits>

namespace engine::render {
	std::optional<std::string>
	Renderer::InstallComposerShader(core::Name owner, core::Name name, const assets::ShaderData &shader) {
		RequireOwningThread("InstallComposerShader");
		if (!State) return "Composer renderer state is unavailable";
		uint64_t before = 0, after = 0;
		(void)State->ComposerShaders.Find(owner, name, before);
		if (const auto failure = State->ComposerShaders.Install(owner, name, shader)) return failure;
		(void)State->ComposerShaders.Find(owner, name, after);
		if (before == after) return {};
		std::erase_if(State->ComposerCaptures, [&](const auto &entry) {
			return entry.Owner == owner && entry.Shader == name;
		});
		for (auto &slot : State->GraphResources.Transform3D)
			if (slot.Owner == owner && slot.ComposerRequest && slot.ComposerRequest->Shader == name) {
				if (slot.Phase == Impl::GraphResourceCache::Transform3DPhase::Queued)
					State->ReleaseTransform3D(slot);
				else
					slot.Cancelled = true;
			}
		return {};
	}
	bool Renderer::RemoveComposerShader(core::Name owner, core::Name name, uint64_t installedRevision) {
		RequireOwningThread("RemoveComposerShader");
		if (!State || !State->ComposerShaders.Remove(owner, name, installedRevision)) return false;
		std::erase_if(State->ComposerCaptures, [&](const auto &entry) {
			return entry.Owner == owner && entry.Shader == name;
		});
		for (auto &slot : State->GraphResources.Transform3D)
			if (slot.Owner == owner && slot.ComposerRequest && slot.ComposerRequest->Shader == name &&
				slot.ComposerRequest->ShaderRevision == installedRevision) {
				if (slot.Phase == Impl::GraphResourceCache::Transform3DPhase::Queued)
					State->ReleaseTransform3D(slot);
				else
					slot.Cancelled = true;
			}
		return true;
	}
	uint64_t Renderer::ComposerShaderRevision(core::Name owner, core::Name name) const {
		RequireOwningThread("ComposerShaderRevision");
		uint64_t revision = 0;
		if (State) (void)State->ComposerShaders.Find(owner, name, revision);
		return revision;
	}
	std::optional<std::string> Renderer::BuildComposerSurface(
		const engine::imagegraph::Node &node,
		const engine::imagegraph::EvaluationSnapshot &snapshot,
		core::Name owner,
		bool displayColorSpace,
		hlsl::SurfaceRequest &output
	) const {
		RequireOwningThread("BuildComposerSurface");
		if (!State) return "Composer renderer state is unavailable";
		return hlsl::BuildSurfaceRequest(
			node, snapshot, State->ComposerShaders, owner, displayColorSpace, output
		);
	}
	std::optional<std::string> Renderer::BuildComposerSurface(
		const engine::imagegraph::HostNodeInvocation &invocation,
		core::Name owner,
		bool displayColorSpace,
		hlsl::SurfaceRequest &output
	) const {
		RequireOwningThread("BuildComposerSurface");
		if (!State) return "Composer renderer state is unavailable";
		return hlsl::BuildSurfaceRequest(
			invocation, State->ComposerShaders, owner, displayColorSpace, output
		);
	}
	imagegraph::TransformImage3DQueueResult
	Renderer::QueueComposerSurface(hlsl::SurfaceLiveRequest request) try {
		RequireOwningThread("QueueComposerSurface");
		ENGINE_PROFILE("composer surface admission");
		using Result = imagegraph::TransformImage3DQueueResult;
		using Phase = Impl::GraphResourceCache::Transform3DPhase;
		if (!State || !request.Owner.IsValid() || !request.Name.IsValid() || request.Generation == 0 ||
			hlsl::ValidateSurfaceRequest(request.Request))
			return Result::Invalid;
		uint64_t revision = 0;
		const auto *installed = State->ComposerShaders.Find(request.Owner, request.Request.Shader, revision);
		if (!installed || revision != request.Request.ShaderRevision || *installed != request.Request.Pair)
			return Result::Invalid;
		const uint64_t bytes = hlsl::SurfaceSourceBytes(request.Request);
		if (bytes > hlsl::MAXIMUM_SURFACE_JOB_BYTES) return Result::Invalid;
		Impl::GraphResourceCache::Transform3DSlot *free = nullptr, *replacement = nullptr;
		for (auto &slot : State->GraphResources.Transform3D) {
			if (slot.Phase == Phase::Free) {
				free = &slot;
				continue;
			}
			if (slot.Owner != request.Owner) continue;
			if (slot.CameraRequest) {
				for (const auto &binding : slot.CameraBindings)
					if (!binding.Cancelled && binding.Name == request.Name &&
						binding.Generation >= request.Generation)
						return Result::Duplicate;
			} else if (slot.Name == request.Name) {
				if (slot.Generation >= request.Generation) return Result::Duplicate;
				if (slot.Phase == Phase::Queued) replacement = &slot;
			}
		}
		for (const auto &published : State->GraphResources.PublishedTransform3DOutputs)
			if (published.Owner == request.Owner && published.Name == request.Name &&
				published.Generation >= request.Generation)
				return Result::Duplicate;
		auto *destination = replacement ? replacement : free;
		if (!destination) return Result::Full;
		const uint64_t held =
			State->GraphResources.Transform3DSourceBytes - (replacement ? replacement->SourceBytes : 0);
		if (held > hlsl::MAXIMUM_SURFACE_JOB_BYTES - bytes) return Result::Full;
		// The admitted request is already owned; publication cannot allocate after
		// retirement.
		static_assert(std::is_nothrow_move_assignable_v<decltype(destination->ComposerRequest)>);
		static_assert(std::is_nothrow_move_constructible_v<hlsl::SurfaceRequest>);
		if (replacement) State->ReleaseTransform3D(*replacement);
		destination->Phase = Phase::Queued;
		destination->Owner = request.Owner;
		destination->Name = request.Name;
		destination->Generation = request.Generation;
		destination->Width = request.Request.Textures[0].Width;
		destination->Height = request.Request.Textures[0].Height;
		destination->SourceBytes = bytes;
		destination->ComposerRequest = std::move(request.Request);
		State->GraphResources.Transform3DSourceBytes += bytes;
		for (auto &slot : State->GraphResources.Transform3D) {
			if (&slot == destination || slot.Phase == Phase::Free || slot.Owner != request.Owner) continue;
			if (slot.CameraRequest) {
				for (auto &binding : slot.CameraBindings)
					if (binding.Name == request.Name) binding.Cancelled = true;
			} else if (slot.Name == request.Name)
				slot.Cancelled = true;
		}
		return replacement ? Result::Replaced : Result::Queued;
	} catch (const std::bad_alloc &) {
		return imagegraph::TransformImage3DQueueResult::Full;
	}
	void Renderer::CancelComposerCapture(core::Name owner, core::Name name) {
		RequireOwningThread("CancelComposerCapture");
		if (!State) return;
		std::erase_if(State->ComposerCaptures, [&](const auto &entry) {
			return entry.Owner == owner && entry.Name == name;
		});
		for (auto &slot : State->GraphResources.Transform3D)
			if (slot.Owner == owner && slot.Name == name &&
				(slot.ComposerCaptureReadback || slot.TransformCaptureReadback ||
				 slot.CameraCaptureReadback)) {
				if (slot.Phase == Impl::GraphResourceCache::Transform3DPhase::Queued)
					State->ReleaseTransform3D(slot);
				else
					slot.Cancelled = true;
			}
	}
	bool Renderer::CaptureComposerSurfaceAsync(
		const engine::imagegraph::HostNodeInvocation &invocation,
		core::Name owner,
		core::Name name,
		engine::imagegraph::HostNodeCapture &output,
		std::string &failure,
		bool *pending
	) {
		return CaptureResolvedSurfaceAsync(invocation, owner, name, output, failure, pending);
	}
	bool Renderer::CaptureSourceCamera3DAsync(
		const engine::imagegraph::HostNodeInvocation &invocation,
		core::Name owner,
		core::Name name,
		engine::imagegraph::HostNodeCapture &output,
		std::string &failure,
		bool *pending
	) {
		if (pending) *pending = false;
		if (invocation.Authored.Type != "pc.3_d_camera" && invocation.Authored.Type != "pc.3_d_camera_set") {
			failure = "camera host requires an explicit source camera node";
			return false;
		}
		return CaptureResolvedSurfaceAsync(invocation, owner, name, output, failure, pending);
	}
	bool Renderer::CaptureTransformImage3DAsync(
		const engine::imagegraph::HostNodeInvocation &invocation,
		core::Name owner,
		core::Name name,
		engine::imagegraph::HostNodeCapture &output,
		std::string &failure,
		bool *pending
	) {
		if (pending) *pending = false;
		if (invocation.Authored.Type != "pc.3_d_transform_image" &&
			invocation.Authored.Type != "image.transform_3d") {
			failure = "Source Transform host requires pc.3_d_transform_image";
			return false;
		}
		return CaptureResolvedSurfaceAsync(invocation, owner, name, output, failure, pending);
	}
	bool Renderer::CaptureResolvedSurfaceAsync(
		const engine::imagegraph::HostNodeInvocation &invocation,
		core::Name owner,
		core::Name name,
		engine::imagegraph::HostNodeCapture &output,
		std::string &failure,
		bool *pending
	) try {
		RequireOwningThread("CaptureComposerSurfaceAsync");
		ENGINE_PROFILE("composer asynchronous host capture");
		if (pending) *pending = false;
		if (!State || !State->Device || !owner.IsValid() || !name.IsValid()) {
			failure = "Composer preview capture needs an explicit owner, capture name "
					  "and renderer device";
			return false;
		}
		using namespace engine::imagegraph;
		const bool transform = invocation.Authored.Type == "pc.3_d_transform_image" ||
							   invocation.Authored.Type == "image.transform_3d";
		const bool camera =
			invocation.Authored.Type == "pc.3_d_camera" || invocation.Authored.Type == "pc.3_d_camera_set";
		const uint64_t maximum = std::min(invocation.MaximumOperationBytes, hlsl::MAXIMUM_SURFACE_JOB_BYTES);
		const auto previousBytes = HostCaptureRetainedPayloadBytes(output);
		uint64_t held = sizeof(State->ComposerCaptures) +
						State->ComposerCaptures.capacity() * sizeof(Impl::ComposerCaptureEntry);
		for (const auto &entry : State->ComposerCaptures) {
			const auto bytes = HostCaptureRetainedPayloadBytes(entry.Receipt);
			if (!bytes || std::max(*bytes, entry.MaximumBytes) > hlsl::MAXIMUM_SURFACE_JOB_BYTES - held) {
				failure = "Composer retained preview cache exceeds budget";
				return false;
			}
			held += std::max(*bytes, entry.MaximumBytes);
		}
		if (!previousBytes || *previousBytes > maximum || held > maximum - *previousBytes) {
			failure = "Composer previous output and preview cache exceed operation budget";
			return false;
		}
		HostNodeCapture receipt;
		Diagnostic diagnostic;
		uint64_t receiptBytes = 0;
		if (PrepareResolvedHostCapture(
				invocation, maximum - *previousBytes - held, receipt, receiptBytes, diagnostic
			) != Status::Ok) {
			failure = diagnostic.Message;
			return false;
		}
		auto found = std::find_if(
			State->ComposerCaptures.begin(), State->ComposerCaptures.end(), [&](const auto &entry) {
				return entry.Owner == owner && entry.Name == name;
			}
		);
		if (found != State->ComposerCaptures.end() &&
			detail::SameComposerCaptureInputs(
				found->Receipt, receipt, found->Interpolation, invocation.Interpolation
			) &&
			(!found->Shader.IsValid() ||
			 ComposerShaderRevision(owner, found->Shader) == found->ShaderRevision)) {
			if (found->Complete) {
				const auto bytes = HostCaptureRetainedPayloadBytes(found->Receipt);
				if (!bytes || *bytes > maximum - *previousBytes - held - receiptBytes) {
					failure = "Composer completed preview copy exceeds operation budget";
					return false;
				}
				HostNodeCapture candidate = found->Receipt;
				output = std::move(candidate);
				failure.clear();
				return true;
			}
			const bool queued = std::any_of(
				State->GraphResources.Transform3D.begin(),
				State->GraphResources.Transform3D.end(),
				[&](const auto &slot) {
					return slot.Owner == owner && slot.Name == name && slot.Generation == found->Generation &&
						   (slot.ComposerCaptureReadback || slot.TransformCaptureReadback ||
							slot.CameraCaptureReadback) &&
						   !slot.Cancelled;
				}
			);
			if (queued) {
				if (pending) *pending = true;
				failure = "Composer preview capture is pending";
			} else
				failure = "Composer preview capture failed before publication";
			return false;
		}
		const uint64_t generation = State->NextComposerCaptureGeneration;
		if (generation == 0 ||
			(found == State->ComposerCaptures.end() && State->ComposerCaptures.size() >= 16)) {
			failure = "Composer preview capture count or generation exhausted";
			return false;
		}
		HostNodeInvocation bounded = invocation;
		if (receiptBytes > maximum - *previousBytes - held) {
			failure = "Composer pending receipt exceeds operation budget";
			return false;
		}
		bounded.MaximumOperationBytes = maximum - *previousBytes - held - receiptBytes;
		hlsl::SurfaceRequest request;
		imagegraph::TransformImage3DRequest transformRequest;
		imagegraph::SourceCamera3DRequest cameraRequest;
		MeshValue3D mesh;
		if (camera) {
			if (!imagegraph::BuildSourceCamera3DRequest(bounded, "rendered", false, cameraRequest, failure))
				return false;
		} else if (transform) {
			if (!imagegraph::BuildSourceTransformImage3DRequest(bounded, transformRequest, mesh, failure))
				return false;
		} else if (const auto error = BuildComposerSurface(bounded, owner, false, request)) {
			failure = *error;
			return false;
		}
		const uint64_t pixels = camera ? uint64_t(cameraRequest.Width) * cameraRequest.Height
								: transform
									? uint64_t(transformRequest.Front.Width) * transformRequest.Front.Height
									: uint64_t(request.Textures[0].Width) * request.Textures[0].Height;
		const uint64_t completionBytes =
			camera ? pixels * 7 * assets::BytesPerPixel(cameraRequest.Format) +
						 7 * sizeof(HostCapturedImage) + 256
			: transform ? pixels * ((transformRequest.SourcePlane
										 ? 4
										 : assets::BytesPerPixel(transformRequest.Front.Format)) +
									4) +
							  2 * sizeof(HostCapturedImage) + 32
						: pixels * 4 + sizeof(HostCapturedImage) + 16;
		if (transform) {
			receipt.Outputs.push_back({"mesh", std::move(mesh)});
			const auto bytes = HostCaptureRetainedPayloadBytes(receipt);
			if (!bytes || *bytes > maximum - *previousBytes - held) {
				failure = "Transform mesh receipt exceeds operation budget";
				return false;
			}
			receiptBytes = *bytes;
		}
		const uint64_t cameraBytes =
			camera ? imagegraph::SourceCamera3DRetainedBytes(cameraRequest).value_or(UINT64_MAX) : 0;
		const uint64_t ownedRequest = camera	  ? cameraBytes
									  : transform ? sizeof(transformRequest) +
														transformRequest.Front.Pixels.capacity() +
														transformRequest.Back.Pixels.capacity()
												  : hlsl::SurfaceSourceBytes(request);
		if (ownedRequest > maximum - *previousBytes - held - receiptBytes ||
			completionBytes > maximum - *previousBytes - held - receiptBytes - ownedRequest) {
			failure = "Source preview request, receipt and output reservation exceed operation budget";
			return false;
		}
		if (completionBytes > hlsl::MAXIMUM_SURFACE_JOB_BYTES - held ||
			receiptBytes > hlsl::MAXIMUM_SURFACE_JOB_BYTES - held - completionBytes) {
			failure = "Composer preview output reservation exceeds cache budget";
			return false;
		}
		const auto shader = (camera || transform) ? core::Name{} : request.Shader;
		const auto revision = (camera || transform) ? 0 : request.ShaderRevision;
		const size_t wanted = State->ComposerCaptures.size() + (found == State->ComposerCaptures.end());
		std::vector<Impl::ComposerCaptureEntry> grown;
		if (wanted > State->ComposerCaptures.capacity()) {
			const uint64_t oldBacking =
				State->ComposerCaptures.capacity() * sizeof(Impl::ComposerCaptureEntry);
			const uint64_t sourceBytes = ownedRequest;
			if (sourceBytes > maximum || receiptBytes > maximum - sourceBytes ||
				*previousBytes > maximum - sourceBytes - receiptBytes) {
				failure = "Composer preview vector growth scratch exceeds budget";
				return false;
			}
			const uint64_t scratch = sourceBytes + receiptBytes + *previousBytes;
			if (!hlsl::detail::AdmitComposerVectorGrowth(
					oldBacking, wanted * sizeof(Impl::ComposerCaptureEntry), held, scratch, maximum
				)) {
				failure = "Composer preview vector growth exceeds operation budget";
				return false;
			}
			grown.reserve(wanted);
			core::Metrics::Count(
				"render.composer.capture_vector_backing_bytes",
				grown.capacity() * sizeof(Impl::ComposerCaptureEntry)
			);
			core::Metrics::Count("render.composer.capture_vector_allocations", 1);
			const auto nextHeld = hlsl::detail::AdmitComposerVectorGrowth(
				oldBacking, grown.capacity() * sizeof(Impl::ComposerCaptureEntry), held, scratch, maximum
			);
			if (!nextHeld || completionBytes > hlsl::MAXIMUM_SURFACE_JOB_BYTES - *nextHeld ||
				receiptBytes > hlsl::MAXIMUM_SURFACE_JOB_BYTES - *nextHeld - completionBytes) {
				failure = "Composer preview actual vector capacity exceeds budget";
				return false;
			}
			held = *nextHeld;
		}
		static_assert(std::is_nothrow_move_constructible_v<Impl::ComposerCaptureEntry>);
		static_assert(std::is_nothrow_move_assignable_v<Impl::ComposerCaptureEntry>);
		const auto result =
			camera		? QueueCameraResolved({owner, name, generation, std::move(cameraRequest)}, true)
			: transform ? QueueTransformImage3D(
							  {owner,
							   name,
							   generation,
							   imagegraph::TransformImage3DOutput::Rendered,
							   std::move(transformRequest)}
						  )
						: QueueComposerSurface({owner, name, generation, std::move(request)});
		using Result = imagegraph::TransformImage3DQueueResult;
		if (result != Result::Queued && result != Result::Replaced) {
			failure = "Composer preview source queue refused capture";
			return false;
		}
		if (grown.capacity() != 0) {
			for (auto &cached : State->ComposerCaptures)
				grown.push_back(std::move(cached));
			State->ComposerCaptures.swap(grown);
			found = std::find_if(
				State->ComposerCaptures.begin(), State->ComposerCaptures.end(), [&](const auto &cached) {
					return cached.Owner == owner && cached.Name == name;
				}
			);
		}
		Impl::ComposerCaptureEntry entry{
			owner,
			name,
			shader,
			generation,
			revision,
			receiptBytes + completionBytes,
			std::move(receipt),
			false,
			invocation.Interpolation
		};
		++State->NextComposerCaptureGeneration;
		if (found == State->ComposerCaptures.end())
			State->ComposerCaptures.push_back(std::move(entry));
		else
			*found = std::move(entry);
		for (auto &slot : State->GraphResources.Transform3D)
			if (slot.Owner == owner && slot.Name == name && slot.Generation == generation) {
				if (camera)
					slot.CameraCaptureReadback = true;
				else if (transform)
					slot.TransformCaptureReadback = true;
				else
					slot.ComposerCaptureReadback = true;
			}
		if (pending) *pending = true;
		failure = "Composer preview capture is pending";
		return false;
	} catch (const std::bad_alloc &) {
		if (pending) *pending = false;
		failure = "Composer asynchronous capture allocation failed";
		return false;
	}
	bool Renderer::CaptureComposerSurface(
		const engine::imagegraph::HostNodeInvocation &invocation,
		core::Name owner,
		engine::imagegraph::HostNodeCapture &output,
		std::string &failure
	) try {
		RequireOwningThread("CaptureComposerSurface");
		ENGINE_PROFILE("composer resolved host surface");
		using namespace engine::imagegraph;
		const auto previous = HostCaptureRetainedPayloadBytes(output);
		const uint64_t maximum = std::min(invocation.MaximumOperationBytes, hlsl::MAXIMUM_SURFACE_JOB_BYTES);
		if (!previous || *previous >= maximum) {
			failure = "Composer previous host recording exceeds budget";
			return false;
		}
		HostNodeCapture candidate;
		Diagnostic diagnostic;
		uint64_t receiptBytes = 0;
		if (PrepareResolvedHostCapture(
				invocation, maximum - *previous, candidate, receiptBytes, diagnostic
			) != Status::Ok) {
			failure = diagnostic.Message;
			return false;
		}
		const Image *base = nullptr;
		for (const auto &image : invocation.Images)
			if (image.Port == "base_texture") base = image.Data;
		if (!base || !ValidSurfaceLayout(*base, 4096, maximum)) {
			failure = "Composer host base texture is missing or invalid";
			return false;
		}
		const uint64_t pixels = uint64_t(base->Width) * base->Height * 4;
		const uint64_t metadata = sizeof(HostCapturedImage) + std::string{}.capacity() + 1;
		if (receiptBytes > maximum - *previous || pixels + metadata > maximum - *previous - receiptBytes) {
			failure = "Composer host receipt and output exceed budget";
			return false;
		}
		HostNodeInvocation bounded = invocation;
		bounded.MaximumOperationBytes = maximum - *previous - receiptBytes - pixels - metadata;
		hlsl::SurfaceRequest request;
		if (const auto error = BuildComposerSurface(bounded, owner, false, request)) {
			failure = *error;
			return false;
		}
		candidate.Images.reserve(1);
		Image image;
		if (const auto error = ExecuteComposerSurface(owner, request, image)) {
			failure = *error;
			return false;
		}
		candidate.Images.push_back({"surface", std::move(image)});
		const auto retained = HostCaptureRetainedPayloadBytes(candidate);
		const uint64_t requestBytes = hlsl::SurfaceSourceBytes(request);
		if (requestBytes > maximum - *previous || !retained ||
			*retained > maximum - *previous - requestBytes) {
			failure = "Composer completed host recording exceeds retained budget";
			return false;
		}
		output = std::move(candidate);
		failure.clear();
		return true;
	} catch (const std::bad_alloc &) {
		failure = "Composer host capture allocation failed";
		return false;
	}
	std::optional<std::string> Renderer::ExecuteComposerSurface(
		core::Name owner, const hlsl::SurfaceRequest &request, engine::imagegraph::Image &output
	) {
		RequireOwningThread("ExecuteComposerSurface");
		ENGINE_PROFILE("composer surface authoring readback");
		using Phase = Impl::GraphResourceCache::Transform3DPhase;
		if (!State || !State->Device || State->BatchActive)
			return "Composer readback needs an idle renderer device";
		if (const auto failure = hlsl::ValidateSurfaceRequest(request)) return failure;
		uint64_t revision = 0;
		const auto *installed = State->ComposerShaders.Find(owner, request.Shader, revision);
		if (!installed || revision != request.ShaderRevision || *installed != request.Pair)
			return "Composer readback cooked revision changed";
		Impl::GraphResourceCache::Transform3DSlot *slot = nullptr;
		for (auto &candidate : State->GraphResources.Transform3D) {
			if (candidate.Phase == Phase::Recorded)
				return "Composer readback cannot join an unsubmitted frame";
			if (candidate.Phase == Phase::Free) slot = &candidate;
		}
		const uint64_t source = hlsl::SurfaceSourceBytes(request);
		const uint64_t completionBytes = uint64_t(request.Textures[0].Width) * request.Textures[0].Height * 4;
		if (output.Pixels.capacity() > hlsl::MAXIMUM_SURFACE_JOB_BYTES ||
			completionBytes > hlsl::MAXIMUM_SURFACE_JOB_BYTES - output.Pixels.capacity())
			return "Composer readback previous output and replacement exceed CPU byte "
				   "budget";
		const uint64_t scratch = hlsl::SurfaceScratchBytes(request) +
								 uint64_t(request.Textures[0].Width) * request.Textures[0].Height * 4;
		if (!slot || source > hlsl::MAXIMUM_SURFACE_JOB_BYTES ||
			State->GraphResources.Transform3DSourceBytes > hlsl::MAXIMUM_SURFACE_JOB_BYTES - source ||
			scratch > 128ull * 1024 * 1024 ||
			State->GraphResources.Transform3DScratchBytes > 128ull * 1024 * 1024 - scratch)
			return "Composer readback exceeds retained job budget";
		auto *device = State->Device;
		auto *command = SDL_AcquireGPUCommandBuffer(device);
		if (!command) return "Composer readback command acquisition failed";
		slot->Owner = owner;
		slot->Cancelled = true;
		slot->Phase = Phase::Queued;
		slot->SourceBytes = source;
		State->GraphResources.Transform3DSourceBytes += source;
		auto cancel = [&] {
			SDL_CancelGPUCommandBuffer(command);
			State->ReleaseTransform3D(*slot);
		};
		if (!hlsl::RecordComposerSurface(
				device, command, request, slot->CameraResources, slot->ComposerDisplayDownload
			)) {
			cancel();
			return "Composer readback graph recording failed";
		}
		const auto &base = request.Textures[0];
		const uint32_t bytes = uint64_t(base.Width) * base.Height * 4;
		auto *download = imagegraph::gpu_helpers::Transfer(
			device, bytes, SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, slot->CameraResources
		);
		if (!download) {
			cancel();
			return "Composer readback transfer allocation failed";
		}
		auto *copy = SDL_BeginGPUCopyPass(command);
		if (!copy) {
			cancel();
			return "Composer readback copy pass failed";
		}
		SDL_GPUTextureRegion from{};
		from.texture = slot->CameraResources.Output;
		from.w = base.Width;
		from.h = base.Height;
		from.d = 1;
		SDL_GPUTextureTransferInfo to{};
		to.transfer_buffer = download;
		to.pixels_per_row = base.Width;
		to.rows_per_layer = base.Height;
		SDL_DownloadFromGPUTexture(copy, &from, &to);
		SDL_EndGPUCopyPass(copy);
		slot->Phase = Phase::Recorded;
		slot->ScratchBytes = scratch;
		State->GraphResources.Transform3DScratchBytes += scratch;
		// A cancelled publication slot owns export resources through the same real
		// submission fence.
		if (!State->SubmitSceneCommand(command)) return "Composer readback submission failed";
		SDL_GPUFence *fence = nullptr;
		const auto index = uint32_t(slot - State->GraphResources.Transform3D.data());
		for (auto &submission : State->GraphResources.PendingSceneSubmissions)
			for (uint32_t i = 0; i < submission.Transform3DCount; ++i)
				if (submission.Transform3DSlots[i] == index) fence = submission.Fence;
		if (!fence || !SDL_WaitForGPUFences(device, true, &fence, 1))
			return "Composer readback completion failed; resources remain fence-owned";
		void *mapped = SDL_MapGPUTransferBuffer(device, download, false);
		if (!mapped) {
			State->PollSceneFrames();
			return "Composer readback map failed";
		}
		engine::imagegraph::Image candidate;
		candidate.Width = base.Width;
		candidate.Height = base.Height;
		candidate.Format = engine::imagegraph::SurfaceFormat::RGBA8Unorm;
		try {
			const auto *data = static_cast<const uint8_t *>(mapped);
			candidate.Pixels.assign(data, data + bytes);
		} catch (const std::bad_alloc &) {
			SDL_UnmapGPUTransferBuffer(device, download);
			State->PollSceneFrames();
			return "Composer readback CPU allocation failed";
		}
		SDL_UnmapGPUTransferBuffer(device, download);
		core::Metrics::Count("render.composer.readback_bytes", bytes);
		core::Metrics::Count("render.composer.readbacks", 1);
		State->PollSceneFrames();
		candidate.Hash = engine::imagegraph::SurfaceHash(candidate);
		output = std::move(candidate);
		return {};
	}

} // namespace engine::render
