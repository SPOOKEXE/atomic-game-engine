#pragma once

#include <engine/imagegraph/HostCapture.hpp>

namespace engine::imagegraph {
	struct ComposerLuaLimits {
		uint64_t MaximumMemoryBytes = 64 * 1024 * 1024;
		uint64_t MaximumSteps = 100000;
		uint64_t MaximumPixelVisits = 16 * 1024 * 1024;
		size_t MaximumStates = 64;
		size_t MaximumSourceBytes = 64 * 1024;
	};
	struct ComposerLuaMessage {
		std::string NodeId;
		std::string Text;
		uint64_t Order = 0;
		bool Warning = false;
	};
	// Explicit host state owns Lua globals, retained surfaces and node scheduling.
	class ComposerLuaHost : public HostNodeProvider {
	  public:
		virtual void Reset() = 0;
		virtual std::vector<ComposerLuaMessage> TakeMessages() = 0;
	};
}
