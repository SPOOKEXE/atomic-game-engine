#pragma once

// The engine API dump: one JSON file naming every class, datatype, enum,
// service and global a script can reach, with types.
//
// It is built from the two things that already describe the surface. The class
// table supplies what reflection knows (property storage, writability,
// creatability, service capabilities). The generated `engine.d.luau` supplies
// what only the declarations know (method signatures, events, datatype
// constructors and operators, globals), read through the Luau type checker
// rather than by parsing text. Because the declarations are generated from the
// same class table in the same run, the dump and the declaration files cannot
// disagree.
//
// Consumers: Studio's script completion embeds the file, and a standalone type
// checker can load it without linking the engine.

#include <string>
#include <string_view>
#include <vector>

namespace apidump {

	// One scriptable property as the class table declares it.
	struct ReflectedProperty {
		std::string Name;

		// The class table's own type name (`bool`, `Vector3`, `Instance`).
		std::string Type;

		// `field`, `computed`, `structural` or `resource`.
		std::string Kind;

		bool Writable = true;
	};

	// One class as the class table declares it. `Properties` may be the
	// inherited span; the dump keeps only the ones the parent does not have.
	struct ReflectedClass {
		std::string Name;
		std::string Parent;

		// Roblox-style tags: `NotCreatable`, `Service`, `Abstract`,
		// `NotBrowsable`, `RuntimeLocal`, `PluginSecurity`.
		std::vector<std::string> Tags;

		std::vector<ReflectedProperty> Properties;
	};

	struct ReflectedEnum {
		std::string Name;
		std::vector<std::string> Items;
	};

	// One service row from the script service catalogue, or a scene service.
	struct ReflectedService {
		std::string Name;

		// Capability names a script needs to reach it (`world`, `plugin-host`).
		std::vector<std::string> Capabilities;

		// `always` or `studio`.
		std::string Availability = "always";

		// `luau`, `javascript`.
		std::vector<std::string> Languages;
	};

	// A global whose availability is narrower than every script.
	struct ReflectedGlobalTag {
		std::string Name;
		std::vector<std::string> Tags;
	};

	struct Reflection {
		std::vector<ReflectedClass> Classes;
		std::vector<ReflectedEnum> Enums;
		std::vector<ReflectedService> Services;
		std::vector<ReflectedGlobalTag> GlobalTags;
	};

	struct Result {
		bool Ok = false;

		// The dump, ending in a newline, when `Ok`.
		std::string Json;

		// What stopped it: parse or type errors in the declarations.
		std::vector<std::string> Errors;
	};

	// The dump format's version. Bump it when a consumer would misread the
	// new shape.
	constexpr int DUMP_VERSION = 1;

	// Builds the dump from Luau declarations and the reflected class table.
	//
	// @param luauDeclarations The text of `engine.d.luau`.
	// @param reflection       What the class table and service catalogue say.
	// @return The JSON, or the reasons it could not be built.
	Result Generate(std::string_view luauDeclarations, const Reflection &reflection);

}
