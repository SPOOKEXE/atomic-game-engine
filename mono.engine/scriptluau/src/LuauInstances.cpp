#include "LuauBindings.hpp"

#include <engine/core/Log.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Subtree.hpp>

#include <algorithm>
#include <lualib.h>
#include <string_view>
#include <vector>

namespace engine::script {

	namespace {
		using core::Name;
		using ecs::Entity;
		using ecs::PropertyDescriptor;
		using ecs::PropertyType;
		using ecs::Store;

		// The world every instance in this VM belongs to.
		//
		// Reached through the context on a light-userdata upvalue rather than a
		// global, so two runtimes over two worlds cannot reach each other's
		// store - the mistake a file-static would make available.
		Store &InstanceStoreOf(lua_State *state) {
			return *UpvalueContext(state).World;
		}

		Entity CheckInstance(lua_State *state, int index) {
			void *value = lua_touserdatatagged(state, index, TAG_INSTANCE);
			if (value == nullptr) {
				luaL_typeerrorL(state, index, "Instance");
			}
			return *static_cast<Entity *>(value);
		}

		// This world's `Workspace`, as `OpenWorkspace` resolved it.
		//
		// **From the registry rather than from `WorkspaceOf`**, which is a scan
		// of every root in the world. This is read on the miss path of a member
		// lookup, so it has to be a table read and a compare rather than a
		// search.
		Entity WorkspaceEntity(lua_State *state) {
			lua_getfield(state, LUA_REGISTRYINDEX, "engine.workspace");
			Entity workspace = ecs::NULL_ENTITY;
			if (void *value = lua_touserdatatagged(state, -1, TAG_INSTANCE); value != nullptr) {
				workspace = *static_cast<Entity *>(value);
			}
			lua_pop(state, 1);
			return workspace;
		}

		void PushInstance(lua_State *state, Entity entity) {
			void *memory = lua_newuserdatatagged(state, sizeof(Entity), TAG_INSTANCE);
			*static_cast<Entity *>(memory) = entity;

			luaL_getmetatable(state, "Instance");
			lua_setmetatable(state, -2);
		}

		// --- marshalling -----------------------------------------------------
		//
		// **A switch over `PropertyType` and nothing else.** No property is
		// named here and none ever should be: a property `scene` declares
		// tomorrow is readable and writable from Luau today, because this code
		// only ever learned the shapes a value can have.

		// **Both halves live at the bottom of this file and are declared in
		// `LuauBindings.hpp`.** They used to be file-local and took a
		// `PropertyDescriptor`, which was right while a property was the only
		// thing with a `PropertyType`. It is not: v0.12's ECS surface marshals
		// the same value types for a **component field**, which is not a
		// property and has no descriptor. Taking the type and the enum name
		// instead of the descriptor is what keeps that one switch rather than
		// two that agree until somebody edits one.

		// The method table, built once and shared by every instance.
		//
		// On the metatable rather than on each userdata, for the reason the
		// JavaScript side puts accessors on a prototype: a scene of five hundred
		// parts would otherwise carry five hundred copies of one closure.
		void PushMethods(lua_State *state) {
			lua_getfield(state, LUA_REGISTRYINDEX, "engine.instance.methods");
		}

		// --- the metatable ---------------------------------------------------

		// The signals `InstanceIndex` answers, in the order it tests them.
		//
		// **A list beside a branch chain, which is the shape this file
		// otherwise refuses.** It is here because the chain below is not
		// enumerable: a signal is not a property and not a member of any table,
		// so it exists only as a string comparison, and nothing can walk a
		// comparison. An editor offering `part.Changed` has to be told.
		//
		// The duplication is real and is why `engine.scripthost.vocabulary`
		// checks every entry against a live VM rather than trusting this to stay
		// in step. Add a branch below, add a name here, and the suite says so if
		// you forget.
		//
		// **It stays in this module.** Until v0.19 `script/Signals.hpp` declared
		// the accessor below so that `script/src/Vocabulary.cpp` could name the
		// list one layer down, which made an L9 file reference a symbol an L10
		// module defines. `LuauRuntime::Surface` is the only caller now and the
		// declaration lives in `LuauBindings.hpp` beside it.
		constexpr std::string_view SIGNALS[] = {
			"Changed",
			"ChildAdded",
			"ChildRemoved",
			"DescendantAdded",
			"DescendantRemoving",
			"AncestryChanged",
			"PlayerAdded",
			"PlayerRemoving",
			"CharacterAdded",
			"CharacterRemoving",

			// The 2D tree's input, and then the keyboard pair a `TextBox`
			// carries. Grouped with blank lines and a comment so the list stays
			// one name per line: without them the formatter packs nineteen
			// short strings into a grid nobody can add to without reflowing it.
			"Activated",
			"MouseButton1Click",
			"InputBegan",
			"InputEnded",
			"MouseEnter",
			"MouseLeave",
			"MouseMoved",
			"Focused",
			"FocusLost",
		};

