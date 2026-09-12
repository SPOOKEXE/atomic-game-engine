#include "RendererState.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>

#include <algorithm>
#include <bit>
#include <cstring>
#include <utility>

namespace engine::render {
	bool Renderer::CanPublishResourceImage(uint64_t token, uint32_t width, uint32_t height) const {
		RequireOwningThread("CanPublishResourceImage");
		return token != 0 && std::any_of(
								 State->ResourceImages.begin(),
								 State->ResourceImages.end(),
								 [&](const Impl::ResourceImageSlot &slot) {
									 return slot.Image.Request.Token == token && !slot.Cancelled &&
											slot.Resident != nullptr &&
											slot.Image.Request.Delivery == ResourceImageDelivery::Resident &&
											slot.Image.Status == ResourceImageStatus::Ok &&
											slot.Image.Width == width && slot.Image.Height == height &&
											(slot.Phase == Impl::ResourceImagePhase::Submitted ||
											 slot.Phase == Impl::ResourceImagePhase::Ready);
								 }
							 );
	}

	uint64_t Renderer::QueueResourceImage(
		core::Name pipeline,
		core::Name node,
		size_t viewSlot,
		ResourceImageDelivery delivery,
		std::string expectedSnapshotId
	) {
		RequireOwningThread("QueueResourceImage");
		if (expectedSnapshotId.empty()) {
			uint64_t token = 0;
			return QueueResourceImages(
					   pipeline, std::span(&node, 1), viewSlot, delivery, std::span(&token, 1)
				   )
					   ? token
					   : 0;
		}
		if (State->NextResourceImageToken == 0) return 0;
		const ResourceImageRequest request{
			.Token = State->NextResourceImageToken,
			.Pipeline = pipeline,
			.Node = node,
			.ViewSlot = viewSlot,
			.Delivery = delivery,
			.ExpectedSnapshotId = std::move(expectedSnapshotId),
		};
		if (!RequestResourceImage(request)) return 0;
		return State->NextResourceImageToken++;
	}

	bool Renderer::QueueResourceImages(
		core::Name pipeline,
		std::span<const core::Name> nodes,
		size_t viewSlot,
		ResourceImageDelivery delivery,
		std::span<uint64_t> tokens
	) {
		RequireOwningThread("QueueResourceImages");
		if (nodes.empty() || nodes.size() > State->ResourceImages.size() || tokens.size() != nodes.size())
			return false;
		std::array<ResourceImageRequest, std::tuple_size_v<decltype(State->ResourceImages)>> requests{};
		size_t next = 0;
		// Explicit caller tokens can occupy generated values. The slot array bounds live tokens.
		for (size_t attempt = 0; attempt < nodes.size() + State->ResourceImages.size(); ++attempt) {
			if (State->NextResourceImageToken == 0) return false;
			const uint64_t token = State->NextResourceImageToken++;
			const bool occupied = std::any_of(
				State->ResourceImages.begin(),
				State->ResourceImages.end(),
				[token](const Impl::ResourceImageSlot &slot) {
					return slot.Phase != Impl::ResourceImagePhase::Free && slot.Image.Request.Token == token;
				}
			);
			if (occupied) continue;
			requests[next] = {token, pipeline, nodes[next], viewSlot, delivery};
			if (++next != nodes.size()) continue;
			if (!RequestResourceImages(std::span(requests).first(next))) return false;
			for (size_t index = 0; index < next; ++index)
				tokens[index] = requests[index].Token;
			return true;
		}
		return false;
	}

	std::optional<ResourceImage> Renderer::TakeResourceImage(uint64_t token) {
		RequireOwningThread("TakeResourceImage");
		if (State->Device != nullptr) {
			State->PollSceneFrames();
		}
		for (Impl::ResourceImageSlot &slot : State->ResourceImages) {
			if (slot.Phase == Impl::ResourceImagePhase::Ready && slot.Image.Request.Token == token &&
				slot.Image.Request.Delivery == ResourceImageDelivery::CopiedPixels) {
				ResourceImage image = std::move(slot.Image);
				slot.Image = {};
				slot.Phase = Impl::ResourceImagePhase::Free;
				return image;
			}
		}
		return {};
	}

