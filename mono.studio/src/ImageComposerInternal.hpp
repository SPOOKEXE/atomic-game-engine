#pragma once
#include <engine/core/Name.hpp>

namespace engine::render {
	class Renderer;
}

namespace studio {

	bool ImageComposerHasPendingCapture();
	void CloseImageComposerAudioPreview();
	void CloseImageComposerVector2Preview(engine::render::Renderer &renderer);

	void DrawImageComposer(engine::render::Renderer &renderer, engine::core::Name owner, bool &open);
}
