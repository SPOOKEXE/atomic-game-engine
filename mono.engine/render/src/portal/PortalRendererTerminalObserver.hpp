#pragma once

#include <cstdint>

namespace engine::render {
	class Renderer;

#if ENGINE_ASSERTS_ENABLED
	namespace test_support {
		enum class PortalRendererTerminalKind {
			CancelResourceImage,
			DropPortalImage,
			ReplacePortalImage,
			ReleasePortalImport,
			DropCaptureTree,
			ReleaseCaptureTreeLease
		};
		struct PortalRendererTerminalCall {
			const Renderer *Owner = nullptr;
			PortalRendererTerminalKind Kind = PortalRendererTerminalKind::CancelResourceImage;
			uint64_t Token = 0;
			bool Applied = false;
		};
		struct PortalRendererTerminalObserver {
			void *Context = nullptr;
			void (*Record)(void *, PortalRendererTerminalCall) = nullptr;
		};
		inline thread_local PortalRendererTerminalObserver PortalRendererTerminalObserverForTests;
		inline void ObservePortalRendererTerminal(PortalRendererTerminalCall call) {
			const auto observer = PortalRendererTerminalObserverForTests;
			if (observer.Record) observer.Record(observer.Context, call);
		}
	}
#endif
}
