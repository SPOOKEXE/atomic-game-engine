#include "RendererState.hpp"
#include "compute/ImageGraphGpu.hpp"

#include <engine/core/FrameGraph.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <limits>
#include <numbers>

namespace engine::render {
	namespace imagegraph_gpu_detail {
		uint64_t Key(core::Name owner, core::Name name) {
			return (uint64_t(owner.Id()) << 32) | name.Id();
		}
		bool Refuse(imagegraph::Diagnostic &diagnostic, std::string_view node, std::string_view message) {
			diagnostic = {std::string(node.substr(0, 128)), std::string(message.substr(0, 4096))};
			core::Metrics::Count("render.imagegraph.refusals", 1);
			return false;
		}
		uint64_t Fold(uint64_t signature, uint64_t word) {
			return (signature ^ word) * 1099511628211ull;
		}
		uint64_t Text(uint64_t signature, std::string_view text) {
			for (unsigned char character : text)
				signature = Fold(signature, character);
			return Fold(signature, text.size());
		}
		uint64_t Signature(const imagegraph::Node &node) {
			uint64_t signature = Fold(14695981039346656037ull, imagegraph::Fingerprint(node.Value));
			for (const std::string &input : node.Inputs)
				signature = Text(signature, input);
			return signature;
		}
		const ImageGraphSourceBinding *
		SourceBinding(const ImageGraphGpuDocument &graph, const imagegraph::Source &source) {
			const auto found =
				std::find_if(graph.Sources.begin(), graph.Sources.end(), [&](const auto &binding) {
					return binding.Path == source.Path && binding.Interpretation == source.Interpretation;
				});
			return found == graph.Sources.end() ? nullptr : &*found;
		}
		uint64_t GeometrySignature(const imagegraph::Document &document) {
			uint64_t signature = Fold(14695981039346656037ull, static_cast<uint64_t>(document.Storage));
			for (const auto &node : document.Nodes) {
				signature = Text(Text(signature, node.Id), imagegraph::Kind(node.Value));
				// Kinds after blend size their extent or work from several properties.
				if (node.Value.index() > 6) signature = Fold(signature, imagegraph::Fingerprint(node.Value));
				for (const auto &input : node.Inputs)
					signature = Text(signature, input);
				std::visit(
					[&](const auto &value) {
						using T = std::decay_t<decltype(value)>;
						if constexpr (std::is_same_v<T, imagegraph::Solid> ||
									  std::is_same_v<T, imagegraph::Resize> ||
									  std::is_same_v<T, imagegraph::Crop> ||
									  std::is_same_v<T, imagegraph::Transform>)
							signature = Fold(Fold(signature, value.Width), value.Height);
						// Filter changes alter the admitted pixel-work bound.
						if constexpr (std::is_same_v<T, imagegraph::Resize> ||
									  std::is_same_v<T, imagegraph::Transform>)
							signature = Fold(signature, static_cast<uint64_t>(value.Filter));
					},
					node.Value
				);
			}
			return signature;
		}
		bool FloatCoordinate(double value) {
			return std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max() &&
				   (value == 0 || std::abs(value) >= std::numeric_limits<float>::denorm_min());
		}
		bool Uniforms(
			const imagegraph::Node &node,
			const imagegraph::ImageExtent &extent,
			const imagegraph::ImageExtent &input,
			assets::TextureFormat sourceFormat,
			ImageGraphGpuUniforms &out,
			imagegraph::Diagnostic &diagnostic
		) {
			out = {};
			out.Control[0] = static_cast<uint32_t>(node.Value.index());
			out.Extents = {
				static_cast<int32_t>(extent.Width),
				static_cast<int32_t>(extent.Height),
				static_cast<int32_t>(input.Width),
				static_cast<int32_t>(input.Height)
			};
			return std::visit(
				[&](const auto &value) {
					using T = std::decay_t<decltype(value)>;
					if constexpr (std::is_same_v<T, imagegraph::Source>) {
						const bool encodedSampler = sourceFormat == assets::TextureFormat::RGBA8 ||
													sourceFormat == assets::TextureFormat::RGBA4_SRGB;
						out.Control[2] = encodedSampler ||
										 value.Interpretation == imagegraph::SourceInterpretation::Colour;
						if (sourceFormat == assets::TextureFormat::R8 ||
							sourceFormat == assets::TextureFormat::R16_FLOAT ||
							sourceFormat == assets::TextureFormat::R32_FLOAT) {
							// Ordinary R8 uploads expand to grayscale, but numeric imports
							// retain the raw asset layout's missing green/blue channels.
							out.Control[2] |=
								value.Interpretation == imagegraph::SourceInterpretation::Data ? 4u : 2u;
						}
					}
					if constexpr (std::is_same_v<T, imagegraph::Solid>)
						for (size_t channel = 0; channel < 4; ++channel)
							out.Colour[channel] = value.Colour[channel];
					if constexpr (std::is_same_v<T, imagegraph::Crop>) out.Region = {value.X, value.Y, 0, 0};
					if constexpr (std::is_same_v<T, imagegraph::Flip>)
						out.Region = {value.Horizontal, value.Vertical, 0, 0};
					const int32_t present = node.Inputs.empty() ? 0 : 1;
					if constexpr (std::is_same_v<T, imagegraph::Blend>) {
						out.Parameters[2] = static_cast<float>(value.Opacity);
						out.Control[3] = static_cast<uint32_t>(value.Mode);
						out.Region = {0, node.Inputs.size() == 3, 0, 0};
					}
					if constexpr (std::is_same_v<T, imagegraph::Resize> ||
								  std::is_same_v<T, imagegraph::Transform> ||
								  std::is_same_v<T, imagegraph::Warp> ||
								  std::is_same_v<T, imagegraph::Lookup>)
						out.Control[1] = value.Filter == imagegraph::Sampling::Bilinear;
					// Generators read an optional Vector input of any size.
					const auto generator = [&](uint32_t seed) {
						out.Region = {
							static_cast<int32_t>(seed),
							present,
							static_cast<int32_t>(input.Width),
							static_cast<int32_t>(input.Height)
						};
					};
					if constexpr (std::is_same_v<T, imagegraph::Noise>) {
						out.Control[3] = static_cast<uint32_t>(value.Type);
						out.Values[0] = {
							static_cast<float>(value.Scale),
							static_cast<float>(value.Octaves),
							static_cast<float>(value.Lacunarity),
							static_cast<float>(value.Gain)
						};
						out.Values[1] = {
							static_cast<float>(
								std::remainder(value.Degrees, 360.0) * std::numbers::pi / 180.0
							),
							static_cast<float>(value.Frequency),
							static_cast<float>(value.Bandwidth),
							static_cast<float>(value.Impulses)
						};
						generator(value.Seed);
					}
					if constexpr (std::is_same_v<T, imagegraph::Voronoi>) {
						out.Control[3] = static_cast<uint32_t>(value.Feature);
						out.Values[0] = {
							static_cast<float>(value.Scale), static_cast<float>(value.Randomness), 0, 0
						};
						generator(value.Seed);
					}
					if constexpr (std::is_same_v<T, imagegraph::Gradient>) {
						out.Control[3] = static_cast<uint32_t>(value.Type);
						out.Values[0] = {
							static_cast<float>(value.StartX),
							static_cast<float>(value.StartY),
							static_cast<float>(value.EndX),
							static_cast<float>(value.EndY)
						};
						generator(0);
					}
					if constexpr (std::is_same_v<T, imagegraph::Wave>) {
						out.Control[3] =
							static_cast<uint32_t>(value.Type) | static_cast<uint32_t>(value.Profile) << 4;
						out.Values[0] = {
							static_cast<float>(value.Scale),
							static_cast<float>(
								std::remainder(value.Degrees, 360.0) * std::numbers::pi / 180.0
							),
							static_cast<float>(value.Phase),
							static_cast<float>(value.Distortion)
						};
						out.Values[1] = {
							static_cast<float>(value.DetailScale), static_cast<float>(value.Detail), 0, 0
						};
						generator(value.Seed);
					}
					if constexpr (std::is_same_v<T, imagegraph::Ramp>) {
						out.Control[3] = static_cast<uint32_t>(value.Factor) |
										 static_cast<uint32_t>(value.Interpolation) << 4;
						out.Region = {static_cast<int32_t>(value.Stops.size()), 0, 0, 0};
						for (size_t stop = 0; stop < value.Stops.size(); ++stop) {
							out.StopPositions[stop / 4][stop % 4] =
								static_cast<float>(value.Stops[stop].Position);
							for (size_t channel = 0; channel < 4; ++channel)
								out.StopColours[stop][channel] = value.Stops[stop].Colour[channel];
						}
					}
					if constexpr (std::is_same_v<T, imagegraph::Levels>) {
						out.Control[3] = value.Channels;
						out.Values[0] = {
							static_cast<float>(value.InputMinimum),
							static_cast<float>(value.InputMaximum),
							static_cast<float>(value.OutputMinimum),
							static_cast<float>(value.OutputMaximum)
						};
						out.Values[1] = {static_cast<float>(value.Gamma), value.Clamp ? 1.0f : 0.0f, 0, 0};
					}
					if constexpr (std::is_same_v<T, imagegraph::Math>) {
						out.Control[3] = static_cast<uint32_t>(value.Operation) | uint32_t(value.Channels)
																					  << 4;
						for (size_t channel = 0; channel < 4; ++channel)
							out.Values[0][channel] = static_cast<float>(value.Value[channel]);
						out.Region = {0, node.Inputs.size() == 2, 0, 0};
					}
					if constexpr (std::is_same_v<T, imagegraph::Channels>)
						for (size_t channel = 0; channel < 4; ++channel) {
							const auto &pick = value.Picks[channel];
							out.Region[channel] = pick.Input < 0 ? pick.Input : pick.Input * 4 + pick.Channel;
						}
					if constexpr (std::is_same_v<T, imagegraph::Invert>) out.Control[3] = value.Channels;
					if constexpr (std::is_same_v<T, imagegraph::Blur>)
						out.Values[0][0] = static_cast<float>(value.Radius);
					if constexpr (std::is_same_v<T, imagegraph::NormalMap>) {
						out.Control[3] = static_cast<uint32_t>(value.Height) | uint32_t(value.FlipY) << 4;
						out.Values[0][0] = static_cast<float>(value.Strength);
					}
					if constexpr (std::is_same_v<T, imagegraph::Warp>)
						out.Values[0][0] = static_cast<float>(value.Strength);
					if constexpr (std::is_same_v<T, imagegraph::Lookup>) {
						out.Control[3] = static_cast<uint32_t>(value.Wrap);
						out.Values[0] = {
							static_cast<float>(value.ScaleX),
							static_cast<float>(value.ScaleY),
							static_cast<float>(value.OffsetX),
							static_cast<float>(value.OffsetY)
						};
					}
					if constexpr (std::is_same_v<T, imagegraph::MeshAttribute>)
						return Refuse(
							diagnostic, node.Id, "mesh attributes must be lowered to data sources by the host"
						);
					if constexpr (std::is_same_v<T, imagegraph::Transform>) {
						const double radians =
							std::remainder(value.Degrees, 360.0) * std::numbers::pi / 180.0;
						const double cosine = std::cos(radians), sine = std::sin(radians);
						const std::array<double, 4> affine{
							cosine / value.ScaleX,
							sine / value.ScaleX,
							-sine / value.ScaleY,
							cosine / value.ScaleY
						};
						const std::array<double, 2> offset{
							value.PivotX - affine[0] * (value.PivotX + value.TranslateX) -
								affine[1] * (value.PivotY + value.TranslateY),
							value.PivotY - affine[2] * (value.PivotX + value.TranslateX) -
								affine[3] * (value.PivotY + value.TranslateY)
						};
						for (size_t index = 0; index < affine.size(); ++index) {
							if (!FloatCoordinate(affine[index]))
								return Refuse(
									diagnostic, node.Id, "transform exceeds GPU coordinate precision"
								);
							out.Affine[index] = static_cast<float>(affine[index]);
						}
						for (size_t index = 0; index < offset.size(); ++index) {
							if (!FloatCoordinate(offset[index]))
								return Refuse(
									diagnostic, node.Id, "transform exceeds GPU coordinate precision"
								);
							out.Parameters[index] = static_cast<float>(offset[index]);
						}
					}
					return true;
				},
				node.Value
			);
		}
		SDL_GPUTexture *Allocate(
			SDL_GPUDevice *device,
			const imagegraph::ImageExtent &extent,
			SDL_GPUTextureFormat format,
			SDL_GPUTextureUsageFlags usage,
			ImageGraphGpuState &state
		) {
			SDL_GPUTextureCreateInfo description{};
			description.type = SDL_GPU_TEXTURETYPE_2D;
			description.format = format;
			description.usage = usage;
			description.width = extent.Width;
			description.height = extent.Height;
			description.layer_count_or_depth = description.num_levels = 1;
			description.sample_count = SDL_GPU_SAMPLECOUNT_1;
			SDL_GPUTexture *texture = gpu::CreateTexture(device, &description);
			if (texture) {
				state.Profile.AllocatedBytes += extent.Bytes;
				state.Profile.ResidentBytes += extent.Bytes;
				core::Metrics::Count("render.imagegraph.allocated_bytes", extent.Bytes);
			}
			return texture;
		}
		// A blur's row pass is always wide.
		size_t ScratchBytes(const imagegraph::ImageExtent &extent) {
			return size_t(extent.Width) * extent.Height * 8;
		}
		constexpr std::array<SDL_GPUTextureFormat, 4> PUBLICATION_FORMATS{
			SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB,
			SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
			SDL_GPU_TEXTUREFORMAT_R16_FLOAT,
			SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT
		};
		size_t PublicationSlot(imagegraph::OutputFormat format, imagegraph::OutputSpace space) {
			if (format == imagegraph::OutputFormat::R16F) return 2;
			if (format == imagegraph::OutputFormat::RGBA16F) return 3;
			return space == imagegraph::OutputSpace::SRGB ? 0 : 1;
		}
		void
		Release(SDL_GPUDevice *device, SDL_GPUTexture *texture, size_t bytes, ImageGraphGpuState &state) {
			if (!texture) return;
			gpu::ReleaseTexture(device, texture);
			state.Profile.ResidentBytes -= std::min<uint64_t>(state.Profile.ResidentBytes, bytes);
		}
	}

