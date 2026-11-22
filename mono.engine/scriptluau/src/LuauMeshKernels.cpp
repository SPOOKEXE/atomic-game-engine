// The `MeshKernels` global, in Luau.
//
// Half of a pair with `scriptjs/src/JsMeshKernels.cpp`. The kernels are in
// `script/MeshKernels.hpp`; this file turns `buffer` arguments into byte spans
// and the kernel's `bool` back into a Luau boolean. A wrong argument type
// raises; a stream too short for the count returns false and writes nothing.
//
// @tier L10 · shared

#include "LuauBindings.hpp"

#include <engine/script/MeshKernels.hpp>

#include <cmath>
#include <lua.h>
#include <lualib.h>

namespace engine::script {

	namespace {
		namespace mk = mesh_kernels;

		mk::Bytes Stream(lua_State *state, int index) {
			size_t length = 0;
			void *data = luaL_checkbuffer(state, index, &length);
			return {static_cast<std::byte *>(data), length};
		}

		// An absent or nil stream is `nullopt`; anything else must be a buffer.
		std::optional<mk::ConstBytes> OptionalStream(lua_State *state, int index) {
			if (lua_isnoneornil(state, index)) return std::nullopt;
			return Stream(state, index);
		}

		// A count, stride or width: a whole number in u32 range.
		uint32_t Count(lua_State *state, int index) {
			const double value = luaL_checknumber(state, index);
			if (!(value >= 0.0) || value > 4294967295.0 || std::floor(value) != value) {
				luaL_argerrorL(state, index, "expected a whole number from 0 to 2^32 - 1");
			}
			return static_cast<uint32_t>(value);
		}

		int Answer(lua_State *state, bool ok) {
			lua_pushboolean(state, ok);
			return 1;
		}

		int Axpy(lua_State *state) {
			return Answer(state, mk::Axpy(Stream(state, 1), float(luaL_checknumber(state, 2)), Stream(state, 3), Count(state, 4)));
		}

		int WeightedAxpy(lua_State *state) {
			return Answer(
				state, mk::WeightedAxpy(Stream(state, 1), Stream(state, 2), Stream(state, 3), Count(state, 4), Count(state, 5))
			);
		}

		int Transform(lua_State *state) {
			return Answer(
				state, mk::Transform(
						   Stream(state, 1), Stream(state, 2), Stream(state, 3), Stream(state, 4), Stream(state, 5),
						   Count(state, 6)
					   )
			);
		}

		int Normals(lua_State *state) {
			return Answer(
				state, mk::Normals(Stream(state, 1), Stream(state, 2), Stream(state, 3), OptionalStream(state, 4), Count(state, 5))
			);
		}

		// `adjacency` is `{offsets, neighbours}`, or the same two named.
		int Laplacian(lua_State *state) {
			const mk::Bytes out = Stream(state, 1);
			const mk::Bytes positions = Stream(state, 2);
			luaL_checktype(state, 3, LUA_TTABLE);
			lua_rawgeti(state, 3, 1);
			if (lua_isnil(state, -1)) {
				lua_pop(state, 1);
				lua_getfield(state, 3, "offsets");
			}
			lua_rawgeti(state, 3, 2);
			if (lua_isnil(state, -1)) {
				lua_pop(state, 1);
				lua_getfield(state, 3, "neighbours");
			}
			if (!lua_isbuffer(state, -2) || !lua_isbuffer(state, -1)) {
				luaL_argerrorL(state, 3, "expected {offsets: buffer, neighbours: buffer}");
			}
			const mk::Bytes offsets = Stream(state, -2);
			const mk::Bytes neighbours = Stream(state, -1);
			const std::optional<mk::ConstBytes> mask = OptionalStream(state, 4);
			const uint32_t iterations = Count(state, 5);
			const float amount = float(luaL_checknumber(state, 6));
			return Answer(state, mk::Laplacian(out, positions, offsets, neighbours, mask, iterations, amount));
		}

		int Gather(lua_State *state) {
			return Answer(state, mk::Gather(Stream(state, 1), Stream(state, 2), Stream(state, 3), Count(state, 4)));
		}

		int Scatter(lua_State *state) {
			return Answer(state, mk::Scatter(Stream(state, 1), Stream(state, 2), Stream(state, 3), Count(state, 4)));
		}
	}

	void OpenMeshKernels(lua_State *state) {
		static const luaL_Reg kernels[] = {
			{"Axpy", Axpy},
			{"WeightedAxpy", WeightedAxpy},
			{"Transform", Transform},
			{"Normals", Normals},
			{"Laplacian", Laplacian},
			{"Gather", Gather},
			{"Scatter", Scatter},
		};
		lua_createtable(state, 0, std::size(kernels));
		for (const luaL_Reg &kernel : kernels) {
			lua_pushcfunction(state, kernel.func, kernel.name);
			lua_setfield(state, -2, kernel.name);
		}
		lua_setreadonly(state, -1, 1);
		lua_setglobal(state, "MeshKernels");
	}
}
