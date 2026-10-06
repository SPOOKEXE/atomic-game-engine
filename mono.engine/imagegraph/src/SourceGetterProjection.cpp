#include "SourceGetterProjection.hpp"

#include "SourceLuaSockets.hpp"
#include "SourceMappedInputs.hpp"
#include "TimelineDrivers.hpp"
#include "ValuePayload.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>
#include <optional>
#include <type_traits>

namespace engine::imagegraph::detail {
	namespace source_getter_detail {
		struct Validator {
			std::string_view Node;
			int32_t Index;
			double Minimum, Maximum;
		};
		// Exact pinned constructor validators. Display slider ranges are not getter validators.
		constexpr Validator VALIDATORS[] = {
			{"Node_Blobify", 2, 0.0, std::numeric_limits<double>::infinity()},
			{"Node_Brush_Linear", 2, 1.0, std::numeric_limits<double>::infinity()},
			{"Node_Convolution", 10, 3.0, 16.0},
			{"Node_Image_Grid", 1, 1.0, std::numeric_limits<double>::infinity()},
			{"Node_Image_Grid_Patreon", 1, 1.0, std::numeric_limits<double>::infinity()},
			{"Node_IsoSurf", 0, 1.0, std::numeric_limits<double>::infinity()},
			{"Node_JPEG", 2, 1.0, std::numeric_limits<double>::infinity()},
			{"Node_JPEG", 4, 0.0, std::numeric_limits<double>::infinity()},
			{"Node_Kuwahara", 2, 1.0, std::numeric_limits<double>::infinity()},
			{"Node_MK_Pile", 6, 1.0, std::numeric_limits<double>::infinity()},
			{"Node_MK_Pile", 11, 0.0, std::numeric_limits<double>::infinity()},
			{"Node_Path_Fill", 2, 2.0, std::numeric_limits<double>::infinity()},
			{"Node_Path_Map", 3, 2.0, std::numeric_limits<double>::infinity()},
			{"Node_Path_Morph", 3, 2.0, std::numeric_limits<double>::infinity()},
			{"Node_Path_Revolve", 7, 2.0, std::numeric_limits<double>::infinity()},
			{"Node_Path_Revolve", 3, 2.0, std::numeric_limits<double>::infinity()},
			{"Node_Path_SDF", 2, 2.0, std::numeric_limits<double>::infinity()},
			{"Node_Scatter", 2, 0.0, std::numeric_limits<double>::infinity()},
			{"Node_3D_Mesh_Cone", 4, 3.0, std::numeric_limits<double>::infinity()},
			{"Node_3D_Mesh_Cylinder", 4, 3.0, std::numeric_limits<double>::infinity()},
			{"Node_3D_Mesh_Cylinder", 10, 1.0, std::numeric_limits<double>::infinity()},
			{"Node_3D_Mesh_Path_Extrude", 10, 2.0, std::numeric_limits<double>::infinity()},
			{"Node_3D_Mesh_Path_Extrude", 5, 2.0, std::numeric_limits<double>::infinity()},
			{"Node_3D_Mesh_Path_Revolve", 8, 2.0, std::numeric_limits<double>::infinity()},
			{"Node_3D_Mesh_Path_Revolve", 9, 3.0, std::numeric_limits<double>::infinity()},
			{"Node_3D_Mesh_Sphere_Ico", 4, 0.0, std::numeric_limits<double>::infinity()},
			{"Node_3D_Mesh_Sphere_UV", 4, 2.0, std::numeric_limits<double>::infinity()},
			{"Node_3D_Mesh_Sphere_UV", 5, 3.0, std::numeric_limits<double>::infinity()},
			{"Node_3D_Mesh_Torus", 4, 3.0, std::numeric_limits<double>::infinity()},
			{"Node_3D_Mesh_Torus", 5, 3.0, std::numeric_limits<double>::infinity()},
		};
		Validator LimitsFor(const CatalogueEntry &entry, const CatalogueInput &input) {
			for (const auto &validator : VALIDATORS)
				if (validator.Node == entry.SourceNode && validator.Index == input.SourceIndex)
					return validator;
			return {
				{}, -1, -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()
			};
		}
		template <class T>
		std::optional<ElementValue> Leaf(const T &value, bool integer, Validator validator) {
			long double raw;
			if constexpr (std::is_same_v<T, double> || std::is_same_v<T, int64_t> || std::is_same_v<T, bool>)
				raw = value;
			else if constexpr (std::is_same_v<T, EnumValue>)
				raw = value.Value;
			else if constexpr (std::is_same_v<T, Colour>)
				raw = uint32_t(value.Red) | uint32_t(value.Green) << 8 | uint32_t(value.Blue) << 16 |
					  uint32_t(value.Alpha) << 24;
			else
				return std::nullopt;
			if (!std::isfinite(raw)) return std::nullopt;
			if (!integer) return ElementValue{raw > .5L};
			raw = std::clamp(
				raw, static_cast<long double>(validator.Minimum), static_cast<long double>(validator.Maximum)
			);
			// Already integral native values retain their exact representation without a double roundtrip.
			if constexpr (std::is_same_v<T, int64_t>) {
				if (raw == value) return ElementValue{value};
			}
			const double rounded = DriverRoundHalfEven(static_cast<double>(raw));
			if (rounded < -0x1p63 || rounded >= 0x1p63) return std::nullopt;
			return ElementValue{static_cast<int64_t>(rounded)};
		}
		bool DeepIntegerArray(const ArrayValue &array) {
			return !array.Nested.empty() || array.ElementType == ValueType::Vector2 ||
				   array.ElementType == ValueType::Vector3 || array.ElementType == ValueType::Vector4 ||
				   array.ElementType == ValueType::Quaternion || array.ElementType == ValueType::Area;
		}
		struct Tuple {
			std::array<double, 6> Fixed{};
			const std::vector<double> *Dynamic = nullptr;
			size_t Count = 0;
			double At(size_t index) const {
				return Dynamic ? (*Dynamic)[index] : Fixed[index];
			}
		};
		template <class T> std::optional<Tuple> Components(const T &item, bool integer) {
			if constexpr (std::is_same_v<T, Vector2>)
				return Tuple{{item.X, item.Y}, nullptr, 2};
			else if constexpr (std::is_same_v<T, Vector3>)
				return Tuple{{item.X, item.Y, item.Z}, nullptr, 3};
			else if constexpr (std::is_same_v<T, Vector4>)
				return Tuple{{item.X, item.Y, item.Z, item.W}, nullptr, 4};
			else if constexpr (std::is_same_v<T, Area>)
				return Tuple{
					{item.CenterX,
					 item.CenterY,
					 item.HalfWidth,
					 item.HalfHeight,
					 double(item.Shape),
					 double(item.Mode)},
					nullptr,
					6
				};
			else if constexpr (std::is_same_v<T, MatrixValue>) {
				// __matrix.to_real exposes its flat raw array to the Int getter. Bool has no such hook.
				if (integer) return Tuple{{}, &item.Values, item.Values.size()};
			}
			return std::nullopt;
		}
		std::optional<Tuple> Components(const Value &value, bool integer) {
			return std::visit([&](const auto &item) { return Components(item, integer); }, value);
		}
		std::optional<Tuple> Components(const ElementValue &value, bool integer) {
			return std::visit([&](const auto &item) { return Components(item, integer); }, value);
		}
		bool PackedArray(const ArrayValue &array) {
			return array.ElementType == ValueType::Vector2 || array.ElementType == ValueType::Vector3 ||
				   array.ElementType == ValueType::Vector4 || array.ElementType == ValueType::Area;
		}
		std::optional<uint64_t> ProjectionBytes(const Value &value, bool integer) {
			if (const auto tuple = Components(value, integer)) return tuple->Count * sizeof(ElementValue);
			if (const auto *array = std::get_if<ArrayValue>(&value)) {
				if (!integer && PackedArray(*array)) {
					if (!array->Nested.empty()) return std::nullopt;
					uint64_t leaves = 0;
					for (const auto &element : array->Elements) {
						const auto tuple = Components(element, false);
						if (!tuple) return std::nullopt;
						leaves += tuple->Count;
					}
					if (leaves > Limits::MaximumArrayElements) return std::nullopt;
					return leaves * sizeof(ElementValue) +
						   array->Elements.size() * sizeof(std::vector<ElementValue>);
				}
				return PayloadOwnedBytes(*array);
			}
			return 0;
		}

