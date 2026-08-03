// The bindings manifest, and the type declarations generated from it.
//
// **One source of truth for what a class is and what a property costs.** The
// class table already holds all of it — `ecs::Classes` knows the tree,
// `PropertyDescriptor` knows the types and the components each side touches —
// so this writes that out rather than restating it. A second hand-maintained
// list of what a script can touch is the thing this exists to prevent.
//
// **No offsets, and that is a property of the design rather than a choice made
// here.** A property is a conversion, so there is no byte offset to leak; every
// identity in the output is a string, which is rule 4 satisfied by construction
// instead of by a disclaimer about which fields survive a recompile.
//
// Run with `--check` it regenerates and compares instead of writing, which is
// what `just bindings-check` uses. The pattern is `expected_graph.json`'s: a
// checked-in expectation, a tool that compares, and a rule that says if the two
// disagree the question is which one is wrong.

#include <engine/core/Arguments.hpp>
#include <engine/core/Log.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/scene/Part.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

	using engine::ecs::Classes;
	using engine::ecs::ClassId;
	using engine::ecs::ClassInfo;
	using engine::ecs::PropertyDescriptor;
	using engine::ecs::PropertyKind;
	using engine::ecs::PropertyType;

	// The format's own version, carried from the first commit.
	//
	// v0.4 bumped a snapshot format and argued that one bump beats two. A
	// manifest that shipped unversioned would make the first change to it an
	// archaeology problem.
	constexpr int MANIFEST_VERSION = 1;

	const char *TypeName(PropertyType type) {
		switch (type) {
		case PropertyType::Bool:
			return "bool";
		case PropertyType::Int32:
			return "int32";
		case PropertyType::Int64:
			return "int64";
		case PropertyType::Float:
			return "float";
		case PropertyType::Double:
			return "double";
		case PropertyType::Name:
			return "Name";
		case PropertyType::Reference:
			return "Instance";
		case PropertyType::Vector3:
			return "Vector3";
		case PropertyType::CFrame:
			return "CFrame";
		case PropertyType::Color3:
			return "Color3";
		case PropertyType::Opaque:
			break;
		}
		return "opaque";
	}

	const char *KindName(PropertyKind kind) {
		switch (kind) {
		case PropertyKind::Field:
			return "field";
		case PropertyKind::Computed:
			return "computed";
		case PropertyKind::Structural:
			return "structural";
		}
		return "field";
	}

	// The script type a property's value appears as, per language.
	const char *LuauType(PropertyType type) {
		switch (type) {
		case PropertyType::Bool:
			return "boolean";
		case PropertyType::Int32:
		case PropertyType::Int64:
		case PropertyType::Float:
		case PropertyType::Double:
			return "number";
		case PropertyType::Name:
			return "string";
		case PropertyType::Vector3:
			return "Vector3";
		case PropertyType::CFrame:
			return "CFrame";
		case PropertyType::Color3:
			return "Color3";
		case PropertyType::Reference:
			return "Instance";
		case PropertyType::Opaque:
			break;
		}
		return "unknown";
	}

	const char *TypeScriptType(PropertyType type) {
		switch (type) {
		case PropertyType::Bool:
			return "boolean";
		case PropertyType::Int32:
		case PropertyType::Int64:
		case PropertyType::Float:
		case PropertyType::Double:
			return "number";
		case PropertyType::Name:
			return "string";
		case PropertyType::Vector3:
			return "Vector3";
		case PropertyType::CFrame:
			return "CFrame";
		case PropertyType::Color3:
			return "Color3";
		case PropertyType::Reference:
			return "Instance";
		case PropertyType::Opaque:
			break;
		}
		return "unknown";
	}

	// The component names a set holds, sorted so the output is stable.
	//
	// **Sorted rather than in registration order**, because registration order
	// depends on which translation unit ran its static initialiser first. A
	// manifest that changed when a link line was reordered would fail its own
	// drift check for a reason nobody could act on.
	std::vector<std::string> ComponentNames(const engine::ecs::ComponentSet *set) {
		std::vector<std::string> names;
		if (set == nullptr) {
			return names;
		}
		for (const engine::ecs::ComponentId id : set->Ids()) {
			names.emplace_back(engine::ecs::Components::Describe(id).Name.Text());
		}
		std::sort(names.begin(), names.end());
		return names;
	}

	void WriteStrings(std::ostringstream &out, const std::vector<std::string> &values) {
		out << "[";
		for (size_t index = 0; index < values.size(); index++) {
			out << (index == 0 ? "" : ", ") << "\"" << values[index] << "\"";
		}
		out << "]";
	}

	// Every registered class, in registration order.
	//
	// Order is the tree's, not a sort: a base is always registered before what
	// derives from it, so this reads top-down and a declaration file generated
	// from it never forward-references.
	std::vector<ClassId> AllClasses() {
		std::vector<ClassId> ids;
		for (size_t index = 0; index < Classes::Count(); index++) {
			ids.emplace_back(static_cast<uint32_t>(index));
		}
		return ids;
	}

	std::string Manifest() {
		std::ostringstream out;
		out << "{\n";
		out << "\t\"_comment\": [\n";
		out << "\t\t\"Generated by mono.tools/bindings. Do not edit by hand.\",\n";
		out << "\t\t\"\",\n";
		out << "\t\t\"What a script can name and what each name costs, taken from the class\",\n";
		out << "\t\t\"table rather than restated beside it. A change here is a change to the\",\n";
		out << "\t\t\"scripting surface, and reviewing the diff is reviewing that change.\",\n";
		out << "\t\t\"\",\n";
		out << "\t\t\"Every identity is a string. There are no byte offsets and no component\",\n";
		out << "\t\t\"ids, because a property is a conversion rather than a field at an\",\n";
		out << "\t\t\"address -- so nothing here goes stale when a struct is reordered or a\",\n";
		out << "\t\t\"subdirectory moves in the link line.\",\n";
		out << "\t\t\"\",\n";
		out << "\t\t\"'reads' and 'writes' are what a getter needs and what a setter\",\n";
		out << "\t\t\"touches. Size writes two components; that is not a mistake.\"\n";
		out << "\t],\n";
		out << "\t\"version\": " << MANIFEST_VERSION << ",\n";
		out << "\t\"classes\": [\n";

		const std::vector<ClassId> ids = AllClasses();
		for (size_t index = 0; index < ids.size(); index++) {
			const ClassInfo &info = Classes::Describe(ids[index]);

			out << "\t\t{\n";
			out << "\t\t\t\"name\": \"" << info.Name.Text() << "\",\n";

			out << "\t\t\t\"parent\": ";
			if (info.Parent.IsValid()) {
				out << "\"" << Classes::Describe(info.Parent).Name.Text() << "\"";
			} else {
				out << "null";
			}
			out << ",\n";

			// Every class registered today is constructible. The field ships
			// now anyway, because v0.9's `UserInputService` is a service — one
			// instance, reachable by name, not a class you construct — and
			// nothing in the table can express that yet. One field now against
			// a format bump later.
			out << "\t\t\t\"constructible\": true,\n";

			out << "\t\t\t\"components\": ";
			WriteStrings(out, ComponentNames(info.Set));
			out << ",\n";

			out << "\t\t\t\"properties\": [\n";
			std::vector<PropertyDescriptor> properties(info.Properties.begin(), info.Properties.end());
			std::sort(
				properties.begin(),
				properties.end(),
				[](const PropertyDescriptor &left, const PropertyDescriptor &right) {
					return left.Name.Text() < right.Name.Text();
				}
			);

			for (size_t property = 0; property < properties.size(); property++) {
				const PropertyDescriptor &described = properties[property];
				out << "\t\t\t\t{";
				out << "\"name\": \"" << described.Name.Text() << "\", ";
				out << "\"type\": \"" << TypeName(described.Type) << "\", ";
				out << "\"kind\": \"" << KindName(described.Kind) << "\", ";
				out << "\"bytes\": " << described.Size << ", ";
				out << "\"writable\": " << (described.Writable ? "true" : "false") << ", ";
				out << "\"reads\": ";
				WriteStrings(out, ComponentNames(described.Reads));
				out << ", \"writes\": ";
				WriteStrings(out, ComponentNames(described.Writes));
				out << "}" << (property + 1 == properties.size() ? "" : ",") << "\n";
			}

			out << "\t\t\t]\n";
			out << "\t\t}" << (index + 1 == ids.size() ? "" : ",") << "\n";
		}

		out << "\t]\n";
		out << "}\n";
		return out.str();
	}

	// The Luau declaration file.
	std::string LuauDeclarations() {
		std::ostringstream out;
		out << "--!strict\n";
		out << "-- Generated by mono.tools/bindings. Do not edit by hand.\n";
		out << "--\n";
		out << "-- What a Luau script can name, typed. Generated from the same manifest the\n";
		out << "-- TypeScript declarations come from, so the two surfaces cannot drift into\n";
		out << "-- two APIs.\n\n";

		out << "export type Vector3 = { X: number, Y: number, Z: number }\n";
		out << "export type Color3 = { R: number, G: number, B: number }\n";
		out << "export type CFrame = { Position: Vector3 }\n\n";

		for (const ClassId id : AllClasses()) {
			const ClassInfo &info = Classes::Describe(id);

			out << "-- " << info.Name.Text();
			if (info.Parent.IsValid()) {
				out << " : " << Classes::Describe(info.Parent).Name.Text();
			}
			out << "\n";
			out << "export type " << info.Name.Text() << " = {\n";
			out << "\tName: string,\n";

			std::vector<PropertyDescriptor> properties(info.Properties.begin(), info.Properties.end());
			std::sort(
				properties.begin(),
				properties.end(),
				[](const PropertyDescriptor &left, const PropertyDescriptor &right) {
					return left.Name.Text() < right.Name.Text();
				}
			);
			for (const PropertyDescriptor &property : properties) {
				out << "\t" << property.Name.Text() << ": " << LuauType(property.Type) << ",\n";
			}
			out << "}\n\n";
		}
		return out.str();
	}

	// The TypeScript declaration file.
	std::string TypeScriptDeclarations() {
		std::ostringstream out;
		out << "// Generated by mono.tools/bindings. Do not edit by hand.\n";
		out << "//\n";
		out << "// What a TypeScript author can name, typed. TypeScript is the typed\n";
		out << "// authoring surface over the JavaScript VM -- it erases its types by\n";
		out << "// design, so this describes what the bindings expose and nothing about\n";
		out << "// how a value is represented at run time.\n\n";

		out << "declare interface Vector3 { readonly X: number; readonly Y: number; readonly Z: number; }\n";
		out << "declare interface Color3 { readonly R: number; readonly G: number; readonly B: number; }\n";
		out << "declare interface CFrame { readonly Position: Vector3; }\n\n";

		out << "declare const Vector3: { new(x?: number, y?: number, z?: number): Vector3 };\n";
		out << "declare const Color3: { new(r?: number, g?: number, b?: number): Color3 };\n";
		out << "declare function print(...values: unknown[]): void;\n\n";

		for (const ClassId id : AllClasses()) {
			const ClassInfo &info = Classes::Describe(id);

			out << "declare interface " << info.Name.Text();
			if (info.Parent.IsValid()) {
				out << " extends " << Classes::Describe(info.Parent).Name.Text();
			}
			out << " {\n";

			if (!info.Parent.IsValid()) {
				out << "\treadonly Name: string;\n";
			}

			// Only what this class declares itself. A derived interface extends
			// its base, so repeating an inherited property here would be a
			// second declaration of one fact — and TypeScript would accept a
			// narrowing of it silently.
			std::vector<PropertyDescriptor> own;
			for (const PropertyDescriptor &property : info.Properties) {
				bool inherited = false;
				if (info.Parent.IsValid()) {
					for (const PropertyDescriptor &above : Classes::Describe(info.Parent).Properties) {
						if (above.Name == property.Name) {
							inherited = true;
							break;
						}
					}
				}
				if (!inherited) {
					own.push_back(property);
				}
			}
			std::sort(own.begin(), own.end(), [](const auto &left, const auto &right) {
				return left.Name.Text() < right.Name.Text();
			});

			for (const PropertyDescriptor &property : own) {
				out << "\t";
				if (!property.Writable) {
					out << "readonly ";
				}
				out << property.Name.Text() << ": " << TypeScriptType(property.Type) << ";\n";
			}
			out << "}\n\n";
		}

		out << "declare const Instance: {\n";
		for (const ClassId id : AllClasses()) {
			const ClassInfo &info = Classes::Describe(id);
			out << "\tnew(className: \"" << info.Name.Text() << "\"): " << info.Name.Text() << ";\n";
		}
		out << "};\n";
		return out.str();
	}

	std::string Read(const std::filesystem::path &path) {
		std::ifstream file(path, std::ios::binary);
		if (!file) {
			return {};
		}
		std::ostringstream contents;
		contents << file.rdbuf();
		return contents.str();
	}

	bool Write(const std::filesystem::path &path, const std::string &contents) {
		std::filesystem::create_directories(path.parent_path());
		std::ofstream file(path, std::ios::binary);
		if (!file) {
			ENGINE_ERROR("could not write {}", path.string());
			return false;
		}
		file << contents;
		return true;
	}

	struct Artefact {
		const char *Name;
		std::filesystem::path Path;
		std::string Contents;
	};
}