	bool ImageGraphGpuState::Reserve(
		TextureTable &table, core::Name owner, core::Name name, size_t bytes, Reservation &reservation
	) {
		const uint64_t key = imagegraph_gpu_detail::Key(owner, name);
		const auto found = table.Textures.find(key);
		const size_t oldBytes = found == table.Textures.end() ? 0 : found->second.Bytes;
		const size_t retained = table.UploadedBytes - std::min(table.UploadedBytes, oldBytes);
		if (retained > table.MaximumBytes || bytes > table.MaximumBytes - retained) return false;
		const auto [entry, inserted] = table.Textures.try_emplace(key);
		(void)entry;
		reservation = {key, inserted};
		return true;
	}
	void ImageGraphGpuState::Cancel(TextureTable &table, const Reservation &reservation) {
		if (reservation.Inserted) table.Textures.erase(reservation.Key);
	}
	SDL_GPUTexture *ImageGraphGpuState::Commit(
		TextureTable &table,
		const Reservation &reservation,
		SDL_GPUTexture *texture,
		const imagegraph::ImageExtent &extent,
		imagegraph::OutputSpace space,
		imagegraph::OutputFormat format
	) {
		auto &entry = table.Textures.find(reservation.Key)->second;
		SDL_GPUTexture *retired = entry.Texture == texture ? nullptr : entry.Texture;
		table.UploadedBytes -= std::min(table.UploadedBytes, entry.Bytes);
		table.RetainedCopyBytes -= std::min(table.RetainedCopyBytes, entry.SourcePixels.size());
		entry = {};
		entry.Texture = texture;
		entry.Bytes = extent.Bytes;
		entry.Width = extent.Width;
		entry.Height = extent.Height;
		entry.Format = format == imagegraph::OutputFormat::R16F		 ? assets::TextureFormat::R16_FLOAT
					   : format == imagegraph::OutputFormat::RGBA16F ? assets::TextureFormat::RGBA16_FLOAT
					   : space == imagegraph::OutputSpace::SRGB		 ? assets::TextureFormat::RGBA8
																	 : assets::TextureFormat::RGBA8_LINEAR;
		entry.Revision = ++table.ContentGeneration;
		table.UploadedBytes += extent.Bytes;
		table.Awaiting.erase(reservation.Key);
		return retired;
	}
	void ImageGraphGpuState::Touch(TextureTable &table, core::Name owner, core::Name name) {
		const auto found = table.Textures.find(imagegraph_gpu_detail::Key(owner, name));
		if (found != table.Textures.end()) found->second.Revision = ++table.ContentGeneration;
	}
	void ImageGraphGpuState::Collect(SDL_GPUDevice *device) {
		for (Pending &pending : PendingSubmissions) {
			if (!pending.Fence || !SDL_QueryGPUFence(device, pending.Fence)) continue;
			SDL_ReleaseGPUFence(device, pending.Fence);
			for (size_t index = 0; index < pending.Count; ++index)
				imagegraph_gpu_detail::Release(device, pending.Retired[index], pending.Bytes[index], *this);
			pending = {};
		}
		for (uint32_t slot = 0; slot < VulkanTimestamps::SLOTS; ++slot) {
			if (!Timestamps.Pending(slot)) continue;
			std::array<double, VulkanTimestamps::MARKS> times{};
			uint32_t count = 0;
			if (!Timestamps.Collect(slot, times.data(), count)) continue;
			if (count >= 2) {
				const double microseconds = VulkanTimestamps::Between(times.data(), 0, 1) / 1000.0;
				core::FrameGraph::Report(
					"gpu imagegraph", core::ProfileCategory::Gpu, static_cast<float>(microseconds / 1000.0)
				);
				if (TimingSequences[slot] >= Profile.GpuTimingSequence) {
					Profile.GpuTimingSequence = TimingSequences[slot];
					Profile.GpuMicroseconds = microseconds;
				}
			}
			TimingSequences[slot] = 0;
		}
	}
	void ImageGraphGpuState::AbandonJob(SDL_GPUDevice *device) {
		for (const auto &image : Job.Pool)
			imagegraph_gpu_detail::Release(device, image.Texture, image.Extent.Bytes, *this);
		Job = {};
	}
	void ImageGraphGpuState::ReleaseDocument(
		SDL_GPUDevice *device, TextureTable &table, ImageGraphGpuDocument &graph
	) {
		if (Job.Graph == &graph) AbandonJob(device);
		// SDL defers native destruction until submitted readers complete. Dropping
		// the public handle now prevents later submissions from reusing it.
		for (auto &node : graph.Nodes) {
			imagegraph_gpu_detail::Release(device, node.Texture, node.Extent.Bytes, *this);
			imagegraph_gpu_detail::Release(
				device, node.Scratch, imagegraph_gpu_detail::ScratchBytes(node.Extent), *this
			);
		}
		for (const auto &publication : graph.Publications) {
			if (table.Find(publication.Name, graph.Owner) != publication.Texture) continue;
			if (table.Drop(publication.Name, graph.Owner))
				Profile.ResidentBytes -= std::min<uint64_t>(Profile.ResidentBytes, publication.Extent.Bytes);
		}
		graph = {};
	}
	void ImageGraphGpuState::Shutdown(SDL_GPUDevice *device, TextureTable &table) {
		AbandonJob(device);
		if (JobFence) SDL_ReleaseGPUFence(device, JobFence);
		JobFence = nullptr;
		JobRetryAt = {};
		for (auto &[key, graph] : Documents) {
			(void)key;
			ReleaseDocument(device, table, graph);
		}
		Documents.clear();
		for (Pending &pending : PendingSubmissions) {
			if (pending.Fence) SDL_ReleaseGPUFence(device, pending.Fence);
			for (size_t index = 0; index < pending.Count; ++index)
				imagegraph_gpu_detail::Release(device, pending.Retired[index], pending.Bytes[index], *this);
			pending = {};
		}
		Timestamps.Shutdown();
		if (Compute) SDL_ReleaseGPUComputePipeline(device, Compute);
		if (Wide) SDL_ReleaseGPUComputePipeline(device, Wide);
		for (auto *&pipeline : Publish) {
			if (pipeline) SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
			pipeline = nullptr;
		}
		Compute = nullptr;
		Wide = nullptr;
		Profile = {};
		Sequence = 0;
		TimingSequences = {};
	}

