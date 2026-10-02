#include <engine/scripthost/ComposerLua.hpp>
#include <engine/scriptluau/ComposerLua.hpp>
namespace engine::script {
	std::unique_ptr<imagegraph::ComposerLuaHost>
	MakeComposerLuaHost(const imagegraph::ComposerLuaLimits &limits) {
		return MakeLuauComposerHost(limits);
	}
}
