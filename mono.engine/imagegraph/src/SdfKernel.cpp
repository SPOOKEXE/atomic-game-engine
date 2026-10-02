#include "SdfMath.hpp"
#include "SdfPayload.hpp"

#include <array>
#include <numbers>

namespace engine::imagegraph {
	namespace {
		using namespace detail::sdf_math;
		V3 ToFloat(Vector3 p) {
			return {float(p.X), float(p.Y), float(p.Z)};
		}
		bool Finite(Vector3 p) {
			return std::isfinite(p.X) && std::isfinite(p.Y) && std::isfinite(p.Z) &&
				   std::abs(p.X) <= std::numeric_limits<float>::max() &&
				   std::abs(p.Y) <= std::numeric_limits<float>::max() &&
				   std::abs(p.Z) <= std::numeric_limits<float>::max();
		}
		float ShapeDistance(const SourceSdfShape &s, V3 p) {
			p = p - ToFloat(s.Position);
			const V3 r = ToFloat(s.Rotation) * (std::numbers::pi_v<float> / 180.f);
			float c = cos(r.x), sn = sin(r.x);
			p = {p.x, c * p.y - sn * p.z, sn * p.y + c * p.z};
			c = cos(r.y);
			sn = sin(r.y);
			p = {c * p.x + sn * p.z, p.y, -sn * p.x + c * p.z};
			c = cos(r.z);
			sn = sin(r.z);
			p = {c * p.x - sn * p.y, sn * p.x + c * p.y, p.z};
			p = p / float(s.Scale);
			const V3 a = ToFloat(s.WaveAmplitude),
					 w = ToFloat(s.WavePhase) * (2.f * std::numbers::pi_v<float>),
					 i = ToFloat(s.WaveIntensity);
			p.x += sin(p.y * a.y + w.x) * i.x + sin(p.z * a.z + w.x) * i.x;
			p.y += sin(p.x * a.x + w.y) * i.y + sin(p.z * a.z + w.y) * i.y;
			p.z += sin(p.y * a.y + w.z) * i.z + sin(p.x * a.x + w.z) * i.z;
			if (s.Tile) {
				const V3 size = ToFloat(s.TileDistance), amount = ToFloat(s.TileAmount);
				const bool infinite = amount.x == 0 && amount.y == 0 && amount.z == 0;
				const auto tile = [&](float v, float size, float amount) {
					const float quotient = v / size;
					const float lower = std::floor(quotient);
					const float rounded = quotient - lower >= .5f ? std::ceil(quotient) : lower;
					return size * (infinite ? rounded : clamp(rounded, -amount, amount));
				};
				p = p -
					V3{tile(p.x, size.x, amount.x), tile(p.y, size.y, amount.y), tile(p.z, size.z, amount.z)};
			}
			const float axis = s.TwistAxis == 0 ? p.x : s.TwistAxis == 1 ? p.y : p.z;
			c = cos(float(s.TwistAmount) * axis);
			sn = sin(float(s.TwistAmount) * axis);
			if (s.TwistAxis == 0)
				p = {p.x, c * p.y + sn * p.z, -sn * p.y + c * p.z};
			else if (s.TwistAxis == 1)
				p = {c * p.x + sn * p.z, p.y, -sn * p.x + c * p.z};
			else
				p = {c * p.x + sn * p.y, -sn * p.x + c * p.y, p.z};
			float elongation = 0;
			if (s.Elongate.X != 0 || s.Elongate.Y != 0 || s.Elongate.Z != 0) {
				const V3 q = abs(p) - ToFloat(s.Elongate);
				p = max(q, 0.f);
				elongation = min(max(q.x, max(q.y, q.z)), 0.f);
			}
			return (Primitive(s.Shape, p, s) + elongation - float(s.Rounded)) * float(s.Scale);
		}
		float Distance(const SdfData &data, V3 p) {
			std::array<float, SOURCE_SDF_MAXIMUM_OPERATIONS> stack{};
			size_t top = 0;
			for (const auto &op : data.Operations) {
				if (op.Code < 100) {
					stack[top++] = ShapeDistance(data.Shapes[size_t(op.Code)], p);
					continue;
				}
				const float right = stack[--top], left = stack[--top], k = float(op.Merge);
				float d = 0;
				if (op.Code == 100)
					d = min(right, left);
				else if (op.Code == 101) {
					const float h = 1.f - min(abs(right - left) / (4.f * k), 1.f);
					d = min(right, left) - h * h * k;
				} else if (op.Code == 102) {
					const float h = clamp(.5f - .5f * (left + right) / k, 0.f, 1.f);
					d = mix(left, -right, h) + k * h * (1.f - h);
				} else {
					const float h = clamp(.5f - .5f * (left - right) / k, 0.f, 1.f);
					d = mix(left, right, h) + k * h * (1.f - h);
				}
				stack[top++] = d;
			}
			return top ? stack[0] : std::numeric_limits<float>::infinity();
		}
		Status Fail(Diagnostic &d, std::string message) {
			d = {Status::InvalidValue, {}, {}, std::move(message)};
			return d.Code;
		}
	}
	bool ValidateSourceSdfValue(const SdfValue &sdf) {
		return detail::ValidSdfPayload(sdf);
	}

