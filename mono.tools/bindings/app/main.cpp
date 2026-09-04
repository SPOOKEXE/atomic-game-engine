// The bindings manifest, and the type declarations generated from it.
//
// **One source of truth for what a class is and what a property costs.** The
// class table already holds all of it - `ecs::Classes` knows the tree,
// `PropertyDescriptor` knows the types and the components each side touches -
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
#include <engine/ecs/EnumTable.hpp>
#include <engine/effects/Registration.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/scene/EditableImage.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Shaders.hpp>
#include <engine/script/Datatypes.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/Vocabulary.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
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
		case PropertyType::String:
			return "String";
		case PropertyType::Enum:
			return "enum";
		case PropertyType::Reference:
			return "Instance";
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
		case PropertyKind::Resource:
			return "resource";
		}
		return "field";
	}

	// The script type a property's value appears as, per language.
	//
	// **Takes the descriptor rather than the type**, because an `Enum` property
	// has no single answer: its script type is the enum it names, and that is
	// carried on the descriptor. A function of the type alone could only have
	// said `EnumItem`, which is the shape and not the type - and would have
	// given an author completion for every enum in the engine at once.
	std::string LuauType(const PropertyDescriptor &property) {
		if (property.Type == PropertyType::Enum) {
			// **`Enum.Material`, the same as the TypeScript half**, and getting
			// here took a correction worth recording rather than editing out.
			//
			// This used to emit `Enum_Material` with a comment arguing that Luau
			// *could not* express the dotted form. That was wrong. What is true is
			// that a **definitions file** cannot declare one: `loadDefinitionFile`
			// only ever writes `exportedTypeBindings[name]`, a flat name, and
			// there is no `declare` syntax for anything else.
			//
			// What resolves `Enum.Material` is `Scope::lookupImportedType("Enum",
			// "Material")` - the `importedTypeBindings` map, which `require`
			// populates and which a **host** may populate directly. That is
			// precisely what Roblox does, and it is what
			// `mono.tools/scriptcheck` now does: it walks the `Enum_*` types this
			// file emits and aliases each under the `Enum` prefix.
			//
			// **The declaration file keeps the flat name and scripts get both**,
			// which is forced by the order things happen in rather than chosen.
			// The aliases are registered *after* `loadDefinitionFile` returns -
			// they are built by walking the `Enum_*` types it created - so this
			// file cannot use the dotted form in its own declarations. Emitting it
			// here made the file fail to load with "Unknown type
			// 'Enum.AspectType'" before a single script was checked.
			//
			// So: the vocabulary is declared flat, and `Enum.Material` is an alias
			// pointing at the same `TypeFun`. A script may write either, and they
			// are the same type rather than two that agree.
			return std::string("Enum_") + property.EnumName.Text().data();
		}

		switch (property.Type) {
		case PropertyType::Bool:
			return "boolean";
		case PropertyType::Int32:
		case PropertyType::Int64:
		case PropertyType::Float:
		case PropertyType::Double:
			return "number";
		case PropertyType::Name:
		case PropertyType::String:
			// **The same word in both languages, and it should be.** Whether
			// the engine interns text or owns it is a storage decision an
			// author has no way to observe from a script - `label.Text` is a
			// string either way. A declaration file that spelled them
			// differently would be leaking a C++ concern into a type position.
			return "string";
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
		case PropertyType::Reference:
			return "Instance";
		case PropertyType::Enum:
		case PropertyType::Opaque:
			break;
		}
		return "unknown";
	}

	std::string TypeScriptType(const PropertyDescriptor &property) {
		if (property.Type == PropertyType::Enum) {
			// **`Enum.Material`, which is what a Roblox script spells**, and this
			// is the half of `ROADMAP.md` v0.10's rename that a type system can
			// actually express. TypeScript merges an `interface Material` and a
			// `const Material` inside `namespace Enum`, so one name is both the
			// type and the table of members - see the namespace this emits below.
			//
			// The Luau half cannot do this and `LuauType` says why.
			return std::string("Enum.") + property.EnumName.Text().data();
		}

		switch (property.Type) {
		case PropertyType::Bool:
			return "boolean";
		case PropertyType::Int32:
		case PropertyType::Int64:
		case PropertyType::Float:
		case PropertyType::Double:
			return "number";
		case PropertyType::Name:
		case PropertyType::String:
			// **The same word in both languages, and it should be.** Whether
			// the engine interns text or owns it is a storage decision an
			// author has no way to observe from a script - `label.Text` is a
			// string either way. A declaration file that spelled them
			// differently would be leaking a C++ concern into a type position.
			return "string";
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
		case PropertyType::Reference:
			return "Instance";
		case PropertyType::Enum:
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

	// Whether `Instance.new` should offer this class.
	//
	// The class table owns this answer. Runtime creation, generated overloads
	// and Studio all read the same exact-class flag, while `IsA` keeps using the
	// ancestry regardless of whether the named base can be created itself.
	bool Constructible(ClassId id) {
		const ClassInfo &info = Classes::Describe(id);
		return info.Name.IsValid() && info.Creatable;
	}

	// The classes a script reaches through `GetService`.
	//
	// **Derived from the class table rather than listed by hand, and that is
	// the fix rather than the tidy-up.** `GetService` resolves a name against
	// the world's roots - `RunService.cpp` says so - so *every* service
	// `scene::InstallServices` furnishes is reachable, and the hand-written
	// list of five said otherwise. `Interface.luau` asking for `StarterGui`
	// typechecked as an error against an engine that answers it perfectly well.
	//
	// A hand-written list has to be edited every time a service is added, and
	// nothing fails when it is not: the run time keeps working and only the
	// types disagree. That is the drift this file exists to prevent everywhere
	// else, arriving through the one list that was still manual.
	//
	// The bus services are *not* here - they are globals installed by the
	// bindings rather than instances in the tree - so they are appended by the
	// caller. Every class carrying `ServiceComponent` is.
	std::vector<ClassId> ServiceClasses() {
		const ClassId service = Classes::Find(engine::core::Name("Service"));

		std::vector<ClassId> ids;
		for (const ClassId id : AllClasses()) {
			const ClassInfo &info = Classes::Describe(id);
			if (id != service && info.Set != nullptr &&
				info.Set->Contains(engine::ecs::Components::Of<engine::scene::ServiceComponent>())) {
				ids.push_back(id);
			}
		}
		return ids;
	}

	// The classes a script can construct, which is what both `Instance.new`
	// overload lists are built from.
	std::vector<ClassId> ConstructibleClasses() {
		std::vector<ClassId> ids;
		for (const ClassId id : AllClasses()) {
			if (Constructible(id)) {
				ids.push_back(id);
			}
		}
		return ids;
	}

	// The properties a class declares itself, sorted by name.
	//
	// **Own, not inherited.** Both declaration files now express inheritance -
	// TypeScript through `extends` on an interface and Luau through `extends` on
	// a `declare extern type` - so repeating an inherited property would be a
	// second declaration of one fact. TypeScript accepts a narrowing of one
	// silently, and Luau's would simply be noise growing with the depth of the
	// tree.
	std::vector<PropertyDescriptor> OwnProperties(const ClassInfo &info) {
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
		return own;
	}

	// Every way a property descriptor can contradict itself, checked against
	// the whole class table.
	//
	// **The manifest publishes `reads` and `writes`, so a descriptor that
	// declares the wrong thing is not a private slip - it is a wrong sentence in
	// a checked-in contract that script authors and both declaration files are
	// generated from.** Four read-only properties declared a write set:
	// `Attachment.WorldCFrame` and `WorldPosition`, `Humanoid.Grounded` and
	// `MeshPart.TrianglesCount` each said they wrote the component they merely
	// project. The manifest said so too, in the file, for two versions.
	//
	// **Here rather than in a suite**, because this is the one binary that
	// registers every class in the engine: `scene`, `script`, `effects`, `gui`
	// and the services all arrive in `main` below, and no test target links the
	// set. It runs on `--check`, which `just check` runs, so a descriptor that
	// drifts fails the build rather than the next reader.
	//
	// **What is deliberately not checked is an empty `reads` set.**
	// `Players.LocalPlayer` has one and is right to: it projects a world
	// *resource*, not a component on the row. The consequence - a property that
	// reads no component can never fire `.Changed`, because `script::ChangeQueue`
	// subscribes through `reads` - is a real limitation and not a contradiction,
	// and that descriptor says so in its own comment.
	//
	// @return How many contradictions were reported.
	size_t ReportDescriptorFaults() {
		size_t faults = 0;

		for (const ClassId id : AllClasses()) {
			const ClassInfo &info = Classes::Describe(id);
			for (const PropertyDescriptor &property : info.Properties) {
				// A property that cannot be written writes nothing. The
				// converse is not a fault: a writable property may legitimately
				// write nothing on a class that has no storage for it, which is
				// how `Set` reports `false` rather than aborting.
				if (!property.Writable && property.Writes != nullptr && !property.Writes->Ids().empty()) {
					ENGINE_ERROR(
						"{}.{} is read-only and declares a write set. `writes` reaches the "
						"manifest; a read-only property must write nothing.",
						info.Name.Text(),
						property.Name.Text()
					);
					faults++;
				}

				// **And the converse, which is what the studio's scale gizmo
				// went round.** `scene::SizeProperty` writes `Bounds` *and*
				// `Collider` - a caller that moved only the first left a part
				// drawn at one size and collided at another. A writable property
				// that declares nothing is a setter whose blast radius is
				// undocumented, and the manifest is where a caller would look.
				if (property.Writable && (property.Writes == nullptr || property.Writes->Ids().empty())) {
					ENGINE_ERROR(
						"{}.{} is writable and declares no write set. `writes` reaches the "
						"manifest; say what the setter touches.",
						info.Name.Text(),
						property.Name.Text()
					);
					faults++;
				}
			}
		}

		return faults;
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

			// **Answered from the tree rather than asserted.** This shipped
			// hard-coded `true` against the day a service arrived. Both declaration
			// files now use the class table's creatable flag.
			out << "\t\t\t\"constructible\": " << (Constructible(ids[index]) ? "true" : "false") << ",\n";

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
				if (described.Type == PropertyType::Enum) {
					// Only on an enum property, so the shape of every other row
					// is unchanged and the diff of this file reads as what
					// actually moved.
					out << "\"enum\": \"" << described.EnumName.Text() << "\", ";
				}
				out << "\"reads\": ";
				WriteStrings(out, ComponentNames(described.Reads));
				out << ", \"writes\": ";
				WriteStrings(out, ComponentNames(described.Writes));
				out << "}" << (property + 1 == properties.size() ? "" : ",") << "\n";
			}

			out << "\t\t\t]\n";
			out << "\t\t}" << (index + 1 == ids.size() ? "" : ",") << "\n";
		}

		out << "\t],\n";

		// **The enums, listed after the classes that name them.** A binding
		// generator that emitted a property typed `Enum.Material` without
		// saying what `Material` holds would be describing half a contract, and
		// an editor reading this file could offer no completion at all.
		//
		// Members in registration order rather than sorted: the order an author
		// reads them in should be the order they were declared, which is usually
		// meaningful - `Static`, `Kinematic`, `Dynamic` is a progression and
		// alphabetical is not. The enum *names* are sorted, because those come
		// from whichever translation unit ran first.
		out << "\t\"enums\": [\n";

		std::vector<engine::core::Name> enums = engine::ecs::EnumTable::Names();
		std::sort(enums.begin(), enums.end(), [](engine::core::Name left, engine::core::Name right) {
			return left.Text() < right.Text();
		});

		for (size_t index = 0; index < enums.size(); index++) {
			std::vector<std::string> members;
			for (const engine::core::Name member : engine::ecs::EnumTable::MembersOf(enums[index])) {
				members.emplace_back(member.Text());
			}

			out << "\t\t{\"name\": \"" << enums[index].Text() << "\", \"members\": ";
			WriteStrings(out, members);
			out << "}" << (index + 1 == enums.size() ? "" : ",") << "\n";
		}

		out << "\t]\n";
		out << "}\n";
		return out.str();
	}

	// Every registered enum, sorted by name.
	std::vector<engine::core::Name> SortedEnums() {
		std::vector<engine::core::Name> enums = engine::ecs::EnumTable::Names();
		std::sort(enums.begin(), enums.end(), [](engine::core::Name left, engine::core::Name right) {
			return left.Text() < right.Text();
		});
		return enums;
	}

	// The hand-written half of the Luau file: what the VM installs rather than
	// what the class table holds.
	//
	// **A nominal type rather than a table type, and the operators are why.**
	// `part.CFrame * CFrame.Angles(0, angle, 0)` is the idiom every rotation in
	// this repository is written with, and a table type has nowhere to put a
	// `__mul`. The same syntax is what lets the class tree below express
	// `extends`, so the Luau half stopped repeating every inherited property.
	//
	// **`declare extern type X with ... end`, not `declare class X ... end`, and
	// the difference is not cosmetic.** Both spellings parse in the Luau this
	// repository vendors, but the class form is gated behind
	// `FFlag::LuauAllowGlobalDeclarationToBeCalledClass` - whose own comment says
	// the plan is to remove `declare class X [extends Y]`. Anything that enables
	// Luau's flags therefore rejects the class form outright, and `luau-lsp` does
	// exactly that: pointed at a file using it, the language server fails to load
	// the definitions at all and an author gets *no* completion rather than
	// slightly wrong completion. `mono.tools/scriptcheck` runs with default flags
	// and accepts both, so this generator would not have caught it - `just
	// luau-lsp` did, which is the argument for that recipe existing.
	//
	// **The signals are three classes because a definition file cannot declare a
	// generic one.** Roblox spells this `RBXScriptSignal<T...>` and that syntax
	// does not parse here - so each signal is named for what it hands its
	// handler, which is the thing an author actually wants completed. Same
	// `Connect`, same `Once`, same `RBXScriptConnection` back.
	constexpr const char *LUAU_PRELUDE =
		R"LUAU(-- --- the value types -------------------------------------------------------

declare extern type Vector3 with
	X: number
	Y: number
	Z: number
	Magnitude: number
	Unit: Vector3
	function Abs(self): Vector3
	function Ceil(self): Vector3
	function Floor(self): Vector3
	function Sign(self): Vector3
	function Cross(self, other: Vector3): Vector3
	function Dot(self, other: Vector3): number
	-- Unsigned without an axis, and signed by which side of `axis` the turn goes
	-- with one. The unsigned form cannot tell left from right.
	function Angle(self, other: Vector3, axis: Vector3?): number
	function FuzzyEq(self, other: Vector3, epsilon: number?): boolean
	function Lerp(self, goal: Vector3, alpha: number): Vector3
	function Max(self, vector: Vector3): Vector3
	function Min(self, vector: Vector3): Vector3
	function __add(self, other: Vector3): Vector3
	function __sub(self, other: Vector3): Vector3
	function __mul(self, other: Vector3 | number): Vector3
	-- The vector has to be on the left of `/` and `//`. Roblox's operator table
	-- has no `number / Vector3` row, and `2 * v` is the one that does.
	function __div(self, other: Vector3 | number): Vector3
	function __idiv(self, other: Vector3 | number): Vector3
	function __unm(self): Vector3
end

declare Vector3: {
	new: (x: number?, y: number?, z: number?) -> Vector3,

	-- Capitalised, because Roblox capitalises these two and leaves `new` lower
	-- case. It reads as an inconsistency and is one, but it is the one a script
	-- written elsewhere already contains.
	FromNormalId: (normal: Enum_NormalId) -> Vector3,
	FromAxis: (axis: Enum_Axis) -> Vector3,

	-- **Lowercase, because Roblox's are.** Every other member of this vocabulary
	-- is capitalised and these five are not, which reads as a mistake until you
	-- try to run a script written elsewhere.
	zero: Vector3,
	one: Vector3,
	xAxis: Vector3,
	yAxis: Vector3,
	zAxis: Vector3,
}

declare extern type Color3 with
	R: number
	G: number
	B: number
end

declare Color3: {
	new: (r: number?, g: number?, b: number?) -> Color3,
	-- 0-255, the way an author reads a colour off a palette.
	fromRGB: (r: number?, g: number?, b: number?) -> Color3,
}

-- `CFrame * Vector3` takes a point into the frame's space and is the other half
-- of one run-time metamethod. It is not declared: `__mul` is one member here, so
-- the two argument types would have to share a return type, and a `CFrame |
-- Vector3` that every call site had to narrow is worse than the composition
-- overload alone.
declare extern type CFrame with
	Position: Vector3
	-- Roblox's alias for `Position`, kept because a long Roblox habit types it.
	p: Vector3
	X: number
	Y: number
	Z: number

	-- This frame's rotation at the world origin.
	Rotation: CFrame

	-- The three basis columns. `XVector`/`YVector` are the same directions as
	-- `RightVector`/`UpVector`; `ZVector` is the negation of `LookVector`, which
	-- is the one pair that differ.
	RightVector: Vector3
	UpVector: Vector3
	LookVector: Vector3
	XVector: Vector3
	YVector: Vector3
	ZVector: Vector3

	function __mul(self, other: CFrame): CFrame
	function __add(self, offset: Vector3): CFrame
	function __sub(self, offset: Vector3): CFrame

	function Inverse(self): CFrame
	function Orthonormalize(self): CFrame
	function Lerp(self, goal: CFrame, alpha: number): CFrame
	function ToWorldSpace(self, other: CFrame): CFrame
	function ToObjectSpace(self, other: CFrame): CFrame
	function PointToWorldSpace(self, point: Vector3): Vector3
	function PointToObjectSpace(self, point: Vector3): Vector3
	function VectorToWorldSpace(self, direction: Vector3): Vector3
	function VectorToObjectSpace(self, direction: Vector3): Vector3
	function AngleBetween(self, other: CFrame): number
	function FuzzyEq(self, other: CFrame, epsilon: number?): boolean

	-- Twelve values: the position, then the rotation matrix row by row. That is
	-- the order the twelve-argument `CFrame.new` takes them back in.
	function GetComponents(self): (number, number, number, number, number, number, number, number, number, number, number, number)
	function components(self): (number, number, number, number, number, number, number, number, number, number, number, number)

	function ToEulerAnglesXYZ(self): (number, number, number)
	function ToEulerAnglesYXZ(self): (number, number, number)
	function ToOrientation(self): (number, number, number)
	function ToAxisAngle(self): (Vector3, number)
end

declare CFrame: {
	new: ((x: number?, y: number?, z: number?) -> CFrame)
		& ((position: Vector3) -> CFrame)
		& ((x: number, y: number, z: number, qX: number, qY: number, qZ: number, qW: number) -> CFrame)
		& ((x: number, y: number, z: number, r00: number, r01: number, r02: number, r10: number, r11: number, r12: number, r20: number, r21: number, r22: number) -> CFrame),

	-- The identity: no turn, at the origin.
	identity: CFrame,

	-- Radians, because Roblox's is radians - while `Orientation` is degrees.
	--
	-- **X, then Y, then Z**, which is what Roblox means by `Angles` and by
	-- `fromEulerAnglesXYZ`; the two are one function. Until v0.18 this name was
	-- bound to the Y-X-Z composition, so a script pasted from Roblox turned the
	-- wrong way whenever it named two axes at once.
	Angles: (rx: number, ry: number, rz: number) -> CFrame,
	fromEulerAnglesXYZ: (rx: number, ry: number, rz: number) -> CFrame,

	-- Y, then X, then Z. The order `BasePart.Orientation` round-trips through,
	-- here and in Roblox. Two names for one function, as Roblox has.
	fromEulerAnglesYXZ: (rx: number, ry: number, rz: number) -> CFrame,
	fromOrientation: (rx: number, ry: number, rz: number) -> CFrame,

	lookAt: (from: Vector3, to: Vector3, up: Vector3?) -> CFrame,
	lookAlong: (at: Vector3, direction: Vector3, up: Vector3?) -> CFrame,
	fromAxisAngle: (axis: Vector3, angle: number) -> CFrame,

	-- The third basis vector is determined by the first two, so a fourth
	-- argument is accepted and ignored rather than honoured into a basis that is
	-- not a rotation.
	fromMatrix: (position: Vector3, vX: Vector3, vY: Vector3, vZ: Vector3?) -> CFrame,
	fromRotationBetweenVectors: (from: Vector3, to: Vector3) -> CFrame,
}