		int InstanceIndex(lua_State *state) {
			Store &store = InstanceStoreOf(state);
			const Entity instance = CheckInstance(state, 1);
			const char *field = luaL_checkstring(state, 2);
			const std::string_view name(field);

			// **`.Changed` before the property lookup**, because it is not a
			// property and never will be: it projects onto no component, and
			// declaring one for scripts alone is the change `script/AGENTS.md`
			// says to refuse.
			if (name == "Changed") {
				PushSignal(state, SignalKind::Changed, instance);
				return 1;
			}

			// The tree's own signals, for the same reason and in the same
			// place: none of them projects onto a component either.
			if (name == "ChildAdded") {
				PushSignal(state, SignalKind::ChildAdded, instance);
				return 1;
			}
			if (name == "ChildRemoved") {
				PushSignal(state, SignalKind::ChildRemoved, instance);
				return 1;
			}
			if (name == "DescendantAdded") {
				PushSignal(state, SignalKind::DescendantAdded, instance);
				return 1;
			}
			if (name == "DescendantRemoving") {
				PushSignal(state, SignalKind::DescendantRemoving, instance);
				return 1;
			}
			if (name == "AncestryChanged") {
				PushSignal(state, SignalKind::AncestryChanged, instance);
				return 1;
			}

			// **Offered on every instance rather than gated to `Players`**,
			// matching every signal above it and for the reason the gui pair
			// below gives: a connection on anything else is inert by
			// construction, because nothing ever fires one at a subject that is
			// not the service. A class test here would run on every field
			// access on every instance to turn a signal that never fires into
			// an error.
			if (name == "PlayerAdded") {
				PushSignal(state, SignalKind::PlayerAdded, instance);
				return 1;
			}
			if (name == "PlayerRemoving") {
				PushSignal(state, SignalKind::PlayerRemoving, instance);
				return 1;
			}

			// **A player's own pair, offered on every instance for the reason
			// the two above it are.** Nothing fires one at a subject that is not
			// a `Player`, so a connection on a `Part` is inert by construction -
			// which is the same answer a class gate would give, at none of the
			// cost of testing a class on every field access in the world.
			if (name == "CharacterAdded") {
				PushSignal(state, SignalKind::CharacterAdded, instance);
				return 1;
			}
			if (name == "CharacterRemoving") {
				PushSignal(state, SignalKind::CharacterRemoving, instance);
				return 1;
			}

			// The 2D tree's input, in the same place and for the same reason:
			// none of these projects onto a component either.
			//
			// **Offered on every instance rather than only on a `GuiObject`.**
			// Roblox does gate them by class, and matching that would mean a
			// class test on a lookup that already runs for every field access on
			// every instance - to produce, in the failing case, a connection
			// that never fires instead of an error. `gui::Router` only ever
			// names elements it found in a compiled draw list, so a connection
			// on a `Part` is inert by construction, which is the same answer at
			// none of the cost.
			// **`MouseButton1Click` beside `Activated`, and they are one signal
			// under two names rather than two lists.** Roblox has both - the
			// second on `GuiButton` - and this engine's router produces exactly
			// one primary button, so the two questions have one answer here. A
			// second `SignalKind` would be a second list to fan the same event
			// out to, and the first handler an author wrote against the name
			// this file did not know would never fire.
			//
			// **Not `InputChanged`, which stays absent.** Roblox's fires for
			// pointer motion *and* the wheel over an element; `gui::Router`
			// produces motion only, and `MouseMoved` already carries it with the
			// position an argument-less `InputChanged` could not. Half a member
			// under a familiar name is the trade `SoundService.cpp` refuses a
			// list of.
			if (name == "Activated" || name == "MouseButton1Click") {
				PushSignal(state, SignalKind::GuiActivated, instance);
				return 1;
			}
			if (name == "InputBegan") {
				PushSignal(state, SignalKind::GuiInputBegan, instance);
				return 1;
			}
			if (name == "InputEnded") {
				PushSignal(state, SignalKind::GuiInputEnded, instance);
				return 1;
			}
			if (name == "MouseEnter") {
				PushSignal(state, SignalKind::GuiMouseEnter, instance);
				return 1;
			}
			if (name == "MouseLeave") {
				PushSignal(state, SignalKind::GuiMouseLeave, instance);
				return 1;
			}
			if (name == "MouseMoved") {
				PushSignal(state, SignalKind::GuiMouseMoved, instance);
				return 1;
			}

			// A `UIDragDetector`'s three, offered on every instance for the
			// reason the six above are: only a detector is ever the subject of
			// one - `gui::Router` names the modifier it found - so a connection
			// on anything else is inert by construction.
			if (name == "DragStart") {
				PushSignal(state, SignalKind::GuiDragBegan, instance);
				return 1;
			}
			if (name == "DragContinue") {
				PushSignal(state, SignalKind::GuiDragContinue, instance);
				return 1;
			}
			if (name == "DragEnd") {
				PushSignal(state, SignalKind::GuiDragEnded, instance);
				return 1;
			}

			// **A `TextBox`'s pair, offered on every instance for the reason the
			// six above it are.** Only a `TextBox` can take the keyboard -
			// `gui::Focus` refuses anything with no `Entry` component - so a
			// connection on a `Frame` is inert by construction rather than by a
			// class test on every field access in the world.
			if (name == "Focused") {
				PushSignal(state, SignalKind::GuiFocused, instance);
				return 1;
			}
			if (name == "FocusLost") {
				PushSignal(state, SignalKind::GuiFocusLost, instance);
				return 1;
			}

			const PropertyDescriptor *property = ScriptableProperty(store, instance, field);
			if (property != nullptr) {
				// **The one type that cannot ride the shared byte buffer**, and
				// it is worth saying why rather than leaving it to look like an
				// inconsistency. Every other property is trivially copyable, so
				// a `descriptor.Get` into raw bytes is a copy into storage that
				// needed no construction. A `PropertyType::String` value owns an
				// allocation and its getter *assigns* - assigning into
				// uninitialised bytes is undefined behaviour, not a fast path.
				//
				// So this one gets a real object to be assigned into. The cost
				// is one `std::string` on the stack, on the path that already
				// scanned a class's property list.
				if (property->Type == PropertyType::String) {
					std::string text;
					if (!store.GetProperty(instance, *property, &text, sizeof(text))) {
						luaL_errorL(state, "could not read '%s'", field);
					}
					lua_pushlstring(state, text.data(), text.size());
					return 1;
				}

				// Sized from the descriptor rather than from a guess, so this
				// cannot be the place a size mismatch is introduced.
				// **Through the descriptor this function already found**, not
				// through its name. The by-name overload would call
				// `Classes::Describe` and scan the class's property list a
				// second time, for a descriptor that is in scope - which is
				// half the class-table traffic of a scripted frame.
				alignas(16) unsigned char bytes[WIDEST_PROPERTY] = {};
				if (property->Size > sizeof(bytes) ||
					!store.GetProperty(instance, *property, bytes, property->Size)) {
					luaL_errorL(state, "could not read '%s'", field);
				}

				if (!PushPropertyValue(state, property->Type, property->EnumName, bytes)) {
					luaL_errorL(state, "'%s' has no script representation", field);
				}
				return 1;
			}

			// A method next.
			PushMethods(state);
			lua_getfield(state, -1, field);
			if (!lua_isnil(state, -1)) {
				lua_remove(state, -2);
				return 1;
			}
			lua_pop(state, 2);

			// --- what only the Workspace answers ----------------------------
			//
			// **A second table rather than more entries in the shared one.**
			// `Raycast` is a query against a world and `CurrentCamera` is which
			// eye is live; offering either on a `Folder` would be offering an
			// answer that means nothing. They were on the world object before
			// this and they stay together now that the world object is gone -
			// what changed is which instance they hang off, not what they are.
			//
			// Checked after the shared methods and before a child by name, so
			// the precedence is the one two comments in this file already
			// argue for: a part called `CurrentCamera` must not shadow the
			// property.
			if (instance == WorkspaceEntity(state)) {
				if (name == "CurrentCamera") {
					PushCurrentCamera(state);
					return 1;
				}

				lua_getfield(state, LUA_REGISTRYINDEX, "engine.workspace.methods");
				lua_getfield(state, -1, field);
				if (!lua_isnil(state, -1)) {
					lua_remove(state, -2);
					return 1;
				}
				lua_pop(state, 2);
			}

			// **And finally a child by name.** `workspace.Baseplate` is how a
			// Roblox script reaches one, and it is last rather than first
			// deliberately: a child named `Size` must not shadow the property,
			// or a scene could break every script that touched it by adding a
			// part with an unlucky name.
			const Entity child = store.FindFirstChild(instance, name);
			if (child != ecs::NULL_ENTITY) {
				PushInstance(state, child);
				return 1;
			}

			luaL_errorL(state, "'%s' is not a valid member of this instance", field);
		}