	Status SampleSourceSdf(const SdfValue &sdf, Vector3 point, double &distance, Diagnostic &diagnostic) {
		if (!detail::ValidSdfPayload(sdf) || !Finite(point))
			return Fail(
				diagnostic, "SDF sample requires a bounded valid resource and finite float coordinates"
			);
		if (!sdf.Data || sdf.Data->Operations.empty())
			return Fail(diagnostic, "empty SDF has no finite distance");
		const float sampled = Distance(*sdf.Data, ToFloat(point));
		if (!std::isfinite(sampled))
			return Fail(diagnostic, "source SDF kernel produced a non-finite distance");
		distance = sampled;
		diagnostic = {};
		return Status::Ok;
	}
	Status MarchSourceSdf(
		const SdfValue &sdf,
		Vector3 camera,
		Vector3 direction,
		Vector2 viewRange,
		SourceSdfMarchResult &result,
		Diagnostic &diagnostic
	) {
		if (!detail::ValidSdfPayload(sdf) || !Finite(camera) || !Finite(direction) ||
			!std::isfinite(viewRange.X) || !std::isfinite(viewRange.Y) || viewRange.X < 0 ||
			viewRange.Y <= viewRange.X || viewRange.Y > std::numeric_limits<float>::max())
			return Fail(
				diagnostic, "SDF march requires finite coordinates and an ordered nonnegative view interval"
			);
		V3 ray = ToFloat(direction);
		const float magnitude = length(ray);
		if (!std::isfinite(magnitude) || magnitude == 0)
			return Fail(diagnostic, "SDF march direction must have a finite nonzero length");
		ray = ray / magnitude;
		SourceSdfMarchResult next;
		next.Depth = viewRange.Y;
		if (!sdf.Data || sdf.Data->Operations.empty()) {
			result = next;
			diagnostic = {};
			return Status::Ok;
		}
		float depth = float(viewRange.X);
		const V3 eye = ToFloat(camera);
		for (uint32_t step = 0; step < 512; ++step) {
			const V3 point = eye + ray * depth;
			const float distance = Distance(*sdf.Data, point);
			next.Steps = step + 1;
			if (!std::isfinite(distance))
				return Fail(diagnostic, "source SDF march produced a non-finite distance");
			if (distance < 1e-5f) {
				next.Depth = depth;
				next.Hit = true;
				V3 normal{};
				for (const V3 e :
					 std::array<V3, 4>{V3{1, -1, -1}, V3{-1, -1, 1}, V3{-1, 1, -1}, V3{1, 1, 1}}) {
					const V3 offset = e * .0001f;
					normal = normal + offset * Distance(*sdf.Data, point + offset);
				}
				const float norm = length(normal);
				if (!std::isfinite(norm) || norm == 0)
					return Fail(diagnostic, "source SDF hit normal is undefined");
				normal = normal / norm;
				next.Normal = {normal.x, normal.y, normal.z};
				result = next;
				diagnostic = {};
				return Status::Ok;
			}
			depth += distance;
			if (depth >= float(viewRange.Y)) break;
		}
		result = next;
		diagnostic = {};
		return Status::Ok;
	}
}
