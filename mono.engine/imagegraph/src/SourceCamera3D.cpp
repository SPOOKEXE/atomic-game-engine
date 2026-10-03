#include "GroupInputDepth.hpp"
#include "ScenePayload.hpp"
#include "SourceChoice.hpp"
#include "SourceMeshTransform.hpp"
#include "nodes/Processor.hpp"

#include <engine/imagegraph/SourceCamera3D.hpp>

#include <algorithm>
#include <numbers>

namespace engine::imagegraph {
	std::optional<uint64_t> SourceCameraSceneRetainedBytes(const SceneValue3D &scene) {
		if (!detail::ValidScenePayload(scene)) return {};
		const auto bytes = detail::SceneStorageBytes<true>(scene);
		return bytes == UINT64_MAX ? std::nullopt : std::optional<uint64_t>{bytes};
	}
	namespace {
		class CameraReader {
			std::span<const EvaluationInputValue> Values;
			std::span<const AuthoredValue> HostValues;
			Diagnostic &Error;

		  public:
			CameraReader(std::span<const EvaluationInputValue> values, Diagnostic &error)
				: Values(values), Error(error) {}
			CameraReader(std::span<const AuthoredValue> values, Diagnostic &error)
				: HostValues(values), Error(error) {}
			const Value *Find(std::string_view id) const {
				for (const auto &entry : HostValues)
					if (entry.Port == id) return &entry.Data;
				for (const auto &entry : Values)
					if (entry.Port == id) return &entry.Data;
				return nullptr;
			}
			bool Fail(std::string_view id, std::string message, Status status = Status::InvalidValue) {
				if (Error.Code == Status::Ok) Error = {status, {}, std::string(id), std::move(message)};
				return false;
			}
			bool Number(std::string_view id, double &target) {
				const Value *value = Find(id);
				if (!value) return true;
				const auto number = detail::SourceChoiceNumber(*value);
				if (!number || !std::isfinite(*number))
					return Fail(id, "camera control requires a finite number", Status::TypeMismatch);
				target = *number;
				return true;
			}
			template <class T> bool Typed(std::string_view id, T &target) {
				const Value *value = Find(id);
				if (!value) return true;
				const auto *typed = std::get_if<T>(value);
				if (!typed) return Fail(id, "camera control has an incompatible type", Status::TypeMismatch);
				target = *typed;
				return true;
			}
		};
		Vector3 Subtract(Vector3 a, Vector3 b) {
			return {a.X - b.X, a.Y - b.Y, a.Z - b.Z};
		}
		Vector3 Cross(Vector3 a, Vector3 b) {
			return {a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X};
		}
	}

