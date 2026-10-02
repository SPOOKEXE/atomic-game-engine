// Source-backed matrix constructors and operations that stay inside the bounded CPU evaluator.

#include "Families.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::imagegraph::detail {
	namespace {
		const MatrixValue *MatrixOperationInput(const NodeContext &context, std::string_view id) {
			return std::get_if<MatrixValue>(context.Find(id));
		}

		bool ValidMatrix(NodeContext &context, const MatrixValue &matrix, std::string_view port) {
			if (matrix.Columns > Limits::MaximumArrayElements || matrix.Rows > Limits::MaximumArrayElements)
				return context.Fail(
					Status::LimitExceeded, "matrix dimensions exceed the element limit", port
				);
			const uint64_t count = uint64_t(matrix.Columns) * matrix.Rows;
			if (count != matrix.Values.size() || count > Limits::MaximumArrayElements)
				return context.Fail(
					Status::InvalidValue, "matrix dimensions do not match its bounded values", port
				);
			if (std::any_of(matrix.Values.begin(), matrix.Values.end(), [](double value) {
					return !std::isfinite(value);
				}))
				return context.Fail(Status::InvalidValue, "matrix values must be finite", port);
			return true;
		}

		bool PublishMatrix(NodeContext &context, std::string_view port, MatrixValue matrix) {
			if (!ValidMatrix(context, matrix, port)) return false;
			context.SetValue(port, std::move(matrix));
			return true;
		}

		bool ReserveMatrixOutput(NodeContext &context, size_t elementCount, std::string_view port) {
			const uint64_t nameBytes = std::max<uint64_t>(port.size(), std::string{}.capacity());
			if (elementCount > Limits::MaximumArrayElements || nameBytes > Limits::MaximumArrayBytes ||
				uint64_t(elementCount) * sizeof(double) > Limits::MaximumArrayBytes - nameBytes)
				return context.Fail(Status::LimitExceeded, "matrix exceeds the evaluation byte budget", port);
			return context.ReserveOutput(uint64_t(elementCount) * sizeof(double) + nameBytes, port);
		}

		bool AccumulateWorkspace(
			NodeContext &context,
			AllocationReservation &charge,
			uint64_t bytes,
			std::string_view port
		) {
			auto addition = context.ReserveWorkspace(bytes, port);
			if (!addition) return false;
			if (!charge.Merge(std::move(*addition)))
				return context.Fail(Status::InvalidValue, "matrix workspace reservation is incompatible", port);
			return true;
		}

		bool ReadChoiceLabel(NodeContext &context, std::string_view id, std::string_view &label) {
			const double selected = context.SourceChoice(id);
			if (context.FailureCode != Status::Ok) return false;
			if (std::trunc(selected) != selected || selected < 0 ||
				selected > static_cast<double>(std::numeric_limits<int32_t>::max()))
				return context.Fail(
					Status::UnsupportedExecution, "source choice indexing is unresolved for this value", id
				);

			const CatalogueInput *input = FindCatalogueInput(context.Entry, id);
			if (!input || !input->SourceChoices ||
				input->SourceChoices->Status != SourceChoicesStatus::Resolved)
				return context.Fail(Status::UnsupportedExecution, "matrix choice labels are unresolved", id);
			for (const CatalogueSourceChoice &choice : input->SourceChoices->Entries) {
				if (choice.SourceIndex != static_cast<int32_t>(selected)) continue;
				if (choice.Separator)
					return context.Fail(
						Status::UnsupportedExecution, "matrix choice selects a separator", id
					);
				label = choice.Label;
				return true;
			}
			return context.Fail(Status::UnsupportedExecution, "matrix choice label is unavailable", id);
		}

		bool IntegerCoordinate(NodeContext &context, double value, std::string_view port, int64_t &result) {
			if (!std::isfinite(value) || std::trunc(value) != value || value < -0x1p63 || value >= 0x1p63)
				return context.Fail(Status::InvalidValue, "matrix coordinates must be finite integers", port);
			result = static_cast<int64_t>(value);
			return true;
		}

		bool IntegerValue(NodeContext &context, const Value &value, std::string_view port, int64_t &result) {
			if (const auto *integer = std::get_if<int64_t>(&value)) {
				result = *integer;
				return true;
			}
			if (const auto *scalar = std::get_if<double>(&value))
				return IntegerCoordinate(context, *scalar, port, result);
			return context.Fail(Status::InvalidValue, "matrix index must be an integer", port);
		}

		struct CoordinateList {
			AllocationReservation Charge;
			std::vector<std::pair<int64_t, int64_t>> Items;
			bool IsArray = false;
		};

		bool ReadCoordinates(NodeContext &context, std::string_view id, CoordinateList &out) {
			const Value *value = context.Find(id);
			if (!value) return context.Fail(Status::InvalidValue, "matrix position is missing", id);
			const auto reserve = [&](size_t count) {
				auto charge = context.ReserveWorkspace(count * sizeof(std::pair<int64_t, int64_t>), id);
				if (!charge) return false;
				out.Charge = std::move(*charge);
				out.Items.reserve(count);
				return true;
			};
			const auto appendVector = [&](const Vector2 &vector) {
				int64_t x = 0, y = 0;
				if (!IntegerCoordinate(context, vector.X, id, x) ||
					!IntegerCoordinate(context, vector.Y, id, y))
					return false;
				out.Items.emplace_back(x, y);
				return true;
			};
			if (const auto *vector = std::get_if<Vector2>(value))
				return reserve(1) && appendVector(*vector);
			const auto *array = std::get_if<ArrayValue>(value);
			if (!array)
				return context.Fail(
					Status::InvalidValue, "matrix position must be a vector or vector array", id
				);
			if (array->Elements.size() > Limits::MaximumArrayElements ||
				array->Nested.size() > Limits::MaximumArrayElements)
				return context.Fail(
					Status::LimitExceeded, "matrix position array exceeds the element limit", id
				);
			out.IsArray = true;
			if (array->ElementType == ValueType::Vector2 && array->Nested.empty()) {
				if (!reserve(array->Elements.size())) return false;
				for (const ElementValue &element : array->Elements) {
					const auto *vector = std::get_if<Vector2>(&element);
					if (!vector || !appendVector(*vector))
						return context.FailureCode == Status::Ok
								   ? context.Fail(
										 Status::InvalidValue,
										 "matrix position array must contain vectors",
										 id
									 )
								   : false;
				}
				return true;
			}
			const auto number = [&](const ElementValue &element, int64_t &result) {
				if (const auto *integer = std::get_if<int64_t>(&element)) {
					result = *integer;
					return true;
				}
				if (const auto *scalar = std::get_if<double>(&element))
					return IntegerCoordinate(context, *scalar, id, result);
				return context.Fail(Status::InvalidValue, "matrix position components must be integers", id);
			};
			if (!array->Nested.empty()) {
				for (const auto &row : array->Nested)
					if (row.size() != 2)
						return context.Fail(
							Status::InvalidValue, "each matrix position needs two components", id
						);
				if (!reserve(array->Nested.size())) return false;
				for (const auto &row : array->Nested) {
					int64_t x = 0, y = 0;
					if (!number(row[0], x) || !number(row[1], y)) return false;
					out.Items.emplace_back(x, y);
				}
				return true;
			}
			if (array->Elements.size() == 2) {
				if (!reserve(1)) return false;
				int64_t x = 0, y = 0;
				if (!number(array->Elements[0], x) || !number(array->Elements[1], y)) return false;
				out.Items.emplace_back(x, y);
				out.IsArray = false;
				return true;
			}
			return context.Fail(Status::InvalidValue, "matrix position array must contain pairs", id);
		}

		bool ReadIntegerList(
			NodeContext &context,
			std::string_view id,
			std::vector<int64_t> &out,
			AllocationReservation &charge,
			bool &isArray
		) {
			const Value *value = context.Find(id);
			if (!value) return context.Fail(Status::InvalidValue, "matrix index is missing", id);
			if (const auto *array = std::get_if<ArrayValue>(value)) {
				if (!array->Nested.empty())
					return context.Fail(
						Status::UnsupportedExecution, "nested matrix index arrays are unsupported", id
					);
				if (array->Elements.size() > Limits::MaximumArrayElements)
					return context.Fail(
						Status::LimitExceeded, "matrix index array exceeds the element limit", id
					);
				if (!AccumulateWorkspace(context, charge, array->Elements.size() * sizeof(int64_t), id))
					return false;
				out.reserve(array->Elements.size());
				isArray = true;
				for (const ElementValue &element : array->Elements) {
					int64_t index = 0;
					if (const auto *integer = std::get_if<int64_t>(&element))
						index = *integer;
					else if (const auto *scalar = std::get_if<double>(&element)) {
						if (!IntegerCoordinate(context, *scalar, id, index)) return false;
					} else
						return context.Fail(Status::InvalidValue, "matrix indices must be integers", id);
					out.push_back(index);
				}
				return true;
			}
			int64_t index = 0;
			if (!IntegerValue(context, *value, id, index)) return false;
			if (!AccumulateWorkspace(context, charge, sizeof(int64_t), id)) return false;
			out.reserve(1);
			out.push_back(index);
			return true;
		}

		bool ReadNumberList(
			NodeContext &context,
			std::string_view id,
			std::vector<double> &out,
			AllocationReservation &charge
		) {
			const Value *value = context.Find(id);
			if (!value) return context.Fail(Status::InvalidValue, "matrix values are missing", id);
			if (const auto *scalar = std::get_if<double>(value)) {
				if (!AccumulateWorkspace(context, charge, sizeof(double), id)) return false;
				out.reserve(1);
				out.push_back(*scalar);
				return true;
			}
			if (const auto *integer = std::get_if<int64_t>(value)) {
				if (!AccumulateWorkspace(context, charge, sizeof(double), id)) return false;
				out.reserve(1);
				out.push_back(static_cast<double>(*integer));
				return true;
			}
			const auto *array = std::get_if<ArrayValue>(value);
			if (!array || !array->Nested.empty())
				return context.Fail(Status::InvalidValue, "matrix values must be a flat numeric array", id);
			if (array->Elements.size() > Limits::MaximumArrayElements)
				return context.Fail(
					Status::LimitExceeded, "matrix value array exceeds the element limit", id
				);
			for (const ElementValue &element : array->Elements)
				if (!std::holds_alternative<double>(element) && !std::holds_alternative<int64_t>(element))
					return context.Fail(Status::InvalidValue, "matrix values must be numeric", id);
			if (!AccumulateWorkspace(context, charge, array->Elements.size() * sizeof(double), id))
				return false;
			out.reserve(array->Elements.size());
			for (const ElementValue &element : array->Elements) {
				if (const auto *scalar = std::get_if<double>(&element))
					out.push_back(*scalar);
				else if (const auto *integer = std::get_if<int64_t>(&element))
					out.push_back(static_cast<double>(*integer));
				else
					return context.Fail(Status::InvalidValue, "matrix values must be numeric", id);
			}
			return true;
		}

		bool ReadNumberRows(
			NodeContext &context,
			std::string_view id,
			std::vector<std::vector<double>> &out,
			AllocationReservation &charge
		) {
			const Value *value = context.Find(id);
			const auto *array = value ? std::get_if<ArrayValue>(value) : nullptr;
			if (!array || array->Nested.empty())
				return context.Fail(
					Status::InvalidValue, "multiple matrix positions need one vector per position", id
				);
			if (array->Nested.size() > Limits::MaximumArrayElements)
				return context.Fail(
					Status::LimitExceeded, "matrix vector array exceeds the element limit", id
				);
			size_t totalElements = 0;
			for (const auto &row : array->Nested) {
				if (row.size() > Limits::MaximumArrayElements - totalElements)
					return context.Fail(
						Status::LimitExceeded, "matrix vector input exceeds the element limit", id
					);
				totalElements += row.size();
			}
			for (const auto &row : array->Nested)
				for (const ElementValue &element : row)
					if (!std::holds_alternative<double>(element) && !std::holds_alternative<int64_t>(element))
						return context.Fail(Status::InvalidValue, "matrix vector rows must be numeric", id);
			const uint64_t bytes = array->Nested.size() * sizeof(std::vector<double>) +
							   totalElements * sizeof(double);
			if (!AccumulateWorkspace(context, charge, bytes, id)) return false;
			out.reserve(array->Nested.size());
			for (const auto &row : array->Nested) {
				std::vector<double> numbers;
				numbers.reserve(row.size());
				for (const ElementValue &element : row) {
					if (const auto *scalar = std::get_if<double>(&element))
						numbers.push_back(*scalar);
					else if (const auto *integer = std::get_if<int64_t>(&element))
						numbers.push_back(static_cast<double>(*integer));
					else
						return context.Fail(Status::InvalidValue, "matrix vector rows must be numeric", id);
				}
				out.push_back(std::move(numbers));
			}
			return true;
		}

		bool ChargeArrayOutput(NodeContext &context, size_t elementCount, size_t rowCount, std::string_view port) {
			if (elementCount > Limits::MaximumArrayElements || rowCount > Limits::MaximumArrayElements)
				return context.Fail(Status::LimitExceeded, "matrix output exceeds the element limit", port);
			const uint64_t bytes = uint64_t(elementCount) * sizeof(ElementValue) +
								   uint64_t(rowCount) * sizeof(std::vector<ElementValue>);
			const uint64_t nameBytes = std::max<uint64_t>(port.size(), std::string{}.capacity());
			if (nameBytes > Limits::MaximumArrayBytes || bytes > Limits::MaximumArrayBytes - nameBytes)
				return context.Fail(
					Status::LimitExceeded, "matrix output exceeds the array byte budget", port
				);
			return context.ReserveOutput(bytes + nameBytes, port);
		}

		bool MatrixTransform2D(NodeContext &context) {
			const bool affine = context.Boolean("affine", true);
			const Vector2 position = context.Vec2("position");
			const double rotation = context.Scalar("rotation");
			const Vector2 scale = context.Vec2("scale", Vector2{1, 1});
			if (!std::isfinite(position.X) || !std::isfinite(position.Y) || !std::isfinite(rotation) ||
				!std::isfinite(scale.X) || !std::isfinite(scale.Y))
				return context.Fail(Status::InvalidValue, "transform inputs must be finite");
			const double cosine = std::cos(rotation), sine = std::sin(rotation);
			if (!ReserveMatrixOutput(context, 9, "matrix")) return false;
			MatrixValue matrix{3, 3, {}};
			matrix.Values.reserve(9);
			matrix.Values = {
				scale.X * cosine,
				-scale.Y * sine,
				0,
				scale.X * sine,
				scale.Y * cosine,
				0,
				affine ? position.X : 0.0,
				affine ? position.Y : 0.0,
				affine ? 1.0 : 0.0
			};
			return PublishMatrix(context, "matrix", std::move(matrix));
		}

		bool MatrixMath(NodeContext &context) {
			const MatrixValue *first = MatrixOperationInput(context, "matrix_1");
			const MatrixValue *second = MatrixOperationInput(context, "matrix_2");
			if (!first || !second)
				return context.Fail(Status::InvalidValue, "matrix input is missing", "matrix_1");
			if (!ValidMatrix(context, *first, "matrix_1") || !ValidMatrix(context, *second, "matrix_2"))
				return false;
			std::string_view operation;
			if (!ReadChoiceLabel(context, "operation", operation)) return false;
			if (operation == "Add" || operation == "Subtract") {
				if (!ReserveMatrixOutput(context, first->Values.size(), "matrix")) return false;
				MatrixValue result = *first;
				const double sign = operation == "Add" ? 1.0 : -1.0;
				for (size_t index = 0; index < result.Values.size(); index++) {
					const double other = index < second->Values.size() ? second->Values[index] : 0.0;
					result.Values[index] += sign * other;
				}
				return PublishMatrix(context, "matrix", std::move(result));
			}
			if (operation == "Multiply Scalar" || operation == "Divide Scalar") {
				const double scalar = context.Scalar("scala");
				if (!std::isfinite(scalar))
					return context.Fail(Status::InvalidValue, "matrix scalar must be finite", "scala");
				if (operation == "Divide Scalar" && scalar == 0.0)
					return context.Fail(
						Status::InvalidValue, "matrix scalar division by zero has no finite result", "scala"
					);
				if (!ReserveMatrixOutput(context, first->Values.size(), "matrix")) return false;
				MatrixValue result = *first;
				for (double &value : result.Values)
					value = operation == "Multiply Scalar" ? value * scalar : value / scalar;
				return PublishMatrix(context, "matrix", std::move(result));
			}
			if (operation == "Multiply Matrix") {
				// node_matrix_math calls Matrix.multiplyMatrix; preserve its width/height ordering.
				const uint32_t rowsA = first->Columns, colsA = first->Rows;
				const uint32_t rowsB = second->Columns, colsB = second->Rows;
				if (colsA != rowsB) {
					if (!ReserveMatrixOutput(context, first->Values.size(), "matrix")) return false;
					return PublishMatrix(context, "matrix", *first);
				}
				if (uint64_t(rowsA) * colsB > Limits::MaximumArrayElements)
					return context.Fail(
						Status::LimitExceeded, "matrix product exceeds the element limit", "matrix"
					);
				const size_t count = size_t(rowsA) * colsB;
				if (!ReserveMatrixOutput(context, count, "matrix")) return false;
				MatrixValue result{rowsA, colsB, {}};
				result.Values.reserve(count);
				result.Values.resize(count, 0.0);
				for (uint32_t row = 0; row < rowsA; row++)
					for (uint32_t column = 0; column < colsB; column++)
						for (uint32_t inner = 0; inner < colsA; inner++)
							result.Values[size_t(row) * colsB + column] +=
								first->Values[size_t(row) * colsA + inner] *
								second->Values[size_t(inner) * colsB + column];
				return PublishMatrix(context, "matrix", std::move(result));
			}
			return context.Fail(
				Status::UnsupportedExecution, "matrix operation label is unsupported", "operation"
			);
		}

		bool MatrixCrop(NodeContext &context) {
			const MatrixValue *input = MatrixOperationInput(context, "matrix");
			if (!input) return context.Fail(Status::InvalidValue, "matrix input is missing", "matrix");
			if (!ValidMatrix(context, *input, "matrix")) return false;
			const Vector2 size = context.Vec2("size", Vector2{3, 3});
			const Vector2 offset = context.Vec2("offset");
			int64_t width = 0, height = 0, offsetX = 0, offsetY = 0;
			if (!IntegerCoordinate(context, size.X, "size", width) ||
				!IntegerCoordinate(context, size.Y, "size", height) ||
				!IntegerCoordinate(context, offset.X, "offset", offsetX) ||
				!IntegerCoordinate(context, offset.Y, "offset", offsetY))
				return false;
			if (width < 0 || height < 0)
				return context.Fail(Status::InvalidValue, "matrix crop size cannot be negative", "size");
			if (width > int64_t(Limits::MaximumArrayElements) ||
				height > int64_t(Limits::MaximumArrayElements) ||
				uint64_t(width) * uint64_t(height) > Limits::MaximumArrayElements)
				return context.Fail(Status::LimitExceeded, "matrix crop exceeds the element limit", "size");
			std::string_view overflow;
			if (!ReadChoiceLabel(context, "overflow", overflow)) return false;
			if ((input->Columns == 0 || input->Rows == 0) && width != 0 && height != 0) {
				if (overflow != "Zero")
					return context.Fail(
						Status::InvalidValue, "repeat and clamp need a nonempty source matrix", "matrix"
					);
			}
			const size_t elementCount = size_t(width) * size_t(height);
			if (!ReserveMatrixOutput(context, elementCount, "matrix")) return false;
			MatrixValue result{static_cast<uint32_t>(width), static_cast<uint32_t>(height), {}};
			result.Values.reserve(elementCount);
			result.Values.resize(elementCount, 0.0);
			for (int64_t y = 0; y < height; y++) {
				for (int64_t x = 0; x < width; x++) {
					if (offsetX > std::numeric_limits<int64_t>::max() - x ||
						offsetY > std::numeric_limits<int64_t>::max() - y)
						return context.Fail(
							Status::InvalidValue, "matrix crop coordinate overflows", "offset"
						);
					int64_t sourceX = offsetX + x, sourceY = offsetY + y;
					const bool outside =
						sourceX < 0 || sourceY < 0 || sourceX >= input->Columns || sourceY >= input->Rows;
					if (outside && overflow == "Zero") continue;
					if (outside && overflow == "Repeat") {
						sourceX = ((sourceX % input->Columns) + input->Columns) % input->Columns;
						sourceY = ((sourceY % input->Rows) + input->Rows) % input->Rows;
					} else if (outside && overflow == "Clamp") {
						sourceX = std::clamp<int64_t>(sourceX, 0, int64_t(input->Columns) - 1);
						sourceY = std::clamp<int64_t>(sourceY, 0, int64_t(input->Rows) - 1);
					}
					result.Values[size_t(y) * result.Columns + size_t(x)] =
						input->Values[size_t(sourceY) * input->Columns + size_t(sourceX)];
				}
			}
			return PublishMatrix(context, "matrix", std::move(result));
		}

		bool MatrixGet(NodeContext &context) {
			const MatrixValue *matrix = MatrixOperationInput(context, "matrix");
			if (!matrix) return context.Fail(Status::InvalidValue, "matrix input is missing", "matrix");
			if (!ValidMatrix(context, *matrix, "matrix")) return false;
			CoordinateList positions;
			if (!ReadCoordinates(context, "position", positions)) return false;
			if (positions.Items.empty())
				return context.Fail(Status::InvalidValue, "matrix position array is empty", "position");
			if (!positions.IsArray) {
				const auto [x, y] = positions.Items.front();
				if (x < 0 || y < 0 || x >= matrix->Columns || y >= matrix->Rows)
					return context.Fail(
						Status::UnsupportedExecution,
						"source leaves out-of-bounds matrix reads uninitialized",
						"position"
					);
				if (!context.ReserveOutput(std::string{}.capacity(), "output")) return false;
				context.SetValue("output", matrix->Values[size_t(y) * matrix->Columns + size_t(x)]);
				return true;
			}
			if (!ChargeArrayOutput(context, positions.Items.size(), 0, "output")) return false;
			ArrayValue values{ValueType::Scalar, {}};
			values.Elements.reserve(positions.Items.size());
			for (const auto &[x, y] : positions.Items) {
				if (x < 0 || y < 0 || x >= matrix->Columns || y >= matrix->Rows)
					return context.Fail(
						Status::UnsupportedExecution,
						"source leaves out-of-bounds matrix reads uninitialized",
						"position"
					);
				values.Elements.emplace_back(matrix->Values[size_t(y) * matrix->Columns + size_t(x)]);
			}
			context.SetValue("output", std::move(values));
			return true;
		}

		bool MatrixSet(NodeContext &context) {
			const MatrixValue *matrix = MatrixOperationInput(context, "matrix");
			if (!matrix) return context.Fail(Status::InvalidValue, "matrix input is missing", "matrix");
			if (!ValidMatrix(context, *matrix, "matrix")) return false;
			CoordinateList positions;
			if (!ReadCoordinates(context, "position", positions)) return false;
			AllocationReservation valuesCharge;
			std::vector<double> values;
			if (!ReadNumberList(context, "value", values, valuesCharge)) return false;
			if (!positions.IsArray && values.size() != 1)
				return context.Fail(Status::InvalidValue, "single matrix position needs one value", "value");
			if (!ReserveMatrixOutput(context, matrix->Values.size(), "matrix")) return false;
			MatrixValue result = *matrix;
			const size_t count = positions.IsArray ? std::min(positions.Items.size(), values.size()) : 1;
			for (size_t index = 0; index < count; index++) {
				const auto [x, y] = positions.Items[index];
				if (x < 0 || y < 0 || x >= matrix->Columns || y >= matrix->Rows) continue;
				if (!std::isfinite(values[index]))
					return context.Fail(Status::InvalidValue, "matrix values must be finite", "value");
				result.Values[size_t(y) * matrix->Columns + size_t(x)] = values[index];
			}
			return PublishMatrix(context, "matrix", std::move(result));
		}

		bool MatrixGetVector(NodeContext &context) {
			const MatrixValue *matrix = MatrixOperationInput(context, "matrix");
			if (!matrix) return context.Fail(Status::InvalidValue, "matrix input is missing", "matrix");
			if (!ValidMatrix(context, *matrix, "matrix")) return false;
			std::string_view direction;
			if (!ReadChoiceLabel(context, "direction", direction)) return false;
			AllocationReservation positionsCharge;
			std::vector<int64_t> positions;
			bool isArray = false;
			if (!ReadIntegerList(context, "position", positions, positionsCharge, isArray)) return false;
			if (positions.empty())
				return context.Fail(Status::InvalidValue, "matrix position array is empty", "position");
			const size_t vectorCount =
				(direction == "Row")
					? matrix->Columns
					: (direction == "Column" ? matrix->Rows : std::min(matrix->Columns, matrix->Rows));
			if (direction != "Row" && direction != "Column" && direction != "Diagonal" &&
				direction != "Inv. Diagonal")
				return context.Fail(
					Status::UnsupportedExecution, "matrix direction label is unsupported", "direction"
				);
			for (const int64_t position : positions)
				if ((direction == "Row" && (position < 0 || position >= matrix->Rows)) ||
					(direction == "Column" && (position < 0 || position >= matrix->Columns)))
					return context.Fail(
						Status::UnsupportedExecution,
						"source matrix vector index is out of bounds",
						"position"
					);
			if (vectorCount > Limits::MaximumArrayElements / positions.size())
				return context.Fail(
					Status::LimitExceeded, "matrix vector output exceeds the element limit", "matrix"
				);
			const size_t elementCount = vectorCount * positions.size();
			const size_t rowCount = isArray ? positions.size() : 0;
			if (!ChargeArrayOutput(context, elementCount, rowCount, "matrix")) return false;
			ArrayValue result{ValueType::Scalar, {}};
			if (!isArray)
				result.Elements.reserve(vectorCount);
			else
				result.Nested.reserve(rowCount);
			for (const int64_t position : positions) {
				std::vector<ElementValue> row;
				if (isArray) row.reserve(vectorCount);
				for (size_t index = 0; index < vectorCount; index++) {
					size_t flat = 0;
					if (direction == "Row")
						flat = size_t(position) * matrix->Columns + index;
					else if (direction == "Column")
						flat = index * matrix->Columns + size_t(position);
					else if (direction == "Diagonal")
						flat = index * matrix->Columns + index;
					else if (direction == "Inv. Diagonal")
						flat = index * matrix->Columns + (matrix->Columns - index - 1);
					const double value = matrix->Values[flat];
					if (isArray)
						row.emplace_back(value);
					else
						result.Elements.emplace_back(value);
				}
				if (isArray) result.Nested.push_back(std::move(row));
			}
			context.SetValue("matrix", std::move(result));
			return true;
		}

		bool MatrixSetVector(NodeContext &context) {
			const MatrixValue *matrix = MatrixOperationInput(context, "matrix");
			if (!matrix) return context.Fail(Status::InvalidValue, "matrix input is missing", "matrix");
			if (!ValidMatrix(context, *matrix, "matrix")) return false;
			std::string_view direction;
			if (!ReadChoiceLabel(context, "direction", direction)) return false;
			AllocationReservation positionsCharge;
			std::vector<int64_t> positions;
			bool isArray = false;
			if (!ReadIntegerList(context, "position", positions, positionsCharge, isArray)) return false;
			if (positions.empty())
				return context.Fail(Status::InvalidValue, "matrix position array is empty", "position");
			AllocationReservation vectorsCharge;
			std::vector<std::vector<double>> vectors;
			if (isArray) {
				if (!ReadNumberRows(context, "vector", vectors, vectorsCharge)) return false;
			} else {
				std::vector<double> vector;
				if (!ReadNumberList(context, "vector", vector, vectorsCharge)) return false;
				if (!AccumulateWorkspace(context, vectorsCharge, sizeof(std::vector<double>), "vector"))
					return false;
				vectors.reserve(1);
				vectors.push_back(std::move(vector));
			}
			if (!ReserveMatrixOutput(context, matrix->Values.size(), "matrix")) return false;
			MatrixValue result = *matrix;
			const size_t count = isArray ? std::min(positions.size(), vectors.size()) : 1;
			for (size_t rowIndex = 0; rowIndex < count; rowIndex++) {
				const int64_t position = positions[rowIndex];
				const auto &vector = vectors[rowIndex];
				const size_t length = vector.size();
				const size_t limit = direction == "Row" ? std::min<size_t>(matrix->Columns, length)
									 : direction == "Column"
										 ? std::min<size_t>(matrix->Rows, length)
										 : std::min({size_t(matrix->Columns), size_t(matrix->Rows), length});
				if ((direction == "Row" && (position < 0 || position >= matrix->Rows)) ||
					(direction == "Column" && (position < 0 || position >= matrix->Columns)))
					return context.Fail(
						Status::UnsupportedExecution,
						"source matrix vector index is out of bounds",
						"position"
					);
				for (size_t index = 0; index < limit; index++) {
					size_t flat = 0;
					if (direction == "Row")
						flat = size_t(position) * matrix->Columns + index;
					else if (direction == "Column")
						flat = index * matrix->Columns + size_t(position);
					else if (direction == "Diagonal")
						flat = index * matrix->Columns + index;
					else if (direction == "Inv. Diagonal")
						flat = index * matrix->Columns + (matrix->Columns - index - 1);
					else
						return context.Fail(
							Status::UnsupportedExecution, "matrix direction label is unsupported", "direction"
						);
					if (!std::isfinite(vector[index]))
						return context.Fail(
							Status::InvalidValue, "matrix vector values must be finite", "vector"
						);
					result.Values[flat] = vector[index];
				}
			}
			return PublishMatrix(context, "matrix", std::move(result));
		}
	}

	std::span<const ExecutorEntry> MatrixExecutors() {
		static constexpr std::array ENTRIES{
			ExecutorEntry{"pc.matrix_transform_2_d", MatrixTransform2D},
			ExecutorEntry{"pc.matrix_math", MatrixMath},
			ExecutorEntry{"pc.matrix_crop", MatrixCrop},
			ExecutorEntry{"pc.matrix_get", MatrixGet},
			ExecutorEntry{"pc.matrix_set", MatrixSet},
			ExecutorEntry{"pc.matrix_get_vector", MatrixGetVector},
			ExecutorEntry{"pc.matrix_set_vector", MatrixSetVector},
		};
		return ENTRIES;
	}
}
