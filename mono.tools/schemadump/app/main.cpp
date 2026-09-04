// Writes the registered ECS component and class schema as TOML.

#include <engine/core/Arguments.hpp>
#include <engine/core/Log.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/effects/Registration.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/replication/SnapshotBuffer.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/world/Postbox.hpp>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <span>
#include <sstream>
#include <string>
#include <string_view>

namespace {

	using engine::ecs::Classes;
	using engine::ecs::ClassId;
	using engine::ecs::ClassInfo;
	using engine::ecs::ComponentId;
	using engine::ecs::ComponentKind;
	using engine::ecs::Components;
	using engine::ecs::PropertyKind;
	using engine::ecs::PropertyType;
	using engine::ecs::TypeDescriptor;

	std::string Quote(std::string_view value) {
		std::string quoted;
		quoted.reserve(value.size() + 2);
		quoted += '"';
		for (const char character : value) {
			switch (character) {
			case '\\':
				quoted += "\\\\";
				break;
			case '"':
				quoted += "\\\"";
				break;
			case '\n':
				quoted += "\\n";
				break;
			case '\r':
				quoted += "\\r";
				break;
			case '\t':
				quoted += "\\t";
				break;
			default:
				if (static_cast<unsigned char>(character) < 0x20) {
					char escaped[7];
					std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned char>(character));
					quoted += escaped;
				} else {
					quoted += character;
				}
				break;
			}
		}
		quoted += '"';
		return quoted;
	}

	std::string PropertyTypeString(PropertyType type) {
		switch (type) {
		case PropertyType::Bool:
			return "Bool";
		case PropertyType::Int32:
			return "Int32";
		case PropertyType::Int64:
			return "Int64";
		case PropertyType::Float:
			return "Float";
		case PropertyType::Double:
			return "Double";
		case PropertyType::Name:
			return "Name";
		case PropertyType::Enum:
			return "Enum";
		case PropertyType::String:
			return "String";
		case PropertyType::Reference:
			return "Reference";
		case PropertyType::Vector3:
			return "Vector3";
		case PropertyType::CFrame:
			return "CFrame";
		case PropertyType::Color3:
			return "Color3";
		case PropertyType::Vector2:
			return "Vector2";
		case PropertyType::UDim:
			return "UDim";
		case PropertyType::UDim2:
			return "UDim2";
		case PropertyType::Rect:
			return "Rect";
		case PropertyType::NumberRange:
			return "NumberRange";
		case PropertyType::NumberSequence:
			return "NumberSequence";
		case PropertyType::ColorSequence:
			return "ColorSequence";
		default:
			return "Opaque";
		}
	}

	std::string PropertyKindString(PropertyKind kind) {
		switch (kind) {
		case PropertyKind::Field:
			return "Field";
		case PropertyKind::Computed:
			return "Computed";
		case PropertyKind::Structural:
			return "Structural";
		case PropertyKind::Resource:
			return "Resource";
		}
		return "Unknown";
	}

	void RegisterAll() {
		// A store registers ECS's foundational instance components. This mirrors
		// process startup, where a table without them cannot describe a world.
		const engine::ecs::Store store("schemadump");
		(void)store;

		engine::ecs::RegisterAttributeComponents();
		engine::world::RegisterMailboxTypes();
		engine::scene::RegisterSceneComponents();
		engine::gui::RegisterGuiComponents();
		engine::physics::RegisterPhysicsComponents();
		engine::effects::RegisterEffectComponents();
		engine::graph::RegisterPipelineComponents();
		engine::script::RegisterScriptComponents();
		engine::replication::RegisterReplicationComponents();
		engine::examples::RegisterExampleComponents();

		// Class registration is separate from component registration. Leaving a
		// tree out would still write valid TOML, but it would describe less than a
		// game can store.
		engine::scene::RegisterSceneClasses();
		engine::gui::RegisterGuiClasses();
		(void)engine::script::ScriptClass();
		engine::effects::RegisterEffectClasses();
	}

	std::map<uint32_t, std::string> ComponentNames() {
		std::map<uint32_t, std::string> names;
		for (uint32_t index = 0; index < static_cast<uint32_t>(Components::Count()); index++) {
			names.emplace(index, std::string(Components::Describe(ComponentId(index)).Name.Text()));
		}
		return names;
	}

	std::string ComponentName(const std::map<uint32_t, std::string> &names, ComponentId id) {
		const auto found = names.find(id.Index);
		return found == names.end() ? std::string() : found->second;
	}

	void WriteNames(
		std::ostringstream &toml,
		std::span<const ComponentId> ids,
		const std::map<uint32_t, std::string> &names
	) {
		toml << '[';
		for (size_t index = 0; index < ids.size(); index++) {
			if (index != 0) {
				toml << ", ";
			}
			toml << Quote(ComponentName(names, ids[index]));
		}
		toml << ']';
	}

	std::string DumpSchemaData() {
		std::ostringstream toml;
		const std::map<uint32_t, std::string> componentNames = ComponentNames();

		toml << "# Atomic Game Engine ECS schema\n";
		toml << "# Generated by schemadump - do not edit manually\n\n";
		toml << "schema_version = 1\n\n";

		for (uint32_t index = 0; index < static_cast<uint32_t>(Components::Count()); index++) {
			const TypeDescriptor &descriptor = Components::Describe(ComponentId(index));

			toml << "[[components]]\n";
			toml << "name = " << Quote(descriptor.Name.Text()) << "\n";
			toml << "kind = \"" << (descriptor.Kind == ComponentKind::Tag ? "Tag" : "Data") << "\"\n";
			toml << "size = " << descriptor.Size << "\n";
			toml << "alignment = " << descriptor.Alignment << "\n";
			toml << "trivial = " << (descriptor.Trivial ? "true" : "false") << "\n";
			toml << "serialisable = " << (descriptor.Serialisable ? "true" : "false") << "\n\n";
		}

		for (size_t index = 0; index < Classes::Count(); index++) {
			const ClassInfo &info = Classes::Describe(ClassId(index));

			toml << "[[classes]]\n";
			toml << "name = " << Quote(info.Name.Text()) << "\n";
			toml << "creatable = " << (info.Creatable ? "true" : "false") << "\n";
			if (info.Parent.IsValid()) {
				toml << "parent = " << Quote(Classes::Describe(info.Parent).Name.Text()) << "\n";
			}
			if (info.Set) {
				toml << "components = ";
				WriteNames(toml, info.Set->Ids(), componentNames);
				toml << "\n";
			}

			for (const auto &property : info.Properties) {
				toml << "\n[[classes.properties]]\n";
				toml << "name = " << Quote(property.Name.Text()) << "\n";
				toml << "type = " << Quote(PropertyTypeString(property.Type)) << "\n";
				toml << "kind = " << Quote(PropertyKindString(property.Kind)) << "\n";
				toml << "writable = " << (property.Writable ? "true" : "false") << "\n";
				toml << "scriptable = " << (property.Scriptable ? "true" : "false") << "\n";
				if (property.Type == PropertyType::Enum) {
					toml << "enum_name = " << Quote(property.EnumName.Text()) << "\n";
				}
			}
			if (index + 1 < Classes::Count()) {
				toml << "\n";
			}
		}

		return toml.str();
	}

	std::string DumpSchema() {
		return R"schema(# Atomic Game Engine ECS schema
# Generated by schemadump - do not edit manually

schema_version = 1

[components]
kind = "array"
item = "component"

[components.component]
name = "string - stable component name"
kind = "Tag | Data"
size = "integer - sizeof the component in bytes"
alignment = "integer - alignof the component in bytes"
trivial = "boolean - component may be copied as bytes"
serialisable = "boolean - component has a file reader and writer"

[classes]
kind = "array"
item = "class"

[classes.class]
name = "string - stable class name"
creatable = "boolean - authored code may create this class"
parent = "string (optional) - stable parent class name"
components = "array of strings (optional) - stable component names"
properties = "array of class_property descriptors (optional)"

[class_property]
name = "string - property name"
type = "Bool | Int32 | Int64 | Float | Double | Name | Enum | String | Reference | Vector3 | CFrame | Color3 | Vector2 | UDim | UDim2 | Rect | NumberRange | NumberSequence | ColorSequence | Opaque"
kind = "Field | Computed | Structural | Resource"
writable = "boolean"
scriptable = "boolean"
enum_name = "string (optional) - enum name when type is Enum"
)schema";
	}

	bool WriteFile(const std::filesystem::path &path, const std::string &text) {
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		if (!file) {
			return false;
		}
		file << text;
		return file.good();
	}
}

