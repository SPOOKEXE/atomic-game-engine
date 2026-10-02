#include "GraphTextFileHost.hpp"

#include "GraphCsvWrite.hpp"
#include "GraphFileHostIO.hpp"

#include <engine/bake/ComposerXml.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
namespace engine::imagegraphexport {
	namespace {
		using namespace engine::imagegraph;
		const Value *Input(const HostNodeInvocation &call, std::string_view port) {
			const auto i = std::find_if(call.Inputs.begin(), call.Inputs.end(), [&](const auto &v) {
				return v.Port == port;
			});
			return i == call.Inputs.end() ? nullptr : &i->Data;
		}
		template <class T> const T *Get(const HostNodeInvocation &call, std::string_view port) {
			const auto *v = Input(call, port);
			return v ? std::get_if<T>(v) : nullptr;
		}
		std::string Scalar(const Value &value, bool &valid) {
			if (const auto *v = std::get_if<std::string>(&value)) return *v;
			if (const auto *v = std::get_if<bool>(&value)) return *v ? "1" : "0";
			std::ostringstream text;
			text.imbue(std::locale::classic());
			text << std::setprecision(15);
			if (const auto *v = std::get_if<int64_t>(&value))
				text << *v;
			else if (const auto *v = std::get_if<double>(&value)) {
				if (!std::isfinite(*v)) {
					valid = false;
					return {};
				}
				text << *v;
			} else {
				valid = false;
				return {};
			}
			return text.str();
		}
		bool Append(std::string &text, std::string_view value, uint64_t maximum) {
			if (value.size() > maximum - text.size()) return false;
			text += value;
			return true;
		}
		bool Quote(std::string &text, std::string_view value, uint64_t maximum) {
			if (!Append(text, "\"", maximum)) return false;
			for (unsigned char c : value) {
				std::string escaped;
				if (c == '"')
					escaped = "\\\"";
				else if (c == '\\')
					escaped = "\\\\";
				else if (c < 32) {
					static constexpr char hex[] = "0123456789abcdef";
					escaped = "\\u00";
					escaped += hex[c >> 4];
					escaped += hex[c & 15];
				} else
					escaped = static_cast<char>(c);
				if (!Append(text, escaped, maximum)) return false;
			}
			return Append(text, "\"", maximum);
		}
		bool Json(const Value &value, std::string &text, uint64_t maximum, bool pretty, size_t depth = 0) {
			if (depth > 64) return false;
			const auto delimiter = [&](size_t level) {
				return !pretty || Append(text, "\n" + std::string(level * 2, ' '), maximum);
			};
			if (const auto *v = std::get_if<std::string>(&value)) return Quote(text, *v, maximum);
			if (const auto *v = std::get_if<bool>(&value))
				return Append(text, *v ? "true" : "false", maximum);
			if (std::holds_alternative<UndefinedValue>(value)) return Append(text, "null", maximum);
			if (const auto *v = std::get_if<StructValue>(&value)) {
				if (!v->Data || v->Data->Fields.size() > 4096 || !Append(text, "{", maximum)) return false;
				size_t i = 0;
				for (const auto &[name, field] : v->Data->Fields) {
					if ((i++ && !Append(text, ",", maximum)) || !delimiter(depth + 1) ||
						!Quote(text, name, maximum) || !Append(text, pretty ? ": " : ":", maximum) ||
						!Json(field, text, maximum, pretty, depth + 1))
						return false;
				}
				return (v->Data->Fields.empty() || delimiter(depth)) && Append(text, "}", maximum);
			}
			if (const auto *v = std::get_if<ArrayValue>(&value)) {
				if (v->Elements.size() > 4096 || !Append(text, "[", maximum)) return false;
				size_t i = 0;
				for (const auto &item : v->Elements) {
					Value field = std::visit([](const auto &element) -> Value { return element; }, item);
					if ((i++ && !Append(text, ",", maximum)) || !delimiter(depth + 1) ||
						!Json(field, text, maximum, pretty, depth + 1))
						return false;
				}
				return (v->Elements.empty() || delimiter(depth)) && Append(text, "]", maximum);
			}
			bool valid = true;
			const auto scalar = Scalar(value, valid);
			return valid && Append(text, scalar, maximum);
		}
		StructValue XmlNode(const engine::bake::ComposerXmlNode &node) {
			StructValue value;
			auto &fields = value.Data.emplace().Fields;
			fields.emplace_back("type", node.Type);
			if (!node.Attributes.empty()) {
				StructValue attrs;
				attrs.Data.emplace();
				for (const auto &[key, text] : node.Attributes)
					attrs.Data->Fields.emplace_back(key, text);
				fields.emplace_back("attributes", std::move(attrs));
			}
			if (node.Text) fields.emplace_back("text", *node.Text);
			if (!node.Children.empty()) {
				ArrayValue children{ValueType::Struct, {}};
				for (const auto &child : node.Children)
					children.Elements.emplace_back(XmlNode(child));
				fields.emplace_back("children", std::move(children));
			}
			return value;
		}
		const Value *Field(const StructValue &value, std::string_view name) {
			if (!value.Data) return nullptr;
			for (const auto &[key, field] : value.Data->Fields)
				if (key == name) return &field;
			return nullptr;
		}
		bool XmlWrite(
			const StructValue &node, std::string &text, uint64_t maximum, std::string indent, size_t depth
		) {
			if (depth > 64) return false;
			const auto *type = Field(node, "type");
			const auto *name = type ? std::get_if<std::string>(type) : nullptr;
			if (!name || name->empty() || name->find_first_of("<> \r\n\t\"'") != std::string::npos ||
				!Append(text, indent + "<" + *name, maximum))
				return false;
			const auto *attrs = Field(node, "attributes");
			if (const auto *fields = attrs ? std::get_if<StructValue>(attrs) : nullptr) {
				if (!fields->Data) return false;
				for (const auto &[key, value] : fields->Data->Fields) {
					bool valid = true;
					const auto scalar = Scalar(value, valid);
					if (!valid || !Append(text, " " + key + "=\"" + scalar + "\"", maximum)) return false;
				}
			}
			if (!Append(text, ">", maximum)) return false;
			if (const auto *content = Field(node, "text")) {
				bool valid = true;
				const auto scalar = Scalar(*content, valid);
				if (!valid || !Append(text, scalar, maximum)) return false;
			} else if (const auto *items = Field(node, "children")) {
				const auto *children = std::get_if<ArrayValue>(items);
				if (!children || children->ElementType != ValueType::Struct ||
					children->Elements.size() > 4096)
					return false;
				for (const auto &item : children->Elements) {
					const auto *child = std::get_if<StructValue>(&item);
					if (!child || !Append(text, "\r", maximum) ||
						!XmlWrite(*child, text, maximum, indent + "\t", depth + 1))
						return false;
				}
				if (!children->Elements.empty() && !Append(text, "\r" + indent, maximum)) return false;
			}
			return Append(text, "</" + *name + ">", maximum);
		}
		bool Xml(const StructValue &root, std::string &text, uint64_t maximum) {
			if (const auto *value = Field(root, "prolog")) {
				const auto *prolog = std::get_if<StructValue>(value);
				if (!prolog) return false;
				if (const auto *data = Field(*prolog, "attributes")) {
					const auto *attrs = std::get_if<StructValue>(data);
					if (!attrs || !attrs->Data) return false;
					if (!attrs->Data->Fields.empty()) {
						if (!Append(text, "<?xml", maximum)) return false;
						for (const auto &[key, value] : attrs->Data->Fields) {
							bool valid = true;
							const auto scalar = Scalar(value, valid);
							if (!valid || !Append(text, " " + key + "=\"" + scalar + "\"", maximum))
								return false;
						}
						if (!Append(text, "?>\n", maximum)) return false;
					}
				}
			}
			const auto *value = Field(root, "children");
			if (!value) return true;
			const auto *children = std::get_if<ArrayValue>(value);
			if (!children || children->ElementType != ValueType::Struct) return false;
			for (const auto &item : children->Elements) {
				const auto *child = std::get_if<StructValue>(&item);
				if (!child || !XmlWrite(*child, text, maximum, "", 0)) return false;
			}
			return true;
		}
		bool ParseJson(std::string text, uint64_t maximum, Value &parsed, std::string &failure) {
			Document document;
			Node node;
			node.Id = "parse";
			node.Type = "pc.struct_json_parse";
			if (text.size() > maximum / 128) {
				failure = "JSON input exceeds its parser operation budget";
				return false;
			}
			node.Values.push_back({"json_string", std::move(text)});
			document.Nodes.push_back(std::move(node));
			document.Outputs.push_back({"value", "parse", "struct"});
			Plan plan;
			Diagnostic diagnostic;
			EvaluationRequest request;

			EvaluatedValue value;
			if (Compile(document, plan, diagnostic) != Status::Ok ||
				EvaluateValue(document, plan, "value", request, value, diagnostic) != Status::Ok) {
				failure = diagnostic.Message;
				return false;
			}
			parsed = std::move(value.Data);
			return true;
		}
		bool Publish(const std::filesystem::path &path, std::string_view text, std::string &failure) {
			return WriteGraphHostFile(
				path, {reinterpret_cast<const uint8_t *>(text.data()), text.size()}, failure
			);
		}
	}
	bool IsGraphTextFileHost(std::string_view type) {
		return type == "pc.csv_file_read" || type == "pc.csv_file_write" || type == "pc.json_file_read" ||
			   type == "pc.json_file_write" || type == "pc.xml_file_read" || type == "pc.xml_file_write";
	}
	bool CaptureGraphTextFile(
		std::span<const GraphFileGrant> grants,
		const engine::assets::ContentPolicy &policy,
		const HostNodeInvocation &call,
		HostNodeCapture &out,
		std::string &failure
	) {
		const auto &kind = call.Authored.Type;
		if (kind == "pc.csv_file_write") return CaptureGraphCsvWrite(grants, policy, call, out, failure);
		const bool write = kind.ends_with("_write");
		const auto *path = Get<std::string>(call, "path");
		const GraphFileGrant *grant = nullptr;
		for (const auto &g : grants)
			if (g.NodeId == call.Authored.Id && g.Resource.empty()) {
				if (grant) {
					failure = "text document grants are duplicated";
					return false;
				}
				grant = &g;
			}
		if (!path || !grant || *path != grant->File.string() || write != grant->Write ||
			!policy.AllowsName(*path)) {
			failure = "text document requires an exact node/path/operation grant";
			return false;
		}
		const auto ext = kind.starts_with("pc.csv") ? ".csv" : kind.starts_with("pc.json") ? ".json" : ".xml";
		auto target = grant->File;
		if (write && target.extension() != ext) target += ext;
		const uint64_t maximum = std::min<uint64_t>(Limits::MaximumTextBytes, call.MaximumOperationBytes / 4);
		HostNodeCapture capture;
		capture.Authored = call.Authored;
		capture.Tick = call.Request.Tick;
		capture.Subframe = call.Request.Subframe;
		capture.NegativeFrame = call.Request.NegativeFrame;
		capture.Inputs.assign(call.Inputs.begin(), call.Inputs.end());
		std::string text;
		if (write) {
			const auto *value = Get<StructValue>(call, "struct");
			if (!value) {
				failure = "document writer needs an owned struct";
				return false;
			}
			if (kind == "pc.json_file_write") {
				const auto *pretty = Get<bool>(call, "pretty_print"),
						   *serialize = Get<bool>(call, "serialize");
				if (!pretty || !serialize || (*serialize && Field(*value, "serialize")) ||
					!Json(*value, text, maximum, *pretty)) {
					failure = "JSON contains unsupported runtime values or exceeds its text budget";
					return false;
				}
			} else if (!Xml(*value, text, maximum)) {
				failure = "XML source structure is invalid or exceeds its budget";
				return false;
			}
			if (!policy.AllowsName(target.string()) || !Publish(target, text, failure)) return false;
		} else {
			std::error_code error;
			const auto size = std::filesystem::file_size(target, error);
			if (error || size > maximum || target.extension() != ext) {
				failure = "text document size or native extension is invalid";
				return false;
			}
			text.resize(static_cast<size_t>(size));
			std::ifstream stream(target, std::ios::binary);
			stream.read(text.data(), static_cast<std::streamsize>(text.size()));
			if (!stream) {
				failure = "cannot read complete text document";
				return false;
			}
			if (kind == "pc.csv_file_read") {
				const auto *convert = Get<bool>(call, "convert_to_number");
				if (!convert) {
					failure = "CSV numeric conversion control is invalid";
					return false;
				}
				const uint64_t lineCount = static_cast<uint64_t>(std::count(text.begin(), text.end(), '\n')) +
										   (text.empty() ? 0 : 1);
				if (lineCount > 4096 ||
					lineCount * sizeof(ElementValue) + text.size() > call.MaximumOperationBytes / 2) {
					failure = "CSV line storage exceeds its operation budget";
					return false;
				}
				ArrayValue lines{*convert ? ValueType::Scalar : ValueType::Text, {}};
				std::istringstream stream(text);
				std::string line;
				while (std::getline(stream, line)) {
					if (lines.Elements.size() >= 4096) {
						failure = "CSV line count exceeds its source array budget";
						return false;
					}
					if (!line.empty() && line.back() == '\r') line.pop_back();
					if (*convert) {
						double number = 0;
						const auto result = std::from_chars(line.data(), line.data() + line.size(), number);
						if (result.ec != std::errc{} || !std::isfinite(number)) number = 0;
						lines.Elements.emplace_back(number);
					} else
						lines.Elements.emplace_back(line);
				}
				capture.Outputs.push_back({"content", std::move(lines)});
			} else if (kind == "pc.json_file_read") {
				Value value;
				if (!ParseJson(std::move(text), call.MaximumOperationBytes / 2, value, failure)) return false;
				capture.Outputs.push_back({"struct", std::move(value)});
			} else {
				engine::bake::ComposerXml parsed;
				if (!engine::bake::ReadComposerXml(text, parsed, failure, call.MaximumOperationBytes / 2))
					return false;
				auto root = XmlNode(parsed.Root);
				if (parsed.Prolog) root.Data->Fields.emplace_back("prolog", XmlNode(*parsed.Prolog));
				ArrayValue documents{ValueType::Struct, {root}};
				capture.Outputs.push_back({"content", std::move(documents)});
			}
			capture.Outputs.push_back({"path", *path});
		}
		out = std::move(capture);
		return true;
	}
}
