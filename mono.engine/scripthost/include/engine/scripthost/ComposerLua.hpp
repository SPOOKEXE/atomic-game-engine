#pragma once
#include <engine/imagegraph/ComposerLuaHost.hpp>

#include <memory>
namespace engine::script {
	std::unique_ptr<imagegraph::ComposerLuaHost>
	MakeComposerLuaHost(const imagegraph::ComposerLuaLimits &limits = {});
}