	bool Renderer::RequestResourceImage(const ResourceImageRequest &request) {
		return RequestResourceImages(std::span(&request, 1));
	}

	bool Renderer::RequestResourceImages(std::span<const ResourceImageRequest> requests) {
		RequireOwningThread("RequestResourceImages");
		if (State->Device == nullptr || requests.empty() || requests.size() > State->ResourceImages.size())
			return false;
		const auto &first = requests.front();
		const Impl::NamedPipeline *pipeline = State->PipelineFor(first.Pipeline);
		if (pipeline == nullptr || (first.Pipeline.IsValid() && first.Pipeline != pipeline->Name))
			return false;
		for (size_t index = 0; index < requests.size(); ++index) {
			const auto &request = requests[index];
			if (request.Token == 0 || !request.Node.IsValid() || request.Pipeline != first.Pipeline ||
				request.ViewSlot != first.ViewSlot || request.Delivery != first.Delivery ||
				(request.Delivery != ResourceImageDelivery::CopiedPixels &&
				 request.Delivery != ResourceImageDelivery::Resident))
				return false;
			for (size_t previous = 0; previous < index; ++previous)
				if (requests[previous].Token == request.Token) return false;
			bool declared = false;
			for (uint32_t nodeIndex = 1; nodeIndex <= pipeline->Graph.Count(); ++nodeIndex) {
				const graph::Node *node = pipeline->Graph.Find(graph::NodeId{nodeIndex});
				if (node->Name != request.Node || !node->Enabled) continue;
				if (node->Kind == core::Name("shadow-capture")) {
					declared = request.Delivery == ResourceImageDelivery::CopiedPixels &&
							   node->Scope == graph::NodeScope::View && node->Reads.size() == 1 &&
							   node->ReadPorts.size() == 1 && node->ReadPorts.front() == core::Name("shadow");
					break;
				}
				if (node->Kind != core::Name("capture")) continue;
				size_t colour = 0, depth = 0, normal = 0, response = 0, baseline = 0, directional = 0;
				bool portsValid = node->ReadPorts.size() == node->Reads.size();
				if (node->ReadPorts.empty() && node->Reads.size() == 1) {
					colour = 1;
					portsValid = true;
				} else {
					for (const auto port : node->ReadPorts) {
						if (port == core::Name("source"))
							++colour;
						else if (port == core::Name("depth"))
							++depth;
						else if (port == core::Name("normal"))
							++normal;
						else if (port == core::Name("ambient-response"))
							++response;
						else if (port == core::Name("lighting-baseline"))
							++baseline;
						else if (port == core::Name("directional-response"))
							++directional;
						else
							portsValid = false;
					}
				}
				declared = portsValid && colour == 1 && depth <= 1 && normal <= 1 && response == normal &&
						   baseline == normal && directional <= normal && (normal == 0 || depth == 1) &&
						   (node->Scope == graph::NodeScope::View ||
							(node->Scope == graph::NodeScope::Frame &&
							 node->Integer(core::Name("view"), 0) == request.ViewSlot));
				break;
			}
			if (!declared) return false;
		}
		State->PollSceneFrames();
		size_t available = 0;
		for (const auto &slot : State->ResourceImages) {
			if (slot.Phase == Impl::ResourceImagePhase::Free) {
				++available;
				continue;
			}
			for (const auto &request : requests)
				if (slot.Image.Request.Token == request.Token) return false;
		}
		if (available < requests.size()) {
			core::Metrics::Count("render.resource_image.queue_full", 1);
			return false;
		}
		// Validation and capacity checks finish before any slot changes ownership.
		size_t next = 0;
		for (auto &slot : State->ResourceImages) {
			if (slot.Phase != Impl::ResourceImagePhase::Free) continue;
			slot.Image = {};
			slot.Image.Request = requests[next++];
			slot.Image.Request.Pipeline = pipeline->Name;
			slot.Cancelled = false;
			slot.Phase = Impl::ResourceImagePhase::Queued;
			if (next == requests.size()) break;
		}
		return true;
	}

