#pragma once

// The Luau half of the two-VM surface.
//
// Private to this module. Nothing outside constructs one - `MakeRuntime` picks
// an implementation and hands back a `Runtime`, which is what keeps a
// `lua_State` from ever reaching a public header.

#include <engine/script/Runtime.hpp>

struct lua_State;

namespace engine::script {

	class LuauRuntime final : public Runtime {
	  public:
		LuauRuntime(ecs::Store &store, const RuntimeLimits &limits);
		~LuauRuntime() override;

		bool Run(std::string_view source, std::string_view name) override;

		bool RunInstance(ecs::Entity instance) override;

		bool Heartbeat(float delta) override;

		Language Which() const override {
			return Language::Luau;
		}

		ScriptSurface Surface() const override;

		// The interrupt counter the step budget already maintains.
		//
		// **Nothing is added to the hot path to produce this.** `Interrupt` runs
		// at every loop back-edge, call and return because the budget needs it
		// to; this is the number it was already incrementing.
		//
		// @return Cumulative steps since this runtime was made.
		uint64_t StepsTaken() const override;

		// --- the host seam ---------------------------------------------------

		void SetHost(HostSurface *host) override;
		bool Invoke(HostCallback callback, HostArguments arguments) override;
		bool Invoke(HostCallback callback, HostArguments arguments, HostValue &result) override;
		void Release(HostCallback callback) override;

	  private:
		lua_State *State = nullptr;
	};
}
