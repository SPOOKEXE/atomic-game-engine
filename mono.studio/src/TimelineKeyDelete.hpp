#pragma once

#include <engine/imagegraph/SourceKeyframeTransition.hpp>

#include <imgui.h>

namespace studio::detail {
	// one table click deletes the writer, including every captured alias.
	template <class Apply>
	bool DrawTimelineKeyDelete(
		const engine::imagegraph::Document &document,
		size_t index,
		engine::imagegraph::Diagnostic &diagnostic,
		const Apply &apply
	) {
		if (index >= document.Keyframes.size() || !ImGui::SmallButton("Delete")) return false;
		const auto &original = document.Keyframes[index];
		return apply([&](engine::imagegraph::Document &candidate) {
			const engine::imagegraph::SourceKeyframeEdit edit{&original};
			return engine::imagegraph::ApplySourceKeyframeEdits(
					   candidate, {&edit, 1}, candidate, diagnostic
				   ) == engine::imagegraph::Status::Ok;
		});
	}
}