int main(int argc, char **argv) {
	engine::core::Log::Initialise("bindings");

	engine::core::Arguments arguments(
		"bindings", "atomic — generates the scripting manifest and declarations."
	);
	arguments.Flag("check", "Compare against the checked-in files instead of writing them");
	arguments.Value("out", "DIR", "Where the generated files live");

	const engine::core::Arguments::Result parsed = arguments.Parse(argc, argv);
	if (!parsed.Ok || parsed.HelpRequested) {
		return parsed.Ok ? 0 : 2;
	}

	// Registering the classes is what populates the table: a manifest generated
	// from an empty table would be a valid file describing nothing, and its
	// drift check would pass forever.
	(void)engine::scene::PartClass();

	const std::filesystem::path directory = arguments.Get("out").has_value()
												? std::filesystem::path(*arguments.Get("out"))
												: std::filesystem::path("mono.engine/script/bindings");

	const std::vector<Artefact> artefacts{
		{"manifest", directory / "manifest.json", Manifest()},
		{"luau declarations", directory / "engine.d.luau", LuauDeclarations()},
		{"typescript declarations", directory / "engine.d.ts", TypeScriptDeclarations()},
	};

	if (!arguments.Has("check")) {
		for (const Artefact &artefact : artefacts) {
			if (!Write(artefact.Path, artefact.Contents)) {
				return 1;
			}
			ENGINE_INFO("wrote {}", artefact.Path.string());
		}
		return 0;
	}

	bool drifted = false;
	for (const Artefact &artefact : artefacts) {
		if (Read(artefact.Path) == artefact.Contents) {
			continue;
		}

		drifted = true;
		ENGINE_ERROR(
			"{} is out of date: {} does not match what the class table says.",
			artefact.Name,
			artefact.Path.string()
		);
	}

	if (drifted) {
		ENGINE_ERROR(
			"The scripting surface changed. Run `just bindings` and review the diff — a change "
			"here is a change to what every script can name."
		);
		return 1;
	}

	ENGINE_INFO("bindings ok — {} artefact(s) match the class table", artefacts.size());
	return 0;
}
