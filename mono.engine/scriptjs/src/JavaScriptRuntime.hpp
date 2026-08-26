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

		ScriptSurface Surface() const override;

		// The interrupt counter the step budget already maintains.
		//
		// **Nothing is added to the hot path to produce this**, exactly as on
		// the Luau side. One tick of it is ten thousand VM safepoints, because
		// QuickJS polls the handler on a fixed divider - so it is stable across
		// machines and coarser than a Luau step, which is why nothing compares
		// the two.
		//
		// @return Cumulative steps since this runtime was made.
		uint64_t StepsTaken() const override;

	  private:
		// Drains promise reactions until there are none left, or until
		// `RuntimeLimits::JobBudget` of them have run.
		//
		// **The host drives this, and that is the entire reason a JavaScript VM
		// can live inside a deterministic tick.** A runtime that owned its own
		// event loop would resolve work at a point nobody chose.
		//
		// **The bound is what keeps that true.** A reaction may queue a
		// reaction, so an unbounded drain is a tick that never ends.
		//
		// @return `false` when a job threw or the queue outlasted its budget,
		//         with `Error` filled in.
		bool DrainJobs();

		JSRuntime *Vm = nullptr;
		JSContext *Context = nullptr;
	};
}