	std::optional<std::vector<ResourceImage>> Renderer::TakeResourceImages(std::span<const uint64_t> tokens) {
		RequireOwningThread("TakeResourceImages");
		if (tokens.empty() || tokens.size() > State->ResourceImages.size()) return {};
		if (State->Device != nullptr) State->PollSceneFrames();
		std::array<Impl::ResourceImageSlot *, std::tuple_size_v<decltype(State->ResourceImages)>> selected{};
		for (size_t index = 0; index < tokens.size(); ++index) {
			if (tokens[index] == 0) return {};
			for (size_t previous = 0; previous < index; ++previous)
				if (tokens[previous] == tokens[index]) return {};
			for (auto &slot : State->ResourceImages) {
				if (slot.Image.Request.Token == tokens[index] &&
					slot.Phase == Impl::ResourceImagePhase::Ready &&
					slot.Image.Request.Delivery == ResourceImageDelivery::CopiedPixels) {
					selected[index] = &slot;
					break;
				}
			}
			if (selected[index] == nullptr) return {};
		}
		std::vector<ResourceImage> images;
		images.reserve(tokens.size());
		for (size_t index = 0; index < tokens.size(); ++index) {
			auto &slot = *selected[index];
			images.push_back(std::move(slot.Image));
			slot.Image = {};
			slot.Phase = Impl::ResourceImagePhase::Free;
		}
		return images;
	}

	bool Renderer::CancelResourceImage(uint64_t token) {
		RequireOwningThread("CancelResourceImage");
		for (Impl::ResourceImageSlot &slot : State->ResourceImages) {
			if (slot.Phase == Impl::ResourceImagePhase::Free || slot.Image.Request.Token != token) {
				continue;
			}
			slot.Cancelled = true;
			if (slot.Phase == Impl::ResourceImagePhase::Queued ||
				slot.Phase == Impl::ResourceImagePhase::Ready) {
				State->ReleaseResidentImage(slot);
				slot.Image = {};
				slot.Phase = Impl::ResourceImagePhase::Free;
			}
			return true;
		}
		return false;
	}

	std::vector<ResourceImage> Renderer::TakeResourceImages() {
		RequireOwningThread("TakeResourceImages");
		if (State->Device != nullptr) {
			State->PollSceneFrames();
		}
		std::vector<ResourceImage> images;
		for (Impl::ResourceImageSlot &slot : State->ResourceImages) {
			if (slot.Phase != Impl::ResourceImagePhase::Ready ||
				slot.Image.Request.Delivery != ResourceImageDelivery::CopiedPixels) {
				continue;
			}
			images.push_back(std::move(slot.Image));
			slot.Image = {};
			slot.Phase = Impl::ResourceImagePhase::Free;
		}
		return images;
	}

	bool Renderer::Impl::HasShadowCaptureRequest(core::Name pipelineName, size_t viewSlot) const {
		const auto *pipeline = PipelineFor(pipelineName);
		if (!pipeline) return false;
		for (const auto &slot : ResourceImages) {
			if (slot.Phase != ResourceImagePhase::Queued || slot.Cancelled ||
				slot.Image.Request.Pipeline != pipeline->Name || slot.Image.Request.ViewSlot != viewSlot)
				continue;
			for (uint32_t index = 1; index <= pipeline->Graph.Count(); ++index) {
				const auto *node = pipeline->Graph.Find(graph::NodeId{index});
				if (node && node->Enabled && node->Name == slot.Image.Request.Node &&
					node->Kind == core::Name("shadow-capture"))
					return true;
			}
		}
		return false;
	}

