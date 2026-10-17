#include <studio/Editor.hpp>
#include <studio/RenderPipelineGraph.hpp>

namespace studio {

	void Editor::PrepareRenderPipelineCanvas() {
		if (RenderPipelineCanvasReady) return;
		RegisterRenderPipelineNodeTypes();
		RenderPipelineCanvas.Observe(&RenderPipelinePreviewEvaluator);
		RenderPipelineCanvas.Images(
			[this](uint64_t key, const std::function<bool(nodegraph::PreviewImage &)> &) {
				const auto found = RenderPipelinePreviewTextures.find(key);
				return found == RenderPipelinePreviewTextures.end() ? nullptr : found->second;
			}
		);
		RenderPipelineCanvas.Aspects([this](uint64_t key) {
			const auto found = RenderPipelinePreviewAspects.find(key);
			return found == RenderPipelinePreviewAspects.end() ? 0.0f : found->second;
		});
		RenderPipelineCanvas.Signals.Changed = [this] { RenderPipelineDirty = true; };
		RenderPipelineCanvasReady = true;
	}
}