		int InstanceNewIndex(lua_State *state) {
			Store &store = InstanceStoreOf(state);
			const Entity instance = CheckInstance(state, 1);
			const char *field = luaL_checkstring(state, 2);

			// The other half of the Workspace's own members. Before the property
			// lookup rather than after it only because there is no property of
			// this name to find; the read path checks in the same order for the
			// same reason.
			if (std::string_view(field) == "CurrentCamera" && instance == WorkspaceEntity(state)) {
				SetCurrentCamera(state, 3);
				return 0;
			}

			const PropertyDescriptor *property = ScriptableProperty(store, instance, field);
			if (property == nullptr) {
				luaL_errorL(state, "'%s' is not a valid member of this instance", field);
			}
			if (!property->Writable) {
				luaL_errorL(state, "'%s' is read-only", field);
			}

			// The write half of the same exception - see the getter above.
			// `luaL_checklstring` rather than `luaL_checkstring`, so an embedded
			// zero in a string a script built survives instead of truncating the
			// value at it.
			if (property->Type == PropertyType::String) {
				size_t length = 0;
				const char *text = luaL_checklstring(state, 3, &length);
				const std::string value(text, length);

				if (!store.SetProperty(instance, *property, &value, sizeof(value))) {
					luaL_errorL(state, "could not write '%s'", field);
				}
				return 0;
			}

			alignas(16) unsigned char bytes[WIDEST_PROPERTY] = {};
			if (property->Size > sizeof(bytes) ||
				!ReadPropertyValue(state, 3, property->Type, property->EnumName, bytes)) {
				luaL_errorL(state, "'%s' cannot take that value", field);
			}

			// A refusal is an error rather than a silent no-op. A replica
			// rejecting the write is the case that matters: a script author
			// cannot tell "rejected" from "applied and then overwritten by the
			// next delta" without being told.
			if (!store.SetProperty(instance, *property, bytes, property->Size)) {
				if (store.AdoptOnly()) {
					luaL_errorL(
						state,
						"'%s' cannot be set here: this world is a replica, and the authority owns it. "
						"Test RunService:IsServer() first",
						field
					);
				}
				luaL_errorL(state, "could not set '%s'", field);
			}
			return 0;
		}

