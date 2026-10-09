#include "ImageGraphGpu.hpp"
#include "RendererState.hpp"

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
			uint64_t signature = Text(14695981039346656037ull, imagegraph::Kind(node.Value));
			for (const std::string &input : node.Inputs)
				signature = Text(signature, input);
			return std::visit(
				[&](const auto &value) {
					using T = std::decay_t<decltype(value)>;
					if constexpr (std::is_same_v<T, imagegraph::Source>) {
						signature = Text(signature, value.Path);
						signature = Fold(signature, static_cast<uint64_t>(value.Interpretation));
					}
					if constexpr (std::is_same_v<T, imagegraph::Solid> ||
								  std::is_same_v<T, imagegraph::Resize> ||
								  std::is_same_v<T, imagegraph::Crop> ||
								  std::is_same_v<T, imagegraph::Transform>) {
						signature = Fold(Fold(signature, value.Width), value.Height);
					}
					if constexpr (std::is_same_v<T, imagegraph::Solid>)
						for (uint8_t channel : value.Colour)
							signature = Fold(signature, channel);
					if constexpr (std::is_same_v<T, imagegraph::Resize> ||
								  std::is_same_v<T, imagegraph::Transform>)
						signature = Fold(signature, static_cast<uint64_t>(value.Filter));
					if constexpr (std::is_same_v<T, imagegraph::Crop>)
						signature = Fold(
							Fold(signature, static_cast<uint32_t>(value.X)), static_cast<uint32_t>(value.Y)
						);
					if constexpr (std::is_same_v<T, imagegraph::Transform>)
						for (double scalar :
							 {value.TranslateX,
							  value.TranslateY,
							  value.ScaleX,
							  value.ScaleY,
							  value.Degrees,
							  value.PivotX,
							  value.PivotY})
							signature = Fold(signature, std::bit_cast<uint64_t>(scalar));
					if constexpr (std::is_same_v<T, imagegraph::Flip>)
						signature = Fold(Fold(signature, value.Horizontal), value.Vertical);
					if constexpr (std::is_same_v<T, imagegraph::Blend>)
						signature = Fold(signature, std::bit_cast<uint64_t>(value.Opacity));
					return signature;
				},
				node.Value
			);
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
			uint64_t signature = 14695981039346656037ull;
			for (const auto &node : document.Nodes) {
				signature = Text(Text(signature, node.Id), imagegraph::Kind(node.Value));
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
					if constexpr (std::is_same_v<T, imagegraph::Blend>)
						out.Parameters[2] = static_cast<float>(value.Opacity);
					if constexpr (std::is_same_v<T, imagegraph::Resize> ||
								  std::is_same_v<T, imagegraph::Transform>)
						out.Control[1] = value.Filter == imagegraph::Sampling::Bilinear;
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
		imagegraph::OutputSpace space
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
		entry.Format = space == imagegraph::OutputSpace::SRGB ? assets::TextureFormat::RGBA8
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
	void ImageGraphGpuState::ReleaseDocument(
		SDL_GPUDevice *device, TextureTable &table, ImageGraphGpuDocument &graph
	) {
		// SDL defers native destruction until submitted readers complete. Dropping
		// the public handle now prevents later submissions from reusing it.
		for (auto &node : graph.Nodes)
			imagegraph_gpu_detail::Release(device, node.Texture, node.Extent.Bytes, *this);
		for (const auto &publication : graph.Publications) {
			if (table.Find(publication.Name, graph.Owner) != publication.Texture) continue;
			if (table.Drop(publication.Name, graph.Owner))
				Profile.ResidentBytes -= std::min<uint64_t>(Profile.ResidentBytes, publication.Extent.Bytes);
		}
		graph = {};
	}
	void ImageGraphGpuState::Shutdown(SDL_GPUDevice *device, TextureTable &table) {
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
		if (Publish) SDL_ReleaseGPUGraphicsPipeline(device, Publish);
		Compute = nullptr;
		Publish = nullptr;
		Profile = {};
		Sequence = 0;
		TimingSequences = {};
	}

	bool Renderer::Impl::EnsureImageGraphPipelines() {
		if (ImageGraphs.Compute && ImageGraphs.Publish) return true;
		const auto storageUsage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
		const auto colourUsage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
		if (!SDL_GPUTextureSupportsFormat(
				Device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTURETYPE_2D, storageUsage
			) ||
			!SDL_GPUTextureSupportsFormat(
				Device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB, SDL_GPU_TEXTURETYPE_2D, colourUsage
			))
			return false;
		if (!ImageGraphs.Compute)
			ImageGraphs.Compute = LoadComputePipeline("imagegraph.comp", 2, 0, 1, 0, 8, 8);
		if (!ImageGraphs.Publish) {
			SDL_GPUShader *vertex = LoadShader("overlay.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
			SDL_GPUShader *fragment =
				LoadShader("imagegraph-publish.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
			if (vertex && fragment) {
				SDL_GPUColorTargetDescription target{};
				target.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
				SDL_GPUGraphicsPipelineCreateInfo pipeline{};
				pipeline.vertex_shader = vertex;
				pipeline.fragment_shader = fragment;
				pipeline.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
				pipeline.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
				pipeline.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
				pipeline.target_info.color_target_descriptions = &target;
				pipeline.target_info.num_color_targets = 1;
				ImageGraphs.Publish = SDL_CreateGPUGraphicsPipeline(Device, &pipeline);
			}
			if (vertex) SDL_ReleaseGPUShader(Device, vertex);
			if (fragment) SDL_ReleaseGPUShader(Device, fragment);
		}
		if (ImageGraphs.Compute && ImageGraphs.Publish && !ImageGraphs.Profile.HasGpuTimings)
			ImageGraphs.Profile.HasGpuTimings = ImageGraphs.Timestamps.Probe(Device);
		if (ImageGraphs.Compute && ImageGraphs.Publish && !ImageGraphs.Instrumented) {
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
		return ImageGraphs.Compute && ImageGraphs.Publish;
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

	ImageGraphEvaluation Renderer::EvaluateImageGraph(
		core::Name owner,
		core::Name name,
		std::string_view output,
		core::Name publishedTexture,
		imagegraph::Diagnostic &diagnostic
	) {
		RequireOwningThread("EvaluateImageGraph");
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
		const size_t targetIndex = graph.Plan.Outputs[outputIndex];
		const auto space = graph.Document.Outputs[outputIndex].Space;
		std::array<uint8_t, imagegraph::Limits::MaximumNodes> needed{};
		std::array<uint64_t, imagegraph::Limits::MaximumNodes> signatures{};
		std::array<SDL_GPUTexture *, imagegraph::Limits::MaximumNodes> sourceTextures{};
		std::array<assets::TextureFormat, imagegraph::Limits::MaximumNodes> sourceFormats{};
		needed[targetIndex] = 1;
		for (auto node = graph.Plan.Order.rbegin(); node != graph.Plan.Order.rend(); ++node)
			if (needed[*node])
				for (size_t input : graph.Plan.Inputs[*node])
					needed[input] = 1;
		bool dirty = false;
		for (size_t index : graph.Plan.Order) {
			if (!needed[index]) continue;
			uint64_t signature = graph.Nodes[index].AuthoredSignature;
			for (size_t input : graph.Plan.Inputs[index])
				signature = Fold(signature, signatures[input]);
			if (const auto *source = std::get_if<imagegraph::Source>(&graph.Document.Nodes[index].Value)) {
				const auto *binding = SourceBinding(graph, *source);
				sourceTextures[index] = State->Textures.Find(binding->Texture, binding->Owner);
				if (!sourceTextures[index] ||
					!State->Textures.FormatOf(binding->Texture, sourceFormats[index], binding->Owner))
					return refuse(graph.Document.Nodes[index].Id, "resident source is unavailable");
				if (binding->Owner == owner && binding->Texture == publishedTexture)
					return refuse(
						graph.Document.Nodes[index].Id, "an output cannot be its own resident source"
					);
				signature = Fold(
					Fold(signature, Key(binding->Owner, binding->Texture)),
					State->Textures.RevisionOf(binding->Texture, binding->Owner)
				);
			}
			signatures[index] = signature;
			dirty = dirty || !graph.Nodes[index].Texture || graph.Nodes[index].AcceptedSignature != signature;
		}
		auto publication =
			std::find_if(graph.Publications.begin(), graph.Publications.end(), [&](const auto &binding) {
				return binding.Name == publishedTexture;
			});
		const uint64_t publicationSignature = Fold(signatures[targetIndex], static_cast<uint64_t>(space));
		if (publication != graph.Publications.end() &&
			State->Textures.Find(publishedTexture, owner) != publication->Texture)
			return refuse({}, "published output was replaced by another content owner");
		if (!dirty && publication != graph.Publications.end() &&
			publication->Signature == publicationSignature) {
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
					if (needed[index] && sourceTextures[index] == published.Texture)
						return refuse(
							graph.Document.Nodes[index].Id, "live graph outputs cannot be source feedback"
						);
		}
		auto pending = std::find_if(
			state.PendingSubmissions.begin(), state.PendingSubmissions.end(), [](const auto &slot) {
				return slot.Fence == nullptr;
			}
		);
		if (pending == state.PendingSubmissions.end())
			return refuse({}, "GPU imagegraph queue is full; retry after completion");
		ENGINE_PROFILE_CAT("imagegraph gpu evaluation", core::ProfileCategory::Render);
		const auto started = std::chrono::steady_clock::now();
		std::array<SDL_GPUTexture *, imagegraph::Limits::MaximumNodes> textures{};
		std::array<ImageGraphGpuUniforms, imagegraph::Limits::MaximumNodes> uniforms{};
		std::array<uint8_t, imagegraph::Limits::MaximumNodes> allocated{};
		imagegraph::ExecutionPlan execution;
		SDL_GPUTexture *publicationTarget = nullptr;
		bool allocatedPublication = false;
		ImageGraphGpuState::Reservation reservation;
		bool reserved = false;
		bool committed = false;
		bool borrowedExecution = false;
		uint64_t executionSignature = Fold(graph.GeometrySignature, targetIndex);
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
			for (size_t index = 0; index < graph.Nodes.size(); ++index)
				if (allocated[index]) {
					Release(State->Device, textures[index], execution.Extents[index].Bytes, state);
					allocated[index] = 0;
				}
			if (allocatedPublication) {
				Release(State->Device, publicationTarget, execution.Extents[targetIndex].Bytes, state);
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
				if (!needed[index] || !sourceTextures[index]) continue;
				uint32_t width = 0, height = 0;
				const auto *binding =
					SourceBinding(graph, std::get<imagegraph::Source>(graph.Document.Nodes[index].Value));
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
					if (needed[index] && sourceTextures[index])
						sources.push_back(
							{graph.Document.Nodes[index].Id, sourceShapes[index][0], sourceShapes[index][1]}
						);
				if (!imagegraph::Prepare(graph.Document, graph.Plan, output, sources, execution, diagnostic))
					return ImageGraphEvaluation::Refused;
			}
			const auto &outputExtent = execution.Extents[targetIndex];
			size_t newBytes = 0;
			for (size_t index : execution.Order) {
				const auto &extent = execution.Extents[index];
				const auto &resident = graph.Nodes[index];
				if (!resident.Texture || resident.Extent != extent) newBytes += extent.Bytes;
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
								   publication->Extent != outputExtent || publication->Space != space;
			if (allocatedPublication) newBytes += outputExtent.Bytes;
			if (state.Profile.ResidentBytes > imagegraph::Limits::MaximumRetainedBytes ||
				newBytes > imagegraph::Limits::MaximumRetainedBytes - state.Profile.ResidentBytes) {
				cleanup();
				return refuse({}, "GPU imagegraph residency budget exceeded");
			}
			if (!state.Reserve(State->Textures, owner, publishedTexture, outputExtent.Bytes, reservation)) {
				cleanup();
				return refuse({}, "texture publication budget exceeded");
			}
			reserved = true;
			for (size_t index : execution.Order) {
				const auto &resident = graph.Nodes[index];
				textures[index] = resident.Texture;
				if (!resident.Texture || resident.Extent != execution.Extents[index]) {
					textures[index] = Allocate(
						State->Device,
						execution.Extents[index],
						SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
						SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE,
						state
					);
					allocated[index] = textures[index] != nullptr;
					if (!textures[index]) {
						cleanup();
						return refuse(graph.Document.Nodes[index].Id, "GPU intermediate allocation failed");
					}
				}
			}
			if (allocatedPublication) {
				publicationTarget = Allocate(
					State->Device,
					outputExtent,
					space == imagegraph::OutputSpace::SRGB ? SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB
														   : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
					SDL_GPU_TEXTUREUSAGE_SAMPLER |
						(space == imagegraph::OutputSpace::SRGB ? SDL_GPU_TEXTUREUSAGE_COLOR_TARGET : 0u),
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
				SDL_GPUTexture *first =
					sourceTextures[index] ? sourceTextures[index] : State->FallbackTexture;
				SDL_GPUTexture *second = State->FallbackTexture;
				if (!graph.Plan.Inputs[index].empty()) first = textures[graph.Plan.Inputs[index][0]];
				if (graph.Plan.Inputs[index].size() == 2) second = textures[graph.Plan.Inputs[index][1]];
				if (!RecordImageGraphNode(
						command,
						state.Compute,
						State->Textures.PixelSampler(),
						first,
						second,
						textures[index],
						uniforms[index]
					)) {
					cleanup();
					return refuse(graph.Document.Nodes[index].Id, "GPU compute recording failed");
				}
				++dispatches;
			}
			if (space == imagegraph::OutputSpace::SRGB) {
				if (!RecordImageGraphPublication(
						command,
						state.Publish,
						State->Textures.PixelSampler(),
						textures[targetIndex],
						publicationTarget,
						space
					)) {
					cleanup();
					return refuse({}, "GPU sRGB publication recording failed");
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
				resident.Texture = textures[index];
				resident.Extent = execution.Extents[index];
				resident.AcceptedSignature = signatures[index];
			}
			if (allocatedPublication) {
				const size_t retiredBytes =
					publication == graph.Publications.end() ? 0 : publication->Extent.Bytes;
				retire(
					state.Commit(State->Textures, reservation, publicationTarget, outputExtent, space),
					retiredBytes
				);
			} else
				state.Touch(State->Textures, owner, publishedTexture);
			if (publication == graph.Publications.end()) {
				graph.Publications.push_back(
					{publishedTexture, publicationTarget, outputExtent, space, publicationSignature}
				);
			} else
				*publication = {
					publishedTexture, publicationTarget, outputExtent, space, publicationSignature
				};
			++State->ResourceEpoch;
			++state.Profile.Evaluations;
			++state.Profile.CommandBuffers;
			state.Profile.ComputeDispatches += dispatches;
			state.Profile.CopiedBytes += outputExtent.Bytes;
			state.Profile.CpuRecordingMicroseconds =
				std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count();
			graph.Executions[outputIndex] = std::move(execution);
			graph.ExecutionSignatures[outputIndex] = executionSignature;
			committed = true;
			core::Metrics::Count("render.imagegraph.dispatches", dispatches);
			core::Metrics::Count("render.imagegraph.command_buffers", 1);
			core::Metrics::Count(
				"render.imagegraph.published_bytes", graph.Executions[outputIndex].Extents[targetIndex].Bytes
			);
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
