#include "ApiDump.hpp"

#include <engine/core/Log.hpp>

#include <EngineApiDump.hpp>
#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>
#include <set>

namespace studio {

	namespace {

		using Json = nlohmann::json;

		std::string Text(const Json &object, const char *key) {
			const auto found = object.find(key);
			return found != object.end() && found->is_string() ? found->get<std::string>() : std::string{};
		}

		std::vector<std::string> Strings(const Json &object, const char *key) {
			std::vector<std::string> values;
			const auto found = object.find(key);
			if (found != object.end() && found->is_array()) {
				for (const Json &value : *found) {
					if (value.is_string()) {
						values.push_back(value.get<std::string>());
					}
				}
			}
			return values;
		}

		std::vector<ApiParameter> Parameters(const Json &object) {
			std::vector<ApiParameter> parameters;
			const auto found = object.find("parameters");
			if (found != object.end() && found->is_array()) {
				for (const Json &parameter : *found) {
					parameters.push_back({Text(parameter, "name"), Text(parameter, "type")});
				}
			}
			return parameters;
		}

		// A callable row: the dump's `returns` becomes the member's type.
		ApiMember Callable(const Json &object, const ApiMemberKind kind) {
			ApiMember member;
			member.Kind = kind;
			member.Name = Text(object, "name");
			member.Type = Text(object, "returns");
			member.Parameters = Parameters(object);
			return member;
		}

		ApiMember ReadMember(const Json &object) {
			const std::string kind = Text(object, "kind");
			if (kind == "method") {
				return Callable(object, ApiMemberKind::Method);
			}
			if (kind == "callback") {
				return Callable(object, ApiMemberKind::Callback);
			}

			ApiMember member;
			member.Name = Text(object, "name");
			member.Type = Text(object, "type");
			if (kind == "event") {
				member.Kind = ApiMemberKind::Event;
				member.Parameters = Parameters(object);
			} else {
				member.Kind = ApiMemberKind::Property;
				member.Writable = object.value("writable", false);
			}
			return member;
		}

