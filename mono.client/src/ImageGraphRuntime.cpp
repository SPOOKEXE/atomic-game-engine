#include "ImageGraphCameraAdapter.hpp"
#include "ImageGraphCameraRoute.hpp"
#include "ImageGraphComposerAdapter.hpp"
#include "ImageGraphFontInputs.hpp"
#include "ImageGraphSdfAdapter.hpp"
#include "ImageGraphSkyboxCache.hpp"
#include "ImageGraphSurfaceFormat.hpp"
#include "ImageGraphTransform3DAdapter.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/effects/Particles.hpp>
#include <engine/effects/Ribbon.hpp>
#include <engine/gui/Components.hpp>
#include <engine/imagegraph/FeedbackReplay.hpp>
#include <engine/imagegraphphysics/RigidReplay.hpp>
#include <engine/render/ImageGraphTransform3D.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/SourceSkyboxGroup.hpp>
#include <engine/scene/Atmosphere.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/TextureCatalogue.hpp>
#include <engine/scripthost/ComposerLua.hpp>

#include <algorithm>
#include <client/ContentDemand.hpp>
#include <client/ImageGraphRuntime.hpp>
#include <cmath>
#include <fstream>
#include <functional>
#include <limits>
#include <new>
#include <system_error>
#include <unordered_set>

namespace client {
	namespace {
		constexpr size_t MAXIMUM_DOCUMENT_BYTES = 8u * 1024u * 1024u;

		bool SafeStem(std::string_view stem) {
			if (stem.empty() || stem.size() > engine::scene::IMAGE_GRAPH_BINDING_MAXIMUM_NAME_BYTES)
				return false;
			return std::all_of(stem.begin(), stem.end(), [](unsigned char character) {
				return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
					   (character >= '0' && character <= '9') || character == '_' || character == '-';
			});
		}

		uint64_t Key(engine::core::Name owner, engine::core::Name texture) {
			return (uint64_t(owner.Id()) << 32) | texture.Id();
		}

		bool SameSelector(
			const engine::scene::ImageGraphBinding &left, const engine::scene::ImageGraphBinding &right
		) {
			return left.Graph == right.Graph && left.Output == right.Output &&
				   left.Texture == right.Texture && left.Seed == right.Seed &&
				   left.FixedTick == right.FixedTick && left.TickPolicy == right.TickPolicy &&
				   left.ColorSpace == right.ColorSpace;
		}

		engine::imagegraph::Status ReadCompiled(
			const std::filesystem::path &path,
			engine::imagegraph::Document &document,
			engine::imagegraph::Plan &plan,
			engine::imagegraph::Diagnostic &diagnostic
		) {
			using engine::imagegraph::Status;
			std::ifstream input(path, std::ios::binary | std::ios::ate);
			if (!input) {
				diagnostic = {Status::Malformed, {}, {}, "graph document is unavailable"};
				return diagnostic.Code;
			}
			const std::streamoff bytes = input.tellg();
			if (bytes < 0 || static_cast<uint64_t>(bytes) > MAXIMUM_DOCUMENT_BYTES) {
				diagnostic = {Status::LimitExceeded, {}, {}, "graph document exceeds host byte limit"};
				return diagnostic.Code;
			}
			std::string text(static_cast<size_t>(bytes), '\0');
			input.seekg(0);
			if (!input.read(text.data(), bytes)) {
				diagnostic = {Status::Malformed, {}, {}, "graph document could not be read"};
				return diagnostic.Code;
			}
			const Status parsed = engine::imagegraph::Read(text, document, diagnostic);
			if (parsed != Status::Ok) return parsed;
			return engine::imagegraph::Compile(document, plan, diagnostic);
		}

		bool PackArrayOutput(
			const engine::imagegraph::ImageArray &array, double framesPerSecond, ImageGraphFrameResult &result
		) try {
			using namespace engine::imagegraph;
			std::vector<size_t> frames;
			std::function<bool(const ImageArrayItem &, size_t)> flatten = [&](const ImageArrayItem &item,
																			  size_t depth) {
				if (depth > 64) return false;
				if (const auto *index = std::get_if<size_t>(&item.Data)) {
					if (*index >= array.Images.size() || frames.size() >= 4096) return false;
					frames.push_back(*index);
					return true;
				}
				for (const auto &child : std::get<std::vector<ImageArrayItem>>(item.Data))
					if (!flatten(child, depth + 1)) return false;
				return true;
			};
			for (const auto &item : array.Items)
				if (!flatten(item, 0)) return false;
			if (frames.empty() || !std::isfinite(framesPerSecond) || framesPerSecond <= 0) return false;
			const float duration = static_cast<float>(1.0 / framesPerSecond);
			if (!std::isfinite(duration) || duration <= 0) return false;
			const auto &first = array.Images[frames.front()];
			uint32_t side = 1;
			while (side * side < frames.size())
				side *= 2;
			const uint64_t width = uint64_t(first.Width) * side, height = uint64_t(first.Height) * side;
			if (width > engine::render::LiveImagePublisher::MAXIMUM_SIDE ||
				height > engine::render::LiveImagePublisher::MAXIMUM_SIDE)
				return false;
			const auto layout = CheckedSurfaceLayout(
				static_cast<uint32_t>(width),
				static_cast<uint32_t>(height),
				first.Format,
				engine::render::LiveImagePublisher::MAXIMUM_IMAGE_BYTES
			);
			if (!layout) return false;
			for (size_t index : frames) {
				const auto &frame = array.Images[index];
				if (frame.Width != first.Width || frame.Height != first.Height ||
					frame.Format != first.Format ||
					!ValidSurfaceLayout(frame, Limits::MaximumDimension, Limits::MaximumOutputBytes) ||
					!FiniteSurfaceSamples(frame))
					return false;
			}
			result.Image.Width = static_cast<uint32_t>(width);
			result.Image.Height = static_cast<uint32_t>(height);
			result.Image.Format = first.Format;
			result.Image.Pixels.resize(static_cast<size_t>(layout->Bytes));
			const size_t rowBytes = first.Pixels.size() / first.Height;
			for (size_t index = 0; index < frames.size(); ++index) {
				const auto &frame = array.Images[frames[index]];
				for (size_t row = 0; row < first.Height; ++row) {
					const size_t destination =
						((index / side * first.Height + row) * side + index % side) * rowBytes;
					std::copy_n(
						frame.Pixels.begin() + row * rowBytes,
						rowBytes,
						result.Image.Pixels.begin() + destination
					);
				}
			}
			result.FlipbookSide = static_cast<uint8_t>(side);
			result.FrameDurations.assign(frames.size(), duration);
			result.Image.Hash = SurfaceHash(result.Image);
			return true;
		} catch (const std::bad_alloc &) {
			return false;
		}

		bool HasLuaNodes(const engine::imagegraph::Document &document) {
			return std::any_of(document.Nodes.begin(), document.Nodes.end(), [](const auto &node) {
				return node.Type.starts_with("pc.lua_");
			});
		}

		std::unique_ptr<engine::imagegraph::ComposerLuaHost>
		LuaHostFor(const engine::imagegraph::Document &document) {
			return HasLuaNodes(document) ? engine::script::MakeComposerLuaHost() : nullptr;
		}

		struct LuaMessageDrain {
			engine::imagegraph::ComposerLuaHost *Host = nullptr;
			~LuaMessageDrain() {
				if (Host)
					for (const auto &message : Host->TakeMessages())
						ENGINE_INFO("image graph Lua {}: {}", message.NodeId, message.Text);
			}
		};

		bool NeedsFrameSamples(const engine::imagegraph::Document &document) {
			if (!document.Keyframes.empty()) return true;
			return std::any_of(document.Nodes.begin(), document.Nodes.end(), [&](const auto &node) {
				return bool(node.SourceSeparatedVec2Animators) || !node.SourceAnimatedInputs.empty() ||
					   node.Type == "image.audio_window" || node.Type == "image.audio_recording" ||
					   node.Type == "image.captured" || node.Type == "pc.audio_window" ||
					   node.Type == "pc.audio_loudness" || node.Type == "pc.interlaced" ||
					   node.Type == "pc.sequence_anim" || node.Type == "pc.cache_array" ||
					   (node.Type == "pc.cache" &&
						(std::any_of(
							 node.Values.begin(),
							 node.Values.end(),
							 [](const auto &v) {
								 return v.Port == "animated" && std::get_if<bool>(&v.Data) &&
										std::get<bool>(v.Data);
							 }
						 ) ||
						 std::any_of(
							 document.Links.begin(),
							 document.Links.end(),
							 [&](const auto &link) {
								 return link.ToNode == node.Id && link.ToPort == "animated";
							 }
						 ))) ||
					   node.Type == "pc.3_d_affector" || node.Type.starts_with("pc.verlet_sim_") ||
					   node.Type.starts_with("pc.flip_") || node.Type.starts_with("pc.rigid_") ||
					   node.Type.starts_with("pc.lua_") || node.Type.starts_with("pc.pcx_");
			});
		}
		class ArgumentProvider final : public engine::imagegraph::HostNodeProvider {
			engine::imagegraph::SourceArgumentHost &Arguments;
			engine::imagegraph::HostNodeProvider *Fallback;

