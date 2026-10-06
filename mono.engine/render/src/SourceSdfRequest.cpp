#include "SourceSdf.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/SourceCamera3D.hpp>
#include <engine/imagegraph/Surface.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>
namespace engine::render::imagegraph {
	using namespace engine::imagegraph;
	namespace {
		constexpr uint64_t SDF_HOST_BYTES = 64ull * 1024 * 1024;
		class Controls {
			std::span<const AuthoredValue> Values;
			std::span<const EvaluationInputValue> SnapshotValues;
			Diagnostic &Error;
			std::string_view Node;

		  public:
			Controls(std::span<const EvaluationInputValue> values, Diagnostic &error, std::string_view node)
				: SnapshotValues(values), Error(error), Node(node) {}
			Controls(std::span<const AuthoredValue> values, Diagnostic &error, std::string_view node)
				: Values(values), Error(error), Node(node) {}
			const Value *Find(std::string_view port) const {
				for (const auto &value : SnapshotValues)
					if (value.Port == port) return &value.Data;
				for (const auto &value : Values)
					if (value.Port == port) return &value.Data;
				return nullptr;
			}
			bool Fail(std::string_view port, const char *message, Status code = Status::InvalidValue) {
				if (Error.Code == Status::Ok) Error = {code, std::string(Node), std::string(port), message};
				return false;
			}
			bool Number(std::string_view port, double &result) {
				const auto *value = Find(port);
				if (!value) return true;
				if (const auto *number = std::get_if<double>(value))
					result = *number;
				else if (const auto *number = std::get_if<int64_t>(value))
					result = static_cast<double>(*number);
				else if (const auto *number = std::get_if<EnumValue>(value))
					result = number->Value;
				else
					return Fail(
						port, "sdf numeric input requires one scalar processor row", Status::TypeMismatch
					);
				return std::isfinite(result) || Fail(port, "sdf numeric input must be finite");
			}
			bool Float(std::string_view port, float &result) {
				double value = result;
				if (!Number(port, value)) return false;
				if (std::abs(value) > std::numeric_limits<float>::max())
					return Fail(port, "sdf numeric input exceeds renderer precision range");
				result = static_cast<float>(value);
				return true;
			}
			bool Unsigned(std::string_view port, uint32_t &result, uint32_t maximum = UINT32_MAX) {
				double value = result;
				if (!Number(port, value)) return false;
				if (std::trunc(value) != value || value < 0 || value > maximum)
					return Fail(port, "sdf choice requires a bounded integer");
				result = static_cast<uint32_t>(value);
				return true;
			}
			bool Boolean(std::string_view port, bool &result) {
				const auto *value = Find(port);
				if (!value) return true;
				if (const auto *boolean = std::get_if<bool>(value)) {
					result = *boolean;
					return true;
				}
				return Fail(
					port, "sdf boolean input requires one boolean processor row", Status::TypeMismatch
				);
			}
			bool Color(std::string_view port, std::array<float, 4> &result) {
				const auto *value = Find(port);
				if (!value) return true;
				const auto *colour = std::get_if<Colour>(value);
				if (!colour) return Fail(port, "sdf colour input requires a colour", Status::TypeMismatch);
				result = {
					colour->Red / 255.0f,
					colour->Green / 255.0f,
					colour->Blue / 255.0f,
					colour->Alpha / 255.0f
				};
				return true;
			}

			bool Vector(std::string_view port, std::span<float> result) {
				const auto *value = Find(port);
				if (!value) return true;
				std::array<double, 3> components{};
				if (result.size() == 2) {
					const auto *vector = std::get_if<Vector2>(value);
					if (!vector) return Fail(port, "sdf input requires a vector2", Status::TypeMismatch);
					components = {vector->X, vector->Y, 0};
				} else {
					const auto *vector = std::get_if<Vector3>(value);
					if (!vector) return Fail(port, "sdf input requires a vector3", Status::TypeMismatch);
					components = {vector->X, vector->Y, vector->Z};
				}
				for (size_t i = 0; i < result.size(); ++i) {
					if (!std::isfinite(components[i]) ||
						std::abs(components[i]) > std::numeric_limits<float>::max())
						return Fail(port, "sdf vector exceeds renderer precision range");
					result[i] = static_cast<float>(components[i]);
				}
				return true;
			}
		};

