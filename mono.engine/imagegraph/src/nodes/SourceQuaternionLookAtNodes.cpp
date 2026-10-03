#include "../SourceQuaternion.hpp"
#include "Families.hpp"
#include "Processor.hpp"

namespace engine::imagegraph::detail {
	namespace {
		bool LookAtFinite(Vector3 v) {
			return std::isfinite(v.X) && std::isfinite(v.Y) && std::isfinite(v.Z);
		}
		bool LookAtNumber(const Value &v, double &out) {
			if (const auto *p = std::get_if<double>(&v))
				out = *p;
			else if (const auto *p = std::get_if<int64_t>(&v))
				out = double(*p);
			else if (const auto *p = std::get_if<bool>(&v))
				out = *p ? 1. : 0.;
			else if (const auto *p = std::get_if<EnumValue>(&v))
				out = double(p->Value);
			else
				return false;
			return std::isfinite(out);
		}
		bool LookAtControl(NodeContext &c, std::string_view id, Vector3 &out) {
			const Value *v = c.Find(id);
			if (!v) return true;
			if (const auto *p = std::get_if<Vector3>(v))
				out = *p;
			else if (const auto *p = std::get_if<Vector2>(v))
				out = {p->X, p->Y, 0};
			else if (const auto *p = std::get_if<Vector4>(v))
				out = {p->X, p->Y, p->Z};
			else if (const auto *p = std::get_if<Quaternion>(v))
				out = {p->X, p->Y, p->Z};
			else if (const auto *a = std::get_if<ArrayValue>(v)) {
				if (!ValidRuntimeValue(*v) || !a->Nested.empty() || !a->Items.empty())
					return c.Fail(
						Status::UnsupportedExecution, "Look At requires selected numeric coordinate rows", id
					);
				out = {};
				double *components[] = {&out.X, &out.Y, &out.Z};
				for (size_t i = 0; i < std::min<size_t>(3, a->Elements.size()); ++i) {
					const bool numeric = std::visit(
						[&](const auto &leaf) {
							using T = std::decay_t<decltype(leaf)>;
							if constexpr (std::is_same_v<T, double> || std::is_same_v<T, int64_t> ||
										  std::is_same_v<T, bool>) {
								*components[i] = double(leaf);
								return std::isfinite(*components[i]);
							} else if constexpr (std::is_same_v<T, EnumValue>) {
								*components[i] = double(leaf.Value);
								return true;
							} else
								return false;
						},
						a->Elements[i]
					);
					if (!numeric)
						return c.Fail(Status::TypeMismatch, "Look At coordinate must be numeric", id);
				}
			} else {
				double scalar = 0;
				if (!LookAtNumber(*v, scalar))
					return c.Fail(Status::TypeMismatch, "Look At requires numeric coordinates", id);
				out = {scalar, scalar, scalar};
			}
			return LookAtFinite(out) ||
				   c.Fail(Status::InvalidValue, "Look At coordinates must be finite", id);
		}
		double LookAtLengthSquared(Vector3 v) {
			return v.X * v.X + v.Y * v.Y + v.Z * v.Z;
		}
		// Native finite-double profile preserves BBMOD's small-vector normalization
		// threshold.
		bool LookAtNormalize(Vector3 &v) {
			double squared = LookAtLengthSquared(v);
			if (!std::isfinite(squared)) return false;
			if (squared >= .00001) {
				double factor = 1 / std::sqrt(squared);
				v = {v.X * factor, v.Y * factor, v.Z * factor};
			}
			return LookAtFinite(v);
		}
		Quaternion LookAtRotation(Vector3 forward, Vector3 up, bool &valid) {
			valid = LookAtNormalize(forward);
			if (!valid) return {};
			const double dot = up.X * forward.X + up.Y * forward.Y + up.Z * forward.Z;
			up = {up.X - forward.X * dot, up.Y - forward.Y * dot, up.Z - forward.Z * dot};
			const double lengthSquared = LookAtLengthSquared(up);
			if (!std::isfinite(lengthSquared)) {
				valid = false;
				return {};
			}
			if (std::sqrt(lengthSquared) <= 0) return {};
			if (!LookAtNormalize(up)) {
				valid = false;
				return {};
			}
			const Vector3 right{
				up.Y * forward.Z - up.Z * forward.Y,
				up.Z * forward.X - up.X * forward.Z,
				up.X * forward.Y - up.Y * forward.X
			};
			const double w = std::sqrt(std::abs(1 + right.X + up.Y + forward.Z)) * .5;
			if (w >= .0001) {
				const double r = 1 / (4 * w);
				return {(up.Z - forward.Y) * r, (forward.X - right.Z) * r, (right.Y - up.X) * r, w};
			}
			if (right.X > up.Y && right.X > forward.Z) {
				const double x = std::sqrt(std::abs(1 + right.X - up.Y - forward.Z)) * .5, r = 1 / (4 * x);
				return {x, (right.Y + up.X) * r, (forward.X + right.Z) * r, (up.Z - forward.Y) * r};
			}
			if (up.Y > forward.Z) {
				const double y = std::sqrt(std::abs(1 + up.Y - right.X - forward.Z)) * .5, r = 1 / (4 * y);
				return {(right.Y + up.X) * r, y, (up.Z + forward.Y) * r, (forward.X - right.Z) * r};
			}
			const double z = std::sqrt(std::abs(1 + forward.Z - right.X - up.Y)) * .5, r = 1 / (4 * z);
			return {(forward.X + right.Z) * r, (up.Z + forward.Y) * r, z, (right.Y - up.X) * r};
		}
		bool SourceQuaternionLookAt(NodeContext &c) {
			ENGINE_PROFILE("imagegraph.quaternion.lookat");
			if (c.ProcessorCount > Limits::MaximumArrayElements)
				return c.Fail(Status::LimitExceeded, "Look At rows exceed bounded array work", "rotation");
			Vector3 origin{}, target{1, 0, 0}, up{0, 0, -1};
			if (!LookAtControl(c, "origin", origin) || !LookAtControl(c, "target", target) ||
				!LookAtControl(c, "up", up))
				return false;
			const auto *unitValue = c.Find("unit");
			const auto *booleanUnit = unitValue ? std::get_if<bool>(unitValue) : nullptr;
			const double unit = booleanUnit ? double(*booleanUnit) : c.SourceChoice("unit");
			if (!std::isfinite(unit))
				return c.Fail(Status::InvalidValue, "Look At Unit must be finite", "unit");
			Vector3 forward{target.X - origin.X, origin.Y - target.Y, target.Z - origin.Z};
			if (!LookAtNormalize(forward) || !LookAtNormalize(up))
				return c.Fail(
					Status::UnsupportedExecution,
					"Look At source normalization overflow is unverified",
					"rotation"
				);
			const bool coincident = LookAtLengthSquared(forward) == 0;
			bool valid = true;
			const Quaternion q = coincident ? Quaternion{} : LookAtRotation(forward, up, valid);
			if (!valid || !std::isfinite(q.X) || !std::isfinite(q.Y) || !std::isfinite(q.Z) ||
				!std::isfinite(q.W))
				return c.Fail(
					Status::UnsupportedExecution,
					"Look At source arithmetic produced undefined coordinates",
					"rotation"
				);
			Value output = (coincident || unit == 0) ? Value{Vector4{q.X, q.Y, q.Z, q.W}}
													 : Value{SourceQuaternionToEuler(q)};
			if (!ValidRuntimeValue(output))
				return c.Fail(Status::InvalidValue, "Look At result must be finite", "rotation");
			if (!c.ReserveOutput(std::string{}.capacity(), "rotation")) return false;
			c.SetValue("rotation", std::move(output));
			return c.FailureCode == Status::Ok;
		}
	} // namespace
	std::span<const ExecutorEntry> SourceQuaternionLookAtExecutors() {
		static constexpr ExecutorEntry entries[] = {{"pc.quarternion_lookat", SourceQuaternionLookAt}};
		return entries;
	}
} // namespace engine::imagegraph::detail
