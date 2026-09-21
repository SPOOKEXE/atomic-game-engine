// Writes the registered runtime API reference as TOML.

#include <engine/core/Arguments.hpp>
#include <engine/core/Log.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/EnumTable.hpp>
#include <engine/ecs/Schema.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/effects/Registration.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/replication/SnapshotBuffer.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/script/EcsInstanceMethods.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/ScriptCall.hpp>
#include <engine/script/ServiceCatalogue.hpp>
#include <engine/script/ServiceSurface.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/world/Postbox.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
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
	using engine::ecs::Schemas;
	using engine::ecs::TypeDescriptor;
	using engine::script::ServiceAvailability;
	using engine::script::ServiceLanguages;

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

	std::string FieldPackingString(engine::ecs::FieldPacking packing) {
		switch (packing) {
		case engine::ecs::FieldPacking::Native:
			return "Native";
		case engine::ecs::FieldPacking::Float16:
			return "Float16";
		case engine::ecs::FieldPacking::UFloat16:
			return "UFloat16";
		case engine::ecs::FieldPacking::Float8:
			return "Float8";
		case engine::ecs::FieldPacking::UFloat8:
			return "UFloat8";
		case engine::ecs::FieldPacking::Int16:
			return "Int16";
		case engine::ecs::FieldPacking::UInt16:
			return "UInt16";
		case engine::ecs::FieldPacking::Int8:
			return "Int8";
		case engine::ecs::FieldPacking::UInt8:
			return "UInt8";
		case engine::ecs::FieldPacking::Int4:
			return "Int4";
		case engine::ecs::FieldPacking::UInt4:
			return "UInt4";
		case engine::ecs::FieldPacking::Bool:
			return "Bool";
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

	void WriteStrings(std::ostringstream &toml, std::span<const std::string> values) {
		toml << '[';
		for (size_t index = 0; index < values.size(); index++) {
			if (index != 0) {
				toml << ", ";
			}
			toml << Quote(values[index]);
		}
		toml << ']';
	}

	void WriteTable(std::ostringstream &toml, std::initializer_list<std::string_view> path) {
		toml << '[';
		size_t index = 0;
		for (const std::string_view key : path) {
			if (index++ != 0) {
				toml << '.';
			}
			toml << Quote(key);
		}
		toml << "]\n";
	}

	void WriteComponentSet(
		std::ostringstream &toml,
		const engine::ecs::ComponentSet *set,
		const std::map<uint32_t, std::string> &names
	) {
		if (set == nullptr) {
			toml << "[]";
			return;
		}
		WriteNames(toml, set->Ids(), names);
	}

	std::string ServiceAvailabilityString(ServiceAvailability availability) {
		return availability == ServiceAvailability::Always ? "Always" : "Studio";
	}

	std::vector<std::string> ServiceLanguagesStrings(ServiceLanguages languages) {
		std::vector<std::string> result;
		if (engine::script::Binds(languages, ServiceLanguages::Luau)) {
			result.emplace_back("Luau");
		}
		if (engine::script::Binds(languages, ServiceLanguages::JavaScript)) {
			result.emplace_back("JavaScript");
		}
		return result;
	}

	std::vector<std::string> CapabilityStrings(engine::script::ScriptCapabilities capabilities) {
		constexpr std::array CAPABILITIES{
			engine::script::ScriptCapabilities::World,
			engine::script::ScriptCapabilities::Messaging,
			engine::script::ScriptCapabilities::Persistence,
			engine::script::ScriptCapabilities::Teleport,
			engine::script::ScriptCapabilities::Input,
			engine::script::ScriptCapabilities::Audio,
			engine::script::ScriptCapabilities::StudioDebug,
			engine::script::ScriptCapabilities::PluginHost,
		};
		std::vector<std::string> result;
		for (const engine::script::ScriptCapabilities capability : CAPABILITIES) {
			if (engine::script::HasCapabilities(capabilities, capability)) {
				result.emplace_back(engine::script::CapabilityName(capability));
			}
		}
		return result;
	}

	std::string DumpSchemaData() {
		std::ostringstream toml;
		const std::map<uint32_t, std::string> componentNames = ComponentNames();
		std::vector<ComponentId> components;
		components.reserve(Components::Count());
		for (uint32_t index = 0; index < static_cast<uint32_t>(Components::Count()); index++) {
			components.emplace_back(index);
		}
		std::sort(components.begin(), components.end(), [&](ComponentId left, ComponentId right) {
			return componentNames.at(left.Index) < componentNames.at(right.Index);
		});

		std::vector<ClassId> classes;
		classes.reserve(Classes::Count());
		for (size_t index = 0; index < Classes::Count(); index++) {
			classes.emplace_back(static_cast<uint32_t>(index));
		}
		std::sort(classes.begin(), classes.end(), [](ClassId left, ClassId right) {
			return Classes::Describe(left).Name.Text() < Classes::Describe(right).Name.Text();
		});

		size_t propertyCount = 0;
		for (const ClassId id : classes) {
			propertyCount += Classes::Describe(id).Properties.size();
		}
		std::vector<engine::core::Name> enums = engine::ecs::EnumTable::Names();
		std::sort(enums.begin(), enums.end(), [](engine::core::Name left, engine::core::Name right) {
			return left.Text() < right.Text();
		});
		std::vector<std::pair<std::string_view, std::string_view>> functions;
		for (const engine::script::InstanceMethod &method : engine::script::NeutralInstanceMethods()) {
			functions.emplace_back(method.Name, "neutral");
		}
		for (const engine::script::EcsInstanceMethodDescriptor &method :
			 engine::script::EcsInstanceMethods()) {
			functions.emplace_back(method.Name, "ecs");
		}
		std::sort(functions.begin(), functions.end(), [](const auto &left, const auto &right) {
			return left.first < right.first;
		});

		std::vector<const engine::script::ServiceRow *> services;
		for (const ServiceAvailability phase : {ServiceAvailability::Always, ServiceAvailability::Studio}) {
			for (const engine::script::ServiceRow &row : engine::script::ServiceRows(phase)) {
				services.push_back(&row);
			}
		}
		std::sort(services.begin(), services.end(), [](const auto *left, const auto *right) {
			return std::string_view(left->Definition.Name) < std::string_view(right->Definition.Name);
		});

		size_t serviceMethodCount = 0;
		size_t servicePropertyCount = 0;
		size_t serviceSignalCount = 0;
		for (const engine::script::ServiceRow *row : services) {
			if (row->Surface == nullptr) {
				continue;
			}
			const engine::script::ServiceSurface &surface = row->Surface();
			serviceMethodCount += surface.Methods.size() + surface.LuauMethods.size();
			servicePropertyCount += surface.Properties.size();
			serviceSignalCount += surface.Signals.size();
		}

		toml << "# Atomic Game Engine runtime API reference\n";
		toml << "# Generated by schemadump. Do not edit manually.\n\n";
		WriteTable(toml, {"reference"});
		toml << "name = \"Atomic Game Engine runtime API\"\n";
		toml << "format = \"atomic-game-engine.runtime-api\"\n";
		toml << "schema_version = 3\n";
		toml << "generated_by = \"schemadump\"\n";
		toml << "source = \"runtime registries\"\n\n";
		WriteTable(toml, {"reference", "counts"});
		toml << "components = " << components.size() << "\n";
		toml << "classes = " << classes.size() << "\n";
		toml << "properties = " << propertyCount << "\n";
		toml << "enums = " << enums.size() << "\n";
		toml << "functions = " << functions.size() << "\n\n";
		toml << "services = " << services.size() << "\n";
		toml << "service_methods = " << serviceMethodCount << "\n";
		toml << "service_properties = " << servicePropertyCount << "\n";
		toml << "service_signals = " << serviceSignalCount << "\n\n";

		for (const ComponentId id : components) {
			const TypeDescriptor &descriptor = Components::Describe(id);
			const engine::ecs::Schema *schema = Schemas::Of(id);

			WriteTable(toml, {"components", descriptor.Name.Text()});
			toml << "name = " << Quote(descriptor.Name.Text()) << "\n";
			toml << "kind = \"" << (descriptor.Kind == ComponentKind::Tag ? "Tag" : "Data") << "\"\n";
			toml << "size = " << descriptor.Size << "\n";
			toml << "alignment = " << descriptor.Alignment << "\n";
			toml << "trivial = " << (descriptor.Trivial ? "true" : "false") << "\n";
			toml << "serialisable = " << (descriptor.Serialisable ? "true" : "false") << "\n";
			toml << "raw_serialisation = " << (descriptor.RawSerialisation ? "true" : "false") << "\n";
			toml << "padded = " << (descriptor.Padded ? "true" : "false") << "\n";
			toml << "wire_size = " << (descriptor.Wire.Present() ? descriptor.Wire.Size : 0) << "\n";
			toml << "described = " << (schema != nullptr ? "true" : "false") << "\n";
			if (schema != nullptr) {
				const std::vector<std::string> tags = schema->Tags();
				toml << "tags = ";
				WriteStrings(toml, tags);
				toml << "\n";
				toml << "field_count = " << schema->Fields().size() << "\n";
				for (const engine::ecs::FieldDescriptor &field : schema->Fields()) {
					toml << "\n";
					WriteTable(toml, {"components", descriptor.Name.Text(), "fields", field.Spelling});
					toml << "name = " << Quote(field.Spelling) << "\n";
					toml << "type = " << Quote(PropertyTypeString(field.Type)) << "\n";
					toml << "offset = " << field.Offset << "\n";
					toml << "size = " << field.Size << "\n";
					toml << "packing = " << Quote(FieldPackingString(field.Packing)) << "\n";
					toml << "bit_offset = " << static_cast<uint32_t>(field.BitOffset) << "\n";
					toml << "storage_bits = " << field.StorageBits << "\n";
					toml << "exposed = " << (field.Exposed ? "true" : "false") << "\n";
					if (field.Enum.IsValid()) {
						toml << "enum = " << Quote(field.Enum.Text()) << "\n";
					}
					toml << "tags = ";
					WriteStrings(toml, field.Tags);
					toml << "\n";
				}
			}
			toml << "\n";
		}

		for (const ClassId id : classes) {
			const ClassInfo &info = Classes::Describe(id);

			WriteTable(toml, {"classes", info.Name.Text()});
			toml << "name = " << Quote(info.Name.Text()) << "\n";
			toml << "creatable = " << (info.Creatable ? "true" : "false") << "\n";
			if (info.Parent.IsValid()) {
				toml << "inherits = " << Quote(Classes::Describe(info.Parent).Name.Text()) << "\n";
			}
			toml << "components = ";
			WriteComponentSet(toml, info.Set, componentNames);
			toml << "\n";
			toml << "property_count = " << info.Properties.size() << "\n";

			for (const auto &property : info.Properties) {
				toml << "\n";
				WriteTable(toml, {"classes", info.Name.Text(), "properties", property.Spelling});
				toml << "name = " << Quote(property.Name.Text()) << "\n";
				toml << "type = " << Quote(PropertyTypeString(property.Type)) << "\n";
				toml << "value_size = " << property.Size << "\n";
				toml << "kind = " << Quote(PropertyKindString(property.Kind)) << "\n";
				toml << "writable = " << (property.Writable ? "true" : "false") << "\n";
				toml << "predicted_writable = " << (property.PredictedWritable ? "true" : "false") << "\n";
				toml << "scriptable = " << (property.Scriptable ? "true" : "false") << "\n";
				toml << "reads = ";
				WriteComponentSet(toml, property.Reads, componentNames);
				toml << "\n";
				toml << "writes = ";
				WriteComponentSet(toml, property.Writes, componentNames);
				toml << "\n";
				if (property.Type == PropertyType::Enum) {
					toml << "enum = " << Quote(property.EnumName.Text()) << "\n";
				}
			}
			toml << "\n";
		}

		for (const auto &[function, source] : functions) {
			WriteTable(toml, {"classes", "Instance", "functions", function});
			toml << "name = " << Quote(function) << "\n";
			toml << "owner = \"Instance\"\n";
			toml << "kind = \"Method\"\n";
			toml << "source = " << Quote(source) << "\n";
			toml << "scriptable = true\n\n";
		}

		for (const engine::script::ServiceRow *row : services) {
			const engine::script::ServiceDefinition &definition = row->Definition;
			const std::vector<std::string> languages = ServiceLanguagesStrings(definition.Languages);
			const std::vector<std::string> capabilities = CapabilityStrings(definition.RequiredCapabilities);
			WriteTable(toml, {"services", definition.Name});
			toml << "name = " << Quote(definition.Name) << "\n";
			toml << "availability = " << Quote(ServiceAvailabilityString(definition.Availability)) << "\n";
			toml << "languages = ";
			WriteStrings(toml, languages);
			toml << "\nrequired_capabilities = ";
			WriteStrings(toml, capabilities);
			toml << "\nsurface = " << (row->Surface != nullptr ? "true" : "false") << "\n";

			if (row->Surface == nullptr) {
				toml << "\n";
				continue;
			}

			const engine::script::ServiceSurface &surface = row->Surface();
			toml << "method_count = " << surface.Methods.size() + surface.LuauMethods.size() << "\n";
			toml << "property_count = " << surface.Properties.size() << "\n";
			toml << "signal_count = " << surface.Signals.size() << "\n";
			for (const engine::script::ServiceMethod &method : surface.Methods) {
				toml << "\n";
				WriteTable(toml, {"services", definition.Name, "methods", method.Name});
				toml << "name = " << Quote(method.Name)
					 << "\nkind = \"Method\"\nlanguages = [\"Luau\", \"JavaScript\"]\n";
			}
			for (const engine::script::LuauServiceMethod &method : surface.LuauMethods) {
				toml << "\n";
				WriteTable(toml, {"services", definition.Name, "luau_methods", method.Name});
				toml << "name = " << Quote(method.Name) << "\nkind = \"Method\"\nlanguages = [\"Luau\"]\n";
			}
			for (const engine::script::ServiceProperty &property : surface.Properties) {
				toml << "\n";
				WriteTable(toml, {"services", definition.Name, "properties", property.Name});
				toml << "name = " << Quote(property.Name)
					 << "\nkind = \"Property\"\nwritable = " << (property.Set != nullptr ? "true" : "false")
					 << "\nlanguages = [\"Luau\", \"JavaScript\"]\n";
			}
			for (const engine::script::ServiceSignal &signal : surface.Signals) {
				toml << "\n";
				WriteTable(toml, {"services", definition.Name, "signals", signal.Name});
				toml << "name = " << Quote(signal.Name)
					 << "\nkind = \"Signal\"\nlanguages = [\"Luau\", \"JavaScript\"]\n";
				if (signal.Property != nullptr) {
					toml << "filter = " << Quote(signal.Property) << "\n";
				}
			}
			toml << "\n";
		}

		for (size_t enumIndex = 0; enumIndex < enums.size(); enumIndex++) {
			const engine::core::Name enumName = enums[enumIndex];
			WriteTable(toml, {"enums", enumName.Text()});
			toml << "name = " << Quote(enumName.Text()) << "\n";
			toml << "items = [";
			const std::vector<engine::core::Name> members = engine::ecs::EnumTable::MembersOf(enumName);
			for (size_t index = 0; index < members.size(); index++) {
				if (index != 0) {
					toml << ", ";
				}
				toml << Quote(members[index].Text());
			}
			toml << "]\n";
			if (enumIndex + 1 < enums.size()) {
				toml << "\n";
			}
		}

		return toml.str();
	}

	std::string DumpSchema() {
		return R"schema(# Atomic Game Engine runtime API reference schema
# Generated by schemadump. Do not edit manually.

[reference]
name = "string - reference title"
format = "atomic-game-engine.runtime-api"
schema_version = 3
generated_by = "schemadump"
source = "runtime registries"

[reference.counts]
components = "integer - records in components"
classes = "integer - records in classes"
properties = "integer - class property records, including inherited properties"
enums = "integer - records in enums"
functions = "integer - shared Instance script methods"
services = "integer - catalogued script services"
service_methods = "integer - methods enumerated by service surfaces"
service_properties = "integer - properties enumerated by service surfaces"
service_signals = "integer - signals enumerated by service surfaces"

# Keys are stable runtime names. Quoted keys preserve dots in component names.
[components]
kind = "table keyed by stable component name"

[components.component]
name = "string - stable component name"
kind = "Tag | Data"
size = "integer - resident component size in bytes"
alignment = "integer - resident component alignment in bytes"
trivial = "boolean - component may be copied as bytes"
serialisable = "boolean - component has a file reader and writer"
raw_serialisation = "boolean - serialisation uses the object representation"
padded = "boolean - raw object representation contains padding"
wire_size = "integer - compact wire bytes, or zero when absent"
described = "boolean - component has a runtime field schema"
tags = "array of strings - optional descriptive component tags"
field_count = "integer - present when described is true"

[components.component.fields]
kind = "table keyed by stable field name, present when described is true"

[components.component.fields.field]
name = "string - stable field name"
type = "Bool | Int32 | Int64 | Float | Double | Name | Enum | String | Reference | Vector3 | CFrame | Color3 | Vector2 | UDim | UDim2 | Rect | NumberRange | NumberSequence | ColorSequence | Opaque"
offset = "integer - byte offset in the component blob"
size = "integer - resident field bytes"
packing = "Native | Float16 | UFloat16 | Float8 | UFloat8 | Int16 | UInt16 | Int8 | UInt8 | Int4 | UInt4 | Bool"
bit_offset = "integer - bit offset inside the resident byte"
storage_bits = "integer - resident field width in bits"
exposed = "boolean - Studio exposes this field as configuration"
enum = "string (optional) - enum name when type is Enum"
tags = "array of strings - descriptive field tags"

[classes]
kind = "table keyed by stable class name"

[classes.class]
name = "string - stable class name"
inherits = "string (optional) - stable parent class name"
creatable = "boolean - authored code may create this exact class"
components = "array of strings - full inherited component set"
property_count = "integer - properties exposed by this class, including inherited properties"

[classes.class.properties]
kind = "table keyed by stable property name"

[classes.class.properties.property]
name = "string - stable property name"
type = "Bool | Int32 | Int64 | Float | Double | Name | Enum | String | Reference | Vector3 | CFrame | Color3 | Vector2 | UDim | UDim2 | Rect | NumberRange | NumberSequence | ColorSequence | Opaque"
value_size = "integer - bytes in the value crossing the property boundary"
kind = "Field | Computed | Structural | Resource"
writable = "boolean - any authoring caller may write"
predicted_writable = "boolean - client-predicted replicas may write"
scriptable = "boolean - property is eligible for script access; writable controls script writes"
reads = "array of strings - components the getter reads"
writes = "array of strings - components the setter changes"
enum = "string (optional) - enum name when type is Enum"

# Instance functions are shared by every script-visible class. Names come from
# the neutral method table or the shared ECS adapter-method catalogue.
[classes.Instance.functions]
kind = "table keyed by stable script method name"

[classes.Instance.functions.function]
name = "string - script method name"
owner = "Instance"
kind = "Method"
source = "neutral | ecs"
scriptable = true

# Services come from the runtime service catalogue. Members are present only
# when a ServiceSurface describes them. A service with surface = false has an
# adapter-specific implementation, so this reference lists its availability and
# language coverage but not invented members.
[services]
kind = "table keyed by stable service name"

[services.service]
name = "string - stable service name"
availability = "Always | Studio"
languages = "array of strings - languages with a binding"
required_capabilities = "array of strings - grants required to install the service"
surface = "boolean - a ServiceSurface enumerates members below"
method_count = "integer - present when surface is true"
property_count = "integer - present when surface is true"
signal_count = "integer - present when surface is true"

[services.service.methods.method]
name = "string - stable method name"
kind = "Method"
languages = "array of strings - languages exposing the method"

[services.service.luau_methods.method]
name = "string - stable method name"
kind = "Method"
languages = ["Luau"]

[services.service.properties.property]
name = "string - stable property name"
kind = "Property"
writable = "boolean - the property has a setter"
languages = "array of strings - languages exposing the property"

[services.service.signals.signal]
name = "string - stable signal name"
kind = "Signal"
languages = "array of strings - languages exposing the signal"
filter = "string (optional) - shared signal filter name"

[enums]
kind = "table keyed by stable enum name"

[enums.enum]
name = "string - stable enum name"
items = "array of strings - members in runtime registration order"
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

	engine::core::Arguments arguments("schemadump", "atomic - dumps the runtime API reference as TOML.");
	arguments.Value("schema", "FILE", "Reference schema TOML path (default: docs/schema.toml)");
	arguments.Value("data", "FILE", "Reference data TOML path (default: docs/schema-data.toml)");

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
