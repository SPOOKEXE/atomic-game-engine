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
#include <engine/ecs/EnumTable.hpp>
#include <engine/effects/Registration.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Datatypes.hpp>
#include <engine/script/Instances.hpp>

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
		}
		return "field";
	}

	// The script type a property's value appears as, per language.
	//
	// **Takes the descriptor rather than the type**, because an `Enum` property
	// has no single answer: its script type is the enum it names, and that is
	// carried on the descriptor. A function of the type alone could only have
	// said `EnumItem`, which is the shape and not the type — and would have
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
			// "Material")` — the `importedTypeBindings` map, which `require`
			// populates and which a **host** may populate directly. That is
			// precisely what Roblox does, and it is what
			// `mono.tools/scriptcheck` now does: it walks the `Enum_*` types this
			// file emits and aliases each under the `Enum` prefix.
			//
			// **The declaration file keeps the flat name and scripts get both**,
			// which is forced by the order things happen in rather than chosen.
			// The aliases are registered *after* `loadDefinitionFile` returns —
			// they are built by walking the `Enum_*` types it created — so this
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
			// author has no way to observe from a script — `label.Text` is a
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
			// type and the table of members — see the namespace this emits below.
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
			// author has no way to observe from a script — `label.Text` is a
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
	// **A service is not constructible, and the table can now say so.** This
	// field shipped hard-coded `true` with a note that nothing in the class
	// table could express a service — one instance per world, reached by name,
	// never minted by a script. `scene::ServiceClass()` is that expression:
	// `Workspace` and `Lighting` derive from `Service`, so the question is an
	// `IsA` rather than a list of names, and a tenth service never touches this
	// function.
	//
	// The abstract bases — `Instance`, `BasePart`, `LuaSourceContainer` — stay
	// constructible here, because the *run time* still accepts them:
	// `Instances.cpp` looks the name up in the class table and mints whatever it
	// finds. `Explorer.cpp` filters them out of the class picker by name, which
	// is a decision about a palette rather than about the binding, and copying
	// that list into a second place is how the two would disagree later.
	bool Constructible(ClassId id) {
		const ClassId service = Classes::Find(engine::core::Name("Service"));
		return !service.IsValid() || !Classes::IsA(id, service);
	}

	// The classes a script reaches through `GetService`.
	//
	// **Derived from the class table rather than listed by hand, and that is
	// the fix rather than the tidy-up.** `GetService` resolves a name against
	// the world's roots — `RunService.cpp` says so — so *every* service
	// `scene::InstallServices` furnishes is reachable, and the hand-written
	// list of five said otherwise. `Interface.luau` asking for `StarterGui`
	// typechecked as an error against an engine that answers it perfectly well.
	//
	// A hand-written list has to be edited every time a service is added, and
	// nothing fails when it is not: the run time keeps working and only the
	// types disagree. That is the drift this file exists to prevent everywhere
	// else, arriving through the one list that was still manual.
	//
	// The bus services are *not* here — they are globals installed by the
	// bindings rather than instances in the tree — so they are appended by the
	// caller. Everything derived from `Service` is.
	std::vector<ClassId> ServiceClasses() {
		const ClassId service = Classes::Find(engine::core::Name("Service"));
		if (!service.IsValid()) {
			return {};
		}

		std::vector<ClassId> ids;
		for (const ClassId id : AllClasses()) {
			if (id != service && Classes::IsA(id, service)) {
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
	// **Own, not inherited.** Both declaration files now express inheritance —
	// TypeScript through `extends` on an interface and Luau through `extends` on
	// a `declare extern type` — so repeating an inherited property would be a
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
			// hard-coded `true` against the day a service arrived; services are
			// here now, so `Constructible` asks whether the class derives from
			// `Service` and both declaration files build their `Instance.new`
			// overloads from the same answer.
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
		// meaningful — `Static`, `Kinematic`, `Dynamic` is a progression and
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
	// `FFlag::LuauAllowGlobalDeclarationToBeCalledClass` — whose own comment says
	// the plan is to remove `declare class X [extends Y]`. Anything that enables
	// Luau's flags therefore rejects the class form outright, and `luau-lsp` does
	// exactly that: pointed at a file using it, the language server fails to load
	// the definitions at all and an author gets *no* completion rather than
	// slightly wrong completion. `mono.tools/scriptcheck` runs with default flags
	// and accepts both, so this generator would not have caught it — `just
	// luau-lsp` did, which is the argument for that recipe existing.
	//
	// **The signals are three classes because a definition file cannot declare a
	// generic one.** Roblox spells this `RBXScriptSignal<T...>` and that syntax
	// does not parse here — so each signal is named for what it hands its
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
	function __add(self, other: Vector3): Vector3
	function __sub(self, other: Vector3): Vector3
	function __mul(self, other: Vector3 | number): Vector3
	function __unm(self): Vector3
end

declare Vector3: {
	new: (x: number?, y: number?, z: number?) -> Vector3,

	-- **Lowercase, because Roblox's are.** Every other member of this vocabulary
	-- is capitalised and these two are not, which reads as a mistake until you
	-- try to run a script written elsewhere.
	zero: Vector3,
	one: Vector3,
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
	function __mul(self, other: CFrame): CFrame
end

declare CFrame: {
	new: ((x: number?, y: number?, z: number?) -> CFrame) & ((position: Vector3) -> CFrame),
	-- Radians, because Roblox's is radians — while `Orientation` is degrees.
	Angles: (pitch: number, yaw: number, roll: number) -> CFrame,
	lookAt: (from: Vector3, to: Vector3, up: Vector3?) -> CFrame,
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

declare extern type UserInputServiceType with
	MouseBehavior: Enum_MouseBehavior
	MouseDeltaSensitivity: number
	read KeyboardEnabled: boolean
	read MouseEnabled: boolean

	read InputBegan: PropertyChangedSignal
	read InputEnded: PropertyChangedSignal
	read InputChanged: PropertyChangedSignal

	function IsKeyDown(self, key: Enum_KeyCode): boolean
	function IsMouseButtonPressed(self, button: Enum_UserInputType): boolean
	function GetMouseLocation(self): Vector2
	function GetMouseDelta(self): Vector2
	function GetKeysPressed(self): { Enum_KeyCode }
end

declare UserInputService: UserInputServiceType

declare extern type ContextActionServiceType with
	-- The touch-button argument is accepted and ignored: there is no touch
	-- surface, and refusing it would make a Roblox script fail on a line that
	-- describes something this engine simply does not have.
	function BindAction(
		self,
		name: string,
		handler: (string, Enum_UserInputState, Enum_KeyCode) -> (),
		createTouchButton: boolean,
		...: Enum_KeyCode
	): ()

	function BindActionAtPriority(
		self,
		name: string,
		handler: (string, Enum_UserInputState, Enum_KeyCode) -> (),
		createTouchButton: boolean,
		priority: number,
		...: Enum_KeyCode
	): ()

	function UnbindAction(self, name: string): ()
	function UnbindAllActions(self): ()
end

declare ContextActionService: ContextActionServiceType

declare extern type PropertyChangedSignal with
	function Connect(self, handler: () -> ()): RBXScriptConnection
	function Once(self, handler: () -> ()): RBXScriptConnection
end

-- The 2D tree's input, in two shapes because the arguments differ.
--
-- **`GuiSignal` takes no arguments and that is deliberate rather than
-- unfinished.** Roblox hands `Activated`, `InputBegan` and `InputEnded` an
-- `InputObject`; this engine has no such datatype, and inventing a different
-- one now would have to change the day one arrives. A handler that cannot rely
-- on an argument is better than one relying on an argument that will move.
declare extern type GuiSignal with
	function Connect(self, handler: () -> ()): RBXScriptConnection
	function Once(self, handler: () -> ()): RBXScriptConnection
end

-- `(x, y)` in canvas pixels, which is Roblox's signature for the three pointer
-- signals exactly.
declare extern type PointerSignal with
	function Connect(self, handler: (x: number, y: number) -> ()): RBXScriptConnection
	function Once(self, handler: (x: number, y: number) -> ()): RBXScriptConnection
end

-- --- queries ---------------------------------------------------------------

declare extern type RaycastParams with
	CollisionGroup: string
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
	// A sequence keypoint is a table rather than a type of its own —
	// `{time, value, envelope}` for a number and `{time, Color3}` for a colour —
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
-- accepted by both constructors — the table form is how a gradient is written
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
end

declare Random: {
	new: (seed: number?) -> Random,
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

	// The globals that reach the class tree, so they are emitted after it.
	//
	// **`RunService` is a class and a value of the same name**, which is legal
	// because Luau keeps types and values in separate namespaces and is how
	// Roblox's own declarations spell a service.
	//
	// The bus services carry `(value, status, version)` back, which is the shape
	// `Services.cpp` resumes a suspended thread with. Roblox's `GetAsync` returns
	// the value alone and swallows the rest; the status rides beside it here
	// because a refusal a script cannot see is a refusal it cannot handle.
	constexpr const char *LUAU_SERVICES =
		R"LUAU(-- --- the bus services ------------------------------------------------------

export type BusStatus = "Ok" | "NotFound" | "Conflict" | "OverBudget" | "NoSuchWorld" | "Unsupported" | "Unknown"

declare extern type MessagingService with
	function PublishAsync(self, topic: string, message: any): (any, BusStatus, number)
	function SubscribeAsync(self, topic: string, handler: (message: any) -> ()): ()
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
-- A `MeshId` is a name a publisher wrote — an id does not cross — and until
-- this there was no way for a script to ask what those names were. Every list
-- is sorted, so a scene that lays content out arranges itself the same way on
-- every run.
declare extern type ContentService with
	-- Every mesh registered in this world, sorted. Empty on a headless server
	-- and before content has arrived, which are the same honest answer.
	function GetMeshes(self): { string }

	-- Every mesh the *store* published, sorted — the signed manifest, which a
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

	-- Every texture registered in this world, sorted.
	function GetTextures(self): { string }

	-- A texture's flipbook grid and authored rate, or nil when it is a still
	-- image or this world has not been told about it.
	function GetFlipbook(self, texture: string): { Side: number, Frames: number, FrameRate: number }?

	-- The same number `MeshPart.TrianglesCount` gives, asked about a mesh
	-- rather than a part — so a layout can be sized before anything is built.
	function GetTriangleCount(self, mesh: string): number
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
declare MemoryStoreService: MemoryStoreService
declare DataStoreService: DataStoreService
declare RunService: RunService
declare ContentService: ContentService

-- The world this script runs on. `game` is the universe above it.
declare workspace: Workspace

-- The instance this chunk was loaded from, set on the thread after the sandbox.
declare script: LuaSourceContainer

-- `require(moduleScript)`, and declaring it here is what makes it checkable.
--
-- **Upstream's `require` is resolved by the *frontend* rather than typed by the
-- checker**: seeing a call to the global, it hands the argument to a module
-- resolver expecting a path, and reports `Unknown require: unsupported path` for
-- anything else. Every argument in this engine is an instance, so every module
-- a script loaded was an error — which made `require` unusable under
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
		out << "-- the two surfaces cannot drift into two APIs by accident — where they do\n";
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
				out << "\t";

				// **`read` is Luau's `readonly`, and without it the two
				// languages disagreed about the same descriptor.** The
				// TypeScript half below has emitted `readonly` since it was
				// written; this half emitted nothing, so a Luau script
				// assigning `MeshPart.TrianglesCount` typechecked clean and was
				// then refused at run time by `Store::SetProperty` — a
				// diagnostic arriving one layer later than the one that exists
				// to prevent it.
				if (!property.Writable) {
					out << "read ";
				}

				out << property.Name.Text() << ": " << LuauType(property) << "\n";
			}

			// **The host members, which project onto no component and never
			// will.** `.Changed` and the methods below it are the binding's
			// rather than the class table's — declaring components for them so
			// this loop could find them is the change `script/AGENTS.md` says to
			// refuse. They are written here, against the one class that has
			// them, so they are declared once and inherited by the rest.
			if (name == "Instance") {
				out << "\tChanged: ChangedSignal\n";
				out << "\tfunction IsA(self, className: string): boolean\n";

				// **The tags, which sit beside `IsA` in `OpenInstances`' method
				// table and were missing here.** v0.9 added them to the run time
				// and not to this generator, so every script calling `AddTag`
				// typechecked as an error against a method the engine answers —
				// `examples/Meshes.luau` among them. A host member is only
				// declared where it is written down, and there is no test that
				// would notice one of these tables growing without the other.
				out << "\tfunction AddTag(self, tag: string): boolean\n";
				out << "\tfunction RemoveTag(self, tag: string): boolean\n";
				out << "\tfunction HasTag(self, tag: string): boolean\n";

				out << "\tfunction Destroy(self): ()\n";
				out << "\tfunction Clone(self): Instance\n";
				out << "\tfunction GetChildren(self): { Instance }\n";
				out << "\tfunction GetDescendants(self): { Instance }\n";
				out << "\tfunction FindFirstChild(self, name: string): Instance?\n";
				out << "\tfunction IsDescendantOf(self, ancestor: Instance): boolean\n";
				out << "\tfunction ClearAllChildren(self): ()\n";

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
				out << "\tfunction GetPropertyChangedSignal(self, property: string): "
					   "PropertyChangedSignal\n";

				// **Attributes, and the value type is a union rather than
				// `any`.** An attribute holds one of a closed set —
				// `ecs::AttributeTypeAllowed` refuses everything else — so a
				// union is what the run time actually accepts, and `any` would
				// typecheck `part:SetAttribute("k", part)` which the binding
				// refuses at run time.
				//
				// `nil` is in the union on the way *in* because that is how an
				// attribute is removed, and on the way *out* because that is what
				// an unset one reads as.
				//
				// **The alias is at file scope, not here.** A `declare extern
				// type ... with` block takes members and not type aliases —
				// putting one inside is a syntax error four lines later that
				// reads as an unclosed function, which is exactly how it was
				// found. `EngineAttribute` is declared beside the datatypes.
				out << "\tfunction GetAttribute(self, name: string): EngineAttribute?\n";
				out << "\tfunction SetAttribute(self, name: string, value: EngineAttribute?): ()\n";
				out << "\tfunction GetAttributes(self): { [string]: EngineAttribute }\n";
				out << "\tfunction GetAttributeChangedSignal(self, name: string): "
					   "PropertyChangedSignal\n";

				// **The 2D tree's input, declared on `Instance` rather than on
				// `GuiObject`.** That is what the run time does — `InstanceIndex`
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
				out << "\tInputBegan: GuiSignal\n";
				out << "\tInputEnded: GuiSignal\n";
				out << "\tMouseEnter: PointerSignal\n";
				out << "\tMouseLeave: PointerSignal\n";
				out << "\tMouseMoved: PointerSignal\n";
			}

			// The member only the Workspace answers, for the reason
			// `Instances.cpp` keeps it in a table of its own: a `Raycast` on a
			// `Folder` would be an answer that means nothing.
			//
			// A raycast result is a plain table at run time — read once and
			// discarded — so it is written inline rather than given a name.
			//
			// **`CurrentCamera` used to be written here too, and removing it was
			// a fix rather than a tidy-up** — the same correction the `Name` line
			// on `Instance` went through at v0.5, and it failed the same way. It
			// was hand-written because it *was* a special case: `PushCurrentCamera`
			// served it from the `ActiveCamera` resource and no property projected
			// it. v0.10 declared one, so the loop below started emitting it as
			// well — and an extern type with the member twice is not a warning,
			// it is an assertion failure inside Luau's constraint generator, which
			// `just typecheck` reports as `SIGILL` on every script in the tree.
			//
			// What is lost is the type: a declared `Reference` property is
			// `Instance` and this said `Camera?`, so a script assigning a `Part`
			// to `workspace.CurrentCamera` now typechecks and is refused at run
			// time instead. That is the same trade every other reference property
			// makes — `Parent: Instance` — and narrowing it needs
			// `PropertyDescriptor` to carry which class a reference points at,
			// which is a change to `ecs` rather than to this generator.
			if (name == "Workspace") {
				out << "\tfunction Raycast(self, origin: Vector3, direction: Vector3, "
					   "params: RaycastParams?): {\n";
				out << "\t\tInstance: Instance,\n";
				out << "\t\tPosition: Vector3,\n";
				out << "\t\tNormal: Vector3,\n";
				out << "\t\tDistance: number,\n";

				// **A string, where this said `Enum_Material` until v0.10.** The
				// enum is gone and the field is `Surface::Material` now — the row
				// a contact reads friction out of — which is a plain name.
				// `script/LuauQuery.cpp` carries why a hit result reports that
				// one rather than resolving the part's `Material` instance.
				out << "\t\tMaterial: string,\n";
				out << "\t}?\n";
			}

			out << "end\n\n";
		}

		out << LUAU_SERVICES;

		// **A table of names to types, read with `index`, rather than one
		// overload per class.** Both of these started as an intersection of
		// function types — which is how a reader would first write "one
		// signature per class name" — and the type checker refused the file
		// outright with *"Code is too complex to typecheck"* at nine of them.
		// An intersection is solved by trying every branch; a lookup is one
		// step, and it stays one step when the tree doubles.
		//
		// `keyof` narrows the argument to a name that exists, so a typo is
		// still an error at the call rather than an `Instance` handed back.
		out << "-- --- what a name resolves to --------------------------------------------\n\n";

		out << "type Services = {\n";

		// Everything in the tree, from the class table — see `ServiceClasses`
		// for why this is derived rather than listed.
		for (const ClassId id : ServiceClasses()) {
			const std::string_view name = Classes::Describe(id).Name.Text();
			out << "\t" << name << ": " << name << ",\n";
		}

		// The bus services, which are globals rather than instances and so are
		// not in the tree for the walk above to find.
		out << "\tRunService: RunService,\n";
		out << "\tMessagingService: MessagingService,\n";
		out << "\tContentService: ContentService,\n";
		out << "\tMemoryStoreService: MemoryStoreService,\n";
		out << "\tDataStoreService: DataStoreService,\n";
		out << "}\n\n";

		out << "type CreatableInstances = {\n";
		for (const ClassId id : ConstructibleClasses()) {
			const std::string_view name = Classes::Describe(id).Name.Text();
			out << "\t" << name << ": " << name << ",\n";
		}
		out << "}\n\n";

		// **`game` is the universe, and `GetService` is a global lookup at run
		// time** — so `Services` above is exactly the globals that exist, plus
		// `Workspace`, which `RunService.cpp` special-cases because its global
		// is spelled lower case.
		//
		// **Named `DataModel`, and `self` is that name rather than `any`.** The
		// name is the run time's — `OpenGame` sets `__metatable` to the same
		// string — and it has to be written down because a generic method whose
		// `self` is `any` does not infer: `game:GetService("RunService")`
		// resolved `T` to `any` and handed back a service with no members. A
		// concrete `self` is what makes the singleton argument reach `T`.
		out << "type DataModel = {\n";
		out << "\tWorkspace: Workspace,\n";

		// **Which world this script is standing on**, by name. Roblox's
		// `JobId` identifies the server instance, and a world here *is* that
		// instance — see `RunService.cpp`. `Mirrors-4-worlds` is the caller:
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
	// bug.** This declared `{ new(x?: number, ...): Vector3 }` — which inside an
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
	Equals(other: Vector3): boolean;
}

declare interface Color3 {
	readonly R: number;
	readonly G: number;
	readonly B: number;
	Equals(other: Color3): boolean;
}

declare interface CFrame {
	readonly Position: Vector3;
	mul(other: CFrame): CFrame;
}

declare const Vector3: {
	new: (x?: number, y?: number, z?: number) => Vector3;

	// Lowercase, because Roblox's are. The Luau half carries the argument.
	readonly zero: Vector3;
	readonly one: Vector3;
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
	};
	// Radians, because Roblox's is radians -- while `Orientation` is degrees.
	Angles: (pitch: number, yaw: number, roll: number) => CFrame;
	lookAt: (from: Vector3, to: Vector3, up?: Vector3) => CFrame;
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

// What an attribute may hold, which is `ecs::AttributeTypeAllowed`'s closed set.
//
// **Named at file scope rather than nested**, because TypeScript has no
// interface-scoped type alias the way the Luau half does — so the name is
// prefixed to say where it belongs rather than risking a collision with a game's
// own `Attribute`.
// --- input ------------------------------------------------------------------
//
// Hand-written for the Luau half's reason: these are globals, not classes.
declare interface UserInputServiceType {
	MouseBehavior: Enum.MouseBehavior;
	MouseDeltaSensitivity: number;
	readonly KeyboardEnabled: boolean;
	readonly MouseEnabled: boolean;

	readonly InputBegan: PropertyChangedSignal;
	readonly InputEnded: PropertyChangedSignal;
	readonly InputChanged: PropertyChangedSignal;

	IsKeyDown(key: Enum.KeyCode): boolean;
	IsMouseButtonPressed(button: Enum.UserInputType): boolean;
	GetMouseLocation(): Vector2;
	GetMouseDelta(): Vector2;
	GetKeysPressed(): Enum.KeyCode[];
}

declare const UserInputService: UserInputServiceType;

declare interface ContextActionServiceType {
	BindAction(
		name: string,
		handler: (name: string, state: Enum.UserInputState, key: Enum.KeyCode) => void,
		createTouchButton: boolean,
		...keys: Enum.KeyCode[]
	): void;

	BindActionAtPriority(
		name: string,
		handler: (name: string, state: Enum.UserInputState, key: Enum.KeyCode) => void,
		createTouchButton: boolean,
		priority: number,
		...keys: Enum.KeyCode[]
	): void;

	UnbindAction(name: string): void;
	UnbindAllActions(): void;
}

declare const ContextActionService: ContextActionServiceType;

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
// `InputObject`, this engine has no such datatype, and a different one invented
// now would have to change the day one arrives.
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

// --- queries ---------------------------------------------------------------

declare interface RaycastParams {
	CollisionGroup: string;
}

declare const RaycastParams: {
	new: () => RaycastParams;
};

declare interface RaycastResult {
	readonly Instance: Instance;
	readonly Position: Vector3;
	readonly Normal: Vector3;
	readonly Distance: number;

	// The `Surface` name — what the part is like to touch, not what it looks
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
}

declare const Random: {
	new: (seed?: number) => Random;
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
		R"TS(// --- the bus services ------------------------------------------------------

declare type BusStatus =
	| "Ok"
	| "NotFound"
	| "Conflict"
	| "OverBudget"
	| "NoSuchWorld"
	| "Unsupported"
	| "Unknown";

declare interface StoreReply {
	readonly Value: unknown;
	readonly Status: BusStatus;
	readonly Version: number;
}

declare interface MessagingService {
	PublishAsync(topic: string, message: unknown): Promise<StoreReply>;
	SubscribeAsync(topic: string, handler: (message: unknown) => void): void;
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

// --- the globals -----------------------------------------------------------

declare const MessagingService: MessagingService;
declare const MemoryStoreService: MemoryStoreService;
declare const DataStoreService: DataStoreService;
declare const RunService: RunService;

// The world this script runs on. `game` is the universe above it.
declare const workspace: Workspace;

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
		// one name meaning two things rather than a redeclaration — the type in a
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
			// `readonly Name: string;` on `Instance` — which then declared
			// `Name: string;` again from the property loop, because `Name` has
			// been a real property since v0.5. Two declarations of one member
			// that *disagree about whether it can be assigned*, in the file whose
			// whole job is to tell an author what they may write.
			//
			// A Roblox script sets `.Name`, so the property's own answer —
			// writable — is the right one, and it is the one that survives now
			// that nothing competes with it.
			for (const PropertyDescriptor &property : OwnProperties(info)) {
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

				// The tags, for the reason the Luau half states.
				out << "\tAddTag(tag: string): boolean;\n";
				out << "\tRemoveTag(tag: string): boolean;\n";
				out << "\tHasTag(tag: string): boolean;\n";

				out << "\tDestroy(): void;\n";
				out << "\tClone(): Instance;\n";
				out << "\tGetChildren(): Instance[];\n";
				out << "\tGetDescendants(): Instance[];\n";
				out << "\tFindFirstChild(name: string): Instance | null;\n";
				out << "\tIsDescendantOf(ancestor: Instance): boolean;\n";
				out << "\tClearAllChildren(): void;\n";

				// The pivot pair, matching the Luau half and declared in the
				// same place for the same reason.
				out << "\tGetPivot(): CFrame;\n";
				out << "\tPivotTo(target: CFrame): void;\n";
				out << "\tGetPropertyChangedSignal(property: string): PropertyChangedSignal;\n";

				// Attributes, matching the Luau half. The union is the same
				// closed set and for the same reason.
				out << "\tGetAttribute(name: string): EngineAttribute | null;\n";
				out << "\tSetAttribute(name: string, value: EngineAttribute | null): void;\n";
				out << "\tGetAttributes(): { [name: string]: EngineAttribute };\n";
				out << "\tGetAttributeChangedSignal(name: string): PropertyChangedSignal;\n";

				// The 2D tree's input, on `Instance` for the reason the Luau
				// half states at length: the run time answers them for any
				// instance, and a type narrower than the run time refuses code
				// the engine accepts.
				out << "\treadonly Activated: GuiSignal;\n";
				out << "\treadonly InputBegan: GuiSignal;\n";
				out << "\treadonly InputEnded: GuiSignal;\n";
				out << "\treadonly MouseEnter: PointerSignal;\n";
				out << "\treadonly MouseLeave: PointerSignal;\n";
				out << "\treadonly MouseMoved: PointerSignal;\n";
			}

			// The member only the Workspace answers, matching the Luau half.
			//
			// `CurrentCamera` is gone from here for the reason the Luau half
			// gives: it is a declared property now and the loop above emits it.
			// TypeScript would have merged the duplicate rather than crashing,
			// which is worse — the hand-written `Camera | null` and the generated
			// `Instance` would have intersected to something no assignment
			// satisfies.
			if (name == "Workspace") {
				out << "\tRaycast(origin: Vector3, direction: Vector3, params?: RaycastParams): "
					   "RaycastResult | null;\n";
			}

			out << "}\n\n";
		}

		out << TYPESCRIPT_SERVICES;

		// `game` carries `GetService` alone. The Luau half also answers
		// `game.Workspace`, because `RunService.cpp` gives it a `__index`;
		// `JsBindings.cpp` builds a plain object with one property, so this one
		// does not.
		out << "declare const game: {\n";

		// Which world this script is standing on, by name — the same value the
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
		// A list edited by hand is a list nothing fails to update — the run time
		// keeps working and only the types disagree, which is the drift this
		// whole file exists to prevent.
		out << "\tGetService: {\n";
		for (const ClassId id : ServiceClasses()) {
			const std::string_view name = Classes::Describe(id).Name.Text();
			out << "\t\t(service: \"" << name << "\"): " << name << ";\n";
		}
		out << "\t\t(service: \"RunService\"): RunService;\n";
		out << "\t\t(service: \"MessagingService\"): MessagingService;\n";
		out << "\t\t(service: \"MemoryStoreService\"): MemoryStoreService;\n";
		out << "\t\t(service: \"DataStoreService\"): DataStoreService;\n";
		out << "\t};\n";
		out << "};\n\n";

		// **A `new` property carrying overloads, not a construct signature.**
		// `Instance.new("Part")` is what the binding provides; `new
		// Instance("Part")` is what a construct signature would have described,
		// and this file described that for three versions.
		//
		// Services are absent because a script does not mint one — the same
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

	// The script classes too. A manifest that described `Part` and not `Script`
	// would be describing what a script can *build* and not what a world can
	// *hold*, and the second is the half a save file needs.
	(void)engine::script::ScriptClass();

	// The particle and ribbon classes, for the reason above: they are part of
	// what a script can name.
	engine::effects::RegisterEffectClasses();

	// **And the 2D tree, which a game file can carry and a script can build.**
	// A manifest that stopped at `Part` and `Script` would leave every
	// `TextLabel` property untyped in both declaration files — so an author
	// gets no completion for the half of v0.8 that is meant to be authored from
	// TypeScript, which is the point of generating this at all.
	(void)engine::gui::RegisterGuiClasses();

	// **And the services, because `workspace` is one.** Every script opens by
	// reaching into the world through the `Workspace` global, and a manifest
	// that stopped at `Part` had no type to give it — so the declaration files
	// either left the most-used global in the engine untyped or hand-wrote a
	// class the table already knew. This is also what makes `constructible`
	// answerable: `Service` is the ancestor that says a class is reached by
	// name rather than minted.
	(void)engine::scene::ServiceClass();

	// **The datatype enums, which no class registration reaches.** `EasingStyle`
	// and `EasingDirection` belong to `TweenInfo` rather than to any class, so
	// they arrive through the enum table alone — and both VMs used to register
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
