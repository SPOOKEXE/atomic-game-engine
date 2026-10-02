#include "../Utf8TextOps.hpp"
#include "ArraySource.hpp"
#include "Families.hpp"

#include <engine/imagegraph/DataReplay.hpp>
#include <engine/imagegraph/FrameTime.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>

namespace engine::imagegraph::detail {
	namespace {
		bool StatefulDataNode(NodeContext &context) {
			const auto *replay = context.CurrentData ? context.CurrentData : context.Request.DataReplay;
			if (replay) {
				Diagnostic diagnostic;
				const auto status = ValidateDataReplay(*replay, context.ByteBudget, diagnostic);
				if (status != Status::Ok) return context.Fail(status, diagnostic.Message);
			}
			const DataReplayEntry *previous = nullptr;
			if (replay)
				for (const auto &entry : replay->Entries)
					if (entry.NodeId == context.Authored.Id && entry.ProcessorRow == context.ProcessorRow)
						previous = &entry;
			const size_t bytes =
				sizeof(DataReplayEntry) + std::max(context.Authored.Id.size(), std::string{}.capacity());
			if (!context.ReserveOutput(bytes, "data history")) return false;
			DataReplayEntry state;
			state.NodeId = context.Authored.Id;
			state.ProcessorRow = context.ProcessorRow;
			state.Tick = context.Request.Tick;
			state.Subframe = context.Request.Subframe;
			state.NegativeFrame = context.Request.NegativeFrame;
			state.Initialized = true;
			state.PreviousFrame = double(FrameTimeToReal({state.Tick, state.Subframe, state.NegativeFrame}));
			if (context.Authored.Type == "pc.trigger_bool") {
				const bool value = context.Boolean("boolean");
				const bool before = previous && previous->PreviousValue != 0;
				const double mode = context.SourceChoice("trigger_condition");
				state.Trigger = previous && previous->Trigger;
				if (mode == 0)
					state.Trigger = value;
				else if (mode == 1)
					state.Trigger = !before && value;
				else if (mode == 2)
					state.Trigger = before && !value;
				else if (mode == 3)
					state.Trigger = before != value;
				state.PreviousValue = value ? 1 : 0;
				context.SetValue("trigger", state.Trigger);
			} else {
				state.PreviousValue = context.Scalar("value");
				const double delta = state.PreviousFrame - (previous ? previous->PreviousFrame : 0);
				const double output =
					delta == 0 ? 0 : (state.PreviousValue - (previous ? previous->PreviousValue : 0)) / delta;
				if (!std::isfinite(state.PreviousValue) || !std::isfinite(output))
					return context.Fail(Status::InvalidValue, "differential result is nonfinite", "value");
				context.SetValue("result", output);
			}
			if (context.FailureCode != Status::Ok) return false;
			context.DataUpdates.push_back(std::move(state));
			return true;
		}
		std::string_view
		TextInput(NodeContext &context, std::string_view id, std::string_view fallback = {}) {
			const auto *value = context.Find(id);
			const auto *text = value ? std::get_if<std::string>(value) : nullptr;
			return text ? std::string_view(*text) : fallback;
		}
		bool SeparateFilePath(NodeContext &context) {
			const auto path = TextInput(context, "path");
			size_t characters = 0;
			if (CountText(path, characters) != TextOpStatus::Ok)
				return context.Fail(Status::InvalidValue, "file path is not valid UTF-8", "path");
			const size_t slash = path.find_last_of("/\\");
			const bool split = slash != std::string_view::npos && slash > 0;
			const auto directory = split ? path.substr(0, slash) : path;
			const auto name = split ? path.substr(slash + 1) : path;
			std::string_view extension;
			if (!context.Boolean("keep_extension", true) && !name.empty()) {
				const auto dot = name.find_last_of('.'), backslash = name.find_last_of('\\');
				if (backslash == std::string_view::npos ||
					(dot != std::string_view::npos && backslash <= dot)) {
					if (dot != std::string_view::npos)
						extension = name.substr(dot);
					else {
						size_t length = 0;
						CountText(name, length);
						extension = name.substr(ByteOffset(name, length - 1));
						if (extension.size() == 4)
							return context.Fail(
								Status::UnsupportedExecution,
								"HTML5 filename extension splits a surrogate pair",
								"path"
							);
					}
				}
			}
			auto charge = context.ReserveWorkspace(directory.size() + name.size(), "path");
			if (!charge) return false;
			std::string filename(name);
			if (!extension.empty()) {
				const auto occurrence = filename.find(extension);
				if (occurrence != std::string::npos) filename.erase(occurrence, extension.size());
			}
			context.SetValue("directory", std::string(directory));
			context.SetValue("file_name", std::move(filename));
			return context.FailureCode == Status::Ok;
		}
		bool BufferTextNode(NodeContext &context) {
			const auto *value = context.Find("input_0");
			const auto *buffer = value ? std::get_if<BufferValue>(value) : nullptr;
			const double mode = context.SourceChoice("format", 1);
			if (mode < 0 || mode > 3 || std::floor(mode) != mode)
				return context.Fail(Status::InvalidValue, "buffer text format is invalid", "format");
			if (value && !buffer)
				return context.Fail(Status::TypeMismatch, "buffer text requires a buffer", "input_0");
			const size_t count = buffer ? buffer->Bytes.size() : 0;
			size_t length = mode == 0	? count * 8
							: mode == 1 ? count * 2
							: mode == 3 ? ((count + 2) / 3) * 4
										: count;
			if (mode == 2 && buffer)
				length = size_t(
					std::find(buffer->Bytes.begin(), buffer->Bytes.end(), uint8_t{0}) - buffer->Bytes.begin()
				);
			if (length > Limits::MaximumTextBytes)
				return context.Fail(
					Status::LimitExceeded, "encoded buffer exceeds text bounds", "string_out"
				);
			auto charge = context.ReserveWorkspace(length, "string_out");
			if (!charge) return false;
			std::string output;
			output.reserve(length);
			constexpr std::string_view hex = "0123456789ABCDEF",
									   base64 =
										   "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
			if (buffer && mode == 2) {
				if (length) output.assign(reinterpret_cast<const char *>(buffer->Bytes.data()), length);
			} else if (buffer && mode == 3) {
				// Unsigned bytes keep the embedded source extension's intended Base64 encoding defined.
				for (size_t index = 0; index < count; index += 3) {
					const uint32_t bits = uint32_t(buffer->Bytes[index]) << 16 |
										  (index + 1 < count ? uint32_t(buffer->Bytes[index + 1]) << 8 : 0) |
										  (index + 2 < count ? uint32_t(buffer->Bytes[index + 2]) : 0);
					output.push_back(base64[(bits >> 18) & 63]);
					output.push_back(base64[(bits >> 12) & 63]);
					output.push_back(index + 1 < count ? base64[(bits >> 6) & 63] : '=');
					output.push_back(index + 2 < count ? base64[bits & 63] : '=');
				}
			} else if (buffer) {
				for (uint8_t byte : buffer->Bytes)
					if (mode == 0)
						for (int bit = 7; bit >= 0; --bit)
							output.push_back((byte & (1u << bit)) ? '1' : '0');
					else {
						output.push_back(hex[byte >> 4]);
						output.push_back(hex[byte & 15]);
					}
			}
			context.SetValue("string_out", std::move(output));
			return context.FailureCode == Status::Ok;
		}
		std::string SourceFixed(double value, int precision) {
			std::array<char, 512> buffer{};
			const double magnitude = std::abs(value);
			const bool exponential = magnitude >= 1e21;
			const auto converted =
				exponential
					? std::to_chars(buffer.data(), buffer.data() + buffer.size(), value == 0 ? 0 : value)
					: std::to_chars(
						  buffer.data(),
						  buffer.data() + buffer.size(),
						  value == 0 ? 0 : value,
						  std::chars_format::fixed,
						  precision
					  );
			if (converted.ec != std::errc{}) return {};
			std::string text(buffer.data(), converted.ptr);
			if (exponential) {
				const auto exponent = text.find('e');
				if (exponent != std::string::npos) {
					size_t digit = exponent + 1;
					if (text[digit] != '+' && text[digit] != '-') text.insert(digit, 1, '+');
					++digit;
					while (digit + 1 < text.size() && text[digit] == '0')
						text.erase(digit, 1);
				}
				return text;
			}
			// ECMAScript toFixed rounds exact decimal ties away from zero, unlike to_chars.
			const uint64_t bits = std::bit_cast<uint64_t>(magnitude);
			uint64_t mantissa = bits & ((uint64_t{1} << 52) - 1);
			const int encodedExponent = int((bits >> 52) & 2047);
			if (encodedExponent) mantissa |= uint64_t{1} << 52;
			if (mantissa) {
				const int zeros = std::countr_zero(mantissa);
				const int exponent = (encodedExponent ? encodedExponent - 1023 - 52 : -1074) + zeros;
				if (exponent + precision == -1 && ((mantissa >> zeros) & 3) == 1) {
					bool carry = true;
					for (size_t index = text.size(); index > 0 && carry;) {
						char &digit = text[--index];
						if (digit == '.' || digit == '-') continue;
						if (digit == '9')
							digit = '0';
						else {
							++digit;
							carry = false;
						}
					}
					if (carry) text.insert(value < 0 ? 1 : 0, 1, '1');
				}
			}
			return text;
		}
		bool NumberTextNode(NodeContext &context) {
			const double number = context.Scalar("number");
			const int64_t decimals = context.Integer("minimum_digit_2", 2),
						  integers = context.Integer("minimum_digit", 2);
			if (!std::isfinite(number) || decimals < 0 || decimals > 100 ||
				(integers > 0 && uint64_t(integers) > Limits::MaximumTextBytes))
				return context.Fail(
					Status::InvalidValue, "number format digits are outside source bounds", "minimum_digit_2"
				);
			const auto intPad = TextInput(context, "pad_letter", "0"),
					   decPad = TextInput(context, "pad_letter_2", "0"),
					   thousands = TextInput(context, "thousands_sep"),
					   separator = TextInput(context, "decimal_sep", "."),
					   prefix = TextInput(context, "prefix"), suffix = TextInput(context, "suffix");
			for (auto text : {intPad, decPad, thousands, separator, prefix, suffix}) {
				size_t characters = 0;
				if (CountText(text, characters) != TextOpStatus::Ok)
					return context.Fail(
						Status::InvalidValue, "number format text is not valid UTF-8", "text"
					);
			}
			const std::string fixed = SourceFixed(number, int(decimals));
			const auto dot = fixed.find('.');
			std::string integer = fixed.substr(0, dot),
						decimal = dot == std::string::npos ? std::string{} : fixed.substr(dot + 1);
			while (!decimal.empty() && decimal.back() == '0')
				decimal.pop_back();
			const size_t integerCount = integer.size();
			const size_t integerPads =
				context.Boolean("pad_integer") && integers > 0 && size_t(integers) > integerCount
					? size_t(integers) - integerCount
					: 0;
			const size_t decimalPads = context.Boolean("pad_decimal") && size_t(decimals) > decimal.size()
										   ? size_t(decimals) - decimal.size()
										   : 0;
			if ((intPad.size() && integerPads > Limits::MaximumTextBytes / intPad.size()) ||
				(decPad.size() && decimalPads > Limits::MaximumTextBytes / decPad.size()))
				return context.Fail(Status::LimitExceeded, "number padding exceeds text bounds", "text");
			size_t intChars = 0;
			CountText(intPad, intChars);
			const size_t paddedChars = integerCount + integerPads * intChars;
			const size_t separators = thousands.empty() || paddedChars == 0 ? 0 : (paddedChars - 1) / 3;
			if (thousands.size() && separators > Limits::MaximumTextBytes / thousands.size())
				return context.Fail(Status::LimitExceeded, "number separators exceed text bounds", "text");
			const size_t bytes = fixed.size() + integerPads * intPad.size() + decimalPads * decPad.size() +
								 separators * thousands.size() + prefix.size() + suffix.size() +
								 separator.size();
			if (bytes > Limits::MaximumTextBytes)
				return context.Fail(Status::LimitExceeded, "formatted number exceeds text bounds", "text");
			auto charge = context.ReserveWorkspace(bytes * 3 + 512, "text");
			if (!charge) return false;
			std::string padded;
			padded.reserve(integer.size() + integerPads * intPad.size());
			for (size_t index = 0; index < integerPads; ++index)
				padded += intPad;
			padded += integer;
			if (!thousands.empty()) {
				integer.clear();
				for (size_t character = 0, offset = 0; offset < padded.size(); ++character) {
					if (character && (paddedChars - character) % 3 == 0) integer += thousands;
					const size_t start = offset;
					NextUtf8(padded, offset);
					integer.append(padded, start, offset - start);
				}
			} else
				integer = std::move(padded);
			for (size_t index = 0; index < decimalPads; ++index)
				decimal += decPad;
			std::string output(prefix);
			output += integer;
			if (!decimal.empty()) {
				output += separator;
				output += decimal;
			}
			output += suffix;
			context.SetValue("text", std::move(output));
			return context.FailureCode == Status::Ok;
		}
		bool AreaNode(NodeContext &context) {
			const auto first = context.Vec2("position"), second = context.Vec2("span", {16, 16});
			const double shape = context.SourceChoice("shape");
			if (shape != 0 && shape != 1)
				return context.Fail(Status::InvalidValue, "area shape cannot be represented", "shape");
			Area output{first.X, first.Y, second.X, second.Y, uint8_t(shape), 0};
			if (context.SourceChoice("type") == 1)
				output = {
					(first.X + second.X) / 2,
					(first.Y + second.Y) / 2,
					std::abs(second.X - first.X) / 2,
					std::abs(second.Y - first.Y) / 2,
					uint8_t(shape),
					0
				};
			context.SetValue("area", output);
			return context.FailureCode == Status::Ok;
		}
		struct BoolRef {
			const Value *Whole = nullptr;
			const ElementValue *Leaf = nullptr;
			const std::vector<SourceArrayItem> *General = nullptr;
			const std::vector<ElementValue> *Flat = nullptr;
			const std::vector<std::vector<ElementValue>> *Rows = nullptr;
			explicit BoolRef(const Value *value = nullptr) : Whole(value) {
				if (const auto *array = value ? std::get_if<ArrayValue>(value) : nullptr) {
					Whole = nullptr;
					if (!array->Items.empty())
						General = &array->Items;
					else if (!array->Nested.empty())
						Rows = &array->Nested;
					else
						Flat = &array->Elements;
				}
			}
			bool Array() const {
				return General || Flat || Rows;
			}
			size_t Count() const {
				return General ? General->size() : Flat ? Flat->size() : Rows ? Rows->size() : 0;
			}
			BoolRef Child(size_t index) const {
				BoolRef result;
				if (!Count()) return result;
				index %= Count();
				if (Flat)
					result.Leaf = &(*Flat)[index];
				else if (Rows)
					result.Flat = &(*Rows)[index];
				else if (const auto *leaf = std::get_if<ElementValue>(&(*General)[index].Data))
					result.Leaf = leaf;
				else if (const auto *children =
							 std::get_if<std::vector<SourceArrayItem>>(&(*General)[index].Data))
					result.General = children;
				return result;
			}
			std::optional<bool> Truth() const {
				const auto read = [](const auto &leaf) -> std::optional<bool> {
					using T = std::decay_t<decltype(leaf)>;
					if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, int64_t> ||
								  std::is_same_v<T, double>)
						return leaf != 0;
					else
						return std::nullopt;
				};
				if (Whole) return std::visit(read, *Whole);
				if (Leaf) return std::visit(read, *Leaf);
				return false;
			}
		};
		bool LogicLeaf(double mode, bool first, bool second) {
			if (mode == 0) return first && second;
			if (mode == 1) return first || second;
			if (mode == 2) return !first;
			if (mode == 3) return !(first && second);
			if (mode == 4) return !(first || second);
			if (mode == 5) return first != second;
			return false;
		}
		bool LogicNode(NodeContext &context) {
			const double mode = context.SourceChoice("type");
			if (mode < 0 || mode > 5 || std::floor(mode) != mode)
				return context.Fail(
					Status::InvalidValue, "source logic mode produces undefined output", "type"
				);
			const auto &ports = context.Authored.DynamicInputs;
			if (ports.empty())
				return context.Fail(Status::InvalidValue, "source logic requires its first input", "jname_0");
			const auto evaluate = [&](auto &&self,
									  BoolRef first,
									  BoolRef second,
									  size_t depth,
									  source_array::TreeCost &cost,
									  SourceArrayItem *output) -> bool {
				if (depth > Limits::MaximumArrayDepth || !source_array::MeasureWrapper(cost))
					return context.Fail(Status::LimitExceeded, "logic output exceeds array bounds", "result");
				if (!first.Array() && !second.Array()) {
					const auto a = first.Truth(), b = second.Truth();
					if (!a || !b)
						return context.Fail(
							Status::InvalidValue, "logic requires numeric or boolean leaves", "result"
						);
					if (output) output->Data = ElementValue{LogicLeaf(mode, *a, *b)};
					return true;
				}
				const size_t count = std::max(first.Count(), second.Count());
				source_array::Items values;
				if (output) values.reserve(count);
				for (size_t index = 0; index < count; ++index) {
					SourceArrayItem child{ElementValue{false}};
					if (!self(
							self,
							first.Array() ? first.Child(index) : first,
							second.Array() ? second.Child(index) : second,
							depth + 1,
							cost,
							output ? &child : nullptr
						))
						return false;
					if (output) values.push_back(std::move(child));
				}
				if (output) output->Data = std::move(values);
				return true;
			};
			const Value *initial = context.Find(ports[0].Id);
			if (initial && !ValidRuntimeValue(*initial))
				return context.Fail(Status::InvalidValue, "logic input payload is invalid", ports[0].Id);
			auto inputCharge =
				context.ReserveWorkspace(initial ? RetainedPayloadBytes(*initial) : 0, ports[0].Id);
			if (!inputCharge) return false;
			Value current = initial ? *initial : Value{false};
			const size_t steps = (mode == 0 || mode == 1) ? ports.size() - 1 : 1;
			for (size_t step = 0; step < steps; ++step) {
				const Value *second =
					mode == 2 || step + 1 >= ports.size() ? nullptr : context.Find(ports[step + 1].Id);
				if (second && !ValidRuntimeValue(*second))
					return context.Fail(
						Status::InvalidValue, "logic input payload is invalid", ports[step + 1].Id
					);
				source_array::TreeCost cost;
				if (!evaluate(evaluate, BoolRef(&current), BoolRef(second), 1, cost, nullptr)) return false;
				auto charge = context.ReserveWorkspace(cost.Bytes, "result");
				if (!charge) return false;
				SourceArrayItem output{ElementValue{false}};
				source_array::TreeCost used;
				if (!evaluate(evaluate, BoolRef(&current), BoolRef(second), 1, used, &output)) return false;
				if (const auto *leaf = std::get_if<ElementValue>(&output.Data))
					current = std::get<bool>(*leaf);
				else {
					ArrayValue array{ValueType::Any, {}};
					array.Items = std::move(std::get<source_array::Items>(output.Data));
					current = std::move(array);
				}
				inputCharge = std::move(charge);
			}
			context.SetValue("result", std::move(current));
			return context.FailureCode == Status::Ok;
		}
		bool StatisticNode(NodeContext &context) {
			size_t count = 0;
			for (const auto &port : context.Authored.DynamicInputs) {
				const Value *value = context.Find(port.Id);
				const auto *array = value ? std::get_if<ArrayValue>(value) : nullptr;
				count += array ? !array->Items.empty() ? array->Items.size() : array->Elements.size() : 1;
				if (count > Limits::MaximumArrayElements)
					return context.Fail(
						Status::LimitExceeded, "statistic input count exceeds bounds", port.Id
					);
			}
			auto charge = context.ReserveWorkspace(count * sizeof(double), "statistic");
			if (!charge) return false;
			std::vector<double> values;
			values.reserve(count);
			const auto append = [&](const auto &leaf, std::string_view port) {
				const auto number = std::visit(
					[](const auto &data) -> std::optional<double> {
						using T = std::decay_t<decltype(data)>;
						if constexpr (std::is_same_v<T, double> || std::is_same_v<T, int64_t> ||
									  std::is_same_v<T, bool>)
							return double(data);
						else
							return std::nullopt;
					},
					leaf
				);
				if (!number || !std::isfinite(*number))
					return context.Fail(Status::InvalidValue, "statistic requires flat numeric inputs", port);
				values.push_back(*number);
				return true;
			};
			for (const auto &port : context.Authored.DynamicInputs) {
				const Value *value = context.Find(port.Id);
				if (!value) {
					values.push_back(-1);
					continue;
				}
				if (const auto *array = std::get_if<ArrayValue>(value)) {
					if (!array->Nested.empty())
						return context.Fail(
							Status::InvalidValue, "source statistic does not flatten nested rows", port.Id
						);
					if (!array->Items.empty())
						for (const auto &item : array->Items) {
							const auto *leaf = std::get_if<ElementValue>(&item.Data);
							if (!leaf || !append(*leaf, port.Id))
								return context.Fail(
									Status::InvalidValue, "statistic requires flat numeric inputs", port.Id
								);
						}
					else
						for (const auto &leaf : array->Elements)
							if (!append(leaf, port.Id)) return false;
				} else if (!append(*value, port.Id))
					return false;
			}
			const double mode = context.SourceChoice("type");
			double output = 0;
			if (mode == 0 || mode == 1) {
				for (double value : values)
					output += value;
				if (mode == 1 && !values.empty()) output /= double(values.size());
			} else if (mode == 2 && !values.empty()) {
				std::sort(values.begin(), values.end());
				const size_t middle = values.size() / 2;
				output = values.size() % 2 ? values[middle] : (values[middle - 1] + values[middle]) / 2;
			} else if (mode == 3 || mode == 4) {
				if (values.empty())
					return context.Fail(
						Status::InvalidValue, "source min/max requires nonempty inputs", "statistic"
					);
				output = mode == 3 ? *std::max_element(values.begin(), values.end())
								   : *std::min_element(values.begin(), values.end());
			}
			if (!std::isfinite(output))
				return context.Fail(Status::InvalidValue, "statistic result is nonfinite", "statistic");
			context.SetValue("statistic", output);
			return context.FailureCode == Status::Ok;
		}
		std::string LeadingZero(double number) {
			std::array<char, 512> buffer{};
			const auto result = std::to_chars(
				buffer.data(), buffer.data() + buffer.size(), number, std::chars_format::fixed, 0
			);
			if (result.ec != std::errc{}) return {};
			std::string text(buffer.data(), result.ptr);
			if (text.size() < 2) text.insert(text.begin(), '0');
			return text;
		}
		bool SecondsNode(NodeContext &context) {
			double seconds = context.Scalar("seconds");
			if (!std::isfinite(seconds))
				return context.Fail(Status::InvalidValue, "seconds must be finite", "seconds");
			const double days = std::floor(seconds / 86400);
			seconds -= days * 86400;
			const double hours = std::floor(seconds / 3600);
			seconds -= hours * 3600;
			const double minutes = std::floor(seconds / 60);
			seconds -= minutes * 60;
			const double whole = std::floor(seconds);
			const Value *value = context.Find("format");
			const auto *format = value ? std::get_if<std::string>(value) : nullptr;
			const std::string_view source = format ? std::string_view(*format) : std::string_view("%h:%n:%s");
			std::array<std::pair<std::string_view, std::string>, 4> replacements{
				{{"%d", LeadingZero(days)},
				 {"%h", LeadingZero(hours)},
				 {"%n", LeadingZero(minutes)},
				 {"%s", LeadingZero(whole)}}
			};
			size_t length = source.size();
			for (const auto &[token, text] : replacements)
				if (source.find(token) != std::string_view::npos) length += text.size() - token.size();
			const bool decimal = context.Boolean("seconds_decimal", true);
			if (decimal) length += 3;
			if (length > Limits::MaximumTextBytes)
				return context.Fail(Status::LimitExceeded, "seconds format exceeds text budget", "format");
			auto charge = context.ReserveWorkspace(length + source.size() + 32, "format");
			if (!charge) return false;
			std::string text(source);
			for (const auto &[token, replacement] : replacements)
				if (const auto position = text.find(token); position != std::string::npos)
					text.replace(position, token.size(), replacement);
			if (decimal) {
				text.push_back('.');
				text += LeadingZero(std::floor((seconds - std::trunc(seconds)) * 100));
			}
			context.SetValue("format_string", std::move(text));
			context.SetValue("seconds", seconds);
			context.SetValue("minutes", minutes);
			context.SetValue("hours", hours);
			context.SetValue("days", days);
			return context.FailureCode == Status::Ok;
		}
	}
	std::span<const ExecutorEntry> SourceDataExecutors() {
		static constexpr std::array entries{
			ExecutorEntry{"pc.path_separate_folder", SeparateFilePath},
			ExecutorEntry{"pc.trigger_bool", StatefulDataNode},
			ExecutorEntry{"pc.differential", StatefulDataNode},
			ExecutorEntry{"pc.buffer_to_string", BufferTextNode},
			ExecutorEntry{"pc.number_text_format", NumberTextNode},
			ExecutorEntry{"pc.area", AreaNode},
			ExecutorEntry{"pc.logic", LogicNode},
			ExecutorEntry{"pc.statistic", StatisticNode},
			ExecutorEntry{"pc.sec_convert", SecondsNode}
		};
		return entries;
	}
}
