#pragma once

#include <engine/imagegraph/CacheGroupReplay.hpp>

#include <algorithm>
#include <imgui.h>
#include <nodegraph/Editor.hpp>
#include <span>
#include <unordered_map>

namespace studio::detail {
	struct ImageGraphCacheBox {
		nodegraph::NodeId Id = nodegraph::NO_NODE;
		float X = 0, Y = 0, Width = 0, Height = 0;
		bool operator==(const ImageGraphCacheBox &) const = default;
	};
	struct ImageGraphCacheBackground {
		nodegraph::NodeId Owner = nodegraph::NO_NODE;
		std::vector<ImageGraphCacheBox> Boxes;
		std::vector<ImVec2> Hull;
	};
	struct ImageGraphCacheBackgrounds {
		std::vector<ImageGraphCacheBackground> Shapes;
		void Clear() {
			Shapes.clear();
		}
		// grug source membership clicks force refresh even when a loaded overlap keeps the list unchanged.
		void Invalidate(nodegraph::NodeId owner) {
			for (auto &shape : Shapes) {
				if (shape.Owner != owner) continue;
				shape.Boxes.clear();
				shape.Hull.clear();
			}
		}
	};
	// grug one owner plus all retained members, including members hidden inside a fold.
	// empty groups have no hull. points stay in graph space until drawing.
	std::vector<ImVec2> BuildImageGraphCacheHull(std::span<const ImageGraphCacheBox> boxes);
	// grug derived view cache only. refusal clears the drawing, never authoring or replay.
	bool DrawImageGraphCacheBackgrounds(
		const nodegraph::Graph &graph,
		const nodegraph::Canvas &canvas,
		const nodegraph::ViewFrame &view,
		const engine::imagegraph::CacheGroupReplayState &groups,
		const std::unordered_map<std::string, nodegraph::NodeId> &ids,
		ImageGraphCacheBackgrounds &backgrounds,
		engine::imagegraph::Diagnostic &diagnostic,
		uint64_t maximumBytes = 64u * 1024u * 1024u
	);
}
