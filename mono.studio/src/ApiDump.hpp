#pragma once

// The engine API dump as script completion reads it.
//
// `mono.tools/bindings` writes `api-dump.json` from the class table and the
// generated Luau declarations; the editor embeds that file at configure time and
// parses it once. Completion asks it what a type's members are, what a call
// returns and what a global holds, so the editor offers the same surface the
// declarations type-check against.
//
// Types are addressed by key. A class or datatype is its own name
// (`Part`, `Vector3`); a library global is `@` plus its name (`@Vector3` holds
// `new` and `zero`, `@task` holds `wait`); `@Enum` is the enum table and
// `@Enum.Material` one set; `Enum.Material` is an item of that set.

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace studio {

	struct ApiParameter {
		std::string Name;
		std::string Type;
	};

	enum class ApiMemberKind : uint8_t {
		Property,
		Method,
		Event,
		Callback,
		Function,
		Constant,
	};

	struct ApiMember {
		ApiMemberKind Kind = ApiMemberKind::Property;
		std::string Name;

		// A property, constant or event's type, or a callable's return.
		std::string Type;

		std::vector<ApiParameter> Parameters;
		bool Writable = false;
	};

	enum class ApiTypeKind : uint8_t {
		Class,
		Datatype,
		Library,
		Alias,
		EnumSet,
		EnumItem,
	};

	struct ApiType {
		std::string Name;
		std::string Superclass;
		ApiTypeKind Kind = ApiTypeKind::Datatype;
		std::vector<std::string> Tags;

		// Own members only, sorted by name. Overloads repeat a name.
		std::vector<ApiMember> Members;
	};

	struct ApiGlobal {
		std::string Name;

		// The type key the global holds, empty for a function.
		std::string Type;

		// A function global's overloads.
		std::vector<ApiMember> Overloads;

		std::vector<std::string> Tags;
	};

	struct ApiService {
		std::string Name;
		std::string Type;
		std::vector<std::string> Capabilities;
	};

	class ApiDump {
	  public:
		// Reads a dump. Answers nothing and fills `error` when it does not parse.
		static std::optional<ApiDump> Parse(std::string_view json, std::string &error);

		// The dump embedded in this build, parsed on first use.
		static const ApiDump &Engine();

		const ApiType *Type(std::string_view key) const;
		const ApiGlobal *Global(std::string_view name) const;
		const ApiService *Service(std::string_view name) const;

		// Every member a value of `key` answers, own first then inherited, one
		// row per name (a method's first overload).
		std::vector<const ApiMember *> Members(std::string_view key) const;

		// The first member named `name` on `key` or an ancestor.
		const ApiMember *Member(std::string_view key, std::string_view name) const;

		// Whether `key` is `base` or descends from it.
		bool IsA(std::string_view key, std::string_view base) const;

		const std::vector<ApiGlobal> &Globals() const {
			return GlobalList;
		}

		const std::vector<ApiService> &Services() const {
			return ServiceList;
		}

		// Enum set names in dump order.
		const std::vector<std::string> &EnumSets() const {
			return EnumNames;
		}

	  private:
		std::map<std::string, ApiType, std::less<>> Types;
		std::vector<ApiGlobal> GlobalList;
		std::vector<ApiService> ServiceList;
		std::vector<std::string> EnumNames;
	};

	// The type key a declared type string names, or empty when it names
	// nothing completion can walk (`number`, `{ Instance }`, a function).
	// `Part?` and `Part | nil` are `Part`; `Enum.Material` stays as written.
	std::string ApiTypeKey(std::string_view declared);

	// The element type of an array type string (`{ Instance }` is `Instance`),
	// or empty when it is not one.
	std::string ApiElementType(std::string_view declared);

	// `name(a: T, b: U): R`, shortening any parameter type too long to read.
	// `spans`, when given, receives each parameter's byte range in the label.
	std::string ApiSignatureLabel(
		std::string_view callee,
		const ApiMember &member,
		std::vector<std::pair<size_t, size_t>> *spans = nullptr
	);

}
