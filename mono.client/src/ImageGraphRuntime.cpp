#include <engine/ecs/Store.hpp>
#include <engine/effects/Particles.hpp>
#include <engine/effects/Ribbon.hpp>
#include <engine/gui/Components.hpp>
#include <engine/render/ImageGraphTransform3D.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scene/Atmosphere.hpp>
#include <engine/scene/Components.hpp>

#include <algorithm>
#include <client/ContentDemand.hpp>
#include <client/ImageGraphRuntime.hpp>
#include <fstream>
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

		ImageGraphFrameResult EvaluateCompiled(
			const engine::imagegraph::Document &document,
			const engine::imagegraph::Plan &plan,
			engine::core::Name output,
			uint64_t tick,
			uint64_t seed
		) {
			ImageGraphFrameResult result;
			result.Animated = !document.Keyframes.empty();
			const auto selected =
				std::find_if(document.Outputs.begin(), document.Outputs.end(), [&](const auto &item) {
					return item.Id == output.Text();
				});
			if (selected != document.Outputs.end()) {
				const auto node =
					std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &item) {
						return item.Id == selected->NodeId;
					});
				if (node != document.Nodes.end() && node->Type == "image.transform_3d") {
					result.Status = engine::imagegraph::Status::UnsupportedExecution;
					result.Diagnostic = {
						result.Status,
						node->Id,
						selected->Port,
						"Transform Image 3D requires the headless export scheduler"
					};
					return result;
				}
			}
			result.Status = engine::imagegraph::Evaluate(
				document,
				plan,
				std::string(output.Text()),
				engine::imagegraph::EvaluationRequest{.Tick = tick, .Seed = seed},
				result.Image,
				result.Diagnostic
			);
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

		bool EvaluateTransformInput(
			const engine::imagegraph::Document &document,
			const engine::imagegraph::Plan &plan,
			const engine::imagegraph::Node &transform,
			std::string_view port,
			uint64_t tick,
			uint64_t seed,
			engine::imagegraph::Image &image,
			engine::imagegraph::Diagnostic &diagnostic
		) {
			const auto link =
				std::find_if(plan.EffectiveLinks.begin(), plan.EffectiveLinks.end(), [&](const auto &item) {
					return item.ToNode == transform.Id && item.ToPort == port;
				});
			if (link == plan.EffectiveLinks.end()) {
				if (port == "back_surface") return true;
				diagnostic = {
					engine::imagegraph::Status::InvalidValue,
					transform.Id,
					std::string(port),
					"Transform Image 3D requires a surface input"
				};
				return false;
			}
			engine::imagegraph::Document source = document;
			source.Outputs = {{"__render_transform_input", link->FromNode, link->FromPort}};
			engine::imagegraph::Plan sourcePlan;
			if (engine::imagegraph::Compile(source, sourcePlan, diagnostic) != engine::imagegraph::Status::Ok)
				return false;
			return engine::imagegraph::Evaluate(
					   source,
					   sourcePlan,
					   "__render_transform_input",
					   engine::imagegraph::EvaluationRequest{.Tick = tick, .Seed = seed},
					   image,
					   diagnostic
				   ) == engine::imagegraph::Status::Ok;
		}

		ImageGraphFrameResult EvaluateTransform(
			const engine::imagegraph::Document &document,
			const engine::imagegraph::Plan &plan,
			const engine::imagegraph::Node &transform,
			std::string_view outputPort,
			engine::render::Renderer &renderer,
			uint64_t tick,
			uint64_t seed
		) {
			using namespace engine;
			ImageGraphFrameResult result;
			result.Animated = !document.Keyframes.empty();
			for (const imagegraph::Keyframe &keyframe : document.Keyframes) {
				if (keyframe.NodeId != transform.Id) continue;
				result.Status = imagegraph::Status::UnsupportedExecution;
				result.Diagnostic = {
					result.Status,
					transform.Id,
					keyframe.Port,
					"animated Transform Image 3D controls require render scheduling"
				};
				return result;
			}
			if (outputPort != "rendered" && outputPort != "depth" && outputPort != "mesh") {
				result.Status = imagegraph::Status::InvalidOutput;
				result.Diagnostic = {
					result.Status, transform.Id, std::string(outputPort), "selected output is not an image"
				};
				return result;
			}
			imagegraph::Image front, back;
			if (!EvaluateTransformInput(
					document, plan, transform, "surface", tick, seed, front, result.Diagnostic
				) ||
				!EvaluateTransformInput(
					document, plan, transform, "back_surface", tick, seed, back, result.Diagnostic
				)) {
				result.Status = result.Diagnostic.Code;
				return result;
			}
			const auto property = [&](std::string_view name) -> const imagegraph::Value * {
				const auto found =
					std::find_if(transform.Values.begin(), transform.Values.end(), [&](const auto &value) {
						return value.Port == name;
					});
				return found == transform.Values.end() ? nullptr : &found->Data;
			};
			const auto *position = std::get_if<imagegraph::Vector3>(property("position"));
			const auto *anchor = std::get_if<imagegraph::Vector3>(property("anchor"));
			const auto *rotation = std::get_if<imagegraph::Quaternion>(property("rotation"));
			const auto *scale = std::get_if<imagegraph::Vector3>(property("scale"));
			const auto *tiling = std::get_if<imagegraph::Vector2>(property("texture_tiling"));
			const auto *projection = std::get_if<imagegraph::EnumValue>(property("projection"));
			const auto *fov = std::get_if<double>(property("fov"));
			const auto *viewRange = std::get_if<imagegraph::Vector2>(property("view_range"));
			const auto *depthRange = std::get_if<imagegraph::Vector2>(property("depth_range"));
			if (!position || !anchor || !rotation || !scale || !tiling || !projection || !fov || !viewRange ||
				!depthRange || (projection->Value != 0 && projection->Value != 1)) {
				result.Status = imagegraph::Status::InvalidValue;
				result.Diagnostic = {
					result.Status, transform.Id, {}, "Transform Image 3D controls are invalid"
				};
				return result;
			}
			render::imagegraph::TransformImage3DRequest request;
			request.Front = {front.Width, front.Height, {}};
			request.Front.Rgba8.assign(
				reinterpret_cast<const std::byte *>(front.Pixels.data()),
				reinterpret_cast<const std::byte *>(front.Pixels.data() + front.Pixels.size())
			);
			if (!back.Pixels.empty()) {
				request.Back = {back.Width, back.Height, {}};
				request.Back.Rgba8.assign(
					reinterpret_cast<const std::byte *>(back.Pixels.data()),
					reinterpret_cast<const std::byte *>(back.Pixels.data() + back.Pixels.size())
				);
			}
			request.Position = {float(position->X), float(position->Y), float(position->Z)};
			request.Anchor = {float(anchor->X), float(anchor->Y), float(anchor->Z)};
			request.Rotation = {
				float(rotation->X), float(rotation->Y), float(rotation->Z), float(rotation->W)
			};
			request.Scale = {float(scale->X), float(scale->Y), float(scale->Z)};
			request.TextureTiling = {float(tiling->X), float(tiling->Y)};
			request.Projection = projection->Value == 0
									 ? render::imagegraph::TransformImage3DProjection::Perspective
									 : render::imagegraph::TransformImage3DProjection::Orthographic;
			request.FieldOfViewDegrees = float(*fov);
			request.ViewRange = {float(viewRange->X), float(viewRange->Y)};
			request.DepthRange = {float(depthRange->X), float(depthRange->Y)};
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
			const std::vector<std::byte> &pixels = outputPort == "depth" ? gpu.DepthRgba8 : gpu.RenderedRgba8;
			result.Image.Width = gpu.Width;
			result.Image.Height = gpu.Height;
			result.Image.Pixels.assign(
				reinterpret_cast<const uint8_t *>(pixels.data()),
				reinterpret_cast<const uint8_t *>(pixels.data() + pixels.size())
			);
			result.Status = imagegraph::Status::Ok;
			return result;
		}

		ImageGraphFrameResult EvaluateForRenderer(
			const engine::imagegraph::Document &document,
			const engine::imagegraph::Plan &plan,
			engine::core::Name output,
			engine::render::Renderer &renderer,
			uint64_t tick,
			uint64_t seed
		) {
			std::string_view port;
			const auto *node = OutputNode(document, output, port);
			if (node != nullptr && node->Type == "image.transform_3d")
				return EvaluateTransform(document, plan, *node, port, renderer, tick, seed);
			return EvaluateCompiled(document, plan, output, tick, seed);
		}
	}

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
		uint64_t seed
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
		return EvaluateCompiled(document, plan, output, tick, seed);
	}

	ImageGraphFrameResult LoadImageGraphRenderExportFrame(
		const std::filesystem::path &directory,
		engine::core::Name graph,
		engine::core::Name output,
		engine::render::Renderer &renderer,
		uint64_t tick,
		uint64_t seed
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
		return EvaluateForRenderer(document, plan, output, renderer, tick, seed);
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
		size_t published = 0;
		size_t ordinal = 0;
		size_t &nextBinding = NextBindingByOwner[owner.Id()];
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
				}
				CachedDocumentBytes += static_cast<size_t>(fileBytes);
				cached = Documents.emplace(pathKey, std::move(prepared)).first;
			} else {
				cached->second.LastUse = ++CacheClock;
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
				if (valid && changed) {
					std::array<ImageGraphFrameResult, 6> frames;
					size_t totalPixels = 0;
					for (size_t index = 0; index < skyFaces.size(); ++index) {
						CachedDocument *document =
							compiledDocument(paths[index], modified[index], fileBytes[index]);
						if (document == nullptr) {
							valid = false;
							break;
						}
						frames[index] = EvaluateCompiled(
							document->Authored,
							document->Compiled,
							skyFaces[index].Selector.Output,
							ticks[index],
							skyFaces[index].Selector.Seed
						);
						if (frames[index].Status != engine::imagegraph::Status::Ok ||
							frames[index].Image.Pixels.size() > 96u * 1024u * 1024u - totalPixels) {
							Error = frames[index].Status == engine::imagegraph::Status::Ok
										? "image graph skybox exceeds group byte limit"
										: frames[index].Diagnostic.Message;
							valid = false;
							break;
						}
						totalPixels += frames[index].Image.Pixels.size();
					}
					if (valid) {
						std::array<engine::render::LiveImageUpload, 6> uploads;
						for (size_t index = 0; index < skyFaces.size(); ++index) {
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
								engine::render::LiveImageColorSpace::Display
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
							published += skyFaces.size();
						}
					}
				}
			}
		}
		if (PrioritySkyboxOwner == owner) PrioritySkyboxVisited = true;
		store.Each<const engine::scene::ImageGraphBinding>(
			[&](engine::ecs::Entity entity, const engine::scene::ImageGraphBinding &selector) {
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
					(PrioritySkyboxOwner.IsValid() && !PrioritySkyboxVisited &&
					 PrioritySkyboxOwner != owner && ChecksRemaining <= 6))
					return;
				--ChecksRemaining;
				nextBinding = current + 1;
				const std::filesystem::path path = ImageGraphDocumentPath(directory, selector.Graph);
				if (path.empty()) {
					Error = "unsafe image graph document name";
					return;
				}
				auto [position, inserted] = Entries.try_emplace(key);
				Entry &entry = position->second;
				const bool changed = inserted || entry.StoreIdentity != store.Identity() ||
									 entry.Entity != entity || !SameSelector(entry.Selector, selector);
				if (changed) {
					if (!inserted && entry.StoreIdentity != store.Identity())
						(void)Publisher.Retire(renderer, entry.Publication);
					const auto binding = Publisher.BeginBinding(owner, selector.Texture);
					if (!binding) {
						Error = "live image publisher has no binding capacity";
						if (inserted) Entries.erase(position);
						return;
					}
					entry.Publication = *binding;
					entry.Entity = entity;
					entry.StoreIdentity = store.Identity();
					entry.Selector = selector;
					entry.Published = false;
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
				if (!changed && entry.Published && entry.Modified == modified &&
					entry.FileBytes == fileBytes && (!entry.Animated || entry.Tick == tick))
					return;
				CachedDocument *cached = compiledDocument(path, modified, fileBytes);
				if (cached == nullptr) return;
				ImageGraphFrameResult frame = EvaluateCompiled(
					cached->Authored, cached->Compiled, selector.Output, tick, selector.Seed
				);
				if (frame.Status != engine::imagegraph::Status::Ok) {
					Error = frame.Diagnostic.Message;
					return;
				}
				const std::span<const std::byte> pixels(
					reinterpret_cast<const std::byte *>(frame.Image.Pixels.data()), frame.Image.Pixels.size()
				);
				const auto status = Publisher.Publish(
					renderer,
					entry.Publication,
					frame.Image.Width,
					frame.Image.Height,
					pixels,
					selector.ColorSpace == engine::scene::ImageGraphColorSpace::Linear
						? engine::render::LiveImageColorSpace::Linear
						: engine::render::LiveImageColorSpace::Display
				);
				if (status != engine::render::LiveImagePublishStatus::Published) {
					Error = "image graph texture upload failed";
					return;
				}
				entry.Tick = tick;
				entry.Modified = modified;
				entry.FileBytes = fileBytes;
				entry.Published = true;
				entry.Animated = frame.Animated;
				++published;
			}
		);
		if (nextBinding >= ordinal) nextBinding = 0;
		for (auto entry = Entries.begin(); entry != Entries.end();) {
			if (entry->second.Publication.Owner == owner && !seen.contains(entry->first)) {
				(void)Publisher.Retire(renderer, entry->second.Publication);
				entry = Entries.erase(entry);
			} else {
				++entry;
			}
		}
		return published;
	}

	void ImageGraphRuntime::RetireInactiveOwners(
		engine::render::Renderer &renderer, std::span<const engine::core::Name> owners
	) {
		for (auto entry = Entries.begin(); entry != Entries.end();) {
			if (std::find(owners.begin(), owners.end(), entry->second.Publication.Owner) != owners.end()) {
				++entry;
				continue;
			}
			(void)Publisher.Retire(renderer, entry->second.Publication);
			entry = Entries.erase(entry);
		}
		for (auto cursor = NextBindingByOwner.begin(); cursor != NextBindingByOwner.end();) {
			if (std::find_if(owners.begin(), owners.end(), [&](engine::core::Name owner) {
					return owner.Id() == cursor->first;
				}) == owners.end())
				cursor = NextBindingByOwner.erase(cursor);
			else
				++cursor;
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
		for (const auto &[key, entry] : Entries)
			(void)Publisher.Retire(renderer, entry.Publication);
		Entries.clear();
		Documents.clear();
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
}