	bool Renderer::Impl::EnsureImageGraphPipelines() {
		const auto storageUsage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
		const auto colourUsage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
		if (!ImageGraphs.Compute) {
			if (!SDL_GPUTextureSupportsFormat(
					Device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTURETYPE_2D, storageUsage
				))
				return false;
			ImageGraphs.Compute = LoadComputePipeline("imagegraph.comp", 4, 0, 1, 0, 8, 8);
			// Wide precision and blur need rgba16 storage; without it they are refused.
			if (SDL_GPUTextureSupportsFormat(
					Device, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UNORM, SDL_GPU_TEXTURETYPE_2D, storageUsage
				))
				ImageGraphs.Wide = LoadComputePipeline("imagegraph-wide.comp", 4, 0, 1, 0, 8, 8);
		}
		if (!ImageGraphs.Publish[0] &&
			!SDL_GPUTextureSupportsFormat(
				Device, imagegraph_gpu_detail::PUBLICATION_FORMATS[0], SDL_GPU_TEXTURETYPE_2D, colourUsage
			))
			return false;
		if (ImageGraphs.Compute && !ImageGraphs.Profile.HasGpuTimings)
			ImageGraphs.Profile.HasGpuTimings = ImageGraphs.Timestamps.Probe(Device);
		if (ImageGraphs.Compute && !ImageGraphs.Instrumented) {
			// Establish bounded metric rows before a successful submit commits ownership.
			for (std::string_view metric :
				 {"render.imagegraph.cache_hits",
				  "render.imagegraph.dispatches",
				  "render.imagegraph.command_buffers",
				  "render.imagegraph.published_bytes",
				  "render.imagegraph.allocated_bytes",
				  "render.imagegraph.refusals"})
				core::Metrics::Count(metric, 0);
			ImageGraphs.Instrumented = true;
		}
		return ImageGraphs.Compute && EnsureImageGraphPublication(0);
	}

