// The `NodeGraph` global: the embedded prelude run over `NodeGraphService`.
//
// Everything a script graph does goes through the neutral service rows; this
// file only compiles the Luau half of the object wrapper and installs it.

#include "LuauBindings.hpp"

#include <engine/core/Log.hpp>
#include <engine/script/NodeGraphService.hpp>

#include <cstdlib>
#include <luacode.h>

namespace engine::script {

	void OpenNodeGraph(lua_State *state) {
		lua_getglobal(state, "NodeGraphService");
		if (lua_isnil(state, -1)) {
			// A context without the service (a data package) gets no library.
			lua_pop(state, 1);
			return;
		}
		const std::string_view source = NodeGraphLuauPrelude();
		lua_CompileOptions options = {};
		size_t bytecodeSize = 0;
		char *bytecode = luau_compile(source.data(), source.size(), &options, &bytecodeSize);
		const int loaded = luau_load(state, "=NodeGraph", bytecode, bytecodeSize, 0);
		std::free(bytecode);
		if (loaded != 0) {
			ENGINE_ERROR("NodeGraph prelude did not load: {}", lua_tostring(state, -1));
			lua_pop(state, 2);
			return;
		}
		lua_insert(state, -2);
		if (lua_pcall(state, 1, 1, 0) != LUA_OK) {
			ENGINE_ERROR("NodeGraph prelude failed: {}", lua_tostring(state, -1));
			lua_pop(state, 1);
			return;
		}
		lua_setglobal(state, "NodeGraph");
	}
}
