#include "../SourceQuaternion.hpp"
#include "Families.hpp"
#include "Processor.hpp"

#include <array>
#include <numbers>
namespace engine::imagegraph::detail {
	namespace {
		bool Publish(NodeContext &context, std::array<double, 16> values) {
			for (double value : values)
				if (!std::isfinite(value))
					return context.Fail(Status::InvalidValue, "matrix result is nonfinite", "matrix");
			if (!context.ReserveOutput(16 * sizeof(double) + 32, "matrix")) return false;
			MatrixValue result{4, 4, {values.begin(), values.end()}};
			context.SetValue("matrix", std::move(result));
			return context.FailureCode == Status::Ok;
		}
		bool Projection(NodeContext &context) {
			uint32_t width = 1, height = 1;
			if (!ResolveDimension(context, "view", width, height)) return false;
			const auto clipping = context.Vec2("clipping", {1, 10});
			const double mode = context.SourceChoice("projection", 1), fov = context.Scalar("fov", 60),
						 scale = context.Scalar("orthographic_scale", .5);
			if (mode != 0 && mode != 1)
				return context.Fail(Status::InvalidValue, "projection mode is invalid", "projection");
			std::array<double, 16> m{};
			const double near = clipping.X, far = clipping.Y;
			if (near == far || (mode == 0 && fov == 0)) {
				m[0] = m[5] = m[10] = m[15] = 1;
				return Publish(context, m);
			}
			if (mode == 0) {
				m[5] = 1 / std::tan(fov * std::numbers::pi / 360);
				m[0] = m[5] * height / width;
				m[10] = far / (far - near);
				m[11] = 1;
				m[14] = -near * far / (far - near);
			} else {
				m[0] = 2 * scale;
				m[5] = 2 * double(width) / height * scale;
				m[10] = 1 / (far - near);
				m[14] = near / (near - far);
				m[15] = 1;
			}
			return Publish(context, m);
		}
		bool Transform(NodeContext &context) {
			Vector3 position{}, scale{1, 1, 1};
			Quaternion rotation{};
			const auto readVector = [&](std::string_view port, Vector3 &output) {
				const auto *value = context.Find(port);
				if (!value) return true;
				if (const auto *typed = std::get_if<Vector3>(value)) {
					output = *typed;
					return true;
				}
				return context.Fail(
					Status::TypeMismatch, "source transform requires a three-component vector", port
				);
			};
			if (!readVector("position", position) || !readVector("scale", scale)) return false;
			if (const auto *value = context.Find("rotation")) {
				const auto *typed = std::get_if<Quaternion>(value);
				if (!typed)
					return context.Fail(
						Status::TypeMismatch, "source transform requires a quaternion", "rotation"
					);
				rotation = *typed;
			}
			const auto euler = SourceQuaternionToEuler(rotation);
			const double radians = -std::numbers::pi / 180;
			const double sinp = std::sin(euler.X * radians), cosp = std::cos(euler.X * radians),
						 sinh = std::sin(euler.Y * radians), cosh = std::cos(euler.Y * radians),
						 sinr = std::sin(euler.Z * radians), cosr = std::cos(euler.Z * radians);
			const double srsp = sinr * sinp, crsp = -cosr * sinp;
			std::array<double, 16> m{};
			m[0] = (cosr * cosh - srsp * sinh) * scale.X;
			m[4] = -sinr * cosp * scale.X;
			m[8] = (cosr * sinh + srsp * cosh) * scale.X;
			m[1] = (sinr * cosh - crsp * sinh) * scale.Y;
			m[5] = cosr * cosp * scale.Y;
			m[9] = (sinr * sinh + crsp * cosh) * scale.Y;
			m[2] = -cosp * sinh * scale.Z;
			m[6] = sinp * scale.Z;
			m[10] = cosp * cosh * scale.Z;
			if (context.Boolean("affine", true)) {
				m[12] = position.X;
				m[13] = position.Y;
				m[14] = position.Z;
				m[15] = 1;
			}
			return Publish(context, m);
		}
		bool Eigen(NodeContext &context) {
			const auto *value = context.Find("matrix");
			const auto *matrix = value ? std::get_if<MatrixValue>(value) : nullptr;
			if (!matrix || !ValidRuntimeValue(*value))
				return context.Fail(Status::InvalidValue, "eigen input requires a bounded matrix", "matrix");
			if (matrix->Rows != matrix->Columns)
				return context.Fail(Status::InvalidValue, "source eigen requires a square matrix", "matrix");
			const size_t n = matrix->Rows;
			// Source performs up to 1000 dense iterations for each eigenpair. Bound its cubic CPU work.
			if (n > 32)
				return context.Fail(
					Status::LimitExceeded, "source eigen iteration work exceeds native bounds", "matrix"
				);
			auto charge = context.ReserveWorkspace(
				(2 * n * n + 4 * n) * sizeof(double) + (n * n + n) * sizeof(ElementValue) +
					n * sizeof(std::vector<ElementValue>),
				"matrix"
			);
			if (!charge) return false;
			auto a = matrix->Values;
			std::vector<double> b(n), next(n), eigenvalues(n), outer(n * n);
			ArrayValue vectors{ValueType::Scalar, {}};
			vectors.Nested.reserve(n);
			for (size_t i = 0; i < n; ++i) {
				std::fill(b.begin(), b.end(), 1);
				double eigenvalue = 0;
				for (size_t iteration = 0; iteration < 1000; ++iteration) {
					std::fill(next.begin(), next.end(), 0);
					for (size_t row = 0; row < n; ++row)
						for (size_t col = 0; col < n; ++col)
							next[row] += a[row * n + col] * b[col];
					double norm = 0;
					for (double v : next)
						norm += v * v;
					norm = std::sqrt(norm);
					if (norm == 0 || !std::isfinite(norm))
						return context.Fail(
							Status::InvalidValue, "source eigen normalization is undefined", "matrix"
						);
					for (double &v : next)
						v /= norm;
					double candidate = 0;
					for (size_t row = 0; row < n; ++row) {
						double dot = 0;
						for (size_t col = 0; col < n; ++col)
							dot += a[row * n + col] * next[col];
						candidate += next[row] * dot;
					}
					if (!std::isfinite(candidate))
						return context.Fail(
							Status::InvalidValue, "source eigen quotient is nonfinite", "matrix"
						);
					// The pinned source breaks before assigning the converged candidate/vector.
					if (std::abs(candidate - eigenvalue) < .00001) break;
					eigenvalue = candidate;
					b = next;
				}
				eigenvalues[i] = eigenvalue;
				std::vector<ElementValue> row;
				row.reserve(n);
				for (double v : b)
					row.emplace_back(v);
				vectors.Nested.push_back(std::move(row));
				for (size_t row = 0; row < n; ++row)
					for (size_t col = 0; col < n; ++col)
						outer[row * n + col] = b[row] * b[col];
				for (size_t j = 0; j < n * n; ++j)
					a[j] -= eigenvalue * outer[j];
			}
			ArrayValue values{ValueType::Scalar, {}};
			values.Elements.reserve(n);
			for (double v : eigenvalues)
				values.Elements.emplace_back(v);
			context.SetValue("eigenvector", std::move(vectors));
			context.SetValue("eigenvalue", std::move(values));
			return context.FailureCode == Status::Ok;
		}
	}
	std::span<const ExecutorEntry> SourceMatrixExecutors() {
		static constexpr std::array entries{
			ExecutorEntry{"pc.matrix_projection", Projection},
			ExecutorEntry{"pc.matrix_transform_3_d", Transform},
			ExecutorEntry{"pc.matrix_eigen", Eigen}
		};
		return entries;
	}
}