	bool Renderer::Impl::EnsureResourceImageTransfer(ResourceImageSlot &slot, uint32_t bytes) {
		if (bytes > MAX_RESOURCE_IMAGE_STAGING_BYTES) return false;
		if (slot.TransferBytes >= bytes) return true;
		size_t allocated = 0;
		for (const auto &held : ResourceImages)
			allocated += held.TransferBytes;
		if (allocated - slot.TransferBytes + bytes > MAX_RESOURCE_IMAGE_STAGING_BYTES) {
			for (auto &held : ResourceImages) {
				if (&held == &slot || held.Phase == ResourceImagePhase::Recorded ||
					held.Phase == ResourceImagePhase::Submitted || !held.Transfer)
					continue;
				allocated -= held.TransferBytes;
				gpu::ReleaseTransferBuffer(Device, held.Transfer);
				held.Transfer = nullptr;
				held.TransferBytes = 0;
			}
		}
		if (allocated - slot.TransferBytes + bytes > MAX_RESOURCE_IMAGE_STAGING_BYTES) {
			core::Metrics::Count("render.resource_image.staging_refused", 1);
			return false;
		}
		gpu::ReleaseTransferBuffer(Device, slot.Transfer);
		slot.Transfer = nullptr;
		slot.TransferBytes = 0;
		SDL_GPUTransferBufferCreateInfo info{};
		info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
		info.size = bytes;
		slot.Transfer = gpu::CreateTransferBuffer(Device, &info);
		if (!slot.Transfer) return false;
		slot.TransferBytes = bytes;
		return true;
	}

	void Renderer::Impl::RecordShadowResourceImages(
		SDL_GPUCommandBuffer *command,
		core::Name pipeline,
		core::Name node,
		size_t viewSlot,
		core::Name resource,
		const NamedTexture &source,
		const ResourceShadowCapture &metadata
	) {
		for (auto &slot : ResourceImages) {
			const auto &request = slot.Image.Request;
			if (slot.Phase != ResourceImagePhase::Queued || request.Pipeline != pipeline ||
				request.Node != node || request.ViewSlot != viewSlot)
				continue;
			ENGINE_PROFILE("shadow image download");
			slot.Phase = ResourceImagePhase::Ready;
			slot.Image.CaptureFrame = FrameCounter;
			slot.Image.Kind = ResourceImageKind::DirectionalShadow;
			slot.Image.Resource = slot.Image.DepthResource = resource;
			slot.Image.Shadow = metadata;
			slot.TransferStride = slot.DepthStride = slot.NormalStride = slot.AmbientResponseStride =
				slot.LightingBaselineStride = slot.DirectionalResponseStride = 0;
			if (request.Delivery != ResourceImageDelivery::CopiedPixels || !source.IsValid() ||
				source.Texture != ShadowTexture || source.Format != SDL_GPU_TEXTUREFORMAT_D32_FLOAT ||
				source.Width > SHADOW_RESOLUTION || source.Width != source.Height) {
				slot.Image.Status = ResourceImageStatus::Unsupported;
				continue;
			}
			const uint32_t stride = (source.Width * 4 + 255) / 256 * 256;
			if (!EnsureResourceImageTransfer(slot, stride * source.Height)) continue;
			auto *copy = SDL_BeginGPUCopyPass(command);
			if (!copy) continue;
			SDL_GPUTextureRegion region{};
			region.texture = source.Texture;
			region.w = source.Width;
			region.h = source.Height;
			region.d = 1;
			SDL_GPUTextureTransferInfo destination{};
			destination.transfer_buffer = slot.Transfer;
			destination.pixels_per_row = stride / 4;
			destination.rows_per_layer = source.Height;
			SDL_DownloadFromGPUTexture(copy, &region, &destination);
			SDL_EndGPUCopyPass(copy);
			slot.DepthStride = stride;
			slot.DepthOffset = 0;
			slot.Image.Width = source.Width;
			slot.Image.Height = source.Height;
			slot.Image.RowStride = source.Width * 4;
			slot.Image.Status = ResourceImageStatus::Ok;
			slot.Phase = ResourceImagePhase::Recorded;
			core::Metrics::Count(
				"render.resource_image.download_bytes", size_t(source.Width) * source.Height * 4
			);
			core::Metrics::Count("render.resource_image.downloads", 1);
		}
	}