	namespace {
		Status CameraPose(
			CameraReader reader,
			uint32_t width,
			uint32_t height,
			SourceCameraPose &result,
			Diagnostic &diagnostic
		) {
			diagnostic = {};
			if (width == 0 || height == 0 || width > Limits::MaximumDimension ||
				height > Limits::MaximumDimension) {
				reader.Fail(
					"dimension", "camera dimensions exceed bounded output limits", Status::LimitExceeded
				);
				return diagnostic.Code;
			}
			SourceCameraPose pose;
			double mode = 2, projection = 1, horizontal = 45, vertical = 30, distance = 4,
				   orthographicScale = .5;
			Quaternion rotation;
			if (!reader.Number("postioning_mode", mode) || !reader.Number("projection", projection) ||
				!reader.Number("horizontal_angle", horizontal) ||
				!reader.Number("vertical_angle", vertical) || !reader.Number("distance", distance) ||
				!reader.Number("orthographic_scale", orthographicScale) ||
				!reader.Number("fov", pose.FieldOfViewDegrees) ||
				!reader.Typed("clipping_distance", pose.ClippingDistance) ||
				!reader.Typed("position", pose.Position) || !reader.Typed("rotation", rotation) ||
				!reader.Typed("lookat_position", pose.Target))
				return diagnostic.Code;
			if (mode == 0) {
				const Vector3 forward = detail::SourceRotate(rotation, {1, 0, 0});
				pose.Up = detail::SourceRotate(rotation, {0, 0, -1});
				pose.Target = {
					pose.Position.X + forward.X, pose.Position.Y + forward.Y, pose.Position.Z + forward.Z
				};
			} else if (mode == 2) {
				if (std::fmod(vertical, 90) == 0) vertical += .01;
				double polarHorizontal = horizontal, polarVertical = vertical;
				if (std::fmod(polarHorizontal, 90) == 0) polarHorizontal += .1;
				if (std::fmod(polarVertical, 90) == 0) polarVertical += .1;
				const double h = horizontal * std::numbers::pi / 180, v = vertical * std::numbers::pi / 180,
							 ph = polarHorizontal * std::numbers::pi / 180,
							 pv = polarVertical * std::numbers::pi / 180;
				pose.Position = {
					pose.Target.X + std::cos(pv) * std::sin(ph) * distance,
					pose.Target.Y + std::cos(pv) * std::cos(ph) * distance,
					pose.Target.Z + std::sin(pv) * distance
				};
				pose.Up = {std::sin(h) * std::sin(v), std::cos(h) * std::sin(v), -std::cos(v)};
				const double length = std::hypot(pose.Up.X, pose.Up.Y, pose.Up.Z);
				if (length) pose.Up = {pose.Up.X / length, pose.Up.Y / length, pose.Up.Z / length};
			} else if (mode != 1) {
				reader.Fail(
					"postioning_mode",
					"camera positioning mode has no defined source case",
					Status::UnsupportedExecution
				);
				return diagnostic.Code;
			}
			if (!detail::MeshFinite(pose.Position) || !detail::MeshFinite(pose.Target) ||
				!detail::MeshFinite(pose.Up)) {
				reader.Fail("position", "camera pose produced nonfinite coordinates");
				return diagnostic.Code;
			}
			const Vector3 forward = Subtract(pose.Target, pose.Position), right = Cross(pose.Up, forward);
			if (std::hypot(forward.X, forward.Y, forward.Z) == 0 ||
				std::hypot(right.X, right.Y, right.Z) == 0) {
				reader.Fail("lookat_position", "camera lookat basis is degenerate");
				return diagnostic.Code;
			}
			if (projection == 0)
				pose.Projection = SourceCameraProjection::Perspective;
			else if (projection == 1) {
				pose.Projection = SourceCameraProjection::Orthographic;
				if (orthographicScale == 0) {
					reader.Fail("orthographic_scale", "orthographic view scale is zero");
					return diagnostic.Code;
				}
				pose.OrthographicViewSize = {
					1 / orthographicScale, double(height) / width / orthographicScale
				};
			} else if (projection == 2) {
				pose.Projection = SourceCameraProjection::Custom;
				if (const Value *value = reader.Find("projection_matrix")) {
					const auto *matrix = std::get_if<MatrixValue>(value);
					if (!matrix || matrix->Columns != 4 || matrix->Rows != 4 || matrix->Values.size() != 16) {
						reader.Fail(
							"projection_matrix",
							"custom camera projection requires a 4 by 4 matrix",
							Status::TypeMismatch
						);
						return diagnostic.Code;
					}
					std::copy(matrix->Values.begin(), matrix->Values.end(), pose.CustomProjection.begin());
					for (double number : pose.CustomProjection)
						if (!std::isfinite(number)) {
							reader.Fail("projection_matrix", "custom camera projection must be finite");
							return diagnostic.Code;
						}
				}
			} else {
				reader.Fail(
					"projection", "camera projection has no defined source case", Status::UnsupportedExecution
				);
				return diagnostic.Code;
			}
			if (pose.Projection != SourceCameraProjection::Custom &&
				(!detail::MeshFinite(pose.ClippingDistance) ||
				 pose.ClippingDistance.X == pose.ClippingDistance.Y)) {
				reader.Fail("clipping_distance", "camera clipping distances must be finite and distinct");
				return diagnostic.Code;
			}
			if (pose.Projection == SourceCameraProjection::Perspective &&
				(pose.FieldOfViewDegrees <= 0 || pose.FieldOfViewDegrees >= 180)) {
				reader.Fail("fov", "perspective field of view must lie between zero and 180 degrees");
				return diagnostic.Code;
			}
			result = pose;
			return Status::Ok;
		}
	}

	Status ResolveSourceCameraPose(
		std::span<const EvaluationInputValue> values,
		uint32_t width,
		uint32_t height,
		SourceCameraPose &result,
		Diagnostic &diagnostic
	) {
		return CameraPose(CameraReader(values, diagnostic), width, height, result, diagnostic);
	}

	Status ResolveSourceCameraPose(
		std::span<const AuthoredValue> values,
		uint32_t width,
		uint32_t height,
		SourceCameraPose &result,
		Diagnostic &diagnostic
	) {
		return CameraPose(CameraReader(values, diagnostic), width, height, result, diagnostic);
	}