-- --- signals ---------------------------------------------------------------

declare extern type RBXScriptConnection with
	Connected: boolean
	function Disconnect(self): ()
end

declare extern type HeartbeatSignal with
	function Connect(self, handler: (deltaTime: number) -> ()): RBXScriptConnection
	function Once(self, handler: (deltaTime: number) -> ()): RBXScriptConnection
end

declare extern type ChangedSignal with
	function Connect(self, handler: (property: string) -> ()): RBXScriptConnection
	function Once(self, handler: (property: string) -> ()): RBXScriptConnection
end

-- --- input ------------------------------------------------------------------
--
-- **Hand-written, because these two are script globals rather than classes.**
-- `RunService` and `MessagingService` are declared the same way for the same
-- reason: nothing about them is in the class table, so the generator has no way
-- to derive them and writing them out is the honest form.

-- What an input signal hands its listener.
--
-- **Roblox's `InputObject`, added at v0.16 because the three signals were
-- passing something no declaration described.** `InputBegan` fired with a bare
-- `Enum.KeyCode`, the bound-action handler took one as its third argument, and
-- these declarations said the signals passed *nothing at all* - three answers to
-- one question, none of them Roblox's. A script copied from a Roblox place
-- indexed `input.KeyCode` and got nil.
--
-- No constructor: a script cannot make one, because an input report is a fact
-- about a frame rather than a value anybody authors. Roblox offers none either.
declare extern type InputObject with
	-- The key, or `Enum.KeyCode.Unknown` for anything that is not one.
	read KeyCode: Enum_KeyCode

	-- Where it came from: mouse, keyboard or one of eight controller slots.
	-- `Enum.UserInputType` is shorter here than in Roblox; every absent member is
	-- one nothing in this engine can produce.
	read UserInputType: Enum_UserInputType

	-- `Begin`, `Change` or `End`.
	read UserInputState: Enum_UserInputState

	-- Where the pointer was, in pixels from the top-left of the window, with the
	-- wheel's notches in `Z`. Zero for a keyboard event.
	read Position: Vector3

	-- How far it moved since the previous frame, in the same space. Zero for a
	-- keyboard event and for a button edge - motion is reported by its own
	-- `InputChanged`, so counting it on the click as well would double it.
	read Delta: Vector3
end

-- What `InputBegan`, `InputEnded` and `InputChanged` call a handler with.
--
-- Its own type rather than `PropertyChangedSignal`, which these three were
-- declared as and which passes nothing.
--
-- **`gameProcessedEvent` is Roblox's second argument and it is real here**: it
	-- is true when the 2D interface took the pointer or keyboard this beat. A
	-- controller report is not claimed by the interface and answers false.
declare extern type InputSignal with
	function Connect(
		self, handler: (input: InputObject, gameProcessedEvent: boolean) -> ()
	): RBXScriptConnection
	function Once(
		self, handler: (input: InputObject, gameProcessedEvent: boolean) -> ()
	): RBXScriptConnection
end

-- What `LastInputTypeChanged` calls a handler with.
declare extern type LastInputTypeSignal with
	function Connect(self, handler: (lastInputType: Enum_UserInputType) -> ()): RBXScriptConnection
	function Once(self, handler: (lastInputType: Enum_UserInputType) -> ()): RBXScriptConnection
end

-- What the gamepad connection signals call a handler with.
declare extern type GamepadConnectionSignal with
	function Connect(self, handler: (gamepad: Enum_UserInputType) -> ()): RBXScriptConnection
	function Once(self, handler: (gamepad: Enum_UserInputType) -> ()): RBXScriptConnection
end

declare extern type UserInputServiceType with
	MouseBehavior: Enum_MouseBehavior

	-- Whether the pointer is drawn. **Separate from `MouseBehavior` because
	-- Roblox's two are**: an inventory screen wants the pointer visible while the
	-- camera stays locked. `LockCenter` and `LockCurrentPosition` hide it
	-- whatever this says, which is the client's rule and not a script's.
	MouseIconEnabled: boolean

	MouseDeltaSensitivity: number
	read KeyboardEnabled: boolean
	read MouseEnabled: boolean
)LUAU"
		// **Two literals rather than one, and they are concatenated by the
		// compiler.** MSVC caps a single string literal at 16380 bytes and
		// truncates the rest with `error C2026: string too big` - a *warning*
		// in shape, an error in effect, and one that would have shipped a
		// half-written declaration file if it had been the former. GCC and
		// Clang have no such cap, which is why this was only ever a problem
		// on the platform nobody builds daily.
		//
		// Adjacent literals are joined at translation phase 6, so the string
		// this produces is byte for byte the one it produced before - which
		// `just bindings-check` proves by regenerating every artefact and
		// comparing. Do not rejoin them.
		R"LUAU(
	-- Live when any mapped gamepad or raw joystick is connected.
	read GamepadEnabled: boolean

	-- These devices are not implemented, so their capability flags stay false.
	read TouchEnabled: boolean
	read VREnabled: boolean
	read AccelerometerEnabled: boolean
	read GyroscopeEnabled: boolean

	read InputBegan: InputSignal
	read InputEnded: InputSignal

	-- Fires for pointer motion, wheel motion, controller sticks and triggers.
	-- **It fired for nothing at all before v0.16**, having been connectable and
	-- silent since v0.10.
	read InputChanged: InputSignal
	read GamepadConnected: GamepadConnectionSignal
	read GamepadDisconnected: GamepadConnectionSignal

	-- The window's focus edges. Everything reads as released on the frame focus
	-- is lost, and `WindowFocusReleased` is delivered before those releases so a
	-- listener can tell which explains which.
	read WindowFocused: PropertyChangedSignal
	read WindowFocusReleased: PropertyChangedSignal

	-- Which device the player switched to. **The edge beside
	-- `GetLastInputType`'s poll**, which is the pair a place needs to swap
	-- "press E" for "click here" and to know which to draw when a menu opens.
	read LastInputTypeChanged: LastInputTypeSignal

	function IsKeyDown(self, key: Enum_KeyCode): boolean

	-- False for every `Enum.UserInputType` member that is not a mouse button.
	function IsMouseButtonPressed(self, button: Enum_UserInputType): boolean

	function GetMouseLocation(self): Vector2
	function GetMouseDelta(self): Vector2
	function GetKeysPressed(self): { Enum_KeyCode }

	-- `InputObject`s and not `EnumItem`s, which is Roblox's shape: the object
	-- carries where the pointer was as well as which button it is.
	function GetMouseButtonsPressed(self): { InputObject }

	-- **`Enum.UserInputType.Keyboard` on a world nobody has touched**, where
	-- Roblox answers `None`. There is no such member here for `InputSource`'s
	-- reason: every member is one something in this engine can produce.
	function GetLastInputType(self): Enum_UserInputType

	-- The `TextBox` a person is typing into, or nil.
	--
	-- **A lookup rather than a tally of the focus signals**, because
	-- `gui::Focus` is the one door a focus change goes through and
	-- `GuiServiceState::FocusedTextBox` is where the answer rests. Nil for a
	-- world with no `GuiService`, and nil for a box destroyed while focused.
	function GetFocusedTextBox(self): Instance?

	function GetConnectedGamepads(self): { Enum_UserInputType }
	function GetGamepadState(self, gamepad: Enum_UserInputType): { InputObject }
	function GetSupportedGamepadKeyCodes(self, gamepad: Enum_UserInputType): { Enum_KeyCode }
	function IsGamepadButtonDown(self, gamepad: Enum_UserInputType, button: Enum_KeyCode): boolean
end

-- **What `UserInputService` deliberately does not have**, which is the half a
-- migrating author needs most. `script/src/UserInputService.cpp` names each and
-- what closing it would take; the short version is that there is no touch
-- surface, cursor image in `render` or keyboard-layout query below L12, so the
-- six touch signals, `MouseIcon` and `GetStringForKeyCode` remain absent.
-- `TextBoxFocused` and its twin are absent for a different reason: the
-- fact exists and reaches a script as `textBox.Focused`, and a world-subject row
-- firing about an instance would put the two input pumps in each other's work.

declare UserInputService: UserInputServiceType

-- What `GetBoundActionInfo` reports about one bound action.
--
-- **Four of Roblox's six fields.** `title` and `description` come from
-- `SetTitle` and `SetDescription`, which decorate a touch button - there is no
-- touch surface, so the pair is not bound and reporting them as empty strings
-- would claim they had been set to nothing.
declare extern type BoundActionInfo with
	-- The keys this action claims. **Keys only**: `BindAction` takes
	-- `Enum.KeyCode` here where Roblox also takes `Enum.UserInputType`.
	read inputTypes: { Enum_KeyCode }

	read priorityLevel: number

	-- **Higher wins, which is Roblox's direction.** The engine's own list is
	-- sorted highest-priority-first, so this is that index inverted rather than
	-- the same word meaning the opposite thing.
	read stackOrder: number

	-- Always false: the argument is accepted at bind time and ignored.
	read createTouchButton: boolean
end

declare extern type ContextActionServiceType with
	-- The touch-button argument is accepted and ignored: there is no touch
	-- surface, and refusing it would make a Roblox script fail on a line that
	-- describes something this engine simply does not have.
	--
	-- **The handler's third argument is an `InputObject` since v0.16**, which is
	-- what Roblox has always passed. It was an `Enum.KeyCode`, so a handler
	-- reading `input.KeyCode` - the form every Roblox place is written in - got
	-- nil.
	--
	-- **What it returns decides who else hears the key.**
	-- `Enum.ContextActionResult.Pass` hands the press to the next claim down;
	-- `Sink`, `nil`, or a handler that raised, stop it here. Returning nothing is
	-- the ordinary case and sinks, which is Roblox's default.
	function BindAction(
		self,
		name: string,
		handler: (string, Enum_UserInputState, InputObject) -> Enum_ContextActionResult?,
		createTouchButton: boolean,
		...: Enum_KeyCode
	): ()

	function BindActionAtPriority(
		self,
		name: string,
		handler: (string, Enum_UserInputState, InputObject) -> Enum_ContextActionResult?,
		createTouchButton: boolean,
		priority: number,
		...: Enum_KeyCode
	): ()

	function UnbindAction(self, name: string): ()
	function UnbindAllActions(self): ()

	-- `nil` for a name nothing has bound, which is what `if info then` reads.
	function GetBoundActionInfo(self, name: string): BoundActionInfo?

	function GetAllBoundActionInfo(self): { [string]: BoundActionInfo }
end

declare ContextActionService: ContextActionServiceType

declare extern type SettingsMenuActionSignal with
	function Connect(self, handler: (name: string) -> ()): RBXScriptConnection
	function Once(self, handler: (name: string) -> ()): RBXScriptConnection
end

declare extern type SettingsServiceType with
	-- Adds or relabels one named row and returns that row's activation signal.
	-- Client scripts only. At most twelve actions may exist at once.
	function SetMenuAction(self, name: string, label: string): SettingsMenuActionSignal
	function RemoveMenuAction(self, name: string): boolean
	function ClearMenuActions(self): ()
end

declare SettingsService: SettingsServiceType

declare extern type PropertyChangedSignal with
	function Connect(self, handler: () -> ()): RBXScriptConnection
	function Once(self, handler: () -> ()): RBXScriptConnection
end

-- The instance tree's signals, in two shapes because the arguments differ.
--
-- **These were undeclared until v0.13**, which is the same omission the
-- generator's own note about `AddTag` describes: `ChildAdded` and its four
-- neighbours have been answered by `InstanceIndex` since v0.6 and named
-- nowhere here, so every script connecting one typechecked as an error against
-- a signal the engine hands back. A host member is only declared where it is
-- written down.
declare extern type InstanceSignal with
	function Connect(self, handler: (instance: Instance) -> ()): RBXScriptConnection
	function Once(self, handler: (instance: Instance) -> ()): RBXScriptConnection
end

-- `AncestryChanged` is the one that takes two: what moved, and where it is now.
declare extern type AncestrySignal with
	function Connect(self, handler: (instance: Instance, parent: Instance) -> ()): RBXScriptConnection
	function Once(self, handler: (instance: Instance, parent: Instance) -> ()): RBXScriptConnection
end

-- The 2D tree's input, in two shapes because the arguments differ.
--
-- **`GuiSignal` takes no arguments and that is deliberate rather than
-- unfinished.** Roblox hands `Activated`, `InputBegan` and `InputEnded` an
-- `InputObject`, and a handler that cannot rely on an argument is better than
-- one relying on an argument that will move.
--
-- **The datatype exists since v0.16 and this still takes none**, which is a
-- narrower gap than it was. `gui::GuiEvent` carries a kind, an entity and two
-- points - no key, no button, no `Enum.UserInputType` - so an `InputObject`
-- built for these would have to *invent* the field a handler reads it for. What
-- closing it needs is `gui::Router` recording which button produced an event.
declare extern type GuiSignal with
	function Connect(self, handler: () -> ()): RBXScriptConnection
	function Once(self, handler: () -> ()): RBXScriptConnection
end

-- `tween.Completed`, which takes no arguments.
--
-- **Its own type rather than `GuiSignal`**, which is structurally identical: a
-- name is what a reader gets from a declaration file, and a tween's completion
-- typed as a gui signal is a name that is simply false. Roblox passes a
-- `PlaybackState` here; this engine has no such enum, and one invented now
-- would have to change the day it arrives.
declare extern type TweenCompletedSignal with
	function Connect(self, handler: () -> ()): RBXScriptConnection
	function Once(self, handler: () -> ()): RBXScriptConnection
end

-- `(x, y)` in canvas pixels, which is Roblox's signature for the three pointer
-- signals exactly.
declare extern type PointerSignal with
	function Connect(self, handler: (x: number, y: number) -> ()): RBXScriptConnection
	function Once(self, handler: (x: number, y: number) -> ()): RBXScriptConnection
end

-- `uiDragDetector.DragStart`, `.DragContinue` and `.DragEnd`.
--
-- **Four numbers: where the pointer is, and how far it has come.** Roblox hands
-- a drag handler an `InputObject`; this engine has none for the reason
-- `GuiSignal` above gives, and the two things a handler actually reads off one
-- are exactly these. The travel is zero on `DragStart`, which is what "how far
-- has this gesture gone" means at its first frame.
declare extern type DragSignal with
	function Connect(self, handler: (x: number, y: number, dx: number, dy: number) -> ()): RBXScriptConnection
	function Once(self, handler: (x: number, y: number, dx: number, dy: number) -> ()): RBXScriptConnection
end

-- `textBox.Focused`, which takes no arguments.
--
-- **Its own name rather than `GuiSignal`**, for `TweenCompletedSignal`'s
-- reason: the two are structurally identical and a name is what a reader gets
-- out of a declaration file. This one is about the keyboard and the other three
-- are about the pointer.
declare extern type FocusSignal with
	function Connect(self, handler: () -> ()): RBXScriptConnection
	function Once(self, handler: () -> ()): RBXScriptConnection
end

-- `textBox.FocusLost`, which takes `enterPressed` and nothing after it.
--
-- **One of Roblox's two arguments.** The second is the `InputObject` that took
-- the focus away, and the router deals in `gui::GuiEvent` - there is no report
-- to hand over that would not be invented at the call site, which is the trade
-- `GuiSignal` above is on.
--
-- `enterPressed` is true when Return released the box and false when a press
-- somewhere else did, which is how a handler tells a submitted field from an
-- abandoned one.
declare extern type FocusLostSignal with
	function Connect(self, handler: (enterPressed: boolean) -> ()): RBXScriptConnection
	function Once(self, handler: (enterPressed: boolean) -> ()): RBXScriptConnection
end

-- --- queries ---------------------------------------------------------------

declare extern type RaycastParams with
	CollisionGroup: string

	-- The most entities an overlap or a cast will report. Ignored by `Raycast`,
	-- which reports one hit or none.
	--
	-- A result exactly this long may have been truncated, which is the author's
	-- own instruction rather than a silent loss. Clamped to 65536.
	MaxParts: number
end

declare RaycastParams: {
	new: () -> RaycastParams,
}

)LUAU";

	// The rest of the datatype vocabulary, emitted after the enums because
	// `TweenInfo` names two of them.
	//
	// **`Region3` is `core::AABB` and `Ray` is `core::Ray`**, which is why
	// neither carries a rotation: Roblox's `Region3` is an axis-aligned box and
	// the engine already had that box under the name every spatial query uses.
	//
	// A sequence keypoint is a table rather than a type of its own -
	// `{time, value, envelope}` for a number and `{time, Color3}` for a colour -
	// because two more userdata types for something written inline once is
	// surface nobody asked for.
	constexpr const char *LUAU_DATATYPES =
		R"LUAU(-- --- the datatype vocabulary ----------------------------------------------

declare extern type Vector2 with
	X: number
	Y: number
	Magnitude: number
	Unit: Vector2
	function __add(self, other: Vector2): Vector2
	function __sub(self, other: Vector2): Vector2
	function __mul(self, other: Vector2 | number): Vector2
end

declare Vector2: {
	new: (x: number?, y: number?) -> Vector2,
}

declare extern type UDim with
	Scale: number
	Offset: number
end

declare UDim: {
	new: (scale: number?, offset: number?) -> UDim,
}

-- `Width` and `Height` are the same two members as `X` and `Y`, which is
-- Roblox's spelling and the run time's.
declare extern type UDim2 with
	X: UDim
	Y: UDim
	Width: UDim
	Height: UDim
	function __add(self, other: UDim2): UDim2
	function __sub(self, other: UDim2): UDim2
end

declare UDim2: {
	new: (xScale: number?, xOffset: number?, yScale: number?, yOffset: number?) -> UDim2,
	-- Four numbers where two are zero is noise an author stops reading.
	fromScale: (x: number?, y: number?) -> UDim2,
	fromOffset: (x: number?, y: number?) -> UDim2,
}

declare extern type Rect with
	Min: Vector2
	Max: Vector2
	Width: number
	Height: number
end

declare Rect: {
	new: ((min: Vector2, max: Vector2) -> Rect)
		& ((minX: number?, minY: number?, maxX: number?, maxY: number?) -> Rect),
}

declare extern type Region3 with
	CFrame: CFrame
	Size: Vector3
end

declare Region3: {
	new: (min: Vector3, max: Vector3) -> Region3,
}

declare extern type NumberRange with
	Min: number
	Max: number
end

declare NumberRange: {
	-- One argument is the degenerate range, which is Roblox's shape.
	new: (min: number, max: number?) -> NumberRange,
}

-- The stops, added at v0.10 because a sequence became a *property* and a value
-- read back has to come back in a shape its own constructor accepts. Until then
-- `Keypoints` handed out `{time, value, envelope}` tables, and those are still
-- accepted by both constructors - the table form is how a gradient is written
-- inline and is not deprecated.
-- What an attribute may hold, which is `ecs::AttributeTypeAllowed`'s closed set.
--
-- At file scope because a `declare extern type ... with` block takes members and
-- not aliases, and because the TypeScript half needs the same for its own reason.
export type EngineAttribute =
	boolean | number | string | Vector3 | Color3 | CFrame
	| Vector2 | UDim | UDim2 | Rect | NumberRange | NumberSequence | ColorSequence

declare extern type NumberSequenceKeypoint with
	read Time: number
	read Value: number
	read Envelope: number
end

declare NumberSequenceKeypoint: {
	new: (time: number, value: number, envelope: number?) -> NumberSequenceKeypoint,
}

declare extern type ColorSequenceKeypoint with
	read Time: number
	read Value: Color3
end

