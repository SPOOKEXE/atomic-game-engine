#include "Bindings.hpp"
#include "Subtree.hpp"

#include <engine/core/Log.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Tagging.hpp>

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
		// store — the mistake a file-static would make available.
		Store &StoreOf(lua_State *state) {
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

		// How wide a property value can be, and therefore how big the shared
		// buffers below are.
		//
		// **This was `sizeof(core::CFrame)` until v0.10 added the sequences.** A
		// `core::ColorSequence` is twenty keypoints and does not fit in
		// twenty-eight bytes, and the guard is `property.Size > sizeof(bytes)` —
		// so an emitter's `Color` would have failed the read with "could not
		// read 'Color'", naming a property that is declared and readable and
		// whose only problem was a buffer one file away.
		//
		// A named constant rather than a `sizeof` at each buffer, because there
		// are two and the getter's being narrower than the setter's is a bug with
		// no symptom on the setter.
		constexpr size_t WIDEST_PROPERTY =
			std::max(sizeof(core::ColorSequence), sizeof(core::NumberSequence));

		// Pushes a property's value, having read it into a buffer of its size.
		bool PushValue(lua_State *state, const PropertyDescriptor &property, const void *bytes) {
			switch (property.Type) {
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
				// An `EnumItem`, not a string — that is the whole difference
				// this type buys. The value is an interned `Name` exactly as
				// `PropertyType::Name` is; what changes is that userland gets a
				// value it can compare against `Enum.AlphaMode.Clip` and be
				// told when it is wrong.
				PushEnumItem(state, property.EnumName, *static_cast<const Name *>(bytes));
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
				// produce and read back — which is what makes
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

		// Reads a Luau value into a buffer of the property's size.
		bool ReadValue(lua_State *state, int index, const PropertyDescriptor &property, void *out) {
			switch (property.Type) {
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
				// `part.AlphaMode = "Clip"` is what Roblox accepts and what a
				// migrating script already contains. What is refused is a
				// member of the *wrong* enum, which is the error a bare string
				// could never have caught.
				return ReadEnumValue(state, index, property.EnumName, *static_cast<Name *>(out));
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
			// harmless only because these types are trivially copyable — which
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
				// an ordinary instance reference and needs no case of its own —
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

		// Compare the stored spelling directly. Interning each lookup takes the
		// process-wide registry lock and adds a hash lookup to every property access.
		const PropertyDescriptor *Find(const Store &store, Entity instance, std::string_view name) {
			for (const PropertyDescriptor &property : store.PropertiesOf(instance)) {
				if (property.Spelling == name) {
					return &property;
				}
			}
			return nullptr;
		}

		// --- methods ---------------------------------------------------------
		//
		// Every one of these is a call `Store` already had and a script could
		// not spell. Nothing new happens in the storage; what is new is that
		// `part:Destroy()` reaches `Store::DestroyInstance` instead of being a
		// missing member.

		// `instance:IsA(className)`
		int InstanceIsA(lua_State *state) {
			Store &store = StoreOf(state);
			const Entity instance = CheckInstance(state, 1);
			const char *className = luaL_checkstring(state, 2);

			const ecs::ClassId wanted = ecs::Classes::Find(Name(className));
			if (!wanted.IsValid()) {
				// False rather than an error, matching Roblox. A script testing
				// for a class this game does not register is asking a question
				// with a correct answer, and it is "no".
				lua_pushboolean(state, false);
				return 1;
			}

			lua_pushboolean(state, ecs::Classes::IsA(store.ClassOf(instance), wanted));
			return 1;
		}

		// `pvInstance:GetPivot()` and `pvInstance:PivotTo(cframe)`
		//
		// **Roblox's pivot pair, and the whole reason it is a pair.** A
		// `Transform` says where the *centre* of something is; almost nothing an
		// author places is placed by its centre — a door turns on its hinge, a
		// lid sits on its rim, a character stands on the ground under its feet.
		// `PivotOffset` is where the handle is and these two are how it is used.
		//
		// **Methods rather than a `Pivot` property**, which is Roblox's shape and
		// is right for a reason of its own: `GetPivot` is *derived* from two
		// fields and `PivotTo` writes a third thing entirely, so a read-write
		// property would look like storage and behave like a computation.
		//
		// **Not refused for a non-`PVInstance`**, matching every other method
		// here: `scene::PivotOf` answers the identity for something with no
		// placement, which is what a script asking a `Folder` for its pivot
		// should get rather than an error mid-frame.
		int InstanceGetPivot(lua_State *state) {
			Store &store = StoreOf(state);
			const Entity instance = CheckInstance(state, 1);
			*PushCFrame(state) = scene::PivotOf(store, instance);
			return 1;
		}

		int InstancePivotTo(lua_State *state) {
			Store &store = StoreOf(state);
			const Entity instance = CheckInstance(state, 1);
			const core::CFrame &target = CheckCFrame(state, 2);

			// The return is dropped on purpose: Roblox's `PivotTo` returns
			// nothing, and an instance with no placement to move is the same
			// "did nothing" a `Folder` would be.
			(void)scene::PivotTo(store, instance, target);
			return 0;
		}

		// `instance:AddTag(name)`, `instance:RemoveTag(name)`, `instance:HasTag(name)`
		//
		// **Roblox puts these on `CollectionService` and they are methods here**,
		// which is the one place this binding departs from that vocabulary
		// deliberately. A service would need a world to be found through, and
		// the thing being tagged is already in hand; `scene::AddTag` takes the
		// store and the entity and there is nothing a service would add but a
		// lookup.
		//
		// `AddTag` answers `false` when the world's tag table is full — see
		// `TagTable::MAXIMUM` — rather than erroring, because a scene that has
		// run out of tags is a scene mistake and not a script one, and a script
		// that wanted to know can read the answer.
		int InstanceAddTag(lua_State *state) {
			Store &store = StoreOf(state);
			const Entity instance = CheckInstance(state, 1);
			lua_pushboolean(state, scene::AddTag(store, instance, Name(luaL_checkstring(state, 2))));
			return 1;
		}

		int InstanceRemoveTag(lua_State *state) {
			Store &store = StoreOf(state);
			const Entity instance = CheckInstance(state, 1);
			lua_pushboolean(state, scene::RemoveTag(store, instance, Name(luaL_checkstring(state, 2))));
			return 1;
		}

		int InstanceHasTag(lua_State *state) {
			Store &store = StoreOf(state);
			const Entity instance = CheckInstance(state, 1);
			lua_pushboolean(state, scene::HasTag(store, instance, Name(luaL_checkstring(state, 2))));
			return 1;
		}

		// `instance:Destroy()`
		int InstanceDestroy(lua_State *state) {
			LuauContext &context = UpvalueContext(state);
			const Entity instance = CheckInstance(state, 1);

			// **The signal table is told before the storage is**, and it is told
			// about the whole subtree. `DestroyInstance` takes every descendant,
			// so a connection anywhere under here would otherwise outlive the row
			// it watched: the ref is never given up, so the closure and
			// everything it captured stay alive for the rest of the world's life.
			ForgetSubtree(
				*context.World, context.Signals, context.Changes, instance, [state](CallbackRef reference) {
					lua_unref(state, reference);
				}
			);

			context.World->DestroyInstance(instance);
			return 0;
		}

		// `instance:Clone()`
		int InstanceClone(lua_State *state) {
			Store &store = StoreOf(state);
			const Entity instance = CheckInstance(state, 1);

			const Entity copy = store.CloneInstance(instance);
			if (copy == ecs::NULL_ENTITY) {
				// Nil rather than an error, matching Roblox: a clone of
				// something unclonable is nil, and a script can test for it.
				lua_pushnil(state);
				return 1;
			}

			PushInstance(state, copy);
			return 1;
		}

		// `instance:GetChildren()`
		int InstanceGetChildren(lua_State *state) {
			Store &store = StoreOf(state);
			const Entity instance = CheckInstance(state, 1);

			lua_newtable(state);
			int index = 0;
			store.EachChild(instance, [&](Entity child) {
				PushInstance(state, child);
				lua_rawseti(state, -2, ++index);
			});
			return 1;
		}

		// `instance:GetDescendants()`
		//
		// Depth first, children before grandchildren, which is Roblox's order
		// and the one a script writing a recursive walk by hand would produce.
		int InstanceGetDescendants(lua_State *state) {
			Store &store = StoreOf(state);
			const Entity instance = CheckInstance(state, 1);

			lua_newtable(state);
			int index = 0;

			EachDescendant(store, instance, [&](Entity descendant) {
				PushInstance(state, descendant);
				lua_rawseti(state, -2, ++index);
			});
			return 1;
		}

		// Pushes an instance, or nil for the null handle.
		//
		// Every lookup below ends the same way, and writing it out six times is
		// six chances to push a userdata wrapping `NULL_ENTITY` — which is not
		// nil, compares equal to nothing, and is the shape a script cannot test
		// for.
		int PushFound(lua_State *state, Entity found) {
			if (found == ecs::NULL_ENTITY) {
				lua_pushnil(state);
				return 1;
			}

			PushInstance(state, found);
			return 1;
		}

		// The class named by an argument, or an invalid id.
		ecs::ClassId CheckClass(lua_State *state, int index) {
			return ecs::Classes::Find(core::Name(luaL_checkstring(state, index)));
		}

		// `instance:FindFirstChild(name, recursive)`
		int InstanceFindFirstChild(lua_State *state) {
			Store &store = StoreOf(state);
			const Entity instance = CheckInstance(state, 1);
			const char *name = luaL_checkstring(state, 2);

			// **The second argument, which used to be read and ignored.** A
			// script calling `FindFirstChild("Humanoid", true)` got the
			// non-recursive answer — nil for anything not a direct child — and
			// nothing said so. Silently answering a different question than the
			// one asked is the worst kind of gap in a binding.
			const bool recursive = lua_toboolean(state, 3) != 0;

			return PushFound(state, store.FindFirstChild(instance, name, recursive));
		}

		// `instance:FindFirstChildOfClass(className)`
		int InstanceFindFirstChildOfClass(lua_State *state) {
			Store &store = StoreOf(state);
			const Entity instance = CheckInstance(state, 1);

			return PushFound(state, store.FindFirstChildOfClass(instance, CheckClass(state, 2)));
		}

		// `instance:FindFirstChildWhichIsA(className, recursive)`
		int InstanceFindFirstChildWhichIsA(lua_State *state) {
			Store &store = StoreOf(state);
			const Entity instance = CheckInstance(state, 1);
			const ecs::ClassId klass = CheckClass(state, 2);
			const bool recursive = lua_toboolean(state, 3) != 0;

			return PushFound(state, store.FindFirstChildWhichIsA(instance, klass, recursive));
		}

		// `instance:FindFirstAncestor(name)`
		int InstanceFindFirstAncestor(lua_State *state) {
			Store &store = StoreOf(state);
			const Entity instance = CheckInstance(state, 1);

			return PushFound(state, store.FindFirstAncestor(instance, luaL_checkstring(state, 2)));
		}

		// `instance:FindFirstAncestorOfClass(className)`
		int InstanceFindFirstAncestorOfClass(lua_State *state) {
			Store &store = StoreOf(state);
			const Entity instance = CheckInstance(state, 1);

			return PushFound(state, store.FindFirstAncestorOfClass(instance, CheckClass(state, 2)));
		}

		// `instance:FindFirstAncestorWhichIsA(className)`
		int InstanceFindFirstAncestorWhichIsA(lua_State *state) {
			Store &store = StoreOf(state);
			const Entity instance = CheckInstance(state, 1);

			return PushFound(state, store.FindFirstAncestorWhichIsA(instance, CheckClass(state, 2)));
		}

		// `instance:GetFullName()`
		int InstanceGetFullName(lua_State *state) {
			Store &store = StoreOf(state);
			const Entity instance = CheckInstance(state, 1);

			const std::string full = store.GetFullName(instance);
			lua_pushlstring(state, full.data(), full.size());
			return 1;
		}

		// `instance:IsDescendantOf(ancestor)`
		int InstanceIsDescendantOf(lua_State *state) {
			Store &store = StoreOf(state);
			const Entity instance = CheckInstance(state, 1);

			// No case for the workspace any more, and losing it is the point:
			// `part:IsDescendantOf(workspace)` used to be true for every live
			// instance in the world, because the world was every root's
			// ancestor. It is now the real subtree question — the same one the
			// renderer asks — so a script and the render gate cannot disagree
			// about whether something is in the scene.
			lua_pushboolean(state, store.IsDescendantOf(instance, CheckInstance(state, 2)));
			return 1;
		}

		// `instance:ClearAllChildren()`
		int InstanceClearAllChildren(lua_State *state) {
			LuauContext &context = UpvalueContext(state);
			const Entity instance = CheckInstance(state, 1);

			// Collected first. `DestroyInstance` unlinks from the sibling list
			// the walk is standing in, so destroying inside `EachChild` would
			// visit whatever moved into the slot — or nothing.
			std::vector<Entity> children;
			context.World->EachChild(instance, [&](Entity child) { children.push_back(child); });

			for (const Entity child : children) {
				// The child's whole subtree, because that is what destroying it
				// takes — forgetting only the child leaves every grandchild's
				// connections pointing at rows that no longer exist.
				ForgetSubtree(
					*context.World, context.Signals, context.Changes, child, [state](CallbackRef reference) {
						lua_unref(state, reference);
					}
				);
				context.World->DestroyInstance(child);
			}
			return 0;
		}

		// `instance:GetPropertyChangedSignal(name)`
		int InstanceGetPropertyChangedSignal(lua_State *state) {
			Store &store = StoreOf(state);
			const Entity instance = CheckInstance(state, 1);
			const char *field = luaL_checkstring(state, 2);

			// Refused for a property that does not exist, which is the one place
			// a typo in a signal name can still be caught. A signal that
			// silently never fired would be indistinguishable from a value that
			// never changed.
			if (Find(store, instance, field) == nullptr) {
				luaL_errorL(state, "'%s' is not a valid member of this instance", field);
			}

			PushSignal(state, SignalKind::PropertyChanged, instance, Name(field));
			return 1;
		}

		// The method table, built once and shared by every instance.
		//
		// On the metatable rather than on each userdata, for the reason the
		// JavaScript side puts accessors on a prototype: a scene of five hundred
		// parts would otherwise carry five hundred copies of one closure.
		void PushMethods(lua_State *state) {
			lua_getfield(state, LUA_REGISTRYINDEX, "engine.instance.methods");
		}

		// --- the metatable ---------------------------------------------------

		int InstanceIndex(lua_State *state) {
			Store &store = StoreOf(state);
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

			// The 2D tree's input, in the same place and for the same reason:
			// none of these projects onto a component either.
			//
			// **Offered on every instance rather than only on a `GuiObject`.**
			// Roblox does gate them by class, and matching that would mean a
			// class test on a lookup that already runs for every field access on
			// every instance — to produce, in the failing case, a connection
			// that never fires instead of an error. `gui::Router` only ever
			// names elements it found in a compiled draw list, so a connection
			// on a `Part` is inert by construction, which is the same answer at
			// none of the cost.
			if (name == "Activated") {
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

			const PropertyDescriptor *property = Find(store, instance, field);
			if (property != nullptr) {
				// **The one type that cannot ride the shared byte buffer**, and
				// it is worth saying why rather than leaving it to look like an
				// inconsistency. Every other property is trivially copyable, so
				// a `descriptor.Get` into raw bytes is a copy into storage that
				// needed no construction. A `PropertyType::String` value owns an
				// allocation and its getter *assigns* — assigning into
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
				// second time, for a descriptor that is in scope — which is
				// half the class-table traffic of a scripted frame.
				alignas(16) unsigned char bytes[WIDEST_PROPERTY] = {};
				if (property->Size > sizeof(bytes) ||
					!store.GetProperty(instance, *property, bytes, property->Size)) {
					luaL_errorL(state, "could not read '%s'", field);
				}

				if (!PushValue(state, *property, bytes)) {
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
			// this and they stay together now that the world object is gone —
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
			Store &store = StoreOf(state);
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

			const PropertyDescriptor *property = Find(store, instance, field);
			if (property == nullptr) {
				luaL_errorL(state, "'%s' is not a valid member of this instance", field);
			}
			if (!property->Writable) {
				luaL_errorL(state, "'%s' is read-only", field);
			}

			// The write half of the same exception — see the getter above.
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
			if (property->Size > sizeof(bytes) || !ReadValue(state, 3, *property, bytes)) {
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
			Store &store = StoreOf(state);
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
			Store &store = StoreOf(state);
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
			// object usable as pure data — a template, a marker, a thing a
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
	// **The same marshalling as a property and deliberately so.** An attribute
	// and a property are one question — what can userland hold — asked at run time
	// and at declaration time, so `PushValue` and `ReadValue` above serve both.
	// Two marshallers would be two places to add a type to and two to forget.
	//
	// What differs is that an attribute has no descriptor, so the *type* has to
	// come from the Luau value itself on the way in and from the stored value on
	// the way out.

	namespace {
		// Turns a Luau value into an attribute, by what it is rather than by what
		// a descriptor said it should be.
		//
		// **Userdata is checked by tag, in the order the vocabulary was added.**
		// A tag check is exact — `Vector2` and `UDim` are both two floats and only
		// the tag tells them apart — so the order here is legibility and not
		// correctness.
		//
		// Returns `Opaque` for anything with no attribute form, which is what the
		// caller turns into a refusal. `nil` is the one exception and is handled
		// by the caller before this: it means *remove*.
		ecs::AttributeValue ReadAttribute(lua_State *state, int index) {
			ecs::AttributeValue value;

			if (lua_isboolean(state, index)) {
				value.Type = ecs::PropertyType::Bool;
				value.Bool = lua_toboolean(state, index) != 0;
				return value;
			}
			if (lua_isnumber(state, index)) {
				// **A double and not an int, even for a whole number.** Luau has
				// one number type; guessing at an integer here would make
				// `SetAttribute("n", 1)` read back as an `Int32` and
				// `SetAttribute("n", 1.5)` as a `Double`, so a script that
				// incremented an attribute would change its own type halfway.
				value.Type = ecs::PropertyType::Double;
				value.Double = lua_tonumber(state, index);
				return value;
			}
			if (lua_isstring(state, index)) {
				// **`String` and never `Name`.** An attribute's value is something
				// a game computes — a title, a state, a message — and `core::Name`
				// is a registry that never releases. `ecs::PropertyType::String`
				// carries the whole argument, and this is the surface it was added
				// for.
				size_t length = 0;
				const char *text = lua_tolstring(state, index, &length);
				value.Type = ecs::PropertyType::String;
				value.String.assign(text, length);
				return value;
			}

			if (lua_touserdatatagged(state, index, TAG_VECTOR3) != nullptr) {
				value.Type = ecs::PropertyType::Vector3;
				value.Vector3 = CheckVector3(state, index);
			} else if (lua_touserdatatagged(state, index, TAG_COLOR3) != nullptr) {
				value.Type = ecs::PropertyType::Color3;
				value.Color3 = CheckColor3(state, index);
			} else if (lua_touserdatatagged(state, index, TAG_CFRAME) != nullptr) {
				value.Type = ecs::PropertyType::CFrame;
				value.CFrame = CheckCFrame(state, index);
			} else if (lua_touserdatatagged(state, index, TAG_VECTOR2) != nullptr) {
				value.Type = ecs::PropertyType::Vector2;
				value.Vector2 = CheckVector2Value(state, index);
			} else if (lua_touserdatatagged(state, index, TAG_UDIM) != nullptr) {
				value.Type = ecs::PropertyType::UDim;
				value.UDim = CheckUDim(state, index);
			} else if (lua_touserdatatagged(state, index, TAG_UDIM2) != nullptr) {
				value.Type = ecs::PropertyType::UDim2;
				value.UDim2 = CheckUDim2(state, index);
			} else if (lua_touserdatatagged(state, index, TAG_RECT) != nullptr) {
				value.Type = ecs::PropertyType::Rect;
				value.Rect = CheckRect(state, index);
			} else if (lua_touserdatatagged(state, index, TAG_NUMBER_RANGE) != nullptr) {
				value.Type = ecs::PropertyType::NumberRange;
				value.NumberRange = CheckNumberRange(state, index);
			} else if (lua_touserdatatagged(state, index, TAG_NUMBER_SEQUENCE) != nullptr) {
				value.Type = ecs::PropertyType::NumberSequence;
				value.NumberSequence = CheckNumberSequence(state, index);
			} else if (lua_touserdatatagged(state, index, TAG_COLOR_SEQUENCE) != nullptr) {
				value.Type = ecs::PropertyType::ColorSequence;
				value.ColorSequence = CheckColorSequence(state, index);
			}

			return value;
		}

		// Pushes a stored attribute.
		//
		// Returns `false` for a type with no Luau form, which cannot happen for a
		// value this binding stored and can for one a future build wrote — so it
		// is a refusal rather than an assert.
		bool PushAttribute(lua_State *state, const ecs::AttributeValue &value) {
			switch (value.Type) {
			case ecs::PropertyType::Bool:
				lua_pushboolean(state, value.Bool);
				return true;
			case ecs::PropertyType::Int32:
				lua_pushinteger(state, value.Int32);
				return true;
			case ecs::PropertyType::Int64:
				lua_pushnumber(state, static_cast<double>(value.Int64));
				return true;
			case ecs::PropertyType::Float:
				lua_pushnumber(state, value.Float);
				return true;
			case ecs::PropertyType::Double:
				lua_pushnumber(state, value.Double);
				return true;
			case ecs::PropertyType::Name:
			case ecs::PropertyType::Enum:
				lua_pushstring(state, value.Name.Text().data());
				return true;
			case ecs::PropertyType::String:
				lua_pushlstring(state, value.String.data(), value.String.size());
				return true;
			case ecs::PropertyType::Vector3:
				*PushVector3(state) = value.Vector3;
				return true;
			case ecs::PropertyType::Color3:
				*PushColor3(state) = value.Color3;
				return true;
			case ecs::PropertyType::CFrame:
				*PushCFrame(state) = value.CFrame;
				return true;
			case ecs::PropertyType::Vector2:
				*PushVector2(state) = value.Vector2;
				return true;
			case ecs::PropertyType::UDim:
				*PushUDim(state) = value.UDim;
				return true;
			case ecs::PropertyType::UDim2:
				*PushUDim2(state) = value.UDim2;
				return true;
			case ecs::PropertyType::Rect:
				*PushRect(state) = value.Rect;
				return true;
			case ecs::PropertyType::NumberRange:
				*PushNumberRange(state) = value.NumberRange;
				return true;
			case ecs::PropertyType::NumberSequence:
				*PushNumberSequence(state) = value.NumberSequence;
				return true;
			case ecs::PropertyType::ColorSequence:
				*PushColorSequence(state) = value.ColorSequence;
				return true;
			case ecs::PropertyType::Reference:
			case ecs::PropertyType::Opaque:
				break;
			}
			return false;
		}

		int InstanceGetAttribute(lua_State *state) {
			Store &store = StoreOf(state);
			const Entity instance = CheckInstance(state, 1);
			const Name name(luaL_checkstring(state, 2));

			ecs::AttributeValue value;
			if (!ecs::GetAttribute(store, instance, name, value) || !PushAttribute(state, value)) {
				// **Nil for an attribute nobody set**, which is Roblox's answer
				// and is the only one a script can act on: an error would make
				// `if part:GetAttribute("Health") then` a crash rather than a
				// test.
				lua_pushnil(state);
			}
			return 1;
		}

		int InstanceSetAttribute(lua_State *state) {
			Store &store = StoreOf(state);
			const Entity instance = CheckInstance(state, 1);
			const Name name(luaL_checkstring(state, 2));

			ecs::AttributeValue value;
			if (!lua_isnoneornil(state, 3)) {
				value = ReadAttribute(state, 3);
				if (value.Type == ecs::PropertyType::Opaque) {
					luaL_errorL(
						state, "SetAttribute: '%s' cannot hold that type", std::string(name.Text()).c_str()
					);
				}
			}

			// An `Opaque` value removes, which is what `nil` fell through to
			// above. `ecs::SetAttribute` carries the argument for why removal is
			// not a method of its own.
			if (!ecs::SetAttribute(store, instance, name, value)) {
				luaL_errorL(state, "could not set attribute '%s'", std::string(name.Text()).c_str());
			}

			// **Queued rather than fired**, so an attribute signals on the same
			// barrier a property does and with the same dedup —
			// `ChangeQueue::Record` carries the argument. `PumpChanges` is what
			// turns this into `.Changed` and into whatever
			// `GetAttributeChangedSignal` connected.
			UpvalueContext(state).Changes.Record(instance, name);
			return 0;
		}

		// `instance:GetAttributes()` — every attribute, as a table.
		//
		// Roblox's name and Roblox's shape: a map from name to value rather than
		// an array, because that is what a caller iterates with `pairs`.
		int InstanceGetAttributes(lua_State *state) {
			Store &store = StoreOf(state);
			const Entity instance = CheckInstance(state, 1);

			lua_newtable(state);
			for (const Name &name : ecs::AttributeNames(store, instance)) {
				ecs::AttributeValue value;
				if (!ecs::GetAttribute(store, instance, name, value)) {
					continue;
				}
				if (!PushAttribute(state, value)) {
					continue;
				}
				lua_setfield(state, -2, name.Text().data());
			}
			return 1;
		}

		// `instance:GetAttributeChangedSignal(name)`.
		int InstanceGetAttributeChangedSignal(lua_State *state) {
			const Entity instance = CheckInstance(state, 1);
			const Name name(luaL_checkstring(state, 2));

			// **The property-changed signal, keyed by the attribute's name.**
			// `SignalKind::PropertyChanged` already filters by a `core::Name`, and
			// an attribute name and a property name live in the same registry — so
			// a second signal kind would be a second table to fan out from for a
			// filter that already exists.
			//
			// The cost is that an attribute sharing a name with a property fires
			// both listeners. That is a collision an author can see and avoid, and
			// the alternative is a parallel signal path for a case nobody has hit.
			PushSignal(state, SignalKind::PropertyChanged, instance, name);
			return 1;
		}
	}

	void PushInstanceValue(lua_State *state, ecs::Entity instance) {
		PushInstance(state, instance);
	}

	void OpenInstances(lua_State *state) {
		LuauContext &context = ContextOf(state);

		// The method table, in the registry so `__index` hands back one shared
		// closure per method rather than building one per access.
		static const struct {
			const char *Name;
			lua_CFunction Function;
		} METHODS[] = {
			{"IsA", InstanceIsA},
			{"GetPivot", InstanceGetPivot},
			{"PivotTo", InstancePivotTo},
			{"AddTag", InstanceAddTag},
			{"RemoveTag", InstanceRemoveTag},
			{"HasTag", InstanceHasTag},
			{"Destroy", InstanceDestroy},
			{"Clone", InstanceClone},
			{"GetChildren", InstanceGetChildren},
			{"GetDescendants", InstanceGetDescendants},
			{"FindFirstChild", InstanceFindFirstChild},
			{"FindFirstChildOfClass", InstanceFindFirstChildOfClass},
			{"FindFirstChildWhichIsA", InstanceFindFirstChildWhichIsA},
			{"FindFirstAncestor", InstanceFindFirstAncestor},
			{"FindFirstAncestorOfClass", InstanceFindFirstAncestorOfClass},
			{"FindFirstAncestorWhichIsA", InstanceFindFirstAncestorWhichIsA},
			{"GetFullName", InstanceGetFullName},
			{"IsDescendantOf", InstanceIsDescendantOf},
			{"ClearAllChildren", InstanceClearAllChildren},
			{"GetPropertyChangedSignal", InstanceGetPropertyChangedSignal},
			{"GetAttribute", InstanceGetAttribute},
			{"SetAttribute", InstanceSetAttribute},
			{"GetAttributes", InstanceGetAttributes},
			{"GetAttributeChangedSignal", InstanceGetAttributeChangedSignal},
		};

		lua_newtable(state);
		for (const auto &method : METHODS) {
			lua_pushlightuserdata(state, &context);
			lua_pushcclosure(state, method.Function, method.Name, 1);
			lua_setfield(state, -2, method.Name);
		}
		lua_setfield(state, LUA_REGISTRYINDEX, "engine.instance.methods");

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

		// What `typeof` reads — see `Values.cpp`'s `Install`.
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
			// which is Roblox's split and the reason the second exists at all —
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
		// leaves the store's list empty before the first one runs — so the move
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
			// `AncestryChanged` should see the move already finished — which it
			// does here whatever the order, because the delivery is a whole
			// tick after the write.
			if (change.From != ecs::NULL_ENTITY) {
				PushInstance(state, change.Instance);
				note(FireSignal(state, SignalKind::ChildRemoved, change.From, 1));
			}

			if (change.To != ecs::NULL_ENTITY) {
				PushInstance(state, change.Instance);
				note(FireSignal(state, SignalKind::ChildAdded, change.To, 1));

				// `DescendantAdded` is every ancestor's, not just the new
				// parent's — that is the whole difference between it and
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
			// one is about — which is the ordinary case for a close button, not
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
		Entity workspace = scene::InstallServices(store);
		if (workspace == ecs::NULL_ENTITY) {
			// A replica may not mint entities, so the fixtures arrive from the
			// authority instead of from here. Whatever it has already sent is
			// what this resolves to — and a null one is a world whose first
			// snapshot has not landed, which reads back as a `workspace` that
			// finds no children rather than as a crash.
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
		// runs after this — `Bindings.hpp` states that ordering, and it is why
		// the table has to exist by the time this returns.
		lua_newtable(state);
		lua_setfield(state, LUA_REGISTRYINDEX, "engine.workspace.methods");
	}
}
