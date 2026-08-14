#include "LuauBindings.hpp"

#include <engine/ecs/EnumTable.hpp>

#include <lualib.h>
#include <string_view>

namespace engine::script {

	namespace {
		using core::Name;
		using ecs::EnumTable;

		// One member of one enum: which set, and which member.
		//
		// Two interned ids and nothing else. Roblox's `EnumItem` also carries a
		// `Value` - the underlying number - and this deliberately does not:
		// **the number is not the format**, and an author who reads one will
		// eventually write it into a save file. `ecs/Enums.hpp` states that rule
		// for the engine's own enums, and handing userland a number would be the
		// one route around it.
		struct EnumItem {
			Name Enum;
			Name Member;
		};

		EnumItem &CheckEnumItem(lua_State *state, int index) {
			void *value = lua_touserdatatagged(state, index, TAG_ENUM_ITEM);
			if (value == nullptr) {
				luaL_typeerrorL(state, index, "EnumItem");
			}
			return *static_cast<EnumItem *>(value);
		}

		int EnumItemIndex(lua_State *state) {
			const EnumItem &item = CheckEnumItem(state, 1);
			const std::string_view field = luaL_checkstring(state, 2);

			if (field == "Name") {
				lua_pushstring(state, item.Member.Text().data());
				return 1;
			}
			if (field == "EnumType") {
				lua_pushstring(state, item.Enum.Text().data());
				return 1;
			}

			luaL_errorL(state, "EnumItem has no member '%s'", std::string(field).c_str());
		}

		int EnumItemToString(lua_State *state) {
			const EnumItem &item = CheckEnumItem(state, 1);
			lua_pushfstring(state, "Enum.%s.%s", item.Enum.Text().data(), item.Member.Text().data());
			return 1;
		}

		int EnumItemEqual(lua_State *state) {
			const EnumItem &left = CheckEnumItem(state, 1);
			const EnumItem &right = CheckEnumItem(state, 2);

			lua_pushboolean(state, left.Enum == right.Enum && left.Member == right.Member);
			return 1;
		}

		// `Enum.AlphaMode.Clip` - the second lookup.
		//
		// A metatable rather than a prebuilt table of members, so an enum
		// extended after the VM opened is still reachable. A game registering
		// its own materials at load time is the ordinary case, and a snapshot
		// taken at open would have missed every one of them.
		int EnumSetIndex(lua_State *state) {
			lua_getfield(state, 1, "__enum");
			const Name enumName(luaL_checkstring(state, -1));
			lua_pop(state, 1);

			const char *member = luaL_checkstring(state, 2);
			const Name value(member);

			if (!EnumTable::Has(enumName, value)) {
				luaL_errorL(state, "'%s' is not a member of Enum.%s", member, enumName.Text().data());
			}

			PushEnumItem(state, enumName, value);
			return 1;
		}

		// `Enum.AlphaMode:GetEnumItems()`
		int EnumSetGetItems(lua_State *state) {
			lua_getfield(state, 1, "__enum");
			const Name enumName(luaL_checkstring(state, -1));
			lua_pop(state, 1);

			lua_newtable(state);
			int index = 0;
			for (const Name member : EnumTable::MembersOf(enumName)) {
				PushEnumItem(state, enumName, member);
				lua_rawseti(state, -2, ++index);
			}
			return 1;
		}

		// `Enum.AlphaMode` - the first lookup.
		int EnumIndex(lua_State *state) {
			const char *name = luaL_checkstring(state, 2);
			const Name enumName(name);

			if (!EnumTable::Known(enumName)) {
				luaL_errorL(state, "'%s' is not an enum this engine registers", name);
			}

			// The set object. Built per access rather than cached, because
			// caching it would need somewhere to put the cache and the only
			// honest place is the registry - where it would then have to be
			// invalidated by a late registration. `__eq` on `EnumItem` is what
			// makes comparison work, so two set objects for one enum cost
			// nothing an author can observe.
			lua_newtable(state);
			lua_pushstring(state, name);
			lua_setfield(state, -2, "__enum");

			lua_pushcfunction(state, EnumSetGetItems, "GetEnumItems");
			lua_setfield(state, -2, "GetEnumItems");

			lua_newtable(state);
			lua_pushcfunction(state, EnumSetIndex, "__index");
			lua_setfield(state, -2, "__index");
			lua_pushstring(state, "EnumSet");
			lua_setfield(state, -2, "__metatable");
			lua_setmetatable(state, -2);

			return 1;
		}
	}

	void PushEnumItem(lua_State *state, core::Name enumName, core::Name member) {
		void *memory = lua_newuserdatatagged(state, sizeof(EnumItem), TAG_ENUM_ITEM);
		auto *item = new (memory) EnumItem();
		item->Enum = enumName;
		item->Member = member;

		luaL_getmetatable(state, "EnumItem");
		lua_setmetatable(state, -2);
	}

	bool ReadAnyEnumValue(lua_State *state, int index, core::Name &enumName, core::Name &member) {
		void *value = lua_touserdatatagged(state, index, TAG_ENUM_ITEM);
		if (value == nullptr) {
			return false;
		}
		const EnumItem &item = *static_cast<EnumItem *>(value);
		enumName = item.Enum;
		member = item.Member;
		return true;
	}

	bool ReadEnumValue(lua_State *state, int index, core::Name enumName, core::Name &out) {
		// An `EnumItem` of the *right* enum. A member of the wrong one is the
		// error a bare string could never have caught, so it is refused here
		// rather than left to `Store::SetProperty`'s registry check - which
		// would accept `Enum.Shape.Box` for an `AlphaMode` if both happened to
		// register a member of that name.
		if (void *value = lua_touserdatatagged(state, index, TAG_ENUM_ITEM); value != nullptr) {
			const auto *item = static_cast<const EnumItem *>(value);
			if (item->Enum != enumName) {
				return false;
			}
			out = item->Member;
			return true;
		}

		// A bare string, because `part.AlphaMode = "Clip"` is what Roblox
		// accepts and what a migrating script already contains. Whether the
		// member exists is `Store::SetProperty`'s check, so there is one answer
		// rather than two.
		if (const char *text = lua_tostring(state, index); text != nullptr) {
			out = core::Name(text);
			return true;
		}
		return false;
	}

	void OpenEnums(lua_State *state) {
		luaL_newmetatable(state, "EnumItem");
		lua_pushcfunction(state, EnumItemIndex, "__index");
		lua_setfield(state, -2, "__index");
		lua_pushcfunction(state, EnumItemToString, "__tostring");
		lua_setfield(state, -2, "__tostring");
		lua_pushcfunction(state, EnumItemEqual, "__eq");
		lua_setfield(state, -2, "__eq");
		lua_pushstring(state, "EnumItem");
		lua_setfield(state, -2, "__metatable");
		lua_pushstring(state, "EnumItem");
		lua_setfield(state, -2, "__type");
		lua_pop(state, 1);

		lua_newtable(state);
		lua_newtable(state);
		lua_pushcfunction(state, EnumIndex, "__index");
		lua_setfield(state, -2, "__index");
		lua_pushstring(state, "Enums");
		lua_setfield(state, -2, "__metatable");
		lua_setmetatable(state, -2);
		lua_setglobal(state, "Enum");
	}
}