declare ColorSequenceKeypoint: {
	-- No envelope: Roblox's colour keypoint has none and `core::ColorKeypoint`
	-- has no field for one.
	new: (time: number, value: Color3) -> ColorSequenceKeypoint,
}

declare extern type NumberSequence with
	read Keypoints: { NumberSequenceKeypoint }
	function Evaluate(self, time: number): number
end

declare NumberSequence: {
	new: ((value: number) -> NumberSequence)
		& ((from: number, to: number) -> NumberSequence)
		-- **Two overloads rather than one over a union**, which is a Luau
		-- inference limit rather than a design: `{ A | B }` does not unify
		-- against a table literal whose elements are all `A`, so the union form
		-- rejects the ordinary case to accept the mixed one. Both are listed and
		-- the run time takes either, element by element.
		& ((keypoints: { NumberSequenceKeypoint }) -> NumberSequence)
		& ((keypoints: { { number } }) -> NumberSequence),
}

declare extern type ColorSequence with
	read Keypoints: { ColorSequenceKeypoint }
	function Evaluate(self, time: number): Color3
end

declare ColorSequence: {
	new: ((value: Color3) -> ColorSequence)
		& ((from: Color3, to: Color3) -> ColorSequence)
		& ((keypoints: { ColorSequenceKeypoint }) -> ColorSequence)
		& ((keypoints: { { number | Color3 } }) -> ColorSequence),
}

declare extern type TweenInfo with
	Time: number
	DelayTime: number
	RepeatCount: number
	Reverses: boolean
	EasingStyle: Enum_EasingStyle
	EasingDirection: Enum_EasingDirection
	function Evaluate(self, time: number): number
end

declare TweenInfo: {
	new: (
		time: number?,
		style: Enum_EasingStyle?,
		direction: Enum_EasingDirection?,
		repeatCount: number?,
		reverses: boolean?,
		delayTime: number?
	) -> TweenInfo,
}

-- The direction is normalised on the way in, so the length an author passed is
-- not silently kept: `core::Ray::Direction` must be unit and Roblox's need not
-- be. `Ray.Unit` hands back the same ray.
declare extern type Ray with
	Origin: Vector3
	Direction: Vector3
	Unit: Ray
	function PointAt(self, distance: number): Vector3
end

declare Ray: {
	new: (origin: Vector3, direction: Vector3) -> Ray,
}

-- Indexed rather than streamed underneath: the seed is a salt and the draw
-- number is an index, so a script's sequence is a pure function of its seed and
-- how many values it has taken. Two runs agree and a recording replays.
declare extern type Random with
	function NextNumber(self, min: number?, max: number?): number
	-- Inclusive of both ends, which is Roblox's contract.
	function NextInteger(self, min: number, max: number): number
	function NextUnitVector2(self): Vector2
	function NextUnitVector3(self): Vector3
end

declare Random: {
	new: (seed: number?) -> Random,
}

declare extern type Scope with
	function Add(self, resource: any): Scope
	function AddBulk(self, resources: {any}): Scope
	function AddBulk(self, ...: any): Scope
	function Remove(self, resource: any): boolean
	function Clean(self): boolean
	function Destroy(self): boolean
	function IsAlive(self): boolean
	function Count(self): number
	function SetErrorHandler(self, handler: (message: string) -> ()): Scope
end

declare Scope: {
	new: () -> Scope,
}

-- A plain table rather than a userdata: it is a value over a number and
-- nothing more.
type DateTime = {
	UnixTimestamp: number,
	UnixTimestampMillis: number,
}

declare DateTime: {
	-- **Always raises**, hence `never`, and the refusal is the design: a world's
	-- clock is simulated, and a script branching on wall time produces a run
	-- that does not replay. The two below are what to use instead.
	now: () -> never,
	fromSimulated: () -> DateTime,
	fromUnixTimestamp: (seconds: number) -> DateTime,
}

)LUAU";

	// The `BusStatus` string union, in each language's spelling.
	//
	// **Generated from `script::BusStatusWords` rather than written out, which is
	// what turns it from a comment into a check.** Both unions were hand-kept
	// text with a note asking whoever appended a status to remember, and nothing
	// in the build compared them against `script::DescribeStatus` - the third
	// category `AGENTS.md` rule 6 refuses. A status appended to
	// `world::BusStatus` now changes what this emits, so `just bindings-check`
	// fails by name until the checked-in declarations are regenerated.
	//
	// **Two layouts rather than one, because the two files are read by different
	// tools.** Luau's alias is one line, which is how the rest of that file
	// spells a union; TypeScript's is one member per line, which is what
	// `prettier` and every hand-written `.d.ts` in the ecosystem produce. A
	// shared layout would be wrong in one of the two.
	//
	// @param out  Where the declaration goes.
	// @param luau Luau's spelling when true, TypeScript's when false.
	void WriteBusStatus(std::ostream &out, bool luau) {
		const char *comment = luau ? "--" : "//";

		out << comment << " --- the bus services "
			<< "------------------------------------------------------\n\n";
		out << comment << " Every word `script::DescribeStatus` can hand back, generated from that\n"
			<< comment << " switch. A status a script branches on cannot be missing here, and a\n"
			<< comment << " status appended to `world::BusStatus` fails `just bindings-check` until\n"
			<< comment << " these files are regenerated.\n";

		const std::span<const std::string_view> words = engine::script::BusStatusWords();

		if (luau) {
			out << "export type BusStatus =";
			for (size_t index = 0; index < words.size(); index++) {
				out << (index == 0 ? " " : " | ") << '"' << words[index] << '"';
			}
			out << "\n\n";
			return;
		}

		out << "declare type BusStatus =\n";
		for (size_t index = 0; index < words.size(); index++) {
			out << "\t| \"" << words[index] << (index + 1 == words.size() ? "\";\n" : "\"\n");
		}
		out << "\n";
	}

	// The globals that reach the class tree, so they are emitted after it.
	//
	// **`RunService` is a class and a value of the same name**, which is legal
	// because Luau keeps types and values in separate namespaces and is how
	// Roblox's own declarations spell a service.
	//
	// The bus services carry `(value, status, version)` back, which is the shape
	// `LuauBus.cpp` resumes a suspended thread with. Roblox's `GetAsync` returns
	// the value alone and swallows the rest; the status rides beside it here
	// because a refusal a script cannot see is a refusal it cannot handle.
	constexpr const char *LUAU_SERVICES =
		R"LUAU(declare extern type MessagingService with
	function PublishAsync(self, topic: string, message: any): (any, BusStatus, number)
	function SubscribeAsync(self, topic: string, handler: (message: any) -> ()): ()
end

declare extern type TeleportService with
	-- Sends a player to another world in this universe, with an optional
	-- payload. The player is removed from *this* world by the call: nothing
	-- crosses but a name and the data, and the destination rebuilds them.
	function Teleport(self, placeName: string, player: Instance, data: any?): ()

	-- What arrived with this client's player, or nil. Nil on a server, where
	-- there is no local player to have arrived.
	function GetLocalPlayerTeleportData(self): any

	-- What arrived with this player, or nil for one that walked in the front
	-- door. The server's half of the call above: the authority decides where an
	-- arriving character stands and needs to see what the sender wrote.
	function GetTeleportData(self, player: Instance): any
end

-- One channel's arrivals. `OpenChannel` hands one back per channel, so two
-- subsystems in one world listen to two of these rather than filtering one.
declare extern type CrossWorldSignal with
	function Connect(self, handler: (message: any, fromWorld: string) -> ()): RBXScriptConnection
	function Once(self, handler: (message: any, fromWorld: string) -> ()): RBXScriptConnection
end

-- The addressed route out of a world, which `MessagingService` is not.
--
-- A topic is a fan-out: the publisher does not know or care who is listening,
-- which is right for "the boss died" and wrong for "world B, here is the score
-- you asked me for". This names one channel on one world and delivers there
-- alone, so two subsystems talking between the same pair of worlds do not read
-- each other's traffic.
--
-- There is deliberately no `GetWorlds`. A world's storage cannot see its
-- siblings - the directory belongs to the barrier's router - so answering it
-- would mean the barrier stamping a list into every world every tick, which is
-- machinery nobody has asked for. `SendAsync` answering `NoSuchWorld` is the
-- honest half of the same question.
declare extern type CrossWorldService with
	-- Listens on a named channel and hands back that channel's signal. The
	-- handler is called `(message, fromWorldName)` - the sender's name is second
	-- because answering is the point, and the receiver already named the channel.
	--
	-- Takes effect at the next barrier, so a message sent in the same tick is
	-- refused. Raises when this world is over its bus budget for the tick.
	function OpenChannel(self, channel: string): CrossWorldSignal

	-- Stops delivery on a channel. Handlers already connected are left alone, so
	-- reopening it later finds them still there.
	function CloseChannel(self, channel: string): ()

	-- Sends a value to a named channel on a named world, and suspends until the
	-- barrier answers. `Ok`, or `NoSuchWorld`, `NoSuchChannel`, `WorldNotReady`
	-- or `Overflow` - a channel send carries no value back, so the status is the
	-- answer. Raises when this world is over its bus budget for the tick.
	function SendAsync(self, world: string, channel: string, message: any): (any, BusStatus, number)
end

declare extern type MemoryStoreService with
	function GetAsync(self, key: string): (any, BusStatus, number)
	function SetAsync(self, key: string, value: any): (any, BusStatus, number)
	function UpdateAsync(self, key: string, version: number, value: any): (any, BusStatus, number)
	function RemoveAsync(self, key: string): (any, BusStatus, number)
end

declare extern type DataStoreService with
	function GetAsync(self, key: string): (any, BusStatus, number)
	function SetAsync(self, key: string, value: any): (any, BusStatus, number)
	function RemoveAsync(self, key: string): (any, BusStatus, number)
end

-- What content this world holds, which is the other half of naming an asset.
--
-- A `MeshId` is a name a publisher wrote - an id does not cross - and until
-- this there was no way for a script to ask what those names were. Every list
-- is sorted, so a scene that lays content out arranges itself the same way on
-- every run.
declare extern type ContentService with
	-- Every mesh registered in this world, sorted. Empty on a headless server
	-- and before content has arrived, which are the same honest answer.
	function GetMeshes(self): { string }

	-- Every mesh the *store* published, sorted - the signed manifest, which a
	-- client verifies before it can fetch anything.
	--
	-- **This is what there is to name; `GetMeshes` is what has been named.** The
	-- two were one question until v0.10, because content was fetched by kind and
	-- so everything published arrived whether a scene wanted it or not. Nothing
	-- is fetched by kind now, which means a scene reading only `GetMeshes` sees
	-- what it has already asked for and can never discover anything.
	--
	-- Setting a `MeshId` from this list *is* the ask: the name enters the world
	-- and the next content pump fetches that one asset. Empty on a process with
	-- no content source, which is the honest answer.
	function GetPublishedMeshes(self): { string }

	-- The sheets a mesh's own submeshes name, in submesh order.
	--
	-- **What a model is wearing, which nothing else can answer.** A `MeshPart`
	-- naming no `TextureID` shows whatever each submesh recorded at bake time,
	-- and those names live inside the mesh file - so swapping a character's
	-- outfit meant guessing, with no way to learn the current sheet and no name
	-- to put back.
	--
	-- **Not sorted and not deduplicated**, unlike every other list here: which
	-- run wears which is a fact, and a character with twenty submeshes sharing
	-- four sheets is four names repeated. Empty for a built-in, which names
	-- none, and for a mesh this world has not been told about - the same two
	-- answers `GetTriangleCount` folds into zero, for the same reason.
	--
	-- `TextureID` overrides *every* run at once, so naming one of several is a
	-- character wearing one sheet all over.
	function GetMeshTextures(self, mesh: string): { string }

	-- Every texture registered in this world, sorted.
	function GetTextures(self): { string }

	-- A texture's flipbook grid and authored rate, or nil when it is a still
	-- image or this world has not been told about it.
	function GetFlipbook(self, texture: string): { Side: number, Frames: number, FrameRate: number }?

	-- The same number `MeshPart.TrianglesCount` gives, asked about a mesh
	-- rather than a part - so a layout can be sized before anything is built.
	function GetTriangleCount(self, mesh: string): number
end

-- Runs a rectangular batch of values compatible with `math.noise` without
-- holding the VM thread. The result is flat and row-major.
declare extern type ComputeService with
	function NoiseGridAsync(
		self,
		width: number,
		depth: number,
		originX: number,
		originZ: number,
		step: number,
		context: "Serial" | "Threaded" | "Processed"?,
		originY: number?
	): { number }
end

-- What carries a tag, which is the half `Instance:AddTag` cannot answer.
--
-- The same three methods the instance has, plus the one nothing else can do:
-- finding an instance you are not already holding. A scene that wanted every
-- door used to keep its own list beside the tags, and that list goes stale the
-- first time one is destroyed.
--
-- **`Tags` is a `BasePart` component**, so `AddTag` on a `Folder` answers
-- `false` rather than raising - the same answer `Instance:AddTag` gives.
--
-- No `GetInstanceAddedSignal`: nothing records that a tag changed, so a signal
-- here would never fire, and one that never fires reads as a broken engine.
-- `CollectionService.cpp` says what closing that would take.
declare extern type CollectionService with
	-- The same call `Instance:AddTag` makes, so which side a script spells it
	-- from cannot change what happens. `false` when the instance carries no
	-- tags and when this world's thirty-two tags are all spoken for.
	function AddTag(self, instance: Instance, tag: string): boolean

	-- The tag's *name* stays registered in this world afterwards, so
	-- `GetAllTags` still lists one nothing carries.
	function RemoveTag(self, instance: Instance, tag: string): boolean
	function HasTag(self, instance: Instance, tag: string): boolean

	-- Every instance in this world carrying the tag, and empty for a tag
	-- nothing has ever added - a script polling before another has run is the
	-- ordinary case, not a mistake.
	--
	-- **Sorted by the order the world was built in**, so a scene laying its
	-- tagged instances out arranges itself the same way on every run.
	function GetTagged(self, tag: string): { Instance }

	-- What one instance carries, sorted.
	function GetTags(self, instance: Instance): { string }

	-- Every tag this world has registered, sorted, carried or not.
	function GetAllTags(self): { string }
end

-- JSON, a GUID and a URL escape - and **no `RequestAsync`, `GetAsync` or
-- `PostAsync`**, which are absent on purpose rather than missing.
--
-- Arbitrary outbound HTTP from a game script is a security decision nobody has
-- taken. This engine's one route to the network is a signed manifest a client
-- verifies against a publisher key, which is a different thing and not a
-- smaller one. Declaring the three here would promise a surface that does not
-- exist; `script/src/HttpService.cpp` says what taking the decision would need.
--
-- The four that are here observe nothing, which is what lets them exist in a VM
-- whose runs have to replay. `GenerateGUID` is *deterministic* for that reason -
-- read its note before using one as a name anything outside this world has to
-- agree is unique.
declare extern type HttpService with
	-- **The same rules `MessagingService` encodes by**, because it is the same
	-- table walk: an array is a table keyed exactly 1 to `#t` and writes as a
	-- JSON array, anything else is an object, a mixed table's numeric keys
	-- become their decimal text, and an empty table is `{}` rather than `[]`.
	--
	-- Keys are written in sorted order, so one table is one document on every
	-- run. Raises for a cycle, for nesting past sixteen deep, for a NaN or an
	-- infinity, and for anything with no JSON form - an `Instance`, a function,
	-- a `Vector3`, a `Color3` or a `CFrame`.
	function JSONEncode(self, value: any): string
)LUAU"
		// Split for the reason the first of these gives: MSVC's 16380-byte cap.
		R"LUAU(
	-- Arrays come back **one-based**, which is what `#` and `ipairs` mean.
	-- `null` becomes `nil`. Raises on malformed text, on text after the value,
	-- and on a number a double cannot hold.
	function JSONDecode(self, text: string): any

	-- `{XXXXXXXX-XXXX-4XXX-YXXX-XXXXXXXXXXXX}`, upper case, braces by default.
	--
	-- **Deterministic**, and that is a decision rather than an oversight: a
	-- value drawn from a clock or from system entropy makes a run that does not
	-- replay, which is why this VM has no `os` either. Two runs of one world
	-- hand out the same sequence, so this is an identifier for things inside a
	-- world and never a token, a secret, or a name a second machine has to
	-- agree is distinct.
	function GenerateGUID(self, wrapInCurlyBraces: boolean?): string

	-- RFC 3986: the unreserved set through unchanged, every other byte as
	-- `%XX`. A space is `%20` and never `+`.
	function UrlEncode(self, text: string): string
end

-- A master volume and where the ear is - **and eleven Roblox members that are
-- absent on purpose rather than missing.**
--
-- `engine::audio` is L12 `client` and the script layer is L9 `shared`, so
-- nothing in the binding can name a mixer: what a script decides lands on
-- `scene::AudioState` and the client walks it, which is the seam
-- `scene::InputState` established. Everything that would need a *node* the
-- graph does not have is therefore not here - reverb needs a filter node,
-- `DopplerScale` and `DistanceFactor` need a Doppler one, `VolumetricAudio`
-- needs a shape in the emitter, and `RolloffScale` needs a rolloff curve where
-- the engine has two distances. `script/src/SoundService.cpp` lists all eleven
-- with what each would take.
--
-- `PlayLocalSound` is absent for a different reason: `sound.Playing = true`
-- already is it.
declare extern type SoundService with
	-- **Not a Roblox property.** Roblox turns a group of sounds down with a
	-- `SoundGroup`, this engine has no such class - every `Sound` gets one fader
	-- straight into the output - and a whole-world level is the honest one-line
	-- version of what a group is for. Linear, 1 being every sound as authored,
	-- and above 1 is legal for the reason `Sound.Volume` is.
	Volume: number

	-- The mode, then whatever it points at. `Camera` points at nothing and hands
	-- back nil, which is also what `SetListener` takes for it.
	function GetListener(self): (Enum_ListenerType, Instance?)

	-- **`Enum.ListenerType` has two members here and four in Roblox.** The two
	-- missing ones place the ear *and turn it*, and the mixer is posted a
	-- position with no facing, so they are absent from the enum rather than
	-- refused by this method - a script naming one fails where it names it.
	function SetListener(self, listenerType: Enum_ListenerType, listener: Instance?): ()
end

-- Interpolating a property from wherever it is to wherever it should end up.
--
-- **A tween is not an instance and is not parented.** `TweenService:Create`
-- hands back the only handle there is, and dropping it does not stop a tween
-- that is playing - the service holds it until it finishes, exactly as Roblox
-- does.
--
-- **Only a value with a midpoint may be tweened**: the numbers, `Vector2`,
-- `Vector3`, `Color3`, `CFrame`, `UDim`, `UDim2` and `Rect`. A `Bool`, a string
-- or an enum is refused *by name* when the tween is created, rather than
-- running for its whole duration and moving nothing.
declare extern type Tween with
	-- Starts it, or resumes a paused one. A tween that has already finished
	-- starts again from wherever its target is now.
	--
	-- False when the target instance has been destroyed, which is the one
	-- ordinary way this fails.
	function Play(self): boolean

	-- Holds it where it is. False when it was not playing.
	function Pause(self): boolean

	-- Stops it and rewinds. **The properties are left where they are** - a
	-- cancel is a stop and not an undo - and `Completed` does not fire.
	function Cancel(self): boolean

	Completed: TweenCompletedSignal
end

declare extern type TweenService with
	-- The easing curve on its own, with no target and nothing to build.
	--
	-- Alpha is clamped: past the end a tween is finished, and an elastic curve
	-- extrapolated past one grows without bound.
	function GetValue(self, alpha: number, easingStyle: Enum_EasingStyle, easingDirection: Enum_EasingDirection): number

	-- A tween over one instance, from where it is now to the values named.
	--
	-- The start is captured when the tween is **played**, not here, so one built
	-- ahead of time interpolates from wherever the instance has got to.
	--
	-- Raises for a property this instance does not have, for one a script may
	-- not assign, and for one whose type has no midpoint - each named.
	function Create(self, instance: Instance, info: TweenInfo, goals: { [string]: any }): Tween
