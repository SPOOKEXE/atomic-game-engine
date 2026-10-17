#pragma once

#include "portal/PortalRendererTerminalObserver.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace engine::render::test_support {
#if ENGINE_ASSERTS_ENABLED
	// Records actual renderer terminal entrypoints for one owner on this thread.
	struct PortalRendererTerminalTrace {
		explicit PortalRendererTerminalTrace(const Renderer &renderer) : Owner(&renderer) {
			Previous = PortalRendererTerminalObserverForTests;
			PortalRendererTerminalObserverForTests = {this, &Record};
		}
		~PortalRendererTerminalTrace() {
			PortalRendererTerminalObserverForTests = Previous;
		}
		PortalRendererTerminalTrace(const PortalRendererTerminalTrace &) = delete;
		PortalRendererTerminalTrace &operator=(const PortalRendererTerminalTrace &) = delete;

		const Renderer *Owner = nullptr;
		PortalRendererTerminalObserver Previous;
		std::vector<PortalRendererTerminalCall> Calls;

		size_t Count(PortalRendererTerminalKind kind, uint64_t token) const {
			return std::count_if(Calls.begin(), Calls.end(), [&](const PortalRendererTerminalCall &call) {
				return call.Kind == kind && call.Token == token;
			});
		}
		size_t AppliedCount(PortalRendererTerminalKind kind, uint64_t token) const {
			return std::count_if(Calls.begin(), Calls.end(), [&](const PortalRendererTerminalCall &call) {
				return call.Kind == kind && call.Token == token && call.Applied;
			});
		}

	  private:
		static void Record(void *context, PortalRendererTerminalCall call) {
			auto &trace = *static_cast<PortalRendererTerminalTrace *>(context);
			if (call.Owner == trace.Owner) trace.Calls.push_back(call);
		}
	};
#endif
}