	namespace {
		bool CaptureFormat(SDL_GPUTextureFormat format, ResourceImageFormat &captured, uint32_t &bytes) {
			switch (format) {
			case SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT:
				captured = ResourceImageFormat::RGBA16_Float;
				bytes = 8;
				return true;
			case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM:
				captured = ResourceImageFormat::RGBA8_UNorm;
				bytes = 4;
				return true;
			case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB:
				captured = ResourceImageFormat::RGBA8_SRGB;
				bytes = 4;
				return true;
			default:
				return false;
			}
		}
	}

	void Renderer::Impl::RecordResourceImages(
		SDL_GPUCommandBuffer *command,
		core::Name pipeline,
		core::Name node,
		size_t viewSlot,
		core::Name resource,
		const NamedTexture &source,
		core::Name depthResource,
		const NamedTexture &depth,
		core::Name normalResource,
		const NamedTexture &normal,
		core::Name ambientResponseResource,
		const NamedTexture &ambientResponse,
		core::Name lightingBaselineResource,
		const NamedTexture &lightingBaseline,
		core::Name directionalResponseResource,
		const NamedTexture &directionalResponse
	) {
		const bool withDirectional = directionalResponseResource.IsValid();
		const bool withDepth = depthResource.IsValid();
		const bool withAmbient = normalResource.IsValid() && ambientResponseResource.IsValid() &&
								 lightingBaselineResource.IsValid();
		const std::array<const NamedTexture *, 6> planes{
			&source, &depth, &normal, &ambientResponse, &lightingBaseline, &directionalResponse
		};
		ResourceImageFormat sourceCaptureFormat = ResourceImageFormat::Unknown;
		uint32_t sourceBytes = 0;
		const bool supportedSource = CaptureFormat(source.Format, sourceCaptureFormat, sourceBytes);
		const std::array<uint32_t, 6> pixelBytes{sourceBytes, 4, 4, 16, 16, 16};
		const std::array<SDL_GPUTextureFormat, 6> formats{
			source.Format,
			SDL_GPU_TEXTUREFORMAT_R32_FLOAT,
			SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM,
			SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT,
			SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT,
			SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT
		};
		const size_t count = withDirectional ? 6 : withAmbient ? 5 : withDepth ? 2 : 1;
		for (ResourceImageSlot &slot : ResourceImages) {
			const auto &request = slot.Image.Request;
			if (slot.Phase != ResourceImagePhase::Queued || request.Pipeline != pipeline ||
				request.Node != node || request.ViewSlot != viewSlot)
				continue;
			ENGINE_PROFILE("resource image download");
			if (!request.ExpectedSnapshotId.empty() &&
				request.ExpectedSnapshotId != ActiveDataCaptureSource.SnapshotId) {
				slot.Image.Status = ResourceImageStatus::Failed;
				slot.Phase = ResourceImagePhase::Ready;
				continue;
			}
			slot.Image.CaptureFrame = FrameCounter;
			slot.Image.SnapshotId = ActiveDataCaptureSource.SnapshotId;
			const glm::mat4 camera = ActiveDataCaptureSource.CameraFrame.ToMatrix();
			for (size_t column = 0; column < 4; ++column)
				for (size_t row = 0; row < 4; ++row)
					slot.Image.CameraWorldFromCamera[column * 4 + row] = camera[column][row];
			slot.Image.CameraFieldOfViewRadians = ActiveDataCaptureSource.Camera.FieldOfViewRadians;
			slot.Image.CameraProjectionAvailable = ActiveDataCaptureSource.ProjectionAvailable;
			slot.Image.CameraProjection = ActiveDataCaptureSource.Projection;
			slot.Image.CameraNearPlane = ActiveDataCaptureSource.Camera.NearPlane;
			slot.Image.CameraFarPlane = ActiveDataCaptureSource.Camera.FarPlane;
			slot.Image.Resource = resource;
			slot.Image.DepthResource = depthResource;
			slot.Image.NormalResource = normalResource;
			slot.Image.AmbientResponseResource = ambientResponseResource;
			slot.Image.LightingBaselineResource = lightingBaselineResource;
			slot.Image.DirectionalResponseResource = directionalResponseResource;
			slot.DirectionalResponseStride = 0;
			slot.DepthStride = slot.NormalStride = slot.AmbientResponseStride = slot.LightingBaselineStride =
				0;
			slot.Phase = ResourceImagePhase::Ready;
			bool valid = supportedSource && source.Width <= 512 && source.Height <= 512 &&
						 normalResource.IsValid() == ambientResponseResource.IsValid() &&
						 normalResource.IsValid() == lightingBaselineResource.IsValid() &&
						 (!withAmbient || withDepth) && (!withDirectional || withAmbient);
			for (size_t plane = 0; plane < count; ++plane)
				valid = valid && planes[plane]->IsValid() && planes[plane]->Format == formats[plane] &&
						planes[plane]->Width == source.Width && planes[plane]->Height == source.Height;
			if (!valid) {
				const std::array names{
					resource,
					depthResource,
					normalResource,
					ambientResponseResource,
					lightingBaselineResource,
					directionalResponseResource
				};
				for (size_t plane = 0; plane < count; ++plane) {
					const auto &input = *planes[plane];
					if (input.IsValid() && input.Format == formats[plane] && input.Width == source.Width &&
						input.Height == source.Height)
						continue;
					ENGINE_WARN(
						"capture '{}' input '{}' unsupported: valid={} format={} extent={}x{}, expected "
						"format={} extent={}x{}",
						node.Text(),
						names[plane].Text(),
						input.IsValid(),
						int(input.Format),
						input.Width,
						input.Height,
						int(formats[plane]),
						source.Width,
						source.Height
					);
				}
				slot.Image.Status = ResourceImageStatus::Unsupported;
				continue;
			}
			if (request.Delivery == ResourceImageDelivery::Resident) {
				std::array<SDL_GPUTexture **, 6> residents{
					&slot.Resident,
					&slot.ResidentDepth,
					&slot.ResidentNormal,
					&slot.ResidentAmbientResponse,
					&slot.ResidentLightingBaseline,
					&slot.ResidentDirectionalResponse
				};
				if (!ReuseResidentImage(
						slot, source.Width, source.Height, withDepth, withAmbient, withDirectional
					)) {
					SDL_GPUTextureCreateInfo info{};
					info.type = SDL_GPU_TEXTURETYPE_2D;
					info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
					info.width = source.Width;
					info.height = source.Height;
					info.layer_count_or_depth = info.num_levels = 1;
					info.sample_count = SDL_GPU_SAMPLECOUNT_1;
					for (size_t plane = 0; plane < count; ++plane) {
						info.format = formats[plane];
						*residents[plane] = gpu::CreateTexture(Device, &info);
						if (!*residents[plane]) {
							valid = false;
							break;
						}
					}
					if (!valid) {
						ReleaseResidentImage(slot);
						continue;
					}
				}
				auto *copy = SDL_BeginGPUCopyPass(command);
				if (!copy) {
					ReleaseResidentImage(slot);
					continue;
				}
				for (size_t plane = 0; plane < count; ++plane) {
					SDL_GPUTextureLocation from{};
					from.texture = planes[plane]->Texture;
					SDL_GPUTextureLocation to{};
					to.texture = *residents[plane];
					SDL_CopyGPUTextureToTexture(copy, &from, &to, source.Width, source.Height, 1, false);
				}
				SDL_EndGPUCopyPass(copy);
			} else {
				// Each plane has backend-local row and image alignment; owned results have tight rows.
				std::array<uint32_t, 6> strides{}, offsets{};
				uint32_t bytes = 0;
				for (size_t plane = 0; plane < count; ++plane) {
					offsets[plane] = (bytes + 511) / 512 * 512;
					strides[plane] = (source.Width * pixelBytes[plane] + 255) / 256 * 256;
					bytes = offsets[plane] + strides[plane] * source.Height;
				}
				if (!EnsureResourceImageTransfer(slot, bytes)) continue;
				auto *copy = SDL_BeginGPUCopyPass(command);
				if (!copy) continue;
				for (size_t plane = 0; plane < count; ++plane) {
					SDL_GPUTextureRegion region{};
					region.texture = planes[plane]->Texture;
					region.w = source.Width;
					region.h = source.Height;
					region.d = 1;
					SDL_GPUTextureTransferInfo destination{};
					destination.transfer_buffer = slot.Transfer;
					destination.offset = offsets[plane];
					destination.pixels_per_row = strides[plane] / pixelBytes[plane];
					destination.rows_per_layer = source.Height;
					SDL_DownloadFromGPUTexture(copy, &region, &destination);
				}
				SDL_EndGPUCopyPass(copy);
				slot.TransferStride = strides[0];
				slot.DepthStride = strides[1];
				slot.DepthOffset = offsets[1];
				slot.NormalStride = strides[2];
				slot.NormalOffset = offsets[2];
				slot.AmbientResponseStride = strides[3];
				slot.AmbientResponseOffset = offsets[3];
				slot.LightingBaselineStride = strides[4];
				slot.LightingBaselineOffset = offsets[4];
				slot.DirectionalResponseStride = strides[5];
				slot.DirectionalResponseOffset = offsets[5];
			}
			slot.Image.Width = source.Width;
			slot.Image.Height = source.Height;
			slot.Image.RowStride = source.Width * sourceBytes;
			slot.Image.Format = sourceCaptureFormat;
			slot.Image.Status = ResourceImageStatus::Ok;
			slot.Phase = ResourceImagePhase::Recorded;
			const double bytes = double(source.Width) * source.Height *
								 (withDirectional ? 64
								  : withAmbient	  ? 48
								  : withDepth	  ? 12
												  : 8);
			const bool resident = request.Delivery == ResourceImageDelivery::Resident;
			core::Metrics::Count(
				resident ? "render.resource_image.resident_copy_bytes"
						 : "render.resource_image.download_bytes",
				bytes
			);
			core::Metrics::Count(
				resident ? "render.resource_image.resident_copies" : "render.resource_image.downloads", count
			);
		}
	}