end

-- Destroying something later, without a script left waiting to do it.
--
-- **The lifetime is seconds in and ticks underneath**, exactly as `task.wait`
-- is: a deadline measured against a wall clock arrives after a different amount
-- of simulation on a busy machine, and a recording would stop replaying. Half a
-- second is thirty ticks at sixty hertz on every machine that runs it.
--
-- **A thousand items at once, and the thousand-and-first destroys the oldest
-- early.** A cleanup call that refused would leave a script's rubbish in the
-- world forever, which is the worse of the two ways to be wrong. There is no
-- `MaxItems`: a cap a script can raise is not a cap.
declare extern type Debris with
	-- Ten seconds by default, which is Roblox's. A lifetime of zero destroys on
	-- the next beat rather than inside this call.
	function AddItem(self, instance: Instance, lifetime: number?): ()
end

declare extern type RunService with
	Heartbeat: HeartbeatSignal
	function IsServer(self): boolean
	function IsClient(self): boolean
	function IsStudio(self): boolean
	-- Not Roblox's, and the more precise question: whether this world's rows
	-- belong to somebody else. A single-player process is a server whose
	-- client-side world is still a replica.
	function IsReplica(self): boolean
end

-- --- the globals -----------------------------------------------------------

declare MessagingService: MessagingService
declare TeleportService: TeleportService
declare CrossWorldService: CrossWorldService
declare MemoryStoreService: MemoryStoreService
declare DataStoreService: DataStoreService
declare RunService: RunService
declare ComputeService: ComputeService
declare ContentService: ContentService
declare CollectionService: CollectionService
declare HttpService: HttpService
declare SoundService: SoundService
declare TweenService: TweenService
declare Debris: Debris

-- The world this script runs on. `game` is the universe above it.
declare workspace: Workspace

-- The storage underneath the class tree.
--
-- **`workspace` is this world's scene and `World` is this world's storage**, and
-- the two are the same world seen from either end: `Instance.new("Part")` is an
-- entity in the archetype the `Part` class names, and `World:Query` is that
-- archetype asked about directly.
--
-- A component a script declares is a real component - the same dense id, the
-- same column, iterated by a C++ system that never heard of the script. What it
-- is not is *typed*: the field list is a run-time fact, so every value table
-- here is `{ [string]: any }` and a definition file cannot do better. See
-- `script/src/LuauEcs.cpp` for what this refuses and why.
declare extern type EngineWorld with
	-- Declares a component, or agrees with one already declared with exactly
	-- these fields. Returns whether this call created it.
	--
	-- The field types are the engine's own spellings - `number`, `boolean`,
	-- `string`, `float`, `int32`, `Vector3`, `CFrame`, `Color3`, `Instance` and
	-- the rest - or `Enum.<set>` for a value that must be a registered member.
	function DefineComponent(self, component: string, fields: { [string]: string }): boolean

	function HasComponentType(self, component: string): boolean

	-- The field list a component was declared with, in a shape
	-- `DefineComponent` accepts back.
	function GetComponentSchema(self, component: string): { [string]: string }?
	function SetComponentTags(self, component: string, tags: { string }): boolean
	function SetComponentFieldTags(self, component: string, field: string, tags: { string }): boolean
	function ExposeComponentField(self, component: string, field: string, exposed: boolean): boolean
	function GetComponentMetadata(self, component: string): {
		Tags: { string },
		Fields: { [string]: { Type: string, Tags: { string }, Exposed: boolean } },
	}?

	-- An entity carrying nothing: no class, no place in the tree, nothing drawn.
	-- Still an `Instance`, because an instance *is* an entity.
	function CreateEntity(self, name: string?): Instance

	-- Every entity carrying all of the named components, in a deterministic
	-- order. Naming a component nothing declared is an error rather than an
	-- empty result.
	function Query(self, ...: string): { Instance }

	-- Every entity carrying all included components and none of the excluded
	-- components.
	function QueryFiltered(self, include: { string }, exclude: { string }): { Instance }

	function Count(self, ...: string): number
end

declare World: EngineWorld

-- The instance this chunk was loaded from, set on the thread after the sandbox.
declare script: LuaSourceContainer

-- `require(moduleScript)`, and declaring it here is what makes it checkable.
--
-- **Upstream's `require` is resolved by the *frontend* rather than typed by the
-- checker**: seeing a call to the global, it hands the argument to a module
-- resolver expecting a path, and reports `Unknown require: unsupported path` for
-- anything else. Every argument in this engine is an instance, so every module
-- a script loaded was an error - which made `require` unusable under
-- `scriptcheck` even where it worked perfectly at run time.
--
-- Declaring it overrides the global and the resolver leaves it alone. The return
-- is `any` because it genuinely is: a module returns one value of whatever type
-- it likes, and there is no static link from an instance to the file behind it.
-- That is the cost of `require` taking an instance, and it is the same cost
-- Roblox pays.
declare require: (module: Instance) -> any

declare warn: (...any) -> ()
declare wait: (seconds: number?) -> number
declare spawn: (callback: (...any) -> (), ...any) -> thread
declare delay: (seconds: number, callback: (...any) -> ()) -> thread
declare time: () -> number
declare elapsedTime: () -> number
declare tick: () -> number

