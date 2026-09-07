#include "RendererState.hpp"

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
		core::Name pipeline, core::Name node, size_t viewSlot, ResourceImageDelivery delivery
	) {
		uint64_t token = 0;
		return QueueResourceImages(pipeline, std::span(&node, 1), viewSlot, delivery, std::span(&token, 1))
				   ? token
				   : 0;
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
		std::array<ResourceImageRequest, 4> requests{};
		size_t next = 0;
		// Explicit caller tokens can occupy generated values. At most four are live.
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
				if (node->Name != request.Node || !node->Enabled || node->Kind != core::Name("capture"))
					continue;
				declared = !node->Reads.empty() && node->Reads.size() <= 2 &&
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
		std::array<Impl::ResourceImageSlot *, 4> selected{};
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

	void Renderer::Impl::RecordResourceImages(
		SDL_GPUCommandBuffer *command,
		core::Name pipeline,
		core::Name node,
		size_t viewSlot,
		core::Name resource,
		const NamedTexture &source,
		core::Name depthResource,
		const NamedTexture &depth
	) {
		for (ResourceImageSlot &slot : ResourceImages) {
			const ResourceImageRequest &request = slot.Image.Request;
			if (slot.Phase != ResourceImagePhase::Queued || request.Pipeline != pipeline ||
				request.Node != node || request.ViewSlot != viewSlot) {
				continue;
			}
			ENGINE_PROFILE("resource image download");
			slot.Image.CaptureFrame = FrameCounter;
			slot.Image.Resource = resource;
			slot.Image.DepthResource = depthResource;
			slot.DepthStride = 0;
			slot.Phase = ResourceImagePhase::Ready;
			if (!source.IsValid() || source.Format != SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT ||
				source.Width > 512 || source.Height > 512 ||
				(depthResource.IsValid() &&
				 (!depth.IsValid() || depth.Format != SDL_GPU_TEXTUREFORMAT_R32_FLOAT ||
				  depth.Width != source.Width || depth.Height != source.Height))) {
				slot.Image.Status = ResourceImageStatus::Unsupported;
				continue;
			}
			if (request.Delivery == ResourceImageDelivery::Resident) {
				SDL_GPUTextureCreateInfo info{};
				info.type = SDL_GPU_TEXTURETYPE_2D;
				info.format = source.Format;
				info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
				info.width = source.Width;
				info.height = source.Height;
				info.layer_count_or_depth = 1;
				info.num_levels = 1;
				info.sample_count = SDL_GPU_SAMPLECOUNT_1;
				if (!ReuseResidentImage(slot, source.Width, source.Height, depthResource.IsValid())) {
					slot.Resident = gpu::CreateTexture(Device, &info);
					if (slot.Resident == nullptr) {
						continue;
					}
					if (depthResource.IsValid()) {
						info.format = depth.Format;
						slot.ResidentDepth = gpu::CreateTexture(Device, &info);
						if (slot.ResidentDepth == nullptr) {
							ReleaseResidentImage(slot);
							continue;
						}
					}
				}
				auto *copy = SDL_BeginGPUCopyPass(command);
				if (copy == nullptr) {
					ReleaseResidentImage(slot);
					continue;
				}
				SDL_GPUTextureLocation from{};
				from.texture = source.Texture;
				SDL_GPUTextureLocation to{};
				to.texture = slot.Resident;
				SDL_CopyGPUTextureToTexture(copy, &from, &to, source.Width, source.Height, 1, false);
				if (slot.ResidentDepth) {
					from.texture = depth.Texture;
					to.texture = slot.ResidentDepth;
					SDL_CopyGPUTextureToTexture(copy, &from, &to, source.Width, source.Height, 1, false);
				}
				SDL_EndGPUCopyPass(copy);
				slot.Image.Width = source.Width;
				slot.Image.Height = source.Height;
				slot.Image.RowStride = source.Width * 8;
				slot.Image.Status = ResourceImageStatus::Ok;
				slot.Phase = ResourceImagePhase::Recorded;
				core::Metrics::Count(
					"render.resource_image.resident_copy_bytes",
					double(source.Width) * source.Height * (depthResource.IsValid() ? 12 : 8)
				);
				core::Metrics::Count(
					"render.resource_image.resident_copies", depthResource.IsValid() ? 2 : 1
				);
				continue;
			}
			// The padded staging layout is backend-local. Only tight rows leave the renderer.
			const uint32_t stride = (source.Width * 8 + 255) / 256 * 256;
			const uint32_t depthStride = depthResource.IsValid() ? (source.Width * 4 + 255) / 256 * 256 : 0;
			const uint32_t depthOffset = (stride * source.Height + 511) / 512 * 512;
			const uint32_t bytes =
				depthStride ? depthOffset + depthStride * source.Height : stride * source.Height;
			if (slot.TransferBytes < bytes) {
				gpu::ReleaseTransferBuffer(Device, slot.Transfer);
				slot.Transfer = nullptr;
				slot.TransferBytes = 0;
				SDL_GPUTransferBufferCreateInfo info{};
				info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
				info.size = bytes;
				slot.Transfer = gpu::CreateTransferBuffer(Device, &info);
				if (slot.Transfer == nullptr) {
					continue;
				}

				slot.TransferBytes = bytes;
			}
			SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);
			if (copy == nullptr) {
				continue;
			}
			SDL_GPUTextureRegion region{};
			region.texture = source.Texture;
			region.w = source.Width;
			region.h = source.Height;
			region.d = 1;
			SDL_GPUTextureTransferInfo destination{};
			destination.transfer_buffer = slot.Transfer;
			destination.pixels_per_row = stride / 8;
			destination.rows_per_layer = source.Height;
			// Copy at the declared graph read, before an aliased texture can be reused.
			SDL_DownloadFromGPUTexture(copy, &region, &destination);
			if (depthStride) {
				region.texture = depth.Texture;
				destination.offset = depthOffset;
				destination.pixels_per_row = depthStride / 4;
				SDL_DownloadFromGPUTexture(copy, &region, &destination);
			}
			SDL_EndGPUCopyPass(copy);
			slot.Image.Width = source.Width;
			slot.Image.Height = source.Height;
			slot.Image.RowStride = source.Width * 8;
			slot.Image.Status = ResourceImageStatus::Ok;
			slot.TransferStride = stride;
			slot.DepthStride = depthStride;
			slot.DepthOffset = depthOffset;
			slot.Phase = ResourceImagePhase::Recorded;
			core::Metrics::Count(
				"render.resource_image.download_bytes",
				double(source.Width) * source.Height * (depthResource.IsValid() ? 12 : 8)
			);
			core::Metrics::Count("render.resource_image.downloads", depthStride ? 2 : 1);
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
		slot.Image.Pixels.resize(size_t(slot.Image.RowStride) * slot.Image.Height);
		for (uint32_t row = 0; row < slot.Image.Height; row++) {
			std::memcpy(
				slot.Image.Pixels.data() + size_t(row) * slot.Image.RowStride,
				mapped + size_t(row) * slot.TransferStride,
				slot.Image.RowStride
			);
		}
		if (slot.DepthStride) {
			const size_t rowBytes = size_t(slot.Image.Width) * 4;
			slot.Image.Depth.resize(rowBytes * slot.Image.Height);
			const auto *depth = mapped + slot.DepthOffset;
			for (uint32_t row = 0; row < slot.Image.Height; ++row)
				std::memcpy(
					slot.Image.Depth.data() + row * rowBytes, depth + size_t(row) * slot.DepthStride, rowBytes
				);
		}
		SDL_UnmapGPUTransferBuffer(Device, slot.Transfer);
		if constexpr (std::endian::native == std::endian::big) {
			for (size_t offset = 0; offset < slot.Image.Pixels.size(); offset += 2) {
				std::swap(slot.Image.Pixels[offset], slot.Image.Pixels[offset + 1]);
			}
			for (size_t offset = 0; offset < slot.Image.Depth.size(); offset += 4) {
				std::swap(slot.Image.Depth[offset], slot.Image.Depth[offset + 3]);
				std::swap(slot.Image.Depth[offset + 1], slot.Image.Depth[offset + 2]);
			}
		}
		slot.Image.Status = ResourceImageStatus::Ok;
		core::Metrics::Count(
			"render.resource_image.copied_bytes", double(slot.Image.Pixels.size() + slot.Image.Depth.size())
		);
	}
	bool
	Renderer::Impl::ReuseResidentImage(ResourceImageSlot &slot, uint32_t width, uint32_t height, bool depth) {
		for (auto &pair : ResidentImageCache) {
			if (!pair.Colour || pair.Width != width || pair.Height != height || bool(pair.Depth) != depth)
				continue;
			// These textures no longer belong to an imported image. Subsequent copies
			// stay ordered after earlier sampling on the renderer's submission queue.
			slot.Resident = pair.Colour;
			slot.ResidentDepth = pair.Depth;
			PortalImportUsage.CachedTextureBytes -= size_t(width) * height * (depth ? 12 : 8);
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
	}
}
