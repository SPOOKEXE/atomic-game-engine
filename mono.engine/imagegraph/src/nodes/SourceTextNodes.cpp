#include "../TextOps.hpp"
#include "../Utf8TextOps.hpp"
#include "ArraySource.hpp"
#include "BoundedRegex.hpp"
#include "Families.hpp"
#include "SourceJson.hpp"

#include <cmath>

namespace engine::imagegraph::detail {
	namespace {
		std::string_view Text(NodeContext &context, std::string_view port) {
			const Value *value = context.Find(port);
			const auto *text = value ? std::get_if<std::string>(value) : nullptr;
			return text ? std::string_view(*text) : std::string_view{};
		}
		bool ValidText(NodeContext &context, std::string_view text, std::string_view port) {
			size_t count = 0;
			const auto status = CountText(text, count);
			return status == TextOpStatus::Ok ||
				   context.Fail(
					   status == TextOpStatus::LimitExceeded ? Status::LimitExceeded : Status::InvalidValue,
					   "text must be bounded UTF-8",
					   port
				   );
		}
		bool Publish(NodeContext &context, std::string_view text) {
			if (!ValidText(context, text, "text") ||
				!context.ReserveOutput(std::max(text.size(), std::string{}.capacity()), "text"))
				return false;
			context.SetValue("text", std::string(text));
			return context.FailureCode == Status::Ok;
		}
		bool Insert(NodeContext &context) {
			const auto text = Text(context, "text"), added = Text(context, "insert_text");
			if (!ValidText(context, text, "text") || !ValidText(context, added, "insert_text")) return false;
			if (added.size() > Limits::MaximumTextBytes - text.size())
				return context.Fail(Status::LimitExceeded, "inserted text exceeds byte limit");
			size_t characters = 0;
			CountText(text, characters);
			const auto position = context.Integer("position");
			const size_t offset =
				ByteOffset(text, position <= 1 ? 0 : std::min(characters, size_t(position - 1)));
			if (!context.ReserveOutput(
					std::max(text.size() + added.size(), std::string{}.capacity()), "text"
				))
				return false;
			std::string result(text.size() + added.size(), '\0');
			auto destination = std::copy(text.begin(), text.begin() + offset, result.begin());
			destination = std::copy(added.begin(), added.end(), destination);
			std::copy(text.begin() + offset, text.end(), destination);
			context.SetValue("text", std::move(result));
			return context.FailureCode == Status::Ok;
		}
		bool Join(NodeContext &context) {
			const auto *input = context.Find("text_array");
			const auto *array = input ? std::get_if<ArrayValue>(input) : nullptr;
			const auto divider = Text(context, "divider");
			if (!ValidText(context, divider, "divider")) return false;
			if (!array) return Publish(context, {});
			if (!array->Items.empty() || !array->Nested.empty() || array->ElementType != ValueType::Text)
				return context.Fail(Status::InvalidValue, "join requires a flat text array", "text_array");
			size_t bytes = 0;
			for (size_t i = 0; i < array->Elements.size(); ++i) {
				const auto *piece = std::get_if<std::string>(&array->Elements[i]);
				if (!piece || !ValidText(context, *piece, "text_array")) return false;
				const size_t gap = i ? divider.size() : 0;
				if (gap > Limits::MaximumTextBytes - bytes ||
					piece->size() > Limits::MaximumTextBytes - bytes - gap)
					return context.Fail(Status::LimitExceeded, "joined text exceeds byte limit");
				bytes += gap + piece->size();
			}
			if (!context.ReserveOutput(std::max(bytes, std::string{}.capacity()), "text")) return false;
			std::string result(bytes, '\0');
			auto destination = result.begin();
			for (size_t i = 0; i < array->Elements.size(); ++i) {
				if (i) destination = std::copy(divider.begin(), divider.end(), destination);
				const auto &piece = std::get<std::string>(array->Elements[i]);
				destination = std::copy(piece.begin(), piece.end(), destination);
			}
			context.SetValue("text", std::move(result));
			return context.FailureCode == Status::Ok;
		}
		bool White(char c) {
			return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
		}
		std::string_view TrimEnd(std::string_view text) {
			while (!text.empty() && White(text.back()))
				text.remove_suffix(1);
			return text;
		}
		bool Split(NodeContext &context) {
			const auto text = Text(context, "text");
			const auto rawDelimiter = Text(context, "delimiter");
			if (!ValidText(context, text, "text") || !ValidText(context, rawDelimiter, "delimiter"))
				return false;
			auto scratch = context.ReserveWorkspace(
				2 * (rawDelimiter.size() + std::string{}.capacity()) +
					std::min(text.size() + 1, Limits::MaximumArrayElements) * sizeof(std::string_view),
				"delimiter"
			);
			if (!scratch) return false;
			std::string delimiter(rawDelimiter);
			for (const auto &[from, to] :
				 {std::pair{std::string_view{"\\n"}, std::string_view{"\n"}},
				  std::pair{std::string_view{"\\t"}, std::string_view{"\t"}}}) {
				auto replacement = ReplaceText(delimiter, from, to, true, Limits::MaximumTextBytes);
				if (!replacement) return false;
				delimiter = std::move(*replacement);
			}
			std::vector<std::string_view> pieces;
			pieces.reserve(std::min(text.size() + 1, Limits::MaximumArrayElements));
			const auto append = [&](std::string_view part) {
				if (pieces.size() == Limits::MaximumArrayElements) return false;
				pieces.push_back(part);
				return true;
			};
			const auto mode = context.Integer("mode");
			if (delimiter.empty() || mode == 1) {
				const size_t period =
					delimiter.empty() ? 1 : size_t(std::max<int64_t>(1, context.Integer("period", 1)));
				size_t offset = 0;
				while (offset < text.size()) {
					const size_t start = offset;
					for (size_t c = 0; c < period && offset < text.size(); ++c)
						NextUtf8(text, offset);
					if (!append(text.substr(start, offset - start)))
						return context.Fail(Status::LimitExceeded, "split exceeds element limit");
				}
			} else if (mode == 0) {
				size_t offset = 0;
				while (true) {
					const size_t match = text.find(delimiter, offset);
					auto part = text.substr(offset, match == std::string_view::npos ? match : match - offset);
					if (context.Boolean("trim_white_space")) {
						part = TrimEnd(part);
						while (part.ends_with(delimiter))
							part.remove_suffix(delimiter.size());
					}
					if (!append(part))
						return context.Fail(Status::LimitExceeded, "split exceeds element limit");
					if (match == std::string_view::npos) break;
					offset = match + delimiter.size();
				}
			} else
				return context.Fail(Status::InvalidValue, "split mode is invalid", "mode");
			uint64_t bytes = pieces.size() * sizeof(ElementValue);
			for (auto piece : pieces)
				bytes += std::max(piece.size(), std::string{}.capacity());
			if (!context.ReserveOutput(bytes, "text")) return false;
			ArrayValue result{ValueType::Text, {}};
			result.Elements.reserve(pieces.size());
			for (auto piece : pieces)
				result.Elements.emplace_back(std::string(piece));
			context.SetValue("text", std::move(result));
			return context.FailureCode == Status::Ok;
		}
		bool Trim(NodeContext &context) {
			const auto text = Text(context, "text");
			if (!ValidText(context, text, "text")) return false;
			const auto trim = context.Integer("trim"), mode = context.Integer("mode");
			if (trim == 2) {
				auto part = TrimEnd(text);
				while (!part.empty() && White(part.front()))
					part.remove_prefix(1);
				const auto token = Text(context, "text_2");
				if (!token.empty()) {
					while (part.starts_with(token))
						part.remove_prefix(token.size());
					while (part.ends_with(token))
						part.remove_suffix(token.size());
				}
				return Publish(context, part);
			}
			if (trim < 0 || trim > 1 || mode < 0 || mode > 1)
				return context.Fail(Status::InvalidValue, "trim mode is invalid");
			const double head = std::max(0., context.Scalar("head")),
						 tail = std::max(0., context.Scalar("tail"));
			size_t length = 0;
			CountText(text, length);
			if (trim == 0) {
				const double h = mode ? std::nearbyint(head * double(length)) : std::trunc(head),
							 t = mode ? std::nearbyint(tail * double(length)) : std::trunc(tail);
				if (!std::isfinite(h) || !std::isfinite(t) || h >= double(length) || t >= double(length) - h)
					return Publish(context, {});
				const size_t start = ByteOffset(text, size_t(h)), end = ByteOffset(text, length - size_t(t));
				return Publish(context, text.substr(start, end - start));
			}
			const size_t count = 1 + size_t(std::count(text.begin(), text.end(), ' '));
			const double h = mode ? head * double(count) : head, t = mode ? tail * double(count) : tail;
			if (!std::isfinite(h) || !std::isfinite(t) || h >= double(count) || t >= double(count) - h)
				return Publish(context, {});
			const size_t first = size_t(h), last = size_t(std::ceil(double(count) - t));
			size_t word = 0, start = 0, end = text.size();
			for (size_t i = 0; i < text.size(); ++i)
				if (text[i] == ' ') {
					++word;
					if (word == first) start = i + 1;
					if (word == last) {
						end = i;
						break;
					}
				}
			return Publish(context, text.substr(start, end - start));
		}
		bool Format(NodeContext &context) {
			const auto source = Text(context, "text");
			if (!ValidText(context, source, "text")) return false;
			auto scratch =
				context.ReserveWorkspace(2 * (Limits::MaximumTextBytes + std::string{}.capacity()), "text");
			if (!scratch) return false;
			std::string result(source);
			std::vector<std::pair<size_t, std::string_view>> keys;
			for (const auto &input : context.Authored.DynamicInputs) {
				size_t group = 0;
				const auto *item = FindDynamicTemplate(context.Entry, input.Id, group);
				if (item && item->Id == "key_in_amo") keys.emplace_back(group, input.Id);
			}
			std::sort(keys.begin(), keys.end());
			for (const auto &[group, port] : keys) {
				const auto key = Text(context, port);
				std::string marker = "{" + std::string(key) + "}";
				const Value *value = nullptr;
				for (const auto &input : context.Authored.DynamicInputs) {
					size_t candidate = 0;
					const auto *item = FindDynamicTemplate(context.Entry, input.Id, candidate);
					if (item && candidate == group && item->Id == "value") value = context.Find(input.Id);
				}
				const auto *replacement = value ? std::get_if<std::string>(value) : nullptr;
				const auto replaced = ReplaceText(
					result,
					marker,
					replacement ? std::string_view(*replacement) : std::string_view{},
					true,
					Limits::MaximumTextBytes
				);
				if (!replaced)
					return context.Fail(Status::LimitExceeded, "formatted text exceeds byte limit");
				result = *replaced;
			}
			return Publish(context, result);
		}
		bool StructPublish(NodeContext &context, StructValue value) {
			if (!ValidStructPayload(value))
				return context.Fail(Status::LimitExceeded, "struct exceeds payload limits");
			if (!context.ReserveOutput(RetainedPayloadBytes(value), "struct")) return false;
			context.SetValue("struct", std::move(value));
			return context.FailureCode == Status::Ok;
		}
		bool StructSet(NodeContext &context) {
			const auto *input = context.Find("struct");
			const auto *original = input ? std::get_if<StructValue>(input) : nullptr;
			if (original && !ValidStructPayload(*original))
				return context.Fail(Status::InvalidValue, "struct input is invalid");
			const auto key = Text(context, "key");
			const auto *replacement = context.Find("value");
			const auto *surface = context.Input("value");
			auto resourceReservation =
				context.ReserveWorkspace(surface ? surface->Pixels.size() : 0, "value");
			if (!resourceReservation) return false;
			std::optional<Value> resource;
			if (surface) {
				resource = SurfaceValue{*surface};
				replacement = &*resource;
			}
			if (replacement && !ValidRuntimeValue(*replacement))
				return context.Fail(Status::InvalidValue, "struct replacement is invalid", "value");
			const uint64_t bytes =
				(original ? RetainedPayloadBytes(*original) : 0) + sizeof(StructData) +
				sizeof(std::pair<std::string, Value>) + key.size() +
				(replacement ? std::visit([](const auto &v) { return RetainedPayloadBytes(v); }, *replacement)
							 : 0);
			auto scratch = context.ReserveWorkspace(bytes, "struct");
			if (!scratch) return false;
			StructValue result = original ? *original : StructValue{};
			if (!key.empty()) {
				if (!result.Data) result.Data.emplace();
				auto &fields = result.Data->Fields;
				auto found = std::find_if(fields.begin(), fields.end(), [&](const auto &entry) {
					return entry.first == key;
				});
				if (found == fields.end())
					fields.emplace_back(std::string(key), replacement ? *replacement : Value{double{0}});
				else
					found->second = replacement ? *replacement : Value{double{0}};
			}
			return StructPublish(context, std::move(result));
		}
		bool StructCreate(NodeContext &context) {
			uint64_t bytes = sizeof(StructData);
			for (const auto &input : context.Authored.DynamicInputs) {
				const auto *value = context.Find(input.Id);
				const auto *surface = context.Input(input.Id);
				if (surface) bytes += surface->Pixels.size();
				if (value && !ValidRuntimeValue(*value))
					return context.Fail(Status::InvalidValue, "struct field payload is invalid", input.Id);
				bytes +=
					sizeof(std::pair<std::string, Value>) +
					(value ? std::visit([](const auto &v) { return RetainedPayloadBytes(v); }, *value) : 0);
			}

			auto scratch = context.ReserveWorkspace(bytes, "struct");
			if (!scratch) return false;
			StructValue result;
			auto &fields = result.Data.emplace().Fields;
			for (const auto &input : context.Authored.DynamicInputs) {
				size_t group = 0;
				const auto *item = FindDynamicTemplate(context.Entry, input.Id, group);
				if (!item || item->Id != "key") continue;
				const auto key = Text(context, input.Id);
				if (key.empty()) continue;
				const Value *value = nullptr;
				const Image *surface = nullptr;
				for (const auto &other : context.Authored.DynamicInputs) {
					size_t candidate = 0;
					const auto *part = FindDynamicTemplate(context.Entry, other.Id, candidate);
					if (part && candidate == group && part->Id == "value") {
						value = context.Find(other.Id);
						surface = context.Input(other.Id);
					}
				}
				std::optional<Value> resource;
				if (surface) {
					resource = SurfaceValue{*surface};
					value = &*resource;
				}
				auto found = std::find_if(fields.begin(), fields.end(), [&](const auto &field) {
					return field.first == key;
				});
				if (found == fields.end())
					fields.emplace_back(std::string(key), value ? *value : Value{double{0}});
				else
					found->second = value ? *value : Value{double{0}};
			}
			return StructPublish(context, std::move(result));
		}
		const Value *StructLookup(const StructValue &object, std::string_view path) {
			const StructValue *current = &object;
			while (true) {
				if (!current->Data) return nullptr;
				const size_t dot = path.find('.');
				const auto key = path.substr(0, dot);
				const auto &fields = current->Data->Fields;
				auto found = std::find_if(fields.begin(), fields.end(), [&](const auto &entry) {
					return entry.first == key;
				});
				if (found == fields.end()) return nullptr;
				if (dot == std::string_view::npos) return &found->second;
				current = std::get_if<StructValue>(&found->second);
				if (!current) return nullptr;
				path.remove_prefix(dot + 1);
			}
		}