declare task: {
	wait: (seconds: number?) -> number,
	spawn: (callback: (...any) -> (), ...any) -> thread,
	defer: (callback: (...any) -> (), ...any) -> thread,
	delay: (seconds: number, callback: (...any) -> ()) -> thread,
	cancel: (task: thread) -> (),
}

)LUAU";

	// The Luau declaration file.
	std::string LuauDeclarations() {
		std::ostringstream out;
		out << "--!strict\n";
		out << "-- Generated by mono.tools/bindings. Do not edit by hand.\n";
		out << "--\n";
		out << "-- What a Luau script can name, typed. The class tree and the enums come out\n";
		out << "-- of the class table; the globals come out of what the VM installs. Both\n";
		out << "-- halves are written by one program alongside the TypeScript declarations, so\n";
		out << "-- the two surfaces cannot drift into two APIs by accident - where they do\n";
		out << "-- differ, they differ because the two VMs do, and the difference is written\n";
		out << "-- down rather than smoothed over.\n";
		out << "--\n";
		out << "-- This is a definition file rather than a module: point a language server at\n";
		out << "-- it, do not `require` it. For luau-lsp that is\n";
		out << "-- `luau-lsp.types.definitionFiles`, which `.vscode/settings.json` sets --\n";
		out << "-- and that file is gitignored because editor configuration is personal, so\n";
		out << "-- the path is written here too. `.luaurc` beside it is checked in and is\n";
		out << "-- what makes a new script strict by default.\n\n";

		out << LUAU_PRELUDE;

		// **The enums, declared before the classes that name them.** A property
		// typed `Enum.Material` with no declaration of `Material` would be half
		// a contract, and `--!strict` would reject the file outright.
		//
		// A member is a *class* rather than a string literal, so
		// `part.Material = "Plastic"` is a type error at authoring time even
		// though the binding accepts it at run time. That gap is deliberate: the
		// run-time acceptance exists for scripts being migrated, and the type
		// error exists so new code does not acquire the habit.
		//
		// One class per enum rather than one shared `EnumItem`, which is the
		// brand the TypeScript half gets from `__enum`: two classes deriving
		// from a common base are not assignable to one another, so the
		// wrong-enum mistake the run time refuses does not typecheck either.
		out << "-- --- the enums ------------------------------------------------------------\n\n";
		out << "declare extern type EnumItem with\n";
		out << "\tName: string\n";
		out << "\tEnumType: string\n";
		out << "end\n\n";
		for (const engine::core::Name enumName : SortedEnums()) {
			out << "declare extern type Enum_" << enumName.Text() << " extends EnumItem with\n";
			out << "end\n\n";
		}

		out << "declare Enum: {\n";
		for (const engine::core::Name enumName : SortedEnums()) {
			out << "\t" << enumName.Text() << ": {\n";
			for (const engine::core::Name member : engine::ecs::EnumTable::MembersOf(enumName)) {
				out << "\t\t" << member.Text() << ": Enum_" << enumName.Text() << ",\n";
			}
			out << "\t},\n";
		}
		out << "}\n\n";

		out << LUAU_DATATYPES;

		out << "-- --- the class tree -------------------------------------------------------\n\n";
		for (const ClassId id : AllClasses()) {
			const ClassInfo &info = Classes::Describe(id);
			const std::string name(info.Name.Text());

			// **Which inherited members are the ones about this class**, for the
			// two where that is genuinely hard to guess.
			//
			// **A note and not a redeclaration.** Every instance method lives on
			// one shared table, so they are declared on `Instance` and inherited
			// - see the block above. Spelling `GetPlayers` again here would be a
			// second declaration of one member, and Luau would be right to
			// complain; what an author is missing is not the type, it is knowing
			// that the member exists and which instance to call it on. That is a
			// sentence, so this emits a sentence.
			if (name == "Players") {
				out << "-- The service every game asks who is in it. `LocalPlayer` and the\n"
					   "-- rest are below; `GetPlayers`, `GetPlayerByUserId`,\n"
					   "-- `GetPlayerFromCharacter`, `PlayerAdded` and `PlayerRemoving` are\n"
					   "-- inherited from `Instance` and are about this one - nothing fires a\n"
					   "-- player signal at any other subject.\n";
			}
			if (name == "Player") {
				out << "-- One occupant. `LoadCharacter`, `CharacterAdded` and\n"
					   "-- `CharacterRemoving` are inherited from `Instance` and are about\n"
					   "-- this one: the first destroys the old body and spawns a new one at\n"
					   "-- `SpawnLocation`, and the other two fire when it arrives and goes.\n"
					   "-- `PlayerGui`, `PlayerScripts`, `Backpack` and `StarterGear` are the\n"
					   "-- four containers a join makes, and every one of them is private to\n"
					   "-- this player on the wire.\n";
			}

			out << "declare extern type " << name;
			if (info.Parent.IsValid()) {
				out << " extends " << Classes::Describe(info.Parent).Name.Text();
			}
			out << " with\n";

			// **No hand-written `Name` line, and removing it was a fix rather
			// than a tidy-up.** `Name` was special-cased here because it *was* a
			// special case: until v0.5 it was not a declared property at all, so
			// the generator had to know about it. The moment
			// `Classes::Property<&InstanceName::Value>` landed it started coming
			// out of the loop below as well, and every class in this file
			// carried the field twice.
			for (const PropertyDescriptor &property : OwnProperties(info)) {
				// **A property a script may not touch is not declared to
				// scripts.** `PropertyDescriptor::Scriptable` is enforced in
				// both bindings by answering "no such member"; a declaration
				// here would tell an author about a member that then does not
				// exist, which is worse than not mentioning it - a typecheck
				// that passes and a run that fails is the pairing this file
				// exists to prevent.
				if (!property.Scriptable) {
					continue;
				}

				out << "\t";

				// **`read` is Luau's `readonly`, and without it the two
				// languages disagreed about the same descriptor.** The
				// TypeScript half below has emitted `readonly` since it was
				// written; this half emitted nothing, so a Luau script
				// assigning `MeshPart.TrianglesCount` typechecked clean and was
				// then refused at run time by `Store::SetProperty` - a
				// diagnostic arriving one layer later than the one that exists
				// to prevent it.
				if (!property.Writable) {
					out << "read ";
				}

				out << property.Name.Text() << ": " << LuauType(property) << "\n";
			}

			// **The host members, which project onto no component and never
			// will.** `.Changed` and the methods below it are the binding's
			// rather than the class table's - declaring components for them so
			// this loop could find them is the change `script/AGENTS.md` says to
			// refuse. They are written here, against the one class that has
			// them, so they are declared once and inherited by the rest.
			if (name == "Instance") {
				out << "\tChanged: ChangedSignal\n";
				out << "\tfunction IsA(self, className: string): boolean\n";

				// **The tags, which sit beside `IsA` in `OpenInstances`' method
				// table and were missing here.** v0.9 added them to the run time
				// and not to this generator, so every script calling `AddTag`
				// typechecked as an error against a method the engine answers -
				// `examples/Meshes.luau` among them. A host member is only
				// declared where it is written down, and there is no test that
				// would notice one of these tables growing without the other.
				out << "\tfunction AddTag(self, tag: string): boolean\n";
				out << "\tfunction RemoveTag(self, tag: string): boolean\n";
				out << "\tfunction HasTag(self, tag: string): boolean\n";

				// **`GetTags`, which Roblox puts on `Instance` and this engine
				// only had on `CollectionService`.** The other three had been on
				// both surfaces since v0.9, so the instance side was three
				// quarters of a pair - the shape a script copied from a Roblox
				// place trips over. Sorted, matching the service's.
				out << "\tfunction GetTags(self): { string }\n";

				out << "\tfunction Destroy(self): ()\n";
				out << "\tfunction Clone(self): Instance\n";
				out << "\tfunction GetChildren(self): { Instance }\n";
				out << "\tfunction GetDescendants(self): { Instance }\n";

				// **The lookups, and six of the seven had never been declared at
				// all.** Every one of them has been bound in both languages
				// since the neutral layer took the instance table, and an author
				// writing `model:FindFirstChildOfClass("Humanoid")` got a
				// typecheck error against a method the engine answers - the exact
				// failure the tag calls had before v0.9 and the reason this block
				// is written where the run-time table is.
				//
				// **`FindFirstChild`'s second argument was the seventh**, and
				// leaving it out was worse than an absent member: the call
				// typechecked, ran, and silently answered the non-recursive
				// question.
				out << "\tfunction FindFirstChild(self, name: string, recursive: boolean?): Instance?\n";

				// **`WaitForChild`'s timeout is not optional, and that is the one
				// place this surface is deliberately stricter than Roblox's.**
				// Roblox's `WaitForChild(name)` waits for ever; this engine refuses
				// that form at run time because a script that never finishes its
				// tick is work crossing a tick boundary, and `ScriptMethods.cpp`
				// carries the whole argument. Declaring the argument `number?` would
				// typecheck the exact call the run time then raises on - which is
				// the failure the lookups above were fixed for, the other way round.
				out << "\tfunction WaitForChild(self, name: string, timeout: number): Instance?\n";
				out << "\tfunction FindFirstChildOfClass(self, className: string): Instance?\n";
				out << "\tfunction FindFirstChildWhichIsA(self, className: string, recursive: boolean?): "
					   "Instance?\n";
				out << "\tfunction FindFirstAncestor(self, name: string): Instance?\n";
				out << "\tfunction FindFirstAncestorOfClass(self, className: string): Instance?\n";
				out << "\tfunction FindFirstAncestorWhichIsA(self, className: string): Instance?\n";
				out << "\tfunction GetFullName(self): string\n";

				out << "\tfunction IsDescendantOf(self, ancestor: Instance): boolean\n";
				out << "\tfunction IsAncestorOf(self, descendant: Instance): boolean\n";
				out << "\tfunction ClearAllChildren(self): ()\n";
				out << "\tfunction Emit(self, count: number): ()\n";

				// **The pivot pair, declared on `Instance` rather than on
				// `PVInstance`.** Roblox puts them on the latter and the binding
				// puts every method here for one reason: the method table is one
				// table, shared by every instance userdata, so a declaration on a
				// subclass would type-check something the run time does not
				// enforce. `scene::PivotOf` answers the identity for anything
				// with no placement, which is what makes that honest rather than
				// merely convenient.
				out << "\tfunction GetPivot(self): CFrame\n";
				out << "\tfunction PivotTo(self, target: CFrame): ()\n";

				// **The batch pair, declared here for the same reason.**
				// Roblox puts `BulkMoveTo` on `WorldRoot`; the method table is
				// one table and the subject carries nothing - every part is
				// named in the argument - so declaring it narrower would type
				// against a rule the run time does not have. `BulkPivotTo` has
				// no Roblox counterpart at all and is the engine's own, because
				// its single-instance pair is `CFrame =` *and* `PivotTo`.
				//
				// **`{ Instance }` and not `{ Part }`**, which is what the run
				// time takes: a list is whatever a script put in it and
				// anything with no placement is skipped rather than refused.
				// Luau's arrays are invariant, so a caller holding a `{ Part }`
				// annotates it `{ Instance }` - the demos in `examples/` do.
				out << "\tfunction BulkMoveTo(self, parts: { Instance }, placements: { CFrame }): ()\n";
				out << "\tfunction BulkPivotTo(self, parts: { Instance }, targets: { CFrame }): ()\n";
				out << "\tfunction GetPropertyChangedSignal(self, property: string): "
					   "PropertyChangedSignal\n";

				// **Attributes, and the value type is a union rather than
				// `any`.** An attribute holds one of a closed set -
				// `ecs::AttributeTypeAllowed` refuses everything else - so a
				// union is what the run time actually accepts, and `any` would
				// typecheck `part:SetAttribute("k", part)` which the binding
				// refuses at run time.
				//
				// `nil` is in the union on the way *in* because that is how an
				// attribute is removed, and on the way *out* because that is what
				// an unset one reads as.
				//
				// **The alias is at file scope, not here.** A `declare extern
				// type ... with` block takes members and not type aliases -
				// putting one inside is a syntax error four lines later that
				// reads as an unclosed function, which is exactly how it was
				// found. `EngineAttribute` is declared beside the datatypes.
				// **Ownership, declared on `Instance` like everything above it,
				// and the argument is an `Instance` rather than a `Player`.**
				// There is no `Player` type in this file - a class is a run-time
				// registration and the declared vocabulary stops at `Instance` -
				// so pinning the argument tighter than the file can spell would
				// mean inventing a type the run time does not check. What it is
				// checked against is `scene::PlayerClass`, at the call.
				//
				// Optional on the way in because that is how a body is given back
				// to the server, and optional on the way out because that is what
				// a server-owned body reads as.
				// **The `Players` pair, declared on `Instance` like every signal
				// above it.** Nothing fires one at a subject that is not the
				// service, so a connection on anything else is inert by
				// construction - the same answer a class gate would give, at
				// none of the cost. `GetPlayers` is the same shape: the
				// `Player` children of the receiver, which is the answer on
				// `Players` and an empty table anywhere else.
				out << "\tChildAdded: InstanceSignal\n";
				out << "\tChildRemoved: InstanceSignal\n";
				out << "\tDescendantAdded: InstanceSignal\n";
				out << "\tDescendantRemoving: InstanceSignal\n";
				out << "\tAncestryChanged: AncestrySignal\n";
				out << "\tPlayerAdded: InstanceSignal\n";
				out << "\tPlayerRemoving: InstanceSignal\n";

				// **A player's own pair, on `Instance` like every signal above
				// it and for the same reason.** Nothing fires one at a subject
				// that is not a `Player`, so a connection anywhere else is inert
				// by construction - which is the answer a class gate would give
				// at none of the cost.
				out << "\tCharacterAdded: InstanceSignal\n";
				out << "\tCharacterRemoving: InstanceSignal\n";
				out << "\tfunction GetPlayers(self): { Instance }\n";
				out << "\tfunction GetPlayerByUserId(self, userId: number): Instance?\n";

				// **The two the neutral method layer added first**, and they are
				// here for the same reason every method above is: the method
				// table is one table shared by every instance userdata, so a
				// declaration on `Players` alone would type-check something the
				// run time does not enforce. Which instance each is *for* is the
				// note above the generated `Players` and `Player` blocks.
				// **`Equals`, which Roblox does not have and this engine needs.**
				// JavaScript has no operator overloading and every instance
				// handle is a fresh object, so `===` on two handles to one part
				// is always false - see `ScriptMethods.cpp`. Declared for Luau
				// too, where it is a longer spelling of `==`, because a method
				// that exists in one language is the drift the neutral layer
				// ended.
				out << "\tfunction Equals(self, other: Instance): boolean\n";
				out << "\tfunction GetPlayerFromCharacter(self, character: Instance): Instance?\n";
				out << "\tfunction LoadCharacter(self): Instance?\n";

				out << "\tfunction KeepWorldAwake(self, reason: string): ()\n";
				out << "\tfunction LetWorldSleep(self): ()\n";
				out << "\tfunction IsKeepingWorldAwake(self): boolean\n";
				out << "\tfunction SetNetworkOwner(self, player: Instance?): ()\n";
				out << "\tfunction GetNetworkOwner(self): Instance?\n";
				out << "\tfunction GetLinearVelocity(self): Vector3\n";
				out << "\tfunction GetAngularVelocity(self): Vector3\n";
				out << "\tfunction SetLinearVelocity(self, velocity: Vector3): ()\n";
				out << "\tfunction SetAngularVelocity(self, velocity: Vector3): ()\n";
				out << "\tfunction ApplyImpulse(self, impulse: Vector3): ()\n";
				out << "\tfunction Break(self): number\n";

				// **The one door onto `LocalTransparency`, for the same reason
				// `SetAttribute` is a method rather than a property write.**
				// `part.LocalTransparency = x` would go through the ordinary
				// property door, which a replica refuses regardless of which
				// property - see `scene::SetLocalTransparency`. Declared on
				// `Instance` for the reason every method above it is: the
				// method table is one table, and a part that is not a
				// `BasePart` gets the runtime's refusal rather than a
				// type-checked one.
				out << "\tfunction SetLocalTransparency(self, value: number): ()\n";

				// **The `EditableMesh` core, declared on `Instance` for the
				// reason every method above it is.**
				//
				// **`AddVertex` returns a `number` and `AddTriangle` returns a
				// `number?`, and the difference is the point.**
				// `AddTriangle`'s own header in `ScriptMethods.cpp` draws the
				// line: a bad vertex id is the ordinary shape of building a
				// mesh from computed indices, so asking "did that work" is
				// control flow rather than a bug to stop a script over, and it
				// answers `nil`. `AddVertex` has no such case - the only thing
				// it can fail on is being called on an instance that is not an
				// `EditableMesh`, which raises, and `ScriptCall::Raise` is
				// `[[noreturn]]`.
				//
				// Typing it optional anyway used to make every caller narrow a
				// `nil` that cannot arrive: `mesh:SetVertexNormal(id, ...)`
				// right after `local id = mesh:AddVertex(...)` was a type error
				// in `examples/Terrain.luau` for exactly that reason. A binding
				// that describes a failure the engine does not have costs every
				// script that reads it.
				out << "\tfunction AddVertex(self, position: Vector3, normal: Vector3?, uv: Vector2?): "
					   "number\n";
				out << "\tfunction AddTriangle(self, a: number, b: number, c: number): number?\n";
				out << "\tfunction RemoveTriangle(self, triangle: number): boolean\n";
				out << "\tfunction SetVertexPosition(self, vertex: number, position: Vector3): boolean\n";
				out << "\tfunction SetVertexNormal(self, vertex: number, normal: Vector3): boolean\n";
				out << "\tfunction SetVertexUV(self, vertex: number, uv: Vector2): boolean\n";
				out << "\tfunction SetVertexColor(self, vertex: number, colour: Color3, alpha: number?): "
					   "boolean\n";
				out << "\tfunction SetGeometry(self, vertices: { { Position: Vector3, Normal: Vector3?, "
					   "UV: Vector2?, Color: Color3?, Alpha: number? } }, indices: { number }): boolean\n";
				out << "\tfunction Clear(self): boolean\n";

				// AnimationBuffer stores canonical AAN1 bytes. BakeAnimation accepts
				// fixed little-endian keyframe records described by KeyframeBytes.
				out << "\t-- Each keyframe is little-endian: joint u16 at 0, reserved u16 at 2, "
					   "time f32 at 4, position xyz at 8, and quaternion xyzw at 20.\n";
				out << "\tfunction BakeAnimation(self, duration: number, keyframes: buffer): boolean\n";
				out << "\tfunction SetAnimationData(self, data: buffer): boolean\n";
				out << "\tfunction GetAnimationData(self): buffer\n";
				out << "\tfunction ClearAnimationData(self): boolean\n";

				// The `EditableImage` core, declared the same way.
				out << "\tfunction Resize(self, width: number, height: number): boolean\n";
				out << "\tfunction DrawRectangle(self, position: Vector2, size: Vector2, colour: Color3, "
					   "transparency: number?): boolean\n";
				out << "\tfunction DrawLine(self, from: Vector2, to: Vector2, colour: Color3, transparency: "
					   "number?): boolean\n";
				out << "\tfunction DrawCircle(self, centre: Vector2, radius: number, colour: Color3, "
					   "transparency: number?): boolean\n";

				out << "\tfunction GetAttribute(self, name: string): EngineAttribute?\n";
				out << "\tfunction SetAttribute(self, name: string, value: EngineAttribute?): ()\n";
				out << "\tfunction GetAttributes(self): { [string]: EngineAttribute }\n";
				out << "\tfunction GetAttributeChangedSignal(self, name: string): "
					   "PropertyChangedSignal\n";

				// **The storage underneath, declared on `Instance` for the
				// reason every method above it is: the method table is one
				// table.** An instance is an entity, so the thing a component
				// attaches to is already in hand - see `script/src/LuauEcs.cpp`.
				//
				// The value tables are `{ [string]: any }` and that is honest
				// rather than lazy: a component's fields are declared at run
				// time, so there is no static type for one. What a definition
				// file *can* pin is that the keys are field names and the call
				// takes a table, which is what stops `SetComponent("Health", 5)`.
				out << "\tfunction SetComponent(self, component: string, values: { [string]: any }): ()\n";
				out << "\tfunction GetComponent(self, component: string): { [string]: any }?\n";
				out << "\tfunction HasComponent(self, component: string): boolean\n";
				out << "\tfunction RemoveComponent(self, component: string): ()\n";
				out << "\tfunction GetComponents(self): { string }\n";

				// **The 2D tree's input, declared on `Instance` rather than on
				// `GuiObject`.** That is what the run time does - `InstanceIndex`
				// answers these for any instance, because gating them by class
				// would put a class test on a lookup that runs for every field
				// access on every instance, to produce a connection that never
				// fires instead of an error. `gui::Router` only ever names
				// elements it found in a compiled draw list, so one on a `Part`
				// is inert by construction.
				//
				// Declaring them here keeps the two in step: a type that
				// offered them only on `GuiObject` would refuse code the engine
				// accepts, which is worse than the reverse.
				out << "\tActivated: GuiSignal\n";

				// **Roblox's second name for the one event, and it is one
				// `SignalKind` under both.** A `GuiButton` carries
				// `MouseButton1Click` there and most scripts connect to that
				// rather than to `Activated`; this engine's router produces
				// exactly one primary button, so the two questions have one
				// answer. `LuauInstances.cpp` says the same from the run time's side.
				//
				// **`InputChanged` is deliberately not here.** Roblox's fires for
				// pointer motion and for the wheel over an element, `gui::Router`
				// produces motion only, and `MouseMoved` already carries it with
				// the position an argument-less signal could not.
				out << "\tMouseButton1Click: GuiSignal\n";

				out << "\tInputBegan: GuiSignal\n";
				out << "\tInputEnded: GuiSignal\n";
				out << "\tMouseEnter: PointerSignal\n";
				out << "\tMouseLeave: PointerSignal\n";
				out << "\tMouseMoved: PointerSignal\n";

				// **A `UIDragDetector`'s three, on `Instance` for the reason the
				// six above are.** Only a detector is ever the subject of one -
				// `gui::Router` names the modifier it found - so a connection
				// anywhere else is inert by construction.
				out << "\tDragStart: DragSignal\n";
				out << "\tDragContinue: DragSignal\n";
				out << "\tDragEnd: DragSignal\n";

				// **A `TextBox`'s pair, on `Instance` for the reason the six
				// above it are.** Only a box can take the keyboard -
				// `gui::Focus` refuses anything with no `Entry` - so a
				// connection made anywhere else is inert by construction, and
				// declaring them on `TextBox` alone would refuse code the engine
				// accepts.
				out << "\tFocused: FocusSignal\n";
				out << "\tFocusLost: FocusLostSignal\n";

				// **The interface surface, declared on `Instance` for the reason
				// every method above it is.** Which instance each is *for* is
				// Roblox's answer and not the run time's: `GetGuiObjectsAtPosition`
				// is a `PlayerGui`'s and answers an empty list anywhere else,
				// because a container with no laid-out elements under it has
				// nothing at any position.
				out << "\tfunction GetGuiObjectsAtPosition(self, x: number, y: number): { Instance }\n";

				// **The goal is a `UDim2` or a `Vector3` and the union is the
				// honest type**, which is the one place these three depart from
				// Roblox's declaration. The run time reads the argument as the
				// *target's own* `Position` or `Size` - see
				// `ScriptCall::ReadProperty` - so on a `GuiObject` it is a
				// `UDim2` and on a `BasePart` it is a `Vector3`, and both calls
				// work. Narrowing to Roblox's `UDim2` would refuse a call the
				// engine serves, which every comment above says is the worse
				// direction to be wrong in.
				//
				// **Easing *direction* before *style***, which is Roblox's order
				// and the opposite of `TweenInfo.new`'s. Correcting it here would
				// silently swap two arguments of the same kind.
				const char *const tweenTail =
					"easingDirection: Enum_EasingDirection?, easingStyle: Enum_EasingStyle?, "
					"time: number?, override: boolean?, callback: (() -> ())?): boolean\n";

				out << "\tfunction TweenPosition(self, endPosition: UDim2 | Vector3, " << tweenTail;
				out << "\tfunction TweenSize(self, endSize: UDim2 | Vector3, " << tweenTail;
				out << "\tfunction TweenSizeAndPosition(self, endSize: UDim2 | Vector3, "
					   "endPosition: UDim2 | Vector3, "
					<< tweenTail;
			}

			// The member only the Workspace answers, for the reason
			// `LuauInstances.cpp` keeps it in a table of its own: a `Raycast` on a
			// `Folder` would be an answer that means nothing.
			//
			// A raycast result is a plain table at run time - read once and
			// discarded - so it is written inline rather than given a name.
			//
			// **`CurrentCamera` used to be written here too, and removing it was
			// a fix rather than a tidy-up** - the same correction the `Name` line
			// on `Instance` went through at v0.5, and it failed the same way. It
			// was hand-written because it *was* a special case: `PushCurrentCamera`
			// served it from the `ActiveCamera` resource and no property projected
			// it. v0.10 declared one, so the loop below started emitting it as
			// well - and an extern type with the member twice is not a warning,
			// it is an assertion failure inside Luau's constraint generator, which
			// `just typecheck` reports as `SIGILL` on every script in the tree.
			//
			// What is lost is the type: a declared `Reference` property is
			// `Instance` and this said `Camera?`, so a script assigning a `Part`
			// to `workspace.CurrentCamera` now typechecks and is refused at run
			// time instead. That is the same trade every other reference property
			// makes - `Parent: Instance` - and narrowing it needs
			// `PropertyDescriptor` to carry which class a reference points at,
			// which is a change to `ecs` rather than to this generator.
			if (name == "Workspace") {
				// **Emitted here rather than in the hand-written block above
				// because they hang off a *class*.** `RaycastParams` is a global
				// and lives in the literal; these are methods on `Workspace`,
				// and the loop that writes a class's members is this one.
				//
				// The volume queries answer with an array of instances and no
				// hit information, which is what `physics::OverlapBox` and the
				// two casts return: there is no contact point for a volume test,
				// so a result carrying `Position` would carry an invented one.
				out << "\tfunction OverlapBox(self, centre: Vector3, size: Vector3, "
					   "params: RaycastParams?): { Instance }\n";
				out << "\tfunction OverlapSphere(self, centre: Vector3, radius: number, "
					   "params: RaycastParams?): { Instance }\n";
				out << "\tfunction BlockCast(self, from: CFrame, size: Vector3, motion: Vector3, "
					   "params: RaycastParams?): { Instance }\n";
				out << "\tfunction SphereCast(self, from: Vector3, radius: number, motion: Vector3, "
					   "params: RaycastParams?): { Instance }\n";

				// The portal cast has the same result shape as `Raycast`, and
				// the two are written out separately because Luau has no way to
				// name an anonymous table type once.
				out << "\tfunction RaycastThroughPortals(self, origin: Vector3, direction: Vector3, "
					   "params: RaycastParams?): {\n";
				out << "\t\tInstance: Instance,\n";
				out << "\t\tPosition: Vector3,\n";
				out << "\t\tNormal: Vector3,\n";
				out << "\t\tDistance: number,\n";
				out << "\t\tMaterial: string,\n";
				out << "\t}?\n";

				out << "\tfunction Raycast(self, origin: Vector3, direction: Vector3, "
					   "params: RaycastParams?): {\n";
				out << "\t\tInstance: Instance,\n";
				out << "\t\tPosition: Vector3,\n";
				out << "\t\tNormal: Vector3,\n";
				out << "\t\tDistance: number,\n";

				// **A string, where this said `Enum_Material` until v0.10.** The
				// enum is gone and the field is `Surface::Material` now - the row
				// a contact reads friction out of - which is a plain name.
				// `script/LuauQuery.cpp` carries why a hit result reports that
				// one rather than resolving the part's `Material` instance.
				out << "\t\tMaterial: string,\n";
				out << "\t}?\n";
			}

			out << "end\n\n";
		}

		WriteBusStatus(out, true);
		out << LUAU_SERVICES;

		// **A table of names to types, read with `index`, rather than one
		// overload per class.** Both of these started as an intersection of
		// function types - which is how a reader would first write "one
		// signature per class name" - and the type checker refused the file
		// outright with *"Code is too complex to typecheck"* at nine of them.
		// An intersection is solved by trying every branch; a lookup is one
		// step, and it stays one step when the tree doubles.
		//
		// `keyof` narrows the argument to a name that exists, so a typo is
		// still an error at the call rather than an `Instance` handed back.
		out << "-- --- what a name resolves to --------------------------------------------\n\n";

		out << "type Services = {\n";

		// Everything in the tree, from the class table - see `ServiceClasses`
		// for why this is derived rather than listed.
		for (const ClassId id : ServiceClasses()) {
			const std::string_view name = Classes::Describe(id).Name.Text();
			out << "\t" << name << ": " << name << ",\n";
		}

		// The bus services, which are globals rather than instances and so are
		// not in the tree for the walk above to find.
		out << "\tRunService: RunService,\n";
		out << "\tMessagingService: MessagingService,\n";
		out << "\tTeleportService: TeleportService,\n";
		out << "\tContentService: ContentService,\n";
		out << "\tComputeService: ComputeService,\n";
		out << "\tCollectionService: CollectionService,\n";
		out << "\tHttpService: HttpService,\n";
		out << "\tCrossWorldService: CrossWorldService,\n";
		out << "\tContextActionService: ContextActionServiceType,\n";
		out << "\tSettingsService: SettingsServiceType,\n";

		// **The two that carry a live property, and the TypeScript map has them
		// too since v0.16.** They were this paragraph's exception for as long as
		// `ServiceSurface` could describe a method and not an accessor; the type
		// is still the hand-written one above rather than a class, because both
		// are globals and not instances in the tree.
		//
		// `UserInputService` is also the row that was reachable at run time and
		// absent from this map, which made `game:GetService("UserInputService")`
		// - the form the docs and every test use, and the only form a live
		// property survives, since a bare global read is a `GETIMPORT` - a
		// typecheck error.
		out << "\tSoundService: SoundService,\n";
		out << "\tUserInputService: UserInputServiceType,\n";
		out << "\tMemoryStoreService: MemoryStoreService,\n";
		out << "\tDataStoreService: DataStoreService,\n";

		// The two that step on the tick. Both languages bind them, so they are
		// here *and* in the TypeScript map - which is the ordinary case and the
		// one the four above are the exception to.
		out << "\tTweenService: TweenService,\n";
		out << "\tDebris: Debris,\n";
		out << "}\n\n";

		out << "type CreatableInstances = {\n";
		for (const ClassId id : ConstructibleClasses()) {
			const std::string_view name = Classes::Describe(id).Name.Text();
			out << "\t" << name << ": " << name << ",\n";
		}
		out << "}\n\n";

		// **`game` is the universe, and `GetService` is a global lookup at run
		// time** - so `Services` above is exactly the globals that exist, plus
		// `Workspace`, which `RunService.cpp` special-cases because its global
		// is spelled lower case.
		//
		// **Named `DataModel`, and `self` is that name rather than `any`.** The
		// name is the run time's - `OpenGame` sets `__metatable` to the same
		// string - and it has to be written down because a generic method whose
		// `self` is `any` does not infer: `game:GetService("RunService")`
		// resolved `T` to `any` and handed back a service with no members. A
		// concrete `self` is what makes the singleton argument reach `T`.
		out << "type DataModel = {\n";
		out << "\tWorkspace: Workspace,\n";

		// **Which world this script is standing on**, by name. Roblox's
		// `JobId` identifies the server instance, and a world here *is* that
		// instance - see `RunService.cpp`. `Mirrors-4-worlds` is the caller:
		// `--worlds N` runs one file in every world, so without this every view
		// would be identical and a compositor that placed them in the wrong
		// order would look correct.
		out << "\tJobId: string,\n";
		out << "\tGetService: <T>(self: DataModel, service: keyof<Services> & T) "
			   "-> index<Services, T>,\n";
		out << "}\n\n";
		out << "declare game: DataModel\n\n";

		// So that `Instance.new("Part")` is a `Part` rather than an `Instance`
		// an author has to cast.
		out << "declare Instance: {\n";
		out << "\tnew: <T>(className: keyof<CreatableInstances> & T, parent: Instance?) "
			   "-> index<CreatableInstances, T>,\n";
		out << "}\n";
		return out.str();
	}

	// The hand-written half of the TypeScript file.
	//
	// **`Vector3.new(...)`, not `new Vector3(...)`, and the difference was a
	// bug.** This declared `{ new(x?: number, ...): Vector3 }` - which inside an
	// object type is a *construct signature*, so the declaration said
	// `new Vector3(1, 2, 3)` while `JsBindings.cpp` provides a `new` **method**
	// and `Mirrors-1-world.ts` calls it. The file disagreed with its own
	// example. A property typed as a function is the unambiguous spelling.
	//
	// **Methods rather than operators**, because JavaScript has no operator
	// overloading: `a.add(b)` and `cf.mul(other)` are the same run-time
	// arithmetic the Luau half spells `+` and `*`.
	constexpr const char *TYPESCRIPT_PRELUDE =
		R"TS(// --- the value types -------------------------------------------------------

declare interface Vector3 {
	readonly X: number;
	readonly Y: number;
	readonly Z: number;
	readonly Magnitude: number;
	readonly Unit: Vector3;
	add(other: Vector3): Vector3;
	sub(other: Vector3): Vector3;
	mul(other: Vector3 | number): Vector3;
	div(other: Vector3 | number): Vector3;
	// `//` in Luau: each quotient floored.
	idiv(other: Vector3 | number): Vector3;
	// Unary minus, which is an operator in Luau and has no method form there.
	neg(): Vector3;
	Equals(other: Vector3): boolean;
	Abs(): Vector3;
	Ceil(): Vector3;
	Floor(): Vector3;
	Sign(): Vector3;
	Cross(other: Vector3): Vector3;
	Dot(other: Vector3): number;
	// Unsigned without an axis, signed by which side of `axis` the turn goes with one.
	Angle(other: Vector3, axis?: Vector3): number;
	FuzzyEq(other: Vector3, epsilon?: number): boolean;
	Lerp(goal: Vector3, alpha: number): Vector3;
	Max(vector: Vector3): Vector3;
	Min(vector: Vector3): Vector3;
}

declare interface Color3 {
	readonly R: number;
	readonly G: number;
	readonly B: number;
	Equals(other: Color3): boolean;
}

