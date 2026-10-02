#include "SourceSdf.hpp"

#include "ImageGraphGpuHelpers.hpp"
#include "SourceSdfCloudUniforms.hpp"
#include "SourceSdfScatterUniforms.hpp"
#include "SourceSdfTerrainUniforms.hpp"
#include "SourceSdfUniforms.hpp"
#include "SourceUniformBlocks.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/Surface.hpp>
#include <engine/render/Renderer.hpp>

#include <glm/gtc/type_ptr.hpp>

#include <cmath>
#include <limits>

namespace engine::render::imagegraph {
	namespace {
		namespace source = engine::imagegraph;
		static_assert(SDF_UNIFORM_CUTS[1] == SourceSdfUniforms::corner);
		static_assert(SDF_UNIFORM_CUTS[2] == SourceSdfUniforms::tileShiftRot);
		static_assert(SDF_UNIFORM_CUTS[1] == SourceSdfScatterUniforms::corner);
		static_assert(SDF_UNIFORM_CUTS[2] == SourceSdfScatterUniforms::tileShiftRot);
		using namespace gpu_helpers;
		glm::vec2 Vec(source::Vector2 v) {
			return {v.X, v.Y};
		}
		glm::vec3 Vec(source::Vector3 v) {
			return {v.X, v.Y, v.Z};
		}
		glm::vec4 Vec(source::Vector4 v) {
			return {v.X, v.Y, v.Z, v.W};
		}
		glm::vec4 Vec(source::Colour v) {
			return glm::vec4(v.Red, v.Green, v.Blue, v.Alpha) / 255.f;
		}
		bool HasShapeTextures(const SourceSdfRequest &request) {
			return request.Object.Data && std::any_of(
											  request.Object.Data->Shapes.begin(),
											  request.Object.Data->Shapes.end(),
											  [](const auto &shape) { return shape.Texture.has_value(); }
										  );
		}
		uint32_t ShapeAtlasHeight(const SourceSdfRequest &request, uint32_t size) {
			uint32_t height = 1;
			if (request.Object.Data)
				for (size_t i = 0; i < request.Object.Data->Shapes.size(); ++i)
					if (request.Object.Data->Shapes[i].Texture)
						height = std::min(size, uint32_t(i / 8 + 1) * 1024 + 1);
			return height;
		}
		source::Image Atlas(const SourceSdfRequest &request, bool environment) {
			const uint32_t size = !environment && request.ShapeTextureAtlasSize
									  ? request.ShapeTextureAtlasSize
									  : request.TextureAtlasSize;
			const uint32_t height = environment ? size : ShapeAtlasHeight(request, size);
			source::Image atlas{size, height, std::vector<uint8_t>(uint64_t(size) * height * 4), 0};
			const auto draw = [&](const source::Image &image, uint32_t index, bool filtered) {
				const uint32_t left = index % 8 * 1024, top = index / 8 * 1024;
				for (uint32_t y = 0; y < 1024 && top + y < atlas.Height; ++y)
					for (uint32_t x = 0; x < 1024 && left + x < atlas.Width; ++x) {
						const double px = (x + .5) * image.Width / 1024 - .5,
									 py = (y + .5) * image.Height / 1024 - .5;
						std::array<double, 4> color{};
						const auto sample = [&](int64_t ix, int64_t iy) {
							std::array<double, 4> value{};
							source::LoadSurfacePixel(
								image,
								uint32_t(std::clamp<int64_t>(ix, 0, image.Width - 1)),
								uint32_t(std::clamp<int64_t>(iy, 0, image.Height - 1)),
								value
							);
							return value;
						};
						if (!filtered)
							color = sample(int64_t(std::floor(px + .5)), int64_t(std::floor(py + .5)));
						else {
							const int64_t ix = int64_t(std::floor(px)), iy = int64_t(std::floor(py));
							const double fx = px - ix, fy = py - iy;
							const auto a = sample(ix, iy), b = sample(ix + 1, iy), c = sample(ix, iy + 1),
									   d = sample(ix + 1, iy + 1);
							for (size_t channel = 0; channel < 4; ++channel)
								color[channel] = std::lerp(
									std::lerp(a[channel], b[channel], fx),
									std::lerp(c[channel], d[channel], fx),
									fy
								);
						}
						source::StoreSurfacePixel(atlas, left + x, top + y, color);
					}
			};
			if (environment) {
				if (request.Environment) draw(*request.Environment, 0, request.EnvironmentInterpolation);
			} else if (request.Object.Data)
				for (size_t i = 0; i < request.Object.Data->Shapes.size(); ++i) {
					const auto &shape = request.Object.Data->Shapes[i];
					if (shape.Texture) draw(*shape.Texture, uint32_t(i), shape.TextureInterpolation);
				}
			return atlas;
		}
		SourceSdfUniforms Uniforms(const SourceSdfRequest &request) {
			SourceSdfUniforms uniforms;
			const auto &object = *request.Object.Data;
			const uint32_t atlasSize =
				request.ShapeTextureAtlasSize ? request.ShapeTextureAtlasSize : request.TextureAtlasSize;
			uniforms.Set(
				uniforms.shapeAtlasUvScale,
				std::array<float, 2>{1, float(atlasSize) / ShapeAtlasHeight(request, atlasSize)}
			);
			uniforms.Set(uniforms.shapeAmount, int32_t(object.Shapes.size()));
			uniforms.Set(uniforms.opLength, int32_t(object.Operations.size()));
			for (size_t i = 0; i < object.Operations.size(); ++i) {
				uniforms.Set(uniforms.operations, object.Operations[i].Code, i);
				uniforms.Set(uniforms.opArgument, float(object.Operations[i].Merge), i);
			}
			for (size_t i = 0; i < object.Shapes.size(); ++i) {
				const auto &s = object.Shapes[i];
#define SET(field, value) uniforms.Set(uniforms.field, value, i)
				SET(shape, s.Shape);
				SET(size, Vec(s.Size));
				SET(radius, float(s.Radius));
				SET(thickness, float(s.Thickness));
				SET(crop, float(s.Crop));
				SET(angle, float(s.Angle));
				SET(height, float(s.Height));
				SET(radRange, Vec(s.RadiusRange));
				SET(sizeUni, float(s.UniformSize));
				SET(elongate, Vec(s.Elongate));
				SET(rounded, float(s.Rounded));
				SET(corner, Vec(s.Corner));
				SET(size2D, Vec(s.Size2D));
				SET(sides, s.Sides);
				SET(waveAmp, Vec(s.WaveAmplitude));
				SET(waveInt, Vec(s.WaveIntensity));
				SET(waveShift, Vec(s.WavePhase));
				SET(twistAxis, s.TwistAxis);
				SET(twistAmount, float(s.TwistAmount));
				SET(position, Vec(s.Position));
				SET(rotation, Vec(s.Rotation));
				SET(objectScale, float(s.Scale));
				SET(tileActive, int32_t(s.Tile));
				SET(tileSize, Vec(s.TileDistance));
				SET(tileAmount, Vec(s.TileAmount));
				SET(diffuseColor, Vec(s.Diffuse));
				SET(reflective, float(s.Reflective));
				SET(specular, float(s.Specular));
				SET(useTexture, int32_t(s.Texture.has_value()));
				SET(textureFilter, int32_t(s.TextureInterpolation));
				SET(textureScale, float(s.TextureScale));
				SET(triplanar, float(s.Triplanar));
				SET(volumetric, int32_t(s.Volumetric));
				SET(volumeDensity, float(s.Density));
#undef SET
			}
			uniforms.Set(uniforms.MAX_MARCHING_STEPS, int32_t{512});
			uniforms.Set(uniforms.ortho, int32_t(request.Projection));
			uniforms.Set(uniforms.fov, request.Fov);
			uniforms.Set(uniforms.orthoScale, request.OrthoScale);
			uniforms.Set(uniforms.viewRange, request.ViewRange);
			uniforms.Set(uniforms.depthRange, request.DepthRange);
			uniforms.Set(uniforms.depthInt, request.DepthIntensity);
			uniforms.Set(uniforms.background, request.Background);
			uniforms.Set(uniforms.ambientIntns, request.AmbientIntensity);
			uniforms.Set(uniforms.useLight, int32_t(request.UseLight));
			uniforms.Set(uniforms.lightPosition, request.LightPosition);
			uniforms.Set(uniforms.lightInten, request.LightIntensity);
			uniforms.Set(uniforms.lightColor, request.LightColor);
			uniforms.Set(uniforms.useEnv, int32_t(request.Environment.has_value()));
			uniforms.Set(uniforms.envFilter, int32_t(request.EnvironmentInterpolation));
			uniforms.Set(uniforms.camRotation, request.CameraRotation);
			uniforms.Set(uniforms.camScale, request.CameraScale);
			uniforms.Set(uniforms.camRatio, float(request.Width) / request.Height);
			uniforms.Set(uniforms.drawBg, int32_t(request.DrawBackground));
			return uniforms;
		}