		int InstanceToString(lua_State *state) {
			Store &store = InstanceStoreOf(state);
			const Entity instance = CheckInstance(state, 1);
			lua_pushstring(state, store.InstanceNameOf(instance).Text().data());
			return 1;
		}

		// Two handles onto one entity are one instance. Without this,
		// `part.Parent.Parent == workspace` would be false for an object a
		// script had never seen before, because each read mints a new userdata.
		int InstanceEqual(lua_State *state) {
			lua_pushboolean(state, CheckInstance(state, 1) == CheckInstance(state, 2));
			return 1;
		}

		int InstanceNew(lua_State *state) {
			Store &store = InstanceStoreOf(state);
			const char *className = luaL_checkstring(state, 1);

			const ecs::ClassId id = ecs::Classes::Find(Name(className));
			if (!id.IsValid()) {
				luaL_errorL(state, "'%s' is not a registered class", className);
			}

			const Entity instance = store.CreateInstance(id, className);
			if (instance == ecs::NULL_ENTITY) {
				if (store.AdoptOnly()) {
					luaL_errorL(
						state,
						"cannot create a '%s' here: this world is a replica and may not mint entities",
						className
					);
				}
				luaL_errorL(state, "could not create a '%s'", className);
			}

			// **The second argument, which Roblox has and v0.5 did not.**
			// `Instance.new("Part", workspace)` is one call rather than two, and
			// the difference is not only brevity: a part created and parented in
			// one statement is never briefly an orphan, so nothing that walks
			// the tree can observe the half-built state.
			//
			// **Omitting it leaves the instance parented to nothing**, which is
			// Roblox's behaviour and now means what it says: the part exists, it
			// is fully formed, no draw list contains it and no walk of the tree
			// reaches it until somebody sets `.Parent`. That is what makes an
			// object usable as pure data - a template, a marker, a thing a
			// script holds and never shows.
			if (!lua_isnoneornil(state, 2)) {
				if (!store.SetParent(instance, CheckInstance(state, 2))) {
					luaL_errorL(state, "could not parent the new '%s'", className);
				}
			}

			PushInstance(state, instance);
			return 1;
		}
	}

	// --- attributes -----------------------------------------------------------
	//
	// **The four attribute methods are `ScriptMethods.cpp`'s now**, and the
	// marshalling that served them went with them: `LuauCall.cpp` reads a Luau
	// value into an `ecs::AttributeValue` and pushes one back, which is the same
	// conversion said in one language rather than the whole method. See
	// `ScriptCall.hpp` for why a method became data the way a property already
	// was.

	// --- marshalling, shared with the ECS surface ---------------------------
	//
	// **The type and the enum name, never a descriptor.** A property has one and
	// a component field does not, and both carry exactly the same values - so a
	// second switch for fields would be the duplicate `script/AGENTS.md` calls
	// the marshalling design's whole reason for existing.

	bool PushPropertyValue(lua_State *state, ecs::PropertyType type, core::Name enumName, const void *bytes) {
		using core::Name;
		using ecs::Entity;
		using ecs::PropertyType;

		switch (type) {
		case PropertyType::Bool:
			lua_pushboolean(state, *static_cast<const bool *>(bytes));
			return true;
		case PropertyType::Float:
			lua_pushnumber(state, *static_cast<const float *>(bytes));
			return true;
		case PropertyType::Double:
			lua_pushnumber(state, *static_cast<const double *>(bytes));
			return true;
		case PropertyType::Int32:
			lua_pushinteger(state, *static_cast<const int32_t *>(bytes));
			return true;
		case PropertyType::Int64:
			lua_pushnumber(state, static_cast<double>(*static_cast<const int64_t *>(bytes)));
			return true;
		case PropertyType::Name:
			// Text, never the interned id. A number that means a string in
			// one process and a different string in the next is the whole
			// hazard `core::Name` exists around.
			lua_pushstring(state, static_cast<const Name *>(bytes)->Text().data());
			return true;
		case PropertyType::String:
			// **Never reached, and refused rather than handled.** The caller
			// takes a `std::string` down its own path before it gets here,
			// because these `bytes` are uninitialised storage and a
			// `std::string` cannot be assigned into that. Returning false
			// makes a future caller that forgot the branch fail loudly
			// rather than corrupt a heap.
			return false;
		case PropertyType::Enum:
			// An `EnumItem`, not a string - that is the whole difference
			// this type buys. The value is an interned `Name` exactly as
			// `PropertyType::Name` is; what changes is that userland gets a
			// value it can compare against `Enum.AlphaMode.Transparency` and be
			// told when it is wrong.
			PushEnumItem(state, enumName, *static_cast<const Name *>(bytes));
			return true;
		case PropertyType::Vector3:
			*PushVector3(state) = *static_cast<const core::Vector3 *>(bytes);
			return true;
		case PropertyType::Color3:
			*PushColor3(state) = *static_cast<const core::Color3 *>(bytes);
			return true;
		case PropertyType::CFrame:
			*PushCFrame(state) = *static_cast<const core::CFrame *>(bytes);
			return true;
		case PropertyType::Vector2:
			*PushVector2(state) = *static_cast<const core::Vector2 *>(bytes);
			return true;
		case PropertyType::UDim:
			*PushUDim(state) = *static_cast<const core::UDim *>(bytes);
			return true;
		case PropertyType::UDim2:
			*PushUDim2(state) = *static_cast<const core::UDim2 *>(bytes);
			return true;
		case PropertyType::Rect:
			*PushRect(state) = *static_cast<const core::Rect *>(bytes);
			return true;
		case PropertyType::NumberRange:
			*PushNumberRange(state) = *static_cast<const core::NumberRange *>(bytes);
			return true;
		case PropertyType::NumberSequence:
			*PushNumberSequence(state) = *static_cast<const core::NumberSequence *>(bytes);
			return true;
		case PropertyType::ColorSequence:
			*PushColorSequence(state) = *static_cast<const core::ColorSequence *>(bytes);
			return true;
		case PropertyType::Reference: {
			// **Nil, and this is where "an orphan is not in the world"
			// begins.** This used to hand back `workspace` for a null
			// reference, because `workspace` *was* the world and a root
			// therefore belonged to it. Now `workspace` is an instance like
			// any other, `Workspace` is somewhere a thing can be parented,
			// and having no parent is an ordinary state a script can both
			// produce and read back - which is what makes
			// `Instance.new("Part")` an object nothing draws and nothing
			// lists until somebody says where it goes.
			const Entity referenced = *static_cast<const Entity *>(bytes);
			if (referenced == ecs::NULL_ENTITY) {
				lua_pushnil(state);
			} else {
				PushInstance(state, referenced);
			}
			return true;
		}
		case PropertyType::Opaque:
			break;
		}
		return false;
	}