declare interface CFrame {
	readonly Position: Vector3;
	readonly X: number;
	readonly Y: number;
	readonly Z: number;

	// This frame's rotation at the world origin.
	readonly Rotation: CFrame;

	// The three basis columns. `XVector`/`YVector` are the same directions as
	// `RightVector`/`UpVector`; `ZVector` is the negation of `LookVector`.
	readonly RightVector: Vector3;
	readonly UpVector: Vector3;
	readonly LookVector: Vector3;
	readonly XVector: Vector3;
	readonly YVector: Vector3;
	readonly ZVector: Vector3;

	// **Methods, where Luau writes operators.** JavaScript has no operator
	// overloading, so `a.mul(b)` is the honest spelling rather than pretending
	// the language has something it does not.
	mul(other: CFrame): CFrame;
	mul(point: Vector3): Vector3;
	add(offset: Vector3): CFrame;
	sub(offset: Vector3): CFrame;

	Inverse(): CFrame;
	Orthonormalize(): CFrame;
	Lerp(goal: CFrame, alpha: number): CFrame;
	ToWorldSpace(other: CFrame): CFrame;
	ToObjectSpace(other: CFrame): CFrame;
	PointToWorldSpace(point: Vector3): Vector3;
	PointToObjectSpace(point: Vector3): Vector3;
	VectorToWorldSpace(direction: Vector3): Vector3;
	VectorToObjectSpace(direction: Vector3): Vector3;
	AngleBetween(other: CFrame): number;
	FuzzyEq(other: CFrame, epsilon?: number): boolean;

	// **Arrays, where Luau returns several values.** Same reason as the methods
	// above: JavaScript has no multiple return.
	GetComponents(): number[];
	components(): number[];
	ToEulerAnglesXYZ(): [number, number, number];
	ToEulerAnglesYXZ(): [number, number, number];
	ToOrientation(): [number, number, number];
	ToAxisAngle(): [Vector3, number];
}

declare const Vector3: {
	new: (x?: number, y?: number, z?: number) => Vector3;

	// Capitalised, because Roblox capitalises these two. The Luau half carries
	// the argument.
	FromNormalId: (normal: Enum.NormalId) => Vector3;
	FromAxis: (axis: Enum.Axis) => Vector3;

	// Lowercase, because Roblox's are. The Luau half carries the argument.
	readonly zero: Vector3;
	readonly one: Vector3;
	readonly xAxis: Vector3;
	readonly yAxis: Vector3;
	readonly zAxis: Vector3;
};

declare const Color3: {
	new: (r?: number, g?: number, b?: number) => Color3;
	// 0-255, the way an author reads a colour off a palette.
	fromRGB: (r?: number, g?: number, b?: number) => Color3;
};

declare const CFrame: {
	new: {
		(x?: number, y?: number, z?: number): CFrame;
		(position: Vector3): CFrame;
		(x: number, y: number, z: number, qX: number, qY: number, qZ: number, qW: number): CFrame;
		(
			x: number, y: number, z: number,
			r00: number, r01: number, r02: number,
			r10: number, r11: number, r12: number,
			r20: number, r21: number, r22: number
		): CFrame;
	};

	// The identity: no turn, at the origin.
	identity: CFrame;

	// Radians, because Roblox's is radians - while `Orientation` is degrees.
	//
	// X, then Y, then Z, which is what Roblox means by `Angles` and by
	// `fromEulerAnglesXYZ`. Until v0.18 this name was bound to the Y-X-Z
	// composition, so a pasted script turned the wrong way when it named two
	// axes at once.
	Angles: (rx: number, ry: number, rz: number) => CFrame;
	fromEulerAnglesXYZ: (rx: number, ry: number, rz: number) => CFrame;

	// Y, then X, then Z. The order `BasePart.Orientation` round-trips through.
	fromEulerAnglesYXZ: (rx: number, ry: number, rz: number) => CFrame;
	fromOrientation: (rx: number, ry: number, rz: number) => CFrame;

	lookAt: (from: Vector3, to: Vector3, up?: Vector3) => CFrame;
	lookAlong: (at: Vector3, direction: Vector3, up?: Vector3) => CFrame;
	fromAxisAngle: (axis: Vector3, angle: number) => CFrame;
	fromMatrix: (position: Vector3, vX: Vector3, vY: Vector3, vZ?: Vector3) => CFrame;
	fromRotationBetweenVectors: (from: Vector3, to: Vector3) => CFrame;
};

// --- signals ---------------------------------------------------------------
//
// `Heartbeat.Connect(fn)` rather than `Heartbeat:Connect(fn)` -- the colon is
// Lua's and a JavaScript author writes the dot. Same signal, same list.

declare interface RBXScriptConnection {
	readonly Connected: boolean;
	Disconnect(): void;
}

declare interface HeartbeatSignal {
	Connect(handler: (deltaTime: number) => void): RBXScriptConnection;
}

declare interface ChangedSignal {
	Connect(handler: (property: string) => void): RBXScriptConnection;
	Once(handler: (property: string) => void): RBXScriptConnection;
	Equals(other: ChangedSignal): boolean;
}

// The instance tree's signals, matching the Luau half - undeclared until v0.13
// for the reason given there.
declare interface InstanceSignal {
	Connect(handler: (instance: Instance) => void): RBXScriptConnection;
	Once(handler: (instance: Instance) => void): RBXScriptConnection;
}

declare interface AncestrySignal {
	Connect(handler: (instance: Instance, parent: Instance) => void): RBXScriptConnection;
	Once(handler: (instance: Instance, parent: Instance) => void): RBXScriptConnection;
}

// What an attribute may hold, which is `ecs::AttributeTypeAllowed`'s closed set.
//
// **Named at file scope rather than nested**, because TypeScript has no
// interface-scoped type alias the way the Luau half does - so the name is
// prefixed to say where it belongs rather than risking a collision with a game's
// own `Attribute`.
// --- input ------------------------------------------------------------------
//
// **`UserInputService` is declared here at last, and the history is the reason
// to say so.** `engine.d.ts` once declared it and `ContextActionService` because
// its prelude was written by mirroring Luau's, and neither global existed in
// that VM - a TypeScript file naming one typechecked and then failed. That is
// the failure `ServiceCatalogue.hpp` opens by naming, and the rule that came out
// of it is that a declaration here is a claim the catalogue has to back.
//
// It does now. `ServiceProperty` gave a live property a neutral shape, so both
// VMs install `UserInputService` and `SoundService` from one description -
// `MouseBehavior` and `Volume` are accessors here where they are a userdata's
// `__index` there - and `engine.script.servicecatalogue` asks a running VM
// whether every row it claims is reachable.
//
// **The two that were absent here are declared now**, which is what
// `ScriptCall::ReturnBoundAction` bought: `GetBoundActionInfo` and
// `GetAllBoundActionInfo` answer a record holding `Enum.KeyCode` members, and a
// record with an `EnumItem` in it still has no `ScriptValue` form - so the *fact*
// became a `BoundActionReport` and each adapter builds its own record, exactly as
// each builds its own `InputObject`.

// What a bound action's handler is handed. Roblox's `InputObject`, and the same
// five fields the Luau half offers over the same report.
declare interface InputObject {
	readonly KeyCode: Enum.KeyCode;
	readonly UserInputType: Enum.UserInputType;
	readonly UserInputState: Enum.UserInputState;

	// Pixels from the top-left of the window, with the wheel's notches in Z.
	readonly Position: Vector3;
	readonly Delta: Vector3;
}
)TS"
		// Split for the reason the first of these gives: MSVC's 16380-byte cap.
		R"TS(
// What `InputBegan`, `InputEnded` and `InputChanged` call a handler with.
//
// Its own type rather than `PropertyChangedSignal`, which passes nothing.
//
// `gameProcessedEvent` is true when the 2D interface took the pointer or keyboard
// this beat. Controller reports are not claimed by the interface and answer false.
declare interface InputSignal {
	Connect(handler: (input: InputObject, gameProcessedEvent: boolean) => void): RBXScriptConnection;
	Once(handler: (input: InputObject, gameProcessedEvent: boolean) => void): RBXScriptConnection;
}

// The window's focus edges, which Roblox calls with nothing.
declare interface WindowFocusSignal {
	Connect(handler: () => void): RBXScriptConnection;
	Once(handler: () => void): RBXScriptConnection;
}

// What `LastInputTypeChanged` calls a handler with.
declare interface LastInputTypeSignal {
	Connect(handler: (lastInputType: Enum.UserInputType) => void): RBXScriptConnection;
	Once(handler: (lastInputType: Enum.UserInputType) => void): RBXScriptConnection;
}

// What the gamepad connection signals call a handler with.
declare interface GamepadConnectionSignal {
	Connect(handler: (gamepad: Enum.UserInputType) => void): RBXScriptConnection;
	Once(handler: (gamepad: Enum.UserInputType) => void): RBXScriptConnection;
}

declare interface UserInputService {
	MouseBehavior: Enum.MouseBehavior;

	// Whether the pointer is drawn. Separate from `MouseBehavior` because
	// Roblox's two are; the two locking modes hide it whatever this says.
	MouseIconEnabled: boolean;

	MouseDeltaSensitivity: number;
	readonly KeyboardEnabled: boolean;
	readonly MouseEnabled: boolean;

	// Live when any mapped gamepad or raw joystick is connected.
	readonly GamepadEnabled: boolean;

	// These devices are not implemented, so their capability flags stay false.
	readonly TouchEnabled: boolean;
	readonly VREnabled: boolean;
	readonly AccelerometerEnabled: boolean;
	readonly GyroscopeEnabled: boolean;

	readonly InputBegan: InputSignal;
	readonly InputEnded: InputSignal;

	// Fires for pointer motion, wheel motion, controller sticks and triggers.
	readonly InputChanged: InputSignal;
	readonly GamepadConnected: GamepadConnectionSignal;
	readonly GamepadDisconnected: GamepadConnectionSignal;

	// Everything reads as released on the frame focus is lost, and
	// `WindowFocusReleased` is delivered before those releases so a listener can
	// tell which explains which.
	readonly WindowFocused: WindowFocusSignal;
	readonly WindowFocusReleased: WindowFocusSignal;

	// Which device the player switched to, beside `GetLastInputType`'s poll.
	readonly LastInputTypeChanged: LastInputTypeSignal;

	IsKeyDown(key: Enum.KeyCode): boolean;

	// False for every `Enum.UserInputType` member that is not a mouse button.
	IsMouseButtonPressed(button: Enum.UserInputType): boolean;

	GetMouseLocation(): Vector2;
	GetMouseDelta(): Vector2;
	GetKeysPressed(): Enum.KeyCode[];

	// `InputObject`s and not `EnumItem`s, which is Roblox's shape: the object
	// carries where the pointer was as well as which button it is.
	GetMouseButtonsPressed(): InputObject[];

	// `Enum.UserInputType.Keyboard` on a world nobody has touched, where Roblox
	// answers `None` - there is no such member here.
	GetLastInputType(): Enum.UserInputType;

	// The `TextBox` a person is typing into, or null. A lookup rather than a
	// tally of the focus signals: `gui::Focus` is the one door a focus change
	// goes through and `GuiServiceState::FocusedTextBox` is where it rests.
	GetFocusedTextBox(): Instance | null;

	GetConnectedGamepads(): Enum.UserInputType[];
	GetGamepadState(gamepad: Enum.UserInputType): InputObject[];
	GetSupportedGamepadKeyCodes(gamepad: Enum.UserInputType): Enum.KeyCode[];
	IsGamepadButtonDown(gamepad: Enum.UserInputType, button: Enum.KeyCode): boolean;
}

// What `UserInputService` deliberately does not have, and it is the half a
// migrating author needs most: no touch surface, no cursor image in `render`,
// and no keyboard-layout query below L12. The six touch signals, `MouseIcon`
// and `GetStringForKeyCode` are absent rather than present and useless.
// `TextBoxFocused` and its twin are absent for a different reason: the fact
// exists and reaches a script as `textBox.Focused`, and a world-subject row
// firing about an instance would put the two input pumps in each other's work.
// `script/src/UserInputService.cpp` names each one and what closing it would
// take.

// What a world decides about how it is heard. See the Luau half for the eleven
// Roblox members that are absent and what each would need first.
declare interface SoundService {
	// **Not a Roblox property**, which has a `SoundGroup` instead and this
	// engine has no such class. Linear, 1 being every sound as authored, and
	// above 1 is legal for the reason `Sound.Volume` is.
	Volume: number;

	// **A pair and not two values, which is the one place this surface reads
	// differently from Luau's.** A method with more than one answer is a Luau
	// shape; JavaScript has no multiple return, so `ScriptCall` packs the two
	// into an array and an author destructures it:
	//
	//     const [mode, ear] = SoundService.GetListener();
	GetListener(): [Enum.ListenerType, Instance | null];

	// **`Enum.ListenerType` has two members here and four in Roblox.** The two
	// missing ones place the ear *and turn it*, and the mixer is posted a
	// position with no facing, so they are absent from the enum rather than
	// refused by this method.
	SetListener(listenerType: Enum.ListenerType, listener?: Instance | null): void;
}

declare type EngineAttribute =
	| boolean
	| number
	| string
	| Vector3
	| Color3
	| CFrame
	| Vector2
	| UDim
	| UDim2
	| Rect
	| NumberRange
	| NumberSequence
	| ColorSequence;

declare interface PropertyChangedSignal {
	Connect(handler: () => void): RBXScriptConnection;
	Once(handler: () => void): RBXScriptConnection;
	Equals(other: PropertyChangedSignal): boolean;
}

// The 2D tree's input, in two shapes because the arguments differ. See the Luau
// half for why the three input signals take none: Roblox hands them an
// `InputObject`, and `gui::GuiEvent` carries nothing one could be built from
// without inventing the field a handler reads it for.
declare interface GuiSignal {
	Connect(handler: () => void): RBXScriptConnection;
	Once(handler: () => void): RBXScriptConnection;
	Equals(other: GuiSignal): boolean;
}

// `(x, y)` in canvas pixels, which is Roblox's signature exactly.
declare interface PointerSignal {
	Connect(handler: (x: number, y: number) => void): RBXScriptConnection;
	Once(handler: (x: number, y: number) => void): RBXScriptConnection;
	Equals(other: PointerSignal): boolean;
}

// `uiDragDetector.DragStart`, `.DragContinue` and `.DragEnd`. Four numbers:
// where the pointer is, then how far the gesture has come. The Luau half says
// why there is no input object.
declare interface DragSignal {
	Connect(handler: (x: number, y: number, dx: number, dy: number) => void): RBXScriptConnection;
	Once(handler: (x: number, y: number, dx: number, dy: number) => void): RBXScriptConnection;
	Equals(other: DragSignal): boolean;
}

// `textBox.Focused`. Its own name rather than `GuiSignal` for the reason the
// Luau half gives: the two are structurally identical and this one is about the
// keyboard.
declare interface FocusSignal {
	Connect(handler: () => void): RBXScriptConnection;
	Once(handler: () => void): RBXScriptConnection;
	Equals(other: FocusSignal): boolean;
}

// `textBox.FocusLost`, with one of Roblox's two arguments. See the Luau half for
// why the `InputObject` is not the second, and for what `enterPressed`
// distinguishes.
declare interface FocusLostSignal {
	Connect(handler: (enterPressed: boolean) => void): RBXScriptConnection;
	Once(handler: (enterPressed: boolean) => void): RBXScriptConnection;
	Equals(other: FocusLostSignal): boolean;
}

// --- queries ---------------------------------------------------------------

declare interface RaycastParams {
	CollisionGroup: string;

	// The most entities an overlap or a cast will report. Ignored by `Raycast`,
	// which reports one hit or none.
	//
	// A result exactly this long may have been truncated, which is the author's
	// own instruction rather than a silent loss. Clamped to 65536.
	MaxParts: number;
}

declare const RaycastParams: {
	new: () => RaycastParams;
};

declare interface RaycastResult {
	readonly Instance: Instance;
	readonly Position: Vector3;
	readonly Normal: Vector3;
	readonly Distance: number;

	// The `Surface` name - what the part is like to touch, not what it looks
	// like. See the Luau declaration above for why it is a string.
	readonly Material: string;
}

)TS";

	// The rest of the datatype vocabulary.
	//
	// **Four of these carry less than the Luau half, and none of it is an
	// omission here.** `JsDatatypes.cpp` installs what is listed below and
	// nothing more:
	//
	//   - `UDim2` has no `Width`/`Height` aliases and no arithmetic;
	//   - `TweenInfo` has no `EasingStyle`/`EasingDirection` getters, though its
	//     constructor still takes both;
	//   - `Ray` has no `Unit`;
	//   - `ColorSequence.new` takes no keypoint array, only one colour or two.
	//
	// Each of those is a gap in the JavaScript binding rather than in this file,
	// and writing them down is what makes the gap visible instead of a run-time
	// `undefined`.
	constexpr const char *TYPESCRIPT_DATATYPES =
		R"TS(// --- the datatype vocabulary ----------------------------------------------

declare interface Vector2 {
	readonly X: number;
	readonly Y: number;
	readonly Magnitude: number;
	readonly Unit: Vector2;
	add(other: Vector2): Vector2;
	sub(other: Vector2): Vector2;
	mul(other: Vector2 | number): Vector2;
	Equals(other: Vector2): boolean;
}

declare const Vector2: {
	new: (x?: number, y?: number) => Vector2;
};

declare interface UDim {
	readonly Scale: number;
	readonly Offset: number;
}

declare const UDim: {
	new: (scale?: number, offset?: number) => UDim;
};

declare interface UDim2 {
	readonly X: UDim;
	readonly Y: UDim;
}

declare const UDim2: {
	new: (xScale?: number, xOffset?: number, yScale?: number, yOffset?: number) => UDim2;
	// Four numbers where two are zero is noise an author stops reading.
	fromScale: (x?: number, y?: number) => UDim2;
	fromOffset: (x?: number, y?: number) => UDim2;
};

declare interface Rect {
	readonly Min: Vector2;
	readonly Max: Vector2;
	readonly Width: number;
	readonly Height: number;
}

declare const Rect: {
	new: {
		(min: Vector2, max: Vector2): Rect;
		(minX?: number, minY?: number, maxX?: number, maxY?: number): Rect;
	};
};

declare interface Region3 {
	readonly CFrame: CFrame;
	readonly Size: Vector3;
}

declare const Region3: {
	new: (min: Vector3, max: Vector3) => Region3;
};

declare interface NumberRange {
	readonly Min: number;
	readonly Max: number;
}

declare const NumberRange: {
	// One argument is the degenerate range, which is Roblox's shape.
	new: (min: number, max?: number) => NumberRange;
};

// The stops. See the Luau half for why these arrived at v0.10.
declare interface NumberSequenceKeypoint {
	readonly Time: number;
	readonly Value: number;
	readonly Envelope: number;
}

declare const NumberSequenceKeypoint: {
	new: (time: number, value: number, envelope?: number) => NumberSequenceKeypoint;
};

declare interface ColorSequenceKeypoint {
	readonly Time: number;
	readonly Value: Color3;
}

declare const ColorSequenceKeypoint: {
	new: (time: number, value: Color3) => ColorSequenceKeypoint;
};

declare interface NumberSequence {
	readonly Keypoints: readonly NumberSequenceKeypoint[];
	Evaluate(time: number): number;
}

declare const NumberSequence: {
	new: {
		(value: number): NumberSequence;
		(from: number, to: number): NumberSequence;
		// Either form. `[time, value, envelope]` is the table one; the Luau half
		// says the same thing in its own syntax.
		(keypoints: (NumberSequenceKeypoint | [number, number] | [number, number, number])[]):
			NumberSequence;
	};
};

declare interface ColorSequence {
	readonly Keypoints: readonly ColorSequenceKeypoint[];
	Evaluate(time: number): Color3;
}

