#pragma once

#include <engine/imagegraphfont/GraphFontInputs.hpp>

namespace client::detail {
	// Every pointer is borrowed only while synchronous CPU input preparation is running.
	inline bool BindFontInputs(
		const engine::imagegraphfont::GraphFontInputs *owner,
		const engine::imagegraph::EvaluationRequest *borrowed,
		engine::imagegraph::SourceFontContext &held,
		engine::imagegraph::EvaluationRequest &request,
		engine::imagegraph::Diagnostic &diagnostic,
		uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
	) {
		if (owner && owner->Revision() && borrowed) {
			diagnostic = {
				engine::imagegraph::Status::InvalidValue,
				{},
				"font_inputs",
				"client font ownership conflicts with borrowed font inputs"
			};
			return false;
		}
		if (owner && owner->Revision()) {
			const auto playing = owner->Configuration().Context.Playing;
			if (!playing) {
				diagnostic = {
					engine::imagegraph::Status::UnsupportedExecution,
					{},
					"font_inputs",
					"client font playback requires explicit configuration"
				};
				return false;
			}
			return owner->Bind(*playing, held, request, maximumBytes, diagnostic);
		}
		if (borrowed) {
			request.SourceFonts = borrowed->SourceFonts;
			request.FontObservations = borrowed->FontObservations;
			request.FontProvider = borrowed->FontProvider;
			request.SourceFontHostResidentBytes = borrowed->SourceFontHostResidentBytes;
		}
		return true;
	}
}
