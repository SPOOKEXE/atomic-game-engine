#include <engine/graph/Cull.hpp>
#include <engine/graph/Frustum.hpp>
#include <engine/render/RuntimeDiagnostics.hpp>
#include <engine/scene/ActiveCamera.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>

namespace engine::render {

	namespace {
		using core::AABB;
		using core::CFrame;
		using core::Color3;
		using core::Ray;
		using core::Vector3;

		constexpr float MINIMUM_DISTANCE = 0.01f;
		constexpr size_t MAX_PASS_THROUGH_BOUNDS = 16;
		constexpr Color3 EMPTY_SPACE{0.10f, 0.55f, 1.0f};
		constexpr Color3 PASS_THROUGH{1.0f, 0.48f, 0.08f};
		constexpr Color3 REFLECTION{1.0f, 0.48f, 0.08f};
		constexpr Color3 TERMINATION{1.0f, 0.12f, 0.12f};
		constexpr Color3 CAMERA_LOCK{0.18f, 0.92f, 0.90f};

		Color3 ColourFor(LightProbeEvent event) {
			switch (event) {
			case LightProbeEvent::EmptySpace:
				return EMPTY_SPACE;
			case LightProbeEvent::PassThrough:
				return PASS_THROUGH;
			case LightProbeEvent::Reflection:
				return REFLECTION;
			case LightProbeEvent::Termination:
				return TERMINATION;
			}
			return EMPTY_SPACE;
		}

		struct RayBound {
			float Enter = 0.0f;
			float Leave = 0.0f;
		};

		std::optional<RayBound> RayBoxDistance(const Ray &ray, const AABB &bounds, float maximum) {
			float enter = 0.0f;
			float leave = maximum;
			const std::array<float, 3> origin{ray.Origin.X, ray.Origin.Y, ray.Origin.Z};
			const std::array<float, 3> direction{ray.Direction.X, ray.Direction.Y, ray.Direction.Z};
			const std::array<float, 3> minimum{bounds.Minimum.X, bounds.Minimum.Y, bounds.Minimum.Z};
			const std::array<float, 3> maximums{bounds.Maximum.X, bounds.Maximum.Y, bounds.Maximum.Z};

			for (size_t axis = 0; axis < origin.size(); axis++) {
				if (std::abs(direction[axis]) < std::numeric_limits<float>::epsilon()) {
					if (origin[axis] < minimum[axis] || origin[axis] > maximums[axis]) return std::nullopt;
					continue;
				}

				float first = (minimum[axis] - origin[axis]) / direction[axis];
				float second = (maximums[axis] - origin[axis]) / direction[axis];
				if (first > second) std::swap(first, second);
				enter = std::max(enter, first);
				leave = std::min(leave, second);
				if (enter > leave) return std::nullopt;
			}

			if (leave < MINIMUM_DISTANCE) return std::nullopt;
			return RayBound{enter, leave};
		}

		struct ProbeBounds {
			float Stop = 0.0f;
			std::array<RayBound, MAX_PASS_THROUGH_BOUNDS> PassThrough{};
			size_t Count = 0;
			const scene::DrawInstance *Reflection = nullptr;
			float ReflectionDistance = 0.0f;
		};

		ProbeBounds TraceBounds(
			const Ray &ray,
			float maximum,
			std::span<const scene::DrawInstance> instances,
			const scene::DrawInstance *ignored = nullptr
		) {
			ProbeBounds result;
			result.Stop = maximum;
			for (const scene::DrawInstance &instance : instances) {
				if (&instance == ignored) continue;
				const AABB bounds = graph::BoundsOf(instance);
				// A local light commonly shares its part's bounds. Treating that
				// enclosure as an occluder would make every sample end at its source.
				if (ray.Origin.X >= bounds.Minimum.X && ray.Origin.X <= bounds.Maximum.X &&
					ray.Origin.Y >= bounds.Minimum.Y && ray.Origin.Y <= bounds.Maximum.Y &&
					ray.Origin.Z >= bounds.Minimum.Z && ray.Origin.Z <= bounds.Maximum.Z) {
					continue;
				}
				const auto hit = RayBoxDistance(ray, bounds, maximum);
				if (!hit) continue;
				const float contact = hit->Enter >= MINIMUM_DISTANCE ? hit->Enter : hit->Leave;
				if (instance.SurfaceIsPortal) {
					// A portal samples a surface slot but opens into another view. It
					// does not reverse this ray, so make its aperture visible as an
					// orange pass-through interval instead of a reflection.
				} else if (instance.Surface >= 0) {
					if (result.Reflection == nullptr || contact < result.ReflectionDistance) {
						result.Reflection = &instance;
						result.ReflectionDistance = contact;
					}
					continue;
				} else if (!scene::IsTransparent(instance) && instance.CastShadow) {
					result.Stop = std::min(result.Stop, contact);
					continue;
				}
				if (result.Count < result.PassThrough.size()) {
					result.PassThrough[result.Count++] = *hit;
					continue;
				}
				const auto farthest = std::max_element(
					result.PassThrough.begin(),
					result.PassThrough.end(),
					[](const RayBound &left, const RayBound &right) { return left.Enter < right.Enter; }
				);
				if (hit->Enter < farthest->Enter) *farthest = *hit;
			}
			std::sort(
				result.PassThrough.begin(),
				result.PassThrough.begin() + result.Count,
				[](const RayBound &left, const RayBound &right) { return left.Enter < right.Enter; }
			);
			if (result.Reflection != nullptr) {
				if (result.ReflectionDistance >= result.Stop) {
					result.Reflection = nullptr;
				} else {
					result.Stop = result.ReflectionDistance;
				}
			}
			return result;
		}

