#include <engine/assets/Builtin.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/EditableImage.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/scene/LocalLight.hpp>
#include <engine/script/GltfSceneExport.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace engine::script {
	namespace {
		constexpr std::string_view STABLE_ID_ATTRIBUTE = "DataFactoryId";

		bool StableId(const ecs::Store &store, ecs::Entity entity, std::string &out) {
			ecs::AttributeValue value;
			if (!ecs::GetAttribute(store, entity, core::Name(STABLE_ID_ATTRIBUTE), value) ||
				value.Type != ecs::PropertyType::String || value.String.empty())
				return false;
			out = value.String;
			return true;
		}

		bool Finite(const core::Vector3 &value) {
			return std::isfinite(value.X) && std::isfinite(value.Y) && std::isfinite(value.Z);
		}

		bool Finite(const core::Color3 &value) {
			return std::isfinite(value.R) && std::isfinite(value.G) && std::isfinite(value.B);
		}

		bool NormalisedFrame(const core::CFrame &source, core::CFrame &out) {
			if (!Finite(source.Position) || !std::isfinite(source.QuaternionX) ||
				!std::isfinite(source.QuaternionY) || !std::isfinite(source.QuaternionZ) ||
				!std::isfinite(source.QuaternionW))
				return false;
			const double length = std::hypot(
				std::hypot(static_cast<double>(source.QuaternionX), static_cast<double>(source.QuaternionY)),
				std::hypot(static_cast<double>(source.QuaternionZ), static_cast<double>(source.QuaternionW))
			);
			if (!std::isfinite(length) || length <= 0.0) return false;
			out = source;
			out.QuaternionX = static_cast<float>(static_cast<double>(source.QuaternionX) / length);
			out.QuaternionY = static_cast<float>(static_cast<double>(source.QuaternionY) / length);
			out.QuaternionZ = static_cast<float>(static_cast<double>(source.QuaternionZ) / length);
			out.QuaternionW = static_cast<float>(static_cast<double>(source.QuaternionW) / length);
			return true;
		}

		bool UnitColour(const core::Color3 &value) {
			return Finite(value) && value.R >= 0.0f && value.R <= 1.0f && value.G >= 0.0f &&
				   value.G <= 1.0f && value.B >= 0.0f && value.B <= 1.0f;
		}

		bool NativeImage(const scene::EditableImage &image) {
			const uint8_t supported = static_cast<uint8_t>(scene::EditablePackingAttribute::Colour) |
									  static_cast<uint8_t>(scene::EditablePackingAttribute::Alpha);
			const uint64_t pixels = static_cast<uint64_t>(image.Width) * image.Height;
			return image.Width > 0 && image.Height > 0 && pixels <= MAX_GLTF_EXPORT_TEXTURE_BYTES / 4 &&
				   image.Pixels.size() == pixels * 4 &&
				   image.Packing.Format == scene::EditablePackingFormat::Float32 &&
				   (image.Packing.Attributes & ~supported) == 0;
		}

		struct ImageSource {
			uint32_t Width = 0;
			uint32_t Height = 0;
			assets::TextureFormat Format = assets::TextureFormat::RGBA8_LINEAR;
			std::span<const uint8_t> Pixels;
		};

		uint8_t SourceChannel(const ImageSource &image, size_t pixel, size_t channel) {
			if (image.Format == assets::TextureFormat::R8) return channel == 3 ? 255 : image.Pixels[pixel];
			return image.Pixels[pixel * 4 + channel];
		}

		uint8_t Srgb8(uint8_t linear) {
			static const auto values = [] {
				std::array<uint8_t, 256> converted{};
				for (size_t index = 0; index < converted.size(); ++index) {
					const double value = static_cast<double>(index) / 255.0;
					const double srgb =
						value <= 0.0031308 ? value * 12.92 : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
					converted[index] = static_cast<uint8_t>(std::lround(srgb * 255.0));
				}
				return converted;
			}();
			return values[linear];
		}

		uint8_t Linear8(uint8_t srgb) {
			static const auto values = [] {
				std::array<uint8_t, 256> converted{};
				for (size_t index = 0; index < converted.size(); ++index) {
					const double value = static_cast<double>(index) / 255.0;
					const double linear =
						value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
					converted[index] = static_cast<uint8_t>(std::lround(linear * 255.0));
				}
				return converted;
			}();
			return values[srgb];
		}

		uint8_t ExportChannel(const ImageSource &image, size_t pixel, size_t channel, bool srgb) {
			const uint8_t source = SourceChannel(image, pixel, channel);
			if (channel == 3) return source;
			if (srgb && image.Format != assets::TextureFormat::RGBA8) return Srgb8(source);
			if (!srgb && image.Format == assets::TextureFormat::RGBA8) return Linear8(source);
			return source;
		}

		bool MeshFromEditable(const scene::EditableMesh &editable, assets::MeshData &out) {
			if (editable.Positions.empty() || editable.Positions.size() > MAX_GLTF_EXPORT_VERTICES ||
				editable.Indices.empty() || editable.Indices.size() > MAX_GLTF_EXPORT_INDICES ||
				editable.Indices.size() % 3 != 0 || editable.Normals.size() != editable.Positions.size() ||
				editable.UVs.size() != editable.Positions.size())
				return false;
			out.Vertices.reserve(editable.Positions.size());
			for (size_t index = 0; index < editable.Positions.size(); ++index) {
				if (!Finite(editable.Positions[index]) || !Finite(editable.Normals[index]) ||
					!std::isfinite(editable.UVs[index].X) || !std::isfinite(editable.UVs[index].Y))
					return false;
				assets::MeshVertex vertex{};
				vertex.Position[0] = editable.Positions[index].X;
				vertex.Position[1] = editable.Positions[index].Y;
				vertex.Position[2] = editable.Positions[index].Z;
				vertex.Normal[0] = editable.Normals[index].X;
				vertex.Normal[1] = editable.Normals[index].Y;
				vertex.Normal[2] = editable.Normals[index].Z;
				vertex.TexCoord[0] = editable.UVs[index].X;
				vertex.TexCoord[1] = editable.UVs[index].Y;
				out.Vertices.push_back(vertex);
			}
			out.Indices = editable.Indices;
			out.ComputeBounds();
			return out.IsValid();
		}

		bool MeshFromName(
			ecs::Store &store,
			const core::Name &name,
			assets::MeshData &out,
			std::string &reason,
			const GltfMeshSource &meshSource
		) {
			if (!name.IsValid()) {
				out = assets::MakeBuiltin(assets::BuiltinMesh::Cube);
				return true;
			}
			assets::BuiltinMesh builtin;
			if (assets::BuiltinFromName(name.Text(), builtin)) {
				out = assets::MakeBuiltin(builtin);
				return true;
			}
			bool matchedEditable = false;
			store.Each<const scene::EditableMesh>([&](ecs::Entity entity,
													  const scene::EditableMesh &editable) {
				if (matchedEditable || scene::EditableMeshContentName(store, entity) != name) return;
				matchedEditable = true;
				if (!MeshFromEditable(editable, out)) reason = "editable_mesh_geometry_invalid_or_over_limit";
			});
			if (matchedEditable) {
				if (reason.empty()) return true;
				return false;
			}
			if (meshSource) {
				switch (meshSource(store.Name(), name.Text(), out)) {
				case GltfMeshSourceStatus::Available:
					if (out.Vertices.empty() || out.Vertices.size() > MAX_GLTF_EXPORT_VERTICES ||
						out.Indices.empty() || out.Indices.size() > MAX_GLTF_EXPORT_INDICES ||
						!out.IsValid()) {
						reason = "source_geometry_invalid_or_over_limit";
						return false;
					}
					return true;
				case GltfMeshSourceStatus::OverLimit:
					reason = "source_geometry_over_limit";
					return false;
				case GltfMeshSourceStatus::Unsupported:
					reason = "source_geometry_unsupported";
					return false;
				case GltfMeshSourceStatus::Invalid:
					reason = "source_geometry_invalid";
					return false;
				case GltfMeshSourceStatus::Missing:
					break;
				}
			}
			reason = "source_geometry_unavailable";
			return false;
		}

		core::Vector3 ScaleFor(const assets::MeshData &mesh, const scene::Bounds &bounds) {
			const core::Vector3 extent{
				(mesh.Maximum.X - mesh.Minimum.X) * 0.5f,
				(mesh.Maximum.Y - mesh.Minimum.Y) * 0.5f,
				(mesh.Maximum.Z - mesh.Minimum.Z) * 0.5f,
			};
			return {
				extent.X > 0.0f ? bounds.HalfExtent.X / extent.X : 1.0f,
				extent.Y > 0.0f ? bounds.HalfExtent.Y / extent.Y : 1.0f,
				extent.Z > 0.0f ? bounds.HalfExtent.Z / extent.Z : 1.0f
			};
		}
	}

	bool CaptureGltfSceneExport(
		ecs::Store &store,
		GltfSceneExport &out,
		std::string &failure,
		const GltfMeshSource &meshSource,
		const GltfTextureSource &textureSource,
		bool extractOnly
	) {
		GltfSceneExport captured;
		captured.Tick = store.Time().Tick;
		std::vector<std::pair<ecs::Entity, std::string>> selected;
		std::unordered_set<std::string> ids;
		store.Each<const ecs::InstanceName>([&](ecs::Entity entity, const ecs::InstanceName &) {
			std::string id;
			if (!StableId(store, entity, id)) return;
			if (id.size() > 256 || !ids.emplace(id).second) {
				failure = "gltf export requires unique stable ids of at most 256 bytes";
				return;
			}
			selected.emplace_back(entity, std::move(id));
		});
		if (!failure.empty()) return false;
		std::sort(selected.begin(), selected.end(), [](const auto &left, const auto &right) {
			return left.second < right.second;
		});

		std::unordered_map<std::string, size_t> meshIndexes;
		std::unordered_map<uint32_t, const scene::EditableImage *> editableSources;
		store.Each<const scene::EditableImage>([&](ecs::Entity entity, const scene::EditableImage &image) {
			editableSources.emplace(scene::EditableImageContentName(store, entity).Id(), &image);
		});
		std::unordered_map<uint32_t, assets::TextureData> residentSources;
		std::unordered_map<uint32_t, ImageSource> imageSources;
		std::unordered_map<uint32_t, size_t> sourceTextureIndexes;
		std::map<std::pair<uint32_t, bool>, size_t> textureIndexes;
		std::map<std::array<uint32_t, 4>, size_t> packedIndexes;
		size_t textureBytes = 0;
		size_t sourceTextureBytes = 0;
		size_t geometryBytes = 0;
		const auto imageOf = [&](core::Name name, const ImageSource *&image, std::string &reason) {
			image = nullptr;
			if (!name.IsValid()) return true;
			if (const auto found = imageSources.find(name.Id()); found != imageSources.end()) {
				image = &found->second;
				return true;
			}
			if (const auto found = editableSources.find(name.Id()); found != editableSources.end()) {
				if (!NativeImage(*found->second)) {
					reason = "source_texture_invalid";
					return false;
				}
				const auto &source = *found->second;
				image = &imageSources
							 .emplace(
								 name.Id(),
								 ImageSource{
									 source.Width,
									 source.Height,
									 assets::TextureFormat::RGBA8_LINEAR,
									 source.Pixels
								 }
							 )
							 .first->second;
				return true;
			}
			if (!textureSource) {
				reason = "source_texture_unavailable";
				return false;
			}
			assets::TextureData source;
			switch (textureSource(store.Name(), name.Text(), source)) {
			case GltfTextureSourceStatus::Available:
				break;
			case GltfTextureSourceStatus::OverLimit:
				reason = "source_texture_over_limit";
				return false;
			case GltfTextureSourceStatus::Unsupported:
				reason = "source_texture_unsupported";
				return false;
			case GltfTextureSourceStatus::Invalid:
				reason = "source_texture_invalid";
				return false;
			case GltfTextureSourceStatus::Missing:
				reason = "source_texture_unavailable";
				return false;
			}
			if (!source.IsValid() || source.Pixels.size() > MAX_GLTF_EXPORT_SOURCE_TEXTURE_BYTES ||
				(source.Format != assets::TextureFormat::RGBA8 &&
				 source.Format != assets::TextureFormat::RGBA8_LINEAR &&
				 source.Format != assets::TextureFormat::R8)) {
				reason = "source_texture_invalid";
				return false;
			}
			const auto found = residentSources.emplace(name.Id(), std::move(source)).first;
			const auto &resident = found->second;
			image = &imageSources
						 .emplace(
							 name.Id(),
							 ImageSource{
								 resident.Width,
								 resident.Height,
								 resident.Format,
								 {reinterpret_cast<const uint8_t *>(resident.Pixels.data()),
								  resident.Pixels.size()}
							 }
						 )
						 .first->second;
			return true;
		};
		const auto addSourceImage = [&](core::Name name, const ImageSource *image) -> std::optional<size_t> {
			if (!name.IsValid() || image == nullptr) return {};
			if (const auto found = sourceTextureIndexes.find(name.Id()); found != sourceTextureIndexes.end())
				return found->second;
			const size_t bytes =
				static_cast<size_t>(image->Width) * image->Height * assets::BytesPerPixel(image->Format);
			if (captured.SourceTextures.size() == MAX_GLTF_EXPORT_TEXTURES ||
				bytes > MAX_GLTF_EXPORT_TEXTURE_BYTES - sourceTextureBytes)
				return {};
			GltfExportSourceTexture source;
			source.Name = std::string(name.Text());
			source.Width = image->Width;
			source.Height = image->Height;
			source.Format = image->Format;
			source.Pixels.assign(
				reinterpret_cast<const std::byte *>(image->Pixels.data()),
				reinterpret_cast<const std::byte *>(image->Pixels.data()) + bytes
			);
			const size_t index = captured.SourceTextures.size();
			captured.SourceTextures.push_back(std::move(source));
			sourceTextureBytes += bytes;
			sourceTextureIndexes.emplace(name.Id(), index);
			return index;
		};
		for (const auto &[entity, id] : selected) {
			if (const auto *camera = store.Get<scene::Camera>(entity); camera != nullptr) {
				const auto *transform = store.Get<scene::Transform>(entity);
				core::CFrame frame;
				if (transform == nullptr || !NormalisedFrame(transform->Frame, frame) ||
					!std::isfinite(camera->NearPlane) || !std::isfinite(camera->FarPlane) ||
					!std::isfinite(camera->FieldOfViewRadians) || !(camera->NearPlane > 0.0f) ||
					!(camera->FarPlane > camera->NearPlane) || !(camera->FieldOfViewRadians > 0.0f) ||
					!(camera->FieldOfViewRadians < std::numbers::pi_v<float>)) {
					captured.Unavailable.push_back({id, "camera", "camera_transform_or_projection_invalid"});
				} else {
					captured.Cameras.push_back(
						{id,
						 std::string(store.InstanceNameOf(entity).Text()),
						 frame,
						 camera->FieldOfViewRadians,
						 camera->NearPlane,
						 camera->FarPlane}
					);
				}
			}

			if (const auto *light = store.Get<scene::Light>(entity); light != nullptr) {
				scene::ResolvedLocalLight resolved;
				const auto rejection = scene::ResolveLocalLight(store, entity, *light, resolved);
				if (rejection != scene::LocalLightRejection::None) {
					captured.Unavailable.push_back(
						{id, "light", "light_" + std::string(scene::Describe(rejection))}
					);
				} else if (light->Kind != scene::LightKind::Point) {
					captured.Unavailable.push_back({id, "light", "light_kind_not_yet_exported"});
				} else if (!UnitColour(light->Colour)) {
					captured.Unavailable.push_back({id, "light", "light_colour_invalid"});
				} else if (captured.Lights.size() == MAX_GLTF_EXPORT_LIGHTS) {
					failure = "gltf export light limit exceeded";
					return false;
				} else {
					captured.Lights.push_back(
						{id,
						 std::string(store.InstanceNameOf(entity).Text()),
						 resolved.Position,
						 light->Colour,
						 light->Brightness,
						 light->Range}
					);
				}
			}

			const auto *visual = store.Get<scene::Visual>(entity);
			const auto *transform = store.Get<scene::Transform>(entity);
			const auto *bounds = store.Get<scene::Bounds>(entity);
			if (visual == nullptr || transform == nullptr || bounds == nullptr) continue;
			if (!visual->Visible) {
				captured.Unavailable.push_back(
					{id,
					 "visibility",
					 extractOnly ? "visibility_renderer_semantics_unavailable"
								 : "visual_hidden_not_representable"}
				);
				if (!extractOnly) continue;
			}
			if (!std::isfinite(visual->Transparency) || visual->Transparency < 0.0f ||
				visual->Transparency > 1.0f) {
				captured.Unavailable.push_back({id, "appearance", "visual_transparency_invalid"});
				continue;
			}
			const auto *surface = store.Get<scene::SurfaceAppearance>(entity);
			if (!Finite(visual->Tint) || (surface != nullptr && !Finite(surface->Colour))) {
				captured.Unavailable.push_back({id, "appearance", "material_colour_invalid"});
				continue;
			}
			if (surface != nullptr && surface->ColourMap.IsValid() &&
				(surface->Mode == scene::AlphaMode::Overlay || surface->Mode == scene::AlphaMode::TintMask)) {
				captured.Unavailable.push_back(
					{id,
					 "appearance",
					 extractOnly ? "surface_alpha_mode_renderer_semantics_unavailable"
								 : "surface_alpha_mode_texture_dependent"}
				);
				if (!extractOnly) continue;
			}
			if (surface != nullptr && (surface->HeightMap.IsValid() || surface->Shader.IsValid())) {
				captured.Unavailable.push_back(
					{id,
					 "appearance",
					 extractOnly ? "surface_height_or_shader_renderer_semantics_unavailable"
								 : "surface_height_or_shader_unavailable"}
				);
				if (!extractOnly) continue;
			}
			if (captured.Nodes.size() == MAX_GLTF_EXPORT_NODES) {
				failure = "gltf export node limit exceeded";
				return false;
			}
			assets::MeshData mesh;
			std::string reason;
			if (!MeshFromName(store, visual->Mesh, mesh, reason, meshSource)) {
				captured.Unavailable.push_back({id, "geometry", std::move(reason)});
				continue;
			}
			const core::Vector3 scale = ScaleFor(mesh, *bounds);
			core::CFrame frame;
			if (!NormalisedFrame(transform->Frame, frame) || !Finite(scale) || !(scale.X > 0.0f) ||
				!(scale.Y > 0.0f) || !(scale.Z > 0.0f)) {
				captured.Unavailable.push_back({id, "geometry", "transform_or_mesh_extent_invalid"});
				continue;
			}
			const std::string meshName(visual->Mesh.IsValid() ? visual->Mesh.Text() : "builtin/cube");
			auto [found, inserted] = meshIndexes.emplace(meshName, captured.Meshes.size());
			if (inserted) captured.Meshes.push_back({meshName, std::move(mesh)});
			GltfExportMaterial material;
			material.BaseColour = visual->Tint;
			material.Alpha = 1.0f - visual->Transparency;
			material.AlphaMode =
				visual->Transparency > 0.0f ? GltfExportAlphaMode::Blend : GltfExportAlphaMode::Opaque;
			const auto addImage = [&](core::Name name,
									  const ImageSource *image,
									  std::string_view role,
									  bool srgb) -> std::optional<size_t> {
				if (image == nullptr) return {};
				const auto key = std::pair{name.Id(), srgb};
				if (const auto existing = textureIndexes.find(key); existing != textureIndexes.end())
					return existing->second;
				const size_t index = captured.Textures.size();
				GltfExportTexture exported;
				exported.Name = id + "/" + std::string(role);
				exported.Width = image->Width;
				exported.Height = image->Height;
				exported.Pixels.resize(static_cast<size_t>(image->Width) * image->Height * 4);
				for (size_t pixel = 0; pixel < static_cast<size_t>(image->Width) * image->Height; ++pixel)
					for (size_t channel = 0; channel < 4; ++channel)
						exported.Pixels[pixel * 4 + channel] = ExportChannel(*image, pixel, channel, srgb);
				captured.Textures.push_back(std::move(exported));
				textureIndexes.emplace(key, index);
				textureBytes += static_cast<size_t>(image->Width) * image->Height * 4;
				return index;
			};
			if (surface != nullptr) {
				material.Pixelated = surface->Resample == scene::SurfaceResampleMode::Pixelated;
				material.BaseColour.R *= surface->Colour.R;
				material.BaseColour.G *= surface->Colour.G;
				material.BaseColour.B *= surface->Colour.B;
				if (!UnitColour(material.BaseColour) || !UnitColour(surface->EmissiveTint) ||
					!std::isfinite(surface->EmissiveStrength) || surface->EmissiveStrength < 0.0f ||
					surface->EmissiveStrength > 1.0f || !std::isfinite(surface->AlphaCutoff) ||
					surface->AlphaCutoff < 0.0f || surface->AlphaCutoff > 1.0f) {
					captured.Unavailable.push_back({id, "appearance", "material_factor_out_of_range"});
					continue;
				}
				const ImageSource *colour = nullptr, *normal = nullptr, *occlusion = nullptr;
				const ImageSource *emissive = nullptr, *roughness = nullptr, *metalness = nullptr,
								  *height = nullptr, *packedPbr = nullptr;
				uint8_t roughnessChannel = 0, occlusionChannel = 0, metalnessChannel = 0;
				std::string textureReason;
				if (extractOnly) {
					auto sourceMap = [&](core::Name name, const ImageSource *&image) {
						std::string reason;
						if (!imageOf(name, image, reason)) {
							captured.Unavailable.push_back({id, "appearance", std::move(reason)});
							image = nullptr;
						}
					};
					sourceMap(surface->ColourMap, colour);
					sourceMap(surface->NormalMap, normal);
					sourceMap(surface->OcclusionMap, occlusion);
					sourceMap(surface->EmissiveMap, emissive);
					sourceMap(surface->RoughnessMap, roughness);
					sourceMap(surface->MetalnessMap, metalness);
					sourceMap(surface->HeightMap, height);
					sourceMap(surface->PackedPbrMap, packedPbr);
				} else if (!imageOf(surface->ColourMap, colour, textureReason) ||
						   !imageOf(surface->NormalMap, normal, textureReason) ||
						   !imageOf(surface->OcclusionMap, occlusion, textureReason) ||
						   !imageOf(surface->EmissiveMap, emissive, textureReason) ||
						   !imageOf(surface->RoughnessMap, roughness, textureReason) ||
						   !imageOf(surface->MetalnessMap, metalness, textureReason) ||
						   !imageOf(surface->HeightMap, height, textureReason)) {
					captured.Unavailable.push_back({id, "appearance", std::move(textureReason)});
					continue;
				}
				if (!extractOnly && !imageOf(surface->PackedPbrMap, packedPbr, textureReason)) {
					captured.Unavailable.push_back({id, "appearance", std::move(textureReason)});
					continue;
				}
				if (packedPbr != nullptr) {
					if (surface->RoughnessChannel < 4) {
						roughness = packedPbr;
						roughnessChannel = surface->RoughnessChannel;
					}
					if (surface->OcclusionChannel < 4) {
						occlusion = packedPbr;
						occlusionChannel = surface->OcclusionChannel;
					}
					if (surface->MetalnessChannel < 4) {
						metalness = packedPbr;
						metalnessChannel = surface->MetalnessChannel;
					}
				}
				const core::Name roughnessName =
					packedPbr != nullptr && surface->RoughnessChannel < 4 ? surface->PackedPbrMap : surface->RoughnessMap;
				const core::Name occlusionName =
					packedPbr != nullptr && surface->OcclusionChannel < 4 ? surface->PackedPbrMap : surface->OcclusionMap;
				const core::Name metalnessName =
					packedPbr != nullptr && surface->MetalnessChannel < 4 ? surface->PackedPbrMap : surface->MetalnessMap;
				if (!extractOnly && roughness != nullptr && metalness != nullptr &&
					(roughness->Width != metalness->Width || roughness->Height != metalness->Height)) {
					captured.Unavailable.push_back(
						{id, "appearance", "metallic_roughness_dimensions_mismatch"}
					);
					continue;
				}
				if (extractOnly) {
					material.SourceColourTexture = addSourceImage(surface->ColourMap, colour);
					material.SourceNormalTexture = addSourceImage(surface->NormalMap, normal);
					material.SourceOcclusionTexture = addSourceImage(occlusionName, occlusion);
					material.SourceEmissiveTexture = addSourceImage(surface->EmissiveMap, emissive);
					material.SourceRoughnessTexture = addSourceImage(roughnessName, roughness);
					material.SourceMetalnessTexture = addSourceImage(metalnessName, metalness);
					material.SourceHeightTexture = addSourceImage(surface->HeightMap, height);
					material.SourceShader =
						surface->Shader.IsValid() ? std::string(surface->Shader.Text()) : "";
					switch (surface->Mode) {
					case scene::AlphaMode::Overlay:
						material.SourceAlphaMode = "overlay";
						break;
					case scene::AlphaMode::Transparency:
						material.SourceAlphaMode = "transparency";
						break;
					case scene::AlphaMode::TintMask:
						material.SourceAlphaMode = "tint_mask";
						break;
					case scene::AlphaMode::Opaque:
						material.SourceAlphaMode = "opaque";
						break;
					}
					if ((colour != nullptr && !material.SourceColourTexture) ||
						(normal != nullptr && !material.SourceNormalTexture) ||
						(occlusion != nullptr && !material.SourceOcclusionTexture) ||
						(emissive != nullptr && !material.SourceEmissiveTexture) ||
						(roughness != nullptr && !material.SourceRoughnessTexture) ||
						(metalness != nullptr && !material.SourceMetalnessTexture) ||
						(height != nullptr && !material.SourceHeightTexture)) {
						captured.Unavailable.push_back({id, "appearance", "source_texture_extract_limit"});
					}
					if (emissive != nullptr)
						material.EmissiveFactor = {
							surface->EmissiveTint.R * surface->EmissiveStrength,
							surface->EmissiveTint.G * surface->EmissiveStrength,
							surface->EmissiveTint.B * surface->EmissiveStrength
						};
					std::vector<std::optional<size_t>> sourceRuns;
					for (const assets::Submesh &run : captured.Meshes[found->second].Data.Submeshes) {
						if (run.Texture.empty()) {
							sourceRuns.push_back({});
							continue;
						}
						const ImageSource *runImage = nullptr;
						std::string runReason;
						if (!imageOf(core::Name(run.Texture), runImage, runReason)) {
							captured.Unavailable.push_back({id, "appearance", "submesh_texture_unavailable"});
							sourceRuns.push_back({});
							continue;
						}
						sourceRuns.push_back(addSourceImage(core::Name(run.Texture), runImage));
					}
					GltfExportNode node{
						id,
						std::string(store.InstanceNameOf(entity).Text()),
						frame,
						scale,
						found->second,
						material,
						{},
						std::move(sourceRuns)
					};
					node.Visible = visual->Visible;
					captured.Nodes.push_back(std::move(node));
					continue;
				}
				std::set<std::pair<uint32_t, bool>> newImages;
				size_t addedBytes = 0;
				for (const auto &[name, srgb] :
					 {std::pair{surface->ColourMap, true},
					  std::pair{surface->NormalMap, false},
					  std::pair{surface->OcclusionMap, false},
					  std::pair{surface->EmissiveMap, true}}) {
					const auto key = std::pair{name.Id(), srgb};
					if (!name.IsValid() || textureIndexes.contains(key) || !newImages.emplace(key).second)
						continue;
					const auto &image = imageSources.at(name.Id());
					addedBytes += static_cast<size_t>(image.Width) * image.Height * 4;
				}
				// The combined glTF texture depends on the effective source and lane of
				// each semantic. Two packed maps, or two selector pairs on one map,
				// cannot share a generated image.
				const std::array pair{
					roughnessName.IsValid() ? roughnessName.Id() : core::Name::INVALID,
					static_cast<uint32_t>(roughnessChannel),
					metalnessName.IsValid() ? metalnessName.Id() : core::Name::INVALID,
					static_cast<uint32_t>(metalnessChannel),
				};
				const bool newPacked =
					(roughness != nullptr || metalness != nullptr) && !packedIndexes.contains(pair);
				const bool swizzleOcclusion =
					packedPbr != nullptr && surface->OcclusionChannel < 4 && occlusionChannel != 0;
				if (newPacked) {
					const auto *shape = roughness != nullptr ? roughness : metalness;
					addedBytes += static_cast<size_t>(shape->Width) * shape->Height * 4;
				}
				if (swizzleOcclusion) {
					addedBytes += static_cast<size_t>(packedPbr->Width) * packedPbr->Height * 4;
				}
				if (newImages.size() + static_cast<size_t>(newPacked) + static_cast<size_t>(swizzleOcclusion) >
						MAX_GLTF_EXPORT_TEXTURES - captured.Textures.size() ||
					addedBytes > MAX_GLTF_EXPORT_TEXTURE_BYTES - textureBytes) {
					captured.Unavailable.push_back({id, "appearance", "texture_export_limit"});
					continue;
				}
				material.ColourTexture = addImage(surface->ColourMap, colour, "colour", true);
				material.NormalTexture = addImage(surface->NormalMap, normal, "normal", false);
				if (swizzleOcclusion) {
					GltfExportTexture packedOcclusion;
					packedOcclusion.Name = id + "/occlusion";
					packedOcclusion.Width = packedPbr->Width;
					packedOcclusion.Height = packedPbr->Height;
					packedOcclusion.Pixels.resize(static_cast<size_t>(packedPbr->Width) * packedPbr->Height * 4);
					for (size_t pixel = 0; pixel < static_cast<size_t>(packedPbr->Width) * packedPbr->Height; ++pixel) {
						packedOcclusion.Pixels[pixel * 4] = ExportChannel(*packedPbr, pixel, occlusionChannel, false);
						packedOcclusion.Pixels[pixel * 4 + 1] = 0;
						packedOcclusion.Pixels[pixel * 4 + 2] = 0;
						packedOcclusion.Pixels[pixel * 4 + 3] = 255;
					}
					material.OcclusionTexture = captured.Textures.size();
					textureBytes += packedOcclusion.Pixels.size();
					captured.Textures.push_back(std::move(packedOcclusion));
				} else {
					material.OcclusionTexture = addImage(
						packedPbr != nullptr && surface->OcclusionChannel < 4 ? surface->PackedPbrMap : surface->OcclusionMap,
						occlusion, "occlusion", false
					);
				}
				material.EmissiveTexture = addImage(surface->EmissiveMap, emissive, "emissive", true);
				if (roughness != nullptr || metalness != nullptr) {
					if (const auto found = packedIndexes.find(pair); found != packedIndexes.end()) {
						material.MetallicRoughnessTexture = found->second;
					} else {
						const ImageSource &shape = *(roughness != nullptr ? roughness : metalness);
						GltfExportTexture packed;
						packed.Name = id + "/metallic-roughness";
						packed.Width = shape.Width;
						packed.Height = shape.Height;
						packed.Pixels.resize(static_cast<size_t>(shape.Width) * shape.Height * 4);
						for (size_t pixel = 0; pixel < static_cast<size_t>(shape.Width) * shape.Height;
							 ++pixel) {
							packed.Pixels[pixel * 4] = 255;
							packed.Pixels[pixel * 4 + 1] =
								roughness != nullptr ? ExportChannel(*roughness, pixel, roughnessChannel, false) : 255;
							packed.Pixels[pixel * 4 + 2] =
								metalness != nullptr ? ExportChannel(*metalness, pixel, metalnessChannel, false) : 255;
							packed.Pixels[pixel * 4 + 3] = 255;
						}
						material.MetallicRoughnessTexture = captured.Textures.size();
						packedIndexes.emplace(pair, *material.MetallicRoughnessTexture);
						textureBytes += packed.Pixels.size();
						captured.Textures.push_back(std::move(packed));
					}
					material.RoughnessFactor = roughness != nullptr ? 1.0f : 0.65f;
					material.MetalnessFactor = metalness != nullptr ? 1.0f : 0.0f;
				}
				if (emissive != nullptr) {
					material.EmissiveFactor = {
						surface->EmissiveTint.R * surface->EmissiveStrength,
						surface->EmissiveTint.G * surface->EmissiveStrength,
						surface->EmissiveTint.B * surface->EmissiveStrength
					};
				}
				if (colour != nullptr && surface->Mode == scene::AlphaMode::Transparency) {
					if (material.Alpha >= 0.98f) {
						material.AlphaMode = GltfExportAlphaMode::Mask;
						material.AlphaCutoff = surface->AlphaCutoff;
					} else {
						material.AlphaMode = GltfExportAlphaMode::Blend;
					}
				}
			}
			bool invalidRun = !UnitColour(material.BaseColour);
			for (const assets::Submesh &run : captured.Meshes[found->second].Data.Submeshes) {
				const std::array<float, 4> factors{
					material.BaseColour.R * run.BaseColour[0],
					material.BaseColour.G * run.BaseColour[1],
					material.BaseColour.B * run.BaseColour[2],
					material.Alpha * run.BaseColour[3]
				};
				for (const float factor : factors)
					invalidRun = invalidRun || !std::isfinite(factor) || factor < 0.0f || factor > 1.0f;
			}
			if (invalidRun) {
				captured.Unavailable.push_back({id, "appearance", "submesh_material_factor_invalid"});
				continue;
			}
			std::vector<std::optional<size_t>> runTextures;
			std::string runTextureLoss;
			for (const assets::Submesh &run : captured.Meshes[found->second].Data.Submeshes) {
				if (run.Texture.empty() || material.ColourTexture.has_value()) {
					runTextures.push_back({});
					continue;
				}
				const core::Name name(run.Texture);
				const ImageSource *image = nullptr;
				std::string sourceReason;
				if (!imageOf(name, image, sourceReason)) {
					runTextureLoss = sourceReason == "source_texture_over_limit"
										 ? "submesh_texture_over_limit"
										 : "submesh_texture_unavailable";
					runTextures.push_back({});
					continue;
				}
				const auto key = std::pair{name.Id(), true};
				const size_t bytes = static_cast<size_t>(image->Width) * image->Height * 4;
				if (!textureIndexes.contains(key) && (captured.Textures.size() == MAX_GLTF_EXPORT_TEXTURES ||
													  bytes > MAX_GLTF_EXPORT_TEXTURE_BYTES - textureBytes)) {
					runTextureLoss = "submesh_texture_over_limit";
					runTextures.push_back({});
					continue;
				}
				runTextures.push_back(addImage(name, image, "submesh-colour", true));
			}
			if (!runTextureLoss.empty())
				captured.Unavailable.push_back({id, "appearance", std::move(runTextureLoss)});
			const auto &nodeMesh = captured.Meshes[found->second].Data;
			// The GLB writer emits geometry per node because node materials can differ.
			const size_t nodeBytes =
				nodeMesh.Vertices.size() * 8 * sizeof(float) + nodeMesh.Indices.size() * sizeof(uint32_t);
			if (nodeBytes > MAX_GLTF_EXPORT_GEOMETRY_BYTES - geometryBytes) {
				captured.Unavailable.push_back({id, "geometry", "scene_geometry_over_limit"});
				continue;
			}
			geometryBytes += nodeBytes;
			captured.Nodes.push_back(
				{id,
				 std::string(store.InstanceNameOf(entity).Text()),
				 frame,
				 scale,
				 found->second,
				 material,
				 std::move(runTextures),
				 {}}
			);
		}
		out = std::move(captured);
		return true;
	}
}
