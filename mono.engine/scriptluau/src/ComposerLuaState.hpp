#pragma once
#include <engine/imagegraph/ComposerLuaHost.hpp>

#include <algorithm>
#include <cmath>
#include <lua.h>
#include <lualib.h>
#include <memory>
#include <vector>
namespace engine::script::composer_lua {
	using namespace imagegraph;
	inline constexpr int SURFACE_TAG = 117, BUFFER_TAG = 118;
	struct Ledger {
		uint64_t VmBytes = 0, ExternalBytes = 0, MaximumBytes = 0, MessageOrder = 0;
	};
	struct Session {
		lua_State *State = nullptr;
		Ledger *Memory = nullptr;
		std::string Id, ActiveNode;
		std::vector<ComposerLuaMessage> Messages;
		uint64_t MessageBytes = 0;
		uint64_t Steps = 0, Base = 0, StepBudget = 0, PixelVisits = 0, PixelLimit = 0, RandomState = 1;
		uint32_t Colour = 0xffffff;
		double Alpha = 1;
		int Blend = 0;
		Image *Target = nullptr;
		std::vector<SurfaceValue> Surfaces;
		std::vector<BufferValue> Buffers;
		~Session() {
			if (State) lua_close(State);
		}
	};
	inline Session &Of(lua_State *state) {
		return *static_cast<Session *>(lua_getthreaddata(state));
	}
	inline double Number(lua_State *state, int index, double fallback = 0) {
		const double value = lua_isnoneornil(state, index) ? fallback : luaL_checknumber(state, index);
		if (!std::isfinite(value) || std::abs(value) > double(uint64_t{1} << 40))
			luaL_errorL(state, "drawing arguments must be finite");
		return value;
	}
	inline uint32_t Packed(double value) {
		return uint32_t(int64_t(std::clamp(value, 0., double(0xffffff))));
	}
	inline bool ChargePixel(Session &session) {
		return ++session.PixelVisits <= session.PixelLimit;
	}
	inline Image *Surface(lua_State *state, int index) {
		if (lua_isnoneornil(state, index)) return nullptr;
		const auto *reference =
			static_cast<const uint32_t *>(lua_touserdatatagged(state, index, SURFACE_TAG));
		auto &session = Of(state);
		if (!reference || *reference >= session.Surfaces.size())
			luaL_errorL(state, "expected an explicit surface argument");
		return &session.Surfaces[*reference].Data;
	}
	void InstallDrawing(lua_State *state);
}