	bool
	ReadPropertyValue(lua_State *state, int index, ecs::PropertyType type, core::Name enumName, void *out) {
		using core::Name;
		using ecs::Entity;
		using ecs::PropertyType;

		switch (type) {
		case PropertyType::Bool:
			*static_cast<bool *>(out) = lua_toboolean(state, index) != 0;
			return true;
		case PropertyType::Float:
			*static_cast<float *>(out) = static_cast<float>(luaL_checknumber(state, index));
			return true;
		case PropertyType::Double:
			*static_cast<double *>(out) = luaL_checknumber(state, index);
			return true;
		case PropertyType::Int32:
			*static_cast<int32_t *>(out) = luaL_checkinteger(state, index);
			return true;
		case PropertyType::Int64:
			*static_cast<int64_t *>(out) = static_cast<int64_t>(luaL_checknumber(state, index));
			return true;
		case PropertyType::Name:
			*static_cast<Name *>(out) = Name(luaL_checkstring(state, index));
			return true;
		case PropertyType::String:
			// Refused here for `PushValue`'s reason, and the caller's own
			// branch is what actually serves this type.
			return false;
		case PropertyType::Enum:
			// **A string is accepted as well as an `EnumItem`**, because
			// `part.AlphaMode = "Transparency"` is what Roblox accepts and what a
			// migrating script already contains. What is refused is a
			// member of the *wrong* enum, which is the error a bare string
			// could never have caught.
			return ReadEnumValue(state, index, enumName, *static_cast<Name *>(out));
		case PropertyType::Vector3:
			*static_cast<core::Vector3 *>(out) = CheckVector3(state, index);
			return true;
		case PropertyType::Color3:
			*static_cast<core::Color3 *>(out) = CheckColor3(state, index);
			return true;
		case PropertyType::CFrame:
			*static_cast<core::CFrame *>(out) = CheckCFrame(state, index);
			return true;
		case PropertyType::Vector2:
			*static_cast<core::Vector2 *>(out) = CheckVector2Value(state, index);
			return true;
		case PropertyType::UDim:
			*static_cast<core::UDim *>(out) = CheckUDim(state, index);
			return true;
		case PropertyType::UDim2:
			*static_cast<core::UDim2 *>(out) = CheckUDim2(state, index);
			return true;
		case PropertyType::Rect:
			*static_cast<core::Rect *>(out) = CheckRect(state, index);
			return true;
		case PropertyType::NumberRange:
			*static_cast<core::NumberRange *>(out) = CheckNumberRange(state, index);
			return true;

		// **Placement-new, where every case above assigns.** `out` is
		// uninitialised stack bytes, and assigning a 328-byte value over it is
		// harmless only because these types are trivially copyable - which
		// they are, and which the caller's zeroed buffer already relies on.
		// Written as a plain assignment for the same reason as the rest: one
		// shape, so a future non-trivial member is caught by the compiler here
		// rather than by a corrupt gradient.
		case PropertyType::NumberSequence:
			*static_cast<core::NumberSequence *>(out) = CheckNumberSequence(state, index);
			return true;
		case PropertyType::ColorSequence:
			*static_cast<core::ColorSequence *>(out) = CheckColorSequence(state, index);
			return true;
		case PropertyType::Reference:
			// `part.Parent = nil` detaches. `part.Parent = workspace` is now
			// an ordinary instance reference and needs no case of its own -
			// which is the whole of what collapsing the two notions of "the
			// workspace" bought.
			if (lua_isnil(state, index)) {
				*static_cast<Entity *>(out) = ecs::NULL_ENTITY;
				return true;
			}
			*static_cast<Entity *>(out) = CheckInstance(state, index);
			return true;
		case PropertyType::Opaque:
			break;
		}
		return false;
	}