		std::string_view Trim(std::string_view value) {
			while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
				value.remove_prefix(1);
			}
			while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
				value.remove_suffix(1);
			}
			return value;
		}

		// Whether a leading bracket closes at the very end, so `(A | B)` can be
		// unwrapped and `(a) -> b` cannot.
		bool Wrapped(const std::string_view value, const char open, const char close) {
			if (value.size() < 2 || value.front() != open || value.back() != close) {
				return false;
			}
			int depth = 0;
			for (size_t at = 0; at < value.size(); at++) {
				depth += value[at] == open ? 1 : value[at] == close ? -1 : 0;
				if (depth == 0 && at + 1 < value.size()) {
					return false;
				}
			}
			return true;
		}

		// The parts of a union at the top level.
		std::vector<std::string_view> UnionParts(const std::string_view value) {
			std::vector<std::string_view> parts;
			int depth = 0;
			size_t start = 0;
			for (size_t at = 0; at < value.size(); at++) {
				const char character = value[at];
				if (character == '(' || character == '{' || character == '<') {
					depth++;
				} else if (character == ')' || character == '}' || character == '>') {
					depth--;
				} else if (character == '|' && depth == 0) {
					parts.push_back(Trim(value.substr(start, at - start)));
					start = at + 1;
				}
			}
			parts.push_back(Trim(value.substr(start)));
			return parts;
		}

	}

	std::string ApiTypeKey(std::string_view declared) {
		declared = Trim(declared);
		while (Wrapped(declared, '(', ')')) {
			declared = Trim(declared.substr(1, declared.size() - 2));
		}

		const std::vector<std::string_view> parts = UnionParts(declared);
		std::string_view chosen;
		for (const std::string_view part : parts) {
			if (part != "nil") {
				chosen = part;
				break;
			}
		}
		while (!chosen.empty() && chosen.back() == '?') {
			chosen.remove_suffix(1);
		}
		chosen = Trim(chosen);
		while (Wrapped(chosen, '(', ')')) {
			chosen = Trim(chosen.substr(1, chosen.size() - 2));
			while (!chosen.empty() && chosen.back() == '?') {
				chosen.remove_suffix(1);
			}
		}

		if (chosen.empty() || std::isdigit(static_cast<unsigned char>(chosen.front())) != 0) {
			return {};
		}
		const bool plain = std::all_of(chosen.begin(), chosen.end(), [](const char character) {
			return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_' ||
				   character == '.' || character == '@';
		});
		return plain ? std::string(chosen) : std::string{};
	}

	std::string ApiElementType(std::string_view declared) {
		declared = Trim(declared);
		while (!declared.empty() && declared.back() == '?') {
			declared.remove_suffix(1);
		}
		if (!Wrapped(declared, '{', '}')) {
			return {};
		}
		const std::string_view inner = Trim(declared.substr(1, declared.size() - 2));
		if (inner.find(':') != std::string_view::npos) {
			return {};
		}
		return std::string(inner);
	}

	std::string ApiSignatureLabel(
		const std::string_view callee, const ApiMember &member, std::vector<std::pair<size_t, size_t>> *spans
	) {
		constexpr size_t READABLE = 48;
		std::string label(callee);
		label += "(";
		for (size_t index = 0; index < member.Parameters.size(); index++) {
			const ApiParameter &parameter = member.Parameters[index];
			if (index > 0) {
				label += ", ";
			}
			const size_t start = label.size();
			label += parameter.Name;
			std::string type = parameter.Type;
			if (type.size() > READABLE) {
				// A long literal union is a list of names; the popup offers those.
				type = type.find("\"") != std::string::npos ? "string" : "...";
			}
			if (parameter.Name != "...") {
				label += ": ";
			}
			label += type;
			if (spans != nullptr) {
				spans->emplace_back(start, label.size());
			}
		}
		label += ")";
		if (!member.Type.empty() && member.Type != "()") {
			label += ": " + member.Type;
		}
		return label;
	}

	std::optional<ApiDump> ApiDump::Parse(const std::string_view json, std::string &error) {
		const Json document = Json::parse(json, nullptr, false);
		if (document.is_discarded() || !document.is_object()) {
			error = "the API dump is not a JSON object";
			return std::nullopt;
		}

		ApiDump dump;

		for (const Json &entry : document.value("classes", Json::array())) {
			ApiType type;
			type.Name = Text(entry, "name");
			type.Superclass = Text(entry, "superclass");
			type.Kind = ApiTypeKind::Class;
			type.Tags = Strings(entry, "tags");
			for (const Json &member : entry.value("members", Json::array())) {
				type.Members.push_back(ReadMember(member));
			}
			dump.Types.emplace(type.Name, std::move(type));
		}

		for (const Json &entry : document.value("datatypes", Json::array())) {
			ApiType type;
			type.Name = Text(entry, "name");
			type.Superclass = Text(entry, "superclass");
			type.Tags = Strings(entry, "tags");
			for (const Json &member : entry.value("members", Json::array())) {
				type.Members.push_back(ReadMember(member));
			}

			// The global table of the same name: `Vector3.new`, `Vector3.zero`.
			if (entry.contains("constructors") || entry.contains("constants")) {
				ApiType library;
				library.Name = "@" + type.Name;
				library.Kind = ApiTypeKind::Library;
				for (const Json &constructor : entry.value("constructors", Json::array())) {
					library.Members.push_back(Callable(constructor, ApiMemberKind::Function));
				}
				for (const Json &constant : entry.value("constants", Json::array())) {
					ApiMember member;
					member.Kind = ApiMemberKind::Constant;
					member.Name = Text(constant, "name");
					member.Type = Text(constant, "type");
					library.Members.push_back(std::move(member));
				}
				dump.Types.emplace(library.Name, std::move(library));
			}
			dump.Types.emplace(type.Name, std::move(type));
		}

		ApiType enums;
		enums.Name = "@Enum";
		enums.Kind = ApiTypeKind::Library;
		for (const Json &entry : document.value("enums", Json::array())) {
			const std::string name = Text(entry, "name");
			dump.EnumNames.push_back(name);
			enums.Members.push_back({ApiMemberKind::Constant, name, "@Enum." + name, {}, false});

			ApiType set;
			set.Name = "@Enum." + name;
			set.Kind = ApiTypeKind::EnumSet;
			for (const Json &item : entry.value("items", Json::array())) {
				set.Members.push_back(
					{ApiMemberKind::Constant, Text(item, "name"), "Enum." + name, {}, false}
				);
			}
			set.Members.push_back(
				{ApiMemberKind::Method, "GetEnumItems", "{ Enum." + name + " }", {}, false}
			);
			dump.Types.emplace(set.Name, std::move(set));

			ApiType item;
			item.Name = "Enum." + name;
			item.Kind = ApiTypeKind::EnumItem;
			item.Members.push_back({ApiMemberKind::Property, "EnumType", "@Enum." + name, {}, false});
			item.Members.push_back({ApiMemberKind::Property, "Name", "string", {}, false});
			item.Members.push_back({ApiMemberKind::Property, "Value", "number", {}, false});
			dump.Types.emplace(item.Name, std::move(item));
		}
		dump.Types.emplace(enums.Name, std::move(enums));

		for (const Json &entry : document.value("services", Json::array())) {
			dump.ServiceList.push_back(
				{Text(entry, "name"), Text(entry, "type"), Strings(entry, "capabilities")}
			);
		}

		for (const Json &entry : document.value("types", Json::array())) {
			ApiType alias;
			alias.Name = Text(entry, "name");
			alias.Kind = ApiTypeKind::Alias;
			for (const Json &field : entry.value("fields", Json::array())) {
				ApiMember member;
				member.Name = Text(field, "name");
				member.Type = Text(field, "type");
				member.Writable = true;

				// `GetService: <T>(self: DataModel, ...) -> ...` is called with
				// a colon. Its return is a type function, so it is left blank.
				if (member.Type.find("(self") != std::string::npos) {
					member.Kind = ApiMemberKind::Method;
					member.Type.clear();
					member.Writable = false;
				}
				alias.Members.push_back(std::move(member));
			}
			dump.Types.emplace(alias.Name, std::move(alias));
		}

		for (const Json &entry : document.value("globals", Json::array())) {
			ApiGlobal global;
			global.Name = Text(entry, "name");
			global.Tags = Strings(entry, "tags");
			const std::string kind = Text(entry, "kind");

			if (kind == "enums") {
				global.Type = "@Enum";
			} else if (kind == "library" && entry.contains("datatype")) {
				global.Type = "@" + Text(entry, "datatype");
			} else if (kind == "library") {
				ApiType library;
				library.Name = "@" + global.Name;
				library.Kind = ApiTypeKind::Library;
				for (const Json &function : entry.value("functions", Json::array())) {
					library.Members.push_back(Callable(function, ApiMemberKind::Function));
				}
				for (const Json &value : entry.value("values", Json::array())) {
					library.Members.push_back(
						{ApiMemberKind::Constant, Text(value, "name"), Text(value, "type"), {}, false}
					);
				}
				global.Type = library.Name;
				dump.Types.emplace(library.Name, std::move(library));
			} else if (kind == "function") {
				for (const Json &overload : entry.value("overloads", Json::array())) {
					ApiMember member = Callable(overload, ApiMemberKind::Function);
					member.Name = global.Name;
					global.Overloads.push_back(std::move(member));
				}
			} else {
				global.Type = Text(entry, "type");
			}
			dump.GlobalList.push_back(std::move(global));
		}

		return dump;
	}

	const ApiDump &ApiDump::Engine() {
		static const ApiDump engine = [] {
			std::string error;
			std::optional<ApiDump> parsed = Parse(ENGINE_API_DUMP_JSON, error);
			if (!parsed) {
				ENGINE_ERROR("the embedded API dump did not parse: {}", error);
				return ApiDump{};
			}
			return std::move(*parsed);
		}();
		return engine;
	}

	const ApiType *ApiDump::Type(const std::string_view key) const {
		const auto found = Types.find(key);
		return found != Types.end() ? &found->second : nullptr;
	}

	const ApiGlobal *ApiDump::Global(const std::string_view name) const {
		for (const ApiGlobal &global : GlobalList) {
			if (global.Name == name) {
				return &global;
			}
		}
		return nullptr;
	}

	const ApiService *ApiDump::Service(const std::string_view name) const {
		for (const ApiService &service : ServiceList) {
			if (service.Name == name) {
				return &service;
			}
		}
		return nullptr;
	}

	std::vector<const ApiMember *> ApiDump::Members(const std::string_view key) const {
		std::vector<const ApiMember *> members;
		std::set<std::string_view> seen;

		// A chain longer than this is a cycle in a malformed dump.
		constexpr int DEPTH = 64;
		const ApiType *type = Type(key);
		for (int depth = 0; type != nullptr && depth < DEPTH; depth++) {
			for (const ApiMember &member : type->Members) {
				if (seen.insert(member.Name).second) {
					members.push_back(&member);
				}
			}
			type = type->Superclass.empty() ? nullptr : Type(type->Superclass);
		}
		return members;
	}

	const ApiMember *ApiDump::Member(const std::string_view key, const std::string_view name) const {
		constexpr int DEPTH = 64;
		const ApiType *type = Type(key);
		for (int depth = 0; type != nullptr && depth < DEPTH; depth++) {
			for (const ApiMember &member : type->Members) {
				if (member.Name == name) {
					return &member;
				}
			}
			type = type->Superclass.empty() ? nullptr : Type(type->Superclass);
		}
		return nullptr;
	}

	bool ApiDump::IsA(const std::string_view key, const std::string_view base) const {
		constexpr int DEPTH = 64;
		const ApiType *type = Type(key);
		for (int depth = 0; type != nullptr && depth < DEPTH; depth++) {
			if (type->Name == base) {
				return true;
			}
			type = type->Superclass.empty() ? nullptr : Type(type->Superclass);
		}
		return false;
	}

}
