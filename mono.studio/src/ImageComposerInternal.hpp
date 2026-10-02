#pragma once

namespace engine::render {
	class Renderer;
}

namespace studio {

	void CloseImageComposerAudioPreview();
	void CloseImageComposerVector2Preview(engine::render::Renderer &renderer);

	void DrawImageComposer(engine::render::Renderer &renderer, bool &open);

}
