#pragma once

// The JavaScript half of the two-VM surface.
//
// Private to this module, for the same reason `LuauRuntime.hpp` is: no
// `JSRuntime` or `JSContext` reaches a public header, so adding this VM changed
// no caller.

#include <engine/script/Runtime.hpp>

struct JSRuntime;
struct JSContext;

namespace engine::script {

	class JavaScriptRuntime final : public Runtime {
	  public:
		JavaScriptRuntime(ecs::Store &store, const RuntimeLimits &limits);
		~JavaScriptRuntime() override;

		bool Run(std::string_view source, std::string_view name) override;

		bool RunInstance(ecs::Entity instance) override;

		bool Heartbeat(float delta) override;

		Language Which() const override {
			return Language::JavaScript;
		}

	  private:
		// Drains promise reactions until there are none left.
		//
		// **The host drives this, and that is the entire reason a JavaScript VM
		// can live inside a deterministic tick.** A runtime that owned its own
		// event loop would resolve work at a point nobody chose.
		//
		// @return `false` when a job threw, with `Error` filled in.
		bool DrainJobs();

		JSRuntime *Vm = nullptr;
		JSContext *Context = nullptr;
	};
}