		bool StructDomain(NodeContext &context, const Value *value) {
			SourceSocketDomain domain{ValueType::Any, std::nullopt, SourceSocketKind::Any};
			if (value) {
				if (std::holds_alternative<UndefinedValue>(*value))
					domain = {ValueType::Any, std::nullopt, SourceSocketKind::Any};
				else if (std::holds_alternative<SurfaceValue>(*value))
					domain = {ValueType::Image, std::nullopt, SourceSocketKind::Surface};
				else if (std::holds_alternative<BufferValue>(*value))
					domain = {ValueType::Buffer, std::nullopt, SourceSocketKind::Object};
				else if (std::holds_alternative<StructValue>(*value))
					domain = {ValueType::Struct, std::nullopt, SourceSocketKind::Struct};
				else if (std::holds_alternative<Gradient>(*value))
					domain = {ValueType::Gradient, std::nullopt, SourceSocketKind::Gradient};
				else if (std::holds_alternative<std::string>(*value))
					domain = {ValueType::Text, std::nullopt, SourceSocketKind::Text};
				else if (const auto *array = std::get_if<ArrayValue>(value)) {
					bool text = !array->Elements.empty() &&
								std::holds_alternative<std::string>(array->Elements.front());
					if (!array->Items.empty()) {
						const auto *leaf = std::get_if<ElementValue>(&array->Items.front().Data);
						text = leaf && std::holds_alternative<std::string>(*leaf);
					}
					domain =
						text ? SourceSocketDomain{ValueType::Text, std::nullopt, SourceSocketKind::Text}
							 : SourceSocketDomain{ValueType::Scalar, std::nullopt, SourceSocketKind::Float};
				} else
					domain = {ValueType::Scalar, std::nullopt, SourceSocketKind::Float};
			}
			switch (context.Integer("type")) {
			case 0:
				break;
			case 1:
				domain = {ValueType::Scalar, std::nullopt, SourceSocketKind::Float};
				break;
			case 2:
				domain = {ValueType::Text, std::nullopt, SourceSocketKind::Text};
				break;
			case 3:
				domain = {ValueType::Image, std::nullopt, SourceSocketKind::Surface};
				break;
			case 4:
				domain = {ValueType::Buffer, std::nullopt, SourceSocketKind::Object};
				break;
			case 5:
				domain = {ValueType::Struct, std::nullopt, SourceSocketKind::Struct};
				break;
			case 6:
				domain = {ValueType::Gradient, std::nullopt, SourceSocketKind::Gradient};
				break;
			default:
				return context.Fail(Status::InvalidValue, "struct output type is invalid", "type");
			}
			return context.SetOutputDomain("value", domain);
		}
		bool StructGet(NodeContext &context) {
			const auto *input = context.Find("struct");
			if (input && !ValidRuntimeValue(*input))
				return context.Fail(Status::InvalidValue, "struct input is invalid");
			const auto key = Text(context, "key");
			if (const auto *array = input ? std::get_if<ArrayValue>(input) : nullptr) {
				auto scratch = context.ReserveWorkspace(
					RetainedPayloadBytes(*array) + array->Elements.size() * sizeof(SourceArrayItem), "struct"
				);
				if (!scratch) return false;
				const auto source = source_array::FromValues(*array);
				source_array::Items result;
				if (!StructDomain(context, nullptr)) return false;
				for (const auto &entry : source) {
					const auto *leaf = std::get_if<ElementValue>(&entry.Data);
					const auto *object = leaf ? std::get_if<StructValue>(leaf) : nullptr;
					const auto *value = object ? StructLookup(*object, key) : nullptr;
					if (value && std::holds_alternative<UndefinedValue>(*value)) value = nullptr;
					if (!StructDomain(context, value)) return false;
					if (const auto *nested = value ? std::get_if<ArrayValue>(value) : nullptr)
						result.push_back({source_array::FromValues(*nested)});
					else {
						const auto converted = value ? ArrayElement(*value)
													 : std::optional<ElementValue>{ElementValue{double{0}}};
						result.push_back({converted ? *converted : ElementValue{double{0}}});
					}
				}
				return source_array::Publish(context, std::move(result), "value", ValueType::Any, false);
			}
			const auto *object = input ? std::get_if<StructValue>(input) : nullptr;
			auto *value = object ? StructLookup(*object, key) : nullptr;
			if (value && std::holds_alternative<UndefinedValue>(*value)) value = nullptr;
			if (!StructDomain(context, value)) return false;
			const Value zero{double{0}};
			const Value &result = value ? *value : zero;
			if (!context.ReserveOutput(
					std::visit([](const auto &v) { return RetainedPayloadBytes(v); }, result), "value"
				))
				return false;
			if (const auto *surface = std::get_if<SurfaceValue>(&result)) {
				auto *image = context.NewImage(
					"value", surface->Data.Width, surface->Data.Height, surface->Data.Format
				);
				if (!image) return false;
				std::copy(surface->Data.Pixels.begin(), surface->Data.Pixels.end(), image->Pixels.begin());
				image->Hash = SurfaceHash(*image);
			} else
				context.SetValue("value", result);
			return context.FailureCode == Status::Ok;
		}

