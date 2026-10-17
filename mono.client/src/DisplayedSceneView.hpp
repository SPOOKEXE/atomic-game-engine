#pragma once

// Binds fields owned by the selected presentation packet onto Client's normal
// game view. ActiveSceneCollector handles Studio Play views independently.

#include <engine/render/Renderer.hpp>

namespace client {
	struct ActiveScene;

	void BindDisplayedSceneFields(const ActiveScene *scene, engine::render::View &view);
}