		std::optional<bool> Changed(const Value &value, bool integer, Validator validator) {
			if (!ValidRuntimeValue(value)) return std::nullopt;
			if (const auto tuple = Components(value, integer)) {
				for (size_t index = 0; index < tuple->Count; ++index)
					if (!Leaf(tuple->At(index), integer, validator)) return std::nullopt;
				return true;
			}
			if (const auto *array = std::get_if<ArrayValue>(&value)) {
				// __NodeValue_Int bypasses valueProcess completely for source depth >= 2.
				if (integer && DeepIntegerArray(*array)) return false;
				if (!integer && PackedArray(*array)) {
					if (!ProjectionBytes(value, integer)) return std::nullopt;
					for (const auto &element : array->Elements) {
						const auto tuple = Components(element, false);
						if (!tuple) return std::nullopt;
						for (size_t index = 0; index < tuple->Count; ++index)
							if (!Leaf(tuple->At(index), false, validator)) return std::nullopt;
					}
					return true;
				}
				bool changed = array->ElementType != (integer ? ValueType::Integer : ValueType::Boolean);
				const auto check = [&](const ElementValue &element) {
					auto normalized =
						std::visit([&](const auto &leaf) { return Leaf(leaf, integer, validator); }, element);
					if (!normalized) return false;
					changed |= *normalized != element;
					return true;
				};
				for (const auto &element : array->Elements)
					if (!check(element)) return std::nullopt;
				for (const auto &row : array->Nested)
					for (const auto &element : row)
						if (!check(element)) return std::nullopt;
				return changed;
			}
			return std::visit(
				[&](const auto &item) -> std::optional<bool> {
					const auto normalized = Leaf(item, integer, validator);
					if (!normalized) return std::nullopt;
					return std::visit(
						[&](const auto &leaf) -> bool {
							using T = std::decay_t<decltype(leaf)>;
							if constexpr (std::is_same_v<T, int64_t> || std::is_same_v<T, bool>)
								return value != Value{leaf};
							else
								return true;
						},
						*normalized
					);
				},
				value
			);
		}
		template <class T> std::optional<double> VectorNumber(const T &value) {
			double number;
			if constexpr (std::is_same_v<T, double> || std::is_same_v<T, int64_t> || std::is_same_v<T, bool>)
				number = static_cast<double>(value);
			else if constexpr (std::is_same_v<T, EnumValue>)
				number = static_cast<double>(value.Value);
			else
				return std::nullopt;
			if (!std::isfinite(number)) return std::nullopt;
			return DriverRoundHalfEven(number);
		}
		std::optional<Vector2> IntegerPair(std::span<const ElementValue> row, bool resize) {
			if (!resize && row.size() < 2) return std::nullopt;
			if (resize && std::any_of(row.begin(), row.end(), [](const auto &element) {
					return Components(element, false).has_value();
				}))
				return std::nullopt;
			Vector2 pair{};
			for (size_t axis = 0; axis < std::min<size_t>(2, row.size()); ++axis) {
				const auto value = std::visit([](const auto &v) { return VectorNumber(v); }, row[axis]);
				if (!value) return std::nullopt;
				(axis == 0 ? pair.X : pair.Y) = *value;
			}
			return pair;
		}
		std::optional<Vector2> IntegerPair(const ElementValue &element) {
			const auto tuple = Components(element, false);
			if (!tuple || tuple->Count < 2) return std::nullopt;
			const std::array<ElementValue, 2> pair{tuple->At(0), tuple->At(1)};
			return IntegerPair(pair, false);
		}
		std::optional<Vector2> IntegerPair(const Value &value) {
			if (const auto tuple = Components(value, false)) {
				if (tuple->Count != 2) return std::nullopt;
				const std::array<ElementValue, 2> pair{tuple->At(0), tuple->At(1)};
				return IntegerPair(pair, false);
			}
			const auto scalar = std::visit([](const auto &v) { return VectorNumber(v); }, value);
			if (!scalar) return std::nullopt;
			return Vector2{*scalar, *scalar};
		}
		std::optional<Vector2> IntegerItemsPair(std::span<const SourceArrayItem> row, bool resize) {
			if (!resize && row.size() < 2) return std::nullopt;
			if (resize && std::any_of(row.begin(), row.end(), [](const auto &item) {
					const auto *leaf = std::get_if<ElementValue>(&item.Data);
					return !leaf || Components(*leaf, false).has_value();
				}))
				return std::nullopt;
			Vector2 pair{};
			for (size_t axis = 0; axis < std::min<size_t>(2, row.size()); ++axis) {
				const auto *element = std::get_if<ElementValue>(&row[axis].Data);
				if (!element) return std::nullopt;
				const auto number = std::visit([](const auto &v) { return VectorNumber(v); }, *element);
				if (!number) return std::nullopt;
				(axis == 0 ? pair.X : pair.Y) = *number;
			}
			return pair;
		}
		bool IntegerItemsAreRows(const ArrayValue &array) {
			return std::any_of(array.Items.begin(), array.Items.end(), [](const auto &item) {
				const auto *leaf = std::get_if<ElementValue>(&item.Data);
				return !leaf || Components(*leaf, false).has_value();
			});
		}
		std::optional<Vector2> IntegerItemRow(const SourceArrayItem &item) {
			if (const auto *row = std::get_if<std::vector<SourceArrayItem>>(&item.Data))
				return IntegerItemsPair(*row, true);
			if (const auto *leaf = std::get_if<ElementValue>(&item.Data)) {
				if (Components(*leaf, false)) return IntegerPair(*leaf);
				// source array_verify_new replaces a scalar row with two zeros.
				return Vector2{};
			}
			return std::nullopt;
		}
		std::optional<uint64_t> IntegerVectorBytes(const Value &value) {
			if (!ValidRuntimeValue(value)) return std::nullopt;
			const auto *array = std::get_if<ArrayValue>(&value);
			if (!array) return IntegerPair(value) ? std::optional<uint64_t>{0} : std::nullopt;
			if (!array->Items.empty()) {
				if (IntegerItemsAreRows(*array)) {
					for (const auto &item : array->Items)
						if (!IntegerItemRow(item)) return std::nullopt;
					return array->Items.size() * sizeof(ElementValue);
				}
				if (array->Items.size() != 2 || !IntegerItemsPair(array->Items, false)) return std::nullopt;
				return 0;
			}
			if (!array->Nested.empty()) {
				if (!array->Elements.empty()) return std::nullopt;
				for (const auto &row : array->Nested)
					if (!IntegerPair(row, true)) return std::nullopt;
				return array->Nested.size() * sizeof(ElementValue);
			}
			if (array->Elements.empty()) return 0;
			if (PackedArray(*array)) {
				for (const auto &element : array->Elements)
					if (!IntegerPair(element)) return std::nullopt;
				return array->Elements.size() * sizeof(ElementValue);
			}
			if (array->Elements.size() != 2 || !IntegerPair(array->Elements, false)) return std::nullopt;
			return 0;
		}
		Value IntegerVector(const Value &value) {
			const auto *array = std::get_if<ArrayValue>(&value);
			if (!array) return *IntegerPair(value);
			if (!array->Items.empty()) {
				if (!IntegerItemsAreRows(*array)) return *IntegerItemsPair(array->Items, false);
				ArrayValue result{ValueType::Vector2, {}};
				result.Elements.reserve(array->Items.size());
				for (const auto &item : array->Items)
					result.Elements.emplace_back(*IntegerItemRow(item));
				return result;
			}
			if (!array->Nested.empty()) {
				ArrayValue result{ValueType::Vector2, {}};
				result.Elements.reserve(array->Nested.size());
				for (const auto &row : array->Nested)
					result.Elements.emplace_back(*IntegerPair(row, true));
				return result;
			}
			if (array->Elements.empty()) return ArrayValue{ValueType::Scalar, {}};
			if (PackedArray(*array)) {
				ArrayValue result{ValueType::Vector2, {}};
				result.Elements.reserve(array->Elements.size());
				for (const auto &element : array->Elements)
					result.Elements.emplace_back(*IntegerPair(element));
				return result;
			}
			return *IntegerPair(array->Elements, false);
		}
		bool NumericIntegerPayload(const Value &value) {
			if (Components(value, true)) return true;
			const auto *array = std::get_if<ArrayValue>(&value);
			const ValueType type = array ? array->ElementType : PayloadType(value);
			return type == ValueType::Scalar || type == ValueType::Integer || type == ValueType::Boolean ||
				   type == ValueType::Enum || type == ValueType::Colour;
		}