		bool JsonParse(NodeContext &context) {
			const auto text = Text(context, "json_string");
			if (!ValidText(context, text, "json_string")) return false;
			size_t structural = 1;
			bool quoted = false, escaped = false;
			for (char character : text) {
				if (quoted) {
					if (escaped)
						escaped = false;
					else if (character == '\\')
						escaped = true;
					else if (character == '"')
						quoted = false;
				} else if (character == '"')
					quoted = true;
				else if (character == '{' || character == '[' || character == ',' || character == ':')
					++structural;
			}
			const uint64_t bytes =
				4 * text.size() +
				2 * structural *
					(sizeof(SourceArrayItem) + sizeof(std::pair<std::string, Value>) + sizeof(StructData));
			auto scratch = context.ReserveWorkspace(bytes, "json_string");
			if (!scratch) return false;
			JsonCursor cursor{text};
			Value parsed;
			const bool ok = cursor.Parse(parsed);
			cursor.Space();
			if (cursor.Limit)
				return context.Fail(
					Status::LimitExceeded, "JSON tree exceeds depth or element limits", "json_string"
				);
			if (!ok || cursor.Offset != text.size()) return StructPublish(context, {});
			if (!ValidRuntimeValue(parsed))
				return context.Fail(Status::LimitExceeded, "JSON payload exceeds limits", "json_string");
			if (!context.ReserveOutput(
					std::visit([](const auto &v) { return RetainedPayloadBytes(v); }, parsed), "struct"
				))
				return false;
			context.SetValue("struct", std::move(parsed));
			return context.FailureCode == Status::Ok;
		}