	Status ResolveSourceCameraMatrices(
		const SourceCameraPose &pose,
		uint32_t width,
		uint32_t height,
		std::array<double, 16> &view,
		std::array<double, 16> &projection,
		Diagnostic &diagnostic
	) {
		diagnostic = {};
		auto fail = [&](std::string port) {
			diagnostic = {
				Status::InvalidValue, {}, std::move(port), "camera matrix basis or projection is invalid"
			};
			return diagnostic.Code;
		};
		if (!width || !height) return fail("dimension");
		auto normalize = [](Vector3 value) {
			const double length = std::hypot(value.X, value.Y, value.Z);
			return Vector3{value.X / length, value.Y / length, value.Z / length};
		};
		auto dot = [](Vector3 a, Vector3 b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; };
		const Vector3 look = normalize(Subtract(pose.Target, pose.Position));
		const Vector3 right = normalize(Cross(normalize(pose.Up), look));
		const Vector3 up = normalize(Cross(look, right));
		if (!detail::MeshFinite(right) || !detail::MeshFinite(up) || !detail::MeshFinite(look))
			return fail("lookat_position");
		std::array<double, 16> v{
			right.X,
			up.X,
			look.X,
			0,
			right.Y,
			up.Y,
			look.Y,
			0,
			right.Z,
			up.Z,
			look.Z,
			0,
			-dot(pose.Position, right),
			-dot(pose.Position, up),
			-dot(pose.Position, look),
			1
		};
		std::array<double, 16> p{};
		const double near = pose.ClippingDistance.X, far = pose.ClippingDistance.Y;
		if (pose.Projection == SourceCameraProjection::Custom)
			p = pose.CustomProjection;
		else {
			if (!std::isfinite(near) || !std::isfinite(far) || near == far) return fail("clipping_distance");
			if (pose.Projection == SourceCameraProjection::Perspective) {
				if (!std::isfinite(pose.FieldOfViewDegrees) || pose.FieldOfViewDegrees <= 0 ||
					pose.FieldOfViewDegrees >= 180)
					return fail("fov");
				p[5] = 1 / std::tan(pose.FieldOfViewDegrees * std::numbers::pi / 360);
				p[0] = p[5] * height / width;
				p[10] = far / (far - near);
				p[11] = 1;
				p[14] = -near * far / (far - near);
			} else if (pose.Projection == SourceCameraProjection::Orthographic) {
				p[0] = 2 / pose.OrthographicViewSize.X;
				p[5] = 2 / pose.OrthographicViewSize.Y;
				p[10] = 1 / (far - near);
				p[14] = -near / (far - near);
				p[15] = 1;
			} else
				return fail("projection");
		}
		for (double number : v)
			if (!std::isfinite(number)) return fail("lookat_position");
		for (double number : p)
			if (!std::isfinite(number)) return fail("projection");
		view = v;
		projection = p;
		return Status::Ok;
	}
	namespace {
		void FillCameraContext(
			detail::NodeContext &context,
			const Document &document,
			std::span<const EvaluationInputValue> values
		) {
			if (document.Project) {
				const auto &project = *document.Project;
				context.Project.SurfaceWidth = project.SurfaceWidth;
				context.Project.SurfaceHeight = project.SurfaceHeight;
				context.Project.ColorDepth = project.ColorDepth;
			}
			context.ValueViews.reserve(values.size());
			context.LinkedValues.reserve(values.size());
			for (const auto &value : values) {
				context.ValueViews.emplace_back(value.Port, &value.Data);
				if (value.Linked) context.LinkedValues.push_back(value.Port);
			}
		}
		Status CameraContextError(const detail::NodeContext &context, Diagnostic &diagnostic) {
			diagnostic = {
				context.FailureCode, context.Authored.Id, context.FailurePort, context.FailureMessage
			};
			return diagnostic.Code;
		}
	}
	Status ResolveSourceCameraDimensions(
		const Document &document,
		const Node &node,
		std::span<const EvaluationInputValue> values,
		uint32_t &width,
		uint32_t &height,
		Diagnostic &diagnostic
	) {
		diagnostic = {};
		const auto *entry = FindCatalogueEntry(node.Type);
		if (!entry) {
			diagnostic = {
				Status::UnsupportedExecution, node.Id, "dimension", "camera catalogue entry is unavailable"
			};
			return diagnostic.Code;
		}
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		FillCameraContext(context, document, values);
		uint32_t w = 0, h = 0;
		if (!detail::ResolveDimension(context, "dimension", w, h))
			return CameraContextError(context, diagnostic);
		width = w;
		height = h;
		return Status::Ok;
	}
	Status ResolveSourceCameraSurfaceFormat(
		const Document &document,
		const Node &node,
		std::span<const EvaluationInputValue> values,
		SurfaceFormat &format,
		Diagnostic &diagnostic
	) {
		diagnostic = {};
		const auto *entry = FindCatalogueEntry(node.Type);
		if (!entry) {
			diagnostic = {
				Status::UnsupportedExecution,
				node.Id,
				"attribute_color_depth",
				"camera catalogue entry is unavailable"
			};
			return diagnostic.Code;
		}
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		FillCameraContext(context, document, values);
		const auto route = detail::FindGroupInputDepth(document, node.GroupId);
		if (route.Source == detail::GroupInputDepth::Kind::Concrete)
			context.InheritedSurfaceFormat = SourceSurfaceFormat(route.Choice);
		else if (route.Source == detail::GroupInputDepth::Kind::AuthoredValue)
			context.InheritedSurfaceFormat = SurfaceFormat::RGBA8Unorm;
		else
			context.InheritedSurfaceFormat.reset();
		const auto resolved = detail::ResolveProcessorSurfaceFormat(context, nullptr);
		if (!resolved) return CameraContextError(context, diagnostic);
		format = *resolved;
		return Status::Ok;
	}
	Status ResolveSourceCameraSurfaceFormat(
		const Document &document,
		const Node &node,
		const EvaluationSnapshot &snapshot,
		SurfaceFormat &format,
		Diagnostic &diagnostic
	) {
		diagnostic = {};
		const auto *entry = FindCatalogueEntry(node.Type);
		if (!entry) {
			diagnostic = {
				Status::UnsupportedExecution,
				node.Id,
				"attribute_color_depth",
				"camera catalogue entry is unavailable"
			};
			return diagnostic.Code;
		}
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		FillCameraContext(context, document, snapshot.Values());
		context.InheritedSurfaceFormat = snapshot.InheritedSurfaceFormat();
		const auto resolved = detail::ResolveProcessorSurfaceFormat(context, nullptr);
		if (!resolved) return CameraContextError(context, diagnostic);
		format = *resolved;
		return Status::Ok;
	}