	void Renderer::Impl::CollectResourceImage(uint32_t index) {
		ResourceImageSlot &slot = ResourceImages[index];
		if (slot.Cancelled) {
			ReleaseResidentImage(slot);
			slot.Image = {};
			slot.Phase = ResourceImagePhase::Free;
			return;
		}
		ENGINE_PROFILE("resource image collect");
		slot.Phase = ResourceImagePhase::Ready;
		if (slot.Image.Request.Delivery == ResourceImageDelivery::Resident) {
			return;
		}
		if (slot.Image.Status != ResourceImageStatus::Ok) {
			slot.Image.Width = slot.Image.Height = slot.Image.RowStride = 0;
			return;
		}
		const auto *mapped =
			static_cast<const std::byte *>(SDL_MapGPUTransferBuffer(Device, slot.Transfer, false));
		if (mapped == nullptr) {
			slot.Image.Status = ResourceImageStatus::Failed;
			slot.Image.Width = slot.Image.Height = slot.Image.RowStride = 0;
			return;
		}
		const std::array<std::vector<std::byte> *, 6> planes{
			&slot.Image.Pixels,
			&slot.Image.Depth,
			&slot.Image.Normal,
			&slot.Image.AmbientResponse,
			&slot.Image.LightingBaseline,
			&slot.Image.DirectionalResponse
		};
		const std::array<uint32_t, 6> strides{
			slot.TransferStride,
			slot.DepthStride,
			slot.NormalStride,
			slot.AmbientResponseStride,
			slot.LightingBaselineStride,
			slot.DirectionalResponseStride
		};
		const std::array<uint32_t, 6> offsets{
			0,
			slot.DepthOffset,
			slot.NormalOffset,
			slot.AmbientResponseOffset,
			slot.LightingBaselineOffset,
			slot.DirectionalResponseOffset
		};
		const std::array<uint32_t, 6> pixelBytes{slot.Image.RowStride / slot.Image.Width, 4, 4, 16, 16, 16};
		for (size_t plane = 0; plane < planes.size(); ++plane) {
			if (!strides[plane]) continue;
			auto &bytes = *planes[plane];
			const size_t rowBytes = size_t(slot.Image.Width) * pixelBytes[plane];
			bytes.resize(rowBytes * slot.Image.Height);
			for (uint32_t row = 0; row < slot.Image.Height; ++row)
				std::memcpy(
					bytes.data() + row * rowBytes,
					mapped + offsets[plane] + size_t(row) * strides[plane],
					rowBytes
				);
			if constexpr (std::endian::native == std::endian::big) {
				const size_t wordBytes = plane == 0 && slot.Image.Format == ResourceImageFormat::RGBA8_UNorm
											 ? 1
										 : plane == 0 ? 2
													  : 4;
				for (size_t offset = 0; offset < bytes.size(); offset += wordBytes)
					std::reverse(bytes.begin() + offset, bytes.begin() + offset + wordBytes);
			}
		}
		SDL_UnmapGPUTransferBuffer(Device, slot.Transfer);
		slot.Image.Status = ResourceImageStatus::Ok;
		core::Metrics::Count(
			"render.resource_image.copied_bytes",
			double(
				slot.Image.Pixels.size() + slot.Image.Depth.size() + slot.Image.Normal.size() +
				slot.Image.AmbientResponse.size() + slot.Image.LightingBaseline.size() +
				slot.Image.DirectionalResponse.size()
			)
		);
	}
	bool Renderer::Impl::ReuseResidentImage(
		ResourceImageSlot &slot, uint32_t width, uint32_t height, bool depth, bool ambient, bool directional
	) {
		for (auto &pair : ResidentImageCache) {
			if (!pair.Colour || pair.Width != width || pair.Height != height || bool(pair.Depth) != depth ||
				bool(pair.Normal) != ambient || bool(pair.AmbientResponse) != ambient ||
				bool(pair.LightingBaseline) != ambient || bool(pair.DirectionalResponse) != directional)
				continue;
			// These textures no longer belong to an imported image. Subsequent copies
			// stay ordered after earlier sampling on the renderer's submission queue.
			slot.Resident = pair.Colour;
			slot.ResidentDepth = pair.Depth;
			slot.ResidentNormal = pair.Normal;
			slot.ResidentAmbientResponse = pair.AmbientResponse;
			slot.ResidentLightingBaseline = pair.LightingBaseline;
			slot.ResidentDirectionalResponse = pair.DirectionalResponse;
			PortalImportUsage.CachedTextureBytes -= size_t(width) * height *
													(directional ? 64
													 : ambient	 ? 48
													 : depth	 ? 12
																 : 8);
			pair = {};
			ReportPortalImportUsage();
			core::Metrics::Count("render.resource_image.resident_reuses", 1);
			return true;
		}
		return false;
	}
	void Renderer::Impl::ReleaseResidentImageCache() {
		for (auto &pair : ResidentImageCache) {
			gpu::ReleaseTexture(Device, pair.Colour);
			gpu::ReleaseTexture(Device, pair.Depth);
			gpu::ReleaseTexture(Device, pair.Normal);
			gpu::ReleaseTexture(Device, pair.AmbientResponse);
			gpu::ReleaseTexture(Device, pair.LightingBaseline);
			gpu::ReleaseTexture(Device, pair.DirectionalResponse);
			pair = {};
		}
		PortalImportUsage.CachedTextureBytes = 0;
		NextResidentCache = 0;
		ReportPortalImportUsage();
	}
	void Renderer::Impl::ReleaseResidentImage(ResourceImageSlot &slot) {
		gpu::ReleaseTexture(Device, slot.Resident);
		slot.Resident = nullptr;
		gpu::ReleaseTexture(Device, slot.ResidentDepth);
		slot.ResidentDepth = nullptr;
		gpu::ReleaseTexture(Device, slot.ResidentNormal);
		slot.ResidentNormal = nullptr;
		gpu::ReleaseTexture(Device, slot.ResidentAmbientResponse);
		slot.ResidentAmbientResponse = nullptr;
		gpu::ReleaseTexture(Device, slot.ResidentLightingBaseline);
		slot.ResidentLightingBaseline = nullptr;
		gpu::ReleaseTexture(Device, slot.ResidentDirectionalResponse);
		slot.ResidentDirectionalResponse = nullptr;
	}
}
