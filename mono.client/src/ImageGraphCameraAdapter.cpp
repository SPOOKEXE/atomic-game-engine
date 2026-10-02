#include "ImageGraphCameraAdapter.hpp"

#include "ImageGraphSurfaceFormat.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/SourceCamera3D.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <numbers>
#include <type_traits>

namespace client::detail {
	namespace {
		using namespace engine::imagegraph;
		constexpr uint64_t CAMERA_HOST_BYTES = 64ull * 1024 * 1024;
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
						port, "camera numeric input requires one scalar processor row", Status::TypeMismatch
					);
				return std::isfinite(result) || Fail(port, "camera numeric input must be finite");
			}
			bool Float(std::string_view port, float &result) {
				double value = result;
				if (!Number(port, value)) return false;
				if (std::abs(value) > std::numeric_limits<float>::max())
					return Fail(port, "camera numeric input exceeds renderer precision range");
				result = static_cast<float>(value);
				return true;
			}
			bool Unsigned(std::string_view port, uint32_t &result, uint32_t maximum = UINT32_MAX) {
				double value = result;
				if (!Number(port, value)) return false;
				if (std::trunc(value) != value || value < 0 || value > maximum)
					return Fail(port, "camera choice requires a bounded integer");
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
					port, "camera boolean input requires one boolean processor row", Status::TypeMismatch
				);
			}
			bool Color(std::string_view port, std::array<float, 4> &result) {
				const auto *value = Find(port);
				if (!value) return true;
				const auto *colour = std::get_if<Colour>(value);
				if (!colour) return Fail(port, "camera colour input requires a colour", Status::TypeMismatch);
				result = {
					colour->Red / 255.0f,
					colour->Green / 255.0f,
					colour->Blue / 255.0f,
					colour->Alpha / 255.0f
				};
				return true;
			}
		};
		bool CameraSetLight(Controls &controls, uint32_t index, LightValue3D &light) {
			const std::string prefix = "l" + std::to_string(index) + "_";
			double horizontal = index == 1 ? 30 : -45, vertical = 45, intensity = index == 1 ? 1 : .25;
			if (!controls.Number(prefix + "h_angle", horizontal) ||
				!controls.Number(prefix + "v_angle", vertical) ||
				!controls.Number(prefix + "intensity", intensity))
				return false;
			if (std::abs(intensity) > std::numeric_limits<float>::max())
				return controls.Fail(
					prefix + "intensity", "light intensity exceeds renderer precision range"
				);
			Colour color{255, 255, 255, 255};
			if (const auto *value = controls.Find(prefix + "color")) {
				const auto *typed = std::get_if<Colour>(value);
				if (!typed)
					return controls.Fail(
						prefix + "color", "light color requires a colour", Status::TypeMismatch
					);
				color = *typed;
			}
			if (std::fmod(horizontal, 90) == 0) horizontal += .1;
			if (std::fmod(vertical, 90) == 0) vertical += .1;
			const double h = horizontal / 180 * std::numbers::pi, v = vertical / 180 * std::numbers::pi;
			auto &data = light.Data.emplace();
			data.Kind = LightKind3D::Directional;
			data.Color = color;
			data.Intensity = intensity;
			data.CastShadow = false;
			data.ShadowBias = .001;
			data.ShadowMapScale = 4;
			data.Transform.Scale = {.6, .6, .6};
			data.Transform.Position = {
				4 * std::cos(v) * std::sin(h), 4 * std::cos(v) * std::cos(h), 4 * std::sin(v)
			};
			// Source __rot3.lookAt uses +Z up, then BBMOD's negative half-angle Euler conversion.
			const auto &p = data.Transform.Position;
			const double length = std::hypot(p.X, p.Y, p.Z);
			const double dx = -p.X / length, dy = -p.Y / length, dz = -p.Z / length;
			const double horizontalLength = std::hypot(dx, dy);
			double roll = std::atan2(0.0 / horizontalLength, -(dx * dx + dy * dy) / horizontalLength);
			if (std::isnan(roll)) roll = 0;
			const double x = -roll / 2, y = std::asin(std::clamp(dz, -1.0, 1.0)) / 2,
						 z = std::atan2(dy, dx) / 2;
			const double qx = std::cos(z) * std::sin(x), qy = std::sin(z) * std::sin(x),
						 qz = std::sin(z) * std::cos(x), qw = std::cos(z) * std::cos(x);
			data.Transform.Rotation = {
				qx * std::cos(y) - qz * std::sin(y),
				qw * std::sin(y) + qy * std::cos(y),
				qz * std::cos(y) + qx * std::sin(y),
				qw * std::cos(y) - qy * std::sin(y)
			};
			return true;
		}
		std::optional<engine::render::imagegraph::SourceCamera3DOutput> CameraOutput(std::string_view port) {
			using Output = engine::render::imagegraph::SourceCamera3DOutput;
			if (port == "rendered") return Output::Rendered;
			if (port == "diffuse") return Output::Diffuse;
			if (port == "normal") return Output::Normal;
			if (port == "view_normal") return Output::ViewNormal;
			if (port == "depth") return Output::Depth;
			if (port == "shadow") return Output::Shadow;
			if (port == "ambient_occlusion") return Output::AmbientOcclusion;
			return std::nullopt;
		}
	}

	bool BuildCameraRequest(
		const engine::imagegraph::Document &document,
		const engine::imagegraph::Plan &plan,
		const engine::imagegraph::Node &node,
		std::string_view outputPort,
		uint64_t tick,
		uint64_t seed,
		bool displayColorSpace,
		engine::render::imagegraph::SourceCamera3DRequest &outputRequest,
		engine::imagegraph::Diagnostic &diagnostic,
		engine::imagegraph::HostNodeProvider *hostProvider,
		engine::imagegraph::CapturedFeedbackHost *replayOwner,
		uint64_t authoringRevision
	) {
		ENGINE_PROFILE("imagegraph source camera request");
		using namespace engine;
		diagnostic = {};
		imagegraph::EvaluationSnapshot localSnapshot;
		imagegraph::CapturedFeedbackHost localReplay;
		auto &owner = replayOwner ? *replayOwner : localReplay;
		imagegraph::EvaluationRequest clock{.Tick = tick, .Seed = seed, .HostProvider = hostProvider};
		if (!owner.PrepareNodeInputs(
				document, plan, authoringRevision, seed, node.Id, clock, diagnostic, CAMERA_HOST_BYTES
			))
			return false;
		if (!owner.Active() &&
			imagegraph::EvaluateNodeInputs(
				document, plan, node.Id, clock, localSnapshot, diagnostic, CAMERA_HOST_BYTES
			) != imagegraph::Status::Ok)
			return false;
		const auto &snapshot = owner.Active() ? owner.Snapshot() : localSnapshot;
		Controls controls(snapshot.Values(), diagnostic, node.Id);
		const auto output = CameraOutput(outputPort);
		if (!output)
			return controls.Fail(outputPort, "unknown camera output", imagegraph::Status::UnknownPort);
		const auto *sceneValue = controls.Find("scene");
		const auto *scene = sceneValue ? std::get_if<imagegraph::SceneValue3D>(sceneValue) : nullptr;
		if (!scene || !scene->Data)
			return controls.Fail(
				"scene", "camera requires a resolved scene", imagegraph::Status::TypeMismatch
			);
		const uint64_t cameraSetBytes = node.Type == "pc.3_d_camera_set"
											? sizeof(imagegraph::SceneData3D) +
												  3 * sizeof(imagegraph::SceneObject3D) +
												  2 * sizeof(imagegraph::LightData3D)
											: 0;
		if (snapshot.RetainedBytes() >
			(CAMERA_HOST_BYTES - sizeof(render::imagegraph::SourceCamera3DRequest) - cameraSetBytes) / 2)
			return controls.Fail(
				"scene",
				"camera snapshot and owned request exceed the host byte limit",
				imagegraph::Status::LimitExceeded
			);
		render::imagegraph::SourceCamera3DRequest request;
		request.Output = *output;
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
		if (!format) return controls.Fail("attribute_color_depth", "unsupported camera surface format");
		request.Format = *format;
		if (!displayColorSpace && request.Format == assets::TextureFormat::RGBA8)
			request.Format = assets::TextureFormat::RGBA8_LINEAR;
		imagegraph::SourceCameraPose pose;
		std::array<double, 16> view, projection;
		if (imagegraph::ResolveSourceCameraPose(
				snapshot.Values(), request.Width, request.Height, pose, diagnostic
			) != imagegraph::Status::Ok ||
			imagegraph::ResolveSourceCameraMatrices(
				pose, request.Width, request.Height, view, projection, diagnostic
			) != imagegraph::Status::Ok ||
			imagegraph::ResolveSourceCameraShader(document, snapshot.Values(), request.Shader, diagnostic) !=
				imagegraph::Status::Ok)
			return false;
		auto narrow = [&](double value, float &result) {
			if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
				return controls.Fail("projection", "camera transform exceeds renderer precision range");
			result = static_cast<float>(value);
			return true;
		};
		for (size_t index = 0; index < 16; index++)
			if (!narrow(view[index], request.View[index]) ||
				!narrow(projection[index], request.Projection[index]))
				return false;
		if (!narrow(pose.Position.X, request.CameraPosition[0]) ||
			!narrow(pose.Position.Y, request.CameraPosition[1]) ||
			!narrow(pose.Position.Z, request.CameraPosition[2]) ||
			!narrow(pose.ClippingDistance.X, request.Near) || !narrow(pose.ClippingDistance.Y, request.Far))
			return false;
		if (!controls.Unsigned("backface_culling", request.CullMode, 2) ||
			!controls.Unsigned("blend_mode", request.BlendMode, 1) ||
			!controls.Unsigned("wire_mode", request.WireMode, 2) ||
			!controls.Unsigned("ao_blur", request.AoBlur) ||
			!controls.Unsigned("round_normal", request.RoundNormal) ||
			!controls.Color("ambient_light", request.AmbientLight) ||
			!controls.Color("wireframe_color", request.WireColor) ||
			!controls.Color("backface_blending", request.BackfaceBlending) ||
			!controls.Boolean("show_background", request.ShowBackground) ||
			!controls.Boolean("gamma_adjust", request.GammaAdjust) ||
			!controls.Boolean("wireframe_antialias", request.WireAntialias) ||
			!controls.Boolean("wireframe_shading", request.WireShading) ||
			!controls.Boolean("wireframe_only", request.WireOnly) ||
			!controls.Boolean("ambient_occlusion", request.AmbientOcclusion) ||
			!controls.Boolean("swap_view_normal_x", request.SwapViewNormalX) ||
			!controls.Float("alpha_threshold", request.AlphaThreshold) ||
			!controls.Float("wireframe_thickness", request.WireThickness) ||
			!controls.Float("ao_strength", request.AoStrength) ||
			!controls.Float("ao_radius", request.AoRadius) || !controls.Float("ao_bias", request.AoBias))
			return false;
		try {
			request.Scene = *scene;
			if (node.Type == "pc.3_d_camera_set") {
				LightValue3D key, fill;
				if (!CameraSetLight(controls, 1, key) || !CameraSetLight(controls, 2, fill)) return false;
				auto authored = std::move(request.Scene.Data);
				auto &combined = request.Scene.Data.emplace();
				combined.Objects = {{std::move(authored)}, {std::move(key)}, {std::move(fill)}};
			}
			for (const auto &image : snapshot.Images())
				if (image.Port == "environment_texture") request.Environment = image.Data;
		} catch (const std::bad_alloc &) {
			return controls.Fail(
				"scene", "camera request allocation failed", imagegraph::Status::LimitExceeded
			);
		}
		if (render::imagegraph::ValidateSourceCamera3D(request) !=
			render::imagegraph::SourceCamera3DStatus::Ok)
			return controls.Fail({}, "camera scene or render controls are invalid");
		outputRequest = std::move(request);
		return true;
	}
}