		bool Csv(NodeContext &context) {
			const auto *input = context.Find("csv_string");
			const auto *lines = input ? std::get_if<ArrayValue>(input) : nullptr;
			const auto raw = Text(context, "csv_string");
			if (!lines && !ValidText(context, raw, "csv_string")) return false;
			uint64_t bytes = raw.size() * (sizeof(ElementValue) + sizeof(std::pair<std::string, Value>) + 4);
			if (lines) bytes += RetainedPayloadBytes(*lines) * 4;
			auto scratch = context.ReserveWorkspace(bytes + sizeof(StructData), "csv_string");
			if (!scratch) return false;
			std::vector<std::string_view> source;
			if (lines) {
				if (lines->ElementType != ValueType::Text || !lines->Nested.empty() || !lines->Items.empty())
					return context.Fail(Status::InvalidValue, "CSV requires flat text lines", "csv_string");
				for (const auto &leaf : lines->Elements) {
					const auto *line = std::get_if<std::string>(&leaf);
					if (!line) return false;
					source.emplace_back(*line);
				}
			} else {
				size_t offset = 0;
				while (true) {
					const size_t end = raw.find('\n', offset);
					source.push_back(raw.substr(offset, end == std::string_view::npos ? end : end - offset));
					if (end == std::string_view::npos) break;
					offset = end + 1;
				}
			}
			const auto textList = [&](std::string_view port) {
				std::vector<std::string_view> result;
				const auto *value = context.Find(port);
				const auto *array = value ? std::get_if<ArrayValue>(value) : nullptr;
				if (array)
					for (const auto &leaf : array->Elements)
						if (const auto *text = std::get_if<std::string>(&leaf)) result.push_back(*text);
				return result;
			};
			const auto numeric = textList("number_columns"), sort = textList("sort"),
					   columns = textList("columns");
			std::vector<std::string> header;
			std::vector<StructValue> rows;
			for (size_t i = size_t(std::max<int64_t>(0, context.Integer("skip_line"))); i < source.size();
				 ++i) {
				std::string line;
				for (char c : source[i])
					if (c != '"') line += c;
				auto view = TrimEnd(line);
				while (!view.empty() && White(view.front()))
					view.remove_prefix(1);
				const auto parts = SplitText(view, ",", Limits::MaximumArrayElements);
				if (!parts) return context.Fail(Status::LimitExceeded, "CSV row exceeds element limit");
				if (header.empty() && context.Boolean("first_row_header")) {
					header = *parts;
					continue;
				}
				if (header.empty()) continue;
				StructValue row;
				auto &fields = row.Data.emplace().Fields;
				for (size_t j = 0; j < std::min(header.size(), parts->size()); ++j) {
					Value value = (*parts)[j];
					if (std::find(numeric.begin(), numeric.end(), header[j]) != numeric.end()) {
						double number = 0;
						auto part = TrimEnd(std::string_view((*parts)[j]));
						while (!part.empty() && White(part.front()))
							part.remove_prefix(1);
						if (!part.empty() && part.front() == '+') part.remove_prefix(1);
						const auto parse = std::from_chars(part.data(), part.data() + part.size(), number);
						if (parse.ec != std::errc{} || parse.ptr != part.data() + part.size() ||
							!std::isfinite(number))
							number = 0;
						value = number;
					}
					fields.emplace_back(header[j], std::move(value));
				}
				rows.push_back(std::move(row));
				if (rows.size() > Limits::MaximumArrayElements)
					return context.Fail(Status::LimitExceeded, "CSV exceeds row limit");
			}
			std::stable_sort(rows.begin(), rows.end(), [&](const auto &a, const auto &b) {
				for (auto order : sort) {
					if (order.empty()) continue;
					const bool ascending = order.front() == '+';
					order.remove_prefix(1);
					const auto *av = StructLookup(a, order), *bv = StructLookup(b, order);
					if (!av || !bv) continue;
					int compare = 0;
					if (const auto *at = std::get_if<std::string>(av)) {
						if (const auto *bt = std::get_if<std::string>(bv)) compare = at->compare(*bt);
					} else if (const auto *an = std::get_if<double>(av)) {
						if (const auto *bn = std::get_if<double>(bv))
							compare = *an < *bn ? -1 : *an > *bn ? 1 : 0;
					}
					if (compare) return ascending ? compare < 0 : compare > 0;
				}
				return false;
			});
			if (context.Boolean("output_struct") && !columns.empty())
				return context.Fail(
					Status::UnsupportedExecution,
					"source CSV struct column selection references unbound outCol",
					"columns"
				);
			source_array::Items result;
			for (auto &row : rows) {
				if (context.Boolean("output_struct"))
					result.push_back({ElementValue{std::move(row)}});
				else {
					source_array::Items cells;
					const auto add = [&](std::string_view key) {
						const auto *field = StructLookup(row, key);
						if (field) {
							auto leaf = ArrayElement(*field);
							if (leaf) cells.push_back({std::move(*leaf)});
						} else
							cells.push_back({ElementValue{double{0}}});
					};
					if (columns.empty())
						for (const auto &column : header)
							add(column);
					else
						for (auto column : columns)
							add(column);
					result.push_back({std::move(cells)});
				}
			}
			return source_array::Publish(context, std::move(result), "array", ValueType::Any, false);
		}

