// Value and project-level executors that follow the pinned source node update functions.

#include "../TextOps.hpp"
#include "../Utf8TextOps.hpp"
#include "../ValueOps.hpp"
#include "../ValuePayload.hpp"
#include "Families.hpp"
#include "Path.hpp"
#include "Processor.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::imagegraph::detail {
	namespace {
		// GameMaker round() rounds halves to even.
		double RoundHalfEven(double value) {
			return std::nearbyint(value);
		}

		uint64_t OutputNameBytes(std::string_view port) {
			return std::max<uint64_t>(port.size(), std::string{}.capacity());
		}

		bool ReserveValueOutput(NodeContext &context, uint64_t payload, std::string_view port) {
			const uint64_t name = OutputNameBytes(port);
			if (name > Limits::MaximumArrayBytes || payload > Limits::MaximumArrayBytes - name)
				return context.Fail(
					Status::LimitExceeded, "value output exceeds evaluation byte budget", port
				);
			return context.ReserveOutput(payload + name, port);
		}

		bool ReserveValueOutputs(
			NodeContext &context, std::initializer_list<std::pair<std::string_view, uint64_t>> outputs
		) {
			uint64_t total = 0;
			for (const auto &[port, payload] : outputs) {
				const uint64_t name = OutputNameBytes(port);
				if (name > Limits::MaximumArrayBytes - total ||
					payload > Limits::MaximumArrayBytes - total - name)
					return context.Fail(
						Status::LimitExceeded, "value outputs exceed evaluation byte budget", port
					);
				total += name + payload;
			}
			return context.ReserveOutput(total, outputs.size() ? outputs.begin()->first : std::string_view{});
		}

		bool ReserveTextOutput(NodeContext &context, size_t capacity, std::string_view port) {
			return ReserveValueOutput(context, std::max<uint64_t>(capacity, std::string{}.capacity()), port);
		}

		// node_number.gml update: passes Value through, rounding it when Integer is set. Flat numeric
		// arrays keep their shape, with each component rounded independently.
		bool Number(NodeContext &context) {
			const Value *value = context.Find("value");
			if (!value) return context.Fail(Status::InvalidValue, "number value is missing", "value");
			if (!context.Boolean("integer")) {
				context.SetValue("number", *value);
				return true;
			}
			if (const auto *scalar = std::get_if<double>(value)) {
				context.SetValue("number", RoundHalfEven(*scalar));
			} else if (const auto *vector = std::get_if<Vector2>(value)) {
				context.SetValue("number", Vector2{RoundHalfEven(vector->X), RoundHalfEven(vector->Y)});
			} else if (const auto *array = std::get_if<ArrayValue>(value)) {
				if (!array->Nested.empty() ||
					(array->ElementType != ValueType::Scalar && array->ElementType != ValueType::Integer))
					return context.Fail(
						Status::UnsupportedExecution, "number rounding needs a flat numeric array", "value"
					);
				if (!ValidRuntimeValue(*value))
					return context.Fail(
						Status::InvalidValue, "number array exceeds finite payload bounds", "value"
					);
				if (!ReserveValueOutput(context, RetainedPayloadBytes(*array), "number")) return false;
				ArrayValue rounded = *array;
				if (rounded.ElementType == ValueType::Scalar)
					for (auto &element : rounded.Elements)
						element = RoundHalfEven(std::get<double>(element));
				context.SetValue("number", std::move(rounded));
			} else {
				context.SetValue("number", *value);
			}
			return true;
		}

		// The official GameMaker default is inferred: no setter occurs in the pinned game sources.
		// This reproduces documented comparisons, without claiming native executable parity.
		constexpr double SOURCE_EPSILON = 0.00001;
		int SourceOrder(double a, double b) {
			const double difference = a - b;
			return std::abs(difference) <= SOURCE_EPSILON ? 0 : difference < 0 ? -1 : 1;
		}

		struct NumericInput {
			double Scalar = 0;
			bool Array = false;
			std::span<const ElementValue> Elements;
			std::span<const std::vector<ElementValue>> Rows;
			std::array<double, 6> Packed{};
			size_t PackedCount = 0;

			size_t Size() const {
				return !Rows.empty() ? Rows.size() : PackedCount ? PackedCount : Elements.size();
			}
		};

		template <class T> std::optional<double> NumericLeaf(const T &leaf) {
			if constexpr (std::is_same_v<T, double> || std::is_same_v<T, int64_t> || std::is_same_v<T, bool>)
				return static_cast<double>(leaf);
			else if constexpr (std::is_same_v<T, EnumValue>)
				return static_cast<double>(leaf.Value);
			else
				return std::nullopt;
		}

		bool ReadNumeric(NodeContext &context, std::string_view port, NumericInput &input) {
			const Value *value = context.Find(port);
			if (!value || !ValidRuntimeValue(*value))
				return context.Fail(
					Status::InvalidValue, "numeric input must have a bounded finite payload", port
				);
			if (auto scalar = std::visit([](const auto &leaf) { return NumericLeaf(leaf); }, *value)) {
				input.Scalar = *scalar;
				return true;
			}
			input.Array = true;
			if (const auto *array = std::get_if<ArrayValue>(value)) {
				if (array->ElementType != ValueType::Scalar && array->ElementType != ValueType::Integer &&
					array->ElementType != ValueType::Boolean && array->ElementType != ValueType::Enum)
					return context.Fail(
						Status::UnsupportedExecution, "arithmetic array needs numeric leaves", port
					);
				input.Elements = array->Elements;
				input.Rows = array->Nested;
			} else if (const auto *v = std::get_if<Vector2>(value)) {
				input.Packed = {v->X, v->Y};
				input.PackedCount = 2;
			} else if (const auto *v = std::get_if<Vector3>(value)) {
				input.Packed = {v->X, v->Y, v->Z};
				input.PackedCount = 3;
			} else if (const auto *v = std::get_if<Vector4>(value)) {
				input.Packed = {v->X, v->Y, v->Z, v->W};
				input.PackedCount = 4;
			} else if (const auto *v = std::get_if<Quaternion>(value)) {
				input.Packed = {v->X, v->Y, v->Z, v->W};
				input.PackedCount = 4;
			} else if (const auto *v = std::get_if<Area>(value)) {
				input.Packed = {
					v->CenterX, v->CenterY, v->HalfWidth, v->HalfHeight, double(v->Shape), double(v->Mode)
				};
				input.PackedCount = 6;
			} else
				return context.Fail(
					Status::UnsupportedExecution,
					"arithmetic input has no verified numeric source shape",
					port
				);
			return true;
		}

		NumericInput NumericAt(const NumericInput &input, size_t index, bool loop) {
			if (!input.Array) return input;
			const size_t count = input.Size();
			if (count == 0 || (!loop && index >= count)) return {};
			index = loop ? index % count : index;
			NumericInput selected;
			if (!input.Rows.empty()) {
				selected.Array = true;
				selected.Elements = input.Rows[index];
			} else if (input.PackedCount)
				selected.Scalar = input.Packed[index];
			else
				selected.Scalar =
					*std::visit([](const auto &leaf) { return NumericLeaf(leaf); }, input.Elements[index]);
			return selected;
		}

		struct ArithmeticOperation {
			double Mode = 0;
			bool Compare = false;
			bool Degrees = false;
			Vector2 From{0, 1};
			Vector2 To{0, 1};

			double Apply(double a, double b, double c) const {
				if (Compare) {
					const int order = SourceOrder(a, b);
					if (Mode == 0) return order == 0;
					if (Mode == 1) return order != 0;
					if (Mode == 2) return order > 0;
					if (Mode == 3) return order >= 0;
					if (Mode == 4) return order < 0;
					if (Mode == 5) return order <= 0;
					return 0;
				}
				if (Mode == 0) return a + b;
				if (Mode == 1) return a - b;
				if (Mode == 2) return a * b;
				if (Mode == 3) return SourceOrder(b, 0) == 0 ? 0 : a / b;
				if (Mode == 4) return SourceOrder(b, 0) >= 0 ? std::pow(a, b) : 1 / std::pow(a, -b);
				if (Mode == 5) return SourceOrder(b, 0) <= 0 ? 0 : std::pow(a, 1 / b);
				const double angle = Degrees ? a * std::numbers::pi / 180 : a;
				if (Mode == 6) return std::sin(angle) * b;
				if (Mode == 7) return std::cos(angle) * b;
				if (Mode == 8) return std::tan(angle) * b;
				if (Mode == 9) return SourceOrder(b, 0) == 0 ? 0 : std::fmod(a, b);
				if (Mode == 10) return std::floor(a);
				if (Mode == 11) return std::ceil(a);
				if (Mode == 12) return RoundHalfEven(a);
				if (Mode == 13) return a + (b - a) * c;
				if (Mode == 14) return std::abs(a);
				// GameMaker clamp applies the lower bound followed by the upper bound, even if reversed.
				if (Mode == 15) return std::min(std::max(a, b), c);
				if (Mode == 16) return SourceOrder(b, 0) == 0 ? a : RoundHalfEven(a / b) * b;
				if (Mode == 17) return a - std::trunc(a);
				if (Mode == 18) return To.X + (To.Y - To.X) * ((a - From.X) / (From.Y - From.X));
				if (Mode == 19) return std::log(a) / std::log(b);
				if (Mode == 20) return std::max(a, b);
				if (Mode == 21) return std::min(a, b);
				return 0;
			}
			bool BooleanResult() const {
				return Compare && Mode >= 0 && Mode <= 5 && Mode == std::floor(Mode);
			}
		};

		struct ArithmeticShape {
			size_t Depth = 0;
			size_t Length = 0;
			bool Boolean = false;
		};
		struct ArithmeticBudget {
			size_t Leaves = 0;
			size_t Rows = 0;
		};

		// Preflight visits every result without allocating; Compare pads, whereas Math loops short arrays.
		bool ArithmeticPreflight(
			NodeContext &context,
			const ArithmeticOperation &operation,
			const NumericInput &a,
			const NumericInput &b,
			const NumericInput &c,
			ArithmeticBudget &budget,
			ArithmeticShape &shape
		) {
			if (!a.Array && !b.Array && (operation.Compare || !c.Array)) {
				if (!std::isfinite(operation.Apply(a.Scalar, b.Scalar, c.Scalar)))
					return context.Fail(
						Status::InvalidValue, "source arithmetic produced a nonfinite result", "result"
					);
				shape.Boolean = operation.BooleanResult();
				if (++budget.Leaves > Limits::MaximumArrayElements)
					return context.Fail(
						Status::LimitExceeded, "source arithmetic exceeds aggregate element budget", "result"
					);
				return true;
			}
			const auto length = [&](const NumericInput &input) {
				return input.Array ? input.Size() : operation.Compare ? size_t{0} : size_t{1};
			};
			const size_t count = std::max({length(a), length(b), operation.Compare ? size_t{0} : length(c)});
			if (!count && operation.Compare) {
				if (++budget.Leaves > Limits::MaximumArrayElements)
					return context.Fail(
						Status::LimitExceeded, "source arithmetic exceeds aggregate element budget", "result"
					);
				return true;
			}
			shape = {1, count};
			size_t childDepth = 0;
			for (size_t index = 0; index < count; ++index) {
				ArithmeticShape child;
				if (!ArithmeticPreflight(
						context,
						operation,
						NumericAt(a, index, !operation.Compare),
						NumericAt(b, index, !operation.Compare),
						NumericAt(c, index, true),
						budget,
						child
					))
					return false;
				if (index && (child.Depth != childDepth || child.Boolean != shape.Boolean))
					return context.Fail(
						Status::UnsupportedExecution,
						"source arithmetic mixed depth or leaf types cannot be represented",
						"result"
					);
				childDepth = child.Depth;
				shape.Boolean = child.Boolean;
			}
			shape.Depth += childDepth;
			if (shape.Depth > 2)
				return context.Fail(
					Status::UnsupportedExecution, "source arithmetic exceeds supported nested depth", "result"
				);
			if (shape.Depth == 2) budget.Rows += count;
			if (count > Limits::MaximumArrayElements || budget.Rows > Limits::MaximumArrayElements ||
				budget.Leaves * sizeof(ElementValue) + budget.Rows * sizeof(std::vector<ElementValue>) >
					Limits::MaximumArrayBytes)
				return context.Fail(
					Status::LimitExceeded, "source arithmetic exceeds array payload budget", "result"
				);
			return true;
		}

		Value ArithmeticBuild(
			const ArithmeticOperation &operation,
			const NumericInput &a,
			const NumericInput &b,
			const NumericInput &c
		) {
			if (!a.Array && !b.Array && (operation.Compare || !c.Array)) {
				const double number = operation.Apply(a.Scalar, b.Scalar, c.Scalar);
				return operation.BooleanResult() ? Value{number != 0} : Value{number};
			}
			const auto length = [&](const NumericInput &input) {
				return input.Array ? input.Size() : operation.Compare ? size_t{0} : size_t{1};
			};
			const size_t count = std::max({length(a), length(b), operation.Compare ? size_t{0} : length(c)});
			if (!count && operation.Compare) return 0.0;
			ArrayValue output{operation.BooleanResult() ? ValueType::Boolean : ValueType::Scalar, {}};
			const bool nested = !a.Rows.empty() || !b.Rows.empty() || !c.Rows.empty();
			if (nested)
				output.Nested.reserve(count);
			else
				output.Elements.reserve(count);
			for (size_t index = 0; index < count; ++index) {
				Value child = ArithmeticBuild(
					operation,
					NumericAt(a, index, !operation.Compare),
					NumericAt(b, index, !operation.Compare),
					NumericAt(c, index, true)
				);
				if (auto *row = std::get_if<ArrayValue>(&child)) {
					if (!index) {
						output.ElementType = row->ElementType;
						output.Nested.reserve(count);
					}
					output.Nested.push_back(std::move(row->Elements));
				} else {
					if (!index) {
						output.ElementType =
							std::holds_alternative<bool>(child) ? ValueType::Boolean : ValueType::Scalar;
						output.Elements.reserve(count);
					}
					output.Elements.push_back(*ArrayElement(std::move(child)));
				}
			}
			return output;
		}

		bool NumberSimple(NodeContext &context) {
			const Value *value = context.Find("value");
			if (!value || !ValidRuntimeValue(*value))
				return context.Fail(
					Status::InvalidValue, "number value must have a bounded finite payload", "value"
				);
			if (!ReserveValueOutput(context, RetainedPayloadBytes(*value), "number")) return false;
			context.SetValue("number", *value);
			return true;
		}

		bool Arithmetic(NodeContext &context, bool compare) {
			ArithmeticOperation operation;
			operation.Compare = compare;
			operation.Mode = context.SourceChoice("type");
			NumericInput a, b, c;
			if (!ReadNumeric(context, "a", a) || !ReadNumeric(context, "b", b)) return false;
			if (!compare) {
				if (!ReadNumeric(context, "amount", c)) return false;
				operation.Degrees = context.SourceChoice("angle", 1) > .5;
				if (operation.Mode == 18) {
					NumericInput from, to;
					if (!ReadNumeric(context, "from", from) || !ReadNumeric(context, "to", to)) return false;
					if (!from.Rows.empty() || !to.Rows.empty() || (from.Array && from.Size() < 2) ||
						(to.Array && to.Size() < 2))
						return context.Fail(
							Status::UnsupportedExecution,
							"math mapping ranges need two finite numeric components",
							"from"
						);
					operation.From = {NumericAt(from, 0, false).Scalar, NumericAt(from, 1, false).Scalar};
					operation.To = {NumericAt(to, 0, false).Scalar, NumericAt(to, 1, false).Scalar};
				}
			}
			if (context.FailureCode != Status::Ok) return false;
			ArithmeticBudget budget;
			ArithmeticShape shape;
			if (!ArithmeticPreflight(context, operation, a, b, c, budget, shape)) return false;
			const uint64_t payloadBytes = shape.Depth ? budget.Leaves * sizeof(ElementValue) +
															budget.Rows * sizeof(std::vector<ElementValue>)
													  : 0;
			if (!ReserveValueOutput(context, payloadBytes, "result")) return false;
			// To Integer changes only the source junction declaration, and Output Vector only its display.
			Value result = ArithmeticBuild(operation, a, b, c);
			context.SetValue("result", std::move(result));
			return true;
		}
		bool Compare(NodeContext &context) {
			return Arithmetic(context, true);
		}
		bool Math(NodeContext &context) {
			return Arithmetic(context, false);
		}

		// node_project_output.gml update: copies the input surface.
		bool ProjectOutput(NodeContext &context) {
			const Image *source = context.Input("surface_in");
			if (!source) return context.Fail(Status::InvalidValue, "image input is missing", "surface_in");
			Image *out = context.NewImage("surface_out", source->Width, source->Height);
			if (!out) return false;
			out->Pixels = source->Pixels;
			return true;
		}

		// GameMaker runtime string(real): whole numbers print without decimals and other reals with two
		// decimal places. This is runtime behavior, not part of the pinned source.
		std::string GmlNumberText(double value) {
			char buffer[64];
			if (std::isfinite(value) && value == std::trunc(value) && std::abs(value) < 1e15)
				std::snprintf(buffer, sizeof(buffer), "%.0f", value);
			else
				std::snprintf(buffer, sizeof(buffer), "%.2f", value);
			return buffer;
		}

		// string(value) for the value kinds a linked input can carry.
		std::string GmlString(const Value &value) {
			if (const auto *text = std::get_if<std::string>(&value)) {
				std::string copy(text->size(), '\0');
				std::copy(text->begin(), text->end(), copy.begin());
				return copy;
			}
			if (const auto *number = std::get_if<double>(&value)) return GmlNumberText(*number);
			if (const auto *integer = std::get_if<int64_t>(&value)) return std::to_string(*integer);
			if (const auto *flag = std::get_if<bool>(&value)) return *flag ? "1" : "0";
			if (const auto *choice = std::get_if<EnumValue>(&value)) return std::to_string(choice->Value);
			if (const auto *vector = std::get_if<Vector2>(&value))
				return "[ " + GmlNumberText(vector->X) + ", " + GmlNumberText(vector->Y) + " ]";
			return {};
		}

		size_t GmlStringCapacity(const Value *value) {
			if (!value) return 0;
			if (const auto *text = std::get_if<std::string>(value)) return text->size();
			if (std::holds_alternative<Vector2>(*value)) return 138;
			if (std::holds_alternative<double>(*value)) return 64;
			if (std::holds_alternative<int64_t>(*value)) return 20;
			if (std::holds_alternative<EnumValue>(*value)) return 12;
			if (std::holds_alternative<bool>(*value)) return 1;
			return 0;
		}

		uint64_t GmlStringScratchBytes(const Value *value) {
			if (value && std::holds_alternative<Vector2>(*value)) return 4 * 138;
			return std::string{}.capacity();
		}

		bool TextFailure(NodeContext &context, TextOpStatus status, std::string_view port) {
			return context.Fail(
				status == TextOpStatus::LimitExceeded ? Status::LimitExceeded : Status::InvalidValue,
				status == TextOpStatus::InvalidUtf8 ? "text is not valid UTF-8"
													: "text operation has no defined result",
				port
			);
		}

		bool StringNode(NodeContext &context) {
			const Value *value = context.Find("text");
			const auto *text = value ? std::get_if<std::string>(value) : nullptr;
			if (text)
				context.SetValue("text", *text);
			else
				context.SetValue("text", std::string{});
			return true;
		}

		bool ToText(NodeContext &context) {
			const Value *value = context.Find("value");
			AllocationReservation scratch;
			if (value && std::holds_alternative<Vector2>(*value)) {
				auto reservation = context.ReserveWorkspace(GmlStringScratchBytes(value), "value");
				if (!reservation) return false;
				scratch = std::move(*reservation);
			}
			if (!ReserveTextOutput(context, GmlStringCapacity(value), "text")) return false;
			std::string text = value ? GmlString(*value) : std::string{};
			context.SetValue("text", std::move(text));
			return true;
		}

		bool StringCount(NodeContext &context) {
			const auto text = context.Find("text");
			const auto needle = context.Find("count_text");
			const auto *textValue = text ? std::get_if<std::string>(text) : nullptr;
			const auto *needleValue = needle ? std::get_if<std::string>(needle) : nullptr;
			context.SetValue(
				"amount",
				int64_t(CountText(
					textValue ? std::string_view(*textValue) : std::string_view{},
					needleValue ? std::string_view(*needleValue) : std::string_view{}
				))
			);
			return true;
		}

		bool StringDelete(NodeContext &context) {
			const auto *value = context.Find("text");
			const auto *text = value ? std::get_if<std::string>(value) : nullptr;
			const std::string_view source = text ? std::string_view(*text) : std::string_view{};
			[[maybe_unused]] size_t characters = 0;
			const TextOpStatus validation = CountText(source, characters);
			if (validation != TextOpStatus::Ok) return TextFailure(context, validation, "index");
			if (!ReserveTextOutput(context, source.size(), "text")) return false;
			std::string output(source.size(), '\0');
			const TextOpStatus status =
				DeleteText(source, context.Integer("index"), context.Integer("amount", 1), output);
			if (status != TextOpStatus::Ok) return TextFailure(context, status, "index");
			context.SetValue("text", std::move(output));
			return true;
		}

		bool StringLength(NodeContext &context) {
			const auto *value = context.Find("text");
			const auto *text = value ? std::get_if<std::string>(value) : nullptr;
			int64_t length = 0;
			const TextOpStatus status = TextLength(
				text ? std::string_view(*text) : std::string_view{}, context.Integer("mode"), length
			);
			if (status != TextOpStatus::Ok) return TextFailure(context, status, "text");
			context.SetValue("length", length);
			return true;
		}

		bool StringGetChar(NodeContext &context) {
			const auto *value = context.Find("text");
			const auto *text = value ? std::get_if<std::string>(value) : nullptr;
			const std::string_view source = text ? std::string_view(*text) : std::string_view{};
			[[maybe_unused]] size_t characters = 0;
			const TextOpStatus validation = CountText(source, characters);
			if (validation != TextOpStatus::Ok) return TextFailure(context, validation, "index");
			if (!ReserveTextOutput(context, source.size(), "text")) return false;
			std::string output(source.size(), '\0');
			const TextOpStatus status =
				CopyText(source, context.Integer("index", 1), context.Integer("amount", 1), output);
			if (status != TextOpStatus::Ok) return TextFailure(context, status, "index");
			context.SetValue("text", std::move(output));
			return true;
		}

		// string_titlecase uppercases after each space. Native case changes touch ASCII letters only; the
		// runtime's handling of other scripts is not in the source.
		bool StringChangeCase(NodeContext &context) {
			const auto *value = context.Find("text");
			const auto *source = value ? std::get_if<std::string>(value) : nullptr;
			const std::string_view input = source ? std::string_view(*source) : std::string_view{};
			if (!ReserveTextOutput(context, input.size(), "text")) return false;
			std::string text(input.size(), '\0');
			std::copy(input.begin(), input.end(), text.begin());
			const int64_t type = context.Integer("type");
			bool afterSpace = true;
			for (char &character : text) {
				const bool upper = type == 1 || (type == 2 && afterSpace);
				if (type == 0 && character >= 'A' && character <= 'Z')
					character = char(character - 'A' + 'a');
				if (upper && character >= 'a' && character <= 'z') character = char(character - 'a' + 'A');
				afterSpace = character == ' ';
			}
			context.SetValue("text", std::move(text));
			return true;
		}

		bool StringMerge(NodeContext &context) {
			size_t pieceCount = 0;
			uint64_t scratchBytes = 0;
			uint64_t outputBytes = 0;
			for (const DynamicInput &input : context.Authored.DynamicInputs) {
				size_t group = 0;
				if (!FindDynamicTemplate(context.Entry, input.Id, group)) continue;
				pieceCount++;
				const Value *value = context.Find(input.Id);
				const uint64_t capacity = GmlStringCapacity(value) + GmlStringScratchBytes(value);
				if (capacity > Limits::MaximumArrayBytes - scratchBytes)
					return context.Fail(
						Status::LimitExceeded, "merge scratch exceeds evaluation byte budget"
					);
				scratchBytes += capacity;
				const size_t length = GmlStringCapacity(context.Find(input.Id));
				if (length > Limits::MaximumTextBytes - outputBytes)
					return context.Fail(Status::LimitExceeded, "merged text exceeds the byte limit");
				outputBytes += length;
			}
			const uint64_t pieceStorage = pieceCount * sizeof(std::pair<size_t, std::string>);
			if (pieceStorage > Limits::MaximumArrayBytes - scratchBytes)
				return context.Fail(Status::LimitExceeded, "merge scratch exceeds evaluation byte budget");
			scratchBytes += pieceStorage;
			auto scratch = context.ReserveWorkspace(scratchBytes, "inputs");
			if (!scratch) return false;
			if (!ReserveTextOutput(context, outputBytes, "text")) return false;
			std::vector<std::pair<size_t, std::string>> pieces;
			pieces.reserve(pieceCount);
			for (const DynamicInput &input : context.Authored.DynamicInputs) {
				size_t group = 0;
				if (!FindDynamicTemplate(context.Entry, input.Id, group)) continue;
				const Value *value = context.Find(input.Id);
				pieces.emplace_back(group, value ? GmlString(*value) : std::string{});
			}
			std::sort(pieces.begin(), pieces.end(), [](const auto &a, const auto &b) {
				return a.first < b.first;
			});
			std::string merged(outputBytes, '\0');
			size_t mergedBytes = 0;
			for (const auto &[group, piece] : pieces) {
				if (piece.size() > Limits::MaximumTextBytes - mergedBytes ||
					piece.size() > outputBytes - mergedBytes)
					return context.Fail(Status::LimitExceeded, "merged text exceeds the byte limit");
				std::copy(piece.begin(), piece.end(), merged.begin() + mergedBytes);
				mergedBytes += piece.size();
			}
			merged.resize(mergedBytes);
			context.SetValue("text", std::move(merged));
			return true;
		}

		bool StringReplace(NodeContext &context) {
			const auto getText = [&](std::string_view id) -> std::string_view {
				const Value *value = context.Find(id);
				const auto *text = value ? std::get_if<std::string>(value) : nullptr;
				return text ? std::string_view(*text) : std::string_view{};
			};
			const std::string_view text = getText("text"), find = getText("find"),
								   replacement = getText("replace");
			// Native treats an empty pattern as matching nothing; the runtime result is not in the source.
			if (find.empty()) {
				if (!ReserveTextOutput(context, text.size(), "results")) return false;
				std::string output(text.size(), '\0');
				std::copy(text.begin(), text.end(), output.begin());
				context.SetValue("results", std::move(output));
				return true;
			}
			const bool all = context.Boolean("all", true);
			const size_t matches =
				all ? CountText(text, find) : size_t(text.find(find) != std::string_view::npos);
			size_t outputBytes = text.size();
			if (replacement.size() >= find.size()) {
				const size_t growth = replacement.size() - find.size();
				if (matches && growth > (Limits::MaximumTextBytes - outputBytes) / matches)
					return context.Fail(
						Status::LimitExceeded, "replaced text exceeds the byte limit", "text"
					);
				outputBytes += matches * growth;
			} else {
				outputBytes -= matches * (find.size() - replacement.size());
			}
			if (outputBytes > Limits::MaximumTextBytes)
				return context.Fail(Status::LimitExceeded, "replaced text exceeds the byte limit", "text");
			if (!ReserveTextOutput(context, outputBytes, "results")) return false;
			auto replaced = ReplaceText(text, find, replacement, all, Limits::MaximumTextBytes, outputBytes);
			if (!replaced)
				return context.Fail(Status::LimitExceeded, "replaced text exceeds the byte limit", "text");
			context.SetValue("results", std::move(*replaced));
			return true;
		}

		// chr(code) as UTF-8.
		bool Unicode(NodeContext &context) {
			const int64_t code = context.Integer("unicode", 64);
			if (code < 0 || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
				return context.Fail(Status::InvalidValue, "code point is outside Unicode", "unicode");
			const size_t encodedBytes = code < 0x80 ? 1 : code < 0x800 ? 2 : code < 0x10000 ? 3 : 4;
			if (!ReserveTextOutput(context, encodedBytes, "character")) return false;
			std::string text;
			const auto point = static_cast<uint32_t>(code);
			if (point < 0x80) {
				text += char(point);
			} else if (point < 0x800) {
				text += char(0xC0 | (point >> 6));
				text += char(0x80 | (point & 0x3F));
			} else if (point < 0x10000) {
				text += char(0xE0 | (point >> 12));
				text += char(0x80 | ((point >> 6) & 0x3F));
				text += char(0x80 | (point & 0x3F));
			} else {
				text += char(0xF0 | (point >> 18));
				text += char(0x80 | ((point >> 12) & 0x3F));
				text += char(0x80 | ((point >> 6) & 0x3F));
				text += char(0x80 | (point & 0x3F));
			}
			context.SetValue("character", std::move(text));
			return true;
		}

		// string_decimal: an optional leading minus, then digits only, keeping the first decimal point.
		std::string GmlDecimal(std::string text) {
			const bool negative = !text.empty() && text[0] == '-';
			if (negative) text.erase(0, 1);
			const auto digits = [](std::string_view part) {
				std::string kept;
				for (const char character : part)
					if (character >= '0' && character <= '9') kept += character;
				return kept;
			};
			const size_t point = text.find('.');
			if (point == std::string::npos) return (negative ? "-" : "") + digits(text);
			return (negative ? "-" : "") + digits(std::string_view(text).substr(0, point)) + "." +
				   digits(std::string_view(text).substr(point + 1));
		}

		// string_decimal.gml toNumberFull: split an exponent at "e", keep digits, then real() times 10^e.
		bool ToNumber(NodeContext &context) {
			const Value *input = context.Find("text");
			const auto *inputText = input ? std::get_if<std::string>(input) : nullptr;
			const std::string_view source = inputText ? std::string_view(*inputText) : std::string_view{};
			if (source.size() > (Limits::MaximumArrayBytes - 256) / 5)
				return context.Fail(
					Status::LimitExceeded, "numeric text scratch exceeds evaluation byte budget", "text"
				);
			auto scratch = context.ReserveWorkspace(source.size() * 5 + 256, "text");
			if (!scratch) return false;
			std::string text(source);
			double exponent = 0.0;
			const size_t e = text.find('e');
			if (e != std::string::npos) {
				const std::string power = text.substr(e + 1);
				if (!power.empty()) {
					char *end = nullptr;
					exponent = std::strtod(power.c_str(), &end);
					if (end == power.c_str() || *end != '\0')
						return context.Fail(Status::InvalidValue, "exponent is not a number", "text");
				}
				text = text.substr(0, e);
			}
			text = GmlDecimal(text);
			if (text.empty() || text == "." || text == "-") {
				if (!ReserveValueOutput(context, 0, "number")) return false;
				context.SetValue("number", 0.0);
				return true;
			}
			char *end = nullptr;
			const double value = std::strtod(text.c_str(), &end);
			if (end == text.c_str()) return context.Fail(Status::InvalidValue, "text has no number", "text");
			if (!ReserveValueOutput(context, 0, "number")) return false;
			context.SetValue("number", value * std::pow(10.0, exponent));
			return true;
		}

		// A linked source value is untyped in GML; read any numeric shape as its number list.
		bool NumbersOf(
			NodeContext &context,
			const Value *value,
			std::string_view port,
			std::vector<double> &numbers,
			AllocationReservation &charge
		) {
			if (!value) return true;
			size_t count = 0;
			if (std::holds_alternative<double>(*value) || std::holds_alternative<int64_t>(*value))
				count = 1;
			else if (std::holds_alternative<Vector2>(*value))
				count = 2;
			else if (std::holds_alternative<Vector3>(*value))
				count = 3;
			else if (std::holds_alternative<Vector4>(*value) || std::holds_alternative<Quaternion>(*value))
				count = 4;
			else if (const auto *array = std::get_if<ArrayValue>(value))
				count = std::count_if(
					array->Elements.begin(), array->Elements.end(), [](const ElementValue &item) {
						return std::holds_alternative<double>(item) || std::holds_alternative<int64_t>(item);
					}
				);
			if (count > Limits::MaximumArrayElements)
				return context.Fail(Status::LimitExceeded, "numeric vector exceeds the element limit", port);
			auto reservation = context.ReserveWorkspace(count * sizeof(double), port);
			if (!reservation) return false;
			charge = std::move(*reservation);
			numbers.reserve(count);
			if (const auto *number = std::get_if<double>(value))
				numbers.push_back(*number);
			else if (const auto *integer = std::get_if<int64_t>(value))
				numbers.push_back(double(*integer));
			else if (const auto *vector = std::get_if<Vector2>(value)) {
				numbers.push_back(vector->X);
				numbers.push_back(vector->Y);
			} else if (const auto *vector = std::get_if<Vector3>(value)) {
				numbers.push_back(vector->X);
				numbers.push_back(vector->Y);
				numbers.push_back(vector->Z);
			} else if (const auto *vector = std::get_if<Vector4>(value)) {
				numbers.push_back(vector->X);
				numbers.push_back(vector->Y);
				numbers.push_back(vector->Z);
				numbers.push_back(vector->W);
			} else if (const auto *rotation = std::get_if<Quaternion>(value)) {
				numbers.push_back(rotation->X);
				numbers.push_back(rotation->Y);
				numbers.push_back(rotation->Z);
				numbers.push_back(rotation->W);
			} else if (const auto *array = std::get_if<ArrayValue>(value))
				for (const ElementValue &element : array->Elements) {
					if (const auto *number = std::get_if<double>(&element))
						numbers.push_back(*number);
					else if (const auto *integer = std::get_if<int64_t>(&element))
						numbers.push_back(double(*integer));
				}
			return true;
		}

		double NumberAt(const std::vector<double> &numbers, size_t index) {
			return index < numbers.size() ? numbers[index] : 0.0;
		}

		bool NumberArray(
			NodeContext &context, std::span<const double> numbers, std::string_view port, ArrayValue &array
		) {
			if (numbers.size() > Limits::MaximumArrayElements ||
				numbers.size() > Limits::MaximumArrayBytes / sizeof(ElementValue))
				return context.Fail(Status::LimitExceeded, "numeric output exceeds the element limit", port);
			if (!ReserveValueOutput(context, numbers.size() * sizeof(ElementValue), port)) return false;
			array = {ValueType::Scalar, {}};
			array.Elements.reserve(numbers.size());
			for (const double number : numbers)
				array.Elements.emplace_back(number);
			return true;
		}

		bool VectorCross2D(NodeContext &context) {
			const Vector2 a = context.Vec2("point_1"), b = context.Vec2("point_2");
			context.SetValue("result", a.X * b.Y - a.Y * b.X);
			return true;
		}

		bool VectorCross3D(NodeContext &context) {
			AllocationReservation aCharge, bCharge;
			std::vector<double> a, b;
			if (!NumbersOf(context, context.Find("point_1"), "point_1", a, aCharge) ||
				!NumbersOf(context, context.Find("point_2"), "point_2", b, bCharge))
				return false;
			const double ax = NumberAt(a, 0), ay = NumberAt(a, 1), az = NumberAt(a, 2);
			const double bx = NumberAt(b, 0), by = NumberAt(b, 1), bz = NumberAt(b, 2);
			if (!ReserveValueOutput(context, 0, "result")) return false;
			context.SetValue("result", Vector3{ay * bz - az * by, az * bx - ax * bz, ax * by - ay * bx});
			return true;
		}

		bool VectorDirectionNode(NodeContext &context) {
			AllocationReservation vectorCharge;
			std::vector<double> vector;
			if (!NumbersOf(context, context.Find("vector"), "vector", vector, vectorCharge)) return false;
			if (vector.empty()) {
				if (!ReserveValueOutput(context, 0, "direction")) return false;
				context.SetValue("direction", 0.0);
				return true;
			}
			if (!ReserveValueOutput(context, 0, "direction")) return false;
			context.SetValue(
				"direction",
				VectorDirection({NumberAt(vector, 0), NumberAt(vector, 1)}, context.Integer("unit") != 0)
			);
			return true;
		}

		bool VectorMagnitudeNode(NodeContext &context) {
			AllocationReservation vectorCharge;
			std::vector<double> vector;
			if (!NumbersOf(context, context.Find("vector"), "vector", vector, vectorCharge)) return false;
			if (vector.size() == 1) {
				if (!ReserveValueOutput(context, 0, "magnitude")) return false;
				context.SetValue("magnitude", vector[0]);
				return true;
			}
			double total = 0.0;
			for (const double component : vector)
				total += component * component;
			if (!ReserveValueOutput(context, 0, "magnitude")) return false;
			context.SetValue("magnitude", std::sqrt(total));
			return true;
		}

		// dot_product with two or three components by the first vector's length, else 0.
		bool VectorDot(NodeContext &context) {
			AllocationReservation aCharge, bCharge;
			std::vector<double> a, b;
			if (!NumbersOf(context, context.Find("point_1"), "point_1", a, aCharge) ||
				!NumbersOf(context, context.Find("point_2"), "point_2", b, bCharge))
				return false;
			double result = 0.0;
			if (a.size() == 2) result = NumberAt(a, 0) * NumberAt(b, 0) + NumberAt(a, 1) * NumberAt(b, 1);
			if (a.size() == 3)
				result = NumberAt(a, 0) * NumberAt(b, 0) + NumberAt(a, 1) * NumberAt(b, 1) +
						 NumberAt(a, 2) * NumberAt(b, 2);
			if (!ReserveValueOutput(context, 0, "result")) return false;
			context.SetValue("result", result);
			return true;
		}

		// The source returns the vector itself; each output is one component.
		bool VectorSplit(NodeContext &context) {
			AllocationReservation vectorCharge;
			std::vector<double> vector;
			if (!NumbersOf(context, context.Find("vector"), "vector", vector, vectorCharge)) return false;
			constexpr std::array<std::string_view, 4> NAMES{"x", "y", "z", "w"};
			for (const std::string_view name : NAMES)
				if (!ReserveValueOutput(context, 0, name)) return false;
			for (size_t index = 0; index < 4; index++)
				context.SetValue(NAMES[index], NumberAt(vector, index));
			return true;
		}

		// A zero-length vector stays zero; an empty list returns 1, as the source does.
		bool VectorNormalize(NodeContext &context) {
			AllocationReservation vectorCharge;
			std::vector<double> vector;
			if (!NumbersOf(context, context.Find("vector"), "vector", vector, vectorCharge)) return false;
			if (vector.empty()) {
				if (!ReserveValueOutput(context, 0, "normalized_vector")) return false;
				context.SetValue("normalized_vector", 1.0);
				return true;
			}
			if (vector.size() == 1) {
				const std::array<double, 1> unit{1.0};
				ArrayValue output;
				if (!NumberArray(context, unit, "normalized_vector", output)) return false;
				context.SetValue("normalized_vector", std::move(output));
				return true;
			}
			double length = 0.0;
			for (const double component : vector)
				length += component * component;
			length = std::sqrt(length);
			ArrayValue normalized{ValueType::Scalar, {}};
			if (vector.size() > Limits::MaximumArrayBytes / sizeof(ElementValue) ||
				vector.size() > Limits::MaximumArrayElements)
				return context.Fail(
					Status::LimitExceeded, "normalized vector exceeds array limits", "vector"
				);
			if (!ReserveValueOutput(context, vector.size() * sizeof(ElementValue), "normalized_vector"))
				return false;
			normalized.Elements.reserve(vector.size());
			for (const double component : vector)
				normalized.Elements.emplace_back(length == 0.0 ? 0.0 : component / length);
			context.SetValue("normalized_vector", std::move(normalized));
			return true;
		}

		// lengthdir_x and lengthdir_y on a Y-down canvas.
		bool VectorPolarToCart(NodeContext &context) {
			const Vector2 polar = context.Vec2("polar_coord"), origin = context.Vec2("cartesian_origin");
			const double degrees =
				context.Integer("angle_unit") ? polar.Y * 180.0 / std::numbers::pi : polar.Y;
			const double radians = degrees * std::numbers::pi / 180.0;
			const double x = polar.X * std::cos(radians);
			double y = -polar.X * std::sin(radians);
			if (context.Boolean("invert_y")) y = -y;
			context.SetValue("cartesian_coord", Vector2{x - origin.X, y - origin.Y});
			return true;
		}

		bool VectorCartToPolar(NodeContext &context) {
			const Vector2 point = context.Vec2("cartesian_coord"), origin = context.Vec2("cartesian_origin");
			const double x = point.X - origin.X;
			double y = point.Y - origin.Y;
			if (context.Boolean("invert_y")) y = -y;
			double angle = std::atan2(y, x);
			if (!context.Integer("angle_unit")) angle = angle * 180.0 / std::numbers::pi;
			context.SetValue("polar_coord", Vector2{std::hypot(x, y), angle});
			return true;
		}

		bool VectorSwizzle(NodeContext &context) {
			AllocationReservation vectorCharge;
			std::vector<double> vector;
			if (!NumbersOf(context, context.Find("vector"), "vector", vector, vectorCharge)) return false;
			const Value *swizzleValue = context.Find("swizzle");
			const std::string *authoredSwizzle =
				swizzleValue ? std::get_if<std::string>(swizzleValue) : nullptr;
			const std::string_view swizzle = authoredSwizzle ? std::string_view(*authoredSwizzle) : "yxz";
			if (swizzle.empty()) {
				ArrayValue output;
				if (!NumberArray(context, vector, "result", output)) return false;
				context.SetValue("result", std::move(output));
				return true;
			}
			const auto component = [&](char character) {
				size_t index = 0;
				switch (character | 0x20) {
				case 'g':
				case 'y':
					index = 1;
					break;
				case 'b':
				case 'z':
					index = 2;
					break;
				case 'a':
				case 'w':
					index = 3;
					break;
				default:
					break;
				}
				return NumberAt(vector, index);
			};
			if (swizzle.size() == 1) {
				if (!ReserveValueOutput(context, 0, "result")) return false;
				context.SetValue("result", component(swizzle.front()));
				return true;
			}
			if (swizzle.size() > Limits::MaximumArrayElements ||
				swizzle.size() > Limits::MaximumArrayBytes / sizeof(ElementValue))
				return context.Fail(Status::LimitExceeded, "swizzle output exceeds array limits", "swizzle");
			if (!ReserveValueOutput(context, swizzle.size() * sizeof(ElementValue), "result")) return false;
			ArrayValue result{ValueType::Scalar, {}};
			result.Elements.reserve(swizzle.size());
			for (const char character : swizzle)
				result.Elements.emplace_back(component(character));
			context.SetValue("result", std::move(result));
			return true;
		}

		bool Vector3Node(NodeContext &context) {
			// node_value_bool.gml applies bool(), whose numeric truth threshold is greater than one half.
			const Value *flag = context.Find("integer");
			const auto number = flag ? std::visit([](const auto &leaf) { return NumericLeaf(leaf); }, *flag)
									 : std::optional<double>{0};
			if (!number)
				return context.Fail(
					Status::UnsupportedExecution,
					"Vector3 Integer requires a selected numeric Boolean",
					"integer"
				);
			if (!std::isfinite(*number))
				return context.Fail(Status::InvalidValue, "Vector3 Integer must be finite", "integer");
			const bool integer = *number > .5;
			const auto read = [&](std::string_view id) {
				const double value = context.Scalar(id);
				return integer ? std::nearbyint(value) : value;
			};
			const double x = read("x"), y = read("y"), z = read("z");
			if (!ReserveValueOutputs(context, {{"vector", 0}, {"x", 0}, {"y", 0}, {"z", 0}})) return false;
			context.SetValue("vector", Vector3{x, y, z});
			context.SetValue("x", x);
			context.SetValue("y", y);
			context.SetValue("z", z);
			return true;
		}

		bool ColourNode(NodeContext &context) {
			context.SetValue("color", context.Get<Colour>("color", Colour{255, 255, 255, 255}));
			return true;
		}

		bool ColourToHsv(NodeContext &context) {
			const auto data = ColorData(context.Get<Colour>("color", Colour{255, 255, 255, 255}), true);
			if (!ReserveValueOutputs(context, {{"hue", 0}, {"saturation", 0}, {"value", 0}})) return false;
			context.SetValue("hue", data[3]);
			context.SetValue("saturation", data[4]);
			context.SetValue("value", data[5]);
			return true;
		}

		bool ColourToRgb(NodeContext &context) {
			const Colour colour = context.Get<Colour>("color", Colour{255, 255, 255, 255});
			if (!ReserveValueOutputs(context, {{"red", 0}, {"green", 0}, {"blue", 0}})) return false;
			context.SetValue("red", colour.Red / 255.0);
			context.SetValue("green", colour.Green / 255.0);
			context.SetValue("blue", colour.Blue / 255.0);
			return true;
		}

		bool ColourMix(NodeContext &context) {
			const Colour from = context.Get<Colour>("color_from", Colour{255, 255, 255, 255});
			const Colour to = context.Get<Colour>("color_to", Colour{255, 255, 255, 255});
			const double amount = context.Scalar("mix", 0.5);
			const int64_t space = context.Integer("color_space");
			context.SetValue(
				"color",
				space == 1	 ? MixHsvColour(from, to, amount)
				: space == 2 ? MixOklabColour(from, to, amount)
							 : MixRgbColour(from, to, amount)
			);
			return true;
		}

		// matrix_functions.gml __matrix_determinant: cofactor expansion along the first row.
		double MatrixDeterminant(NodeContext &context, const std::vector<double> &matrix, size_t size) {
			if (size == 1) return matrix[0];
			if (size == 2) return matrix[0] * matrix[3] - matrix[1] * matrix[2];
			double determinant = 0.0;
			for (size_t column = 0; column < size; column++) {
				const size_t minorElements = (size - 1) * (size - 1);
				auto charge = context.ReserveWorkspace(minorElements * sizeof(double), "matrix");
				if (!charge) return 0.0;
				std::vector<double> minor;
				minor.reserve(minorElements);
				for (size_t row = 1; row < size; row++)
					for (size_t k = 0; k < size; k++)
						if (k != column) minor.push_back(matrix[row * size + k]);
				const double subdeterminant = MatrixDeterminant(context, minor, size - 1);
				if (context.FailureCode != Status::Ok) return 0.0;
				determinant += matrix[column] * subdeterminant * (column % 2 == 0 ? 1 : -1);
			}
			return determinant;
		}

		const MatrixValue *MatrixInput(const NodeContext &context, std::string_view id) {
			return std::get_if<MatrixValue>(context.Find(id));
		}

		bool MatrixNode(NodeContext &context) {
			const Vector2 size = context.Vec2("size", Vector2{3, 3});
			const auto columns = uint32_t(std::max(1.0, size.X)), rows = uint32_t(std::max(1.0, size.Y));
			if (uint64_t(columns) * rows > Limits::MaximumArrayElements)
				return context.Fail(Status::LimitExceeded, "matrix exceeds the element limit", "size");
			MatrixValue matrix{columns, rows, std::vector<double>(size_t(columns) * rows, 0.0)};
			if (const MatrixValue *data = MatrixInput(context, "data"))
				for (size_t index = 0; index < std::min(matrix.Values.size(), data->Values.size()); index++)
					matrix.Values[index] = data->Values[index];
			context.SetValue("matrix", std::move(matrix));
			return true;
		}

		bool MatrixIdentity(NodeContext &context) {
			const int64_t size = context.Integer("size", 3);
			if (size < 1 || uint64_t(size) * uint64_t(size) > Limits::MaximumArrayElements)
				return context.Fail(
					Status::LimitExceeded, "matrix size is outside the element limit", "size"
				);
			MatrixValue matrix{uint32_t(size), uint32_t(size), std::vector<double>(size_t(size * size), 0.0)};
			for (int64_t index = 0; index < size; index++)
				matrix.Values[size_t(index * size + index)] = 1.0;
			context.SetValue("matrix", std::move(matrix));
			return true;
		}

		bool MatrixTranspose(NodeContext &context) {
			const MatrixValue *matrix = MatrixInput(context, "matrix");
			if (!matrix) return context.Fail(Status::InvalidValue, "matrix input is missing", "matrix");
			MatrixValue result{
				matrix->Rows, matrix->Columns, std::vector<double>(matrix->Values.size(), 0.0)
			};
			for (uint32_t row = 0; row < matrix->Rows; row++)
				for (uint32_t column = 0; column < matrix->Columns; column++)
					result.Values[size_t(column) * matrix->Rows + row] =
						matrix->Values[size_t(row) * matrix->Columns + column];
			context.SetValue("matrix", std::move(result));
			return true;
		}

		bool MatrixToArray(NodeContext &context) {
			const MatrixValue *matrix = MatrixInput(context, "matrix");
			if (!matrix) return context.Fail(Status::InvalidValue, "matrix input is missing", "matrix");
			ArrayValue output;
			if (!NumberArray(context, matrix->Values, "array", output)) return false;
			context.SetValue("array", std::move(output));
			return true;
		}

		// A non-square matrix has determinant -1 in the source.
		bool MatrixDet(NodeContext &context) {
			const MatrixValue *matrix = MatrixInput(context, "matrix");
			if (!matrix) return context.Fail(Status::InvalidValue, "matrix input is missing", "matrix");
			if (matrix->Columns != matrix->Rows) {
				if (!ReserveValueOutput(context, 0, "determinant")) return false;
				context.SetValue("determinant", -1.0);
				return true;
			}
			if (matrix->Columns > 8)
				return context.Fail(
					Status::LimitExceeded, "cofactor expansion is bounded to 8 by 8 matrices", "matrix"
				);
			const double determinant = MatrixDeterminant(context, matrix->Values, matrix->Columns);
			if (context.FailureCode != Status::Ok) return false;
			if (!ReserveValueOutput(context, 0, "determinant")) return false;
			context.SetValue("determinant", determinant);
			return true;
		}

		// __matrix_inverse: non-square or singular matrices pass through unchanged.
		bool MatrixInvert(NodeContext &context) {
			const MatrixValue *matrix = MatrixInput(context, "matrix");
			if (!matrix) return context.Fail(Status::InvalidValue, "matrix input is missing", "matrix");
			if (matrix->Columns != matrix->Rows) {
				context.SetValue("matrix", *matrix);
				return true;
			}
			const size_t size = matrix->Columns;
			if (size > 8)
				return context.Fail(
					Status::LimitExceeded, "cofactor inversion is bounded to 8 by 8 matrices", "matrix"
				);
			const std::vector<double> &values = matrix->Values;
			const double determinant = MatrixDeterminant(context, values, size);
			if (context.FailureCode != Status::Ok) return false;
			if (determinant == 0.0) {
				context.SetValue("matrix", *matrix);
				return true;
			}
			if (!ReserveValueOutput(context, values.size() * sizeof(double), "matrix")) return false;
			MatrixValue inverse{matrix->Columns, matrix->Rows, {}};
			inverse.Values.reserve(values.size());
			inverse.Values.resize(values.size(), 0.0);
			if (size == 1) {
				inverse.Values[0] = 1.0 / values[0];
			} else if (size == 2) {
				inverse.Values = {
					values[3] / determinant,
					-values[1] / determinant,
					-values[2] / determinant,
					values[0] / determinant
				};
			} else {
				for (size_t i = 0; i < size; i++)
					for (size_t j = 0; j < size; j++) {
						auto charge =
							context.ReserveWorkspace((size - 1) * (size - 1) * sizeof(double), "matrix");
						if (!charge) return false;
						std::vector<double> minor;
						minor.reserve((size - 1) * (size - 1));
						for (size_t k = 0; k < size; k++) {
							if (k == i) continue;
							for (size_t l = 0; l < size; l++)
								if (l != j) minor.push_back(values[k * size + l]);
						}
						const double minorDeterminant = MatrixDeterminant(context, minor, size - 1);
						if (context.FailureCode != Status::Ok) return false;
						inverse.Values[j * size + i] =
							minorDeterminant * ((i + j) % 2 == 0 ? 1 : -1) / determinant;
					}
			}
			context.SetValue("matrix", std::move(inverse));
			return true;
		}

		// multiplyVector: missing vector components read as 1.
		bool MatrixMultiplyVector(NodeContext &context) {
			const MatrixValue *matrix = MatrixInput(context, "matrix");
			if (!matrix) return context.Fail(Status::InvalidValue, "matrix input is missing", "matrix");
			AllocationReservation vectorCharge;
			std::vector<double> vector;
			if (!NumbersOf(context, context.Find("vector"), "vector", vector, vectorCharge)) return false;
			if (matrix->Rows > Limits::MaximumArrayElements ||
				matrix->Rows > Limits::MaximumArrayBytes / sizeof(ElementValue))
				return context.Fail(
					Status::LimitExceeded, "matrix vector output exceeds array limits", "matrix"
				);
			if (!ReserveValueOutput(context, size_t(matrix->Rows) * sizeof(ElementValue), "vector"))
				return false;
			ArrayValue result{ValueType::Scalar, {}};
			result.Elements.reserve(matrix->Rows);
			for (uint32_t row = 0; row < matrix->Rows; row++) {
				double sum = 0.0;
				for (uint32_t column = 0; column < matrix->Columns; column++)
					sum += matrix->Values[size_t(row) * matrix->Columns + column] *
						   (column < vector.size() ? vector[column] : 1.0);
				result.Elements.emplace_back(sum);
			}
			context.SetValue("vector", std::move(result));
			return true;
		}

		// The source calls a native surface_is_empty_c extension; native reads empty as every RGBA byte zero.
		bool SurfaceIsEmpty(NodeContext &context) {
			const Image *surface = context.Input("surface_in");
			const bool empty =
				!surface || std::all_of(surface->Pixels.begin(), surface->Pixels.end(), [](uint8_t byte) {
					return byte == 0;
				});
			context.SetValue("is_empty", empty);
			return true;
		}

		// Native surfaces are RGBA8 (surface_rgba8unorm).
		bool SurfaceData(NodeContext &context) {
			const Image *surface = context.Input("surface");
			if (!surface) return true;
			const uint64_t formatBytes =
				std::max<uint64_t>(std::string{}.capacity(), std::string_view("8bit RGBA").size());
			if (!ReserveValueOutputs(
					context,
					{{"dimension", 0},
					 {"width", 0},
					 {"height", 0},
					 {"format_string", formatBytes},
					 {"bit_depth", 0},
					 {"channels", 0}}
				))
				return false;
			context.SetValue("dimension", Vector2{double(surface->Width), double(surface->Height)});
			context.SetValue("width", int64_t(surface->Width));
			context.SetValue("height", int64_t(surface->Height));
			context.SetValue("format_string", std::string("8bit RGBA"));
			context.SetValue("bit_depth", int64_t{8});
			context.SetValue("channels", int64_t{4});
			return true;
		}

		bool RotationRangeData(NodeContext &context) {
			context.SetValue(
				"rotation_range", Vector2{context.Scalar("start"), context.Scalar("end", 360.0)}
			);
			return true;
		}

		bool CornerData(NodeContext &context) {
			context.SetValue(
				"corner",
				Vector4{
					context.Scalar("top_left"),
					context.Scalar("top_right"),
					context.Scalar("bottom_left"),
					context.Scalar("bottom_right")
				}
			);
			return true;
		}

		// The source returns [right, top, left, bottom], scaled by the reference surface when fractional.
		bool PaddingData(NodeContext &context) {
			double left = context.Scalar("left"), right = context.Scalar("right");
			double top = context.Scalar("top"), bottom = context.Scalar("bottom");
			if (context.Boolean("use_fractional")) {
				const Image *reference = context.Input("reference_surface");
				const double width = reference ? reference->Width : 1.0,
							 height = reference ? reference->Height : 1.0;
				left *= width;
				right *= width;
				top *= height;
				bottom *= height;
			}
			context.SetValue("padding", Vector4{right, top, left, bottom});
			return true;
		}

		bool PointInArea(NodeContext &context) {
			const Area area = context.Get<Area>("area");
			const Vector2 point = context.Vec2("point");
			const bool boundary = context.Boolean("include_boundary", true);
			const double halfWidth = std::abs(area.HalfWidth), halfHeight = std::abs(area.HalfHeight);
			bool inside = false;
			if (area.Shape == 0) {
				const double x0 = area.CenterX - halfWidth, x1 = area.CenterX + halfWidth;
				const double y0 = area.CenterY - halfHeight, y1 = area.CenterY + halfHeight;
				inside = boundary ? point.X >= x0 && point.X <= x1 && point.Y >= y0 && point.Y <= y1
								  : point.X > x0 && point.X < x1 && point.Y > y0 && point.Y < y1;
			} else {
				const double nx = (point.X - area.CenterX) / halfWidth,
							 ny = (point.Y - area.CenterY) / halfHeight;
				inside = boundary ? nx * nx + ny * ny <= 1.0 : nx * nx + ny * ny < 1.0;
			}
			context.SetValue("is_in", inside);
			return true;
		}

		// point_rotate_array rotates by -angle on a Y-down canvas, with exact 0 and 180 degree cases.
		bool MovePoint(NodeContext &context) {
			Vector2 point = context.Vec2("point");
			const Vector2 anchor = context.Vec2("anchor_point", Vector2{0.5, 0.5});
			const Vector2 position = context.Vec2("position"), scale = context.Vec2("scale", Vector2{1, 1});
			const double angle = context.Scalar("rotation");
			if (angle == 180.0) {
				point = {anchor.X + (anchor.X - point.X), anchor.Y + (anchor.Y - point.Y)};
			} else if (angle != 0.0) {
				const double cx = point.X - anchor.X, cy = point.Y - anchor.Y;
				const double radians = -angle * std::numbers::pi / 180.0;
				point = {
					anchor.X + cx * std::cos(radians) - cy * std::sin(radians),
					anchor.Y + cx * std::sin(radians) + cy * std::cos(radians)
				};
			}
			point.X = anchor.X + (point.X - anchor.X) * scale.X + position.X;
			point.Y = anchor.Y + (point.Y - anchor.Y) * scale.Y + position.Y;
			context.SetValue("result", point);
			return true;
		}

		bool PathLength(NodeContext &context) {
			const Path2D empty;
			const auto *path = std::get_if<Path2D>(context.Find("path"));
			PathRuntime runtime;
			if (!runtime.Init(context, path ? *path : empty)) return false;
			if (!ReserveValueOutput(context, 0, "surface_out")) return false;
			context.SetValue("surface_out", runtime.LengthTotal);
			return context.FailureCode == Status::Ok;
		}

		// number_function.gml convertBase: letters are digits 10 and up, other characters count as 0.
		bool BaseConvert(NodeContext &context) {
			const Value *input = context.Find("value");
			const auto *textValue = input ? std::get_if<std::string>(input) : nullptr;
			const std::string_view text = textValue ? std::string_view(*textValue) : std::string_view{};
			const double from = std::max<int64_t>(2, context.Integer("base_from", 10));
			const double to = std::max<int64_t>(2, context.Integer("base_to", 10));
			double number = 0.0;
			for (size_t index = 1; index <= text.size(); index++) {
				const char digit = text[text.size() - index];
				double value = 0.0;
				if (digit >= '0' && digit <= '9')
					value = digit - '0';
				else if (digit >= 'A' && digit <= 'Z')
					value = digit - 'A' + 10;
				else if (digit >= 'a' && digit <= 'z')
					value = digit - 'a' + 10;
				number += value * std::pow(from, double(index - 1));
			}
			size_t resultBytes = 0;
			for (double remaining = number; remaining > 0.0; remaining = std::floor(remaining / to)) {
				if (resultBytes == 1024)
					return context.Fail(Status::LimitExceeded, "converted number is too long", "value");
				resultBytes++;
			}
			if (!ReserveTextOutput(context, resultBytes, "result")) return false;
			std::string result(resultBytes, '\0');
			size_t destination = resultBytes;
			while (number > 0.0) {
				const double digit = std::fmod(number, to);
				result[--destination] = char(digit < 10 ? '0' + int(digit) : 'A' + int(digit) - 10);
				number = std::floor(number / to);
			}
			context.SetValue("result", std::move(result));
			return true;
		}

		// BBMOD quarternionFromEuler: negated half angles in degrees, composed Z, X, then Y.
		bool QuaternionFromEuler(NodeContext &context) {
			AllocationReservation eulerCharge;
			std::vector<double> euler;
			if (!NumbersOf(context, context.Find("euler_rotation"), "euler_rotation", euler, eulerCharge))
				return false;
			const double degrees = std::numbers::pi / 180.0;
			const double x = -NumberAt(euler, 0) * 0.5, y = -NumberAt(euler, 1) * 0.5,
						 z = -NumberAt(euler, 2) * 0.5;
			double sine = std::sin(z * degrees), cosine = std::cos(z * degrees);
			const double qx = cosine * std::sin(x * degrees), qy = sine * std::sin(x * degrees);
			const double qz = sine * std::cos(x * degrees), qw = cosine * std::cos(x * degrees);
			sine = std::sin(y * degrees);
			cosine = std::cos(y * degrees);
			if (!ReserveValueOutput(context, 0, "rotation")) return false;
			context.SetValue(
				"rotation",
				Vector4{
					qx * cosine - qz * sine,
					qw * sine + qy * cosine,
					qz * cosine + qx * sine,
					qw * cosine - qy * sine
				}
			);
			return true;
		}

		// quarternionToEuler: roll, pitch and yaw in degrees rounded to thousandths.
		bool QuaternionToEuler(NodeContext &context) {
			AllocationReservation rotationCharge;
			std::vector<double> q;
			if (!NumbersOf(context, context.Find("rotation"), "rotation", q, rotationCharge)) return false;
			const double x = NumberAt(q, 0), y = NumberAt(q, 1), z = NumberAt(q, 2), w = NumberAt(q, 3);
			const double roll = std::atan2(2.0 * (w * x + y * z), 1.0 - 2.0 * (x * x + y * y));
			const double pitch = std::asin(std::clamp(2.0 * (w * y - z * x), -1.0, 1.0));
			const double yaw = std::atan2(2.0 * (w * z + x * y), 1.0 - 2.0 * (y * y + z * z));
			const auto degrees = [](double radians) {
				return std::nearbyint(radians * 180.0 / std::numbers::pi * 1000.0) / 1000.0;
			};
			if (!ReserveValueOutput(context, 0, "euler_angles")) return false;
			context.SetValue("euler_angles", Vector3{degrees(roll), degrees(pitch), degrees(yaw)});
			return true;
		}

		constexpr ExecutorEntry VALUE_EXECUTORS[] = {
			{"pc.base_convert", BaseConvert},
			{"pc.corner_data", CornerData},
			{"pc.move_point", MovePoint},
			{"pc.padding_data", PaddingData},
			{"pc.path_length", PathLength},
			{"pc.point_in_area", PointInArea},
			{"pc.quarternion_from_euler", QuaternionFromEuler},
			{"pc.quarternion_to_euler", QuaternionToEuler},
			{"pc.rotation_range_data", RotationRangeData},
			{"pc.surface_data", SurfaceData},
			{"pc.surface_is_empty", SurfaceIsEmpty},
			{"pc.color", ColourNode},
			{"pc.matrix", MatrixNode},
			{"pc.matrix_det", MatrixDet},
			{"pc.matrix_identity", MatrixIdentity},
			{"pc.matrix_invert", MatrixInvert},
			{"pc.matrix_multiply_vector", MatrixMultiplyVector},
			{"pc.matrix_to_array", MatrixToArray},
			{"pc.matrix_transpose", MatrixTranspose},
			{"pc.color_mix", ColourMix},
			{"pc.color_to_hsv", ColourToHsv},
			{"pc.color_to_rgb", ColourToRgb},
			{"pc.number", Number},
			{"pc.number_simple", NumberSimple},
			{"pc.compare", Compare},
			{"pc.math", Math},
			{"pc.project_output", ProjectOutput},
			{"pc.string", StringNode},
			{"pc.string_change_case", StringChangeCase},
			{"pc.string_count", StringCount},
			{"pc.string_delete", StringDelete},
			{"pc.string_get_char", StringGetChar},
			{"pc.string_length", StringLength},
			{"pc.string_merge", StringMerge},
			{"pc.string_replace", StringReplace},
			{"pc.to_number", ToNumber},
			{"pc.to_text", ToText},
			{"pc.unicode", Unicode},
			{"pc.vector3", Vector3Node},
			{"pc.vector_cart_to_polar", VectorCartToPolar},
			{"pc.vector_cross_2_d", VectorCross2D},
			{"pc.vector_cross_3_d", VectorCross3D},
			{"pc.vector_direction", VectorDirectionNode},
			{"pc.vector_dot", VectorDot},
			{"pc.vector_magnitude", VectorMagnitudeNode},
			{"pc.vector_normalize", VectorNormalize},
			{"pc.vector_polar_to_cart", VectorPolarToCart},
			{"pc.vector_split", VectorSplit},
			{"pc.vector_swizzle", VectorSwizzle},
		};
	}

	std::span<const ExecutorEntry> ValueExecutors() {
		return VALUE_EXECUTORS;
	}
}