		std::optional<assets::TextureFormat> TextureFormatForSurface(SurfaceFormat format) {
			using F = SurfaceFormat;
			using T = assets::TextureFormat;
			switch (format) {
			case F::RGBA8Unorm:
				return T::RGBA8;
			case F::RGBA4Unorm:
				return T::RGBA4_UNORM;
			case F::RGBA16Float:
				return T::RGBA16_FLOAT;
			case F::RGBA32Float:
				return T::RGBA32_FLOAT;
			case F::R8Unorm:
				return T::R8;
			case F::R16Float:
				return T::R16_FLOAT;
			case F::R32Float:
				return T::R32_FLOAT;
			}
			return {};
		}
		uint64_t SdfObjectBytes(const SdfValue &object) {
			if (!object.Data) return 0;
			const auto &data = *object.Data;
			uint64_t bytes = sizeof(data) + data.Shapes.capacity() * sizeof(SourceSdfShape) +
							 data.Operations.capacity() * sizeof(SourceSdfOperation);
			for (const auto &shape : data.Shapes)
				bytes +=
					shape.Identity.capacity() + 1 + (shape.Texture ? shape.Texture->Pixels.capacity() : 0);
			return bytes;
		}
		const Image *SdfImage(const HostResolvedImage &image) {
			return image.Data;
		}
		const Image *SdfImage(const EvaluationInputImage &image) {
			return &image.Data;
		}
	}
	template <class Invocation>
	bool BuildSdfRequestImpl(
		const Invocation &invocation,
		std::string_view outputPort,
		bool displayColorSpace,
		const SdfValue *previewObject,
		SourceSdfRequest &outputRequest,
		Diagnostic &diagnostic
	) {
		ENGINE_PROFILE("imagegraph resolved sdf request");
		diagnostic = {};
		const auto &node = invocation.Authored;
		Controls controls(invocation.Inputs, diagnostic, node.Id);
		if (outputPort != "surface_out")
			return controls.Fail(outputPort, "unknown sdf output", Status::UnknownPort);
		const bool preview = node.Type == "pc.rm_primitive" || node.Type == "pc.rm_combine";
		const auto *value = controls.Find("sdf_object");
		const auto *object = previewObject ? previewObject : (value ? std::get_if<SdfValue>(value) : nullptr);
		const bool cloud = node.Type == "pc.rm_cloud", terrain = node.Type == "pc.rm_terrain",
				   scatter = node.Type == "pc.rm_render_scatter";
		if (!cloud && !terrain && (!object || !object->Data))
			return controls.Fail(
				"sdf_object", "sdf renderer requires a resolved object", Status::TypeMismatch
			);
		const uint64_t maximum = std::min(invocation.MaximumOperationBytes, SDF_HOST_BYTES);
		uint64_t admitted = SourceSdfSourceBytes(outputRequest) + sizeof(SourceSdfRequest);
		const auto add = [&](uint64_t bytes) {
			if (bytes > maximum || admitted > maximum - bytes) return false;
			admitted += bytes;
			return true;
		};
		if (!add(0))
			return controls.Fail(
				{}, "sdf request replacement exceeds operation budget", Status::LimitExceeded
			);
		for (const auto &input : invocation.Inputs) {
			const auto bytes = ValueClonePayloadBytes(input.Data);
			if (!bytes || !add(*bytes))
				return controls.Fail(
					input.Port, "sdf request controls exceed operation budget", Status::LimitExceeded
				);
		}
		if (previewObject && !add(SdfObjectBytes(*previewObject)))
			return controls.Fail("sdf_object", "sdf preview exceeds operation budget", Status::LimitExceeded);
		for (const auto &image : invocation.Images) {
			const auto *data = SdfImage(image);
			if (!data || !ValidSurfaceLayout(*data, invocation.Request.MaximumImageDimension, maximum) ||
				!FiniteSurfaceSamples(*data))
				return controls.Fail(image.Port, "sdf image binding is invalid");
			if (!add(data->Pixels.capacity()))
				return controls.Fail(image.Port, "sdf images exceed operation budget", Status::LimitExceeded);
		}
		if (terrain && !add(uint64_t(4192) * 1024 * 4))
			return controls.Fail("surface", "terrain atlas exceeds operation budget", Status::LimitExceeded);
		SourceSdfRequest request;
		if (preview) request.ShapeTextureAtlasSize = 8192;
		if (node.Type == "pc.rm_combine") request.DepthRange = {0, 1};
		if (ResolveSourceCameraDimensions(
				node, invocation.Policy, invocation.Inputs, request.Width, request.Height, diagnostic
			) != Status::Ok)
			return false;
		SurfaceFormat surfaceFormat;
		if (ResolveSourceCameraSurfaceFormat(
				node, invocation.Policy, invocation.Inputs, invocation.OutputFormat, surfaceFormat, diagnostic
			) != Status::Ok)
			return false;
		const auto format = TextureFormatForSurface(surfaceFormat);
		if (!format) return controls.Fail("attribute_color_depth", "unsupported sdf surface format");
		request.Format = *format;
		if (!displayColorSpace && request.Format == assets::TextureFormat::RGBA8)
			request.Format = assets::TextureFormat::RGBA8_LINEAR;
		if (!cloud && !terrain &&
			(!controls.Vector("camera_rotation", request.CameraRotation) ||
			 !controls.Float("camera_scale", request.CameraScale) ||
			 !controls.Unsigned("projection", request.Projection, 1) ||
			 !controls.Unsigned("attribute_texture_size", request.TextureAtlasSize, 16384) ||
			 !controls.Float("fov", request.Fov) || !controls.Float("ortho_scale", request.OrthoScale) ||
			 !controls.Vector("view_range", request.ViewRange) ||
			 !controls.Vector("depth_range", request.DepthRange) ||
			 !controls.Float("depth", request.DepthIntensity) ||
			 !controls.Boolean("draw_bg", request.DrawBackground) ||
			 !controls.Color("background", request.Background) ||
			 !controls.Boolean("env_interpolation", request.EnvironmentInterpolation) ||
			 !controls.Float("ambient_level", request.AmbientIntensity) ||
			 !controls.Boolean("use_light", request.UseLight) ||
			 !controls.Vector(
				 (node.Type == "pc.rm_primitive" || scatter) ? "position_2" : "position",
				 request.LightPosition
			 ) ||
			 !controls.Float("intensity", request.LightIntensity) ||
			 !controls.Color("color", request.LightColor)))
			return false;
		uint32_t interpolation = 0;
		if (!controls.Unsigned("interpolate", interpolation, 6) ||
			(preview && !controls.Boolean("render", request.Render)))
			return false;
		request.TextureFiltering = (interpolation ? int64_t(interpolation) : invocation.Interpolation) > 1;
		try {
			if (object) request.Object = *object;
			for (const auto &image : invocation.Images)
				if (image.Port == "environment") request.Environment = *SdfImage(image);
		} catch (const std::bad_alloc &) {
			return controls.Fail("sdf_object", "sdf request allocation failed", Status::LimitExceeded);
		}

		try {
			if (scatter) {
				request.Mode = SourceRaymarchMode::Scatter;
				auto &c = request.Scatter;
				c.Seed = static_cast<float>(invocation.Request.Seed);
				if (!controls.Float("seed", c.Seed) || !controls.Vector("grid", c.Grid) ||
					!controls.Vector("position", c.PositionOrigin) ||
					!controls.Vector("random_offset", c.PositionOffset) ||
					!controls.Vector("rotation", c.RotationOrigin) ||
					!controls.Vector("rot_random_min", c.RotationMinimum) ||
					!controls.Vector("rot_random_max", c.RotationMaximum) ||
					!controls.Vector("scale_range", c.ObjectScale))
					return false;
			} else if (cloud) {
				request.Mode = SourceRaymarchMode::Cloud;
				auto &c = request.Cloud;
				if (!controls.Vector("position", c.Position) || !controls.Vector("rotation", c.Rotation) ||
					!controls.Float("scale", c.ObjectScale) || !controls.Float("fov", c.Fov) ||
					!controls.Vector("view_range", c.ViewRange) || !controls.Unsigned("shape", c.Type, 1) ||
					!controls.Float("density", c.Density) || !controls.Float("threshold", c.Threshold) ||
					!controls.Unsigned("detail", c.Iteration, 4096) ||
					!controls.Float("detail_scaling", c.DetailScale) ||
					!controls.Float("detail_attenuation", c.DetailAttenuation) ||
					!controls.Boolean("use_fog", c.Fog))
					return false;
				c.ObjectScale *= 4;
				const auto *colors = controls.Find("colors");
				const auto *gradient = colors ? std::get_if<Gradient>(colors) : nullptr;
				if (!gradient)
					return controls.Fail("colors", "cloud requires a gradient", Status::TypeMismatch);
				c.Gradient = *gradient;
				c.GradientBlend = gradient->Mode;
			} else if (terrain) {
				request.Mode = SourceRaymarchMode::Terrain;
				auto &t = request.Terrain;
				bool tile = true;
				if (!controls.Float("height", t.Thickness) || !controls.Boolean("tile", tile) ||
					!controls.Vector("position", t.Position) || !controls.Vector("rotation", t.Rotation) ||
					!controls.Float("scale", t.ObjectScale) || !controls.Float("fov", t.Fov) ||
					!controls.Vector("view_range", t.ViewRange) ||
					!controls.Color("background", t.Background) ||
					!controls.Float("bg_bleed", t.DepthIntensity) || !controls.Color("ambient", t.Ambient) ||
					!controls.Vector("sun_position", t.SunPosition) || !controls.Float("shadow", t.Shadow))
					return false;
				t.Shape = 1;
				t.Tile = tile;
				request.TextureFiltering = true;
				// Only the first source atlas row is addressed by the three submitted texture cells.
				constexpr uint32_t width = 4192, height = 1024;
				constexpr uint64_t packedBytes = uint64_t(width) * height * 4;
				Image atlas{width, height, std::vector<uint8_t>(packedBytes), 0};
				constexpr std::string_view ports[] = {"surface", "texture", "reflection"};
				for (size_t cell = 0; cell < 3; ++cell)
					for (const auto &image : invocation.Images)
						if (image.Port == ports[cell]) {
							if (cell == 1) t.UseTexture = true;
							for (uint32_t y = 0; y < 1024; ++y)
								for (uint32_t x = 0; x < 1024; ++x) {
									std::array<double, 4> pixel{};
									LoadSurfacePixel(
										*SdfImage(image),
										uint32_t((uint64_t(x) * 2 + 1) * SdfImage(image)->Width / 2048),
										uint32_t((uint64_t(y) * 2 + 1) * SdfImage(image)->Height / 2048),
										pixel
									);
									StoreSurfacePixel(atlas, uint32_t(cell) * 1024 + x, y, pixel);
								}
						}
				t.Textures[0] = std::move(atlas);
				t.AtlasUvScale = {1, 4192.f / 1024};
			}
		} catch (const std::bad_alloc &) {
			return controls.Fail({}, "raymarch request allocation failed", Status::LimitExceeded);
		}
		if (ValidateSourceSdfRequest(request) != SourceSdfStatus::Ok)
			return controls.Fail({}, "sdf object or render controls are invalid");
		if (request.Width > invocation.Request.MaximumImageDimension ||
			request.Height > invocation.Request.MaximumImageDimension)
			return controls.Fail("dimension", "sdf dimensions exceed caller limit", Status::LimitExceeded);
		if (SourceSdfSourceBytes(request) > maximum - std::min(maximum, SourceSdfSourceBytes(outputRequest)))
			return controls.Fail({}, "sdf request capacities exceed operation budget", Status::LimitExceeded);
		outputRequest = std::move(request);
		return true;
	}
	bool IsSourceSdfRenderNode(std::string_view type) {
		return type == "pc.rm_render" || type == "pc.rm_render_scatter" || type == "pc.rm_cloud" ||
			   type == "pc.rm_terrain";
	}
	bool BuildSourceSdfRequest(
		const HostNodeInvocation &invocation, bool display, SourceSdfRequest &output, std::string &failure
	) {
		Diagnostic diagnostic;
		if (!IsSourceSdfRenderNode(invocation.Authored.Type) || !invocation.CameraPolicy ||
			!ValidSourceCameraEvaluationPolicy(*invocation.CameraPolicy) ||
			invocation.CameraPolicy->InheritedSurfaceFormat != invocation.OutputFormat ||
			invocation.Inputs.size() > Limits::MaximumLinks ||
			invocation.Images.size() > Limits::MaximumLinks) {
			failure = "sdf host requires an explicit RM node and valid project policy";
			return false;
		}
		struct ResolvedInvocation : HostNodeInvocation {
			const SourceCameraEvaluationPolicy &Policy;
		};
		const bool built = BuildSdfRequestImpl(
			ResolvedInvocation{invocation, *invocation.CameraPolicy},
			"surface_out",
			display,
			nullptr,
			output,
			diagnostic
		);
		failure = diagnostic.Message;
		return built;
	}
	bool BuildSourceSdfRequest(
		const Node &node,
		const EvaluationSnapshot &snapshot,
		const SourceCameraEvaluationPolicy &policy,
		std::string_view port,
		uint64_t seed,
		bool display,
		SourceSdfRequest &output,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) {
		maximumBytes = std::min(maximumBytes, SDF_HOST_BYTES);
		const uint64_t previous = SourceSdfSourceBytes(output);
		if (snapshot.RetainedBytes() >= maximumBytes || previous >= maximumBytes - snapshot.RetainedBytes()) {
			diagnostic = {
				Status::LimitExceeded,
				node.Id,
				{},
				"sdf snapshot and previous request exceed operation budget"
			};
			return false;
		}
		const uint64_t available = maximumBytes - snapshot.RetainedBytes();
		SdfValue preview;
		const bool isPreview = node.Type == "pc.rm_primitive" || node.Type == "pc.rm_combine";
		if (isPreview &&
			BuildSourceSdfObject(node, snapshot, preview, diagnostic, maximumBytes - previous) != Status::Ok)
			return false;
		const uint64_t previewBytes = SdfObjectBytes(preview);
		if (previewBytes > available - previous) {
			diagnostic = {
				Status::LimitExceeded, node.Id, {}, "sdf retained preview exceeds operation budget"
			};
			return false;
		}
		EvaluationRequest clock{.Seed = seed};
		struct SnapshotInvocation {
			const Node &Authored;
			const EvaluationRequest &Request;
			std::span<const EvaluationInputValue> Inputs;
			std::span<const EvaluationInputImage> Images;
			uint64_t MaximumOperationBytes;
			const SourceCameraEvaluationPolicy &Policy;
			std::optional<SurfaceFormat> OutputFormat;
			int64_t Interpolation;
		};
		return BuildSdfRequestImpl(
			SnapshotInvocation{
				node,
				clock,
				snapshot.Values(),
				snapshot.Images(),
				available - previewBytes,
				policy,
				snapshot.InheritedSurfaceFormat(),
				snapshot.InheritedInterpolation()
			},
			port,
			display,
			isPreview ? &preview : nullptr,
			output,
			diagnostic
		);
	}
}
