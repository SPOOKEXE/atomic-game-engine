#include "SourceCamera3D.hpp"

#include "GpuHeap.hpp"
#include "ImageGraphGpuHelpers.hpp"
#include "ShaderBinary.hpp"
#include "SourceCamera3DUniforms.hpp"
#include "SourceUniformBlocks.hpp"
#include "TextureFormatSupport.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/SourceInstance3D.hpp>
#include <engine/imagegraph/Surface.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/resources/Shaders.hpp>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <new>

namespace engine::render::imagegraph {
	namespace {
		namespace source = engine::imagegraph;
		static_assert(CAMERA_UNIFORM_CUTS[1] == offsetof(SourceCameraUniforms, light_pnt_view));
		static_assert(CAMERA_UNIFORM_CUTS[2] == offsetof(SourceCameraUniforms, light_pnt_proj));
		struct Vertex {
			glm::vec3 Position, Normal;
			glm::vec2 UV;
			glm::vec4 Color;
			glm::vec3 Barycentric;
		};
		struct VertexUniforms {
			glm::mat4 View, Projection;
			glm::vec4 Clipping;
		};
		struct PostUniforms {
			glm::mat4 Projection, InverseViewProjection;
			glm::vec4 CameraPosition, Ambient;
			glm::vec2 Dimension;
			float Radius = 0, Bias = 0, Strength = 0;
			int32_t Mode = 0, SwapX = 0, ShowBackground = 0, EnvironmentMapping = 0, AoEnabled = 0;
		};
		struct Part {
			uint32_t First = 0, Count = 0;
			const source::MaterialData3D *Material = nullptr;
			bool Edge = false;
			bool Mirrored = false;
			bool Shadow = false;
			bool Instanced = false;
		};
		struct Light {
			const source::LightData3D *Data = nullptr;
			glm::vec3 Position{};
		};
		struct Prepared {
			std::vector<Vertex> Vertices;
			std::vector<Part> Parts;
			std::vector<Light> Lights;
			uint64_t SourceBytes = 0;
			uint64_t ShadowBytes = 0;
			uint64_t MaximumPreparationBytes = 128ull * 1024 * 1024;
			// Fixed mirror stack and the bounded recursive scene matrix chain coexist with both roots.
			uint64_t PreparationBytes = 2 * sizeof(Prepared) + 128 * sizeof(glm::mat4);
			size_t VertexCount = 0, PartCount = 0, LightCount = 0;
			uint32_t Directional = 0, Point = 0, DirectionalShadow = 0, PointShadow = 0;
			bool Counting = false, LimitExceeded = false;
		};
		template <class T> bool Emit(Prepared &out, std::vector<T> &storage, size_t &count, T value) {
			if (sizeof(T) > out.MaximumPreparationBytes ||
				out.PreparationBytes > out.MaximumPreparationBytes - sizeof(T)) {
				out.LimitExceeded = true;
				return false;
			}
			out.PreparationBytes += sizeof(T);
			++count;
			if (!out.Counting) {
				if (storage.size() >= storage.capacity()) return false;
				storage.push_back(std::move(value));
			}
			return true;
		}
		glm::vec3 Vec(source::Vector3 v) {
			return {v.X, v.Y, v.Z};
		}
		glm::vec2 Vec(source::Vector2 v) {
			return {v.X, v.Y};
		}
		glm::vec4 Color(source::Colour c) {
			return glm::vec4(c.Red, c.Green, c.Blue, c.Alpha) / 255.f;
		}
		glm::mat4 Matrix(const source::MeshTransform3D &t) {
			glm::quat rotation(t.Rotation.W, t.Rotation.X, t.Rotation.Y, t.Rotation.Z);
			return glm::translate(glm::mat4(1), Vec(t.Position)) * glm::mat4_cast(glm::normalize(rotation)) *
				   glm::scale(glm::mat4(1), Vec(t.Scale)) * glm::translate(glm::mat4(1), -Vec(t.Anchor));
		}
		bool Finite(const glm::mat4 &m) {
			for (size_t i = 0; i < 16; ++i)
				if (!std::isfinite(glm::value_ptr(m)[i])) return false;
			return true;
		}
		bool SurfaceValid(const source::Image &image) {
			return source::ValidSurfaceLayout(image, 4096, 64ull * 1024 * 1024) &&
				   source::FiniteSurfaceSamples(image);
		}
		uint64_t MaterialBytes(const source::MaterialData3D &m) {
			uint64_t bytes = sizeof(m);
			for (const auto *surface : {&m.Surface, &m.Normal, &m.PropertiesMap})
				if (*surface) {
					if (!SurfaceValid(**surface)) return UINT64_MAX;
					bytes += (*surface)->Pixels.capacity();
				}
			for (double v :
				 {m.TextureScale.X,
				  m.TextureScale.Y,
				  m.TextureShift.X,
				  m.TextureShift.Y,
				  m.TextureFilter,
				  m.Diffuse,
				  m.Specular,
				  m.Shininess,
				  m.Reflectance,
				  m.NormalStrength,
				  m.MetallicRange.X,
				  m.MetallicRange.Y,
				  m.RoughnessRange.X,
				  m.RoughnessRange.Y})
				if (!std::isfinite(v) || std::abs(v) > std::numeric_limits<float>::max()) return UINT64_MAX;
			return bytes;
		}
		bool Mesh(const source::MeshData3D &mesh, const glm::mat4 &parent, Prepared &out, bool shadow) {
			if (shadow && mesh.Instanced) return true;
			if (mesh.LocalTransforms.empty() || mesh.LocalTransforms.size() > 64 || mesh.Parts.size() > 1024)
				return false;
			std::array<glm::mat4, 64> transforms{};
			size_t transformCount = 1;
			transforms[0] = parent;
			for (size_t transformIndex = 0; transformIndex < mesh.LocalTransforms.size(); ++transformIndex) {
				const auto &local = mesh.LocalTransforms[transformIndex];
				if (shadow && transformIndex + 1 != mesh.LocalTransforms.size()) continue;
				const glm::mat4 m = Matrix(local);
				if (!Finite(m)) return false;
				if (shadow && local.Mirror) continue;
				if (!shadow && local.Mirror && local.ShowOriginal) {
					if (transformCount > transforms.size() / 2) return false;
					for (size_t i = 0; i < transformCount; ++i) {
						transforms[transformCount + i] = transforms[i];
						transforms[i] *= m;
					}
					transformCount *= 2;
				} else
					for (size_t i = 0; i < transformCount; ++i)
						transforms[i] *= m;
			}
			for (const auto &material : mesh.Materials) {
				const auto bytes = MaterialBytes(material.Get());
				if (bytes == UINT64_MAX) return false;
				out.SourceBytes += bytes;
			}
			out.SourceBytes += sizeof(mesh) +
							   mesh.LocalTransforms.capacity() * sizeof(source::MeshTransform3D) +
							   mesh.Parts.capacity() * sizeof(source::MeshPart3D) +
							   mesh.Edges.capacity() * sizeof(source::MeshEdge3D) +
							   mesh.Materials.capacity() * sizeof(source::MaterialValue3D) +
							   mesh.Instances.capacity() * sizeof(source::MeshInstance3D);
			for (const auto &part : mesh.Parts)
				out.SourceBytes += part.Vertices.capacity() * sizeof(source::MeshVertex3D);
			const glm::mat4 objectTransform = Matrix(mesh.InstanceObjectTransform);
			if (mesh.Instanced && !Finite(objectTransform)) return false;
			uint64_t verticesPerCopy = 0;
			for (const auto &part : mesh.Parts) {
				if (part.Vertices.size() > source::Limits::MaximumArrayElements - verticesPerCopy)
					return false;
				verticesPerCopy += part.Vertices.size();
			}
			const uint64_t instances = mesh.Instanced ? mesh.Instances.size() : 1;
			if (instances > UINT64_MAX / transformCount) return false;
			const uint64_t copies = instances * transformCount;
			const auto expansion = [&](uint64_t count, uint64_t bytes) {
				if (count && copies > UINT64_MAX / count) return false;
				const uint64_t total = copies * count;
				if (total > UINT64_MAX / bytes ||
					total * bytes > out.MaximumPreparationBytes - out.PreparationBytes)
					return false;
				return true;
			};
			if (!expansion(verticesPerCopy, sizeof(Vertex)) || !expansion(mesh.Parts.size(), sizeof(Part))) {
				out.LimitExceeded = true;
				return false;
			}
			if (verticesPerCopy &&
				copies > (source::Limits::MaximumArrayElements - out.VertexCount) / verticesPerCopy)
				return false;
			if (out.SourceBytes > 64ull * 1024 * 1024) return false;

			for (size_t transform = 0; transform < transformCount; ++transform) {
				const auto &world = transforms[transform];
				for (size_t instanceIndex = 0; instanceIndex < (mesh.Instanced ? mesh.Instances.size() : 1);
					 ++instanceIndex) {
					for (const auto &part : mesh.Parts) {
						if (part.MaterialIndex >= mesh.Materials.size() || part.Vertices.size() % 3 ||
							part.Vertices.size() > source::Limits::MaximumArrayElements - out.VertexCount)
							return false;
						glm::mat4 partWorld = world;
						if (part.LocalMatrix && !mesh.Instanced) {
							const glm::dmat4 local = glm::make_mat4(part.LocalMatrix->data());
							partWorld *= glm::mat4(local);
							if (!Finite(partWorld)) return false;
						}
						const uint32_t first = out.VertexCount;
						for (size_t i = 0; i < part.Vertices.size(); ++i) {
							const auto &v = part.Vertices[i];
							glm::vec3 localPosition = Vec(v.Position), localNormal = Vec(v.Normal);
							if (mesh.Instanced) {
								localPosition = glm::vec3(objectTransform * glm::vec4(localPosition, 1));
								const auto &instance = mesh.Instances[instanceIndex];
								const auto positioned = source::SourceInstancePosition3D(
									instance, {localPosition.x, localPosition.y, localPosition.z}
								);
								const auto normalRotated = source::SourceInstanceNormal3D(instance, v.Normal);
								localPosition = Vec(positioned);
								localNormal = Vec(normalRotated);
							}
							const glm::vec3 position = glm::vec3(partWorld * glm::vec4(localPosition, 1));
							const glm::vec3 normal = glm::vec3(partWorld * glm::vec4(localNormal, 0));
							Vertex vertex{position, normal, Vec(v.UV), Color(v.Tint), glm::vec3(0)};
							vertex.Barycentric[i % 3] = 1;
							for (float n :
								 {position.x,
								  position.y,
								  position.z,
								  normal.x,
								  normal.y,
								  normal.z,
								  vertex.UV.x,
								  vertex.UV.y})
								if (!std::isfinite(n)) return false;
							if (!Emit(out, out.Vertices, out.VertexCount, vertex)) return false;
						}
						if (!Emit(
								out,
								out.Parts,
								out.PartCount,
								Part{
									first,
									uint32_t(part.Vertices.size()),
									&mesh.Materials[part.MaterialIndex].Get(),
									false,
									glm::determinant(partWorld) < 0,
									shadow,
									mesh.Instanced
								}
							))
							return false;
					}
				}
				if (mesh.Instanced) continue;
				if (mesh.Edges.size() > source::Limits::MaximumArrayElements / 2 ||
					mesh.Edges.size() * 2 > source::Limits::MaximumArrayElements - out.VertexCount)
					return false;
				const uint32_t first = out.VertexCount;
				for (const auto &edge : mesh.Edges)
					for (const auto point : {edge.From, edge.To}) {
						glm::vec3 position = glm::vec3(world * glm::vec4(Vec(point), 1));
						if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
							!std::isfinite(position.z))
							return false;
						if (!Emit(
								out,
								out.Vertices,
								out.VertexCount,
								Vertex{position, {0, 0, 1}, {0, 0}, {1, 1, 1, 1}, {1, 0, 0}}
							))
							return false;
					}
				if (!mesh.Edges.empty())
					if (!Emit(
							out,
							out.Parts,
							out.PartCount,
							Part{first, uint32_t(mesh.Edges.size() * 2), nullptr, true, false, shadow}
						))
						return false;
			}
			return out.SourceBytes <= 64ull * 1024 * 1024;
		}
		bool Scene(
			const source::SceneData3D &scene,
			const glm::mat4 &parent,
			Prepared &out,
			size_t depth,
			size_t &objects,
			bool shadow,
			bool skipTransform = false
		) {
			if (depth >= 64 || scene.Objects.size() > 16384 - objects) return false;
			objects += scene.Objects.size();
			const glm::mat4 world = (skipTransform || shadow) ? parent : parent * Matrix(scene.Transform);
			if (!Finite(world)) return false;

			out.SourceBytes += sizeof(scene) + scene.Objects.capacity() * sizeof(source::SceneObject3D);
			for (const auto &object : scene.Objects) {
				if (const auto *mesh = std::get_if<source::MeshValue3D>(&object.Data)) {
					if (mesh->Data && !Mesh(*mesh->Data, world, out, shadow)) return false;
				} else if (const auto *light = std::get_if<source::LightValue3D>(&object.Data)) {
					if (shadow || !light->Data) continue;
					const auto &data = *light->Data;
					const glm::mat4 local = world * Matrix(data.Transform);
					if (!Finite(local)) return false;
					const glm::vec3 position = glm::vec3(local * glm::vec4(Vec(data.Transform.Position), 1));
					for (double v : {data.Intensity, data.Radius, data.ShadowBias})
						if (!std::isfinite(v) || std::abs(v) > std::numeric_limits<float>::max())
							return false;
					if (data.Radius <= .01 || data.ShadowMapSize < 1 || data.ShadowMapSize > 4096 ||
						data.ShadowMapScale < 1)
						return false;
					if (data.Kind != source::LightKind3D::Point &&
						data.Kind != source::LightKind3D::Directional)
						return false;
					if (data.Kind == source::LightKind3D::Directional) {
						if (++out.Directional > 8 || (data.CastShadow && ++out.DirectionalShadow > 2))
							return false;
					} else if (++out.Point > 8 || (data.CastShadow && ++out.PointShadow > 2))
						return false;
					if (!Emit(out, out.Lights, out.LightCount, Light{&data, position})) return false;
					out.SourceBytes += sizeof(data);
					if (data.CastShadow)
						out.ShadowBytes += uint64_t(data.ShadowMapSize) * data.ShadowMapSize * 8 *
										   (data.Kind == source::LightKind3D::Point ? 6 : 1);
				} else if (const auto *nested =
							   std::get_if<source::OwnedPayload3D<source::SceneData3D>>(&object.Data)) {
					if (*nested && !Scene(**nested, world, out, depth + 1, objects, shadow)) return false;
				}
			}
			if (!skipTransform && !shadow && scene.Transform.Mirror && scene.Transform.ShowOriginal)
				if (!Scene(scene, parent, out, depth + 1, objects, shadow, true)) return false;
			return out.SourceBytes <= 64ull * 1024 * 1024;
		}
		bool Traverse(const SourceCamera3DRequest &request, Prepared &out) {
			size_t objects = 0;
			if (request.Scene.Data && !Scene(*request.Scene.Data, glm::mat4(1), out, 0, objects, false))
				return false;
			const uint64_t sourceBytes = out.SourceBytes;
			objects = 0;
			if (request.Scene.Data && !Scene(*request.Scene.Data, glm::mat4(1), out, 0, objects, true))
				return false;
			out.SourceBytes = sourceBytes;
			return true;
		}
		bool Measure(
			const SourceCamera3DRequest &request, Prepared &out, uint64_t maximumBytes = 128ull * 1024 * 1024
		) {
			out.Counting = true;
			out.MaximumPreparationBytes = maximumBytes;
			if (out.PreparationBytes > maximumBytes) {
				out.LimitExceeded = true;
				return false;
			}
			return Traverse(request, out);
		}
		bool Prepare(const SourceCamera3DRequest &request, Prepared &out) {
			Prepared measured;
			if (!Measure(request, measured)) return false;
			// The measured vectors and result coexist during publication; both fixed roots are admitted.
			out.MaximumPreparationBytes = measured.MaximumPreparationBytes;
			out.Vertices.reserve(measured.VertexCount);
			out.Parts.reserve(measured.PartCount);
			out.Lights.reserve(measured.LightCount);
			const uint64_t actual = out.PreparationBytes + out.Vertices.capacity() * sizeof(Vertex) +
									out.Parts.capacity() * sizeof(Part) +
									out.Lights.capacity() * sizeof(Light);
			if (actual > out.MaximumPreparationBytes) return false;
			return Traverse(request, out) && out.VertexCount == measured.VertexCount &&
				   out.PartCount == measured.PartCount && out.LightCount == measured.LightCount;
		}
		using namespace gpu_helpers;
		SDL_GPUGraphicsPipeline *Pipeline(
			SDL_GPUDevice *device,
			SDL_GPUShader *vertex,
			SDL_GPUShader *fragment,
			SDL_GPUTextureFormat format,
			uint32_t targets,
			bool geometry,
			bool edges,
			uint32_t cull,
			bool mirrored,
			uint32_t blend,
			SourceCamera3DResources &resources
		) {
			SDL_GPUVertexBufferDescription buffer{};
			buffer.slot = 0;
			buffer.pitch = sizeof(Vertex);
			buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
			SDL_GPUVertexAttribute attributes[] = {
				{0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(Vertex, Position)},
				{1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(Vertex, Normal)},
				{2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(Vertex, UV)},
				{3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(Vertex, Color)},
				{4, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(Vertex, Barycentric)}
			};
			std::array<SDL_GPUColorTargetDescription, 4> colors{};
			for (uint32_t i = 0; i < targets; ++i) {
				colors[i].format = format;
				if (blend < 2) {
					colors[i].blend_state.enable_blend = true;
					colors[i].blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
					colors[i].blend_state.dst_color_blendfactor =
						blend ? SDL_GPU_BLENDFACTOR_ONE : SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
					colors[i].blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
					colors[i].blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
					colors[i].blend_state.dst_alpha_blendfactor =
						blend ? SDL_GPU_BLENDFACTOR_ONE : SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
					colors[i].blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
				}
			}
			SDL_GPUGraphicsPipelineCreateInfo info{};
			info.vertex_shader = vertex;
			info.fragment_shader = fragment;
			info.primitive_type = edges ? SDL_GPU_PRIMITIVETYPE_LINELIST : SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
			if (geometry) {
				info.vertex_input_state = {&buffer, 1, attributes, 5};
				info.rasterizer_state.cull_mode = cull == 0 ? SDL_GPU_CULLMODE_NONE : SDL_GPU_CULLMODE_BACK;
				info.rasterizer_state.front_face = (cull == 1) ^ mirrored
													   ? SDL_GPU_FRONTFACE_CLOCKWISE
													   : SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
				info.depth_stencil_state.enable_depth_test = blend != 1;
				info.depth_stencil_state.enable_depth_write = true;
				info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
				info.target_info.has_depth_stencil_target = true;
				info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
			}
			info.target_info.color_target_descriptions = colors.data();
			info.target_info.num_color_targets = targets;
			auto *pipeline = SDL_CreateGPUGraphicsPipeline(device, &info);
			if (pipeline) resources.Pipelines.push_back(pipeline);
			return pipeline;
		}
		SDL_GPURenderPass *BeginPass(
			SDL_GPUCommandBuffer *command,
			std::span<SDL_GPUTexture *const> textures,
			SDL_GPUTexture *depth = nullptr,
			uint32_t layer = 0
		) {
			std::array<SDL_GPUColorTargetInfo, 4> colors{};
			for (size_t i = 0; i < textures.size(); ++i) {
				colors[i].texture = textures[i];
				colors[i].layer_or_depth_plane = layer;
				colors[i].load_op = SDL_GPU_LOADOP_CLEAR;
				colors[i].store_op = SDL_GPU_STOREOP_STORE;
			}
			SDL_GPUDepthStencilTargetInfo d{};
			d.texture = depth;
			d.clear_depth = 1;
			d.load_op = SDL_GPU_LOADOP_CLEAR;
			d.store_op = SDL_GPU_STOREOP_STORE;
			d.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
			d.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
			return SDL_BeginGPURenderPass(command, colors.data(), textures.size(), depth ? &d : nullptr);
		}
	}
	SourceCamera3DStatus
	ValidateSourceCamera3D(const SourceCamera3DRequest &request, uint64_t maximumPreparationBytes) try {
		if (request.Width == 0 || request.Height == 0 || request.Width > 4096 || request.Height > 4096)
			return SourceCamera3DStatus::OutputLimit;
		if (request.Shader > 1 || request.CullMode > 2 || request.BlendMode > 1 || request.WireMode > 2 ||
			uint8_t(request.Output) > uint8_t(SourceCamera3DOutput::AmbientOcclusion) ||
			request.AoBlur > 256 || request.RoundNormal > 256)
			return SourceCamera3DStatus::InvalidControl;
		for (float v : request.View)
			if (!std::isfinite(v)) return SourceCamera3DStatus::InvalidControl;
		for (float v : request.Projection)
			if (!std::isfinite(v)) return SourceCamera3DStatus::InvalidControl;
		for (float v : request.CameraPosition)
			if (!std::isfinite(v)) return SourceCamera3DStatus::InvalidControl;
		for (const auto *color : {&request.AmbientLight, &request.WireColor, &request.BackfaceBlending})
			for (float v : *color)
				if (!std::isfinite(v)) return SourceCamera3DStatus::InvalidControl;
		for (float v :
			 {request.Near,
			  request.Far,
			  request.AlphaThreshold,
			  request.WireThickness,
			  request.AoStrength,
			  request.AoRadius,
			  request.AoBias})
			if (!std::isfinite(v)) return SourceCamera3DStatus::InvalidControl;
		if (request.Near <= 0 || request.Far <= request.Near || request.WireThickness < 0 ||
			request.AoRadius < 0 || request.AoStrength < 0 || !detail::TextureFormatForUpload(request.Format))
			return SourceCamera3DStatus::InvalidControl;
		if (request.Environment && !SurfaceValid(*request.Environment))
			return SourceCamera3DStatus::InvalidScene;
		const glm::mat4 view = glm::make_mat4(request.View.data()),
						projection = glm::make_mat4(request.Projection.data());
		if (glm::determinant(view) == 0 || glm::determinant(projection) == 0)
			return SourceCamera3DStatus::InvalidControl;
		Prepared prepared;
		if (!Measure(request, prepared, maximumPreparationBytes))
			return prepared.LimitExceeded ? SourceCamera3DStatus::OutputLimit
										  : SourceCamera3DStatus::InvalidScene;
		const uint64_t pixels = uint64_t(request.Width) * request.Height;
		const auto support = detail::TextureFormatForUpload(request.Format);
		const uint64_t scratch =
			pixels * (64 + 16 + 12 +
					  (request.Format == assets::TextureFormat::RGBA8 ||
							   request.Format == assets::TextureFormat::RGBA4_SRGB
						   ? 16
						   : 9) *
						  support->UploadBytesPerPixel) +
			2 * prepared.VertexCount * sizeof(Vertex) +
			2 * (prepared.SourceBytes + (request.Environment ? request.Environment->Pixels.capacity() : 0)) +
			prepared.ShadowBytes;
		if (pixels * support->UploadBytesPerPixel > 64ull * 1024 * 1024 || scratch > 128ull * 1024 * 1024)
			return SourceCamera3DStatus::OutputLimit;
		return SourceCamera3DStatus::Ok;
	} catch (const std::bad_alloc &) {
		return SourceCamera3DStatus::OutputLimit;
	}
	uint64_t SourceCamera3DSourceBytes(const SourceCamera3DRequest &request) try {
		Prepared prepared;
		if (!Measure(request, prepared)) return UINT64_MAX;
		return prepared.SourceBytes + (request.Environment ? request.Environment->Pixels.capacity() : 0) +
			   sizeof(request);
	} catch (const std::bad_alloc &) {
		return UINT64_MAX;
	}
	uint64_t SourceCamera3DScratchBytes(const SourceCamera3DRequest &request) try {
		Prepared prepared;
		if (!Measure(request, prepared)) return UINT64_MAX;
		const auto support = detail::TextureFormatForUpload(request.Format);
		if (!support) return UINT64_MAX;
		return uint64_t(request.Width) * request.Height *
				   (64 + 16 + 12 +
					(request.Format == assets::TextureFormat::RGBA8 ||
							 request.Format == assets::TextureFormat::RGBA4_SRGB
						 ? 16
						 : 9) *
						support->UploadBytesPerPixel) +
			   2 * prepared.VertexCount * sizeof(Vertex) +
			   2 * (prepared.SourceBytes +
					(request.Environment ? request.Environment->Pixels.capacity() : 0)) +
			   prepared.ShadowBytes;
	} catch (const std::bad_alloc &) {
		return UINT64_MAX;
	}
	void ReleaseSourceCamera3D(SDL_GPUDevice *device, SourceCamera3DResources &resources) {
		if (!device) return;
		for (auto *texture : resources.Outputs)
			gpu::ReleaseTexture(device, texture);
		for (auto *p : resources.Pipelines)
			SDL_ReleaseGPUGraphicsPipeline(device, p);
		for (auto *p : resources.Shaders)
			SDL_ReleaseGPUShader(device, p);
		for (auto *p : resources.Samplers)
			SDL_ReleaseGPUSampler(device, p);
		for (auto *p : resources.Textures)
			gpu::ReleaseTexture(device, p);
		for (auto *p : resources.Buffers)
			gpu::ReleaseBuffer(device, p);
		for (auto *p : resources.Transfers)
			gpu::ReleaseTransferBuffer(device, p);
		resources = {};
	}
	bool RecordSourceCamera3D(
		SDL_GPUDevice *device,
		SDL_GPUCommandBuffer *command,
		const SourceCamera3DRequest &request,
		SourceCamera3DResources &resources,
		bool captureReadback
	) try {
		ENGINE_PROFILE("imagegraph source camera record");
		if (!device || !command || ValidateSourceCamera3D(request) != SourceCamera3DStatus::Ok) return false;
		assets::TextureFormat storageFormat = request.Format;
		if (storageFormat == assets::TextureFormat::RGBA8)
			storageFormat = assets::TextureFormat::RGBA8_LINEAR;
		if (storageFormat == assets::TextureFormat::RGBA4_SRGB)
			storageFormat = assets::TextureFormat::RGBA4_UNORM;
		const auto support = detail::TextureFormatForUpload(storageFormat);
		if (!support || !SDL_GPUTextureSupportsFormat(
							device,
							support->DeviceFormat,
							SDL_GPU_TEXTURETYPE_2D,
							SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER
						))
			return false;
		Prepared prepared;
		if (!Prepare(request, prepared)) return false;
		// Reserve ownership lists before the first device allocation so a failed vector growth cannot orphan
		// a handle.
		resources.Textures.reserve(prepared.Parts.size() * 3 + 64);
		resources.Transfers.reserve(prepared.Parts.size() * 3 + 32);
		resources.Buffers.reserve(1);
		resources.Shaders.reserve(4);
		resources.Samplers.reserve(2);
		resources.Pipelines.reserve(128);
		auto *vertexShader =
			Shader(device, "imagegraph-camera-3d.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, resources);
		auto *fragmentShader =
			Shader(device, "imagegraph-camera-3d.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 8, resources, 3);
		auto *postVertex =
			Shader(device, "imagegraph-camera-post.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, resources);
		auto *postFragment =
			Shader(device, "imagegraph-camera-post.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 3, resources);
		auto *nearest = Sampler(device, false, resources), *linear = Sampler(device, true, resources);
		if (!vertexShader || !fragmentShader || !postVertex || !postFragment || !nearest || !linear)
			return false;
		const uint32_t width = request.Width, height = request.Height;
		std::array<SDL_GPUTexture *, 4> geometry{}, outputs{};
		for (auto &texture : geometry)
			texture = Texture(device, width, height, SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT, resources);
		for (auto &texture : outputs)
			texture = Texture(device, width, height, support->DeviceFormat, resources);
		auto *diffuse = Texture(device, width, height, support->DeviceFormat, resources);
		auto *viewNormal = Texture(device, width, height, support->DeviceFormat, resources);
		auto *rendered = Texture(device, width, height, support->DeviceFormat, resources);
		auto *aoTemp = Texture(device, width, height, support->DeviceFormat, resources);
		auto *ao = Texture(device, width, height, support->DeviceFormat, resources);
		auto *normalBlur =
			Texture(device, width, height, SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT, resources);
		auto *depth = Texture(device, width, height, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, resources, 1, true);
		auto *white = Upload(device, command, nullptr, {1, 1, 1, 1}, resources);
		auto *black = Upload(device, command, nullptr, {0, 0, 0, 1}, resources);
		auto *environment = request.Environment
								? Upload(device, command, &*request.Environment, {1, 1, 1, 1}, resources)
								: white;
		if (!diffuse || !viewNormal || !rendered || !aoTemp || !ao || !normalBlur || !depth || !white ||
			!black || !environment ||
			std::find(geometry.begin(), geometry.end(), nullptr) != geometry.end() ||
			std::find(outputs.begin(), outputs.end(), nullptr) != outputs.end())
			return false;
		SDL_GPUBuffer *vertices = nullptr;
		if (!prepared.Vertices.empty()) {
			const uint32_t bytes = prepared.VertexCount * sizeof(Vertex);
			SDL_GPUBufferCreateInfo info{};
			info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
			info.size = bytes;
			vertices = gpu::CreateBuffer(device, &info);
			if (!vertices) return false;
			resources.Buffers.push_back(vertices);
			auto *upload = Transfer(device, bytes, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, resources);
			if (!upload) return false;
			auto *mapped = SDL_MapGPUTransferBuffer(device, upload, false);
			if (!mapped) return false;
			std::memcpy(mapped, prepared.Vertices.data(), bytes);
			SDL_UnmapGPUTransferBuffer(device, upload);
			auto *copy = SDL_BeginGPUCopyPass(command);
			if (!copy) return false;
			SDL_GPUTransferBufferLocation from{};
			from.transfer_buffer = upload;
			SDL_GPUBufferRegion to{};
			to.buffer = vertices;
			to.size = bytes;
			SDL_UploadToGPUBuffer(copy, &from, &to, false);
			core::Metrics::Count("render.imagegraph_camera.upload_bytes", bytes);
			SDL_EndGPUCopyPass(copy);
			resources.CommandReferenced = true;
		}
		struct DrawMaterial {
			SDL_GPUTexture *Surface = nullptr, *Normal = nullptr, *Properties = nullptr;
			const source::MaterialData3D *Source = nullptr;
		};
		std::vector<DrawMaterial> materials;
		materials.reserve(prepared.Parts.size());
		for (const auto &part : prepared.Parts) {
			const auto *m = part.Material;
			const auto prior = std::find_if(materials.begin(), materials.end(), [m](const auto &material) {
				return material.Source == m;
			});
			if (prior != materials.end()) {
				materials.push_back(*prior);
				continue;
			}
			DrawMaterial material{white, black, black, m};
			if (m) {
				if (m->Surface)
					material.Surface = Upload(device, command, &*m->Surface, {1, 1, 1, 1}, resources);
				if (m->Normal)
					material.Normal = Upload(device, command, &*m->Normal, {0, 0, 0, 1}, resources);
				if (m->PropertiesMap)
					material.Properties =
						Upload(device, command, &*m->PropertiesMap, {0, 0, 0, 1}, resources);
			}
			if (!material.Surface || !material.Normal || !material.Properties) return false;
			materials.push_back(material);
		}
		SourceCameraUniforms base{};
		base.light_ambient = glm::make_vec4(request.AmbientLight.data());
		base.shader = request.Shader;
		base.cameraPosition = glm::make_vec3(request.CameraPosition.data());
		base.gammaCorrection = request.GammaAdjust;
		base.alphaThreshold = request.AlphaThreshold;
		base.env_use_mapping = request.Environment.has_value();
		base.env_map_dimension = request.Environment
									 ? glm::vec2(request.Environment->Width, request.Environment->Height)
									 : glm::vec2(1);
		base.viewProjMat = glm::make_mat4(request.Projection.data()) * glm::make_mat4(request.View.data());
		base.show_wireframe = request.WireMode == 1;
		base.wireframe_aa = request.WireAntialias;
		base.wireframe_shade = request.WireShading;
		base.wireframe_only = request.WireOnly;
		base.wireframe_width = request.WireThickness;
		base.wireframe_color = glm::make_vec4(request.WireColor.data());
		base.backface_blending = glm::make_vec4(request.BackfaceBlending.data());
		base.obj_color = glm::vec4(1);
		std::array<SDL_GPUTexture *, 2> directionalShadow{white, white}, pointShadow{};
		for (auto &texture : pointShadow) {
			texture = Texture(device, 1, 1, SDL_GPU_TEXTUREFORMAT_R32_FLOAT, resources, 6);
			if (!texture) return false;
		}
		// Every unused layer is cleared before any shader can sample it.
		for (auto *texture : pointShadow)
			for (uint32_t layer = 0; layer < 6; ++layer) {
				auto *pass = BeginPass(command, std::span(&texture, 1), nullptr, layer);
				if (!pass) return false;
				SDL_EndGPURenderPass(pass);
				resources.CommandReferenced = true;
			}
		uint32_t dirShadowCount = 0, pointShadowCount = 0;
		struct ShadowView {
			SDL_GPUTexture *Texture = nullptr, *Depth = nullptr;
			uint32_t Layer = 0;
			glm::mat4 View{}, Projection{};
			uint32_t Side = 0;
		};
		std::vector<ShadowView> shadows;
		for (const auto &light : prepared.Lights) {
			const auto &data = *light.Data;
			if (data.Kind == source::LightKind3D::Directional) {
				const uint32_t i = base.light_dir_count++;
				base.light_dir_direction[i].Value = light.Position;
				base.light_dir_color[i].Value = Color(data.Color);
				base.light_dir_intensity[i].Value = data.Intensity;
				base.light_dir_shadow_active[i].Value = data.CastShadow;
				base.light_dir_shadow_bias[i].Value = data.ShadowBias;
				const glm::mat4 view =
					glm::lookAtLH(Vec(data.Transform.Position), glm::vec3(0), glm::vec3(0, 0, -1));
				const glm::mat4 projection = glm::orthoLH_ZO(
					-float(data.ShadowMapScale) / 2,
					float(data.ShadowMapScale) / 2,
					-float(data.ShadowMapScale) / 2,
					float(data.ShadowMapScale) / 2,
					.01f,
					100.f
				);
				base.light_dir_view[i].Value = view;
				base.light_dir_proj[i].Value = projection;
				if (data.CastShadow) {
					auto *texture = Texture(
						device,
						data.ShadowMapSize,
						data.ShadowMapSize,
						SDL_GPU_TEXTUREFORMAT_R32_FLOAT,
						resources
					);
					auto *shadowDepth = Texture(
						device,
						data.ShadowMapSize,
						data.ShadowMapSize,
						SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
						resources,
						1,
						true
					);
					if (!texture || !shadowDepth) return false;
					directionalShadow[dirShadowCount++] = texture;
					shadows.push_back({texture, shadowDepth, 0, view, projection});
				}
			} else {
				const uint32_t i = base.light_pnt_count++;
				base.light_pnt_position[i].Value = light.Position;
				base.light_pnt_color[i].Value = Color(data.Color);
				base.light_pnt_intensity[i].Value = data.Intensity;
				base.light_pnt_radius[i].Value = data.Radius;
				base.light_pnt_shadow_active[i].Value = data.CastShadow;
				base.light_pnt_shadow_bias[i].Value = data.ShadowBias;
				const glm::mat4 projection =
					glm::perspectiveLH_ZO(glm::radians(90.f), 1.f, .01f, float(data.Radius));
				base.light_pnt_proj[i].Value = projection;
				SDL_GPUTexture *texture = nullptr, *shadowDepth = nullptr;
				if (data.CastShadow) {
					texture = Texture(
						device,
						data.ShadowMapSize,
						data.ShadowMapSize,
						SDL_GPU_TEXTUREFORMAT_R32_FLOAT,
						resources,
						6
					);
					shadowDepth = Texture(
						device,
						data.ShadowMapSize,
						data.ShadowMapSize,
						SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
						resources,
						1,
						true
					);
					if (!texture || !shadowDepth) return false;
					pointShadow[pointShadowCount++] = texture;
				}
				constexpr std::array<glm::vec3, 6> directions{
					{{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}
				};
				for (uint32_t side = 0; side < 6; ++side) {
					const glm::vec3 up = side == 4	 ? glm::vec3(0, 1, 0)
										 : side == 5 ? glm::vec3(0, -1, 0)
													 : glm::vec3(0, 0, -1);
					const auto view = glm::lookAtLH(
						Vec(data.Transform.Position), Vec(data.Transform.Position) + directions[side], up
					);
					base.light_pnt_view[i * 6 + side].Value = view;
					if (data.CastShadow)
						shadows.push_back({texture, shadowDepth, side, view, projection, side});
				}
			}
		}
		auto draw = [&](std::span<SDL_GPUTexture *const> targets,
						SDL_GPUTexture *depthTexture,
						SDL_GPUTextureFormat format,
						VertexUniforms vertexUniforms,
						int32_t nativePass,
						uint32_t layer = 0) -> bool {
			// Pipeline creation precedes opening the render pass. No device allocation is made while
			// recording draws.
			std::array<SDL_GPUGraphicsPipeline *, 4> pipelines{};
			for (size_t mirrored = 0; mirrored < 2; ++mirrored)
				for (size_t edge = 0; edge < 2; ++edge) {
					pipelines[mirrored * 2 + edge] = Pipeline(
						device,
						vertexShader,
						fragmentShader,
						format,
						targets.size(),
						true,
						edge,
						nativePass == 3 ? 2 : request.CullMode,
						mirrored,
						nativePass == 0 ? request.BlendMode : 2,
						resources
					);
					if (!pipelines[mirrored * 2 + edge]) return false;
				}
			auto *pass = BeginPass(command, targets, depthTexture, layer);
			if (!pass) return false;
			resources.CommandReferenced = true;
			SDL_PushGPUVertexUniformData(command, 0, &vertexUniforms, sizeof(vertexUniforms));
			if (vertices) {
				SDL_GPUBufferBinding binding{};
				binding.buffer = vertices;
				SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
			}
			for (size_t p = 0; p < prepared.Parts.size(); ++p) {
				const auto &part = prepared.Parts[p];
				if (part.Count == 0 || part.Shadow != (nativePass == 3)) continue;
				if (part.Edge && (nativePass != 0 || request.WireMode != 2)) continue;
				if (!part.Edge && nativePass == 0 && request.WireMode == 2) continue;
				auto uniforms = base;
				if (part.Instanced) uniforms.shader = 0;
				uniforms.NativePass = part.Edge ? 4 : nativePass;
				const auto &material = materials[p];
				uniforms.mat_texDimension =
					material.Source && material.Source->Surface
						? glm::vec2(material.Source->Surface->Width, material.Source->Surface->Height)
						: glm::vec2(1);
				uniforms.mat_texScale = material.Source ? Vec(material.Source->TextureScale) : glm::vec2(1);
				uniforms.mat_texShift = material.Source ? Vec(material.Source->TextureShift) : glm::vec2(0);
				uniforms.mat_texInterpolate = material.Source ? material.Source->TextureFilter : 0;
				uniforms.mat_diffuse = material.Source ? material.Source->Diffuse : 1;
				uniforms.mat_specular = material.Source ? material.Source->Specular : 0;
				uniforms.mat_shine = material.Source ? material.Source->Shininess : 1;
				uniforms.mat_metalic = material.Source && material.Source->Metal;
				uniforms.mat_reflective = material.Source ? material.Source->Reflectance : 0;
				uniforms.mat_normal_strength = material.Source ? material.Source->NormalStrength : 1;
				uniforms.UseMaterialNormal = material.Source && material.Source->Normal.has_value();
				uniforms.mat_pbr_metalic =
					material.Source ? Vec(material.Source->MetallicRange) : glm::vec2(0);
				uniforms.mat_pbr_roughness =
					material.Source ? Vec(material.Source->RoughnessRange) : glm::vec2(1);
				uniforms.mat_pbr_metalic_use_map = material.Source && material.Source->MetallicMapped;
				uniforms.mat_pbr_roughness_use_map = material.Source && material.Source->RoughnessMapped;
				// Source deferred normal mapping is screen-space. Its geometry pass has already applied the
				// material normal input.
				uniforms.mat_defer_normal = nativePass == 0;
				SDL_GPUTextureSamplerBinding samplers[] = {
					{material.Surface, uniforms.mat_texInterpolate == 1 ? linear : nearest},
					{nativePass == 0 ? normalBlur : material.Normal, nearest},
					{material.Properties, nearest},
					{environment, nearest},
					{directionalShadow[0], nearest},
					{directionalShadow[1], nearest},
					{pointShadow[0], nearest},
					{pointShadow[1], nearest}
				};
				SDL_BindGPUGraphicsPipeline(pass, pipelines[part.Mirrored * 2 + part.Edge]);
				SDL_BindGPUFragmentSamplers(pass, 0, samplers, 8);
				if (!PushSourceFragmentUniforms(
						command, std::as_bytes(std::span(&uniforms, 1)), CAMERA_UNIFORM_CUTS
					)) {
					SDL_EndGPURenderPass(pass);
					return false;
				}
				SDL_DrawGPUPrimitives(pass, part.Count, 1, part.First, 0);
			}
			SDL_EndGPURenderPass(pass);
			return true;
		};
		for (const auto &shadow : shadows)
			if (!draw(
					std::span(&shadow.Texture, 1),
					shadow.Depth,
					SDL_GPU_TEXTUREFORMAT_R32_FLOAT,
					{shadow.View, shadow.Projection, {.01f, 100, 0, 0}},
					3,
					shadow.Layer
				))
				return false;
		const VertexUniforms camera{
			glm::make_mat4(request.View.data()),
			glm::make_mat4(request.Projection.data()),
			{request.Near, request.Far, 0, 0}
		};
		if (!draw(geometry, depth, SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT, camera, 1)) return false;
		auto post = [&](SDL_GPUTexture *target,
						SDL_GPUTextureFormat format,
						std::array<SDL_GPUTexture *, 3> sources,
						PostUniforms uniforms) -> bool {
			auto *pipeline =
				Pipeline(device, postVertex, postFragment, format, 1, false, false, 0, false, 2, resources);
			if (!pipeline) return false;
			auto *pass = BeginPass(command, std::span(&target, 1));
			if (!pass) return false;
			resources.CommandReferenced = true;
			SDL_BindGPUGraphicsPipeline(pass, pipeline);
			SDL_GPUTextureSamplerBinding bindings[3]{
				{sources[0], nearest}, {sources[1], nearest}, {sources[2], nearest}
			};
			SDL_BindGPUFragmentSamplers(pass, 0, bindings, 3);
			SDL_PushGPUFragmentUniformData(command, 0, &uniforms, sizeof(uniforms));
			SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
			SDL_EndGPURenderPass(pass);
			return true;
		};
		PostUniforms postUniforms{};
		postUniforms.Projection = base.viewProjMat;
		postUniforms.InverseViewProjection = glm::inverse(base.viewProjMat);
		postUniforms.CameraPosition = glm::vec4(base.cameraPosition, 1);
		postUniforms.Ambient = base.light_ambient;
		postUniforms.Dimension = {width, height};
		postUniforms.Mode = 4;
		postUniforms.Radius = request.RoundNormal;
		if (!post(
				normalBlur,
				SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT,
				{geometry[2], white, white},
				postUniforms
			))
			return false;
		if (!draw(outputs, depth, support->DeviceFormat, camera, 0) ||
			!draw(std::span(&diffuse, 1), depth, support->DeviceFormat, camera, 2))
			return false;
		postUniforms.Mode = 2;
		postUniforms.Radius = request.AoRadius;
		postUniforms.Bias = request.AoBias;
		postUniforms.Strength = request.AoStrength * 2;
		if (request.AmbientOcclusion &&
			!post(aoTemp, support->DeviceFormat, {geometry[0], normalBlur, white}, postUniforms))
			return false;
		postUniforms.Mode = 3;
		postUniforms.Radius = request.AoBlur;
		if (request.AmbientOcclusion) {
			if (!post(ao, support->DeviceFormat, {aoTemp, normalBlur, white}, postUniforms)) return false;
		} else {
			auto *pass = BeginPass(command, std::span(&ao, 1));
			if (!pass) return false;
			SDL_EndGPURenderPass(pass);
		}
		postUniforms.Mode = 1;
		postUniforms.SwapX = request.SwapViewNormalX;
		if (!post(viewNormal, support->DeviceFormat, {geometry[3], white, white}, postUniforms)) return false;
		postUniforms.Mode = 0;
		postUniforms.ShowBackground = request.ShowBackground;
		postUniforms.EnvironmentMapping = request.Environment.has_value();
		postUniforms.AoEnabled = request.AmbientOcclusion;
		if (!post(rendered, support->DeviceFormat, {outputs[0], ao, environment}, postUniforms)) return false;
		resources.Outputs = {rendered, diffuse, outputs[1], viewNormal, outputs[2], outputs[3], ao};
		for (auto *texture : resources.Outputs)
			std::erase(resources.Textures, texture);
		if (request.Format == assets::TextureFormat::RGBA8 ||
			request.Format == assets::TextureFormat::RGBA4_SRGB) {
			const auto publishedFormat = detail::TextureFormatForUpload(request.Format);
			postUniforms.Mode = 5;
			for (auto *&output : resources.Outputs) {
				auto *interpreted = Texture(device, width, height, publishedFormat->DeviceFormat, resources);
				if (!interpreted ||
					!post(interpreted, publishedFormat->DeviceFormat, {output, white, white}, postUniforms))
					return false;
				std::erase(resources.Textures, interpreted);
				resources.Textures.push_back(output);
				output = interpreted;
			}
		}
		resources.Output = resources.Outputs[size_t(request.Output)];
		for (auto *texture : resources.Outputs)
			std::erase(resources.Textures, texture);
		if (captureReadback) {
			const uint64_t bytes = uint64_t(width) * height * support->UploadBytesPerPixel;
			if (bytes > UINT32_MAX) return false;
			for (auto &download : resources.Downloads) {
				download = Transfer(device, uint32_t(bytes), SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, resources);
				if (!download) return false;
			}
			auto *copy = SDL_BeginGPUCopyPass(command);
			if (!copy) return false;
			resources.CommandReferenced = true;
			for (size_t index = 0; index < resources.Outputs.size(); ++index) {
				SDL_GPUTextureRegion from{};
				from.texture = resources.Outputs[index];
				from.w = width;
				from.h = height;
				from.d = 1;
				SDL_GPUTextureTransferInfo to{};
				to.transfer_buffer = resources.Downloads[index];
				to.pixels_per_row = width;
				to.rows_per_layer = height;
				SDL_DownloadFromGPUTexture(copy, &from, &to);
			}
			SDL_EndGPUCopyPass(copy);
			core::Metrics::Count("render.camera.host_download_bytes", bytes * 7);
			core::Metrics::Count("render.camera.host_downloads", 7);
		}
		return resources.Output != nullptr;
	} catch (const std::bad_alloc &) {
		return false;
	}
	SourceCamera3DStatus ExecuteSourceCamera3D(
		Renderer &renderer, const SourceCamera3DRequest &request, SourceCamera3DResult &result
	) {
		const auto validation = ValidateSourceCamera3D(request);
		if (validation != SourceCamera3DStatus::Ok) return validation;
		auto *device = static_cast<SDL_GPUDevice *>(renderer.Backend().Device);
		if (!device) return SourceCamera3DStatus::GpuUnavailable;
		SourceCamera3DResources resources;
		auto *command = SDL_AcquireGPUCommandBuffer(device);
		if (!command) return SourceCamera3DStatus::GpuUnavailable;
		if (!RecordSourceCamera3D(device, command, request, resources)) {
			SDL_CancelGPUCommandBuffer(command);
			ReleaseSourceCamera3D(device, resources);
			return SourceCamera3DStatus::GpuUnavailable;
		}
		const auto support = detail::TextureFormatForUpload(request.Format);
		const uint32_t bytes = uint64_t(request.Width) * request.Height * support->UploadBytesPerPixel;
		auto *download = Transfer(device, bytes, SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, resources);
		if (!download) {
			SDL_CancelGPUCommandBuffer(command);
			ReleaseSourceCamera3D(device, resources);
			return SourceCamera3DStatus::GpuUnavailable;
		}
		auto *copy = SDL_BeginGPUCopyPass(command);
		if (!copy) {
			SDL_CancelGPUCommandBuffer(command);
			ReleaseSourceCamera3D(device, resources);
			return SourceCamera3DStatus::GpuUnavailable;
		}
		SDL_GPUTextureRegion from{};
		from.texture = resources.Output;
		from.w = request.Width;
		from.h = request.Height;
		from.d = 1;
		SDL_GPUTextureTransferInfo to{};
		to.transfer_buffer = download;
		to.pixels_per_row = request.Width;
		to.rows_per_layer = request.Height;
		SDL_DownloadFromGPUTexture(copy, &from, &to);
		SDL_EndGPUCopyPass(copy);
		auto *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
		if (!fence) {
			ReleaseSourceCamera3D(device, resources);
			return SourceCamera3DStatus::GpuUnavailable;
		}
		const bool completed = SDL_WaitForGPUFences(device, true, &fence, 1);
		SDL_ReleaseGPUFence(device, fence);
		if (!completed) {
			ReleaseSourceCamera3D(device, resources);
			return SourceCamera3DStatus::GpuUnavailable;
		}
		void *mapped = SDL_MapGPUTransferBuffer(device, download, false);
		if (!mapped) {
			ReleaseSourceCamera3D(device, resources);
			return SourceCamera3DStatus::GpuUnavailable;
		}
		SourceCamera3DResult next;
		next.Width = request.Width;
		next.Height = request.Height;
		next.Format = request.Format;
		try {
			const auto *pixels = static_cast<const std::byte *>(mapped);
			if (request.Format == assets::TextureFormat::RGBA4_UNORM ||
				request.Format == assets::TextureFormat::RGBA4_SRGB) {
				next.Pixels.resize(uint64_t(request.Width) * request.Height * 2);
				(void)detail::CopyRgba8ToRgba4(std::span(pixels, bytes), next.Pixels);
			} else if (request.Format == assets::TextureFormat::R8) {
				next.Pixels.resize(uint64_t(request.Width) * request.Height);
				for (size_t p = 0; p < next.Pixels.size(); ++p)
					next.Pixels[p] = pixels[p * 4];
			} else
				next.Pixels.assign(pixels, pixels + bytes);
		} catch (const std::bad_alloc &) {
			SDL_UnmapGPUTransferBuffer(device, download);
			ReleaseSourceCamera3D(device, resources);
			return SourceCamera3DStatus::OutputLimit;
		}
		SDL_UnmapGPUTransferBuffer(device, download);
		ReleaseSourceCamera3D(device, resources);
		result = std::move(next);
		return SourceCamera3DStatus::Ok;
	}
}