		std::vector<std::byte> EncodedUniforms(const SourceSdfRequest &request) {
			if (request.Mode == SourceRaymarchMode::Render) {
				const auto uniforms = Uniforms(request);
				return {uniforms.Bytes.begin(), uniforms.Bytes.end()};
			}
			if (request.Mode == SourceRaymarchMode::Scatter) {
				SourceSdfScatterUniforms u;
				const auto base = Uniforms(request);
				static_assert(SourceSdfUniforms::camRotation == SourceSdfScatterUniforms::seed);
				std::copy_n(base.Bytes.begin(), SourceSdfUniforms::camRotation, u.Bytes.begin());
				std::copy_n(
					base.Bytes.begin() + SourceSdfUniforms::shapeAtlasUvScale,
					16,
					u.Bytes.begin() + SourceSdfScatterUniforms::shapeAtlasUvScale
				);
				const auto &s = request.Scatter;
				u.Set(u.seed, s.Seed);
				u.Set(u.dimension, std::array<float, 2>{float(request.Width), float(request.Height)});
				u.Set(u.grid, s.Grid);
				u.Set(u.positionOrigin, s.PositionOrigin);
				u.Set(u.positionOffset, s.PositionOffset);
				u.Set(u.rotationOrigin, s.RotationOrigin);
				u.Set(u.rotationMin, s.RotationMinimum);
				u.Set(u.rotationMax, s.RotationMaximum);
				u.Set(u.objScale, s.ObjectScale);
				u.Set(u.camRotation, request.CameraRotation);
				u.Set(u.camScale, request.CameraScale);
				u.Set(u.camRatio, float(request.Width) / request.Height);
				u.Set(u.drawBg, int32_t(request.DrawBackground));
				return {u.Bytes.begin(), u.Bytes.end()};
			}
			if (request.Mode == SourceRaymarchMode::Cloud) {
				SourceSdfCloudUniforms u;
				const auto &c = request.Cloud;
				u.Set(u.gradient_blend, int32_t(c.GradientBlend));
				u.Set(u.gradient_keys, int32_t(c.Gradient.Keys.size()));
				u.Set(u.gradient_use_map, int32_t(c.GradientMap.has_value()));
				u.Set(u.gradient_map_range, c.GradientMapRange);
				for (size_t i = 0; i < c.Gradient.Keys.size(); ++i) {
					u.Set(u.gradient_color, Vec(c.Gradient.Keys[i].Color), i);
					u.Set(u.gradient_time, float(c.Gradient.Keys[i].Time), i);
				}
				u.Set(u.dimension, std::array<float, 2>{float(request.Width), float(request.Height)});
				u.Set(u.position, c.Position);
				u.Set(u.rotation, c.Rotation);
				u.Set(u.objectScale, c.ObjectScale);
				u.Set(u.fov, c.Fov);
				u.Set(u.viewRange, c.ViewRange);
				u.Set(u.type, int32_t(c.Type));
				u.Set(u.density, c.Density);
				u.Set(u.iteration, int32_t(c.Iteration));
				u.Set(u.threshold, c.Threshold);
				u.Set(u.fogUse, int32_t(c.Fog));
				u.Set(u.detailScale, c.DetailScale);
				u.Set(u.detailAtten, c.DetailAttenuation);
				return {u.Bytes.begin(), u.Bytes.end()};
			}
			SourceSdfTerrainUniforms u;
			const auto &t = request.Terrain;
			u.Set(u.atlasUvScale, t.AtlasUvScale);
			u.Set(u.useTexture, int32_t(t.UseTexture));
			u.Set(u.shape, int32_t(t.Shape));
			u.Set(u.tile, int32_t(t.Tile));
			u.Set(u.thickness, t.Thickness);
			u.Set(u.time, t.Time);
			u.Set(u.position, t.Position);
			u.Set(u.rotation, t.Rotation);
			u.Set(u.objectScale, t.ObjectScale);
			u.Set(u.fov, t.Fov);
			u.Set(u.viewRange, t.ViewRange);
			u.Set(u.depthInt, t.DepthIntensity);
			u.Set(u.background, t.Background);
			u.Set(u.ambient, t.Ambient);
			u.Set(u.sunPosition, t.SunPosition);
			u.Set(u.shadow, t.Shadow);
			return {u.Bytes.begin(), u.Bytes.end()};
		}
	}
	SourceSdfStatus ValidateSourceSdfRequest(const SourceSdfRequest &request) try {
		if (uint8_t(request.Mode) > uint8_t(SourceRaymarchMode::Terrain))
			return SourceSdfStatus::InvalidControl;
		if (request.Mode == SourceRaymarchMode::Render || request.Mode == SourceRaymarchMode::Scatter) {
			if (!request.Object.Data || !source::ValidateSourceSdfValue(request.Object))
				return SourceSdfStatus::InvalidObject;
		} else if (request.Mode == SourceRaymarchMode::Cloud) {
			if (request.Cloud.Gradient.Keys.empty() || request.Cloud.Gradient.Keys.size() > 64 ||
				request.Cloud.Iteration < 1 || request.Cloud.Iteration > 4096 || request.Cloud.Type > 2 ||
				request.Cloud.ObjectScale == 0 || request.Cloud.Fov <= 0 || request.Cloud.Fov >= 180 ||
				request.Cloud.GradientBlend > 6 || request.Cloud.ViewRange[1] <= request.Cloud.ViewRange[0])
				return SourceSdfStatus::InvalidControl;
		} else if (request.Terrain.ObjectScale == 0 || request.Terrain.Fov <= 0 ||
				   request.Terrain.Fov >= 180 || request.Terrain.AtlasUvScale[0] <= 0 ||
				   request.Terrain.AtlasUvScale[1] <= 0 ||
				   request.Terrain.ViewRange[1] <= request.Terrain.ViewRange[0])
			return SourceSdfStatus::InvalidControl;
		if (request.Mode == SourceRaymarchMode::Scatter &&
			(request.Scatter.Grid[0] <= 0 || request.Scatter.Grid[1] <= 0 ||
			 request.Scatter.ObjectScale[0] <= 0 || request.Scatter.ObjectScale[1] <= 0))
			return SourceSdfStatus::InvalidControl;
		if (!request.Width || !request.Height || request.Width > source::Limits::MaximumDimension ||
			request.Height > source::Limits::MaximumDimension)
			return SourceSdfStatus::OutputLimit;
		if (!request.TextureAtlasSize || request.TextureAtlasSize > 8192 ||
			request.ShapeTextureAtlasSize > 8192 || SourceSdfScratchBytes(request) > 256ull * 1024 * 1024)
			return SourceSdfStatus::OutputLimit;
		const auto support = engine::render::detail::TextureFormatForUpload(request.Format);
		if (!support) return SourceSdfStatus::InvalidControl;
		if (request.Projection > 1 || request.CameraScale <= 0 || request.Fov <= 0 || request.Fov >= 180 ||
			request.OrthoScale <= 0 || request.ViewRange[0] < 0 ||
			request.ViewRange[1] <= request.ViewRange[0] || request.DepthRange[0] == request.DepthRange[1])
			return SourceSdfStatus::InvalidControl;
		for (float value : request.CameraRotation)
			if (!std::isfinite(value)) return SourceSdfStatus::InvalidControl;
		for (const auto *color : {&request.Background, &request.LightColor})
			for (float value : *color)
				if (!std::isfinite(value)) return SourceSdfStatus::InvalidControl;
		for (float value : request.LightPosition)
			if (!std::isfinite(value)) return SourceSdfStatus::InvalidControl;
		for (float value :
			 {request.CameraScale,
			  request.Fov,
			  request.OrthoScale,
			  request.ViewRange[0],
			  request.ViewRange[1],
			  request.DepthRange[0],
			  request.DepthRange[1],
			  request.DepthIntensity,
			  request.AmbientIntensity,
			  request.LightIntensity})
			if (!std::isfinite(value)) return SourceSdfStatus::InvalidControl;
		if (request.Environment &&
			(!source::ValidSurfaceLayout(
				 *request.Environment, source::Limits::MaximumDimension, source::Limits::MaximumArrayBytes
			 ) ||
			 !source::FiniteSurfaceSamples(*request.Environment)))
			return SourceSdfStatus::InvalidControl;
		const auto validImage = [](const std::optional<source::Image> &image) {
			return !image || (source::ValidSurfaceLayout(*image, 8192, 96ull * 1024 * 1024) &&
							  source::FiniteSurfaceSamples(*image));
		};
		if (!validImage(request.Cloud.GradientMap)) return SourceSdfStatus::InvalidControl;
		for (const auto &image : request.Terrain.Textures)
			if (!validImage(image)) return SourceSdfStatus::InvalidControl;
		const auto uniforms = EncodedUniforms(request);
		for (size_t offset = 0; offset < uniforms.size(); offset += sizeof(float)) {
			float value;
			std::memcpy(&value, uniforms.data() + offset, sizeof(value));
			if (!std::isfinite(value)) return SourceSdfStatus::InvalidControl;
		}
		return SourceSdfStatus::Ok;
	} catch (const std::bad_alloc &) {
		return SourceSdfStatus::OutputLimit;
	}
	uint64_t SourceSdfSourceBytes(const SourceSdfRequest &request) {
		uint64_t bytes = sizeof(request);
		if (request.Environment) bytes += request.Environment->Pixels.capacity();
		bytes += request.Cloud.Gradient.Keys.capacity() * sizeof(source::GradientKey);
		if (request.Cloud.GradientMap) bytes += request.Cloud.GradientMap->Pixels.capacity();
		for (const auto &image : request.Terrain.Textures)
			if (image) bytes += image->Pixels.capacity();
		if (request.Object.Data) {
			bytes += sizeof(source::SdfData) +
					 request.Object.Data->Shapes.capacity() * sizeof(source::SourceSdfShape) +
					 request.Object.Data->Operations.capacity() * sizeof(source::SourceSdfOperation);
			for (const auto &shape : request.Object.Data->Shapes) {
				bytes += shape.Identity.capacity();
				if (shape.Texture) bytes += shape.Texture->Pixels.capacity();
			}
		}
		return bytes;
	}
	uint64_t SourceSdfScratchBytes(const SourceSdfRequest &request) {
		const uint64_t output =
			uint64_t(request.Width) * request.Height * 32 + sizeof(SourceSdfScatterUniforms);
		const auto imageBytes = [](const std::optional<source::Image> &image) {
			if (!image) return uint64_t{0};
			const auto format = source::DescribeSurfaceFormat(image->Format);
			return format ? uint64_t(image->Width) * image->Height *
								std::max<uint8_t>(4, format->BytesPerPixel) * 2
						  : UINT64_MAX;
		};
		if (request.Mode == SourceRaymarchMode::Cloud) {
			const auto bytes = imageBytes(request.Cloud.GradientMap);
			return bytes > UINT64_MAX - output ? UINT64_MAX : output + bytes;
		}
		if (request.Mode == SourceRaymarchMode::Terrain) {
			uint64_t bytes = output;
			for (const auto &image : request.Terrain.Textures) {
				const auto size = imageBytes(image);
				if (size > UINT64_MAX - bytes) return UINT64_MAX;
				bytes += size;
			}
			return bytes;
		}
		const uint32_t shapeSize =
			request.ShapeTextureAtlasSize ? request.ShapeTextureAtlasSize : request.TextureAtlasSize;
		return ((request.Environment ? uint64_t(request.TextureAtlasSize) * request.TextureAtlasSize : 0) +
				(HasShapeTextures(request) ? uint64_t(shapeSize) * ShapeAtlasHeight(request, shapeSize)
										   : 0)) *
				   4 * 3 +
			   output;
	}
	bool RecordSourceSdf(
		SDL_GPUDevice *device,
		SDL_GPUCommandBuffer *command,
		const SourceSdfRequest &request,
		SourceCamera3DResources &resources
	) try {
		ENGINE_PROFILE_CAT("render imagegraph sdf", core::ProfileCategory::Render);
		if (!device || !command || ValidateSourceSdfRequest(request) != SourceSdfStatus::Ok) return false;
		const auto support = engine::render::detail::TextureFormatForUpload(request.Format);
		constexpr const char *fragmentNames[] = {
			"imagegraph-sdf.frag",
			"imagegraph-sdf-scatter.frag",
			"imagegraph-sdf-cloud.frag",
			"imagegraph-sdf-terrain.frag"
		};
		const uint32_t samplerCount = request.Mode == SourceRaymarchMode::Cloud ? 1 : 4;
		auto *vertex = Shader(device, "imagegraph-sdf-post.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, resources),
			 *fragment = Shader(
				 device,
				 fragmentNames[uint8_t(request.Mode)],
				 SDL_GPU_SHADERSTAGE_FRAGMENT,
				 samplerCount,
				 resources,
				 request.Mode == SourceRaymarchMode::Render || request.Mode == SourceRaymarchMode::Scatter ? 3
																										   : 1
			 );
		auto *sampler = Sampler(device, request.TextureFiltering, resources);
		if (!vertex || !fragment || !sampler) return false;
		auto *empty = Upload(device, command, nullptr, {0, 0, 0, 0}, resources);
		if (!empty) return false;
		std::array<SDL_GPUTexture *, 4> textures{empty, empty, empty, empty};
		if (request.Mode == SourceRaymarchMode::Cloud) {
			if (request.Cloud.GradientMap)
				textures[0] = Upload(device, command, &*request.Cloud.GradientMap, {0, 0, 0, 0}, resources);
		} else if (request.Mode == SourceRaymarchMode::Terrain) {
			for (size_t i = 0; i < textures.size(); ++i)
				if (request.Terrain.Textures[i])
					textures[i] =
						Upload(device, command, &*request.Terrain.Textures[i], {0, 0, 0, 0}, resources);
		} else {
			if (request.Environment) {
				const auto atlas = Atlas(request, true);
				textures[0] = Upload(device, command, &atlas, {0, 0, 0, 0}, resources);
			}
			if (HasShapeTextures(request)) {
				const auto atlas = Atlas(request, false);
				textures[1] = Upload(device, command, &atlas, {0, 0, 0, 0}, resources);
			}
		}
		if (std::ranges::any_of(textures, [](const auto *texture) { return texture == nullptr; }))
			return false;
		resources.Output = Texture(device, request.Width, request.Height, support->DeviceFormat, resources);
		if (!resources.Output) return false;
		SDL_GPUColorTargetDescription target{};
		target.format = support->DeviceFormat;
		SDL_GPUGraphicsPipelineCreateInfo pipelineInfo{};
		pipelineInfo.vertex_shader = vertex;
		pipelineInfo.fragment_shader = fragment;
		pipelineInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		pipelineInfo.target_info.color_target_descriptions = &target;
		pipelineInfo.target_info.num_color_targets = 1;
		auto *pipeline = SDL_CreateGPUGraphicsPipeline(device, &pipelineInfo);
		if (!pipeline) return false;
		resources.Pipelines.push_back(pipeline);
		SDL_GPUColorTargetInfo color{};
		color.texture = resources.Output;
		color.load_op = SDL_GPU_LOADOP_CLEAR;
		color.store_op = SDL_GPU_STOREOP_STORE;
		auto *pass = SDL_BeginGPURenderPass(command, &color, 1, nullptr);
		if (!pass) return false;
		const auto uniforms = EncodedUniforms(request);
		if (request.Mode == SourceRaymarchMode::Render || request.Mode == SourceRaymarchMode::Scatter) {
			if (!PushSourceFragmentUniforms(command, uniforms, SDF_UNIFORM_CUTS)) {
				SDL_EndGPURenderPass(pass);
				return false;
			}
		} else
			SDL_PushGPUFragmentUniformData(command, 0, uniforms.data(), uniforms.size());
		std::array<SDL_GPUTextureSamplerBinding, 4> bindings{};
		for (size_t i = 0; i < bindings.size(); ++i)
			bindings[i] = {textures[i], sampler};
		SDL_BindGPUGraphicsPipeline(pass, pipeline);
		SDL_BindGPUFragmentSamplers(pass, 0, bindings.data(), samplerCount);
		if (request.Render) SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
		SDL_EndGPURenderPass(pass);
		resources.CommandReferenced = true;
		return true;
	} catch (const std::bad_alloc &) {
		return false;
	}
	SourceSdfStatus
	ExecuteSourceSdf(Renderer &renderer, const SourceSdfRequest &request, SourceSdfResult &result) {
		const auto validation = ValidateSourceSdfRequest(request);
		if (validation != SourceSdfStatus::Ok) return validation;
		auto *device = static_cast<SDL_GPUDevice *>(renderer.Backend().Device);
		if (!device) return SourceSdfStatus::GpuUnavailable;
		SourceCamera3DResources resources;
		auto *command = SDL_AcquireGPUCommandBuffer(device);
		if (!command) return SourceSdfStatus::GpuUnavailable;
		if (!RecordSourceSdf(device, command, request, resources)) {
			SDL_CancelGPUCommandBuffer(command);
			ReleaseSourceCamera3D(device, resources);
			return SourceSdfStatus::GpuUnavailable;
		}
		const auto support = detail::TextureFormatForUpload(request.Format);
		const uint32_t bytes = uint64_t(request.Width) * request.Height * support->UploadBytesPerPixel;
		auto *download = Transfer(device, bytes, SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, resources);
		if (!download) {
			SDL_CancelGPUCommandBuffer(command);
			ReleaseSourceCamera3D(device, resources);
			return SourceSdfStatus::GpuUnavailable;
		}
		auto *copy = SDL_BeginGPUCopyPass(command);
		if (!copy) {
			SDL_CancelGPUCommandBuffer(command);
			ReleaseSourceCamera3D(device, resources);
			return SourceSdfStatus::GpuUnavailable;
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
			return SourceSdfStatus::GpuUnavailable;
		}
		const bool completed = SDL_WaitForGPUFences(device, true, &fence, 1);
		SDL_ReleaseGPUFence(device, fence);
		if (!completed) {
			ReleaseSourceCamera3D(device, resources);
			return SourceSdfStatus::GpuUnavailable;
		}
		void *mapped = SDL_MapGPUTransferBuffer(device, download, false);
		if (!mapped) {
			ReleaseSourceCamera3D(device, resources);
			return SourceSdfStatus::GpuUnavailable;
		}
		SourceSdfResult next;
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
			return SourceSdfStatus::OutputLimit;
		}
		SDL_UnmapGPUTransferBuffer(device, download);
		ReleaseSourceCamera3D(device, resources);
		result = std::move(next);
		return SourceSdfStatus::Ok;
	}
}