	Status ResolveSourceCameraShader(
		const Document &document,
		std::span<const EvaluationInputValue> values,
		uint32_t &shader,
		Diagnostic &diagnostic
	) {
		diagnostic = {};
		CameraReader reader(values, diagnostic);
		double choice = 0;
		if (!reader.Number("shader", choice)) return diagnostic.Code;
		if (choice != 0 && choice != 1 && choice != 2) {
			reader.Fail("shader", "source camera shader choice is invalid");
			return diagnostic.Code;
		}
		const int64_t resolved =
			choice == 0 ? (document.Project ? document.Project->Shader3D : 0) : int64_t(choice) - 1;
		if (resolved < 0 || resolved > 1) {
			reader.Fail("shader", "source project shader choice is invalid");
			return diagnostic.Code;
		}
		shader = uint32_t(resolved);
		return Status::Ok;
	}

	template <class Input>
	Status HostCameraDimensions(
		const Node &node,
		const SourceCameraEvaluationPolicy &policy,
		std::span<const Input> values,
		uint32_t &width,
		uint32_t &height,
		Diagnostic &diagnostic
	) {
		diagnostic = {};
		if (values.size() > Limits::MaximumLinks || !ValidSourceCameraEvaluationPolicy(policy)) {
			diagnostic = {
				Status::InvalidValue, {}, {}, "source camera controls or policy exceed admission bounds"
			};
			return diagnostic.Code;
		}
		const auto *entry = FindCatalogueEntry(node.Type);
		if (!entry) {
			diagnostic = {Status::UnsupportedExecution, node.Id, {}, "camera catalogue entry is unavailable"};
			return diagnostic.Code;
		}
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		context.Project.SurfaceWidth = policy.ProjectWidth;
		context.Project.SurfaceHeight = policy.ProjectHeight;
		context.Project.ColorDepth = policy.ProjectColorDepth;
		context.ValueViews.reserve(values.size());
		for (const auto &value : values)
			context.ValueViews.emplace_back(value.Port, &value.Data);
		if (policy.DimensionLinked) context.LinkedValues.push_back("dimension");
		uint32_t w = 0, h = 0;
		if (!detail::ResolveDimension(context, "dimension", w, h))
			return CameraContextError(context, diagnostic);
		width = w;
		height = h;
		return Status::Ok;
	}
	Status ResolveSourceCameraDimensions(
		const Node &node,
		const SourceCameraEvaluationPolicy &policy,
		std::span<const AuthoredValue> values,
		uint32_t &width,
		uint32_t &height,
		Diagnostic &diagnostic
	) {
		return HostCameraDimensions(node, policy, values, width, height, diagnostic);
	}