	void PushInstanceValue(lua_State *state, ecs::Entity instance) {
		PushInstance(state, instance);
	}

	std::vector<std::string_view> LuauInstanceSignalNames() {
		return {std::begin(SIGNALS), std::end(SIGNALS)};
	}

	void OpenInstances(lua_State *state) {
		LuauContext &context = ContextOf(state);

		// The method table, in the registry so `__index` hands back one shared
		// closure per method rather than building one per access.
		//
		// **Empty when it is made, and that is the change v0.18 finished.** This
		// file used to build twenty `lua_CFunction`s into it and the neutral layer
		// appended to what was left; there is no Luau-only instance method any
		// more, so the table is created here and filled entirely by
		// `InstallLuauNeutralMethods` and by `LuauEcs`, which appends five of its
		// own.
		//
		// **The editor reads this same table back**, rather than a list of method
		// names kept beside it: `LuauRuntime::Vocabulary` walks this registry
		// entry, so a method added to `ScriptMethods.cpp` is offered in the script
		// editor with nothing else changing.
		lua_newtable(state);
		lua_setfield(state, LUA_REGISTRYINDEX, "engine.instance.methods");

		// One trampoline per row - see `ScriptCall.hpp`.
		InstallLuauNeutralMethods(state);

		luaL_newmetatable(state, "Instance");

		lua_pushlightuserdata(state, &context);
		lua_pushcclosure(state, InstanceIndex, "__index", 1);
		lua_setfield(state, -2, "__index");

		lua_pushlightuserdata(state, &context);
		lua_pushcclosure(state, InstanceNewIndex, "__newindex", 1);
		lua_setfield(state, -2, "__newindex");

		lua_pushlightuserdata(state, &context);
		lua_pushcclosure(state, InstanceToString, "__tostring", 1);
		lua_setfield(state, -2, "__tostring");

		lua_pushcfunction(state, InstanceEqual, "__eq");
		lua_setfield(state, -2, "__eq");

		// Hidden, for the reason the value types hide theirs: a metatable a
		// script can reach is one it can rewrite, and then every instance in
		// the world changes behaviour underneath everything holding one.
		lua_pushstring(state, "Instance");
		lua_setfield(state, -2, "__metatable");

		// What `typeof` reads - see `LuauValues.cpp`'s `Install`.
		lua_pushstring(state, "Instance");
		lua_setfield(state, -2, "__type");

		lua_pop(state, 1);

		lua_newtable(state);
		lua_pushlightuserdata(state, &context);
		lua_pushcclosure(state, InstanceNew, "new", 1);
		lua_setfield(state, -2, "new");
		lua_setglobal(state, "Instance");
	}

	std::string PumpChanges(lua_State *state) {
		LuauContext &context = ContextOf(state);
		if (context.Changes.Empty()) {
			return {};
		}

		std::string firstError;
		context.Changes.Drain([&](ecs::Entity instance, core::Name property) {
			// **Both signals, from one queue entry.** `.Changed` takes the
			// property's name and `GetPropertyChangedSignal` takes nothing,
			// which is Roblox's split and the reason the second exists at all -
			// a handler that only cares about one property should not be called
			// for every other one and made to filter.
			lua_pushstring(state, property.Text().data());
			const std::string changed = FireSignal(state, SignalKind::Changed, instance, 1);
			if (firstError.empty()) {
				firstError = changed;
			}

			LuauContext &live = ContextOf(state);
			live.Signals.Fire(SignalKind::PropertyChanged, instance, [&](const Connection &connection) {
				if (connection.Property != property) {
					return;
				}

				lua_getref(state, connection.Callback);
				if (lua_pcall(state, 0, 0, 0) != LUA_OK) {
					if (firstError.empty()) {
						const char *message = lua_tostring(state, -1);
						firstError = message != nullptr ? message : "a property listener failed";
					}
					lua_pop(state, 1);
				}
			});
		});
		return firstError;
	}

