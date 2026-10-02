// Source vector creators and manual vector arithmetic from the pinned Pixel Composer scripts.

#include "../ValuePayload.hpp"
#include "Families.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace engine::imagegraph::detail {
	namespace {
		struct VectorRow {
			bool Scalar = true;
			double Number = 0;
			std::span<const ElementValue> Elements;
			std::array<double, 6> Packed{};
			size_t Count = 0;

			double At(size_t index) const {
				if (Scalar) return Number;
				if (Count) return index < Count ? Packed[index] : 0;
				if (index >= Elements.size()) return 0;
				return std::visit(
					[](const auto &leaf) -> double {
						using T = std::decay_t<decltype(leaf)>;
						if constexpr (std::is_same_v<T, double> || std::is_same_v<T, int64_t> ||
									  std::is_same_v<T, bool>)
							return static_cast<double>(leaf);
						else if constexpr (std::is_same_v<T, EnumValue>)
							return static_cast<double>(leaf.Value);
						else
							return 0;
					},
					Elements[index]
				);
			}
		};

		template <class T> bool PackedRow(const T &value, VectorRow &row) {
			if constexpr (std::is_same_v<T, double> || std::is_same_v<T, int64_t> || std::is_same_v<T, bool>)
				row.Number = static_cast<double>(value);
			else if constexpr (std::is_same_v<T, EnumValue>)
				row.Number = static_cast<double>(value.Value);
			else if constexpr (std::is_same_v<T, Vector2>) {
				row.Packed = {value.X, value.Y};
				row.Count = 2;
			} else if constexpr (std::is_same_v<T, Vector3>) {
				row.Packed = {value.X, value.Y, value.Z};
				row.Count = 3;
			} else if constexpr (std::is_same_v<T, Vector4> || std::is_same_v<T, Quaternion>) {
				row.Packed = {value.X, value.Y, value.Z, value.W};
				row.Count = 4;
			} else if constexpr (std::is_same_v<T, Area>) {
				row.Packed = {
					value.CenterX,
					value.CenterY,
					value.HalfWidth,
					value.HalfHeight,
					double(value.Shape),
					double(value.Mode)
				};
				row.Count = 6;
			} else
				return false;
			row.Scalar = row.Count == 0;
			return true;
		}

		bool NumericArrayType(ValueType type) {
			return type == ValueType::Scalar || type == ValueType::Integer || type == ValueType::Boolean ||
				   type == ValueType::Enum;
		}
		bool PackedArrayType(ValueType type) {
			return type == ValueType::Vector2 || type == ValueType::Vector3 || type == ValueType::Vector4 ||
				   type == ValueType::Quaternion || type == ValueType::Area;
		}

		struct VectorInput {
			VectorRow Single;
			const ArrayValue *Array = nullptr;
			bool FlatScalars = false;
			bool PackedRows = false;
			size_t Rows = 1;

			VectorRow Row(size_t index) const {
				if (!Array) {
					if (!FlatScalars) return Single;
					VectorRow row;
					row.Number = Single.At(index % Rows);
					return row;
				}
				VectorRow row;
				index %= Rows;
				if (PackedRows) {
					std::visit([&](const auto &leaf) { PackedRow(leaf, row); }, Array->Elements[index]);
				} else if (!Array->Nested.empty()) {
					row.Scalar = false;
					row.Elements = Array->Nested[index];
				} else if (FlatScalars) {
					VectorRow flat;
					flat.Scalar = false;
					flat.Elements = Array->Elements;
					row.Number = flat.At(index);
				} else {
					row.Scalar = false;
					row.Elements = Array->Elements;
				}
				return row;
			}
		};

		bool
		ReadVectorInput(NodeContext &context, std::string_view port, bool scalarEntries, VectorInput &input) {
			const Value *value = context.Find(port);
			if (!value || !ValidRuntimeValue(*value))
				return context.Fail(
					Status::InvalidValue, "vector input needs a bounded finite payload", port
				);
			if (const auto *array = std::get_if<ArrayValue>(value)) {
				input.Array = array;
				if (!array->Nested.empty()) {
					if (!NumericArrayType(array->ElementType))
						return context.Fail(
							Status::UnsupportedExecution,
							"vector arithmetic exceeds supported source depth",
							port
						);
					// The source depth probe stops at an empty first row and then treats rows as numeric
					// leaves.
					if (array->Nested.front().empty())
						return context.Fail(
							Status::UnsupportedExecution,
							"source vector depth probe cannot classify an empty first row",
							port
						);
					input.Rows = array->Nested.size();
				} else if (PackedArrayType(array->ElementType) && !array->Elements.empty()) {
					input.PackedRows = true;
					input.Rows = array->Elements.size();
				} else {
					if (!NumericArrayType(array->ElementType) && !array->Elements.empty())
						return context.Fail(
							Status::UnsupportedExecution, "vector arithmetic needs numeric source rows", port
						);
					input.FlatScalars = scalarEntries;
					input.Rows = scalarEntries ? array->Elements.size() : 1;
				}
			} else {
				if (!std::visit([&](const auto &leaf) { return PackedRow(leaf, input.Single); }, *value))
					return context.Fail(
						Status::UnsupportedExecution, "vector input has no verified numeric shape", port
					);
				if (!input.Single.Scalar && scalarEntries) {
					input.FlatScalars = true;
					input.Rows = input.Single.Count;
				}
			}
			if (!input.Rows)
				return context.Fail(
					Status::UnsupportedExecution, "source vector row broadcast has an empty divisor", port
				);
			return true;
		}

		bool ReadComponent(NodeContext &context, std::string_view port, bool integer, double &number) {
			const Value *value = context.Find(port);
			VectorRow row;
			if (!value || !ValidRuntimeValue(*value) ||
				!std::visit([&](const auto &leaf) { return PackedRow(leaf, row); }, *value) || !row.Scalar)
				return context.Fail(
					Status::UnsupportedExecution, "vector component requires a finite selected scalar", port
				);
			number = integer ? std::nearbyint(row.Number) : row.Number;
			return true;
		}

		bool ReadFlag(NodeContext &context, std::string_view port, bool &flag) {
			double number;
			if (!ReadComponent(context, port, false, number)) return false;
			flag = number > .5;
			return true;
		}

		bool CreateVector(NodeContext &context, size_t count) {
			bool integer;
			if (!ReadFlag(context, "integer", integer)) return false;
			constexpr std::array<std::string_view, 4> ports{"x", "y", "z", "w"};
			std::array<double, 4> values{};
			for (size_t i = 0; i < count; ++i)
				if (!ReadComponent(context, ports[i], integer, values[i])) return false;
			const uint64_t nameBytes = std::string{}.capacity();
			if (!context.ReserveOutput(
					std::max<uint64_t>(nameBytes, std::string_view("vector").size()), "vector"
				))
				return false;
			for (size_t i = 0; i < count; ++i)
				if (!context.ReserveOutput(std::max<uint64_t>(nameBytes, ports[i].size()), ports[i]))
					return false;
			if (count == 2)
				context.SetValue("vector", Vector2{values[0], values[1]});
			else
				context.SetValue("vector", Vector4{values[0], values[1], values[2], values[3]});
			for (size_t i = 0; i < count; ++i)
				context.SetValue(ports[i], values[i]);
			return true;
		}
		bool Vector2Node(NodeContext &context) {
			return CreateVector(context, 2);
		}
		bool Vector4Node(NodeContext &context) {
			return CreateVector(context, 4);
		}

		// Official default comparison epsilon is inferred from the pinned source's absence of setters.
		double VectorArithmetic(double mode, double a, double b) {
			constexpr double EPSILON = .00001;
			if (mode == 0) return a + b;
			if (mode == 1) return a - b;
			if (mode == 2) return a * b;
			if (mode == 3) return std::abs(b) <= EPSILON ? 0 : a / b;
			if (mode == 4) return b >= -EPSILON ? std::pow(a, b) : 1 / std::pow(a, -b);
			if (mode == 5) return b <= EPSILON ? 0 : std::pow(a, 1 / b);
			return 0;
		}

		bool VectorMath(NodeContext &context) {
			const double mode = context.SourceChoice("type");
			bool scalarB;
			if (!ReadFlag(context, "scalar_b", scalarB) || context.FailureCode != Status::Ok) return false;
			const Value *dimensionValue = context.Find("dimension");
			VectorRow dimension;
			if (!dimensionValue || !ValidRuntimeValue(*dimensionValue) ||
				!std::visit([&](const auto &leaf) { return PackedRow(leaf, dimension); }, *dimensionValue) ||
				!dimension.Scalar)
				return context.Fail(
					Status::UnsupportedExecution, "vector dimension requires one finite scalar", "dimension"
				);
			const double rounded = std::nearbyint(dimension.Number);
			if (rounded < 0 || rounded > Limits::MaximumArrayElements)
				return context.Fail(
					Status::LimitExceeded, "vector dimension exceeds bounded work limit", "dimension"
				);
			const size_t length = static_cast<size_t>(rounded);
			VectorInput a, b;
			if (!ReadVectorInput(context, "a", false, a) || !ReadVectorInput(context, "b", scalarB, b))
				return false;
			const size_t rows = std::max(a.Rows, b.Rows);
			const uint64_t work = uint64_t(rows) * length;
			if (rows > Limits::MaximumArrayElements || work > Limits::MaximumArrayElements)
				return context.Fail(
					Status::LimitExceeded, "vector arithmetic exceeds aggregate work budget", "result"
				);
			const bool magnitude = mode == 6 || mode == 7;
			const uint64_t payload = magnitude ? (rows > 1 ? rows * sizeof(ElementValue) : 0)
													   : work * sizeof(ElementValue) +
															 (rows > 1 ? rows * sizeof(std::vector<ElementValue>) : 0);
			const uint64_t nameBytes = std::max<uint64_t>(std::string_view("result").size(), std::string{}.capacity());
			if (payload > Limits::MaximumArrayBytes || nameBytes > Limits::MaximumArrayBytes - payload)
				return context.Fail(
					Status::LimitExceeded, "vector result exceeds publication byte budget", "result"
				);
			const auto evaluate = [&](size_t index, std::vector<ElementValue> *out) {
				const VectorRow av = a.Row(index), bv = b.Row(index);
				double total = 0;
				for (size_t i = 0; i < length; ++i) {
					const double x = av.At(i), y = bv.At(i);
					const double result =
						magnitude ? (mode == 6 ? x * x : (x - y) * (x - y)) : VectorArithmetic(mode, x, y);
					if (!std::isfinite(result) || (magnitude && !std::isfinite(total + result)))
						return std::optional<double>{};
					if (magnitude)
						total += result;
					else if (out)
						out->emplace_back(result);
				}
				return std::optional<double>{magnitude ? std::sqrt(total) : 0};
			};
			for (size_t row = 0; row < rows; ++row)
				if (!evaluate(row, nullptr))
					return context.Fail(
						Status::InvalidValue, "source vector arithmetic produced a nonfinite result", "result"
					);
			// Source array_verify mutates nested inputs. Immutable graph inputs retain their original rows.
			if (magnitude && rows == 1) {
				if (!context.ReserveOutput(nameBytes, "result")) return false;
				context.SetValue("result", *evaluate(0, nullptr));
				return true;
			}
			if (!context.ReserveOutput(payload + nameBytes, "result")) return false;
			ArrayValue output{ValueType::Scalar, {}};
			if (magnitude) {
				output.Elements.reserve(rows);
				for (size_t row = 0; row < rows; ++row)
					output.Elements.emplace_back(*evaluate(row, nullptr));
			} else if (rows == 1) {
				output.Elements.reserve(length);
				evaluate(0, &output.Elements);
			} else {
				output.Nested.resize(rows);
				for (size_t row = 0; row < rows; ++row) {
					output.Nested[row].reserve(length);
					evaluate(row, &output.Nested[row]);
				}
			}
			context.SetValue("result", std::move(output));
			return true;
		}

		constexpr ExecutorEntry VECTOR_EXECUTORS[] = {
			{"pc.vector2", Vector2Node}, {"pc.vector4", Vector4Node}, {"pc.vector_math", VectorMath}
		};
	}
	std::span<const ExecutorEntry> VectorExecutors() {
		return VECTOR_EXECUTORS;
	}
}
