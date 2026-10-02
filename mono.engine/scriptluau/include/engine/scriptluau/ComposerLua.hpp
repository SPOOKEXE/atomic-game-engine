#pragma once
#include <engine/imagegraph/ComposerLuaHost.hpp>

#include <memory>
namespace engine::script {
	// Pixel Composer authors Lua code; this adapter therefore has no JavaScript twin.
	std::unique_ptr<imagegraph::ComposerLuaHost>
	MakeLuauComposerHost(const imagegraph::ComposerLuaLimits &limits = {});
}