	std::string PumpTree(lua_State *state) {
		LuauContext &context = ContextOf(state);
		if (!context.World->TreeObserved()) {
			return {};
		}

		// **Taken, not read.** A handler may reparent something, and a swap
		// leaves the store's list empty before the first one runs - so the move
		// it makes belongs to the next delivery instead of being appended to
		// the list being walked.
		std::vector<ecs::TreeChange> changes;
		context.World->TakeTreeChanges(changes);
		if (changes.empty()) {
			return {};
		}

		std::string firstError;
		const auto note = [&](std::string message) {
			if (firstError.empty() && !message.empty()) {
				firstError = std::move(message);
			}
		};

		for (const ecs::TreeChange &change : changes) {
			// **The parent's signals first, then the subtree's.** Roblox orders
			// them the same way, and a handler that reads `Parent` from an
			// `AncestryChanged` should see the move already finished - which it
			// does here whatever the order, because the delivery is a whole
			// tick after the write.
			if (change.From != ecs::NULL_ENTITY) {
				PushInstance(state, change.Instance);
				note(FireSignal(state, SignalKind::ChildRemoved, change.From, 1));
			}

			if (change.To != ecs::NULL_ENTITY) {
				PushInstance(state, change.Instance);
				note(FireSignal(state, SignalKind::ChildAdded, change.To, 1));

				// **`PlayerAdded` is that same arrival, filtered.** Fired here
				// rather than anywhere else because a player joining *is* a
				// reparent - `scene::AddPlayer` puts the instance under the
				// service - so a separate recording would be a second place the
				// same fact lived, and the two would come apart the first time
				// something parented a player by hand.
				if (IsPlayerOfService(*context.World, change.To, change.Instance)) {
					PushInstance(state, change.Instance);
					note(FireSignal(state, SignalKind::PlayerAdded, change.To, 1));
				}

				// `DescendantAdded` is every ancestor's, not just the new
				// parent's - that is the whole difference between it and
				// `ChildAdded`. Walked upwards from the parent, which is a
				// handful of steps rather than a search.
				for (Entity above = change.To; above != ecs::NULL_ENTITY;
					 above = context.World->ParentOf(above)) {
					PushInstance(state, change.Instance);
					note(FireSignal(state, SignalKind::DescendantAdded, above, 1));
				}
			}

			// **The instance and everything under it**, because an ancestry
			// change is inherited: moving a model changes the ancestry of every
			// part in it, and a script watching a part has no way to know its
			// model moved otherwise.
			const auto ancestry = [&](Entity subject) {
				PushInstance(state, subject);
				PushInstance(state, context.World->ParentOf(subject));
				note(FireSignal(state, SignalKind::AncestryChanged, subject, 2));
			};

			ancestry(change.Instance);
			context.World->EachDescendant(change.Instance, ancestry);
		}

		return firstError;
	}

	std::string PumpChildWaiters(lua_State *state) {
		LuauContext &context = ContextOf(state);
		if (context.Waiters.Empty()) {
			return {};
		}

		std::vector<ChildWaiters::Resumption> ready;
		context.Waiters.Advance(*context.World, context.World->Time().Tick, ready);

		std::string firstError;
		for (const ChildWaiters::Resumption &resumption : ready) {
			const auto waiting = context.AwaitedChildren.find(resumption.Waiter);
			if (waiting == context.AwaitedChildren.end()) {
				continue;
			}

			lua_State *thread = waiting->second;
			context.AwaitedChildren.erase(waiting);

			const auto held = context.Threads.find(thread);
			if (held == context.Threads.end()) {
				continue;
			}
			const CallbackRef reference = held->second;
			context.Threads.erase(held);

			// One value, and nil for a wait that ran out - which is what Roblox's
			// timeout form answers and what `if child then` reads.
			if (resumption.Child == ecs::NULL_ENTITY) {
				lua_pushnil(thread);
			} else {
				PushInstanceValue(thread, resumption.Child);
			}

			// **A yield here is success rather than failure**, for the reason
			// `PumpDeliveries` gives: a resumed script may wait again - on
			// another child, on a bus reply, on `task.wait` - and reading an
			// error message off a thread that merely suspended is reading
			// whatever the yield left on its stack. The old reference goes either
			// way, because whatever it suspended on has registered a fresh one.
			const int status = lua_resume(thread, nullptr, 1);
			if (status != LUA_OK && status != LUA_YIELD && firstError.empty()) {
				const char *message = lua_tostring(thread, -1);
				firstError = message != nullptr ? message : "a resumed WaitForChild failed";

				if (const char *trace = lua_debugtrace(thread); trace != nullptr) {
					firstError += "\n";
					firstError += trace;
				}
			}

			lua_unref(state, reference);
		}

		return firstError;
	}

	std::string PumpCharacters(lua_State *state) {
		LuauContext &context = ContextOf(state);

		std::vector<scene::CharacterChange> changes;
		scene::TakeCharacterChanges(*context.World, changes);
		if (changes.empty()) {
			return {};
		}

		std::string firstError;
		for (const scene::CharacterChange &change : changes) {
			// **A model that has gone is not reported here**, and the two halves
			// are disjoint rather than one covering for the other: a body
			// destroyed already fired `CharacterRemoving` synchronously from
			// `OnDescendantRemoving` with the instance still readable, and a body
			// that arrived and went inside one tick is one no handler could act
			// on. What is left is the release that did not destroy -
			// `player.Character = nil` - which the hook cannot see.
			if (!context.World->Alive(change.Character)) {
				continue;
			}

			PushInstance(state, change.Character);
			const std::string failed = FireSignal(
				state,
				change.Added ? SignalKind::CharacterAdded : SignalKind::CharacterRemoving,
				change.Player,
				1
			);
			if (firstError.empty() && !failed.empty()) {
				firstError = failed;
			}
		}
		return firstError;
	}