		  public:
			ArgumentProvider(
				engine::imagegraph::SourceArgumentHost &arguments,
				engine::imagegraph::HostNodeProvider *fallback
			)
				: Arguments(arguments), Fallback(fallback) {}
			bool Capture(
				const engine::imagegraph::HostNodeInvocation &invocation,
				engine::imagegraph::HostNodeCapture &output,
				std::string &failure
			) override {
				if (invocation.Authored.Type == "pc.argument")
					return Arguments.Capture(invocation, output, failure);
				if (Fallback) return Fallback->Capture(invocation, output, failure);
				failure = "Client has no provider for this host node";
				return false;
			}
			bool PcxMessages(
				std::string_view node,
				std::span<const engine::imagegraph::PcxMessage> messages,
				std::string &failure
			) override {
				if (Fallback) return Fallback->PcxMessages(node, messages, failure);
				failure = "Client has no PCX message provider";
				return false;
			}
		};
		ImageGraphFrameResult EvaluateCompiled(
			const engine::imagegraph::Document &document,
			const engine::imagegraph::Plan &plan,
			engine::core::Name output,
			uint64_t tick,
			uint64_t seed,
			engine::imagegraph::CapturedFeedbackHost *feedbackOwner = nullptr,
			engine::imagegraph::HostNodeProvider *hostProvider = nullptr,
			engine::imagegraph::SourceArgumentHost *arguments = nullptr,
			const engine::imagegraphfont::GraphFontInputs *fonts = nullptr,
			const engine::imagegraph::EvaluationRequest *fontInputs = nullptr
		) {
			ImageGraphFrameResult result;
			result.Animated = NeedsFrameSamples(document);
			engine::imagegraph::CapturedFeedbackHost localFeedback;
			auto &feedback = feedbackOwner ? *feedbackOwner : localFeedback;
			auto localLua = hostProvider ? nullptr : LuaHostFor(document);
			LuaMessageDrain drain{localLua.get()};
			engine::imagegraph::SourceArgumentHost emptyArguments;
			ArgumentProvider argumentProvider(arguments ? *arguments : emptyArguments, localLua.get());
			engine::imagegraphphysics::RigidProvider rigid;
			engine::imagegraph::EvaluationRequest clock{
				.Tick = tick, .Seed = seed, .HostProvider = hostProvider ? hostProvider : &argumentProvider
			};
			clock.RigidProvider = &rigid;
			// Client seeks sample played frames, including fixed ticks. Repeated samples
			// use the host cache.
			clock.RigidPlaying = true;
			clock.SourceCachePlayback = engine::imagegraph::SourceCachePlaybackObservation{
				true, engine::imagegraph::SourceCacheSampling::NativePlayedPrefix, true
			};
			clock.RigidFrameProgress = true;
			engine::imagegraph::SourceFontContext heldFontContext;
			if (!detail::BindFontInputs(fonts, fontInputs, heldFontContext, clock, result.Diagnostic)) {
				result.Status = result.Diagnostic.Code;
				return result;
			}
			if (!feedback.Prepare(
					document,
					plan,
					1,
					seed,
					clock,
					result.Diagnostic,
					engine::imagegraph::Limits::MaximumEvaluationBytes,
					output.Text()
				)) {
				result.Status = result.Diagnostic.Code;
				return result;
			}
			if (feedback.Active()) {
				result.Animated = true;
				if (const auto *image = feedback.Output(output.Text())) {
					result.Image = *image;
					result.Status = engine::imagegraph::Status::Ok;
					return result;
				}
				if (const auto *value = feedback.Value(output.Text())) {
					if (const auto *array = std::get_if<engine::imagegraph::ImageArray>(&value->Output)) {
						const double fps = document.Timeline ? document.Timeline->FramesPerSecond : 30;
						result.Status = PackArrayOutput(*array, fps, result)
											? engine::imagegraph::Status::Ok
											: engine::imagegraph::Status::LimitExceeded;
						if (result.Status != engine::imagegraph::Status::Ok)
							result.Diagnostic = {
								result.Status, {}, {}, "stateful image array exceeds atlas bounds"
							};
						return result;
					}
					result.Status = engine::imagegraph::Status::InvalidOutput;
					result.Diagnostic = {result.Status, {}, {}, "image consumer requires an image output"};
					return result;
				}
			}

			const auto selected =
				std::find_if(document.Outputs.begin(), document.Outputs.end(), [&](const auto &item) {
					return item.Id == output.Text();
				});
			if (selected != document.Outputs.end()) {
				const auto node =
					std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &item) {
						return item.Id == selected->NodeId;
					});
				if (node != document.Nodes.end() && node->Type == "image.transform_3d" && !hostProvider) {
					result.Status = engine::imagegraph::Status::UnsupportedExecution;
					result.Diagnostic = {
						result.Status,
						node->Id,
						selected->Port,
						"Transform Image 3D requires a render-backed evaluation API"
					};
					return result;
				}
			}
			// Inspect the owned result after one execution, because live host nodes can
			// have side effects.
			engine::imagegraph::StatefulEvaluationResult evaluated;
			result.Status = engine::imagegraph::EvaluateStateful(
				document, plan, std::string(output.Text()), clock, evaluated, result.Diagnostic
			);
			if (result.Status != engine::imagegraph::Status::Ok) return result;
			if (auto *image = std::get_if<engine::imagegraph::Image>(&evaluated.Output)) {
				result.Image = std::move(*image);
			} else if (const auto *array = std::get_if<engine::imagegraph::ImageArray>(&evaluated.Output)) {
				const double fps = document.Timeline ? document.Timeline->FramesPerSecond : 30.0;
				if (!PackArrayOutput(*array, fps, result)) {
					result.Status = engine::imagegraph::Status::LimitExceeded;
					result.Diagnostic = {
						result.Status,
						{},
						{},
						"image array cannot fit the native atlas size, "
						"format or frame limits"
					};
				}
			} else {
				result.Status = engine::imagegraph::Status::InvalidOutput;
				result.Diagnostic = {result.Status, {}, {}, "image consumer requires an image output"};
			}
			return result;
		}

		const engine::imagegraph::Node *OutputNode(
			const engine::imagegraph::Document &document, engine::core::Name output, std::string_view &port
		) {
			const auto selected =
				std::find_if(document.Outputs.begin(), document.Outputs.end(), [&](const auto &item) {
					return item.Id == output.Text();
				});
			if (selected == document.Outputs.end()) return nullptr;
			port = selected->Port;
			const auto node =
				std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &item) {
					return item.Id == selected->NodeId;
				});
			return node == document.Nodes.end() ? nullptr : &*node;
		}

		ImageGraphFrameResult EvaluateTransform(
			const engine::imagegraph::Document &document,
			const engine::imagegraph::Plan &plan,
			const engine::imagegraph::Node &transform,
			std::string_view outputPort,
			engine::render::Renderer &renderer,
			uint64_t tick,
			uint64_t seed,
			engine::imagegraph::SourceArgumentHost *arguments,
			const engine::imagegraphfont::GraphFontInputs *fonts,
			const engine::imagegraph::EvaluationRequest *fontInputs
		) {
			using namespace engine;
			ImageGraphFrameResult result;
			result.Animated = NeedsFrameSamples(document);
			if (outputPort != "rendered" && outputPort != "depth" && outputPort != "mesh") {
				result.Status = imagegraph::Status::InvalidOutput;
				result.Diagnostic = {
					result.Status, transform.Id, std::string(outputPort), "selected output is not an image"
				};
				return result;
			}
			auto lua = LuaHostFor(document);
			LuaMessageDrain drain{lua.get()};
			engine::imagegraph::SourceArgumentHost emptyArguments;
			ArgumentProvider argumentProvider(arguments ? *arguments : emptyArguments, lua.get());
			render::imagegraph::TransformImage3DRequest request;
			if (!detail::BuildTransformRequest(
					document,
					plan,
					transform,
					tick,
					seed,
					false,
					request,
					result.Diagnostic,
					&argumentProvider,
					nullptr,
					1,
					fonts,
					fontInputs
				)) {
				result.Status = result.Diagnostic.Code;
				return result;
			}
			render::imagegraph::TransformImage3DResult gpu;
			const auto status = render::imagegraph::ExecuteTransformImage3D(renderer, request, gpu);
			if (status != render::imagegraph::TransformImage3DStatus::Ok) {
				result.Status = status == render::imagegraph::TransformImage3DStatus::GpuUnavailable
									? imagegraph::Status::UnsupportedExecution
									: imagegraph::Status::InvalidValue;
				result.Diagnostic = {result.Status, transform.Id, {}, "Transform Image 3D GPU pass failed"};
				return result;
			}
			result.Mesh = gpu.Mesh;
			if (outputPort == "mesh") {
				result.Status = imagegraph::Status::Ok;
				return result;
			}
			result.Image.Width = gpu.Width;
			result.Image.Height = gpu.Height;
			if (outputPort == "depth") {
				result.Image.Format = imagegraph::SurfaceFormat::RGBA8Unorm;
				result.Image.Pixels.assign(
					reinterpret_cast<const uint8_t *>(gpu.DepthRgba8.data()),
					reinterpret_cast<const uint8_t *>(gpu.DepthRgba8.data() + gpu.DepthRgba8.size())
				);
			} else {
				const auto format = detail::SurfaceFormatForTexture(gpu.RenderedFormat);
				if (!format) {
					result.Status = imagegraph::Status::UnsupportedExecution;
					result.Diagnostic = {
						result.Status,
						transform.Id,
						std::string(outputPort),
						"Transform Image 3D output format is unsupported"
					};
					return result;
				}
				result.Image.Format = *format;
				result.Image.Pixels.assign(
					reinterpret_cast<const uint8_t *>(gpu.RenderedPixels.data()),
					reinterpret_cast<const uint8_t *>(gpu.RenderedPixels.data() + gpu.RenderedPixels.size())
				);
			}
			result.Image.Hash = imagegraph::SurfaceHash(result.Image);
			result.Status = imagegraph::Status::Ok;
			return result;
		}

		ImageGraphFrameResult EvaluateCamera(
			const engine::imagegraph::Document &document,
			const engine::imagegraph::Plan &plan,
			const engine::imagegraph::Node &node,
			std::string_view port,
			engine::render::Renderer &renderer,
			uint64_t tick,
			uint64_t seed,
			engine::imagegraph::SourceArgumentHost *arguments,
			const engine::imagegraphfont::GraphFontInputs *fonts,
			const engine::imagegraph::EvaluationRequest *fontInputs
		) {
			ImageGraphFrameResult result;
			result.Animated = NeedsFrameSamples(document);
			auto lua = LuaHostFor(document);
			LuaMessageDrain drain{lua.get()};
			engine::imagegraph::SourceArgumentHost emptyArguments;
			ArgumentProvider argumentProvider(arguments ? *arguments : emptyArguments, lua.get());
			engine::render::imagegraph::SourceCamera3DRequest request;
			if (!detail::BuildCameraRequest(
					document,
					plan,
					node,
					port,
					tick,
					seed,
					false,
					request,
					result.Diagnostic,
					&argumentProvider,
					nullptr,
					1,
					fonts,
					fontInputs
				)) {
				result.Status = result.Diagnostic.Code;
				return result;
			}
			engine::render::imagegraph::SourceCamera3DResult gpu;
			const auto status = engine::render::imagegraph::ExecuteSourceCamera3D(renderer, request, gpu);
			if (status != engine::render::imagegraph::SourceCamera3DStatus::Ok) {
				result.Status = engine::imagegraph::Status::UnsupportedExecution;
				result.Diagnostic = {
					result.Status, node.Id, std::string(port), "source camera GPU pass failed"
				};
				return result;
			}
			const auto format = detail::SurfaceFormatForTexture(gpu.Format);
			if (!format) {
				result.Status = engine::imagegraph::Status::UnsupportedExecution;
				result.Diagnostic = {
					result.Status, node.Id, std::string(port), "source camera output format is unsupported"
				};
				return result;
			}
			result.Image.Width = gpu.Width;
			result.Image.Height = gpu.Height;
			result.Image.Format = *format;
			result.Image.Pixels.assign(
				reinterpret_cast<const uint8_t *>(gpu.Pixels.data()),
				reinterpret_cast<const uint8_t *>(gpu.Pixels.data() + gpu.Pixels.size())
			);
			result.Image.Hash = engine::imagegraph::SurfaceHash(result.Image);
			result.Status = engine::imagegraph::Status::Ok;
			return result;
		}
		ImageGraphFrameResult EvaluateSdf(
			const engine::imagegraph::Document &document,
			const engine::imagegraph::Plan &plan,
			const engine::imagegraph::Node &node,
			std::string_view port,
			engine::render::Renderer &renderer,
			uint64_t tick,
			uint64_t seed,
			engine::imagegraph::SourceArgumentHost *arguments,
			const engine::imagegraphfont::GraphFontInputs *fonts,
			const engine::imagegraph::EvaluationRequest *fontInputs
		) {
			ImageGraphFrameResult result;
			result.Animated = NeedsFrameSamples(document);
			auto lua = LuaHostFor(document);
			LuaMessageDrain drain{lua.get()};
			engine::imagegraph::SourceArgumentHost emptyArguments;
			ArgumentProvider argumentProvider(arguments ? *arguments : emptyArguments, lua.get());
			detail::ComposerProvider composer(
				renderer, engine::core::Name("client.imagegraph.sdf-snapshot"), &argumentProvider
			);
			engine::render::imagegraph::SourceSdfRequest request;
			if (!detail::BuildSdfRequest(
					document,
					plan,
					node,
					port,
					tick,
					seed,
					false,
					request,
					result.Diagnostic,
					&composer,
					nullptr,
					1,
					fonts,
					fontInputs
				)) {
				result.Status = result.Diagnostic.Code;
				return result;
			}
			engine::render::imagegraph::SourceSdfResult gpu;
			const auto status = engine::render::imagegraph::ExecuteSourceSdf(renderer, request, gpu);
			if (status != engine::render::imagegraph::SourceSdfStatus::Ok) {
				result.Status = engine::imagegraph::Status::UnsupportedExecution;
				result.Diagnostic = {result.Status, node.Id, std::string(port), "source sdf GPU pass failed"};
				return result;
			}
			const auto format = detail::SurfaceFormatForTexture(gpu.Format);
			if (!format) {
				result.Status = engine::imagegraph::Status::UnsupportedExecution;
				result.Diagnostic = {
					result.Status, node.Id, std::string(port), "source sdf output format is unsupported"
				};
				return result;
			}
			result.Image.Width = gpu.Width;
			result.Image.Height = gpu.Height;
			result.Image.Format = *format;
			result.Image.Pixels.assign(
				reinterpret_cast<const uint8_t *>(gpu.Pixels.data()),
				reinterpret_cast<const uint8_t *>(gpu.Pixels.data() + gpu.Pixels.size())
			);
			result.Image.Hash = engine::imagegraph::SurfaceHash(result.Image);
			result.Status = engine::imagegraph::Status::Ok;
			return result;
		}
		ImageGraphFrameResult EvaluateForRenderer(
			const engine::imagegraph::Document &document,
			const engine::imagegraph::Plan &plan,
			engine::core::Name output,
			engine::render::Renderer &renderer,
			uint64_t tick,
			uint64_t seed,
			engine::imagegraph::SourceArgumentHost *arguments,
			const engine::imagegraphfont::GraphFontInputs *fonts,
			const engine::imagegraph::EvaluationRequest *fontInputs
		) {
			std::string_view port;
			const auto *node = OutputNode(document, output, port);
			if (node != nullptr && node->Type == "image.transform_3d")
				return EvaluateTransform(
					document, plan, *node, port, renderer, tick, seed, arguments, fonts, fontInputs
				);
			if (node && (node->Type == "pc.3_d_camera" || node->Type == "pc.3_d_camera_set"))
				return EvaluateCamera(
					document, plan, *node, port, renderer, tick, seed, arguments, fonts, fontInputs
				);
			if (node && (node->Type == "pc.rm_render" || node->Type == "pc.rm_render_scatter" ||
						 node->Type == "pc.rm_cloud" || node->Type == "pc.rm_terrain" ||
						 node->Type == "pc.rm_primitive" || node->Type == "pc.rm_combine"))
				return EvaluateSdf(
					document, plan, *node, port, renderer, tick, seed, arguments, fonts, fontInputs
				);
			return EvaluateCompiled(
				document, plan, output, tick, seed, nullptr, nullptr, arguments, fonts, fontInputs
			);
		}
	} // namespace

	std::filesystem::path
	ImageGraphDocumentPath(const std::filesystem::path &assetsDirectory, engine::core::Name graph) {
		if (!graph.IsValid() || !SafeStem(graph.Text())) return {};
		return assetsDirectory / "imagegraphs" / (std::string(graph.Text()) + ".graph");
	}

	ImageGraphFrameResult LoadImageGraphFrame(
		const std::filesystem::path &directory,
		engine::core::Name graph,
		engine::core::Name output,
		uint64_t tick,
		uint64_t seed,
		engine::imagegraph::SourceArgumentHost *arguments,
		const engine::imagegraphfont::GraphFontInputs *fonts,
		const engine::imagegraph::EvaluationRequest *fontInputs
	) {
		ImageGraphFrameResult result;
		const std::filesystem::path path = ImageGraphDocumentPath(directory, graph);
		if (path.empty() || !output.IsValid()) {
			result.Diagnostic = {
				engine::imagegraph::Status::InvalidValue, {}, {}, "invalid graph or output selector"
			};
			result.Status = result.Diagnostic.Code;
			return result;
		}
		engine::imagegraph::Document document;
		engine::imagegraph::Plan plan;
		result.Status = ReadCompiled(path, document, plan, result.Diagnostic);
		if (result.Status != engine::imagegraph::Status::Ok) return result;
		return EvaluateCompiled(
			document, plan, output, tick, seed, nullptr, nullptr, arguments, fonts, fontInputs
		);
	}

	ImageGraphFrameResult LoadImageGraphRenderExportFrame(
		const std::filesystem::path &directory,
		engine::core::Name graph,
		engine::core::Name output,
		engine::render::Renderer &renderer,
		uint64_t tick,
		uint64_t seed,
		engine::imagegraph::SourceArgumentHost *arguments,
		const engine::imagegraphfont::GraphFontInputs *fonts,
		const engine::imagegraph::EvaluationRequest *fontInputs
	) {
		ImageGraphFrameResult result;
		const std::filesystem::path path = ImageGraphDocumentPath(directory, graph);
		if (path.empty() || !output.IsValid()) {
			result.Diagnostic = {
				engine::imagegraph::Status::InvalidValue, {}, {}, "invalid graph or output selector"
			};
			result.Status = result.Diagnostic.Code;
			return result;
		}
		engine::imagegraph::Document document;
		engine::imagegraph::Plan plan;
		result.Status = ReadCompiled(path, document, plan, result.Diagnostic);
		if (result.Status != engine::imagegraph::Status::Ok) return result;
		return EvaluateForRenderer(
			document, plan, output, renderer, tick, seed, arguments, fonts, fontInputs
		);
	}

	bool ImageGraphRuntime::CollectWantedComposerShaders(
		engine::ecs::Store &store,
		engine::core::Name owner,
		const std::filesystem::path &directory,
		std::vector<engine::core::Name> &output
	) {
		ENGINE_PROFILE("composer artifact demand");
		if (!owner.IsValid() || output.size() > MAXIMUM_SINK_REFERENCES || directory.native().size() > 4096)
			return false;
		store.Observe<engine::scene::ImageGraphBinding>();
		const auto version = store.ComponentChangeVersion<engine::scene::ImageGraphBinding>();
		const auto rows = store.CountMatching<engine::scene::ImageGraphBinding>();
		if (rows > MAXIMUM_SINK_REFERENCES) return false;
		auto scan =
			std::find_if(ComposerDemandScans.begin(), ComposerDemandScans.end(), [&](const auto &value) {
				return value.Owner == owner;
			});
		if (scan != ComposerDemandScans.end() && scan->StoreIdentity == store.Identity() &&
			scan->Directory == directory && scan->BindingVersion == version && scan->BindingRows == rows &&
			scan->DocumentRevision == ComposerDocumentRevision)
			return true;
		if (scan == ComposerDemandScans.end()) {
			scan =
				std::find_if(ComposerDemandScans.begin(), ComposerDemandScans.end(), [](const auto &value) {
					return value.StoreIdentity == 0;
				});
			if (scan == ComposerDemandScans.end()) return false;
		}
		std::array<const CachedDocument *, MAXIMUM_CACHED_DOCUMENTS> seen{};
		size_t documentCount = 0, appended = 0;
		bool valid = true;
		try {
			store.Each<const engine::scene::ImageGraphBinding>([&](engine::ecs::Entity, const auto &binding) {
				if (!valid) return;
				if (!engine::scene::IsValidImageGraphBinding(binding)) {
					valid = false;
					return;
				}
				const auto path = ImageGraphDocumentPath(directory, binding.Graph);
				const auto found = Documents.find(path.string());
				// A missing or refused graph must be retried after Refresh, never
				// stamped as complete.
				if (found == Documents.end()) {
					valid = false;
					return;
				}
				const auto *cached = &found->second;
				if (std::find(seen.begin(), seen.begin() + documentCount, cached) !=
					seen.begin() + documentCount)
					return;
				if (documentCount == seen.size()) {
					valid = false;
					return;
				}
				seen[documentCount++] = cached;
				for (const auto &node : cached->Authored.Nodes) {
					if (node.Type != "pc.hlsl") continue;
					const auto artifact = detail::ComposerAsset(node);
					if (!artifact.IsValid() ||
						std::find(output.begin(), output.end(), artifact) != output.end())
						continue;
					if (appended == engine::render::hlsl::MAXIMUM_OWNER_PROGRAMS ||
						output.size() == MAXIMUM_SINK_REFERENCES) {
						valid = false;
						return;
					}
					output.push_back(artifact);
					++appended;
				}
			});
			if (!valid) return false;
			ComposerDemandScan prepared{
				store.Identity(), owner, directory, version, rows, ComposerDocumentRevision
			};
			*scan = std::move(prepared);
			return true;
		} catch (const std::bad_alloc &) {
			return false;
		}
	}

	engine::imagegraph::Status ImageGraphRuntime::PrepareArguments(
		const engine::imagegraph::SourceArgumentOptions &options,
		engine::render::Renderer &renderer,
		engine::imagegraph::Diagnostic &diagnostic,
		uint64_t maximumBytes
	) {
		if (ArgumentsGeneration == UINT64_MAX) {
			diagnostic = {
				engine::imagegraph::Status::LimitExceeded, {}, {}, "Client argument generation is exhausted"
			};
			return diagnostic.Code;
		}
		const auto status = Arguments.PrepareOptions(options, maximumBytes, diagnostic);
		if (status != engine::imagegraph::Status::Ok) return status;
		const auto transformGeneration = NextTransformGeneration;
		Clear(renderer);
		NextTransformGeneration = transformGeneration;
		++ArgumentsGeneration;
		return status;
	}

	engine::imagegraph::Status ImageGraphRuntime::PrepareFonts(
		const engine::imagegraphfont::GraphFontConfiguration &configuration,
		const engine::assets::ContentPolicy &policy,
		engine::render::Renderer &renderer,
		engine::imagegraph::Diagnostic &diagnostic,
		uint64_t maximumBytes
	) {
		if (!Fonts.Replace(configuration, policy, maximumBytes, diagnostic)) return diagnostic.Code;
		const auto transformGeneration = NextTransformGeneration;
		Clear(renderer);
		NextTransformGeneration = transformGeneration;
		return engine::imagegraph::Status::Ok;
	}

	void ImageGraphRuntime::BeginFrame() {
		ChecksRemaining = MAXIMUM_CHECKS_PER_FRAME;
		Error.clear();
		SkyboxOwners = std::move(NextSkyboxOwners);
		NextSkyboxOwners.clear();
		std::sort(SkyboxOwners.begin(), SkyboxOwners.end(), [](auto left, auto right) {
			return left.Id() < right.Id();
		});
		SkyboxOwners.erase(std::unique(SkyboxOwners.begin(), SkyboxOwners.end()), SkyboxOwners.end());
		PrioritySkyboxOwner = SkyboxOwners.empty() ? engine::core::Name{}
												   : SkyboxOwners[NextSkyboxOwner++ % SkyboxOwners.size()];
		PrioritySkyboxVisited = false;
	}

	size_t ImageGraphRuntime::Refresh(
		engine::ecs::Store &store,
		engine::render::Renderer &renderer,
		engine::core::Name owner,
		const std::filesystem::path &directory
	) {
		if (!owner.IsValid()) return 0;
		std::unordered_set<uint64_t> seen;
		size_t updated = 0;
		size_t ordinal = 0;
		size_t &nextBinding = NextBindingByOwner[owner.Id()];
		std::optional<size_t> retryBinding;
		const uint64_t usageRevision = WantedContentRevision(store);
		auto [usagePosition, usageInserted] = SinkUsages.try_emplace(store.Identity());
		SinkUsage &usage = usagePosition->second;
		usage.LastUse = ++CacheClock;
		if (usageInserted || usage.Revision != usageRevision) {
			usage.Flags.clear();
			usage.Valid = true;
			usage.Revision = usageRevision;
			size_t references = 0;
			const auto mark = [&](engine::core::Name name, uint8_t flag) {
				if (!name.IsValid() || !usage.Valid) return;
				if (++references > MAXIMUM_SINK_REFERENCES) {
					usage.Valid = false;
					usage.Flags.clear();
					return;
				}
				usage.Flags[name.Id()] |= flag;
			};
			store.Each<const engine::scene::SurfaceAppearance>(
				[&](engine::ecs::Entity, const engine::scene::SurfaceAppearance &surface) {
					mark(surface.ColourMap, 1);
					mark(surface.EmissiveMap, 1);
					for (engine::core::Name name :
						 {surface.NormalMap,
						  surface.RoughnessMap,
						  surface.OcclusionMap,
						  surface.HeightMap,
						  surface.MetalnessMap,
						  surface.PackedPbrMap})
						mark(name, 2);
				}
			);
			store.Each<const engine::gui::Picture>([&](engine::ecs::Entity,
													   const engine::gui::Picture &picture) {
				mark(picture.Image, 1);
				mark(picture.HoverImage, 1);
				mark(picture.PressedImage, 1);
			});
			store.Each<const engine::effects::ParticleEmitter>(
				[&](engine::ecs::Entity, const engine::effects::ParticleEmitter &emitter) {
					mark(emitter.Texture, 1);
				}
			);
			store.Each<const engine::effects::Beam>(
				[&](engine::ecs::Entity, const engine::effects::Beam &beam) { mark(beam.Texture, 1); }
			);
			store.Each<const engine::effects::Trail>(
				[&](engine::ecs::Entity, const engine::effects::Trail &trail) { mark(trail.Texture, 1); }
			);
			store.Each<const engine::effects::Decal>(
				[&](engine::ecs::Entity, const engine::effects::Decal &decal) { mark(decal.Image, 1); }
			);
			store.Each<const engine::effects::Texture>(
				[&](engine::ecs::Entity, const engine::effects::Texture &texture) { mark(texture.Image, 1); }
			);
			const auto environment = engine::scene::EnvironmentOf(store);
			if (environment.Skybox == engine::scene::SkyboxSource::Textures && environment.Textures.Enabled) {
				for (engine::core::Name name :
					 {environment.Textures.Front,
					  environment.Textures.Back,
					  environment.Textures.Left,
					  environment.Textures.Right,
					  environment.Textures.Up,
					  environment.Textures.Down})
					mark(name, 1);
			}
		}
		while (SinkUsages.size() > MAXIMUM_CACHED_DOCUMENTS) {
			const auto oldest = std::min_element(
				SinkUsages.begin(), SinkUsages.end(), [](const auto &left, const auto &right) {
					return left.second.LastUse < right.second.LastUse;
				}
			);
			if (oldest == usagePosition) break;
			SinkUsages.erase(oldest);
		}
		const auto compiledDocument = [&](const std::filesystem::path &path,
										  const std::filesystem::file_time_type modified,
										  uintmax_t fileBytes) -> CachedDocument * {
			const std::string pathKey = path.string();
			auto cached = Documents.find(pathKey);
			if (cached != Documents.end() &&
				(cached->second.Modified != modified || cached->second.FileBytes != fileBytes)) {
				CachedDocumentBytes -= static_cast<size_t>(cached->second.FileBytes);
				Documents.erase(cached);
				++ComposerDocumentRevision;
				cached = Documents.end();
			}
			if (cached == Documents.end()) {
				CachedDocument prepared;
				engine::imagegraph::Diagnostic diagnostic;
				const auto status = ReadCompiled(path, prepared.Authored, prepared.Compiled, diagnostic);
				if (status != engine::imagegraph::Status::Ok) {
					Error = diagnostic.Message;
					return nullptr;
				}
				++Parses;
				prepared.Modified = modified;
				prepared.FileBytes = fileBytes;
				prepared.LastUse = ++CacheClock;
				while (!Documents.empty() &&
					   (Documents.size() >= MAXIMUM_CACHED_DOCUMENTS ||
						CachedDocumentBytes + fileBytes > MAXIMUM_CACHED_DOCUMENT_BYTES)) {
					const auto oldest = std::min_element(
						Documents.begin(), Documents.end(), [](const auto &left, const auto &right) {
							return left.second.LastUse < right.second.LastUse;
						}
					);
					CachedDocumentBytes -= static_cast<size_t>(oldest->second.FileBytes);
					Documents.erase(oldest);
					++ComposerDocumentRevision;
				}
				CachedDocumentBytes += static_cast<size_t>(fileBytes);
				cached = Documents.emplace(pathKey, std::move(prepared)).first;
				++ComposerDocumentRevision;
			} else {
				cached->second.LastUse = ++CacheClock;
			}
			for (const auto &node : cached->second.Authored.Nodes) {
				if (node.Type != "pc.hlsl") continue;
				const auto artifactName = detail::ComposerAsset(node);
				if (!artifactName.IsValid() || renderer.ComposerShaderRevision(owner, artifactName) != 0)
					continue;
				engine::assets::ShaderData artifact;
				if (!engine::render::hlsl::ReadArtifact(directory, artifactName, artifact)) {
					if (const auto failure = renderer.InstallComposerShader(owner, artifactName, artifact))
						Error = *failure;
				}
			}
			return &cached->second;
		};

		// Six separately authored sky faces are checked and published as one
		// generation. Incomplete rebinding retains the previous complete sky.
		std::unordered_set<uint32_t> groupedNames;
		const auto environment = engine::scene::EnvironmentOf(store);
		const std::array skyNames{
			environment.Textures.Front,
			environment.Textures.Back,
			environment.Textures.Left,
			environment.Textures.Right,
			environment.Textures.Up,
			environment.Textures.Down
		};
		bool sixFaces =
			environment.Skybox == engine::scene::SkyboxSource::Textures && environment.Textures.Enabled;
		for (size_t index = 0; index < skyNames.size() && sixFaces; ++index) {
			if (!skyNames[index].IsValid()) sixFaces = false;
			for (size_t earlier = 0; earlier < index; ++earlier)
				if (skyNames[index] == skyNames[earlier]) sixFaces = false;
		}
		struct SkyFace {
			engine::ecs::Entity Entity;
			engine::scene::ImageGraphBinding Selector;
			bool Found = false;
			bool Duplicate = false;
		};
		std::array<SkyFace, 6> skyFaces{};
		if (sixFaces) {
			store.Each<const engine::scene::ImageGraphBinding>(
				[&](engine::ecs::Entity entity, const engine::scene::ImageGraphBinding &selector) {
					for (size_t index = 0; index < skyNames.size(); ++index) {
						if (selector.Texture != skyNames[index]) continue;
						if (skyFaces[index].Found)
							skyFaces[index].Duplicate = true;
						else
							skyFaces[index] = {entity, selector, true, false};
					}
				}
			);
		}
		const auto cancelSkyboxCaptures = [&](PendingSkyboxGroup &group) {
			for (const auto &selector : group.Selectors) {
				const auto found = Entries.find(Key(owner, selector.Texture));
				if (found == Entries.end()) continue;
				auto &entry = found->second;
				for (const auto name : entry.HostCaptures)
					renderer.CancelComposerCapture(owner, name);
				entry.HostCaptures.clear();
				entry.PendingHostTick.reset();
				entry.HostObservations.Clear();
			}
		};
		bool completeSky = sixFaces;
		bool anySkyBinding = false;
		for (const SkyFace &face : skyFaces)
			anySkyBinding |= face.Found;
		for (const SkyFace &face : skyFaces)
			completeSky &= face.Found && !face.Duplicate &&
						   engine::scene::IsValidImageGraphBinding(face.Selector) &&
						   face.Selector.ColorSpace == engine::scene::ImageGraphColorSpace::Display;
		auto priorSky = SkyboxGroups.find(owner.Id());
		if (priorSky != SkyboxGroups.end() && priorSky->second.StoreIdentity != store.Identity()) {
			for (const engine::core::Name name : priorSky->second.Names) {
				const auto held = Entries.find(Key(owner, name));
				if (held != Entries.end()) {
					for (const auto name : held->second.HostCaptures)
						renderer.CancelComposerCapture(owner, name);
					(void)Publisher.Retire(renderer, held->second.Publication);
					Entries.erase(held);
				}
			}
			priorSky = SkyboxGroups.erase(priorSky);
		}
		const bool samePriorSky = priorSky != SkyboxGroups.end() && priorSky->second.Names == skyNames;
		if (sixFaces && (anySkyBinding || samePriorSky)) {
			for (const auto name : skyNames) {
				groupedNames.insert(name.Id());
				seen.insert(Key(owner, name));
			}
		}
		if (sixFaces && anySkyBinding && !completeSky)
			Error = "skybox needs six distinct valid display image graph bindings";
		if (priorSky != SkyboxGroups.end() && !samePriorSky) SkyboxGroups.erase(priorSky);
		if (!completeSky) {
			auto pending = PendingSkyboxes.find(owner.Id());
			if (pending != PendingSkyboxes.end()) {
				cancelSkyboxCaptures(pending->second);
				engine::render::imagegraph::CancelSourceSkybox(renderer, pending->second.Work);
				PendingSkyboxes.erase(pending);
			}
		}
		if (completeSky) {
			if (NextSkyboxOwners.size() < engine::render::LiveImagePublisher::MAXIMUM_BINDINGS &&
				std::find(NextSkyboxOwners.begin(), NextSkyboxOwners.end(), owner) == NextSkyboxOwners.end())
				NextSkyboxOwners.push_back(owner);
			const bool priority = !PrioritySkyboxOwner.IsValid() || PrioritySkyboxOwner == owner;
			if (priority && ChecksRemaining >= skyFaces.size()) {
				ChecksRemaining -= skyFaces.size();
				PrioritySkyboxVisited = true;
				std::array<std::filesystem::path, 6> paths;
				std::array<std::filesystem::file_time_type, 6> modified;
				std::array<uintmax_t, 6> fileBytes{};
				std::array<uint64_t, 6> ticks{};
				bool valid = usage.Valid;
				bool changed = !samePriorSky;
				for (size_t index = 0; index < skyFaces.size() && valid; ++index) {
					const auto &selector = skyFaces[index].Selector;
					if ((usage.Flags[selector.Texture.Id()] & 2) != 0) {
						Error = "skybox image graph output also has a numeric material sink";
						valid = false;
						break;
					}
					paths[index] = ImageGraphDocumentPath(directory, selector.Graph);
					if (paths[index].empty()) {
						Error = "unsafe image graph document name";
						valid = false;
						break;
					}
					std::error_code error;
					modified[index] = std::filesystem::last_write_time(paths[index], error);
					if (!error) fileBytes[index] = std::filesystem::file_size(paths[index], error);
					if (error || fileBytes[index] > MAXIMUM_DOCUMENT_BYTES) {
						Error = "image graph skybox document is unavailable or too large";
						valid = false;
						break;
					}
					ticks[index] = selector.TickPolicy == engine::scene::ImageGraphTickPolicy::World
									   ? store.Time().Tick
									   : selector.FixedTick;
					const auto held = Entries.find(Key(owner, selector.Texture));
					changed |= held == Entries.end() || held->second.StoreIdentity != store.Identity() ||
							   held->second.Entity != skyFaces[index].Entity ||
							   !SameSelector(held->second.Selector, selector) || !held->second.Published ||
							   held->second.Modified != modified[index] ||
							   held->second.FileBytes != fileBytes[index] ||
							   (held->second.Animated && held->second.Tick != ticks[index]);
				}
				if (!usage.Valid) Error = "image graph sink reference limit exceeded";
				bool sourceSkybox = PendingSkyboxes.contains(owner.Id());
				for (size_t index = 0; index < 6 && valid && !sourceSkybox; ++index) {
					auto *document = compiledDocument(paths[index], modified[index], fileBytes[index]);
					if (!document) {
						valid = false;
						break;
					}
					std::string_view port;
					const auto *node = OutputNode(document->Authored, skyFaces[index].Selector.Output, port);
					sourceSkybox =
						std::any_of(
							document->Authored.Nodes.begin(),
							document->Authored.Nodes.end(),
							[](const auto &authored) {
								return authored.Type == "pc.3_d_transform_image" ||
									   authored.Type == "image.transform_3d";
							}
						) ||
						(node && (node->Type == "pc.3_d_camera" || node->Type == "pc.3_d_camera_set" ||
								  node->Type == "pc.rm_render" || node->Type == "pc.rm_render_scatter" ||
								  node->Type == "pc.rm_cloud" || node->Type == "pc.rm_terrain" ||
								  node->Type == "pc.rm_primitive" || node->Type == "pc.rm_combine"));
				}
				if (sourceSkybox) {
					auto pending = PendingSkyboxes.find(owner.Id());
					if (pending != PendingSkyboxes.end()) {
						std::array<engine::scene::ImageGraphBinding, 6> currentSelectors;
						std::array<engine::ecs::Entity, 6> currentEntities;
						for (size_t index = 0; index < 6; ++index) {
							currentSelectors[index] = skyFaces[index].Selector;
							currentEntities[index] = skyFaces[index].Entity;
						}
						bool same =
							valid &&
							detail::SameSourceSkyboxCapture(
								{pending->second.StoreIdentity,
								 pending->second.Selectors,
								 pending->second.Entities,
								 pending->second.Modified,
								 pending->second.FileBytes},
								{store.Identity(), currentSelectors, currentEntities, modified, fileBytes}
							);
						if (same && pending->second.Work.Failed)
							for (size_t index = 0; index < 6; ++index)
								if (pending->second.Animated[index] &&
									pending->second.Ticks[index] != ticks[index])
									same = false;
						if (!same) {
							cancelSkyboxCaptures(pending->second);
							engine::render::imagegraph::CancelSourceSkybox(renderer, pending->second.Work);
							PendingSkyboxes.erase(pending);
							pending = PendingSkyboxes.end();
						}
					}
					if (valid && changed && pending == PendingSkyboxes.end()) {
						if (std::count_if(
								PendingSkyboxes.begin(),
								PendingSkyboxes.end(),
								[](const auto &entry) { return !entry.second.Work.Failed; }
							) >= 4 ||
							PendingSkyboxes.size() >=
								engine::render::LiveImagePublisher::MAXIMUM_BINDINGS / 6) {
							Error = "skybox pending group limit reached";
							valid = false;
						}
						if (valid) {
							PendingSkyboxGroup candidate;
							candidate.StoreIdentity = store.Identity();
							candidate.Admission.emplace();
							for (size_t index = 0; index < 6; ++index) {
								candidate.Selectors[index] = skyFaces[index].Selector;
								candidate.Entities[index] = skyFaces[index].Entity;
								candidate.Modified[index] = modified[index];
								candidate.FileBytes[index] = fileBytes[index];
								candidate.Ticks[index] = ticks[index];
							}
							pending = PendingSkyboxes.emplace(owner.Id(), std::move(candidate)).first;
						}
					}
					if (valid && pending != PendingSkyboxes.end() && pending->second.Admission) {
						Error.clear();
						auto &candidate = pending->second;
						auto &request = *candidate.Admission;
						auto &retainedBytes = candidate.RetainedFaceBytes;
						auto &outputBytes = candidate.OutputFaceBytes;
						request.Owner = owner;
						// One stable, process-local staging namespace per presented owner,
						// not per graph generation.
						request.StagingOwner = engine::core::Name(
							std::string("engine.imagegraph.skybox.staging.") + std::to_string(owner.Id())
						);
						bool waitingForCapture = false;
						for (size_t index = candidate.PreparedFaces; index < 6 && valid; ++index) {
							CachedDocument *document =
								compiledDocument(paths[index], modified[index], fileBytes[index]);
							if (!document) {
								valid = false;
								break;
							}
							candidate.Animated[index] = NeedsFrameSamples(document->Authored);
							Entry &entry = Entries[Key(owner, skyNames[index])];
							if (entry.StoreIdentity != store.Identity() ||
								entry.Modified != modified[index] || entry.FileBytes != fileBytes[index] ||
								!SameSelector(entry.Selector, skyFaces[index].Selector)) {
								for (const auto name : entry.HostCaptures)
									renderer.CancelComposerCapture(owner, name);
								entry.HostCaptures.clear();
								entry.HostObservations.Clear();
								entry.Feedback.Clear();
								if (entry.LuaHost) entry.LuaHost->Reset();
							}
							if (!entry.LuaHost) entry.LuaHost = LuaHostFor(document->Authored);
							LuaMessageDrain drain{entry.LuaHost.get()};
							ArgumentProvider argumentProvider(Arguments, entry.LuaHost.get());
							entry.HostObservations.BeginAttempt();
							detail::ComposerProvider composerProvider(
								renderer,
								owner,
								&argumentProvider,
								engine::core::Name(
									"client.imagegraph/" + std::string(skyNames[index].Text())
								),
								&entry.HostCaptures,
								&entry.HostObservations
							);
							entry.Owner = owner;
							entry.StoreIdentity = store.Identity();
							entry.Selector = candidate.Selectors[index];
							entry.Modified = candidate.Modified[index];
							entry.FileBytes = candidate.FileBytes[index];
							std::string_view port;
							const auto *node =
								OutputNode(document->Authored, skyFaces[index].Selector.Output, port);
							engine::imagegraph::Diagnostic diagnostic;
							const bool camera =
								node && detail::SourceCameraSingletonCone(document->Authored, *node);
							const bool sdf =
								node &&
								(node->Type == "pc.rm_render" || node->Type == "pc.rm_render_scatter" ||
								 node->Type == "pc.rm_cloud" || node->Type == "pc.rm_terrain" ||
								 node->Type == "pc.rm_primitive" || node->Type == "pc.rm_combine");
							if (camera) {
								engine::render::imagegraph::SourceCamera3DRequest face;
								valid = detail::BuildCameraRequest(
									document->Authored,
									document->Compiled,
									*node,
									port,
									candidate.Ticks[index],
									skyFaces[index].Selector.Seed,
									true,
									face,
									diagnostic,
									&composerProvider,
									&entry.Feedback,
									1,
									&Fonts,
									nullptr
								);
								if (valid) {
									engine::render::imagegraph::SourceSkyboxFace pendingFace =
										std::move(face);
									engine::render::imagegraph::SourceSkyboxFaceInfo info;
									valid =
										engine::render::imagegraph::InspectSourceSkyboxFace(
											pendingFace, info, Error
										) == engine::render::imagegraph::SourceSkyboxStatus::Pending &&
										info.RetainedBytes <=
											engine::render::imagegraph::
													MAXIMUM_TRANSFORM_IMAGE_3D_OUTPUT_BYTES -
												retainedBytes &&
										info.OutputBytes <=
											engine::render::imagegraph::MAXIMUM_SOURCE_SKYBOX_OUTPUT_BYTES -
												outputBytes;
									if (valid) {
										retainedBytes += info.RetainedBytes;
										outputBytes += info.OutputBytes;
										request.Faces[index] = std::move(pendingFace);
									} else if (Error.empty())
										Error = "skybox group exceeds source or output byte cap";
								}
							} else if (sdf) {
								engine::render::imagegraph::SourceSdfRequest face;
								valid = detail::BuildSdfRequest(
									document->Authored,
									document->Compiled,
									*node,
									port,
									candidate.Ticks[index],
									skyFaces[index].Selector.Seed,
									true,
									face,
									diagnostic,
									&composerProvider,
									&entry.Feedback,
									1,
									&Fonts,
									nullptr
								);
								if (valid) {
									engine::render::imagegraph::SourceSkyboxFace pendingFace =
										std::move(face);
									engine::render::imagegraph::SourceSkyboxFaceInfo info;
									valid =
										engine::render::imagegraph::InspectSourceSkyboxFace(
											pendingFace, info, Error
										) == engine::render::imagegraph::SourceSkyboxStatus::Pending &&
										info.RetainedBytes <=
											engine::render::imagegraph::
													MAXIMUM_TRANSFORM_IMAGE_3D_OUTPUT_BYTES -
												retainedBytes &&
										info.OutputBytes <=
											engine::render::imagegraph::MAXIMUM_SOURCE_SKYBOX_OUTPUT_BYTES -
												outputBytes;
									if (valid) {
										retainedBytes += info.RetainedBytes;
										outputBytes += info.OutputBytes;
										request.Faces[index] = std::move(pendingFace);
									} else if (Error.empty())
										Error = "skybox group exceeds source or output byte cap";
								}
							} else {
								auto frame = EvaluateCompiled(
									document->Authored,
									document->Compiled,
									skyFaces[index].Selector.Output,
									candidate.Ticks[index],
									skyFaces[index].Selector.Seed,
									&entry.Feedback,
									&composerProvider,
									nullptr,
									&Fonts,
									nullptr
								);
								const auto format = detail::TextureFormatForSurface(frame.Image.Format);
								valid = frame.Status == engine::imagegraph::Status::Ok &&
										frame.FlipbookSide == 0 && format &&
										engine::imagegraph::FiniteSurfaceSamples(frame.Image);
								if (valid) {
									engine::assets::TextureData face;
									face.Width = frame.Image.Width;
									face.Height = frame.Image.Height;
									face.Format = *format == engine::assets::TextureFormat::RGBA4_UNORM
													  ? engine::assets::TextureFormat::RGBA4_SRGB
													  : *format;
									if (frame.Image.Pixels.size() >
										engine::render::imagegraph::MAXIMUM_TRANSFORM_IMAGE_3D_OUTPUT_BYTES) {
										valid = false;
										Error = "skybox CPU face exceeds source byte cap";
										break;
									}
									const auto stride =
										detail::TextureUploadBytesPerPixel(frame.Image.Format);
									const uint64_t sourceBytes = frame.Image.Pixels.size(),
												   uploadBytes = uint64_t(face.Width) * face.Height * stride;
									if (!stride ||
										sourceBytes > engine::render::imagegraph::
															  MAXIMUM_TRANSFORM_IMAGE_3D_OUTPUT_BYTES -
														  retainedBytes ||
										uploadBytes >
											engine::render::imagegraph::MAXIMUM_SOURCE_SKYBOX_OUTPUT_BYTES -
												outputBytes) {
										valid = false;
										Error = "skybox group exceeds source or output byte cap";
										break;
									}
									retainedBytes += sourceBytes;
									outputBytes += uploadBytes;
									face.Pixels.assign(
										reinterpret_cast<const std::byte *>(frame.Image.Pixels.data()),
										reinterpret_cast<const std::byte *>(
											frame.Image.Pixels.data() + frame.Image.Pixels.size()
										)
									);
									engine::core::Metrics::Count(
										"imagegraph.skybox CPU copied bytes", sourceBytes
									);
									request.Faces[index] = std::move(face);
								}
								diagnostic = frame.Diagnostic;
							}
							if (!valid) {
								waitingForCapture = composerProvider.Pending;
								if (!waitingForCapture) {
									cancelSkyboxCaptures(candidate);
									candidate.Work.Failed = true;
									candidate.Admission.reset();
								}
								if (!diagnostic.Message.empty())
									Error = diagnostic.Message;
								else if (Error.empty())
									Error = "skybox selected output is not a supported still image";
								break;
							}
							const auto binding = Publisher.BeginBinding(owner, skyNames[index]);
							if (!binding) {
								Error = "skybox binding generation unavailable";
								valid = false;
								break;
							}
							entry.Publication = *binding;
							entry.Owner = owner;
							if (NextTransformGeneration == 0) {
								valid = false;
								Error = "skybox source generation space exhausted";
								break;
							}
							const uint64_t generation = NextTransformGeneration;
							NextTransformGeneration =
								generation == std::numeric_limits<uint64_t>::max() ? 0 : generation + 1;
							request.Targets[index] = {skyNames[index], generation};
							if (entry.TransformAdmitted) {
								(void)renderer.CancelTransformImage3D(
									owner, skyNames[index], entry.TransformGeneration
								);
								entry.TransformAdmitted = false;
							}

							candidate.Selectors[index] = skyFaces[index].Selector;
							candidate.Entities[index] = skyFaces[index].Entity;
							candidate.Modified[index] = modified[index];
							candidate.FileBytes[index] = fileBytes[index];
							entry.HostObservations.Clear();
							for (const auto name : entry.HostCaptures)
								renderer.CancelComposerCapture(owner, name);
							entry.HostCaptures.clear();
							candidate.PreparedFaces = index + 1;
						}
						if (valid && engine::render::imagegraph::BeginSourceSkybox(
										 renderer, candidate.Work, std::move(request), Error
									 ) == engine::render::imagegraph::SourceSkyboxStatus::Pending) {
							TransformOwners.try_emplace(owner.Id(), owner);
						} else if (valid) {
							cancelSkyboxCaptures(candidate);
							candidate.Work.Failed = true;
							valid = false;
						}
						if (!valid && candidate.Admission && !waitingForCapture) {
							cancelSkyboxCaptures(candidate);
							candidate.Work.Failed = true;
						}
						if (candidate.PreparedFaces == 6 || candidate.Work.Failed)
							candidate.Admission.reset();
					}
					if (valid && pending != PendingSkyboxes.end() && !pending->second.Admission) {
						// Ignore advancing world ticks while this frozen six-face cohort
						// finishes, avoiding perpetual cancellation.
						const auto status = engine::render::imagegraph::RefreshSourceSkybox(
							renderer, pending->second.Work, Error
						);
						if (status == engine::render::imagegraph::SourceSkyboxStatus::Published) {
							for (size_t index = 0; index < 6; ++index) {
								auto &entry = Entries[Key(owner, skyNames[index])];
								entry.Owner = owner;
								entry.Entity = pending->second.Entities[index];
								entry.StoreIdentity = store.Identity();
								entry.Selector = pending->second.Selectors[index];
								entry.Tick = pending->second.Ticks[index];
								entry.Modified = pending->second.Modified[index];
								entry.FileBytes = pending->second.FileBytes[index];
								entry.Published = true;
								entry.Animated = pending->second.Animated[index];
							}
							SkyboxGroups[owner.Id()] = {skyNames, store.Identity()};
							updated += 6;
							PendingSkyboxes.erase(pending);
						}
					}

				} else if (valid && changed) {
					std::array<ImageGraphFrameResult, 6> frames;
					size_t totalUploadBytes = 0;
					for (size_t index = 0; index < skyFaces.size(); ++index) {
						CachedDocument *document =
							compiledDocument(paths[index], modified[index], fileBytes[index]);
						if (document == nullptr) {
							valid = false;
							break;
						}
						Entry &feedbackEntry = Entries[Key(owner, skyNames[index])];
						if (feedbackEntry.StoreIdentity != store.Identity() ||
							feedbackEntry.Modified != modified[index] ||
							feedbackEntry.FileBytes != fileBytes[index] ||
							!SameSelector(feedbackEntry.Selector, skyFaces[index].Selector)) {
							feedbackEntry.Feedback.Clear();
							if (feedbackEntry.LuaHost) feedbackEntry.LuaHost->Reset();
						}
						if (!feedbackEntry.LuaHost) feedbackEntry.LuaHost = LuaHostFor(document->Authored);
						LuaMessageDrain drain{feedbackEntry.LuaHost.get()};
						frames[index] = EvaluateCompiled(
							document->Authored,
							document->Compiled,
							skyFaces[index].Selector.Output,
							ticks[index],
							skyFaces[index].Selector.Seed,
							&feedbackEntry.Feedback,
							feedbackEntry.LuaHost.get(),
							nullptr,
							&Fonts,
							nullptr
						);
						const auto uploadFormat = detail::TextureFormatForSurface(frames[index].Image.Format);
						const uint32_t uploadBytesPerPixel =
							detail::TextureUploadBytesPerPixel(frames[index].Image.Format);
						const uint64_t pixels =
							uint64_t(frames[index].Image.Width) * frames[index].Image.Height;
						const bool exceedsUploadLimit =
							!uploadFormat || uploadBytesPerPixel == 0 ||
							pixels > std::numeric_limits<uint64_t>::max() / uploadBytesPerPixel ||
							pixels * uploadBytesPerPixel > 96u * 1024u * 1024u - totalUploadBytes;
						if (frames[index].Status != engine::imagegraph::Status::Ok ||
							frames[index].FlipbookSide != 0 || exceedsUploadLimit) {
							Error = frames[index].Status == engine::imagegraph::Status::Ok
										? "image graph skybox exceeds group byte limit"
										: frames[index].Diagnostic.Message;
							valid = false;
							break;
						}
						totalUploadBytes += static_cast<size_t>(pixels * uploadBytesPerPixel);
					}
					if (valid) {
						std::array<engine::render::LiveImageUpload, 6> uploads;
						for (size_t index = 0; index < skyFaces.size(); ++index) {
							const auto textureFormat =
								detail::TextureFormatForSurface(frames[index].Image.Format);
							if (!textureFormat) {
								Error = "image graph skybox surface format is unsupported";
								valid = false;
								break;
							}
							const auto binding = Publisher.BeginBinding(owner, skyNames[index]);
							if (!binding) {
								Error = "live image publisher has no skybox binding capacity";
								valid = false;
								break;
							}
							auto &entry = Entries[Key(owner, skyNames[index])];
							entry.Publication = *binding;
							uploads[index] = {
								*binding,
								frames[index].Image.Width,
								frames[index].Image.Height,
								std::span<const std::byte>(
									reinterpret_cast<const std::byte *>(frames[index].Image.Pixels.data()),
									frames[index].Image.Pixels.size()
								),
								skyFaces[index].Selector.ColorSpace ==
										engine::scene::ImageGraphColorSpace::Linear
									? engine::render::LiveImageColorSpace::Linear
									: engine::render::LiveImageColorSpace::Display,
								*textureFormat
							};
						}
						if (valid && Publisher.PublishBatch(renderer, uploads) !=
										 engine::render::LiveImagePublishStatus::Published) {
							Error = "image graph skybox texture batch upload failed";
							valid = false;
						}
						if (valid) {
							for (size_t index = 0; index < skyFaces.size(); ++index) {
								auto &entry = Entries[Key(owner, skyNames[index])];
								entry.Owner = owner;
								entry.Entity = skyFaces[index].Entity;
								entry.StoreIdentity = store.Identity();
								entry.Selector = skyFaces[index].Selector;
								entry.Tick = ticks[index];
								entry.Modified = modified[index];
								entry.FileBytes = fileBytes[index];
								entry.Published = true;
								entry.Animated = frames[index].Animated;
							}
							SkyboxGroups[owner.Id()] = {skyNames, store.Identity()};
							updated += skyFaces.size();
						}
					}
				}
			}
		}
		if (PrioritySkyboxOwner == owner) PrioritySkyboxVisited = true;
		store.Each<const engine::scene::ImageGraphBinding>([&](engine::ecs::Entity entity,
															   const engine::scene::ImageGraphBinding
																   &selector) {
			if (groupedNames.contains(selector.Texture.Id())) return;
			if (!engine::scene::IsValidImageGraphBinding(selector)) return;
			const size_t current = ordinal++;
			const uint64_t key = Key(owner, selector.Texture);
			if (!seen.insert(key).second) {
				Error = "duplicate image graph texture binding";
				return;
			}
			if (!usage.Valid) {
				Error = "image graph sink reference limit exceeded";
				return;
			}
			const uint8_t sinkFlags = usage.Flags[selector.Texture.Id()];
			const uint8_t incompatible =
				selector.ColorSpace == engine::scene::ImageGraphColorSpace::Linear ? 1 : 2;
			if ((sinkFlags & incompatible) != 0) {
				Error = "image graph output colour space conflicts with a texture sink";
				return;
			}
			// Scan every row for retirement, but only touch a bounded number of
			// files and evaluations on this presentation. The cursor rotates.
			if (current < nextBinding || ChecksRemaining == 0 ||
				(PrioritySkyboxOwner.IsValid() && !PrioritySkyboxVisited && PrioritySkyboxOwner != owner &&
				 ChecksRemaining <= 6))
				return;
			--ChecksRemaining;
			nextBinding = current + 1;
			const std::filesystem::path path = ImageGraphDocumentPath(directory, selector.Graph);
			if (path.empty()) {
				Error = "unsafe image graph document name";
				return;
			}
			std::error_code error;
			const auto modified = std::filesystem::last_write_time(path, error);
			if (error) {
				Error = "image graph document is unavailable";
				return;
			}
			const uintmax_t fileBytes = std::filesystem::file_size(path, error);
			if (error || fileBytes > MAXIMUM_DOCUMENT_BYTES) {
				Error = "image graph document exceeds host byte limit or is unavailable";
				return;
			}
			const uint64_t tick = selector.TickPolicy == engine::scene::ImageGraphTickPolicy::World
									  ? store.Time().Tick
									  : selector.FixedTick;
			CachedDocument *cached = compiledDocument(path, modified, fileBytes);
			if (cached == nullptr) return;
			auto [position, inserted] = Entries.try_emplace(key);
			Entry &entry = position->second;
			const bool changed = inserted || entry.StoreIdentity != store.Identity() ||
								 entry.Entity != entity || !SameSelector(entry.Selector, selector);
			const bool sourceChanged = changed || entry.Modified != modified || entry.FileBytes != fileBytes;
			if (sourceChanged) {
				for (const auto name : entry.HostCaptures)
					renderer.CancelComposerCapture(entry.Owner, name);
				entry.HostCaptures.clear();
				entry.PendingHostTick.reset();
				entry.HostObservations.Clear();
				entry.Feedback.Clear();
				if (entry.LuaHost) entry.LuaHost->Reset();
			}
			if (!HasLuaNodes(cached->Authored))
				entry.LuaHost.reset();
			else if (!entry.LuaHost)
				entry.LuaHost = LuaHostFor(cached->Authored);
			LuaMessageDrain drain{entry.LuaHost.get()};
			const uint64_t evaluationTick = entry.PendingHostTick.value_or(tick);
			ArgumentProvider argumentProvider(Arguments, entry.LuaHost.get());
			entry.HostObservations.BeginAttempt();
			detail::ComposerProvider composerProvider(
				renderer,
				owner,
				&argumentProvider,
				engine::core::Name("client.imagegraph/" + std::string(selector.Texture.Text())),
				&entry.HostCaptures,
				&entry.HostObservations
			);
			struct HostCadence {
				Entry *Target;
				detail::ComposerProvider &Provider;
				uint64_t Tick;
				std::filesystem::file_time_type Modified;
				uintmax_t FileBytes;
				engine::core::Name Owner;
				engine::ecs::Entity Entity;
				uint64_t StoreIdentity;
				const engine::scene::ImageGraphBinding &Selector;
				void Disarm() {
					Target = nullptr;
				}
				~HostCadence() {
					if (!Target) return;
					if (Provider.Pending) {
						Target->PendingHostTick = Tick;
						Target->Modified = Modified;
						Target->FileBytes = FileBytes;
						Target->Owner = Owner;
						Target->Entity = Entity;
						Target->StoreIdentity = StoreIdentity;
						Target->Selector = Selector;
					} else {
						Target->PendingHostTick.reset();
						Target->HostObservations.Clear();
					}
				}
			} cadence{
				&entry,
				composerProvider,
				evaluationTick,
				modified,
				fileBytes,
				owner,
				entity,
				store.Identity(),
				selector
			};
			bool sampleChanged = changed || entry.Modified != modified || entry.FileBytes != fileBytes ||
								 (entry.Animated && entry.Tick != tick) || entry.PendingHostTick.has_value();
			std::string_view outputPort;
			const auto *outputNode = OutputNode(cached->Authored, selector.Output, outputPort);
			const bool sourceComposer = outputNode && outputNode->Type == "pc.hlsl";
			if (sourceComposer &&
				renderer.ComposerShaderRevision(owner, detail::ComposerAsset(*outputNode)) == 0) {
				engine::assets::ShaderData artifact;
				const auto asset = detail::ComposerAsset(*outputNode);
				if (!engine::render::hlsl::ReadArtifact(directory, asset, artifact)) {
					if (const auto failure = renderer.InstallComposerShader(owner, asset, artifact))
						Error = *failure;
				}
			}
			const uint64_t composerRevision =
				sourceComposer ? renderer.ComposerShaderRevision(owner, detail::ComposerAsset(*outputNode))
							   : 0;
			sampleChanged = sampleChanged || (sourceComposer && entry.ComposerRevision != composerRevision);
			const bool cameraOutput = outputNode && (outputNode->Type == "pc.3_d_camera" ||
													 outputNode->Type == "pc.3_d_camera_set");
			const bool sourceCamera =
				cameraOutput && detail::SourceCameraSingletonCone(cached->Authored, *outputNode);
			if (cameraOutput && !sourceCamera && outputPort != "rendered" && outputPort != "diffuse" &&
				selector.ColorSpace != engine::scene::ImageGraphColorSpace::Linear) {
				Error = "source camera numeric output requires linear colour space";
				if (entry.TransformAdmitted) {
					(void)renderer.CancelTransformImage3D(owner, selector.Texture, entry.TransformGeneration);
					entry.TransformAdmitted = false;
				}
				return;
			}
			const bool sourceSdf =
				outputNode &&
				(outputNode->Type == "pc.rm_render" || outputNode->Type == "pc.rm_render_scatter" ||
				 outputNode->Type == "pc.rm_cloud" || outputNode->Type == "pc.rm_terrain" ||
				 outputNode->Type == "pc.rm_primitive" || outputNode->Type == "pc.rm_combine");
			if (outputNode != nullptr &&
				(outputNode->Type == "image.transform_3d" || sourceCamera || sourceSdf || sourceComposer)) {
				if ((!sourceCamera && !sourceSdf && !sourceComposer && outputPort != "rendered" &&
					 outputPort != "depth") ||
					(sourceSdf && outputPort != "surface_out") ||
					(sourceComposer && outputPort != "surface")) {
					Error = "live Transform Image 3D texture bindings require rendered or "
							"depth output";
					if (entry.TransformAdmitted) {
						(void)renderer.CancelTransformImage3D(
							owner, selector.Texture, entry.TransformGeneration
						);
						entry.TransformAdmitted = false;
					}
					return;
				}
				if ((outputPort == "depth" ||
					 (sourceCamera && outputPort != "rendered" && outputPort != "diffuse")) &&
					selector.ColorSpace != engine::scene::ImageGraphColorSpace::Linear) {
					Error = "Transform Image 3D depth output requires linear colour space";
					if (entry.TransformAdmitted) {
						(void)renderer.CancelTransformImage3D(
							owner, selector.Texture, entry.TransformGeneration
						);
						entry.TransformAdmitted = false;
					}
					return;
				}
				if (!sampleChanged && entry.TransformAdmitted) return;
				if (entry.TransformAdmitted) {
					(void)renderer.CancelTransformImage3D(owner, selector.Texture, entry.TransformGeneration);
					entry.TransformAdmitted = false;
				}
				if (NextTransformGeneration == 0) {
					Error = "Transform Image 3D generation limit exhausted";
					return;
				}
				engine::render::imagegraph::TransformImage3DRequest request;
				engine::render::imagegraph::SourceCamera3DRequest cameraRequest;
				engine::render::imagegraph::SourceSdfRequest sdfRequest;
				engine::render::hlsl::SurfaceRequest composerRequest;
				engine::imagegraph::Diagnostic diagnostic;
				const bool built =
					sourceComposer ? detail::BuildComposerRequest(
										 cached->Authored,
										 cached->Compiled,
										 *outputNode,
										 renderer,
										 owner,
										 evaluationTick,
										 selector.Seed,
										 selector.ColorSpace == engine::scene::ImageGraphColorSpace::Display,
										 composerRequest,
										 diagnostic,
										 &composerProvider,
										 &entry.Feedback,
										 1,
										 &Fonts,
										 nullptr
									 )
					: sourceSdf ? detail::BuildSdfRequest(
									  cached->Authored,
									  cached->Compiled,
									  *outputNode,
									  outputPort,
									  evaluationTick,
									  selector.Seed,
									  selector.ColorSpace == engine::scene::ImageGraphColorSpace::Display,
									  sdfRequest,
									  diagnostic,
									  &composerProvider,
									  &entry.Feedback,
									  1,
									  &Fonts,
									  nullptr
								  )
					: sourceCamera ? detail::BuildCameraRequest(
										 cached->Authored,
										 cached->Compiled,
										 *outputNode,
										 outputPort,
										 evaluationTick,
										 selector.Seed,
										 selector.ColorSpace == engine::scene::ImageGraphColorSpace::Display,
										 cameraRequest,
										 diagnostic,
										 &composerProvider,
										 &entry.Feedback,
										 1,
										 &Fonts,
										 nullptr
									 )
								   : detail::BuildTransformRequest(
										 cached->Authored,
										 cached->Compiled,
										 *outputNode,
										 evaluationTick,
										 selector.Seed,
										 selector.ColorSpace == engine::scene::ImageGraphColorSpace::Display,
										 request,
										 diagnostic,
										 &composerProvider,
										 &entry.Feedback,
										 1,
										 &Fonts,
										 nullptr
									 );
				if (!built) {
					Error = diagnostic.Message;
					return;
				}

				if (sourceSdf && !sdfRequest.Render && !changed && entry.TransformGeneration != 0) {
					// Source preview render switches retain the last completed surface.
					entry.Tick = evaluationTick;
					entry.Modified = modified;
					entry.FileBytes = fileBytes;
					entry.Animated = NeedsFrameSamples(cached->Authored);
					entry.TransformAdmitted = true;
					return;
				}
				if (!TransformOwners.contains(owner.Id()) &&
					TransformOwners.size() >= engine::render::LiveImagePublisher::MAXIMUM_BINDINGS) {
					Error = "live Transform Image 3D owner limit exceeded";
					return;
				}
				bool ownerInserted = false;
				try {
					ownerInserted = TransformOwners.try_emplace(owner.Id(), owner).second;
				} catch (const std::bad_alloc &) {
					Error = "Transform Image 3D owner tracking allocation failed";
					return;
				}
				const uint64_t generation = NextTransformGeneration;
				const auto queueStatus =
					sourceComposer ? renderer.QueueComposerSurface(
										 {.Owner = owner,
										  .Name = selector.Texture,
										  .Generation = generation,
										  .Request = std::move(composerRequest)}
									 )
					: sourceSdf ? renderer.QueueSourceSdf(
									  {.Owner = owner,
									   .Name = selector.Texture,
									   .Generation = generation,
									   .Request = std::move(sdfRequest)}
								  )
					: sourceCamera
						? renderer.QueueSourceCamera3D(
							  {.Owner = owner,
							   .Name = selector.Texture,
							   .Generation = generation,
							   .Request = std::move(cameraRequest)}
						  )
						: renderer.QueueTransformImage3D({
							  .Owner = owner,
							  .Name = selector.Texture,
							  .Generation = generation,
							  .Output = outputPort == "depth"
											? engine::render::imagegraph::TransformImage3DOutput::Depth
											: engine::render::imagegraph::TransformImage3DOutput::Rendered,
							  .Request = std::move(request),
						  });
				if (queueStatus != engine::render::imagegraph::TransformImage3DQueueResult::Queued &&
					queueStatus != engine::render::imagegraph::TransformImage3DQueueResult::Replaced) {
					if (ownerInserted) TransformOwners.erase(owner.Id());
					if (queueStatus == engine::render::imagegraph::TransformImage3DQueueResult::Full &&
						!retryBinding)
						retryBinding = current;
					Error = queueStatus == engine::render::imagegraph::TransformImage3DQueueResult::Full
								? "live Transform Image 3D queue is full"
								: "live Transform Image 3D request was refused";
					return;
				}
				const engine::core::Name previousTexture = entry.Selector.Texture;
				if (entry.Publication.Owner.IsValid()) {
					if (!Publisher.ReleaseBinding(entry.Publication)) {
						(void)renderer.CancelTransformImage3D(owner, selector.Texture, generation);
						if (ownerInserted) TransformOwners.erase(owner.Id());
						Error = "live image publisher binding became stale during transform "
								"handoff";
						return;
					}
					entry.Publication = {};
				}
				if (entry.Owner == owner && previousTexture.IsValid() &&
					previousTexture != selector.Texture) {
					const bool sharedByAnotherBinding =
						std::any_of(Entries.begin(), Entries.end(), [&](const auto &candidate) {
							return candidate.first != key && candidate.second.Owner == owner &&
								   candidate.second.Selector.Texture == previousTexture;
						});
					if (!sharedByAnotherBinding) (void)renderer.DropTexture(previousTexture, owner);
				}
				NextTransformGeneration =
					generation == std::numeric_limits<uint64_t>::max() ? 0 : generation + 1;
				entry.Owner = owner;
				entry.Entity = entity;
				entry.StoreIdentity = store.Identity();
				entry.Selector = selector;
				entry.Tick = evaluationTick;
				entry.Modified = modified;
				entry.FileBytes = fileBytes;
				entry.Published = false;
				entry.Animated = NeedsFrameSamples(cached->Authored);
				entry.TransformAdmitted = true;
				entry.TransformGeneration = generation;
				entry.ComposerRevision = composerRevision;
				++updated;
				return;
			}
			if (entry.TransformAdmitted) {
				(void)renderer.CancelTransformImage3D(owner, selector.Texture, entry.TransformGeneration);
				entry.TransformAdmitted = false;
			}
			if (entry.TransformGeneration != 0 && entry.Selector.Texture != selector.Texture) {
				const bool sharedByAnotherBinding =
					std::any_of(Entries.begin(), Entries.end(), [&](const auto &candidate) {
						return candidate.first != key && candidate.second.Owner == owner &&
							   candidate.second.Selector.Texture == entry.Selector.Texture;
					});
				if (!sharedByAnotherBinding) (void)renderer.DropTexture(entry.Selector.Texture, owner);
				entry.TransformGeneration = 0;
			}
			if (!sampleChanged && entry.Published) return;
			const bool needsBinding = changed || !entry.Publication.Owner.IsValid();
			if (needsBinding) {
				if (!inserted && entry.StoreIdentity != store.Identity())
					(void)Publisher.Retire(renderer, entry.Publication);
				const auto binding = Publisher.BeginBinding(owner, selector.Texture);
				if (!binding) {
					Error = "live image publisher has no binding capacity";
					if (inserted) {
						cadence.Disarm();
						Entries.erase(position);
					}
					return;
				}
				entry.Publication = *binding;
				entry.Owner = owner;
				entry.Entity = entity;
				entry.StoreIdentity = store.Identity();
				entry.Selector = selector;
				entry.Published = false;
			}
			ImageGraphFrameResult frame = EvaluateCompiled(
				cached->Authored,
				cached->Compiled,
				selector.Output,
				evaluationTick,
				selector.Seed,
				&entry.Feedback,
				&composerProvider,
				nullptr,
				&Fonts,
				nullptr
			);
			if (frame.Status != engine::imagegraph::Status::Ok) {
				Error = frame.Diagnostic.Message;
				return;
			}
			const std::span<const std::byte> pixels(
				reinterpret_cast<const std::byte *>(frame.Image.Pixels.data()), frame.Image.Pixels.size()
			);
			const auto textureFormat = detail::TextureFormatForSurface(frame.Image.Format);
			if (!textureFormat) {
				Error = "image graph surface format is unsupported";
				return;
			}
			const auto status = Publisher.Publish(
				renderer,
				entry.Publication,
				frame.Image.Width,
				frame.Image.Height,
				pixels,
				selector.ColorSpace == engine::scene::ImageGraphColorSpace::Linear
					? engine::render::LiveImageColorSpace::Linear
					: engine::render::LiveImageColorSpace::Display,
				*textureFormat,
				frame.FlipbookSide,
				frame.FrameDurations
			);
			if (status != engine::render::LiveImagePublishStatus::Published) {
				Error = "image graph texture upload failed";
				return;
			}
			{
				engine::scene::FlipbookFacts facts;
				facts.Side = frame.FlipbookSide;
				facts.Frames = static_cast<uint16_t>(frame.FrameDurations.size());
				facts.FrameDurations = frame.FrameDurations;
				(void)engine::scene::RecordTexture(store, selector.Texture, facts);
			}
			entry.Tick = evaluationTick;
			entry.Modified = modified;
			entry.FileBytes = fileBytes;
			entry.Published = true;
			entry.Animated = frame.Animated;
			entry.TransformAdmitted = false;
			entry.TransformGeneration = 0;
			++updated;
		});
		if (retryBinding)
			nextBinding = *retryBinding;
		else if (nextBinding >= ordinal)
			nextBinding = 0;
		for (auto entry = Entries.begin(); entry != Entries.end();) {
			if (entry->second.Owner == owner && !seen.contains(entry->first)) {
				for (const auto name : entry->second.HostCaptures)
					renderer.CancelComposerCapture(owner, name);
				if (entry->second.TransformAdmitted)
					(void)renderer.CancelTransformImage3D(
						owner, entry->second.Selector.Texture, entry->second.TransformGeneration
					);
				if (entry->second.TransformGeneration != 0)
					(void)renderer.DropTexture(entry->second.Selector.Texture, owner);
				(void)Publisher.Retire(renderer, entry->second.Publication);
				entry = Entries.erase(entry);
			} else {
				++entry;
			}
		}
		return updated;
	}

	void ImageGraphRuntime::RetireInactiveOwners(
		engine::render::Renderer &renderer, std::span<const engine::core::Name> owners
	) {
		for (auto entry = Entries.begin(); entry != Entries.end();) {
			if (std::find(owners.begin(), owners.end(), entry->second.Owner) != owners.end()) {
				++entry;
				continue;
			}
			for (const auto name : entry->second.HostCaptures)
				renderer.CancelComposerCapture(entry->second.Owner, name);
			if (entry->second.TransformAdmitted)
				(void)renderer.CancelTransformImage3D(
					entry->second.Owner, entry->second.Selector.Texture, entry->second.TransformGeneration
				);
			if (entry->second.TransformGeneration != 0)
				(void)renderer.DropTexture(entry->second.Selector.Texture, entry->second.Owner);
			(void)Publisher.Retire(renderer, entry->second.Publication);
			entry = Entries.erase(entry);
		}
		for (auto transformOwner = TransformOwners.begin(); transformOwner != TransformOwners.end();) {
			if (std::find(owners.begin(), owners.end(), transformOwner->second) != owners.end()) {
				++transformOwner;
				continue;
			}
			renderer.DropTransformImage3DOwner(transformOwner->second);
			transformOwner = TransformOwners.erase(transformOwner);
		}
		for (auto &scan : ComposerDemandScans)
			if (std::find(owners.begin(), owners.end(), scan.Owner) == owners.end()) scan = {};
		for (auto cursor = NextBindingByOwner.begin(); cursor != NextBindingByOwner.end();) {
			if (std::find_if(owners.begin(), owners.end(), [&](engine::core::Name owner) {
					return owner.Id() == cursor->first;
				}) == owners.end())
				cursor = NextBindingByOwner.erase(cursor);
			else
				++cursor;
		}
		for (auto group = PendingSkyboxes.begin(); group != PendingSkyboxes.end();) {
			if (std::none_of(owners.begin(), owners.end(), [&](auto owner) {
					return owner.Id() == group->first;
				})) {
				engine::render::imagegraph::CancelSourceSkybox(renderer, group->second.Work);
				group = PendingSkyboxes.erase(group);
			} else
				++group;
		}
		for (auto group = SkyboxGroups.begin(); group != SkyboxGroups.end();) {
			if (std::find_if(owners.begin(), owners.end(), [&](engine::core::Name owner) {
					return owner.Id() == group->first;
				}) == owners.end())
				group = SkyboxGroups.erase(group);
			else
				++group;
		}
	}

	void ImageGraphRuntime::Clear(engine::render::Renderer &renderer) {
		for (auto &[_, group] : PendingSkyboxes)
			engine::render::imagegraph::CancelSourceSkybox(renderer, group.Work);
		PendingSkyboxes.clear();
		for (const auto &[_, owner] : TransformOwners)
			renderer.DropTransformImage3DOwner(owner);
		TransformOwners.clear();
		NextTransformGeneration = 1;
		for (const auto &[key, entry] : Entries) {
			for (const auto name : entry.HostCaptures)
				renderer.CancelComposerCapture(entry.Owner, name);
			(void)Publisher.Retire(renderer, entry.Publication);
		}
		Entries.clear();
		Documents.clear();
		++ComposerDocumentRevision;
		ComposerDemandScans = {};
		SinkUsages.clear();
		SkyboxGroups.clear();
		SkyboxOwners.clear();
		NextSkyboxOwners.clear();
		PrioritySkyboxOwner = {};
		PrioritySkyboxVisited = false;
		NextSkyboxOwner = 0;
		CachedDocumentBytes = 0;
		NextBindingByOwner.clear();
		Error.clear();
	}
} // namespace client
