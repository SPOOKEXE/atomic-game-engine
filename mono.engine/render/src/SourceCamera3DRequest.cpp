#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/SourceCamera3D.hpp>
#include <engine/render/SourceCamera3D.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <numbers>
namespace engine::render::imagegraph {
	using namespace engine::imagegraph;
	namespace {
		class Controls {
			std::span<const AuthoredValue> Values;
			std::span<const EvaluationInputValue> SnapshotValues;
			Diagnostic &Error;
			std::string_view Node;

		  public:
			Controls(std::span<const AuthoredValue> values, Diagnostic &error, std::string_view node)
				: Values(values), Error(error), Node(node) {}
			Controls(std::span<const EvaluationInputValue> values, Diagnostic &error, std::string_view node)
				: SnapshotValues(values), Error(error), Node(node) {}
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
		std::optional<SourceCamera3DOutput> CameraOutput(std::string_view port) {
			using Output = SourceCamera3DOutput;
			if (port == "rendered") return Output::Rendered;
			if (port == "diffuse") return Output::Diffuse;
			if (port == "normal") return Output::Normal;
			if (port == "view_normal") return Output::ViewNormal;
			if (port == "depth") return Output::Depth;
			if (port == "shadow") return Output::Shadow;
			if (port == "ambient_occlusion") return Output::AmbientOcclusion;
			return std::nullopt;
		}
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
	}
	const Image *CameraImagePointer(const HostResolvedImage &image) {
		return image.Data;
	}
	const Image *CameraImagePointer(const EvaluationInputImage &image) {
		return &image.Data;
	}
	const Image &CameraImage(const HostResolvedImage &image) {
		return *image.Data;
	}
	const Image &CameraImage(const EvaluationInputImage &image) {
		return image.Data;
	}
	std::optional<uint64_t> SourceCamera3DRetainedBytes(const SourceCamera3DRequest &request) {
		const auto scene = SourceCameraSceneRetainedBytes(request.Scene);
		if (!scene) return {};
		uint64_t bytes = sizeof(request) + *scene;
		if (request.Environment) {
			if (request.Environment->Pixels.capacity() > UINT64_MAX - bytes) return {};
			bytes += request.Environment->Pixels.capacity();
		}
		return bytes;
	}
	template <class Invocation>
	bool BuildCameraRequestImpl(
		const Invocation &invocation,
		std::string_view outputPort,
		bool displayColorSpace,
		SourceCamera3DRequest &outputRequest,
		std::string &failure,
		Diagnostic &diagnostic
	) try {
		ENGINE_PROFILE("source camera resolved request");
		diagnostic = {};
		const auto &node = invocation.Authored;
		if (invocation.Inputs.size() > Limits::MaximumLinks ||
			invocation.Images.size() > Limits::MaximumLinks) {
			failure = "camera resolved bindings exceed the bounded input limit";
			diagnostic = {Status::LimitExceeded, node.Id, {}, failure};
			return false;
		}
		if ((node.Type != "pc.3_d_camera" && node.Type != "pc.3_d_camera_set") || !invocation.CameraPolicy ||
			!ValidSourceCameraEvaluationPolicy(*invocation.CameraPolicy) || !invocation.CameraRow ||
			*invocation.CameraRow >= Limits::MaximumArrayElements ||
			invocation.CameraPolicy->InheritedSurfaceFormat != invocation.OutputFormat) {
			failure = "camera requires valid explicit source project policy and bounded inputs";
			diagnostic = {Status::InvalidValue, node.Id, {}, failure};
			return false;
		}
		const bool built = [&]() -> bool {
			Controls controls(invocation.Inputs, diagnostic, node.Id);
			const auto output = CameraOutput(outputPort);
			if (!output) return controls.Fail(outputPort, "unknown camera output", Status::UnknownPort);
			const auto *sceneValue = controls.Find("scene");
			const auto *scene = sceneValue ? std::get_if<SceneValue3D>(sceneValue) : nullptr;
			if (!scene || !scene->Data)
				return controls.Fail("scene", "camera requires a resolved scene", Status::TypeMismatch);
			const uint64_t maximum = std::min(invocation.MaximumOperationBytes, MAXIMUM_CAMERA_HOST_BYTES);
			const auto previous = SourceCamera3DRetainedBytes(outputRequest);
			const auto sceneBytes = SourceCameraSceneRetainedBytes(*scene);
			const uint64_t cameraSetBytes =
				node.Type == "pc.3_d_camera_set"
					? sizeof(SceneData3D) + 3 * sizeof(SceneObject3D) + 2 * sizeof(LightData3D)
					: 0;
			uint64_t candidateBytes = sizeof(SourceCamera3DRequest) + cameraSetBytes;
			const auto add = [&](uint64_t bytes) {
				if (bytes > maximum || candidateBytes > maximum - bytes) return false;
				candidateBytes += bytes;
				return true;
			};
			if (!previous || !sceneBytes || !add(*previous) || !add(*sceneBytes))
				return controls.Fail(
					"scene", "camera request replacement exceeds operation budget", Status::LimitExceeded
				);
			for (const auto &image : invocation.Images)
				if (image.Port == "environment_texture" && CameraImagePointer(image) &&
					!add(CameraImage(image).Pixels.capacity()))
					return controls.Fail(
						"environment_texture",
						"camera environment exceeds operation budget",
						Status::LimitExceeded
					);
			const uint64_t views =
				invocation.Inputs.size() * sizeof(std::pair<std::string_view, const Value *>) +
				sizeof(std::string_view);
			if (!add(views))
				return controls.Fail(
					{}, "camera control scratch exceeds operation budget", Status::LimitExceeded
				);
			SourceCamera3DRequest request;
			request.Output = *output;
			if (ResolveSourceCameraDimensions(
					node,
					*invocation.CameraPolicy,
					invocation.Inputs,
					request.Width,
					request.Height,
					diagnostic
				) != Status::Ok)
				return false;
			SurfaceFormat surfaceFormat;
			if (ResolveSourceCameraSurfaceFormat(
					node,
					*invocation.CameraPolicy,
					invocation.Inputs,
					invocation.OutputFormat,
					surfaceFormat,
					diagnostic
				) != Status::Ok)
				return false;
			const auto format = TextureFormatForSurface(surfaceFormat);
			if (!format) return controls.Fail("attribute_color_depth", "unsupported camera surface format");
			request.Format = *format;
			if (!displayColorSpace && request.Format == assets::TextureFormat::RGBA8)
				request.Format = assets::TextureFormat::RGBA8_LINEAR;
			SourceCameraPose pose;
			std::array<double, 16> view, projection;
			if (ResolveSourceCameraPose(invocation.Inputs, request.Width, request.Height, pose, diagnostic) !=
					Status::Ok ||
				ResolveSourceCameraMatrices(
					pose, request.Width, request.Height, view, projection, diagnostic
				) != Status::Ok ||
				ResolveSourceCameraShader(
					*invocation.CameraPolicy, invocation.Inputs, request.Shader, diagnostic
				) != Status::Ok)
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
				!narrow(pose.ClippingDistance.X, request.Near) ||
				!narrow(pose.ClippingDistance.Y, request.Far))
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
					combined.Objects.reserve(3);
					const uint64_t actualObjects = combined.Objects.capacity() * sizeof(SceneObject3D);
					if (actualObjects > 3 * sizeof(SceneObject3D) &&
						!add(actualObjects - 3 * sizeof(SceneObject3D)))
						return controls.Fail(
							"scene",
							"camera set object backing exceeds operation budget",
							Status::LimitExceeded
						);
					combined.Objects.emplace_back(SceneObject3D{std::move(authored)});
					combined.Objects.emplace_back(SceneObject3D{std::move(key)});
					combined.Objects.emplace_back(SceneObject3D{std::move(fill)});
				}
				for (const auto &image : invocation.Images)
					if (image.Port == "environment_texture" && CameraImagePointer(image))
						request.Environment = CameraImage(image);
			} catch (const std::bad_alloc &) {
				return controls.Fail("scene", "camera request allocation failed", Status::LimitExceeded);
			}
			if (request.Width > invocation.Request.MaximumImageDimension ||
				request.Height > invocation.Request.MaximumImageDimension)
				return controls.Fail(
					"dimension", "camera dimensions exceed caller limit", Status::LimitExceeded
				);
			const auto retained = SourceCamera3DRetainedBytes(request);
			if (!retained || *retained > maximum - *previous || views > maximum - *previous - *retained)
				return controls.Fail(
					{}, "camera actual retained request exceeds operation budget", Status::LimitExceeded
				);
			const auto validation = ValidateSourceCamera3D(request, maximum - *previous - *retained - views);
			if (validation != SourceCamera3DStatus::Ok)
				return controls.Fail(
					{},
					"camera scene or preparation exceeds its admitted controls and budget",
					validation == SourceCamera3DStatus::OutputLimit ? Status::LimitExceeded
																	: Status::InvalidValue
				);
			core::Metrics::Count("render.camera.request_copied_payload_bytes", *retained - sizeof(request));
			core::Metrics::Count("render.camera.request_copies", 1);
			outputRequest = std::move(request);
			return true;
		}();
		if (!built && diagnostic.NodeId.empty()) diagnostic.NodeId = node.Id;
		failure = built ? std::string{} : diagnostic.Message;
		return built;
	} catch (const std::bad_alloc &) {
		failure = "camera request allocation failed";
		diagnostic = {Status::LimitExceeded, invocation.Authored.Id, {}, failure};
		return false;
	}

	bool BuildSourceCamera3DRequest(
		const HostNodeInvocation &invocation,
		std::string_view port,
		bool display,
		SourceCamera3DRequest &output,
		std::string &failure
	) {
		Diagnostic diagnostic;
		return BuildCameraRequestImpl(invocation, port, display, output, failure, diagnostic);
	}
	bool BuildSourceCamera3DRequest(
		const Node &node,
		const EvaluationSnapshot &snapshot,
		const SourceCameraEvaluationPolicy &policy,
		std::string_view port,
		bool display,
		SourceCamera3DRequest &output,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) {
		std::string failure;
		EvaluationRequest request;
		struct SnapshotInvocation {
			const Node &Authored;
			const EvaluationRequest &Request;
			std::span<const EvaluationInputValue> Inputs;
			std::span<const EvaluationInputImage> Images;
			uint64_t MaximumOperationBytes;
			std::optional<SurfaceFormat> OutputFormat;
			std::optional<SourceCameraEvaluationPolicy> CameraPolicy;
			std::optional<uint32_t> CameraRow = 0;
		};
		if (snapshot.RetainedBytes() >= maximumBytes) {
			diagnostic = {Status::LimitExceeded, node.Id, {}, "camera snapshot exceeds operation budget"};
			return false;
		}
		return BuildCameraRequestImpl(
			SnapshotInvocation{
				node,
				request,
				snapshot.Values(),
				snapshot.Images(),
				maximumBytes - snapshot.RetainedBytes(),
				snapshot.InheritedSurfaceFormat(),
				policy
			},
			port,
			display,
			output,
			failure,
			diagnostic
		);
	}
}