		Value Converted(const Value &value, bool integer, Validator validator) {
			const auto appendTuple = [&](const Tuple &tuple, std::vector<ElementValue> &to) {
				to.reserve(tuple.Count);
				for (size_t index = 0; index < tuple.Count; ++index)
					to.push_back(*Leaf(tuple.At(index), integer, validator));
			};
			if (const auto tuple = Components(value, integer)) {
				ArrayValue result{integer ? ValueType::Integer : ValueType::Boolean, {}};
				appendTuple(*tuple, result.Elements);
				return result;
			}
			if (const auto *array = std::get_if<ArrayValue>(&value)) {
				ArrayValue result{integer ? ValueType::Integer : ValueType::Boolean, {}};
				if (!integer && PackedArray(*array)) {
					result.Nested.reserve(array->Elements.size());
					for (const auto &element : array->Elements) {
						result.Nested.emplace_back();
						appendTuple(*Components(element, false), result.Nested.back());
					}
					return result;
				}
				const auto append = [&](const auto &from, auto &to) {
					to.reserve(from.size());
					for (const auto &element : from)
						to.push_back(*std::visit(
							[&](const auto &leaf) { return Leaf(leaf, integer, validator); }, element
						));
				};
				append(array->Elements, result.Elements);
				result.Nested.reserve(array->Nested.size());
				for (const auto &row : array->Nested) {
					result.Nested.emplace_back();
					append(row, result.Nested.back());
				}
				return result;
			}
			const auto element =
				*std::visit([&](const auto &item) { return Leaf(item, integer, validator); }, value);
			if (integer) return std::get<int64_t>(element);
			return std::get<bool>(element);
		}
	}
	SourceGetterProjection::~SourceGetterProjection() {
		if (Installed) Context.ValueViews = std::move(OriginalViews);
	}
	bool SourceGetterProjection::Prepare() {
		ENGINE_PROFILE("imagegraph.source_getter");
		using namespace source_getter_detail;
		const auto inputs = [&](const auto &visit) {
			for (const auto &input : Context.Entry.Inputs)
				if (!visit(input, input.Id)) return false;
			for (const auto &input : Context.Authored.DynamicInputs) {
				size_t group = 0;
				const auto *source = FindDynamicTemplate(Context.Entry, input.Id, group);
				if (source && Context.Authored.Type == "pc.hlsl" &&
					SourceArgumentType(Context.Authored, input.Id)) {
					const std::string selector =
						"argument_type_" + input.Id.substr(std::string_view("argument_value_").size());
					const auto declaredType =
						SourceArgumentType(Context.Authored, input.Id, Context.Find(selector));
					if (!declaredType || *declaredType != input.Type)
						return Context.Fail(
							Status::UnsupportedExecution,
							"resolved HLSL argument type differs from its native cooked schema",
							selector
						);
					const auto *value = Context.Find(input.Id);
					if (value &&
						!SourceHlslArgumentValue(
							Context.Authored,
							input.Id,
							*value,
							Context.Find(
								"argument_type_" + input.Id.substr(std::string_view("argument_value_").size())
							),
							true
						))
						return Context.Fail(
							Status::TypeMismatch,
							"HLSL argument value does not match its declared source shape",
							input.Id
						);
				}
				if (source && !visit(*source, input.Id)) return false;
			}
			return true;
		};
		size_t count = 0;
		uint64_t payloadBytes = 0;
		if (!inputs([&](const CatalogueInput &input, std::string_view port) {
				if (Context.Authored.Type == "pc.hlsl" && SourceArgumentType(Context.Authored, port) &&
					Context.Find(port)) {
					if (const auto tuple = Components(*Context.Find(port), false)) {
						++count;
						payloadBytes += tuple->Count * sizeof(ElementValue);
						return true;
					}
				}
				if (input.SourceKind == "IVec2" && input.Type == ValueType::Vector2) {
					// pinned IVec2 constructors use constant units; import refuses other saved unit modes.
					const auto *value = Context.Find(port);
					if (!value) return true;
					const auto bytes = IntegerVectorBytes(*value);
					if (!bytes)
						return Context.Fail(
							Status::UnsupportedExecution, "source IVec2 getter shape is unrepresented", port
						);
					++count;
					payloadBytes += *bytes;
					return true;
				}
				if (!CatalogueSourceRawValue(input, Value{0.})) return true;
				const auto *value =
					SourceRangeMapped(Context, port) ? SourceMappedRange(Context, port) : Context.Find(port);
				if (!value) return true;
				const auto *array = std::get_if<ArrayValue>(value);
				if (std::holds_alternative<Quaternion>(*value) ||
					(array && array->ElementType == ValueType::Quaternion))
					return Context.Fail(
						Status::UnsupportedExecution,
						"source raw quaternion getter representation is unverified",
						port
					);
				if (input.Type == ValueType::Boolean && std::holds_alternative<MatrixValue>(*value))
					return Context.Fail(
						Status::UnsupportedExecution,
						"source Bool conversion of Matrix struct is unverified",
						port
					);
				if (input.Type == ValueType::Boolean && array && PackedArray(*array) &&
					!array->Nested.empty())
					return Context.Fail(
						Status::UnsupportedExecution,
						"source recursive Bool shape exceeds represented array depth",
						port
					);
				if (input.Type == ValueType::Boolean && array && PackedArray(*array) &&
					!ProjectionBytes(*value, false))
					return Context.Fail(
						Status::LimitExceeded, "source Bool projection exceeds array leaf cap", port
					);
				if (input.SourceKind == "Active" &&
					(std::holds_alternative<ArrayValue>(*value) || Components(*value, false)))
					return Context.Fail(
						Status::UnsupportedExecution,
						"source Active non-scalar bool conversion is unverified",
						port
					);
				const auto changed =
					Changed(*value, input.Type == ValueType::Integer, LimitsFor(Context.Entry, input));
				if (!changed) {
					if (input.Type == ValueType::Integer && NumericIntegerPayload(*value))
						return Context.Fail(
							Status::InvalidValue, "integer input must be finite and within int64 range", port
						);
					return Context.Fail(
						Status::UnsupportedExecution, "source getter payload is unrepresented", port
					);
				}
				if (*changed) {
					++count;
					payloadBytes += *ProjectionBytes(*value, input.Type == ValueType::Integer);
				}
				return true;
			}))
			return false;
		if (!count) return true;
		const uint64_t bytes = payloadBytes + count * sizeof(Projected[0]) +
							   (Context.ValueViews.size() + count) * sizeof(OriginalViews[0]);
		auto reservation = Context.ReserveWorkspace(bytes, "source_getter");
		if (!reservation) return false;
		Charge = std::move(*reservation);
		try {
			Projected.reserve(count);
			if (!inputs([&](const CatalogueInput &input, std::string_view port) {
					if (Context.Authored.Type == "pc.hlsl" && SourceArgumentType(Context.Authored, port) &&
						Context.Find(port)) {
						if (const auto tuple = Components(*Context.Find(port), false)) {
							ArrayValue array;
							array.ElementType = ValueType::Scalar;
							array.Elements.reserve(tuple->Count);
							for (size_t i = 0; i < tuple->Count; ++i)
								array.Elements.emplace_back(tuple->At(i));
							Projected.emplace_back(port, std::move(array));
							return true;
						}
					}
					if (input.SourceKind == "IVec2" && input.Type == ValueType::Vector2) {
						if (const auto *value = Context.Find(port))
							Projected.emplace_back(port, IntegerVector(*value));
						return true;
					}
					if (!CatalogueSourceRawValue(input, Value{0.})) return true;
					const auto *value = SourceRangeMapped(Context, port) ? SourceMappedRange(Context, port)
																		 : Context.Find(port);
					if (!value) return true;
					const auto validator = LimitsFor(Context.Entry, input);
					if (*Changed(*value, input.Type == ValueType::Integer, validator))
						Projected.emplace_back(
							port, Converted(*value, input.Type == ValueType::Integer, validator)
						);
					return true;
				}))
				return false;
			OriginalViews = std::move(Context.ValueViews);
			Installed = true;
			Context.ValueViews.reserve(OriginalViews.size() + count);
			Context.ValueViews.insert(Context.ValueViews.end(), OriginalViews.begin(), OriginalViews.end());
			for (const auto &[port, value] : Projected)
				Context.ValueViews.emplace_back(port, &value);
		} catch (const std::bad_alloc &) {
			return Context.Fail(Status::LimitExceeded, "source getter allocation failed");
		}
		core::Metrics::Count("imagegraph.source_getter.owned_payload_bytes", bytes);
		return true;
	}
}