	Status ResolveSourceCameraDimensions(
		const Node &node,
		const SourceCameraEvaluationPolicy &policy,
		std::span<const EvaluationInputValue> values,
		uint32_t &width,
		uint32_t &height,
		Diagnostic &diagnostic
	) {
		return HostCameraDimensions(node, policy, values, width, height, diagnostic);
	}

	template <class Input>
	Status HostCameraSurfaceFormat(
		const Node &node,
		const SourceCameraEvaluationPolicy &policy,
		std::span<const Input> values,
		std::optional<SurfaceFormat> inherited,
		SurfaceFormat &format,
		Diagnostic &diagnostic
	) {
		diagnostic = {};
		if (values.size() > Limits::MaximumLinks || !ValidSourceCameraEvaluationPolicy(policy)) {
			diagnostic = {
				Status::InvalidValue, {}, {}, "source camera controls or policy exceed admission bounds"
			};
			return diagnostic.Code;
		}
		const auto *entry = FindCatalogueEntry(node.Type);
		if (!entry) {
			diagnostic = {Status::UnsupportedExecution, node.Id, {}, "camera catalogue entry is unavailable"};
			return diagnostic.Code;
		}
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		context.Project.ColorDepth = policy.ProjectColorDepth;
		context.InheritedSurfaceFormat = inherited;
		context.ValueViews.reserve(values.size());
		for (const auto &value : values)
			context.ValueViews.emplace_back(value.Port, &value.Data);
		const auto resolved = detail::ResolveProcessorSurfaceFormat(context, nullptr);
		if (!resolved) return CameraContextError(context, diagnostic);
		format = *resolved;
		return Status::Ok;
	}
	Status ResolveSourceCameraSurfaceFormat(
		const Node &node,
		const SourceCameraEvaluationPolicy &policy,
		std::span<const AuthoredValue> values,
		std::optional<SurfaceFormat> inherited,
		SurfaceFormat &format,
		Diagnostic &diagnostic
	) {
		return HostCameraSurfaceFormat(node, policy, values, inherited, format, diagnostic);
	}

	Status ResolveSourceCameraSurfaceFormat(
		const Node &node,
		const SourceCameraEvaluationPolicy &policy,
		std::span<const EvaluationInputValue> values,
		std::optional<SurfaceFormat> inherited,
		SurfaceFormat &format,
		Diagnostic &diagnostic
	) {
		return HostCameraSurfaceFormat(node, policy, values, inherited, format, diagnostic);
	}

	template <class Input>
	Status HostCameraShader(
		const SourceCameraEvaluationPolicy &policy,
		std::span<const Input> values,
		uint32_t &shader,
		Diagnostic &diagnostic
	) {
		diagnostic = {};
		if (values.size() > Limits::MaximumLinks || !ValidSourceCameraEvaluationPolicy(policy)) {
			diagnostic = {
				Status::InvalidValue, {}, {}, "source camera controls or policy exceed admission bounds"
			};
			return diagnostic.Code;
		}
		CameraReader reader(values, diagnostic);
		double choice = 0;
		if (!reader.Number("shader", choice)) return diagnostic.Code;
		if (choice != 0 && choice != 1 && choice != 2) {
			reader.Fail("shader", "source camera shader choice is invalid");
			return diagnostic.Code;
		}
		const int64_t resolved = choice == 0 ? policy.ProjectShader3D : int64_t(choice) - 1;
		if (resolved < 0 || resolved > 1) {
			reader.Fail("shader", "source project shader choice is invalid");
			return diagnostic.Code;
		}
		shader = uint32_t(resolved);
		return Status::Ok;
	}
	Status ResolveSourceCameraShader(
		const SourceCameraEvaluationPolicy &policy,
		std::span<const AuthoredValue> values,
		uint32_t &shader,
		Diagnostic &diagnostic
	) {
		return HostCameraShader(policy, values, shader, diagnostic);
	}

	Status ResolveSourceCameraShader(
		const SourceCameraEvaluationPolicy &policy,
		std::span<const EvaluationInputValue> values,
		uint32_t &shader,
		Diagnostic &diagnostic
	) {
		return HostCameraShader(policy, values, shader, diagnostic);
	}
}