		Vector3 SurfaceNormal(const AABB &bounds, const Vector3 &point, const Vector3 &fallback) {
			const std::array<float, 6> distances{
				std::abs(point.X - bounds.Minimum.X),
				std::abs(point.X - bounds.Maximum.X),
				std::abs(point.Y - bounds.Minimum.Y),
				std::abs(point.Y - bounds.Maximum.Y),
				std::abs(point.Z - bounds.Minimum.Z),
				std::abs(point.Z - bounds.Maximum.Z),
			};
			const size_t face = static_cast<size_t>(
				std::distance(distances.begin(), std::min_element(distances.begin(), distances.end()))
			);
			switch (face) {
			case 0:
				return {-1.0f, 0.0f, 0.0f};
			case 1:
				return {1.0f, 0.0f, 0.0f};
			case 2:
				return {0.0f, -1.0f, 0.0f};
			case 3:
				return {0.0f, 1.0f, 0.0f};
			case 4:
				return {0.0f, 0.0f, -1.0f};
			case 5:
				return {0.0f, 0.0f, 1.0f};
			default:
				return fallback * -1.0f;
			}
		}

		void AddProbe(
			std::vector<LightProbeSegment> &paths,
			const Vector3 &from,
			const Vector3 &direction,
			float distance,
			LightProbeEvent event
		) {
			if (!(distance > 0.0f)) return;
			AdornmentLine line;
			line.From = from;
			line.To = from + direction * distance;
			line.Colour = ColourFor(event);
			line.Transparency = 0.0f;
			line.Thickness = 0.035f;
			line.AlwaysOnTop = true;
			paths.push_back({line, event});
		}

		void AddTermination(
			std::vector<LightProbeSegment> &paths, const Vector3 &at, const Vector3 &direction, float range
		) {
			const Vector3 reference =
				std::abs(direction.Y) < 0.95f ? Vector3{0.0f, 1.0f, 0.0f} : Vector3{1.0f, 0.0f, 0.0f};
			const Vector3 across = direction.Cross(reference).Unit();
			const Vector3 upward = direction.Cross(across).Unit();
			const float radius = std::clamp(range * 0.02f, 0.05f, 0.35f);
			for (const Vector3 offset : {across, upward}) {
				AdornmentLine line;
				line.From = at - offset * radius;
				line.To = at + offset * radius;
				line.Colour = ColourFor(LightProbeEvent::Termination);
				line.Transparency = 0.0f;
				line.Thickness = 0.055f;
				line.AlwaysOnTop = true;
				paths.push_back({line, LightProbeEvent::Termination});
			}
		}

		void AddPointDirections(std::array<Vector3, 6> &directions) {
			directions = {
				Vector3{1.0f, 0.0f, 0.0f},
				Vector3{-1.0f, 0.0f, 0.0f},
				Vector3{0.0f, 1.0f, 0.0f},
				Vector3{0.0f, -1.0f, 0.0f},
				Vector3{0.0f, 0.0f, 1.0f},
				Vector3{0.0f, 0.0f, -1.0f},
			};
		}

		size_t AddSpotDirections(const SceneLight &light, std::array<Vector3, 6> &directions) {
			const Vector3 forward = light.Direction.Unit();
			if (forward == Vector3::Zero) return 0;
			directions[0] = forward;
			const float cosine = std::clamp(light.ConeCosine, -1.0f, 1.0f);
			const float angle = std::acos(cosine);
			if (!(angle > 0.0f)) return 1;

			const Vector3 reference =
				std::abs(forward.Y) < 0.95f ? Vector3{0.0f, 1.0f, 0.0f} : Vector3{1.0f, 0.0f, 0.0f};
			const Vector3 right = forward.Cross(reference).Unit();
			const Vector3 up = right.Cross(forward).Unit();
			const float sine = std::sin(angle);
			for (size_t sample = 0; sample < 4; sample++) {
				const float theta = std::numbers::pi_v<float> * 0.5f * static_cast<float>(sample);
				const Vector3 radial = right * std::cos(theta) + up * std::sin(theta);
				directions[sample + 1] = (forward * cosine + radial * sine).Unit();
			}
			return 5;
		}