declare const ColorSequence: {
	new: {
		(value: Color3): ColorSequence;
		(from: Color3, to: Color3): ColorSequence;
		(keypoints: (ColorSequenceKeypoint | [number, Color3])[]): ColorSequence;
	};
};

declare interface TweenInfo {
	readonly Time: number;
	readonly DelayTime: number;
	readonly RepeatCount: number;
	readonly Reverses: boolean;
	Evaluate(time: number): number;
}

declare const TweenInfo: {
	new: (
		time?: number,
		style?: Enum.EasingStyle,
		direction?: Enum.EasingDirection,
		repeatCount?: number,
		reverses?: boolean,
		delayTime?: number
	) => TweenInfo;
};

// The direction is normalised on the way in, so the length an author passed is
// not silently kept.
declare interface Ray {
	readonly Origin: Vector3;
	readonly Direction: Vector3;
	PointAt(distance: number): Vector3;
}

declare const Ray: {
	new: (origin: Vector3, direction: Vector3) => Ray;
};

// Indexed rather than streamed underneath: the seed is a salt and the draw
// number is an index, so a script's sequence is a pure function of its seed and
// how many values it has taken. Two runs agree and a recording replays.
declare interface Random {
	NextNumber(min?: number, max?: number): number;
	// Inclusive of both ends, which is Roblox's contract.
	NextInteger(min: number, max: number): number;
	NextUnitVector2(): Vector2;
	NextUnitVector3(): Vector3;
}

declare const Random: {
	new: (seed?: number) => Random;
};

declare interface Scope {
	Add(resource: unknown): this;
	AddBulk(resources: unknown[]): this;
	AddBulk(...resources: unknown[]): this;
	Remove(resource: unknown): boolean;
	Clean(): boolean;
	Destroy(): boolean;
	IsAlive(): boolean;
	Count(): number;
	SetErrorHandler(handler: (message: string) => void): this;
}

declare const Scope: {
	new: () => Scope;
};

declare interface DateTime {
	readonly UnixTimestamp: number;
	readonly UnixTimestampMillis: number;
}

declare const DateTime: {
	// **Always throws**, hence `never`, and the refusal is the design: a world's
	// clock is simulated, and a script branching on wall time produces a run
	// that does not replay. The two below are what to use instead.
	now: () => never;
	fromSimulated: () => DateTime;
	fromUnixTimestamp: (seconds: number) => DateTime;
};

)TS";

	// The TypeScript globals, emitted after the class tree they reach.
	//
	// **`RunService` carries `Heartbeat` and nothing else here, and that is not
	// an omission in this file.** `JsBindings.cpp` builds the service out of one
	// property; `IsServer`, `IsClient`, `IsStudio` and `IsReplica` exist on the
	// Luau side only. Declaring them anyway would turn a missing binding into a
	// run-time `undefined is not a function` instead of a red squiggle.
	//
	// A store call suspends on a promise rather than on a coroutine, and the
	// reply arrives as one object because a promise resolves with a single
	// value. That is the same `(value, status, version)` the Luau side returns.
	constexpr const char *TYPESCRIPT_SERVICES =
		R"TS(declare interface StoreReply {
	readonly Value: unknown;
	readonly Status: BusStatus;
	readonly Version: number;
}

declare interface MessagingService {
	PublishAsync(topic: string, message: unknown): Promise<StoreReply>;
	SubscribeAsync(topic: string, handler: (message: unknown) => void): void;
}

declare interface TeleportService {
	Teleport(placeName: string, player: Instance, data?: unknown): void;
	GetLocalPlayerTeleportData(): unknown;

	/** What arrived with this player, or nil for one that walked in the front door. */
	GetTeleportData(player: Instance): unknown;
}

declare interface MemoryStoreService {
	GetAsync(key: string): Promise<StoreReply>;
	SetAsync(key: string, value: unknown): Promise<StoreReply>;
	UpdateAsync(key: string, version: number, value: unknown): Promise<StoreReply>;
	RemoveAsync(key: string): Promise<StoreReply>;
}

declare interface DataStoreService {
	GetAsync(key: string): Promise<StoreReply>;
	SetAsync(key: string, value: unknown): Promise<StoreReply>;
	RemoveAsync(key: string): Promise<StoreReply>;
}

declare interface RunService {
	readonly Heartbeat: HeartbeatSignal;
}

// One channel's arrivals, handed back by `OpenChannel`. `(message,
// fromWorldName)`: the sender's name is second because answering is the point,
// and the receiver already named the channel.
declare interface CrossWorldSignal {
	Connect(handler: (message: unknown, fromWorld: string) => void): RBXScriptConnection;
	Once(handler: (message: unknown, fromWorld: string) => void): RBXScriptConnection;
}

// The addressed route out of a world, where `MessagingService` is the fan-out.
// One channel on one world, so two subsystems talking between the same pair of
// worlds do not read each other's traffic. There is deliberately no
// `GetWorlds`: a world's storage cannot see its siblings, and `SendAsync`
// answering `NoSuchWorld` is the honest half of the same question.
declare interface CrossWorldService {
	// Listens on a named channel and hands back that channel's signal. Takes
	// effect at the next barrier, so a message sent in the same tick is refused.
	// Throws when this world is over its bus budget for the tick.
	OpenChannel(channel: string): CrossWorldSignal;

	// Stops delivery. Handlers already connected are left alone, so reopening
	// the channel later finds them still there.
	CloseChannel(channel: string): void;

	// `Ok`, or `NoSuchWorld`, `NoSuchChannel`, `WorldNotReady` or `Overflow` - a
	// channel send carries no value back, so the status is the answer.
	SendAsync(world: string, channel: string, message: unknown): Promise<StoreReply>;
}

// What content this world holds, which is the other half of naming an asset.
// Every list is sorted, so a scene that lays content out arranges itself the
// same way on every run. See the Luau half for what each answer means.
declare interface ContentService {
	GetMeshes(): string[];

	// What there is to name, where `GetMeshes` is what has been named. Setting a
	// `MeshId` from this list is what fetches that one asset.
	GetPublishedMeshes(): string[];

	// The sheets a mesh's own submeshes name, in submesh order - not sorted and
	// not deduplicated, unlike every other list here. An untextured run keeps
	// its slot as an empty string.
	GetMeshTextures(mesh: string): string[];

	GetTextures(): string[];

	// Null for a still image and for a texture this world has not been told
	// about, which are the same answer.
	GetFlipbook(texture: string): { Side: number; Frames: number; FrameRate: number } | null;

	GetTriangleCount(mesh: string): number;
}

// Runs a rectangular batch of values compatible with `math.noise` without
// holding the VM thread. The result is flat and row-major.
declare interface ComputeService {
	NoiseGridAsync(
		width: number,
		depth: number,
		originX: number,
		originZ: number,
		step: number,
		context?: "Serial" | "Threaded" | "Processed",
		originY?: number
	): Promise<number[]>;
}

// What carries a tag, which is the half `Instance.AddTag` cannot answer.
//
// No `GetInstanceAddedSignal`: nothing records that a tag changed, so a signal
// here would never fire, and one that never fires reads as a broken engine.
declare interface CollectionService {
	// False when the instance carries no tags and when this world's thirty-two
	// tags are all spoken for.
	AddTag(instance: Instance, tag: string): boolean;

	// The tag's name stays registered afterwards, so `GetAllTags` still lists
	// one nothing carries.
	RemoveTag(instance: Instance, tag: string): boolean;
	HasTag(instance: Instance, tag: string): boolean;

	// Sorted by the order the world was built in, and empty for a tag nothing
	// has ever added.
	GetTagged(tag: string): Instance[];

	GetTags(instance: Instance): string[];
	GetAllTags(): string[];
}

// JSON, a GUID and a URL escape - and **no `RequestAsync`, `GetAsync` or
// `PostAsync`**, which are absent on purpose rather than missing. See the Luau
// half, which carries the whole argument.
declare interface HttpService {
	// The same rules `MessagingService` encodes by, because it is the same table
	// walk. Keys are written in sorted order. Throws for a cycle, for nesting
	// past sixteen deep, for a NaN or an infinity, and for anything with no JSON
	// form.
	JSONEncode(value: unknown): string;

	// `null` becomes `null`. Throws on malformed text, on text after the value,
	// and on a number a double cannot hold.
	JSONDecode(text: string): unknown;

	// **Deterministic**, and that is a decision rather than an oversight - two
	// runs of one world hand out the same sequence, so this is an identifier for
	// things inside a world and never a token or a secret.
	//
	// Braces by default. **`GenerateGUID(0)` differs from Luau's**, because the
	// argument is read with this language's own truthiness and zero is falsy
	// here and truthy there.
	GenerateGUID(wrapInCurlyBraces?: boolean): string;

	// RFC 3986: the unreserved set through unchanged, every other byte as `%XX`.
	// A space is `%20` and never `+`.
	UrlEncode(text: string): string;
}

// What `GetBoundActionInfo` reports about one bound action.
//
// **Four of Roblox's six fields.** `title` and `description` come from
// `SetTitle` and `SetDescription`, which decorate a touch button - there is no
// touch surface, so the pair is not bound and reporting them as empty strings
// would claim they had been set to nothing.
declare interface BoundActionInfo {
	// The keys this action claims. **Keys only**: `BindAction` takes
	// `Enum.KeyCode` here where Roblox also takes `Enum.UserInputType`.
	readonly inputTypes: Enum.KeyCode[];

	readonly priorityLevel: number;

	// **Higher wins, which is Roblox's direction.** The engine's own list is
	// sorted highest-priority-first, so this is that index inverted rather than
	// the same word meaning the opposite thing.
	readonly stackOrder: number;

	// Always false: the argument is accepted at bind time and ignored.
	readonly createTouchButton: boolean;
}

// A priority stack over the keyboard, which is what this adds over polling: the
// highest claim on a key decides, and what its handler returns decides whether
// anything below it hears the press.
declare interface ContextActionService {
	// The touch-button argument is accepted and ignored: there is no touch
	// surface, and refusing it would make a script fail on a line describing
	// something this engine does not have.
	//
	// **What the handler returns decides who else hears the key.**
	// `Enum.ContextActionResult.Pass` hands the press to the next claim down;
	// `Sink`, `undefined`, or a handler that threw, stop it here.
	BindAction(
		name: string,
		handler: (
			name: string,
			state: Enum.UserInputState,
			input: InputObject
		) => Enum.ContextActionResult | void,
		createTouchButton: boolean,
		...keys: Enum.KeyCode[]
	): void;

	BindActionAtPriority(
		name: string,
		handler: (
			name: string,
			state: Enum.UserInputState,
			input: InputObject
		) => Enum.ContextActionResult | void,
		createTouchButton: boolean,
		priority: number,
		...keys: Enum.KeyCode[]
	): void;

	UnbindAction(name: string): void;
	UnbindAllActions(): void;

	// `null` for a name nothing has bound, which is what `if (info)` reads.
	GetBoundActionInfo(name: string): BoundActionInfo | null;

	GetAllBoundActionInfo(): Record<string, BoundActionInfo>;
}

declare interface SettingsMenuActionSignal {
	Connect(handler: (name: string) => void): RBXScriptConnection;
	Once(handler: (name: string) => void): RBXScriptConnection;
}

declare interface SettingsService {
	// Adds or relabels one named row and returns that row's activation signal.
	// Client scripts only. At most twelve actions may exist at once.
	SetMenuAction(name: string, label: string): SettingsMenuActionSignal;
	RemoveMenuAction(name: string): boolean;
	ClearMenuActions(): void;
}

// `tween.Completed`, which takes no arguments. Its own type rather than
// `GuiSignal` for the reason the Luau half gives: they are structurally
// identical and one of the two names would be false.
declare interface TweenCompletedSignal {
	Connect(handler: () => void): RBXScriptConnection;
	Once(handler: () => void): RBXScriptConnection;
	Equals(other: TweenCompletedSignal): boolean;
}

// Interpolating a property from wherever it is to wherever it should end up.
// See the Luau half for what a tween is, what it may drive, and why dropping
// the handle does not stop one that is playing.
declare interface Tween {
	// False when the target instance has been destroyed.
	Play(): boolean;
	Pause(): boolean;

	// A stop and not an undo: the properties stay where they are, and
	// `Completed` does not fire.
	Cancel(): boolean;

	readonly Completed: TweenCompletedSignal;

	// **Two handles to one tween are not `===`**, because `Create` and every
	// return build a fresh object - the same reason `Instance.Equals` exists.
	Equals(other: Tween): boolean;
}

declare interface TweenService {
	GetValue(alpha: number, easingStyle: Enum.EasingStyle, easingDirection: Enum.EasingDirection): number;

	// Raises for a property this instance does not have, for one a script may
	// not assign, and for one whose type has no midpoint - each named.
	Create(instance: Instance, info: TweenInfo, goals: { [property: string]: unknown }): Tween;
}

// Destroying something later, without a script left waiting to do it. The
// lifetime is seconds in and ticks underneath, exactly as `task.wait` is.
declare interface Debris {
	// Ten seconds by default. A lifetime of zero destroys on the next beat
	// rather than inside this call.
	AddItem(instance: Instance, lifetime?: number): void;
}

// --- the globals -----------------------------------------------------------

declare const MessagingService: MessagingService;
declare const TeleportService: TeleportService;
declare const MemoryStoreService: MemoryStoreService;
declare const DataStoreService: DataStoreService;
declare const RunService: RunService;
declare const ComputeService: ComputeService;
declare const TweenService: TweenService;
declare const Debris: Debris;

// The seven that stopped being Luau's at v0.16 - five when `ServiceSurface`
// stopped naming a VM, and the last two when `ServiceProperty` gave a live
// property a neutral shape. **Every surface service this engine has is now in
// both languages**, and `BreakpointService` is not one: it is absent outside a
// studio and refuses a JavaScript chunk outright, which is a debugger feature
// and not a binding.
declare const CrossWorldService: CrossWorldService;
declare const ContentService: ContentService;
declare const CollectionService: CollectionService;
declare const HttpService: HttpService;
declare const ContextActionService: ContextActionService;
declare const SettingsService: SettingsService;
declare const UserInputService: UserInputService;
declare const SoundService: SoundService;

// The world this script runs on. `game` is the universe above it.
declare const workspace: Workspace;

// The storage underneath the class tree. See the Luau half for the whole
// argument; the only difference here is that there is no colon call, so the
// world object is not an argument.
interface EngineWorld {
	// Declares a component, or agrees with one already declared with exactly
	// these fields. Returns whether this call created it.
	DefineComponent(component: string, fields: { [field: string]: string }): boolean;

	HasComponentType(component: string): boolean;

	// The field list a component was declared with, in a shape
	// `DefineComponent` accepts back.
	GetComponentSchema(component: string): { [field: string]: string } | null;
	SetComponentTags(component: string, tags: string[]): boolean;
	SetComponentFieldTags(component: string, field: string, tags: string[]): boolean;
	ExposeComponentField(component: string, field: string, exposed: boolean): boolean;
	GetComponentMetadata(component: string): {
		Tags: string[];
		Fields: { [field: string]: { Type: string; Tags: string[]; Exposed: boolean } };
	} | null;

	// An entity carrying nothing: no class, no place in the tree, nothing drawn.
	// Still an `Instance`, because an instance is an entity.
	CreateEntity(name?: string): Instance;

	// Every entity carrying all of the named components, in a deterministic
	// order. Naming a component nothing declared is an error rather than an
	// empty result.
	Query(...components: string[]): Instance[];

	// Every entity carrying all included components and none of the excluded
	// components.
	QueryFiltered(include: string[], exclude: string[]): Instance[];

	Count(...components: string[]): number;
}

declare const World: EngineWorld;

// An alias for `null`, because this is a Roblox-shaped API and a Roblox author
// writes `part.Parent = nil`. A third empty value would be a footgun wearing a
// familiar name, so it is not one.
declare const nil: null;

declare function print(...values: unknown[]): void;
declare function warn(...values: unknown[]): void;
declare function typeOf(value: unknown): string;
declare function time(): number;
declare function tick(): number;

