#pragma once

#include "portal/PortalRenderOperations.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace engine::render::test_support {
#if ENGINE_ASSERTS_ENABLED
	// Counts host terminal calls for one renderer on the test's owning thread.
	struct PortalTerminalTrace {
		explicit PortalTerminalTrace(const Renderer &renderer) : Owner(&renderer) {
			Previous = PortalTerminalObserverForTests;
			PortalTerminalObserverForTests = {this, &Record};
		}
		~PortalTerminalTrace() {
			PortalTerminalObserverForTests = Previous;
		}
		PortalTerminalTrace(const PortalTerminalTrace &) = delete;
		PortalTerminalTrace &operator=(const PortalTerminalTrace &) = delete;

		const Renderer *Owner = nullptr;
		PortalTerminalObserver Previous;
		std::vector<PortalTerminalCall> Calls;

		size_t Count(PortalTerminalKind kind, uint64_t token) const {
			return std::count_if(Calls.begin(), Calls.end(), [&](const PortalTerminalCall &call) {
				return call.Kind == kind && call.Token == token;
			});
		}
		bool Released(uint64_t token) const {
			return std::any_of(Calls.begin(), Calls.end(), [&](const PortalTerminalCall &call) {
				return call.Kind == PortalTerminalKind::ReleaseImage && call.Token == token && call.Released;
			});
		}

	  private:
		static void Record(void *context, PortalTerminalCall call) {
			auto &trace = *static_cast<PortalTerminalTrace *>(context);
			if (call.Owner == trace.Owner) trace.Calls.push_back(call);
		}
	};
#endif
}