int main(int argc, char **argv) {
	engine::core::Log::Initialise("schemadump");

	engine::core::Arguments arguments("schemadump", "atomic - dumps the ECS schema as TOML.");
	arguments.Value("schema", "FILE", "Schema TOML path (default: docs/schema.toml)");
	arguments.Value("data", "FILE", "Schema data TOML path (default: docs/schema-data.toml)");

	const engine::core::Arguments::Result parsed = arguments.Parse(argc, argv);
	if (parsed.Ok && parsed.VersionRequested) {
		std::cout << arguments.VersionLine();
		return 0;
	}
	if (!parsed.Ok || parsed.HelpRequested) {
		return parsed.Ok ? 0 : 2;
	}
	if (parsed.DescribeRequested) {
		std::fputs(arguments.Describe().c_str(), stdout);
		return 0;
	}

	const std::filesystem::path schemaPath{std::string{arguments.Get("schema").value_or("docs/schema.toml")}};
	const std::filesystem::path dataPath{
		std::string{arguments.Get("data").value_or("docs/schema-data.toml")}
	};
	RegisterAll();
	if (!WriteFile(schemaPath, DumpSchema())) {
		std::cerr << "cannot write " << schemaPath << "\n";
		return 2;
	}
	if (!WriteFile(dataPath, DumpSchemaData())) {
		std::cerr << "cannot write " << dataPath << "\n";
		return 2;
	}

	std::cout << "schemadump - schema written to " << schemaPath << " and " << dataPath << "\n";
	return 0;
}