		bool Regex(NodeContext &context) {
			auto text = Text(context, "text"), pattern = Text(context, "regex"),
				 replacement = Text(context, "replacement");
			if (!ValidText(context, text, "text") || !ValidText(context, pattern, "regex") ||
				!ValidText(context, replacement, "replacement"))
				return false;

			const auto type = context.Authored.Type;
			if (text.empty() || pattern.empty()) {
				if (type == "pc.string_regex_replace")
					context.SetValue("results", std::string{});
				else
					context.SetValue("results", false);
				return context.FailureCode == Status::Ok;
			}
			// The source DLL consumes NUL-terminated byte strings.
			const auto prefix = [](std::string_view value) { return value.substr(0, value.find('\0')); };
			text = prefix(text);
			pattern = prefix(pattern);
			replacement = prefix(replacement);
			auto workspace = context.ReserveWorkspace(
				bounded_regex::MAXIMUM_STATES * (6 * sizeof(bounded_regex::State) + 128) +
					bounded_regex::MAXIMUM_NODES * (sizeof(bounded_regex::Node) + 32) +
					2 * Limits::MaximumTextBytes,
				"regex"
			);
			if (!workspace) return false;
			bounded_regex::Pattern compiled{pattern, {}};
			if (!compiled.Compile())
				return context.Fail(
					compiled.Limited ? Status::LimitExceeded : Status::InvalidValue,
					"regex pattern is invalid or exceeds the parser budget",
					"regex"
				);
			bounded_regex::Machine machine{compiled, text};
			bounded_regex::State match;
			bool found = machine.Search(0, match);
			if (machine.Limited)
				return context.Fail(
					Status::LimitExceeded, "regex exceeds the state or instruction budget", "regex"
				);
			if (type == "pc.string_regex_match") {
				context.SetValue("results", found);
				return context.FailureCode == Status::Ok;
			}
			if (type == "pc.string_regex_search") {
				source_array::Items captures;
				if (found) {
					std::string joined;
					for (size_t i = 0; i <= compiled.Captures; ++i) {
						if (i) joined += '\n';
						const auto capture = match.Captures[i];
						if (capture.Matched)
							joined += text.substr(capture.Start, capture.End - capture.Start);
						if (joined.size() > Limits::MaximumTextBytes)
							return context.Fail(Status::LimitExceeded, "regex captures exceed text limit");
					}
					auto pieces = SplitText(joined, "\n", Limits::MaximumArrayElements);
					if (!pieces)
						return context.Fail(Status::LimitExceeded, "regex captures exceed array limit");
					for (auto &piece : *pieces)
						captures.push_back({ElementValue{std::move(piece)}});
				}
				return source_array::Publish(context, std::move(captures), "results", ValueType::Text, false);
			}
			std::string output;
			output.reserve(Limits::MaximumTextBytes);
			const auto append = [&](std::string_view value) {
				if (value.size() > Limits::MaximumTextBytes - output.size()) return false;
				output.append(value);
				return true;
			};
			const auto expand = [&](const bounded_regex::State &state) {
				for (size_t i = 0; i < replacement.size(); ++i) {
					if (replacement[i] != '$' || i + 1 == replacement.size()) {
						if (!append(replacement.substr(i, 1))) return false;
						continue;
					}
					const char next = replacement[i + 1];
					if (next == '$') {
						if (!append("$")) return false;
						++i;
					} else if (next == '&') {
						const auto c = state.Captures[0];
						if (!append(text.substr(c.Start, c.End - c.Start))) return false;
						++i;
					} else if (next == '`') {
						if (!append(text.substr(0, state.Captures[0].Start))) return false;
						++i;
					} else if (next == '\'') {
						if (!append(text.substr(state.Captures[0].End))) return false;
						++i;
					} else if (next >= '0' && next <= '9') {
						size_t group = size_t(next - '0');
						++i;
						if (i + 1 < replacement.size() && replacement[i + 1] >= '0' &&
							replacement[i + 1] <= '9') {
							group = group * 10 + size_t(replacement[++i] - '0');
						}
						if (group <= compiled.Captures) {
							const auto c = state.Captures[group];
							if (c.Matched && !append(text.substr(c.Start, c.End - c.Start))) return false;
						}
					} else if (!append("$"))
						return false;
				}
				return true;
			};
			size_t copied = 0;
			while (found) {
				const auto whole = match.Captures[0];
				if (!append(text.substr(copied, whole.Start - copied)) || !expand(match))
					return context.Fail(Status::LimitExceeded, "regex replacement exceeds text limit");
				copied = whole.End;
				if (whole.Start == whole.End) {
					if (machine.At(whole.End, match, true))
						found = true;
					else if (whole.End == text.size())
						found = false;
					else
						found = machine.Search(whole.End + 1, match);
				} else
					found = machine.Search(whole.End, match);
				if (machine.Limited)
					return context.Fail(
						Status::LimitExceeded, "regex exceeds the state or instruction budget", "regex"
					);
			}
			if (!append(text.substr(copied)))
				return context.Fail(Status::LimitExceeded, "regex replacement exceeds text limit");
			if (!context.ReserveOutput(output.capacity(), "results")) return false;
			context.SetValue("results", std::move(output));
			return context.FailureCode == Status::Ok;
		}

	}
	std::span<const ExecutorEntry> SourceTextExecutors() {
		static constexpr ExecutorEntry entries[] = {
			{"pc.string_insert", Insert},
			{"pc.string_join", Join},
			{"pc.string_split", Split},
			{"pc.string_trim", Trim},
			{"pc.string_format", Format},
			{"pc.struct", StructCreate, true},
			{"pc.struct_set", StructSet, true},
			{"pc.struct_get", StructGet, true},
			{"pc.struct_json_parse", JsonParse},
			{"pc.array_csv_parse", Csv},
			{"pc.string_regex_match", Regex},
			{"pc.string_regex_search", Regex},
			{"pc.string_regex_replace", Regex}
		};
		return entries;
	}
}
