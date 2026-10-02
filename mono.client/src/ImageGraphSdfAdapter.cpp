#include "ImageGraphSdfAdapter.hpp"

#include "ImageGraphSurfaceFormat.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/SourceCamera3D.hpp>
#include <engine/imagegraph/Surface.hpp>
#include <engine/imagegraphphysics/RigidReplay.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <new>
namespace client::detail {
	namespace {
		using namespace engine::imagegraph;
		constexpr uint64_t SDF_HOST_BYTES = 64ull * 1024 * 1024;
		class Controls {
			std::span<const EvaluationInputValue> Values;
			Diagnostic &Error;
			std::string_view Node;

		  public:
			Controls(std::span<const EvaluationInputValue> values, Diagnostic &error, std::string_view node)
				: Values(values), Error(error), Node(node) {}
			const Value *Find(std::string_view port) const {
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

	}
	bool BuildSdfRequest(
		const engine::imagegraph::Document &document,
		const engine::imagegraph::Plan &plan,
		const engine::imagegraph::Node &node,
		std::string_view outputPort,
		uint64_t tick,
		uint64_t seed,
		bool displayColorSpace,
		engine::render::imagegraph::SourceSdfRequest &outputRequest,
		engine::imagegraph::Diagnostic &diagnostic,
		engine::imagegraph::HostNodeProvider *hostProvider,
		engine::imagegraph::CapturedFeedbackHost *replayOwner,
		uint64_t authoringRevision
	) {
		ENGINE_PROFILE("imagegraph source sdf request");
		using namespace engine;
		diagnostic = {};
		imagegraph::EvaluationSnapshot localSnapshot;
		imagegraph::CapturedFeedbackHost localReplay;
		auto &owner = replayOwner ? *replayOwner : localReplay;
		imagegraphphysics::RigidProvider rigid;
		imagegraph::EvaluationRequest clock{.Tick = tick, .Seed = seed, .HostProvider = hostProvider};
		clock.RigidProvider = &rigid;
		// Native client snapshots sample played frames, including fixed seeks.
		clock.RigidPlaying = true;
		clock.RigidFrameProgress = true;
		if (!owner.PrepareNodeInputs(
				document, plan, authoringRevision, seed, node.Id, clock, diagnostic, SDF_HOST_BYTES
			))
			return false;
		if (!owner.Active() && imagegraph::EvaluateNodeInputs(
								   document, plan, node.Id, clock, localSnapshot, diagnostic, SDF_HOST_BYTES
							   ) != imagegraph::Status::Ok)
			return false;
		const auto &snapshot = owner.Active() ? owner.Snapshot() : localSnapshot;
		Controls controls(snapshot.Values(), diagnostic, node.Id);
		if (outputPort != "surface_out")
			return controls.Fail(outputPort, "unknown sdf output", imagegraph::Status::UnknownPort);
		const bool preview = node.Type == "pc.rm_primitive" || node.Type == "pc.rm_combine";
		imagegraph::SdfValue previewObject;
		const auto *value = controls.Find("sdf_object");
		const auto *object = value ? std::get_if<imagegraph::SdfValue>(value) : nullptr;
		if (preview) {
			if (imagegraph::BuildSourceSdfObject(node, snapshot, previewObject, diagnostic, SDF_HOST_BYTES) !=
				imagegraph::Status::Ok)
				return false;
			object = &previewObject;
		}
		const bool cloud = node.Type == "pc.rm_cloud", terrain = node.Type == "pc.rm_terrain",
				   scatter = node.Type == "pc.rm_render_scatter";
		if (!cloud && !terrain && (!object || !object->Data))
			return controls.Fail(
				"sdf_object", "sdf renderer requires a resolved object", imagegraph::Status::TypeMismatch
			);
		if (snapshot.RetainedBytes() > (SDF_HOST_BYTES - sizeof(render::imagegraph::SourceSdfRequest)) / 2)
			return controls.Fail(
				"sdf_object",
				"sdf snapshot and request exceed the host byte limit",
				imagegraph::Status::LimitExceeded
			);
		render::imagegraph::SourceSdfRequest request;
		if (preview) request.ShapeTextureAtlasSize = 8192;
		if (node.Type == "pc.rm_combine") request.DepthRange = {0, 1};
		if (imagegraph::ResolveSourceCameraDimensions(
				document, node, snapshot.Values(), request.Width, request.Height, diagnostic
			) != imagegraph::Status::Ok)
			return false;
		imagegraph::SurfaceFormat surfaceFormat;
		if (imagegraph::ResolveSourceCameraSurfaceFormat(
				document, node, snapshot, surfaceFormat, diagnostic
			) != imagegraph::Status::Ok)
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
		request.TextureFiltering =
			(interpolation ? int64_t(interpolation) : snapshot.InheritedInterpolation()) > 1;
		try {
			if (object) request.Object = *object;
			for (const auto &image : snapshot.Images())
				if (image.Port == "environment") request.Environment = image.Data;
		} catch (const std::bad_alloc &) {
			return controls.Fail(
				"sdf_object", "sdf request allocation failed", imagegraph::Status::LimitExceeded
			);
		}

		try {
			if (scatter) {
				request.Mode = render::imagegraph::SourceRaymarchMode::Scatter;
				auto &c = request.Scatter;
				c.Seed = static_cast<float>(seed);
				if (!controls.Float("seed", c.Seed) || !controls.Vector("grid", c.Grid) ||
					!controls.Vector("position", c.PositionOrigin) ||
					!controls.Vector("random_offset", c.PositionOffset) ||
					!controls.Vector("rotation", c.RotationOrigin) ||
					!controls.Vector("rot_random_min", c.RotationMinimum) ||
					!controls.Vector("rot_random_max", c.RotationMaximum) ||
					!controls.Vector("scale_range", c.ObjectScale))
					return false;
			} else if (cloud) {
				request.Mode = render::imagegraph::SourceRaymarchMode::Cloud;
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
				const auto *gradient = colors ? std::get_if<imagegraph::Gradient>(colors) : nullptr;
				if (!gradient)
					return controls.Fail(
						"colors", "cloud requires a gradient", imagegraph::Status::TypeMismatch
					);
				c.Gradient = *gradient;
				c.GradientBlend = gradient->Mode;
			} else if (terrain) {
				request.Mode = render::imagegraph::SourceRaymarchMode::Terrain;
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
				if (snapshot.RetainedBytes() * 2 + packedBytes + sizeof(request) > SDF_HOST_BYTES)
					return controls.Fail(
						"surface",
						"terrain atlas and snapshot exceed the host byte limit",
						imagegraph::Status::LimitExceeded
					);
				imagegraph::Image atlas{width, height, std::vector<uint8_t>(packedBytes), 0};
				constexpr std::string_view ports[] = {"surface", "texture", "reflection"};
				for (size_t cell = 0; cell < 3; ++cell)
					for (const auto &image : snapshot.Images())
						if (image.Port == ports[cell]) {
							if (cell == 1) t.UseTexture = true;
							for (uint32_t y = 0; y < 1024; ++y)
								for (uint32_t x = 0; x < 1024; ++x) {
									std::array<double, 4> pixel{};
									imagegraph::LoadSurfacePixel(
										image.Data,
										uint32_t((uint64_t(x) * 2 + 1) * image.Data.Width / 2048),
										uint32_t((uint64_t(y) * 2 + 1) * image.Data.Height / 2048),
										pixel
									);
									imagegraph::StoreSurfacePixel(atlas, uint32_t(cell) * 1024 + x, y, pixel);
								}
						}
				t.Textures[0] = std::move(atlas);
				t.AtlasUvScale = {1, 4192.f / 1024};
			}
		} catch (const std::bad_alloc &) {
			return controls.Fail({}, "raymarch request allocation failed", imagegraph::Status::LimitExceeded);
		}
		if (render::imagegraph::ValidateSourceSdfRequest(request) != render::imagegraph::SourceSdfStatus::Ok)
			return controls.Fail({}, "sdf object or render controls are invalid");
		outputRequest = std::move(request);
		return true;
	}
}
