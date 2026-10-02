// Lattice points preserve the pinned source's x-fast order and singleton midpoint.

#include "Families.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace engine::imagegraph::detail {
	namespace {
		bool LatticeVector(NodeContext &context, std::string_view port, Vector3 fallback, Vector3 &result) {
			const Value *value = context.Find(port);
			result = fallback;
			if (value) {
				if (const auto *vector = std::get_if<Vector3>(value))
					result = *vector;
				else if (const auto *scalar = std::get_if<double>(value); scalar && port != "subdivision")
					result = {*scalar, *scalar, *scalar};
				else if (const auto *integer = std::get_if<int64_t>(value);
						 integer && port != "subdivision") {
					const double scalar = static_cast<double>(*integer);
					result = {scalar, scalar, scalar};
				} else if (const auto *array = std::get_if<ArrayValue>(value)) {
					if (!array->Nested.empty() || array->Elements.size() != 3)
						return context.Fail(
							Status::TypeMismatch, "lattice control requires three numeric components", port
						);
					std::array<double, 3> components{};
					for (size_t axis = 0; axis < components.size(); ++axis) {
						if (const auto *scalar = std::get_if<double>(&array->Elements[axis]))
							components[axis] = *scalar;
						else if (const auto *integer = std::get_if<int64_t>(&array->Elements[axis]))
							components[axis] = double(*integer);
						else
							return context.Fail(
								Status::TypeMismatch, "lattice control requires numeric components", port
							);
					}
					result = {components[0], components[1], components[2]};
				} else
					return context.Fail(
						Status::TypeMismatch, "lattice control requires a numeric vector", port
					);
			}
			if (!std::isfinite(result.X) || !std::isfinite(result.Y) || !std::isfinite(result.Z))
				return context.Fail(Status::InvalidValue, "lattice controls must be finite", port);
			return true;
		}

		bool Lattice3D(NodeContext &context) {
			Vector3 center, half, subdivision;
			if (!LatticeVector(context, "center", {}, center) ||
				!LatticeVector(context, "half_size", {1, 1, 1}, half) ||
				!LatticeVector(context, "subdivision", {2, 2, 2}, subdivision))
				return false;
			const std::array<double, 3> authored{subdivision.X, subdivision.Y, subdivision.Z};
			for (const double axis : authored)
				if (axis != std::trunc(axis))
					return context.Fail(
						Status::UnsupportedExecution,
						"fractional lattice subdivision allocation coercion is unverified",
						"subdivision"
					);
			std::array<size_t, 3> counts{};
			size_t points = 0;
			if (std::all_of(authored.begin(), authored.end(), [](double axis) { return axis > -1; })) {
				points = 1;
				for (size_t axis = 0; axis < counts.size(); ++axis) {
					if (authored[axis] >= Limits::MaximumArrayElements / 3)
						return context.Fail(
							Status::LimitExceeded, "lattice points exceed the element budget", "subdivision"
						);
					counts[axis] = static_cast<size_t>(authored[axis]) + 1;
					if (counts[axis] > Limits::MaximumArrayElements / 3 / points)
						return context.Fail(
							Status::LimitExceeded, "lattice points exceed the element budget", "subdivision"
						);
					points *= counts[axis];
				}
			}
			const uint64_t bytes = points * (sizeof(std::vector<ElementValue>) + 3 * sizeof(ElementValue)) +
								   std::max(std::string_view("points").size(), std::string{}.capacity());
			if (!context.ReserveOutput(bytes, "points")) return false;
			ArrayValue output{ValueType::Scalar, {}};
			output.Nested.reserve(points);
			const std::array<double, 3> origins{center.X - half.X, center.Y - half.Y, center.Z - half.Z};
			const std::array<double, 3> ends{center.X + half.X, center.Y + half.Y, center.Z + half.Z};
			// The source seeds ambient randomness but takes no draws while producing these positions.
			for (size_t index = 0; index < points; ++index) {
				const std::array<size_t, 3> coordinate{
					index % counts[0], (index / counts[0]) % counts[1], index / (counts[0] * counts[1])
				};
				std::array<double, 3> position{};
				for (size_t axis = 0; axis < position.size(); ++axis) {
					const double ratio =
						counts[axis] <= 1 ? .5 : double(coordinate[axis]) / double(counts[axis] - 1);
					position[axis] = origins[axis] + (ends[axis] - origins[axis]) * ratio;
					if (!std::isfinite(position[axis]))
						return context.Fail(
							Status::InvalidValue, "lattice point arithmetic is not finite", "points"
						);
				}
				auto &row = output.Nested.emplace_back();
				row.reserve(3);
				for (double component : position)
					row.emplace_back(component);
			}
			context.SetValue("points", std::move(output));
			return context.FailureCode == Status::Ok;
		}
	}

	std::span<const ExecutorEntry> PointExecutors() {
		static constexpr std::array ENTRIES{ExecutorEntry{"pc.scatter_point_lattice_3_d", Lattice3D}};
		return ENTRIES;
	}
}