	std::string PumpGuiEvents(lua_State *state, std::span<const gui::GuiEvent> events) {
		if (events.empty()) {
			return {};
		}

		LuauContext &context = ContextOf(state);

		std::string firstError;
		const auto note = [&](std::string message) {
			if (firstError.empty()) {
				firstError = std::move(message);
			}
		};

		for (const gui::GuiEvent &event : events) {
			// **An element may have been destroyed since the router named it.**
			// The events were produced from the previous frame's compiled list
			// and a handler earlier in *this* loop may have deleted what a later
			// one is about - which is the ordinary case for a close button, not
			// an edge one. Firing at a dead entity would push a userdata for a
			// row that is gone.
			if (!context.World->Alive(event.Instance)) {
				continue;
			}

			switch (event.Kind) {
			case gui::EventKind::MouseEnter:
			case gui::EventKind::MouseLeave:
			case gui::EventKind::MouseMoved: {
				// `(x, y)` in canvas pixels, which is Roblox's signature.
				const SignalKind kind = event.Kind == gui::EventKind::MouseEnter ? SignalKind::GuiMouseEnter
										: event.Kind == gui::EventKind::MouseLeave
											? SignalKind::GuiMouseLeave
											: SignalKind::GuiMouseMoved;
				lua_pushnumber(state, event.Position.X);
				lua_pushnumber(state, event.Position.Y);
				note(FireSignal(state, kind, event.Instance, 2));
				break;
			}

			case gui::EventKind::InputBegan:
				note(FireSignal(state, SignalKind::GuiInputBegan, event.Instance, 0));
				break;

			case gui::EventKind::InputEnded:
				note(FireSignal(state, SignalKind::GuiInputEnded, event.Instance, 0));
				break;

			case gui::EventKind::Activated:
				note(FireSignal(state, SignalKind::GuiActivated, event.Instance, 0));
				break;

			case gui::EventKind::Focused:
				note(FireSignal(state, SignalKind::GuiFocused, event.Instance, 0));
				break;

			case gui::EventKind::DragBegan:
			case gui::EventKind::DragContinue:
			case gui::EventKind::DragEnded: {
				// **`(x, y)` of the pointer, then how far it has come.** Roblox
				// hands a drag handler an input object; this engine has no such
				// type, and four numbers is what a handler actually reads off
				// one - where the pointer is and what the gesture has amounted
				// to. `gui::EventKind::DragBegan` says the same at the source.
				const SignalKind kind = event.Kind == gui::EventKind::DragBegan ? SignalKind::GuiDragBegan
										: event.Kind == gui::EventKind::DragContinue
											? SignalKind::GuiDragContinue
											: SignalKind::GuiDragEnded;
				lua_pushnumber(state, event.Position.X);
				lua_pushnumber(state, event.Position.Y);
				lua_pushnumber(state, event.Local.X);
				lua_pushnumber(state, event.Local.Y);
				note(FireSignal(state, kind, event.Instance, 4));
				break;
			}

			case gui::EventKind::FocusReleased:
				// **`enterPressed`, read off the event rather than assumed.**
				// Roblox's first argument, which a handler written
				// `function(enterPressed)` reads - see `SignalKind::GuiFocusLost`
				// for why the second argument it also declares is not here. True
				// when Return released the box and false when a press elsewhere
				// did, which is the difference `GuiEvent::Entered` carries.
				lua_pushboolean(state, event.Entered ? 1 : 0);
				note(FireSignal(state, SignalKind::GuiFocusLost, event.Instance, 1));
				break;
			}
		}

		return firstError;
	}

	void OpenWorkspace(lua_State *state, ecs::Store &store) {
		// **Idempotent, and called here so that a world always has one.** A
		// world read out of a game file brings its own services; a world built
		// by hand or by a test has none, and a `workspace` global that resolved
		// to nothing would make every script in the engine fail at its first
		// `.Parent`. `InstallServices` finds before it creates, so this is a
		// lookup on the common path.
		//
		// It hands back the `Workspace` because that is what callers want next,
		// which is exactly this call site.
		// **A replica is not asked, because asking is what logs.** Its fixtures
		// arrive from the authority, and `Store::MayMintAuthoritative` reports a
		// refused mint at error level once per store - so a client that opened a
		// VM over its replica said "refusing CreateInstance" on every join, which
		// reads as a fault and is the ordinary state of a world that owns
		// nothing. Whatever the authority has already sent is what this resolves
		// to, and a null one is a world whose first snapshot has not landed:
		// `workspace` then finds no children rather than crashing.
		Entity workspace = store.AdoptOnly() ? scene::WorkspaceOf(store) : scene::InstallServices(store);
		if (workspace == ecs::NULL_ENTITY) {
			workspace = scene::WorkspaceOf(store);
		}

		PushInstance(state, workspace);

		// Kept in the registry as well as in a global, so `game.Workspace`,
		// the `Parent` getter and the member lookup all hand back *the same*
		// instance rather than three userdata that merely compare equal.
		lua_pushvalue(state, -1);
		lua_setfield(state, LUA_REGISTRYINDEX, "engine.workspace");
		lua_setglobal(state, "workspace");

		// The table `InstanceIndex` consults for the two members only the
		// Workspace has. Created empty here and filled by `OpenQueries`, which
		// runs after this - `LuauBindings.hpp` states that ordering, and it is why
		// the table has to exist by the time this returns.
		lua_newtable(state);
		lua_setfield(state, LUA_REGISTRYINDEX, "engine.workspace.methods");
	}
	ecs::Entity CheckInstanceArgument(lua_State *state, int index) {
		return CheckInstance(state, index);
	}

}
