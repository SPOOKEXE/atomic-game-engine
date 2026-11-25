#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/ui/NodeGraphPainter.hpp>

#include <functional>
#include <optional>
#include <studio/Editor.hpp>
#include <studio/RenderPipelineGraph.hpp>

namespace studio {

	void Editor::PrepareRenderPipelineCanvas() {
		if (RenderPipelineCanvas != nullptr) return;
		RenderPipelineCanvas = std::make_unique<engine::angview::View>(
			RenderPipelineGraph, RenderPipelineTypes(), engine::angview::Theme::Dark()
		);
		RenderPipelineCanvas->NodeTitle = [](const engine::ang::NodeInstance &node) {
			return RenderPipelineLabel(node);
		};
		// Headers keep the catalogue's category colours.
		RenderPipelineCanvas->NodeHeader =
			[](const engine::ang::NodeInstance &node) -> std::optional<engine::angview::Rgba> {
			using engine::graph::NodeCategory;
			const auto def = RenderPipelineTypes().FindNode(node.Def);
			if (def == nullptr) return std::nullopt;
			const auto hex = [](uint32_t rgb) {
				return engine::angview::Rgba{
					((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f, 1.0f
				};
			};
			if (def->Category == engine::graph::Describe(NodeCategory::Draw)) return hex(0x4776A8);
			if (def->Category == engine::graph::Describe(NodeCategory::Composite)) return hex(0x8059A6);
			if (def->Category == engine::graph::Describe(NodeCategory::Interface)) return hex(0xA86F47);
			if (def->Category == engine::graph::Describe(NodeCategory::Output)) return hex(0x4A9164);
			return std::nullopt;
		};
		RenderPipelineCanvas->NodeImages = [this](const engine::ang::NodeInstance &node) {
			const auto handle = RenderPipelinePreviewHandles.find(node.Id);
			if (handle == RenderPipelinePreviewHandles.end()) return engine::angview::NodeImage{};
			const auto aspect = RenderPipelinePreviewAspects.find(handle->second);
			return engine::angview::NodeImage{
				.Handle = handle->second,
				.Aspect = aspect == RenderPipelinePreviewAspects.end() ? 1.0f : aspect->second,
			};
		};
	}

	bool Editor::ApplyRenderPipelineEdit(const engine::ang::Edit &edit, bool amend) {
		engine::ang::EditResult result = RenderPipelineGraph.Apply(edit);
		if (!result.Ok) {
			RenderPipelineStatus = result.Error;
			return false;
		}
		if (amend)
			RenderPipelineHistory.Amend(std::move(result.Record));
		else
			RenderPipelineHistory.Record(std::move(result.Record));
		if (RenderPipelineCanvas != nullptr) RenderPipelineCanvas->GraphChanged();
		RenderPipelineDirty = true;
		return true;
	}

	void Editor::UndoRenderPipeline() {
		if (!RenderPipelineHistory.Undo(RenderPipelineGraph)) return;
		if (RenderPipelineCanvas != nullptr) RenderPipelineCanvas->GraphChanged();
		RenderPipelineEditing.clear();
		RenderPipelineDirty = true;
	}

	void Editor::RedoRenderPipeline() {
		if (!RenderPipelineHistory.Redo(RenderPipelineGraph)) return;
		if (RenderPipelineCanvas != nullptr) RenderPipelineCanvas->GraphChanged();
		RenderPipelineEditing.clear();
		RenderPipelineDirty = true;
	}
}