declare const task: {
	wait: (seconds?: number) => number;
	spawn: (callback: (...args: unknown[]) => void, ...args: unknown[]) => void;
	defer: (callback: (...args: unknown[]) => void, ...args: unknown[]) => void;
	delay: (seconds: number, callback: (...args: unknown[]) => void) => void;
	cancel: (handle: unknown) => void;
};

)TS";

	// The TypeScript declaration file.
	std::string TypeScriptDeclarations() {
		std::ostringstream out;
		out << "// Generated by mono.tools/bindings. Do not edit by hand.\n";
		out << "//\n";
		out << "// What a TypeScript author can name, typed. TypeScript is the typed\n";
		out << "// authoring surface over the JavaScript VM -- it erases its types by\n";
		out << "// design, so this describes what the bindings expose and nothing about\n";
		out << "// how a value is represented at run time.\n";
		out << "//\n";
		out << "// This is a type root: a `tsconfig.json` lists it in place of\n";
		out << "// `@rbxts/types`. The one in the repository root already does, and\n";
		out << "// `bunx tsc --noEmit` is what checks a script against it.\n";
		out << "//\n";
		out << "// Written alongside the Luau declarations by one program. Where the two\n";
		out << "// disagree -- `game.GetService(\"Workspace\")`, the `RunService`\n";
		out << "// predicates -- they disagree because the two VMs do, and each file says\n";
		out << "// what its own VM installs rather than what the other one has.\n\n";

		out << TYPESCRIPT_PRELUDE;

		// The enums, for the reason the Luau file declares them: a property
		// typed `Enum.Material` needs `Material` to exist.
		//
		// **A branded interface per enum**, so `Enum.Material.Plastic` and
		// `Enum.EasingStyle.Linear` are not assignable to one another. Without
		// the brand every `EnumItem` would be interchangeable, and the wrong-enum
		// mistake the run time refuses would compile.
		out << "declare interface EnumItem { readonly Name: string; readonly EnumType: string; ";
		out << "Equals(other: EnumItem): boolean; }\n\n";

		// **One namespace holding both declaration spaces**, which is what makes
		// `Enum.Material` a type here where the Luau half could only have the
		// alias. TypeScript keeps types and values in separate namespaces, so an
		// `interface Material` and a `const Material` under `namespace Enum` are
		// one name meaning two things rather than a redeclaration - the type in a
		// type position, the table of members in an expression.
		//
		// **The brand stays.** `__enum` is what stops `Enum.Material.Plastic` and
		// `Enum.EasingStyle.Linear` being assignable to one another; without it
		// every `EnumItem` is interchangeable and the wrong-enum mistake the run
		// time refuses would compile. Moving the interface inside the namespace
		// changed where it is spelled and nothing about what it does.
		out << "declare namespace Enum {\n";
		for (const engine::core::Name enumName : SortedEnums()) {
			out << "\tinterface " << enumName.Text() << " extends EnumItem { ";
			out << "readonly __enum: \"" << enumName.Text() << "\"; }\n";
		}
		out << "\n";
		for (const engine::core::Name enumName : SortedEnums()) {
			// Members are typed by the bare name rather than `Enum.Material`,
			// because inside the namespace the bare name is already this
			// interface. Both spell the same type; the short one is what a reader
			// of a generated file wants to see repeated a few hundred times.
			out << "\tconst " << enumName.Text() << ": {\n";
			for (const engine::core::Name member : engine::ecs::EnumTable::MembersOf(enumName)) {
				out << "\t\treadonly " << member.Text() << ": " << enumName.Text() << ";\n";
			}
			out << "\t};\n";
		}
		out << "}\n\n";

		out << TYPESCRIPT_DATATYPES;

		out << "// --- the class tree -------------------------------------------------------\n\n";
		for (const ClassId id : AllClasses()) {
			const ClassInfo &info = Classes::Describe(id);
			const std::string name(info.Name.Text());

			out << "declare interface " << name;
			if (info.Parent.IsValid()) {
				out << " extends " << Classes::Describe(info.Parent).Name.Text();
			}
			out << " {\n";

			// **The root's `Name` is not written by hand either, and this one was
			// actively wrong rather than merely redundant.** It emitted
			// `readonly Name: string;` on `Instance` - which then declared
			// `Name: string;` again from the property loop, because `Name` has
			// been a real property since v0.5. Two declarations of one member
			// that *disagree about whether it can be assigned*, in the file whose
			// whole job is to tell an author what they may write.
			//
			// A Roblox script sets `.Name`, so the property's own answer -
			// writable - is the right one, and it is the one that survives now
			// that nothing competes with it.
			for (const PropertyDescriptor &property : OwnProperties(info)) {
				// Left out for the reason the Luau half above gives.
				if (!property.Scriptable) {
					continue;
				}

				out << "\t";
				if (!property.Writable) {
					out << "readonly ";
				}
				out << property.Name.Text() << ": " << TypeScriptType(property) << ";\n";
			}

			// The host members, for the reason the Luau half states: they
			// project onto no component, so the loop above cannot find them and
			// declaring components so that it could is the change
			// `script/AGENTS.md` says to refuse.
			if (name == "Instance") {
				out << "\treadonly Changed: ChangedSignal;\n";
				out << "\tIsA(className: string): boolean;\n";

				// The tags, for the reason the Luau half states - including
				// `GetTags`, which was the missing quarter of the set.
				out << "\tAddTag(tag: string): boolean;\n";
				out << "\tRemoveTag(tag: string): boolean;\n";
				out << "\tHasTag(tag: string): boolean;\n";
				out << "\tGetTags(): string[];\n";

				out << "\tDestroy(): void;\n";
				out << "\tClone(): Instance;\n";
				out << "\tGetChildren(): Instance[];\n";
				out << "\tGetDescendants(): Instance[];\n";

				// The lookups, matching the Luau half - six of the seven had
				// never been declared and the seventh was declared without its
				// second argument. `null` rather than `undefined` because that is
				// what a JavaScript lookup answers for nothing found.
				out << "\tFindFirstChild(name: string, recursive?: boolean): Instance | null;\n";

				// **The required timeout of the Luau half, plus the one thing a
				// JavaScript author has to know: this may or may not be a promise.**
				// A child that is already there is answered directly and a wait is a
				// `Promise` the barrier resolves, so `await` is what reads either -
				// and the union is what says so. A bare `Promise<Instance | null>`
				// would have typechecked a `.then` that fails on the direct answer,
				// which is the same class of lie as an optional timeout.
				out << "\tWaitForChild(name: string, timeout: number): Instance | Promise<Instance | null> | "
					   "null;\n";
				out << "\tFindFirstChildOfClass(className: string): Instance | null;\n";
				out << "\tFindFirstChildWhichIsA(className: string, recursive?: boolean): Instance | null;\n";
				out << "\tFindFirstAncestor(name: string): Instance | null;\n";
				out << "\tFindFirstAncestorOfClass(className: string): Instance | null;\n";
				out << "\tFindFirstAncestorWhichIsA(className: string): Instance | null;\n";
				out << "\tGetFullName(): string;\n";

				out << "\tIsDescendantOf(ancestor: Instance): boolean;\n";
				out << "\tIsAncestorOf(descendant: Instance): boolean;\n";
				out << "\tClearAllChildren(): void;\n";
				out << "\tEmit(count: number): void;\n";

				// The pivot pair, matching the Luau half and declared in the
				// same place for the same reason.
				out << "\tGetPivot(): CFrame;\n";
				out << "\tPivotTo(target: CFrame): void;\n";

				// The batch pair, matching the Luau half.
				out << "\tBulkMoveTo(parts: Instance[], placements: CFrame[]): void;\n";
				out << "\tBulkPivotTo(parts: Instance[], targets: CFrame[]): void;\n";
				out << "\tGetPropertyChangedSignal(property: string): PropertyChangedSignal;\n";

				// Ownership, matching the Luau half. `Instance` rather than a
				// `Player` type for the reason given there, and `null` rather
				// than `undefined` because `null` is what `GetNetworkOwner`
				// returns for a server-owned body.
				// The `Players` pair, matching the Luau half.
				out << "\treadonly ChildAdded: InstanceSignal;\n";
				out << "\treadonly ChildRemoved: InstanceSignal;\n";
				out << "\treadonly DescendantAdded: InstanceSignal;\n";
				out << "\treadonly DescendantRemoving: InstanceSignal;\n";
				out << "\treadonly AncestryChanged: AncestrySignal;\n";
				out << "\treadonly PlayerAdded: InstanceSignal;\n";
				out << "\treadonly PlayerRemoving: InstanceSignal;\n";

				// A player's own pair, matching the Luau half and declared on
				// `Instance` for the reason given there.
				out << "\treadonly CharacterAdded: InstanceSignal;\n";
				out << "\treadonly CharacterRemoving: InstanceSignal;\n";
				out << "\tGetPlayers(): Instance[];\n";
				out << "\tGetPlayerByUserId(userId: number): Instance | undefined;\n";

				// The two the neutral method layer added first. Declared here as
				// well as on the Luau side because they are genuinely bound in
				// both languages now - which is what that layer is for, and is
				// the opposite of the nine it started by closing, where this file
				// declared methods JavaScript did not have.
				out << "\tEquals(other: Instance): boolean;\n";
				out << "\tGetPlayerFromCharacter(character: Instance): Instance | undefined;\n";
				out << "\tLoadCharacter(): Instance | undefined;\n";

				out << "\tKeepWorldAwake(reason: string): void;\n";
				out << "\tLetWorldSleep(): void;\n";
				out << "\tIsKeepingWorldAwake(): boolean;\n";
				out << "\tSetNetworkOwner(player?: Instance | null): void;\n";
				out << "\tGetNetworkOwner(): Instance | null;\n";
				out << "\tGetLinearVelocity(): Vector3;\n";
				out << "\tGetAngularVelocity(): Vector3;\n";
				out << "\tSetLinearVelocity(velocity: Vector3): void;\n";
				out << "\tSetAngularVelocity(velocity: Vector3): void;\n";
				out << "\tApplyImpulse(impulse: Vector3): void;\n";
				out << "\tBreak(): number;\n";

				// The one door onto `LocalTransparency`, matching the Luau half
				// and for the same reason declared there.
				out << "\tSetLocalTransparency(value: number): void;\n";

				// The `EditableMesh` core, matching the Luau half and for
				// the same reason declared there.
				out << "\tAddVertex(position: Vector3, normal?: Vector3, uv?: Vector2): number;\n";
				out << "\tAddTriangle(a: number, b: number, c: number): number | undefined;\n";
				out << "\tRemoveTriangle(triangle: number): boolean;\n";
				out << "\tSetVertexPosition(vertex: number, position: Vector3): boolean;\n";
				out << "\tSetVertexNormal(vertex: number, normal: Vector3): boolean;\n";
				out << "\tSetVertexUV(vertex: number, uv: Vector2): boolean;\n";
				out << "\tSetVertexColor(vertex: number, colour: Color3, alpha?: number): boolean;\n";
				out << "\tSetGeometry(vertices: { Position: Vector3; Normal?: Vector3; UV?: Vector2; "
					   "Color?: Color3; Alpha?: number }[], indices: number[]): Promise<boolean>;\n";
				out << "\tClear(): boolean;\n";

				// AnimationBuffer uses ArrayBuffer here for the same owned byte block
				// Luau exposes as buffer.
				out << "\t/** Each keyframe is little-endian: joint u16 at 0, reserved u16 at 2, "
					   "time f32 at 4, position xyz at 8, and quaternion xyzw at 20. */\n";
				out << "\tBakeAnimation(duration: number, keyframes: ArrayBuffer): boolean;\n";
				out << "\tSetAnimationData(data: ArrayBuffer): boolean;\n";
				out << "\tGetAnimationData(): ArrayBuffer;\n";
				out << "\tClearAnimationData(): boolean;\n";

				// The `EditableImage` core, matching the Luau half.
				out << "\tResize(width: number, height: number): boolean;\n";
				out << "\tDrawRectangle(position: Vector2, size: Vector2, colour: Color3, transparency?: "
					   "number): boolean;\n";
				out << "\tDrawLine(from: Vector2, to: Vector2, colour: Color3, transparency?: number): "
					   "boolean;\n";
				out << "\tDrawCircle(centre: Vector2, radius: number, colour: Color3, transparency?: "
					   "number): boolean;\n";

				// Attributes, matching the Luau half. The union is the same
				// closed set and for the same reason.
				out << "\tGetAttribute(name: string): EngineAttribute | null;\n";
				out << "\tSetAttribute(name: string, value: EngineAttribute | null): void;\n";
				out << "\tGetAttributes(): { [name: string]: EngineAttribute };\n";
				out << "\tGetAttributeChangedSignal(name: string): PropertyChangedSignal;\n";

				// The storage underneath, matching the Luau half and declared in
				// the same place for the same reason: the method table is one
				// table, so a declaration on a subclass would type-check
				// something the run time does not enforce.
				out << "\tSetComponent(component: string, values: { [field: string]: any }): void;\n";
				out << "\tGetComponent(component: string): { [field: string]: any } | null;\n";
				out << "\tHasComponent(component: string): boolean;\n";
				out << "\tRemoveComponent(component: string): void;\n";
				out << "\tGetComponents(): string[];\n";

				// The 2D tree's input, on `Instance` for the reason the Luau
				// half states at length: the run time answers them for any
				// instance, and a type narrower than the run time refuses code
				// the engine accepts.
				out << "\treadonly Activated: GuiSignal;\n";

				// Roblox's second name for the one event, matching the Luau half
				// - and `InputChanged` is absent here for the reason given there.
				out << "\treadonly MouseButton1Click: GuiSignal;\n";

				out << "\treadonly InputBegan: GuiSignal;\n";
				out << "\treadonly InputEnded: GuiSignal;\n";
				out << "\treadonly MouseEnter: PointerSignal;\n";
				out << "\treadonly MouseLeave: PointerSignal;\n";
				out << "\treadonly MouseMoved: PointerSignal;\n";

				// A `UIDragDetector`'s three, on `Instance` and inert anywhere
				// else - the Luau half says why.
				out << "\treadonly DragStart: DragSignal;\n";
				out << "\treadonly DragContinue: DragSignal;\n";
				out << "\treadonly DragEnd: DragSignal;\n";

				// A `TextBox`'s pair, on `Instance` and inert anywhere else -
				// the Luau half says why.
				out << "\treadonly Focused: FocusSignal;\n";
				out << "\treadonly FocusLost: FocusLostSignal;\n";

				// The interface surface, matching the Luau half - including the
				// `UDim2 | Vector3` goal, which is the target's own property type
				// rather than Roblox's narrower one.
				out << "\tGetGuiObjectsAtPosition(x: number, y: number): Instance[];\n";

				// `Enum.EasingStyle` rather than `Enum_EasingStyle`, which is the
				// one spelling difference between the two declaration files: the
				// TypeScript half puts the enums in a namespace where the Luau
				// half prefixes them.
				const char *const tweenTail =
					"easingDirection?: Enum.EasingDirection, easingStyle?: Enum.EasingStyle, "
					"time?: number, override?: boolean, callback?: () => void): boolean;\n";

				out << "\tTweenPosition(endPosition: UDim2 | Vector3, " << tweenTail;
				out << "\tTweenSize(endSize: UDim2 | Vector3, " << tweenTail;
				out << "\tTweenSizeAndPosition(endSize: UDim2 | Vector3, endPosition: UDim2 | Vector3, "
					<< tweenTail;
			}

			// The member only the Workspace answers, matching the Luau half.
			//
			// `CurrentCamera` is gone from here for the reason the Luau half
			// gives: it is a declared property now and the loop above emits it.
			// TypeScript would have merged the duplicate rather than crashing,
			// which is worse - the hand-written `Camera | null` and the generated
			// `Instance` would have intersected to something no assignment
			// satisfies.
			if (name == "Workspace") {
				out << "\tRaycast(origin: Vector3, direction: Vector3, params?: RaycastParams): "
					   "RaycastResult | null;\n";
				out << "\tRaycastThroughPortals(origin: Vector3, direction: Vector3, "
					   "params?: RaycastParams): RaycastResult | null;\n";
				out << "\tOverlapBox(centre: Vector3, size: Vector3, params?: RaycastParams): "
					   "Instance[];\n";
				out << "\tOverlapSphere(centre: Vector3, radius: number, params?: RaycastParams): "
					   "Instance[];\n";
				out << "\tBlockCast(from: CFrame, size: Vector3, motion: Vector3, "
					   "params?: RaycastParams): Instance[];\n";
				out << "\tSphereCast(from: Vector3, radius: number, motion: Vector3, "
					   "params?: RaycastParams): Instance[];\n";
			}

			out << "}\n\n";
		}

		WriteBusStatus(out, false);
		out << TYPESCRIPT_SERVICES;

		// `game` carries `GetService` alone. The Luau half also answers
		// `game.Workspace`, because `RunService.cpp` gives it a `__index`;
		// `JsBindings.cpp` builds a plain object with one property, so this one
		// does not.
		out << "declare const game: {\n";

		// Which world this script is standing on, by name - the same value the
		// Luau half answers and for the same reason. `JsBindings.cpp` sets it
		// as a plain string property rather than a getter, because a world's
		// name is fixed for its life and a getter would imply otherwise.
		out << "\treadonly JobId: string;\n";
		// **Every service in the tree, from the class table.** This was a
		// hand-written four while the Luau half had already been made to derive
		// its list, and the two drifted immediately: a panel asking for
		// `StarterGui` typechecked as an error against an engine that answers it
		// perfectly well, and the overloads collapsed to an intersection that
		// was not an `Instance` at all.
		//
		// A list edited by hand is a list nothing fails to update - the run time
		// keeps working and only the types disagree, which is the drift this
		// whole file exists to prevent.
		out << "\tGetService: {\n";
		for (const ClassId id : ServiceClasses()) {
			const std::string_view name = Classes::Describe(id).Name.Text();
			out << "\t\t(service: \"" << name << "\"): " << name << ";\n";
		}
		out << "\t\t(service: \"RunService\"): RunService;\n";
		out << "\t\t(service: \"ComputeService\"): ComputeService;\n";
		out << "\t\t(service: \"MessagingService\"): MessagingService;\n";
		out << "\t\t(service: \"MemoryStoreService\"): MemoryStoreService;\n";
		out << "\t\t(service: \"DataStoreService\"): DataStoreService;\n";

		// The two that step on the tick, which this language binds as well.
		out << "\t\t(service: \"TweenService\"): TweenService;\n";
		out << "\t\t(service: \"Debris\"): Debris;\n";

		// The seven that stopped being Luau's at v0.16. **The last asymmetry
		// between this list and the Luau map is gone**: `UserInputService` and
		// `SoundService` are in both, because `ServiceProperty` gave a live
		// property a shape both VMs can install.
		out << "\t\t(service: \"CrossWorldService\"): CrossWorldService;\n";
		out << "\t\t(service: \"ContentService\"): ContentService;\n";
		out << "\t\t(service: \"CollectionService\"): CollectionService;\n";
		out << "\t\t(service: \"HttpService\"): HttpService;\n";
		out << "\t\t(service: \"ContextActionService\"): ContextActionService;\n";
		out << "\t\t(service: \"UserInputService\"): UserInputService;\n";
		out << "\t\t(service: \"SoundService\"): SoundService;\n";
		out << "\t};\n";
		out << "};\n\n";

		// **A `new` property carrying overloads, not a construct signature.**
		// `Instance.new("Part")` is what the binding provides; `new
		// Instance("Part")` is what a construct signature would have described,
		// and this file described that for three versions.
		//
		// Services are absent because a script does not mint one - the same
		// `constructible` answer the manifest now carries.
		out << "declare const Instance: {\n";
		out << "\tnew: {\n";
		for (const ClassId id : ConstructibleClasses()) {
			const std::string_view name = Classes::Describe(id).Name.Text();
			out << "\t\t(className: \"" << name << "\", parent?: Instance): " << name << ";\n";
		}
		out << "\t};\n";
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
		"bindings", "atomic - generates the scripting manifest and declarations."
	);
	arguments.Flag("check", "Compare against the checked-in files instead of writing them");
	arguments.Value("out", "DIR", "Where the generated files live");

	const engine::core::Arguments::Result parsed = arguments.Parse(argc, argv);
	// Guarded on `parsed.Ok` because the block below folds a parse failure in
	// with `--help`, and a command line that did not parse should report that
	// rather than answer a question it may not have been asked.
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

	// Registering the classes is what populates the table: a manifest generated
	// from an empty table would be a valid file describing nothing, and its
	// drift check would pass forever.
	(void)engine::scene::PartClass();

	// **`ShaderScript` and `EditableMesh`, which `PartClass` does not reach.**
	// Both self-register through their own accessor rather than through
	// `RegisterSceneClasses` - `Registration.cpp`'s own comment on the first
	// explains why - so a bootstrap that only ever called `PartClass` and
	// `ServiceClass` left two classes' worth of properties undeclared in
	// both languages, silently: `Instance.new("EditableMesh")` still worked
	// at runtime, and an author reached for `.VertexCount` with no
	// completion and no type error to say the property was real.
	(void)engine::scene::ShaderScriptClass();
	(void)engine::scene::EditableMeshClass();
	(void)engine::scene::EditableImageClass();

	// The script classes too. A manifest that described `Part` and not `Script`
	// would be describing what a script can *build* and not what a world can
	// *hold*, and the second is the half a save file needs.
	(void)engine::script::ScriptClass();

	// The particle and ribbon classes, for the reason above: they are part of
	// what a script can name.
	engine::effects::RegisterEffectClasses();

	// **And the 2D tree, which a game file can carry and a script can build.**
	// A manifest that stopped at `Part` and `Script` would leave every
	// `TextLabel` property untyped in both declaration files - so an author
	// gets no completion for the half of v0.8 that is meant to be authored from
	// TypeScript, which is the point of generating this at all.
	(void)engine::gui::RegisterGuiClasses();

	// **And the services, because `workspace` is one.** Every script opens by
	// reaching into the world through the `Workspace` global, and a manifest
	// that stopped at `Part` had no type to give it - so the declaration files
	// either left the most-used global in the engine untyped or hand-wrote a
	// class the table already knew. This is also what makes `constructible`
	// answerable: `Service` is the ancestor that says a class is reached by
	// name rather than minted.
	(void)engine::scene::ServiceClass();

	// **The datatype enums, which no class registration reaches.** `EasingStyle`
	// and `EasingDirection` belong to `TweenInfo` rather than to any class, so
	// they arrive through the enum table alone - and both VMs used to register
	// them while opening, which is a moment this tool never has. The result was
	// a manifest that described `Material` and not `EasingStyle`, and a script
	// that got no completion for a value the run time accepts.
	engine::script::RegisterDatatypeEnums();

	const std::filesystem::path directory = arguments.Get("out").has_value()
												? std::filesystem::path(*arguments.Get("out"))
												: std::filesystem::path("mono.engine/script/bindings");

	const std::vector<Artefact> artefacts{
		{"manifest", directory / "manifest.json", Manifest()},
		{"luau declarations", directory / "engine.d.luau", LuauDeclarations()},
		{"typescript declarations", directory / "engine.d.ts", TypeScriptDeclarations()},
	};

	// **Before the artefacts are compared, and it runs on a write too.** A
	// descriptor that contradicts itself would otherwise be written into the
	// manifest, checked in, and then agree with itself for ever.
	if (const size_t faults = ReportDescriptorFaults(); faults > 0) {
		ENGINE_ERROR("{} property descriptor(s) contradict themselves; see above.", faults);
		return 1;
	}

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
			"The scripting surface changed. Run `just bindings` and review the diff - a change "
			"here is a change to what every script can name."
		);
		return 1;
	}

	ENGINE_INFO("bindings ok - {} artefact(s) match the class table", artefacts.size());
	return 0;
}
