#pragma once

#include <studio/ImageGraph.hpp>

namespace studio {
	// The host owns preview invalidation; this transaction owns the authored undo entry.
	template <class Edit>
	bool ApplyImageGraphDocumentEdit(
		engine::imagegraph::Document &document, ImageGraphHistory &history, const Edit &edit
	) {
		const engine::imagegraph::Document before = document;
		edit(document);
		if (document == before) return false;
		history.Record(before, document);
		return true;
	}
}