		void AddLine(
			std::vector<AdornmentLine> &lines, const Vector3 &from, const Vector3 &to, const Color3 &colour
		) {
			AdornmentLine line;
			line.From = from;
			line.To = to;
			line.Colour = colour;
			line.Thickness = 0.04f;
			line.AlwaysOnTop = true;
			lines.push_back(line);
		}
	}

	void LightPathGeometry::Build(
		std::span<const SceneLight> lights, std::span<const scene::DrawInstance> instances
	) {
		Paths.clear();
		Paths.reserve(lights.size() * 18);
		for (const SceneLight &light : lights) {
			if (!(light.Range > MINIMUM_DISTANCE)) continue;

			std::array<Vector3, 6> directions{};
			size_t directionCount = 0;
			if (light.ConeCosine < 0.0f) {
				AddPointDirections(directions);
				directionCount = directions.size();
			} else {
				directionCount = AddSpotDirections(light, directions);
			}

			for (size_t index = 0; index < directionCount; index++) {
				const Vector3 &direction = directions[index];
				if (direction == Vector3::Zero) continue;
				const Ray ray{light.Position, direction};
				const ProbeBounds bounds = TraceBounds(ray, light.Range, instances);
				float travelled = 0.0f;
				for (size_t pass = 0; pass < bounds.Count; ++pass) {
					const RayBound &hit = bounds.PassThrough[pass];
					if (hit.Enter >= bounds.Stop) break;
					const float entry = std::max(travelled, hit.Enter);
					const float leave = std::min(bounds.Stop, hit.Leave);
					if (leave <= entry) continue;
					AddProbe(
						Paths,
						ray.PointAt(travelled),
						direction,
						entry - travelled,
						LightProbeEvent::EmptySpace
					);
					AddProbe(
						Paths, ray.PointAt(entry), direction, leave - entry, LightProbeEvent::PassThrough
					);
					travelled = leave;
				}
				AddProbe(
					Paths,
					ray.PointAt(travelled),
					direction,
					bounds.Stop - travelled,
					LightProbeEvent::EmptySpace
				);
				if (bounds.Reflection == nullptr) {
					AddTermination(Paths, ray.PointAt(bounds.Stop), direction, light.Range);
					continue;
				}

				const Vector3 reflectionPoint = ray.PointAt(bounds.ReflectionDistance);
				const Vector3 normal =
					SurfaceNormal(graph::BoundsOf(*bounds.Reflection), reflectionPoint, direction);
				const Vector3 reflected = (direction - normal * (2.0f * direction.Dot(normal))).Unit();
				if (reflected == Vector3::Zero) {
					AddTermination(Paths, reflectionPoint, direction, light.Range);
					continue;
				}

				const float remaining = light.Range - bounds.ReflectionDistance;
				const Ray bounced{reflectionPoint + reflected * MINIMUM_DISTANCE, reflected};
				const ProbeBounds rebound = TraceBounds(bounced, remaining, instances, bounds.Reflection);
				const float reflectedDistance = std::min(remaining, rebound.Stop + MINIMUM_DISTANCE);
				AddProbe(Paths, reflectionPoint, reflected, reflectedDistance, LightProbeEvent::Reflection);
				AddTermination(
					Paths, reflectionPoint + reflected * reflectedDistance, reflected, light.Range
				);
			}
		}
	}

	size_t CullForCamera(
		std::span<const scene::DrawInstance> instances,
		const CFrame &cameraFrame,
		const scene::Camera &camera,
		float aspectRatio,
		std::vector<uint32_t> &visible
	) {
		visible.clear();
		if (!(aspectRatio > 0.0f)) return 0;
		const scene::CameraMatrices matrices = scene::ResolveCamera(cameraFrame, camera, aspectRatio);
		return graph::Cull(instances, graph::Frustum::FromViewProjection(matrices.ViewProjection), visible);
	}

	void AppendCameraLockAdornment(
		std::vector<AdornmentLine> &lines,
		const CFrame &frame,
		const scene::Camera &camera,
		float aspectRatio,
		float distance
	) {
		if (!(aspectRatio > 0.0f) || !(distance > 0.0f)) return;
		const float halfHeight = std::tan(camera.FieldOfViewRadians * 0.5f) * distance;
		const float halfWidth = halfHeight * aspectRatio;
		const std::array<Vector3, 4> corners{
			frame.PointToWorldSpace({-halfWidth, -halfHeight, -distance}),
			frame.PointToWorldSpace({halfWidth, -halfHeight, -distance}),
			frame.PointToWorldSpace({halfWidth, halfHeight, -distance}),
			frame.PointToWorldSpace({-halfWidth, halfHeight, -distance}),
		};
		for (size_t corner = 0; corner < corners.size(); corner++) {
			AddLine(lines, frame.Position, corners[corner], CAMERA_LOCK);
			AddLine(lines, corners[corner], corners[(corner + 1) % corners.size()], CAMERA_LOCK);
		}
	}
}