	bool Renderer::Impl::EnsureImageGraphPublication(size_t slot) {
		if (ImageGraphs.Publish[slot]) return true;
		SDL_GPUShader *vertex = LoadShader("overlay.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
		SDL_GPUShader *fragment = LoadShader("imagegraph-publish.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
		if (vertex && fragment) {
			SDL_GPUColorTargetDescription target{};
			target.format = imagegraph_gpu_detail::PUBLICATION_FORMATS[slot];
			SDL_GPUGraphicsPipelineCreateInfo pipeline{};
			pipeline.vertex_shader = vertex;
			pipeline.fragment_shader = fragment;
			pipeline.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
			pipeline.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
			pipeline.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
			pipeline.target_info.color_target_descriptions = &target;
			pipeline.target_info.num_color_targets = 1;
			ImageGraphs.Publish[slot] = SDL_CreateGPUGraphicsPipeline(Device, &pipeline);
		}
		if (vertex) SDL_ReleaseGPUShader(Device, vertex);
		if (fragment) SDL_ReleaseGPUShader(Device, fragment);
		return ImageGraphs.Publish[slot] != nullptr;
	}

	bool Renderer::SetImageGraph(
		core::Name owner,
		core::Name name,
		const imagegraph::Document &document,
		std::span<const ImageGraphSourceBinding> sources,
		imagegraph::Diagnostic &diagnostic
	) {
		RequireOwningThread("SetImageGraph");
		using namespace imagegraph_gpu_detail;
		if (!State || !State->Device || !name.IsValid())
			return Refuse(diagnostic, {}, "renderer or graph name unavailable");
		try {
			ImageGraphGpuDocument candidate;
			candidate.Owner = owner;
			if (!imagegraph::ResolveInputs(document, {}, candidate.Document, diagnostic) ||
				!imagegraph::Compile(candidate.Document, candidate.Plan, diagnostic))
				return false;
			if (sources.size() > imagegraph::Limits::MaximumNodes)
				return Refuse(diagnostic, {}, "too many resident sources");
			for (size_t index = 0; index < sources.size(); ++index) {
				const auto &source = sources[index];
				if (!source.Texture.IsValid() || source.Path.empty() || source.Path.size() > 4096)
					return Refuse(diagnostic, {}, "invalid resident source binding");
				for (size_t earlier = 0; earlier < index; ++earlier)
					if (sources[earlier].Path == source.Path &&
						sources[earlier].Interpretation == source.Interpretation)
						return Refuse(diagnostic, {}, "duplicate resident source binding");
			}
			candidate.Sources.assign(sources.begin(), sources.end());
			candidate.Nodes.resize(candidate.Document.Nodes.size());
			candidate.Publications.reserve(imagegraph::Limits::MaximumOutputs);
			candidate.GeometrySignature = GeometrySignature(candidate.Document);
			candidate.Executions.resize(candidate.Document.Outputs.size());
			candidate.ExecutionSignatures.resize(candidate.Document.Outputs.size());
			for (size_t index = 0; index < candidate.Nodes.size(); ++index) {
				const auto &node = candidate.Document.Nodes[index];
				candidate.Nodes[index].AuthoredSignature = Signature(node);
				if (const auto *source = std::get_if<imagegraph::Source>(&node.Value))
					if (!SourceBinding(candidate, *source))
						return Refuse(diagnostic, node.Id, "source has no exact resident binding");
			}
			const uint64_t key = Key(owner, name);
			auto found = State->ImageGraphs.Documents.find(key);
			if (found == State->ImageGraphs.Documents.end() &&
				State->ImageGraphs.Documents.size() >= ImageGraphGpuState::MAXIMUM_GRAPHS)
				return Refuse(diagnostic, {}, "resident graph count exceeded");
			if (!State->EnsureImageGraphPipelines())
				return Refuse(diagnostic, {}, "GPU image composition unavailable");
			const bool wideAvailable = State->ImageGraphs.Wide != nullptr;
			if (candidate.Document.Storage == imagegraph::Precision::Wide && !wideAvailable)
				return Refuse(diagnostic, {}, "wide precision needs rgba16 storage images");
			for (const auto &node : candidate.Document.Nodes) {
				if (std::holds_alternative<imagegraph::MeshAttribute>(node.Value))
					return Refuse(
						diagnostic, node.Id, "mesh attributes must be lowered to data sources by the host"
					);
				if (std::holds_alternative<imagegraph::Blur>(node.Value) && !wideAvailable)
					return Refuse(diagnostic, node.Id, "blur needs rgba16 storage images");
			}
			if (found == State->ImageGraphs.Documents.end())
				State->ImageGraphs.Documents.emplace(key, std::move(candidate));
			else {
				auto &previous = found->second;
				if (candidate.GeometrySignature == previous.GeometrySignature) {
					for (size_t outputIndex = 0; outputIndex < candidate.Document.Outputs.size();
						 ++outputIndex) {
						const auto &output = candidate.Document.Outputs[outputIndex];
						const auto oldOutput = std::find_if(
							previous.Document.Outputs.begin(),
							previous.Document.Outputs.end(),
							[&](const auto &binding) {
								return binding.Name == output.Name && binding.Node == output.Node;
							}
						);
						if (oldOutput == previous.Document.Outputs.end()) continue;
						const size_t oldIndex =
							static_cast<size_t>(oldOutput - previous.Document.Outputs.begin());
						candidate.Executions[outputIndex] = std::move(previous.Executions[oldIndex]);
						candidate.Executions[outputIndex].Output = outputIndex;
						candidate.ExecutionSignatures[outputIndex] = previous.ExecutionSignatures[oldIndex];
					}
				}
				for (size_t index = 0; index < candidate.Nodes.size(); ++index) {
					const auto old = std::find_if(
						previous.Document.Nodes.begin(),
						previous.Document.Nodes.end(),
						[&](const auto &node) { return node.Id == candidate.Document.Nodes[index].Id; }
					);
					if (old == previous.Document.Nodes.end()) continue;
					auto &resident =
						previous.Nodes[static_cast<size_t>(old - previous.Document.Nodes.begin())];
					candidate.Nodes[index].Texture = std::exchange(resident.Texture, nullptr);
					candidate.Nodes[index].Scratch = std::exchange(resident.Scratch, nullptr);
					candidate.Nodes[index].Extent = resident.Extent;
					candidate.Nodes[index].AcceptedSignature = resident.AcceptedSignature;
				}
				candidate.Publications = std::move(previous.Publications);
				State->ImageGraphs.ReleaseDocument(State->Device, State->Textures, previous);
				previous = std::move(candidate);
			}
			diagnostic = {};
			return true;
		} catch (const std::bad_alloc &) {
			return Refuse(diagnostic, {}, "imagegraph installation allocation refused");
		}
	}

	// Starts, continues or finishes the one resident-free evaluation. Every image is
	// allocated from the job's pool as its node begins and returned after its last reader,
	// so the live set stays near Execution.LiveBytes whatever the node count.
	ImageGraphEvaluation Renderer::Impl::AdvanceImageGraphJob(
		const ImageGraphJobRequest &request,
		const imagegraph::ExecutionPlan &execution,
		std::span<SDL_GPUTexture *const> sourceTextures,
		std::span<const assets::TextureFormat> sourceFormats,
		const imagegraph::ImageExtent &published,
		imagegraph::Diagnostic &diagnostic
	) {
		using namespace imagegraph_gpu_detail;
		ENGINE_PROFILE_CAT("imagegraph job slice", core::ProfileCategory::Render);
		auto &state = ImageGraphs;
		auto &graph = request.Graph;
		auto &job = state.Job;
		const auto now = std::chrono::steady_clock::now();
		const bool wide = graph.Document.Storage == imagegraph::Precision::Wide;
		const auto fail = [&](std::string_view node, std::string_view message) {
			state.AbandonJob(Device);
			state.JobRetryAt = now + ImageGraphGpuState::JOB_IDLE;
			Refuse(diagnostic, node, message);
			return ImageGraphEvaluation::Refused;
		};
		const bool mine = job.Graph == &graph && job.Published == request.Published;
		if (mine) job.Touched = now;
		if (job.Graph && !mine) {
			if (now - job.Touched < ImageGraphGpuState::JOB_IDLE) return ImageGraphEvaluation::Pending;
			state.AbandonJob(Device);
		}
		if (state.JobFence) {
			if (!SDL_QueryGPUFence(Device, state.JobFence)) return ImageGraphEvaluation::Pending;
			SDL_ReleaseGPUFence(Device, state.JobFence);
			state.JobFence = nullptr;
		}
		if (mine && job.Signature != request.Signature) state.AbandonJob(Device);
		if (!job.Graph) {
			if (now < state.JobRetryAt) {
				Refuse(diagnostic, {}, "GPU imagegraph allocation failed; waiting to retry");
				return ImageGraphEvaluation::Refused;
			}
			// Nothing kept from a smaller version of this graph is read again.
			for (auto &node : graph.Nodes) {
				Release(Device, node.Texture, node.Extent.Bytes, state);
				Release(Device, node.Scratch, ScratchBytes(node.Extent), state);
				node.Texture = node.Scratch = nullptr;
				node.AcceptedSignature = 0;
			}
			// A full texture table is reported before the work rather than after it.
			ImageGraphGpuState::Reservation room;
			if (!state.Reserve(Textures, graph.Owner, request.Published, published.Bytes, room))
				return fail({}, "texture publication budget exceeded");
			state.Cancel(Textures, room);
			job.Graph = &graph;
			job.Published = request.Published;
			job.Signature = request.Signature;
			job.Execution = execution;
			job.Images.assign(graph.Nodes.size(), -1);
			job.Readers.assign(graph.Nodes.size(), 0);
			for (const size_t index : execution.Order)
				for (const size_t input : graph.Plan.Inputs[index])
					++job.Readers[input];
			job.Touched = now;
		}
		const size_t target = job.Execution.Target;
		const auto take = [&](const imagegraph::ImageExtent &extent, SDL_GPUTextureFormat format) -> int32_t {
			for (size_t index = 0; index < job.Pool.size(); ++index) {
				auto &image = job.Pool[index];
				if (!image.Free || image.Extent != extent || image.Format != format) continue;
				image.Free = false;
				return static_cast<int32_t>(index);
			}
			// Free images of another shape are dropped before the job grows.
			for (size_t index = 0; index < job.Pool.size(); ++index) {
				auto &image = job.Pool[index];
				if (!image.Free || !image.Texture) continue;
				Release(Device, image.Texture, image.Extent.Bytes, state);
				image.Texture = nullptr;
				image.Extent = {};
				image.Format = SDL_GPU_TEXTUREFORMAT_INVALID;
			}
			SDL_GPUTexture *texture = Allocate(
				Device,
				extent,
				format,
				SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE,
				state
			);
			if (!texture) return -1;
			job.Pool.push_back({texture, extent, format, false});
			return static_cast<int32_t>(job.Pool.size() - 1);
		};
		SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(Device);
		if (!command) return fail({}, "GPU command acquisition failed");
		const auto cancel = [&](std::string_view node, std::string_view message) {
			SDL_CancelGPUCommandBuffer(command);
			return fail(node, message);
		};
		uint64_t budget = ImageGraphGpuState::JOB_SLICE_WORK, dispatches = 0;
		const auto format =
			wide ? SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UNORM : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
		bool waiting = false;
		while (budget > 0 && job.Position < job.Execution.Order.size()) {
			const size_t index = job.Execution.Order[job.Position];
			const auto &node = graph.Document.Nodes[index];
			const auto &extent = job.Execution.Extents[index];
			const auto &inputs = graph.Plan.Inputs[index];
			if (const auto *source = std::get_if<imagegraph::Source>(&node.Value);
				source && !sourceTextures[index]) {
				graph.NeededSource = SourceBinding(graph, *source)->Texture;
				waiting = true;
				break;
			}
			const bool blur = std::holds_alternative<imagegraph::Blur>(node.Value);
			if (job.Images[index] < 0 && (job.Images[index] = take(extent, format)) < 0)
				return cancel(node.Id, "GPU intermediate allocation failed");
			if (blur && job.Scratch < 0) {
				const imagegraph::ImageExtent scratch{extent.Width, extent.Height, ScratchBytes(extent)};
				job.Scratch = take(scratch, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UNORM);
				if (job.Scratch < 0) return cancel(node.Id, "GPU blur scratch allocation failed");
			}
			ImageGraphGpuUniforms uniforms;
			const auto input = inputs.empty() ? extent : job.Execution.Extents[inputs[0]];
			if (!Uniforms(node, extent, input, sourceFormats[index], uniforms, diagnostic)) {
				const imagegraph::Diagnostic refused = diagnostic;
				return cancel(refused.Node, refused.Message);
			}
			std::array<SDL_GPUTexture *, imagegraph::Limits::MaximumInputs> bound{};
			bound.fill(FallbackTexture);
			if (sourceTextures[index]) bound[0] = sourceTextures[index];
			for (size_t slot = 0; slot < inputs.size(); ++slot)
				bound[slot] = job.Pool[size_t(job.Images[inputs[slot]])].Texture;
			SDL_GPUTexture *output = job.Pool[size_t(job.Images[index])].Texture;
			SDL_GPUComputePipeline *pipeline = wide || (blur && job.Pass == 0) ? state.Wide : state.Compute;
			if (blur) {
				uniforms.Control[3] = job.Pass;
				(job.Pass == 0 ? output : bound[0]) = job.Pool[size_t(job.Scratch)].Texture;
			}
			const uint64_t perRow = std::max<uint64_t>(1, job.Execution.Work[index] / extent.Height);
			const uint64_t fits = std::max<uint64_t>(8, budget / perRow / 8 * 8);
			const auto rows = static_cast<uint32_t>(std::min<uint64_t>(fits, extent.Height - job.Row));
			if (!RecordImageGraphNode(
					command, pipeline, Textures.PixelSampler(), bound, output, uniforms, job.Row, rows
				))
				return cancel(node.Id, "GPU compute recording failed");
			++dispatches;
			budget -= std::min(budget, perRow * rows);
			job.Row += rows;
			if (job.Row < extent.Height) continue;
			job.Row = 0;
			if (blur && job.Pass == 0) {
				job.Pass = 1;
				continue;
			}
			job.Pass = 0;
			if (blur) {
				job.Pool[size_t(job.Scratch)].Free = true;
				job.Scratch = -1;
			}
			for (const size_t read : inputs)
				if (--job.Readers[read] == 0) {
					job.Pool[size_t(job.Images[read])].Free = true;
					job.Images[read] = -1;
				}
			++job.Position;
		}
		job.Dispatches += dispatches;
		const bool finished = job.Position == job.Execution.Order.size();
		SDL_GPUTexture *publicationTarget = nullptr;
		const auto &binding = graph.Document.Outputs[request.Output];
		if (finished) {
			const bool copy = request.PublishSlot == 1 && !wide;
			publicationTarget = Allocate(
				Device,
				published,
				PUBLICATION_FORMATS[request.PublishSlot],
				SDL_GPU_TEXTUREUSAGE_SAMPLER | (copy ? 0u : SDL_GPU_TEXTUREUSAGE_COLOR_TARGET),
				state
			);
			if (!publicationTarget) return cancel({}, "GPU output allocation failed");
			SDL_GPUTexture *image = job.Pool[size_t(job.Images[target])].Texture;
			bool recorded = true;
			if (copy) {
				auto *pass = SDL_BeginGPUCopyPass(command);
				recorded = pass != nullptr;
				if (pass) {
					SDL_GPUTextureLocation source{}, destination{};
					source.texture = image;
					destination.texture = publicationTarget;
					SDL_CopyGPUTextureToTexture(
						pass, &source, &destination, published.Width, published.Height, 1, false
					);
					SDL_EndGPUCopyPass(pass);
				}
			} else
				recorded = RecordImageGraphPublication(
					command,
					state.Publish[request.PublishSlot],
					Textures.PixelSampler(),
					image,
					publicationTarget,
					request.PublishSlot == 0
				);
			if (!recorded) {
				Release(Device, publicationTarget, published.Bytes, state);
				return cancel({}, "GPU publication recording failed");
			}
		}
		if (dispatches == 0 && !finished) {
			SDL_CancelGPUCommandBuffer(command);
			return ImageGraphEvaluation::Pending;
		}
		ImageGraphGpuState::Reservation reservation;
		if (finished &&
			!state.Reserve(Textures, graph.Owner, request.Published, published.Bytes, reservation)) {
			Release(Device, publicationTarget, published.Bytes, state);
			return cancel({}, "texture publication budget exceeded");
		}
		state.JobFence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
		if (!state.JobFence) {
			if (finished) {
				state.Cancel(Textures, reservation);
				Release(Device, publicationTarget, published.Bytes, state);
			}
			return fail({}, "GPU submission failed");
		}
		++state.Profile.CommandBuffers;
		state.Profile.ComputeDispatches += dispatches;
		core::Metrics::Count("render.imagegraph.dispatches", dispatches);
		core::Metrics::Count("render.imagegraph.command_buffers", 1);
		diagnostic = {};
		if (!finished) return ImageGraphEvaluation::Pending;
		// SDL keeps a released texture alive until submitted readers complete.
		auto publication =
			std::find_if(graph.Publications.begin(), graph.Publications.end(), [&](const auto &entry) {
				return entry.Name == request.Published;
			});
		const size_t retiredBytes = publication == graph.Publications.end() ? 0 : publication->Extent.Bytes;
		Release(
			Device,
			state.Commit(Textures, reservation, publicationTarget, published, binding.Space, binding.Format),
			retiredBytes,
			state
		);
		const ImageGraphGpuPublication entry{
			request.Published,
			publicationTarget,
			published,
			binding.Space,
			binding.Format,
			request.PublicationSignature
		};
		if (publication == graph.Publications.end())
			graph.Publications.push_back(entry);
		else
			*publication = entry;
		state.AbandonJob(Device);
		++ResourceEpoch;
		++state.Profile.Evaluations;
		state.Profile.CopiedBytes += published.Bytes;
		core::Metrics::Count("render.imagegraph.published_bytes", published.Bytes);
		return ImageGraphEvaluation::Updated;
	}

	core::Name Renderer::ImageGraphNeededSource(core::Name owner, core::Name name) const {
		if (!State) return {};
		const auto found = State->ImageGraphs.Documents.find(imagegraph_gpu_detail::Key(owner, name));
		return found == State->ImageGraphs.Documents.end() ? core::Name{} : found->second.NeededSource;
	}

	ImageGraphEvaluation Renderer::EvaluateImageGraph(
		core::Name owner,
		core::Name name,
		std::string_view output,
		core::Name publishedTexture,
		imagegraph::Diagnostic &diagnostic
	) {
		RequireOwningThread("EvaluateImageGraph");
		ENGINE_PROFILE_CAT("imagegraph evaluation", core::ProfileCategory::Render);
		using namespace imagegraph_gpu_detail;
		const auto refuse = [&](std::string_view node, std::string_view message) {
			Refuse(diagnostic, node, message);
			return ImageGraphEvaluation::Refused;
		};
		if (!State || !State->Device || !publishedTexture.IsValid())
			return refuse({}, "renderer or output name unavailable");
		auto &state = State->ImageGraphs;
		state.Collect(State->Device);
		const auto found = state.Documents.find(Key(owner, name));
		if (found == state.Documents.end()) return refuse({}, "imagegraph is not installed for this owner");
		auto &graph = found->second;
		size_t outputIndex = 0;
		if (output.empty()) {
			if (graph.Document.Outputs.size() != 1) return refuse({}, "select one named output explicitly");
		} else {
			const auto selected = std::find_if(
				graph.Document.Outputs.begin(), graph.Document.Outputs.end(), [&](const auto &binding) {
					return binding.Name == output;
				}
			);
			if (selected == graph.Document.Outputs.end()) return refuse({}, "selected output does not exist");
			outputIndex = static_cast<size_t>(selected - graph.Document.Outputs.begin());
		}
		graph.NeededSource = {};
		const size_t targetIndex = graph.Plan.Outputs[outputIndex];
		const auto space = graph.Document.Outputs[outputIndex].Space;
		const auto format = graph.Document.Outputs[outputIndex].Format;
		const bool wide = graph.Document.Storage == imagegraph::Precision::Wide;
		const size_t publishSlot = PublicationSlot(format, space);
		std::array<uint8_t, imagegraph::Limits::MaximumNodes> needed{};
		std::array<uint64_t, imagegraph::Limits::MaximumNodes> signatures{};
		std::array<SDL_GPUTexture *, imagegraph::Limits::MaximumNodes> sourceTextures{};
		std::array<assets::TextureFormat, imagegraph::Limits::MaximumNodes> sourceFormats{};
		needed[targetIndex] = 1;
		for (auto node = graph.Plan.Order.rbegin(); node != graph.Plan.Order.rend(); ++node)
			if (needed[*node])
				for (size_t input : graph.Plan.Inputs[*node])
					needed[input] = 1;
		for (size_t index : graph.Plan.Order) {
			if (!needed[index]) continue;
			uint64_t signature = graph.Nodes[index].AuthoredSignature;
			for (size_t input : graph.Plan.Inputs[index])
				signature = Fold(signature, signatures[input]);
			if (const auto *source = std::get_if<imagegraph::Source>(&graph.Document.Nodes[index].Value)) {
				const auto *binding = SourceBinding(graph, *source);
				sourceTextures[index] = State->Textures.Find(binding->Texture, binding->Owner);
				if (binding->ContentAddressed)
					sourceFormats[index] = binding->Format;
				else if (!sourceTextures[index] ||
						 !State->Textures.FormatOf(binding->Texture, sourceFormats[index], binding->Owner))
					return refuse(graph.Document.Nodes[index].Id, "resident source is unavailable");
				if (binding->Owner == owner && binding->Texture == publishedTexture)
					return refuse(
						graph.Document.Nodes[index].Id, "an output cannot be its own resident source"
					);
				signature = Fold(signature, Key(binding->Owner, binding->Texture));
				if (!binding->ContentAddressed)
					signature = Fold(signature, State->Textures.RevisionOf(binding->Texture, binding->Owner));
			}
			signatures[index] = signature;
		}
		auto publication =
			std::find_if(graph.Publications.begin(), graph.Publications.end(), [&](const auto &binding) {
				return binding.Name == publishedTexture;
			});
		const uint64_t publicationSignature =
			Fold(Fold(signatures[targetIndex], static_cast<uint64_t>(space)), static_cast<uint64_t>(format));
		if (publication != graph.Publications.end() &&
			State->Textures.Find(publishedTexture, owner) != publication->Texture)
			return refuse({}, "published output was replaced by another content owner");
		// The target's signature covers its whole cone, resident or not.
		if (publication != graph.Publications.end() && publication->Signature == publicationSignature) {
			++state.Profile.CacheHits;
			core::Metrics::Count("render.imagegraph.cache_hits", 1);
			diagnostic = {};
			return ImageGraphEvaluation::Reused;
		}
		if (publication == graph.Publications.end() &&
			graph.Publications.size() >= imagegraph::Limits::MaximumOutputs)
			return refuse({}, "published output count exceeded");
		if (publication == graph.Publications.end() && State->Textures.Find(publishedTexture, owner))
			return refuse({}, "output name already contains unrelated content");
		// Graph-to-graph feedback can bypass document cycle validation. Publications are
		// terminal images; importing any live graph output as a source is refused.
		for (const auto &[otherKey, other] : state.Documents) {
			(void)otherKey;
			for (const auto &published : other.Publications)
				for (size_t index : graph.Plan.Order)
					if (sourceTextures[index] && sourceTextures[index] == published.Texture)
						return refuse(
							graph.Document.Nodes[index].Id, "live graph outputs cannot be source feedback"
						);
		}
		auto pending = std::find_if(
			state.PendingSubmissions.begin(), state.PendingSubmissions.end(), [](const auto &slot) {
				return slot.Fence == nullptr;
			}
		);
		if (pending == state.PendingSubmissions.end()) return ImageGraphEvaluation::Pending;
		const auto started = std::chrono::steady_clock::now();
		std::array<SDL_GPUTexture *, imagegraph::Limits::MaximumNodes> textures{};
		std::array<ImageGraphGpuUniforms, imagegraph::Limits::MaximumNodes> uniforms{};
		std::array<uint8_t, imagegraph::Limits::MaximumNodes> allocated{};
		std::array<SDL_GPUTexture *, imagegraph::Limits::MaximumNodes> scratches{};
		std::array<uint8_t, imagegraph::Limits::MaximumNodes> allocatedScratch{};
		imagegraph::ExecutionPlan execution;
		SDL_GPUTexture *publicationTarget = nullptr;
		bool allocatedPublication = false;
		ImageGraphGpuState::Reservation reservation;
		bool reserved = false;
		bool committed = false;
		bool borrowedExecution = false;
		uint64_t executionSignature = Fold(graph.GeometrySignature, targetIndex);
		imagegraph::ImageExtent published;
		SDL_GPUCommandBuffer *command = nullptr;
		uint32_t timingSlot = VulkanTimestamps::NO_SLOT;
		const auto cleanup = [&] {
			if (committed) return;
			if (command) {
				SDL_CancelGPUCommandBuffer(command);
				command = nullptr;
			}
			state.Timestamps.Abandon(timingSlot);
			if (reserved) {
				state.Cancel(State->Textures, reservation);
				reserved = false;
			}
			for (size_t index = 0; index < graph.Nodes.size(); ++index) {
				if (allocated[index]) {
					Release(State->Device, textures[index], execution.Extents[index].Bytes, state);
					allocated[index] = 0;
				}
				if (allocatedScratch[index]) {
					Release(State->Device, scratches[index], ScratchBytes(execution.Extents[index]), state);
					allocatedScratch[index] = 0;
				}
			}
			if (allocatedPublication) {
				Release(State->Device, publicationTarget, published.Bytes, state);
				allocatedPublication = false;
			}
			if (borrowedExecution) {
				graph.Executions[outputIndex] = std::move(execution);
				borrowedExecution = false;
			}
		};
		try {
			std::array<std::array<uint32_t, 2>, imagegraph::Limits::MaximumNodes> sourceShapes{};
			for (size_t index : graph.Plan.Order) {
				const auto *source = std::get_if<imagegraph::Source>(&graph.Document.Nodes[index].Value);
				if (!needed[index] || !source) continue;
				const auto *binding = SourceBinding(graph, *source);
				uint32_t width = binding->Width, height = binding->Height;
				if (!binding->ContentAddressed)
					State->Textures.SizeOf(binding->Texture, width, height, binding->Owner);
				sourceShapes[index] = {width, height};
				executionSignature = Fold(Fold(Fold(executionSignature, index), width), height);
			}
			if (graph.ExecutionSignatures[outputIndex] == executionSignature &&
				!graph.Executions[outputIndex].Order.empty()) {
				execution = std::move(graph.Executions[outputIndex]);
				borrowedExecution = true;
			} else {
				std::vector<imagegraph::SourceExtent> sources;
				sources.reserve(graph.Sources.size());
				for (size_t index : graph.Plan.Order)
					if (needed[index] && sourceShapes[index][0] != 0)
						sources.push_back(
							{graph.Document.Nodes[index].Id, sourceShapes[index][0], sourceShapes[index][1]}
						);
				if (!imagegraph::Prepare(graph.Document, graph.Plan, output, sources, execution, diagnostic))
					return ImageGraphEvaluation::Refused;
			}
			const auto &outputExtent = execution.Extents[targetIndex];
			published = {
				outputExtent.Width,
				outputExtent.Height,
				size_t(outputExtent.Width) * outputExtent.Height * imagegraph::BytesPerPixel(format)
			};
			if (!State->EnsureImageGraphPublication(publishSlot))
				return refuse({}, "GPU output format is unavailable");
			size_t newBytes = 0;
			for (size_t index : execution.Order) {
				const auto &extent = execution.Extents[index];
				const auto &resident = graph.Nodes[index];
				const bool blur = std::holds_alternative<imagegraph::Blur>(graph.Document.Nodes[index].Value);
				if (!resident.Texture || resident.Extent != extent) newBytes += extent.Bytes;
				if (blur && (!resident.Scratch || resident.Extent != extent))
					newBytes += ScratchBytes(extent);
				const auto input = graph.Plan.Inputs[index].empty()
									   ? extent
									   : execution.Extents[graph.Plan.Inputs[index][0]];
				if (!Uniforms(
						graph.Document.Nodes[index],
						extent,
						input,
						sourceFormats[index],
						uniforms[index],
						diagnostic
					)) {
					cleanup();
					return ImageGraphEvaluation::Refused;
				}
			}
			allocatedPublication = publication == graph.Publications.end() ||
								   publication->Extent != published || publication->Space != space ||
								   publication->Format != format;
			if (allocatedPublication) newBytes += published.Bytes;
			if (execution.PixelWork > ImageGraphGpuState::RESIDENT_WORK ||
				state.Profile.ResidentBytes > imagegraph::Limits::MaximumRetainedBytes ||
				newBytes > imagegraph::Limits::MaximumRetainedBytes - state.Profile.ResidentBytes) {
				allocatedPublication = false;
				const uint64_t signature = Fold(publicationSignature, executionSignature);
				const auto result = State->AdvanceImageGraphJob(
					{graph, publishedTexture, signature, publicationSignature, outputIndex, publishSlot},
					execution,
					sourceTextures,
					sourceFormats,
					published,
					diagnostic
				);
				graph.Executions[outputIndex] = std::move(execution);
				graph.ExecutionSignatures[outputIndex] = executionSignature;
				borrowedExecution = false;
				return result;
			}
			for (size_t index : execution.Order) {
				const auto *source = std::get_if<imagegraph::Source>(&graph.Document.Nodes[index].Value);
				if (!source || sourceTextures[index] ||
					(graph.Nodes[index].Texture && graph.Nodes[index].AcceptedSignature == signatures[index]))
					continue;
				allocatedPublication = false;
				cleanup();
				graph.NeededSource = SourceBinding(graph, *source)->Texture;
				return ImageGraphEvaluation::Pending;
			}
			if (!state.Reserve(State->Textures, owner, publishedTexture, published.Bytes, reservation)) {
				cleanup();
				return refuse({}, "texture publication budget exceeded");
			}
			reserved = true;
			const auto storage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
			for (size_t index : execution.Order) {
				const auto &resident = graph.Nodes[index];
				const auto &extent = execution.Extents[index];
				textures[index] = resident.Texture;
				scratches[index] = resident.Scratch;
				if (!resident.Texture || resident.Extent != extent) {
					textures[index] = Allocate(
						State->Device,
						extent,
						wide ? SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UNORM
							 : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
						storage,
						state
					);
					allocated[index] = textures[index] != nullptr;
					if (!textures[index]) {
						cleanup();
						return refuse(graph.Document.Nodes[index].Id, "GPU intermediate allocation failed");
					}
				}
				if (std::holds_alternative<imagegraph::Blur>(graph.Document.Nodes[index].Value) &&
					(!resident.Scratch || resident.Extent != extent)) {
					const imagegraph::ImageExtent scratch{extent.Width, extent.Height, ScratchBytes(extent)};
					scratches[index] = Allocate(
						State->Device, scratch, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UNORM, storage, state
					);
					allocatedScratch[index] = scratches[index] != nullptr;
					if (!scratches[index]) {
						cleanup();
						return refuse(graph.Document.Nodes[index].Id, "GPU blur scratch allocation failed");
					}
				}
			}
			// Byte-precision linear RGBA8 is a plain copy; every other target is drawn.
			const bool copyPublication = publishSlot == 1 && !wide;
			if (allocatedPublication) {
				publicationTarget = Allocate(
					State->Device,
					published,
					PUBLICATION_FORMATS[publishSlot],
					SDL_GPU_TEXTUREUSAGE_SAMPLER | (copyPublication ? 0u : SDL_GPU_TEXTUREUSAGE_COLOR_TARGET),
					state
				);
				if (!publicationTarget) {
					cleanup();
					return refuse({}, "GPU output allocation failed");
				}
			} else
				publicationTarget = publication->Texture;
			command = SDL_AcquireGPUCommandBuffer(State->Device);
			if (!command) {
				cleanup();
				return refuse({}, "GPU command acquisition failed");
			}
			timingSlot = state.Timestamps.Begin(command);
			state.Timestamps.Mark(command);
			uint64_t dispatches = 0;
			for (size_t index : execution.Order) {
				if (!allocated[index] && graph.Nodes[index].AcceptedSignature == signatures[index]) continue;
				std::array<SDL_GPUTexture *, imagegraph::Limits::MaximumInputs> inputs{};
				inputs.fill(State->FallbackTexture);
				if (sourceTextures[index]) inputs[0] = sourceTextures[index];
				for (size_t slot = 0; slot < graph.Plan.Inputs[index].size(); ++slot)
					inputs[slot] = textures[graph.Plan.Inputs[index][slot]];
				SDL_GPUComputePipeline *pipeline = wide ? state.Wide : state.Compute;
				bool recorded = true;
				if (std::holds_alternative<imagegraph::Blur>(graph.Document.Nodes[index].Value)) {
					// Rows into wide scratch, then columns into the node's own storage.
					ImageGraphGpuUniforms rows = uniforms[index];
					rows.Control[3] = 0;
					recorded = RecordImageGraphNode(
						command, state.Wide, State->Textures.PixelSampler(), inputs, scratches[index], rows
					);
					ImageGraphGpuUniforms columns = uniforms[index];
					columns.Control[3] = 1;
					inputs[0] = scratches[index];
					recorded = recorded && RecordImageGraphNode(
											   command,
											   pipeline,
											   State->Textures.PixelSampler(),
											   inputs,
											   textures[index],
											   columns
										   );
					++dispatches;
				} else
					recorded = RecordImageGraphNode(
						command,
						pipeline,
						State->Textures.PixelSampler(),
						inputs,
						textures[index],
						uniforms[index]
					);
				if (!recorded) {
					cleanup();
					return refuse(graph.Document.Nodes[index].Id, "GPU compute recording failed");
				}
				++dispatches;
			}
			if (!copyPublication) {
				if (!RecordImageGraphPublication(
						command,
						state.Publish[publishSlot],
						State->Textures.PixelSampler(),
						textures[targetIndex],
						publicationTarget,
						publishSlot == 0
					)) {
					cleanup();
					return refuse({}, "GPU publication recording failed");
				}
			} else {
				auto *copy = SDL_BeginGPUCopyPass(command);
				if (!copy) {
					cleanup();
					return refuse({}, "GPU publication copy recording failed");
				}
				SDL_GPUTextureLocation source{}, destination{};
				source.texture = textures[targetIndex];
				destination.texture = publicationTarget;
				SDL_CopyGPUTextureToTexture(
					copy, &source, &destination, outputExtent.Width, outputExtent.Height, 1, false
				);
				SDL_EndGPUCopyPass(copy);
			}
			state.Timestamps.Mark(command);
			SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
			command = nullptr; // SDL consumes the command buffer even when submission fails.
			if (!fence) {
				cleanup();
				return refuse({}, "GPU submission failed");
			}
			pending->Fence = fence;
			state.Timestamps.Submitted(timingSlot);
			if (timingSlot < VulkanTimestamps::SLOTS) state.TimingSequences[timingSlot] = ++state.Sequence;
			const auto retire = [&](SDL_GPUTexture *texture, size_t bytes) {
				if (!texture) return;
				pending->Retired[pending->Count] = texture;
				pending->Bytes[pending->Count++] = bytes;
			};
			for (size_t index : execution.Order) {
				auto &resident = graph.Nodes[index];
				if (allocated[index]) retire(resident.Texture, resident.Extent.Bytes);
				if (allocatedScratch[index]) retire(resident.Scratch, ScratchBytes(resident.Extent));
				resident.Scratch = scratches[index];
				resident.Texture = textures[index];
				resident.Extent = execution.Extents[index];
				resident.AcceptedSignature = signatures[index];
			}
			if (allocatedPublication) {
				const size_t retiredBytes =
					publication == graph.Publications.end() ? 0 : publication->Extent.Bytes;
				retire(
					state.Commit(State->Textures, reservation, publicationTarget, published, space, format),
					retiredBytes
				);
			} else
				state.Touch(State->Textures, owner, publishedTexture);
			if (publication == graph.Publications.end()) {
				graph.Publications.push_back(
					{publishedTexture, publicationTarget, published, space, format, publicationSignature}
				);
			} else
				*publication = {
					publishedTexture, publicationTarget, published, space, format, publicationSignature
				};
			++State->ResourceEpoch;
			++state.Profile.Evaluations;
			++state.Profile.CommandBuffers;
			state.Profile.ComputeDispatches += dispatches;
			state.Profile.CopiedBytes += published.Bytes;
			state.Profile.CpuRecordingMicroseconds =
				std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count();
			graph.Executions[outputIndex] = std::move(execution);
			graph.ExecutionSignatures[outputIndex] = executionSignature;
			committed = true;
			core::Metrics::Count("render.imagegraph.dispatches", dispatches);
			core::Metrics::Count("render.imagegraph.command_buffers", 1);
			core::Metrics::Count("render.imagegraph.published_bytes", published.Bytes);
			diagnostic = {};
			return ImageGraphEvaluation::Updated;
		} catch (const std::bad_alloc &) {
			// Instrumentation cannot roll back pixels already submitted and committed.
			if (committed) {
				diagnostic = {};
				return ImageGraphEvaluation::Updated;
			}
			cleanup();
			return refuse({}, "imagegraph evaluation allocation refused");
		}
	}

	bool Renderer::DropImageGraph(core::Name owner, core::Name name) {
		RequireOwningThread("DropImageGraph");
		if (!State || !State->Device) return false;
		const auto found = State->ImageGraphs.Documents.find(imagegraph_gpu_detail::Key(owner, name));
		if (found == State->ImageGraphs.Documents.end()) return false;
		State->ImageGraphs.Collect(State->Device);
		State->ImageGraphs.ReleaseDocument(State->Device, State->Textures, found->second);
		State->ImageGraphs.Documents.erase(found);
		++State->ResourceEpoch;
		return true;
	}
	ImageGraphStatistics Renderer::ImageGraphProfile() {
		RequireOwningThread("ImageGraphProfile");
		if (!State || !State->Device) return {};
		State->ImageGraphs.Collect(State->Device);
		return State->ImageGraphs.Profile;
	}
}
